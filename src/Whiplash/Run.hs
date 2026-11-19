-- | The entry point: loads ~/.config/whiplash/config.toml over the
-- compiled-in defaults, wires the brain to the heart's events, and reloads
-- the file whenever it is saved. A broken file never replaces a working
-- config: the error goes to the log and to a desktop notification.
module Whiplash.Run
  ( whiplash
  , configPath
  ) where

import Control.Exception (SomeException, evaluate, handle, try)
import Control.Monad (forM_, unless, when)
import Data.Char (toLower)
import Data.IORef
import qualified Data.Map.Strict as M
import qualified Data.Set as S
import System.Directory (createDirectoryIfMissing, doesFileExist, findExecutable)
import System.Environment (getArgs, lookupEnv)
import System.Exit (exitFailure)
import System.IO (hPutStrLn, stderr)

import Whiplash.Core
import qualified Whiplash.Heart as H
import Whiplash.Layout
import Whiplash.Settings
import qualified Whiplash.StackSet as W

-- | $XDG_CONFIG_HOME/whiplash (or ~/.config/whiplash) and the file name.
configPath :: IO (FilePath, String)
configPath = do
  xdg <- lookupEnv "XDG_CONFIG_HOME"
  home <- maybe "/" id <$> lookupEnv "HOME"
  let base = case xdg of
        Just d | not (null d) -> d
        _ -> home ++ "/.config"
  pure (base ++ "/whiplash", "config.toml")

-- | Load the file over the defaults. A missing file means the defaults.
readConfig :: Config -> FilePath -> IO (Either String Config)
readConfig base path = do
  exists <- doesFileExist path
  if not exists
    then pure (Right base)
    else do
      r <- try (readFile path >>= \s -> evaluate (length s) >> pure s)
      case r of
        Left e -> pure (Left ("cannot read " ++ path ++ ": " ++ show (e :: SomeException)))
        Right src -> either (Left . ((path ++ ": ") ++)) Right <$> loadSettings base src

complain :: String -> IO ()
complain msg = do
  hPutStrLn stderr ("whiplash: " ++ msg)
  notify <- findExecutable "notify-send"
  forM_ notify $ \_ ->
    H.spawnCommand ("notify-send -a whiplash 'whiplash: config error' " ++ shellQuote msg)
  where
    shellQuote s = "'" ++ concatMap (\c -> if c == '\'' then "'\\''" else [c]) s ++ "'"

-- | Push the parts of a config the heart owns.
applyToHeart :: Config -> IO ()
applyToHeart cfg = do
  H.setBorder (borderWidth cfg) (focusedBorderColor cfg) (normalBorderColor cfg)
  H.setMouseMod (modMask cfg)
  H.setOutputRules (outputRules cfg)
  let kb = keyboard cfg
  H.setKeyboard (kbLayout kb) (kbVariant kb) (kbOptions kb) (kbRepeatRate kb) (kbRepeatDelay kb)
  let tp = touchpad cfg
  H.setTouchpad (tpTap tp) (tpTapDrag tp) (tpDragLock tp) (tpNaturalScroll tp)
    (tpDisableWhileTyping tp) (tpAccelSpeed tp)
    (fromEnum (tpAccelProfile tp)) (fromEnum (tpClickMethod tp))

keyMapOf :: Config -> KeyMap
keyMapOf cfg = M.fromList (keys cfg cfg)

-- | Exceptions must never cross back into C: log them and carry on.
guarded :: String -> IO a -> a -> IO a
guarded what act fallback = handle report act
  where
    report e = do
      hPutStrLn stderr ("whiplash: " ++ what ++ ": " ++ show (e :: SomeException))
      pure fallback

whiplash :: Config -> IO ()
whiplash defaults = do
  args <- getArgs
  -- -s CMD runs CMD after startup; -n skips the autostart (handy for
  -- nested or headless test runs that must not start session services).
  let startupCmd = case dropWhile (/= "-s") args of
        (_ : cmd : _) -> Just cmd
        _ -> Nothing
      skipHook = "-n" `elem` args
  -- Started inside another graphical session (a nested test run)?
  nested <- or <$> mapM (fmap (maybe False (not . null)) . lookupEnv) ["WAYLAND_DISPLAY", "DISPLAY"]
  (dir, name) <- configPath
  let path = dir ++ "/" ++ name

  cfg0 <- readConfig defaults path >>= either (\e -> complain e >> pure defaults) pure
  cfgRef <- newIORef cfg0
  keyRef <- newIORef (keyMapOf cfg0)
  stRef <- newIORef WState
    { windowSet = W.new 0 (workspaceTags cfg0)
    , screens = []
    , ratio = 0.55
    , floating = M.empty
    , fullscreen = S.empty
    }
  let reloadNow = do
        old <- getConfig
        r <- io (readConfig defaults path)
        case r of
          Left err -> io (complain err)
          Right new -> do
            -- Workspaces are announced to bars once; keep the running set.
            let cfg = new {workspaceTags = workspaceTags old}
            io $ do
              writeIORef cfgRef cfg
              writeIORef keyRef (keyMapOf cfg)
              applyToHeart cfg
              H.applyOutputRules
              hPutStrLn stderr ("whiplash: reloaded " ++ path)
            refresh
      env = XEnv cfgRef keyRef stRef reloadNow
      run what act = guarded what (runX env act) ()

  H.onViewMapped $ \v -> run "map" $ do
    app <- io (H.appId v)
    rules <- windowRules <$> getConfig
    let rule = lookup (map toLower app) rules
    -- In the log, so the name for a [rules] entry is easy to find.
    io $ hPutStrLn stderr ("whiplash: window " ++ show v ++ " app_id " ++ show app
      ++ maybe "" (const " (rule)") rule)
    dialog <- io (H.wantsFloat v)
    when (maybe dialog id (rule >>= ruleFloat)) (floatCentered v)
    windows (maybe W.insertUp W.insertOn (rule >>= ruleWorkspace) v)
  H.onViewUnmapped $ \v -> run "unmap" (forget v >> windows (W.delete v))
  H.onViewClicked $ \v -> run "click" (windows (W.focusWindow v))
  H.onViewEntered $ \v -> run "enter" $ do
    on <- focusFollowsMouse <$> getConfig
    when on (windows (W.focusWindow v))
  H.onViewFullscreen $ \v on -> run "fullscreen" (setFullscreen v on)
  -- Mod+drag on a tiled window floats it where it stands, like xmonad.
  H.onViewGrab $ \v (x, y, w, h) -> run "grab" $ do
    st <- getState
    unless (M.member v (floating st) || S.member v (fullscreen st)) $
      modifyState (\s -> s {floating = M.insert v (Rect x y w h) (floating s)})
    windows (W.focusWindow v)
  H.onViewMoved $ \v (x, y, w, h) -> run "moved" $
    modifyState (\s -> s {floating = M.adjust (const (Rect x y w h)) v (floating s)})
  H.onWorkspaceActivate $ \t -> run "workspace" (windows (W.view t))
  H.onOutputsChanged $ run "outputs" $ do
    outs <- io H.outputs
    let rect (x, y, w, h) = Rect x y w h
        scr = [(rect (H.outputFull o), rect (H.outputUsable o)) | o <- outs]
    modifyState $ \s ->
      s {screens = scr, windowSet = W.setScreens (length scr) (windowSet s)}
    refresh
  H.onKey $ \mods sym -> guarded "key" (lookupKey env keyRef mods sym) False
  H.onConfigChanged (run "reload" reloadNow)

  applyToHeart cfg0
  ok <- H.heartInit
  unless ok $ hPutStrLn stderr "whiplash: the heart failed to start" >> exitFailure
  createDirectoryIfMissing True dir
  watching <- H.watchConfig dir name
  unless watching $ hPutStrLn stderr ("whiplash: cannot watch " ++ dir ++ "; no hot reload")
  -- D-Bus-activated services (portals: screen sharing, file pickers) only
  -- see the environment the bus was started with, from before whiplash
  -- set WAYLAND_DISPLAY/DISPLAY. Never from a nested run: it would point
  -- the host session's portals at the nested display.
  unless nested $
    H.spawnCommand "dbus-update-activation-environment WAYLAND_DISPLAY DISPLAY XDG_CURRENT_DESKTOP"
  forM_ (workspaceTags cfg0) $ \t -> H.addWorkspace t (show t)
  run "workspaces" refresh
  unless skipHook $ run "startup" (startupHook cfg0)
  forM_ startupCmd H.spawnCommand
  H.heartRun
  H.heartShutdown
  where
    lookupKey env keyRef mods sym = do
      km <- readIORef keyRef
      case M.lookup (mods, sym) km of
        Just act -> runX env act >> pure True
        Nothing -> pure False
