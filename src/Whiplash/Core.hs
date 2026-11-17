{-# LANGUAGE GeneralizedNewtypeDeriving #-}
-- | The brain: configuration, the X monad that actions run in, and the
-- event handlers the heart calls into.
module Whiplash.Core
  ( Config (..)
  , X
  , WState (..)
  , whiplash
  , windows
  , spawn
  , kill
  , quit
  , nextLayout
  , jumpToLayout
  , incRatio
  , toggleFloat
  , toggleFullscreen
  , withFocused
  , io
  ) where

import Control.Exception (SomeException, handle)
import Control.Monad (forM_, unless)
import Control.Monad.Reader (MonadIO, MonadReader, ReaderT, asks, liftIO, runReaderT)
import Data.IORef
import Data.List ((\\))
import qualified Data.Map.Strict as M
import qualified Data.Set as S
import Data.Word (Word32)
import System.Environment (getArgs)
import System.Exit (exitFailure)
import System.IO (hPutStrLn, stderr)

import qualified Whiplash.Heart as H
import Whiplash.Keys
import Whiplash.Layout
import qualified Whiplash.StackSet as W

data Config = Config
  { terminal :: String
  , modMask :: KeyMask
  , borderWidth :: Int
  , focusedBorderColor :: Word32 -- ^ 0xRRGGBBAA
  , normalBorderColor :: Word32
  , gaps :: Int
  , layouts :: [Layout]
  , workspaceTags :: [Int]
  , keyboardLayout :: Maybe String -- ^ xkb layout, e.g. Just "pt"
  , keys :: Config -> [((KeyMask, KeySym), X ())]
  , startupHook :: X ()
  }

data WState = WState
  { windowSet :: W.StackSet Int H.ViewId -- ^ layout index per workspace
  , screen :: Rect -- ^ usable area: the output minus bars
  , fullRect :: Rect -- ^ the whole output, for fullscreen windows
  , ratio :: Double
  , floating :: M.Map H.ViewId Rect -- ^ floating windows and where they sit
  , fullscreen :: S.Set H.ViewId
  }

data XEnv = XEnv {xConfig :: Config, xState :: IORef WState}

newtype X a = X (ReaderT XEnv IO a)
  deriving (Functor, Applicative, Monad, MonadIO, MonadReader XEnv)

runX :: XEnv -> X a -> IO a
runX env (X m) = runReaderT m env

io :: IO a -> X a
io = liftIO

getState :: X WState
getState = asks xState >>= io . readIORef

modifyState :: (WState -> WState) -> X ()
modifyState f = asks xState >>= io . flip modifyIORef' f

-- | Change the window set, then push the result to the screen.
windows :: (W.StackSet Int H.ViewId -> W.StackSet Int H.ViewId) -> X ()
windows f = modifyState (\s -> s {windowSet = f (windowSet s)}) >> refresh

refresh :: X ()
refresh = do
  cfg <- asks xConfig
  st <- getState
  let ws = windowSet st
      cur = W.currentWorkspace ws
      wins = maybe [] W.integrate (W.stack cur)
      isFull v = S.member v (fullscreen st)
      floatRect v = M.lookup v (floating st)
      tiled = [v | v <- wins, not (isFull v), floatRect v == Nothing]
      lay = layouts cfg !! (W.layoutIx cur `mod` length (layouts cfg))
      g = gaps cfg
      area = shrinkRect g (screen st)
      placed =
        [(v, H.Tiled, shrinkRect g r) | (v, r) <- arrange lay (ratio st) area tiled]
          ++ [(v, H.Floating, r) | v <- wins, not (isFull v), Just r <- [floatRect v]]
          ++ [(v, H.Fullscreen, fullRect st) | v <- wins, isFull v]
  io $ do
    -- The mode goes first: it decides the border width the geometry uses.
    forM_ placed $ \(v, mode, Rect x y w h) -> do
      H.setMode v mode
      H.setGeometry v x y w h
      H.setVisible v True
    forM_ (W.allWindows ws \\ wins) $ \v -> H.setVisible v False
    H.focusView (W.peek ws)

withFocused :: (H.ViewId -> X ()) -> X ()
withFocused f = getState >>= maybe (pure ()) f . W.peek . windowSet

-- | Float a window at the size it picked, centred on the usable area.
floatCentered :: H.ViewId -> X ()
floatCentered v = do
  bw <- asks (borderWidth . xConfig)
  st <- getState
  (nw, nh) <- io (H.naturalSize v)
  let Rect sx sy sw sh = screen st
      fit n total fallback = if n <= 0 then fallback else min total (n + 2 * bw)
      w = fit nw sw (sw * 3 `div` 5)
      h = fit nh sh (sh * 3 `div` 5)
      r = Rect (sx + (sw - w) `div` 2) (sy + (sh - h) `div` 2) w h
  modifyState (\s -> s {floating = M.insert v r (floating s)})

-- | Float the focused window, or sink it back into the tiling.
toggleFloat :: X ()
toggleFloat = withFocused $ \v -> do
  st <- getState
  if M.member v (floating st)
    then modifyState (\s -> s {floating = M.delete v (floating s)})
    else floatCentered v
  refresh

setFullscreen :: H.ViewId -> Bool -> X ()
setFullscreen v on = do
  modifyState (\s -> s {fullscreen = (if on then S.insert else S.delete) v (fullscreen s)})
  refresh

toggleFullscreen :: X ()
toggleFullscreen = withFocused $ \v -> getState >>= setFullscreen v . not . S.member v . fullscreen

-- | Forget everything about a window that went away.
forget :: H.ViewId -> X ()
forget v = modifyState $ \s ->
  s {floating = M.delete v (floating s), fullscreen = S.delete v (fullscreen s)}

spawn :: String -> X ()
spawn = io . H.spawnCommand

-- | Politely ask the focused window to close.
kill :: X ()
kill = getState >>= maybe (pure ()) (io . H.closeView) . W.peek . windowSet

quit :: X ()
quit = io H.heartQuit

nextLayout :: X ()
nextLayout = windows (W.modifyLayout (+ 1))

-- | Switch the current workspace to the layout with this name.
jumpToLayout :: String -> X ()
jumpToLayout name = do
  ls <- asks (layouts . xConfig)
  case lookup name (zip (map layoutName ls) [0 ..]) of
    Just i -> windows (W.modifyLayout (const i))
    Nothing -> pure ()

incRatio :: Double -> X ()
incRatio d = modifyState (\s -> s {ratio = clamp (ratio s + d)}) >> refresh
  where
    clamp = max 0.1 . min 0.9

-- | Exceptions must never cross back into C: log them and carry on.
guarded :: String -> IO a -> a -> IO a
guarded what act fallback = handle report act
  where
    report e = do
      hPutStrLn stderr ("whiplash: " ++ what ++ ": " ++ show (e :: SomeException))
      pure fallback

whiplash :: Config -> IO ()
whiplash cfg = do
  args <- getArgs
  -- -s CMD runs CMD after startup; -n skips the startupHook (handy for
  -- nested or headless test runs that must not start session services).
  let startupCmd = case dropWhile (/= "-s") args of
        (_ : cmd : _) -> Just cmd
        _ -> Nothing
      skipHook = "-n" `elem` args
  ref <- newIORef WState
    { windowSet = W.new 0 (workspaceTags cfg)
    , screen = Rect 0 0 1280 720
    , fullRect = Rect 0 0 1280 720
    , ratio = 0.55
    , floating = M.empty
    , fullscreen = S.empty
    }
  let env = XEnv cfg ref
      run what act = guarded what (runX env act) ()
      keymap = M.fromList (keys cfg cfg)

  H.onViewMapped $ \v -> run "map" $ do
    dialog <- io (H.wantsFloat v)
    if dialog then floatCentered v else pure ()
    windows (W.insertUp v)
  H.onViewUnmapped $ \v -> run "unmap" (forget v >> windows (W.delete v))
  H.onViewClicked $ \v -> run "click" (windows (W.focusWindow v))
  H.onViewFullscreen $ \v on -> run "fullscreen" (setFullscreen v on)
  -- Mod+drag on a tiled window floats it where it stands, like xmonad.
  H.onViewGrab $ \v (x, y, w, h) -> run "grab" $ do
    st <- getState
    unless (M.member v (floating st) || S.member v (fullscreen st)) $
      modifyState (\s -> s {floating = M.insert v (Rect x y w h) (floating s)})
    windows (W.focusWindow v)
  H.onViewMoved $ \v (x, y, w, h) -> run "moved" $
    modifyState (\s -> s {floating = M.adjust (const (Rect x y w h)) v (floating s)})
  H.onOutput $ \(x, y, w, h) (ux, uy, uw, uh) -> run "output" $ do
    modifyState (\s -> s {fullRect = Rect x y w h, screen = Rect ux uy uw uh})
    refresh
  H.onKey $ \mods sym -> guarded "key" (lookupKey env keymap mods sym) False

  H.setBorder (borderWidth cfg) (focusedBorderColor cfg) (normalBorderColor cfg)
  H.setMouseMod (modMask cfg)
  ok <- H.heartInit (keyboardLayout cfg)
  unless ok $ hPutStrLn stderr "whiplash: the heart failed to start" >> exitFailure
  unless skipHook $ run "startup" (startupHook cfg)
  forM_ startupCmd H.spawnCommand
  H.heartRun
  H.heartShutdown
  where
    lookupKey env keymap mods sym = case M.lookup (mods, sym) keymap of
      Just act -> runX env act >> pure True
      Nothing -> pure False
