-- | The entry point: loads ~/.config/whiplash/config.toml over the
-- compiled-in defaults, wires the brain to the heart's events, and reloads
-- the file whenever it is saved. A broken file never replaces a working
-- config: the error goes to the log and to a desktop notification.
module Whiplash.Run
  ( whiplash
  , whiplashWith
  , configPath
  ) where

import Control.Exception (IOException, SomeException, catch, evaluate, handle, try)
import Control.Monad (forM_, unless, when)
import Data.Char (toLower)
import Data.IORef
import qualified Data.Map.Strict as M
import qualified Data.Set as S
import System.Directory (createDirectoryIfMissing, doesFileExist, findExecutable, removeFile)
import System.Environment (getArgs, lookupEnv, setEnv, unsetEnv)
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
readConfig :: H.Backend -> Config -> FilePath -> IO (Either String Config)
readConfig backend base path = do
  exists <- doesFileExist path
  if not exists
    then pure (Right base)
    else do
      r <- try (readFile path >>= \s -> evaluate (length s) >> pure s)
      case r of
        Left e -> pure (Left ("cannot read " ++ path ++ ": " ++ show (e :: SomeException)))
        Right src -> either (Left . ((path ++ ": ") ++)) Right <$> loadSettingsFor backend base src

complain :: String -> IO ()
complain msg = do
  hPutStrLn stderr ("whiplash: " ++ msg)
  notify <- findExecutable "notify-send"
  forM_ notify $ \_ ->
    H.spawnCommand ("notify-send -a whiplash 'whiplash: config error' " ++ shellQuote msg)
  where
    shellQuote s = "'" ++ concatMap (\c -> if c == '\'' then "'\\''" else [c]) s ++ "'"

-- | Push the parts of a config the heart owns.
applyToHeart :: H.Backend -> Config -> IO ()
applyToHeart backend cfg = do
  H.setBorder (borderWidth cfg) (focusedBorderColor cfg) (normalBorderColor cfg)
  H.setMouseMod (modMask cfg)
  when (backend == H.X11) (H.setFocusFollowsMouse (focusFollowsMouse cfg))
  when (backend == H.Wayland) $ do
    H.setOutputRules (outputRules cfg)
    let kb = keyboard cfg
    H.setKeyboard (kbLayout kb) (kbVariant kb) (kbOptions kb) (kbRepeatRate kb) (kbRepeatDelay kb)
    let tp = touchpad cfg
    H.setTouchpad (tpTap tp) (tpTapDrag tp) (tpDragLock tp) (tpNaturalScroll tp)
      (tpDisableWhileTyping tp) (tpAccelSpeed tp)
      (fromEnum (tpAccelProfile tp)) (fromEnum (tpClickMethod tp))

-- | A new window that is not a scratchpad: window rules, the dialog
-- guess, then into the stack.
placeNew :: String -> H.ViewId -> X ()
placeNew app v = do
  rules <- windowRules <$> getConfig
  let rule = lookup (map toLower app) rules
  -- In the log, so the name for a [rules] entry is easy to find.
  io $ hPutStrLn stderr ("whiplash: window " ++ show v ++ " app_id " ++ show app
    ++ maybe "" (const " (rule)") rule)
  dialog <- io (H.wantsFloat v)
  when (maybe dialog id (rule >>= ruleFloat)) (floatCentered v)
  windows (maybe W.insertUp W.insertOn (rule >>= ruleWorkspace) v)

keyMapOf :: Config -> KeyMap
keyMapOf cfg = M.fromList (keys cfg cfg)

-- | Exceptions must never cross back into C: log them and carry on.
guarded :: String -> IO a -> a -> IO a
guarded what act fallback = handle report act
  where
    report e = do
      hPutStrLn stderr ("whiplash: " ++ what ++ ": " ++ show (e :: SomeException))
      pure fallback

data Options = Options H.Backend Bool (Maybe String)

parseArgs :: [String] -> Either String Options
parseArgs = go (Options H.Wayland False Nothing)
  where
    go opts [] = Right opts
    go (Options _ skip cmd) ("--backend" : name : rest) = case name of
      "wayland" -> go (Options H.Wayland skip cmd) rest
      "x11" -> go (Options H.X11 skip cmd) rest
      _ -> Left ("unknown backend " ++ show name)
    go (Options backend _ cmd) ("-n" : rest) = go (Options backend True cmd) rest
    go (Options backend skip _) ("-s" : command : rest) =
      go (Options backend skip (Just command)) rest
    go _ (arg : _) = Left ("unexpected argument " ++ show arg)

whiplash :: Config -> IO ()
whiplash defaults = whiplashWith $ \backend ->
  if backend == H.Wayland then defaults else defaults {startupHook = pure ()}

-- | Select compiled defaults for the chosen backend before reading TOML.
whiplashWith :: (H.Backend -> Config) -> IO ()
whiplashWith defaults = do
  args <- getArgs
  case parseArgs args of
    Left err -> do
      hPutStrLn stderr ("whiplash: " ++ err)
      hPutStrLn stderr "Usage: whiplash [--backend wayland|x11] [-n] [-s CMD]"
      exitFailure
    Right opts@(Options backend _ _) -> runSession opts (defaults backend)

runSession :: Options -> Config -> IO ()
runSession (Options backend skipHook startupCmd) defaults = do
  -- Started inside another graphical session (a nested test run)?
  nested <- or <$> mapM (fmap (maybe False (not . null)) . lookupEnv) ["WAYLAND_DISPLAY", "DISPLAY"]
  H.setBackend backend
  when (backend == H.X11) $ do
    unsetEnv "WAYLAND_DISPLAY"
    unsetEnv "WAYLAND_SOCKET"
    setEnv "XDG_SESSION_TYPE" "x11"
  (dir, name) <- configPath
  let path = dir ++ "/" ++ name
      installBindings bindings = when (backend == H.X11) (H.setBindings (M.keys bindings))

  cfg0 <- readConfig backend defaults path >>= either (\e -> complain e >> pure defaults) pure
  cfgRef <- newIORef cfg0
  keyRef <- newIORef (keyMapOf cfg0)
  stRef <- newIORef WState
    { windowSet = W.new 0 (workspaceTags cfg0)
    , screens = []
    , ratio = 0.55
    , floating = M.empty
    , fullscreen = S.empty
    , scratch = M.empty
    , urgent = S.empty
    , published = ""
    }
  let reloadNow = do
        old <- getConfig
        r <- io (readConfig backend defaults path)
        case r of
          Left err -> io (complain err)
          Right new -> do
            -- Workspaces are announced to bars once; keep the running set.
            let cfg = new {workspaceTags = workspaceTags old}
                bindings = keyMapOf cfg
            io $ do
              writeIORef cfgRef cfg
              writeIORef keyRef bindings
              installBindings bindings
              applyToHeart backend cfg
              when (backend == H.Wayland) H.applyOutputRules
              hPutStrLn stderr ("whiplash: reloaded " ++ path)
            refresh
      env = XEnv cfgRef keyRef stRef reloadNow
      run what act = guarded what (runX env act) ()

  H.onViewMapped $ \v -> run "map" $ do
    app <- io (H.appId v)
    caught <- captureScratchpad app v
    if caught
      then io (hPutStrLn stderr ("whiplash: window " ++ show v ++ " app_id " ++ show app ++ " (scratchpad)"))
      else placeNew app v
  H.onViewUnmapped $ \v -> run "unmap" (forget v >> windows (W.delete v))
  H.onViewClicked $ \v -> run "click" (windows (W.focusWindow v))
  H.onViewEntered $ \v -> run "enter" $ do
    on <- focusFollowsMouse <$> getConfig
    when on (windows (W.focusWindow v))
  H.onViewFullscreen $ \v on -> run "fullscreen" (setFullscreen v on)
  H.onViewUrgent $ \v on -> run "urgent" (setUrgent v on)
  -- Mod+drag on a tiled window floats it where it stands, like xmonad.
  H.onViewGrab $ \v (x, y, w, h) -> run "grab" $ do
    st <- getState
    unless (M.member v (floating st) || S.member v (fullscreen st)) $
      modifyState (\s -> s {floating = M.insert v (Rect x y w h) (floating s)})
    windows (W.focusWindow v)
  H.onViewMoved $ \v (x, y, w, h) -> run "moved" (floatMoved v (Rect x y w h))
  H.onWorkspaceActivate $ \t -> run "workspace" (windows (W.view t))
  H.onOutputsChanged $ run "outputs" $ do
    outs <- io H.outputs
    let rect (x, y, w, h) = Rect x y w h
        scr = [(rect (H.outputFull o), rect (H.outputUsable o)) | o <- outs]
    modifyState $ \s ->
      s {screens = scr, windowSet = W.setScreens (length scr) (windowSet s)}
    refresh
  H.onKey $ \mods sym -> guarded "key" (lookupKey env keyRef mods sym) False
  -- A swipe is taken from clients only when its finger count is bound.
  H.onSwipe $ \n d -> guarded "swipe" (do
    gs <- gestures <$> readIORef cfgRef
    if d < 0
      then pure (any ((== n) . fst . fst) gs)
      else do
        forM_ (lookup (n, d) gs) (runX env)
        pure True) False
  H.onConfigChanged (run "reload" reloadNow)

  applyToHeart backend cfg0
  ok <- H.heartInit
  unless ok $ hPutStrLn stderr "whiplash: the heart failed to start" >> exitFailure
  readIORef keyRef >>= installBindings
  createDirectoryIfMissing True dir
  watching <- H.watchConfig dir name
  unless watching $ hPutStrLn stderr ("whiplash: cannot watch " ++ dir ++ "; no hot reload")
  -- D-Bus-activated services (portals: screen sharing, file pickers) only
  -- see the environment the bus was started with, from before whiplash
  -- set WAYLAND_DISPLAY/DISPLAY. Never from a nested run: it would point
  -- the host session's portals at the nested display.
  when (backend == H.Wayland && not nested) $
    H.spawnCommand "dbus-update-activation-environment WAYLAND_DISPLAY DISPLAY XDG_CURRENT_DESKTOP"
  forM_ (workspaceTags cfg0) $ \t -> H.addWorkspace t (show t)
  run "workspaces" refresh
  unless skipHook $ run "startup" (startupHook cfg0)
  forM_ startupCmd H.spawnCommand
  H.heartRun
  H.heartShutdown
  -- Bars must not read a stale file after the session ends.
  workspacesFile >>= mapM_ (\p -> removeFile p `catch` \e -> const (pure ()) (e :: IOException))
  where
    lookupKey env keyRef mods sym = do
      km <- readIORef keyRef
      case M.lookup (mods, sym) km of
        Just act -> runX env act >> pure True
        Nothing -> pure False
