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
  , io
  ) where

import Control.Exception (SomeException, handle)
import Control.Monad (forM_, unless)
import Control.Monad.Reader (MonadIO, MonadReader, ReaderT, asks, liftIO, runReaderT)
import Data.IORef
import Data.List ((\\))
import qualified Data.Map.Strict as M
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
  , screen :: Rect
  , ratio :: Double
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
      tiled = maybe [] W.integrate (W.stack cur)
      lay = layouts cfg !! (W.layoutIx cur `mod` length (layouts cfg))
      g = gaps cfg
      area = shrinkRect g (screen st)
      placed = arrange lay (ratio st) area tiled
  io $ do
    forM_ placed $ \(v, r) -> do
      let Rect x y w h = shrinkRect g r
      H.setGeometry v x y w h
      H.setVisible v True
    forM_ (W.allWindows ws \\ tiled) $ \v -> H.setVisible v False
    H.focusView (W.peek ws)

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
    , ratio = 0.55
    }
  let env = XEnv cfg ref
      run what act = guarded what (runX env act) ()
      keymap = M.fromList (keys cfg cfg)

  H.onViewMapped $ \v -> run "map" (windows (W.insertUp v))
  H.onViewUnmapped $ \v -> run "unmap" (windows (W.delete v))
  H.onViewClicked $ \v -> run "click" (windows (W.focusWindow v))
  H.onOutput $ \x y w h -> run "output" $ do
    modifyState (\s -> s {screen = Rect x y w h})
    refresh
  H.onKey $ \mods sym -> guarded "key" (lookupKey env keymap mods sym) False

  H.setBorder (borderWidth cfg) (focusedBorderColor cfg) (normalBorderColor cfg)
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
