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
  , workspacesFile
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
  , setUrgent
  , floatCentered
  , floatMoved
  , focusScreenBy
  , moveToScreenBy
  , OutputRule
  , WindowRule (..)
  , Scratchpad (..)
  , toggleScratchpad
  , cycleWorkspace
  , captureScratchpad
  , forget
  , withFocused
  , io
  ) where

import Control.Monad (forM_, join, unless)
import Control.Monad.Reader (MonadIO, MonadReader, ReaderT, asks, liftIO, runReaderT)
import Data.Char (toLower)
import Data.IORef
import Data.List ((\\))
import qualified Data.Map.Strict as M
import qualified Data.Set as S
import Data.Word (Word32)
import System.Directory (renameFile)
import System.Environment (lookupEnv)

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
  , scratchpads :: [(String, Scratchpad)] -- ^ by slot name
  , gestures :: [((Int, Int), X ())] -- ^ (fingers, direction 0-3: left right up down)
  }

-- | A named scratchpad slot: a floating window shown and hidden with one
-- key. The first toggle runs 'spCommand'; the window it opens is caught
-- by its app_id and kept from then on, hidden rather than closed.
data Scratchpad = Scratchpad
  { spCommand :: String
  , spAppId :: String -- ^ matched ignoring case; defaults to the slot name
  , spWidth :: Double -- ^ fraction of the screen's usable area
  , spHeight :: Double
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
  , scratch :: M.Map String H.ViewId -- ^ slot name -> its window, shown or hidden
  , urgent :: S.Set H.ViewId -- ^ windows asking for attention, until focused
  , published :: String -- ^ last contents of 'workspacesFile', to skip rewrites
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
  relocateFloats
  -- A window stops being urgent once it has focus.
  modifyState $ \s -> s {urgent = maybe id S.delete (W.peek (windowSet s)) (urgent s)}
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
      counts = [(t, maybe 0 (length . W.integrate) (W.stack w)) | (t, w) <- M.toList (W.workspaces ws)]
  io $ do
    -- The mode goes first: it decides the border width the geometry uses.
    forM_ placed $ \(v, mode, Rect x y w h) -> do
      H.setMode v mode
      H.setGeometry v x y w h
      H.setVisible v True
    forM_ (W.allWindows ws \\ wins) $ \v -> H.setVisible v False
    H.focusView (W.peek ws)
    H.setActiveWorkspace (W.current ws)
    H.setWorkspaceCounts counts
    let urgentTags = [t | v <- S.toList (urgent st), Just t <- [W.findTag v ws]]
    forM_ (M.keys (W.workspaces ws)) $ \t -> H.setWorkspaceUrgent t (t `elem` urgentTags)
  publishWorkspaces counts

-- | Floating windows belong to their workspace, not to a screen: when a
-- workspace is shown on another screen (greedy view, a monitor unplugged),
-- its floats move along, keeping their offset from the screen's corner.
relocateFloats :: X ()
relocateFloats = modifyState $ \st ->
  let ws = windowSet st
      wholes = map fst (screens st)
      onScreen k tag fl = case (drop k (screens st), M.lookup tag (W.workspaces ws)) of
        ((whole, _) : _, Just w) ->
          foldr (M.adjust (relocate wholes whole)) fl (maybe [] W.integrate (W.stack w))
        _ -> fl
   in st {floating = M.foldrWithKey onScreen (floating st) (W.screenTags ws)}

-- | Bring a floating rect onto screen @to@. One already there stays put;
-- one on another screen keeps its offset from that screen's corner; one on
-- no screen (its monitor is gone) is centred. Then it is fitted inside.
relocate :: [Rect] -> Rect -> Rect -> Rect
relocate wholes to r@(Rect x y w h)
  | inside to = r
  | otherwise = fit $ case filter inside wholes of
      (from : _) -> Rect (x - rectX from + rectX to) (y - rectY from + rectY to) w h
      [] -> Rect (rectX to + (rectW to - w) `div` 2) (rectY to + (rectH to - h) `div` 2) w h
  where
    (cx, cy) = (x + w `div` 2, y + h `div` 2)
    inside (Rect sx sy sw sh) = cx >= sx && cx < sx + sw && cy >= sy && cy < sy + sh
    fit (Rect fx fy fw fh) =
      let w' = min fw (rectW to)
          h' = min fh (rectH to)
          clamp lo len v = max lo (min v (lo + len))
       in Rect (clamp (rectX to) (rectW to - w') fx) (clamp (rectY to) (rectH to - h') fy) w' h'

-- | A floating window was moved or resized (end of a drag, or an X11
-- window resizing itself). Dropped on another screen, it joins the
-- workspace shown there, as in xmonad.
floatMoved :: H.ViewId -> Rect -> X ()
floatMoved v r@(Rect x y w h) = do
  modifyState (\s -> s {floating = M.adjust (const r) v (floating s)})
  st <- getState
  let ws = windowSet st
      (cx, cy) = (x + w `div` 2, y + h `div` 2)
      inside (Rect sx sy sw sh) = cx >= sx && cx < sx + sw && cy >= sy && cy < sy + sh
      target = [t | (k, (whole, _)) <- zip [0 ..] (screens st), inside whole
                  , Just t <- [M.lookup k (W.screenTags ws)]]
  case (target, W.findTag v ws) of
    (t : _, Just from) | t /= from -> windows (W.focusWindow v . W.shiftWin t v)
    _ -> pure ()

-- | Where bars read how many windows each workspace holds, a d77 convention
-- shared with wasp and spitfire (ext-workspace has no "occupied" state):
-- one line per workspace, "<ext-workspace id> <window count>".
workspacesFile :: IO (Maybe FilePath)
workspacesFile = do
  dir <- lookupEnv "XDG_RUNTIME_DIR"
  disp <- lookupEnv "WAYLAND_DISPLAY"
  pure $ case (dir, disp) of
    (Just d, Just w) | not (null d), not (null w) -> Just (d ++ "/d77-workspaces-" ++ w)
    _ -> Nothing

-- | Rewrite 'workspacesFile' when a workspace gains or loses windows.
-- Written beside it and renamed, so readers never see half a file. The
-- ids match the heart's ext-workspace ids ("whiplash-<n>").
publishWorkspaces :: [(Int, Int)] -> X ()
publishWorkspaces counts = do
  st <- getState
  let body = concat ["whiplash-" ++ show t ++ " " ++ show count ++ "\n" | (t, count) <- counts]
  unless (body == published st) $ do
    path <- io workspacesFile
    -- Only what was really written counts as published.
    forM_ path $ \p -> do
      io $ writeFile (p ++ ".new") body >> renameFile (p ++ ".new") p
      modifyState (\s -> s {published = body})

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
  s { floating = M.delete v (floating s)
    , fullscreen = S.delete v (fullscreen s)
    , scratch = M.filter (/= v) (scratch s)
    , urgent = S.delete v (urgent s)
    }

-- | A window asks for attention, or (X11) drops the hint. The focused
-- window is never urgent: it already has the user's attention.
setUrgent :: H.ViewId -> Bool -> X ()
setUrgent v on = do
  focused <- W.peek . windowSet <$> getState
  unless (on && focused == Just v) $ do
    modifyState (\s -> s {urgent = (if on then S.insert else S.delete) v (urgent s)})
    refresh

-- | The next (+1) or previous (-1) workspace, wrapping (9 -> 1, 1 -> 9).
cycleWorkspace :: Int -> X ()
cycleWorkspace d = do
  tags <- workspaceTags <$> getConfig
  cur <- W.current . windowSet <$> getState
  case lookup cur (zip tags [0 ..]) of
    Nothing -> pure ()
    Just i -> windows (W.view (tags !! ((i + d) `mod` length tags)))

-- | Show or hide a scratchpad slot. Empty slot: run its command (the
-- window is caught on map). Shown on the current workspace: hide it, out
-- of every workspace. Hidden or elsewhere: bring it here, centred, focused.
toggleScratchpad :: String -> X ()
toggleScratchpad name = do
  cfg <- getConfig
  st <- getState
  case (lookup name (scratchpads cfg), M.lookup name (scratch st)) of
    (Nothing, _) -> pure ()
    (Just sp, Nothing) -> spawn (spCommand sp)
    (Just sp, Just v)
      | v `elem` here (windowSet st) -> do
          windows (W.delete v)
          io (H.setVisible v False)
      | otherwise -> do
          placeScratchpad sp v
          windows (W.insertUp v . W.delete v)
  where
    here = maybe [] W.integrate . W.stack . W.currentWorkspace

-- | A new window: if it belongs to an empty slot, keep it there and show
-- it as the slot's floating window. True when it was caught.
captureScratchpad :: String -> H.ViewId -> X Bool
captureScratchpad app v = do
  cfg <- getConfig
  st <- getState
  let free = [ (n, sp) | (n, sp) <- scratchpads cfg
             , map toLower (spAppId sp) == map toLower app
             , not (M.member n (scratch st)) ]
  case free of
    [] -> pure False
    (n, sp) : _ -> do
      modifyState (\s -> s {scratch = M.insert n v (scratch s)})
      placeScratchpad sp v
      windows (W.insertUp v)
      pure True

-- | Float a scratchpad window centred on the focused screen at its size.
placeScratchpad :: Scratchpad -> H.ViewId -> X ()
placeScratchpad sp v = do
  st <- getState
  let Rect sx sy sw sh = focusedArea st
      w = round (fromIntegral sw * spWidth sp)
      h = round (fromIntegral sh * spHeight sp)
      r = Rect (sx + (sw - w) `div` 2) (sy + (sh - h) `div` 2) w h
  modifyState $ \s ->
    s {floating = M.insert v r (floating s), fullscreen = S.delete v (fullscreen s)}

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
