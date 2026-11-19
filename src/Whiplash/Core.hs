{-# LANGUAGE GeneralizedNewtypeDeriving #-}
-- | The brain: configuration, the X monad that actions run in, window
-- placement, and the actions bindings call. The entry point that wires
-- this to the heart lives in "Whiplash.Run".
module Whiplash.Core
  ( Config (..)
  , KeyboardSettings (..)
  , TouchpadSettings (..)
  , AccelProfile (..)
  , ClickMethod (..)
  , defaultTouchpad
  , KeyMap
  , X
  , XEnv (..)
  , WState (..)
  , runX
  , getConfig
  , getState
  , modifyState
  , refresh
  , windows
  , spawn
  , kill
  , quit
  , reload
  , nextLayout
  , jumpToLayout
  , incRatio
  , toggleFloat
  , toggleFullscreen
  , setFullscreen
  , floatCentered
  , focusScreenBy
  , moveToScreenBy
  , OutputRule
  , WindowRule (..)
  , forget
  , withFocused
  , io
  ) where

import Control.Monad (forM_, join)
import Control.Monad.Reader (MonadIO, MonadReader, ReaderT, asks, liftIO, runReaderT)
import Data.Char (toLower)
import Data.IORef
import Data.List ((\\))
import qualified Data.Map.Strict as M
import qualified Data.Set as S
import Data.Word (Word32)

import qualified Whiplash.Heart as H
import Whiplash.Keys
import Whiplash.Layout
import qualified Whiplash.StackSet as W

data KeyboardSettings = KeyboardSettings
  { kbLayout :: String -- ^ xkb layout, e.g. "pt" or "pt,us"; "" for the default
  , kbVariant :: String
  , kbOptions :: String -- ^ e.g. "caps:escape,grp:alt_shift_toggle"
  , kbRepeatRate :: Int -- ^ repeats per second
  , kbRepeatDelay :: Int -- ^ ms held before the first repeat
  }

-- | Settings for touchpads (devices that can tap); mice are left alone.
data TouchpadSettings = TouchpadSettings
  { tpTap :: Bool -- ^ tap to click: 1/2/3 fingers = left/right/middle
  , tpTapDrag :: Bool -- ^ tap, then touch again and move to drag
  , tpDragLock :: Bool -- ^ a drag survives lifting the finger briefly
  , tpNaturalScroll :: Bool
  , tpDisableWhileTyping :: Bool
  , tpAccelSpeed :: Double -- ^ -1 (slow) .. 1 (fast)
  , tpAccelProfile :: AccelProfile
  , tpClickMethod :: ClickMethod
  }

data AccelProfile = AccelDefault | AccelAdaptive | AccelFlat
  deriving (Eq, Show, Enum)

-- | How a physical press picks the button: by where it lands on the pad
-- (button areas) or by how many fingers touch (clickfinger).
data ClickMethod = ClickDefault | ClickButtonAreas | ClickFinger
  deriving (Eq, Show, Enum)

-- | The wasp defaults.
defaultTouchpad :: TouchpadSettings
defaultTouchpad = TouchpadSettings True True True False True 0 AccelDefault ClickDefault

data Config = Config
  { terminal :: String
  , modMask :: KeyMask
  , borderWidth :: Int
  , focusedBorderColor :: Word32 -- ^ 0xRRGGBBAA
  , normalBorderColor :: Word32
  , gaps :: Int
  , layouts :: [Layout]
  , workspaceTags :: [Int]
  , keyboard :: KeyboardSettings
  , touchpad :: TouchpadSettings
  , keys :: Config -> [((KeyMask, KeySym), X ())]
  , startupHook :: X ()
  , outputRules :: [OutputRule]
  , windowRules :: [(String, WindowRule)] -- ^ by lowercased app_id / X11 class
  , focusFollowsMouse :: Bool -- ^ hovering a window focuses it (no raise)
  }

-- | What to do with a new window whose app_id (Wayland) or class (X11)
-- matches. 'Nothing' leaves that part to the usual behaviour.
data WindowRule = WindowRule
  { ruleWorkspace :: Maybe Int -- ^ open there, without moving focus
  , ruleFloat :: Maybe Bool -- ^ float, or tile even a dialog
  }

-- | Connector name ("eDP-1"), scale (0 = leave it), optional position.
type OutputRule = (String, Double, Maybe (Int, Int))

type KeyMap = M.Map (KeyMask, KeySym) (X ())

data WState = WState
  { windowSet :: W.StackSet Int H.ViewId -- ^ layout index per workspace
  , screens :: [(Rect, Rect)] -- ^ per screen: whole output, usable area (minus bars)
  , ratio :: Double
  , floating :: M.Map H.ViewId Rect -- ^ floating windows and where they sit
  , fullscreen :: S.Set H.ViewId
  }

-- | The config can change at runtime (hot reload), so it lives in an
-- IORef next to the key map built from it.
data XEnv = XEnv
  { xConfig :: IORef Config
  , xKeyMap :: IORef KeyMap
  , xState :: IORef WState
  , xReload :: X () -- ^ re-read the config file (see "Whiplash.Run")
  }

newtype X a = X (ReaderT XEnv IO a)
  deriving (Functor, Applicative, Monad, MonadIO, MonadReader XEnv)

runX :: XEnv -> X a -> IO a
runX env (X m) = runReaderT m env

io :: IO a -> X a
io = liftIO

getConfig :: X Config
getConfig = asks xConfig >>= io . readIORef

getState :: X WState
getState = asks xState >>= io . readIORef

modifyState :: (WState -> WState) -> X ()
modifyState f = asks xState >>= io . flip modifyIORef' f

-- | Change the window set, then push the result to the screen.
windows :: (W.StackSet Int H.ViewId -> W.StackSet Int H.ViewId) -> X ()
windows f = modifyState (\s -> s {windowSet = f (windowSet s)}) >> refresh

refresh :: X ()
refresh = do
  cfg <- getConfig
  st <- getState
  let ws = windowSet st
      isFull v = S.member v (fullscreen st)
      floatRect v = M.lookup v (floating st)
      lays = layouts cfg
      g = gaps cfg
      -- Each screen shows one workspace, laid out in that screen's area.
      onScreen (k, tag) = case (drop k (screens st), M.lookup tag (W.workspaces ws)) of
        ((whole, usable) : _, Just w) ->
          let here = maybe [] W.integrate (W.stack w)
              tiled = [v | v <- here, not (isFull v), floatRect v == Nothing]
              lay = lays !! (W.layoutIx w `mod` length lays)
              area = shrinkRect g usable
           in ( here
              , [(v, H.Tiled, shrinkRect g r) | (v, r) <- arrange lay (ratio st) area tiled]
                  ++ [(v, H.Floating, r) | v <- here, not (isFull v), Just r <- [floatRect v]]
                  ++ [(v, H.Fullscreen, whole) | v <- here, isFull v]
              )
        _ -> ([], [])
      shown = map onScreen (M.toList (W.screenTags ws))
      wins = concatMap fst shown
      placed = concatMap snd shown
  io $ do
    -- The mode goes first: it decides the border width the geometry uses.
    forM_ placed $ \(v, mode, Rect x y w h) -> do
      H.setMode v mode
      H.setGeometry v x y w h
      H.setVisible v True
    forM_ (W.allWindows ws \\ wins) $ \v -> H.setVisible v False
    H.focusView (W.peek ws)
    H.setActiveWorkspace (W.current ws)

withFocused :: (H.ViewId -> X ()) -> X ()
withFocused f = getState >>= maybe (pure ()) f . W.peek . windowSet

-- | Float a window at the size it picked, centred on the usable area.
floatCentered :: H.ViewId -> X ()
floatCentered v = do
  bw <- borderWidth <$> getConfig
  st <- getState
  (nw, nh) <- io (H.naturalSize v)
  let Rect sx sy sw sh = focusedArea st
      fit n total fallback = if n <= 0 then fallback else min total (n + 2 * bw)
      w = fit nw sw (sw * 3 `div` 5)
      h = fit nh sh (sh * 3 `div` 5)
      r = Rect (sx + (sw - w) `div` 2) (sy + (sh - h) `div` 2) w h
  modifyState (\s -> s {floating = M.insert v r (floating s)})

-- | The usable area of the focused screen.
focusedArea :: WState -> Rect
focusedArea st = case drop (W.focusedScreen (windowSet st)) (screens st) of
  ((_, usable) : _) -> usable
  [] -> Rect 0 0 1280 720

-- | Focus the next (+1) or previous (-1) screen.
focusScreenBy :: Int -> X ()
focusScreenBy d = do
  n <- length . screens <$> getState
  if n < 2
    then pure ()
    else windows (\ws -> W.focusScreen ((W.focusedScreen ws + d) `mod` n) ws)

-- | Send the focused window to the workspace on the next/previous screen.
moveToScreenBy :: Int -> X ()
moveToScreenBy d = do
  n <- length . screens <$> getState
  if n < 2
    then pure ()
    else windows $ \ws ->
      case M.lookup ((W.focusedScreen ws + d) `mod` n) (W.screenTags ws) of
        Just t -> W.shift t ws
        Nothing -> ws

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
kill = withFocused (io . H.closeView)

quit :: X ()
quit = io H.heartQuit

-- | Re-read the config file now (it is also reloaded on every save).
reload :: X ()
reload = join (asks xReload)

nextLayout :: X ()
nextLayout = windows (W.modifyLayout (+ 1))

-- | Switch the current workspace to the layout with this name
-- (case-insensitive).
jumpToLayout :: String -> X ()
jumpToLayout name = do
  ls <- layouts <$> getConfig
  let lower = map toLower
  case lookup (lower name) (zip (map (lower . layoutName) ls) [0 ..]) of
    Just i -> windows (W.modifyLayout (const i))
    Nothing -> pure ()

incRatio :: Double -> X ()
incRatio d = modifyState (\s -> s {ratio = clamp (ratio s + d)}) >> refresh
  where
    clamp = max 0.1 . min 0.9
