{-# LANGUAGE ForeignFunctionInterface #-}
-- | Raw bindings to the C heart (heart/heart.h). Nothing here makes a
-- decision; this module only moves values across the FFI boundary.
module Whiplash.Heart
  ( ViewId
  , ViewCallback
  , KeyCallback
  , OutputInfo (..)
  , onViewMapped
  , onViewUnmapped
  , onViewClicked
  , onViewEntered
  , onKey
  , onSwipe
  , onOutputsChanged
  , outputs
  , setOutputRules
  , applyOutputRules
  , onViewFullscreen
  , onViewUrgent
  , setWorkspaceUrgent
  , onViewGrab
  , onViewMoved
  , ViewMode (..)
  , setMode
  , wantsFloat
  , appId
  , naturalSize
  , setMouseMod
  , onWorkspaceActivate
  , addWorkspace
  , setActiveWorkspace
  , onConfigChanged
  , setKeyboard
  , setTouchpad
  , keysymFromName
  , watchConfig
  , heartInit
  , heartRun
  , heartQuit
  , heartShutdown
  , setGeometry
  , setVisible
  , focusView
  , closeView
  , setBorder
  , spawnCommand
  ) where

import Data.Word (Word32)
import Foreign.C.String (CString, peekCString, withCString)
import Foreign.C.Types (CDouble (..), CFloat (..), CInt (..))
import Foreign.Marshal.Array (allocaArray, peekArray)
import Foreign.Ptr (FunPtr, Ptr, nullPtr)

type ViewId = Word32

type ViewCallback = ViewId -> IO ()
type KeyCallback = Word32 -> Word32 -> IO Bool
-- | A screen: connector name, full box and the usable box left after bars,
-- each as (x, y, width, height).
data OutputInfo = OutputInfo
  { outputName :: String
  , outputFull :: (Int, Int, Int, Int)
  , outputUsable :: (Int, Int, Int, Int)
  }

data ViewMode = Tiled | Floating | Fullscreen
  deriving (Show, Eq, Enum)

type CViewCb = Word32 -> IO ()
type CKeyCb = Word32 -> Word32 -> IO CInt
type CSwipeCb = CInt -> CInt -> IO CInt
type CStateCb = Word32 -> CInt -> IO ()
type CRectCb = Word32 -> CInt -> CInt -> CInt -> CInt -> IO ()
type CTagCb = CInt -> IO ()
type CVoidCb = IO ()

foreign import ccall "wrapper" mkViewCb :: CViewCb -> IO (FunPtr CViewCb)
foreign import ccall "wrapper" mkKeyCb :: CKeyCb -> IO (FunPtr CKeyCb)
foreign import ccall "wrapper" mkSwipeCb :: CSwipeCb -> IO (FunPtr CSwipeCb)
foreign import ccall unsafe "wh_on_swipe" c_on_swipe :: FunPtr CSwipeCb -> IO ()
foreign import ccall "wrapper" mkStateCb :: CStateCb -> IO (FunPtr CStateCb)
foreign import ccall "wrapper" mkRectCb :: CRectCb -> IO (FunPtr CRectCb)
foreign import ccall "wrapper" mkTagCb :: CTagCb -> IO (FunPtr CTagCb)
foreign import ccall "wrapper" mkVoidCb :: CVoidCb -> IO (FunPtr CVoidCb)

foreign import ccall unsafe "wh_on_view_mapped" c_on_view_mapped :: FunPtr CViewCb -> IO ()
foreign import ccall unsafe "wh_on_view_unmapped" c_on_view_unmapped :: FunPtr CViewCb -> IO ()
foreign import ccall unsafe "wh_on_view_clicked" c_on_view_clicked :: FunPtr CViewCb -> IO ()
foreign import ccall unsafe "wh_on_view_entered" c_on_view_entered :: FunPtr CViewCb -> IO ()
foreign import ccall unsafe "wh_on_key" c_on_key :: FunPtr CKeyCb -> IO ()
foreign import ccall unsafe "wh_on_outputs_changed" c_on_outputs_changed :: FunPtr CVoidCb -> IO ()
foreign import ccall unsafe "wh_output_count" c_output_count :: IO CInt
foreign import ccall unsafe "wh_output_info" c_output_info :: CInt -> Ptr CInt -> IO CInt
foreign import ccall unsafe "wh_output_name" c_output_name :: CInt -> IO CString
foreign import ccall unsafe "wh_output_rules_clear" c_output_rules_clear :: IO ()
foreign import ccall unsafe "wh_output_rule_add" c_output_rule_add
  :: CString -> CFloat -> CInt -> CInt -> CInt -> IO ()
foreign import ccall safe "wh_output_rules_apply" c_output_rules_apply :: IO ()
foreign import ccall unsafe "wh_on_view_fullscreen" c_on_view_fullscreen :: FunPtr CStateCb -> IO ()
foreign import ccall unsafe "wh_on_view_urgent" c_on_view_urgent :: FunPtr CStateCb -> IO ()
foreign import ccall unsafe "wh_workspace_set_urgent" c_workspace_set_urgent :: CInt -> CInt -> IO ()
foreign import ccall unsafe "wh_on_view_grab" c_on_view_grab :: FunPtr CRectCb -> IO ()
foreign import ccall unsafe "wh_on_view_moved" c_on_view_moved :: FunPtr CRectCb -> IO ()
foreign import ccall unsafe "wh_view_set_mode" c_set_mode :: Word32 -> CInt -> IO ()
foreign import ccall unsafe "wh_view_app_id" c_app_id :: Word32 -> IO CString
foreign import ccall unsafe "wh_view_wants_float" c_wants_float :: Word32 -> IO CInt
foreign import ccall unsafe "wh_view_natural_width" c_natural_width :: Word32 -> IO CInt
foreign import ccall unsafe "wh_view_natural_height" c_natural_height :: Word32 -> IO CInt
foreign import ccall unsafe "wh_set_mouse_mod" c_set_mouse_mod :: Word32 -> IO ()
foreign import ccall unsafe "wh_on_workspace_activate" c_on_workspace_activate :: FunPtr CTagCb -> IO ()
foreign import ccall unsafe "wh_workspace_add" c_workspace_add :: CInt -> CString -> IO ()
foreign import ccall unsafe "wh_workspace_set_active" c_workspace_set_active :: CInt -> IO ()
foreign import ccall unsafe "wh_on_config_changed" c_on_config_changed :: FunPtr CVoidCb -> IO ()
foreign import ccall unsafe "wh_set_keyboard" c_set_keyboard
  :: CString -> CString -> CString -> CInt -> CInt -> IO ()
foreign import ccall unsafe "wh_set_touchpad" c_set_touchpad
  :: CInt -> CInt -> CInt -> CInt -> CInt -> CDouble -> CInt -> CInt -> IO ()
foreign import ccall unsafe "wh_keysym_from_name" c_keysym_from_name :: CString -> IO Word32
foreign import ccall unsafe "wh_watch_config" c_watch_config :: CString -> CString -> IO CInt

-- Calls that can re-enter Haskell through callbacks must be "safe".
foreign import ccall safe "wh_init" c_init :: IO CInt
foreign import ccall safe "wh_run" c_run :: IO ()
foreign import ccall safe "wh_shutdown" c_shutdown :: IO ()
foreign import ccall safe "wh_spawn" c_spawn :: CString -> IO ()

foreign import ccall unsafe "wh_quit" c_quit :: IO ()
foreign import ccall unsafe "wh_view_set_geometry" c_set_geometry
  :: Word32 -> CInt -> CInt -> CInt -> CInt -> IO ()
foreign import ccall unsafe "wh_view_set_visible" c_set_visible :: Word32 -> CInt -> IO ()
foreign import ccall unsafe "wh_view_focus" c_focus :: Word32 -> IO ()
foreign import ccall unsafe "wh_view_close" c_close :: Word32 -> IO ()
foreign import ccall unsafe "wh_set_border" c_set_border :: CInt -> Word32 -> Word32 -> IO ()

onViewMapped, onViewUnmapped, onViewClicked :: ViewCallback -> IO ()
onViewMapped f = mkViewCb f >>= c_on_view_mapped
onViewUnmapped f = mkViewCb f >>= c_on_view_unmapped
onViewClicked f = mkViewCb f >>= c_on_view_clicked

-- | The pointer moved onto another view (focus-follows-mouse).
onViewEntered :: ViewCallback -> IO ()
onViewEntered f = mkViewCb f >>= c_on_view_entered

-- | Touchpad swipes: called with direction -1 when a swipe starts (answer
-- True to take it from clients), then with 0 left, 1 right, 2 up, 3 down
-- when a taken swipe ends.
onSwipe :: (Int -> Int -> IO Bool) -> IO ()
onSwipe f = mkSwipeCb (\n d -> (\b -> if b then 1 else 0) <$> f (fromIntegral n) (fromIntegral d)) >>= c_on_swipe

onKey :: KeyCallback -> IO ()
onKey f = mkKeyCb (\m s -> fromBool <$> f m s) >>= c_on_key
  where
    fromBool b = if b then 1 else 0

onOutputsChanged :: IO () -> IO ()
onOutputsChanged f = mkVoidCb f >>= c_on_outputs_changed

-- | The screens, left to right then top to bottom.
outputs :: IO [OutputInfo]
outputs = do
  n <- c_output_count
  fmap concat $ mapM one [0 .. n - 1]
  where
    one i = allocaArray 8 $ \buf -> do
      r <- c_output_info i buf
      if r /= 0
        then pure []
        else do
          vs <- map fromIntegral <$> peekArray 8 buf
          namePtr <- c_output_name i
          name <- if namePtr == nullPtr then pure "" else peekCString namePtr
          case vs of
            [x, y, w, h, ux, uy, uw, uh] -> pure [OutputInfo name (x, y, w, h) (ux, uy, uw, uh)]
            _ -> pure []

-- | Per-output (name, scale, position) rules; scale <= 0 leaves it alone.
setOutputRules :: [(String, Double, Maybe (Int, Int))] -> IO ()
setOutputRules rules = do
  c_output_rules_clear
  mapM_ add rules
  where
    add (name, scale, pos) = withCString name $ \n ->
      let (x, y, has) = maybe (0, 0, 0) (\(a, b) -> (a, b, 1)) pos
       in c_output_rule_add n (realToFrac scale) (fromIntegral x) (fromIntegral y) has

applyOutputRules :: IO ()
applyOutputRules = c_output_rules_apply

onViewFullscreen :: (ViewId -> Bool -> IO ()) -> IO ()
onViewFullscreen f = mkStateCb (\v on -> f v (on /= 0)) >>= c_on_view_fullscreen

onViewGrab, onViewMoved :: (ViewId -> (Int, Int, Int, Int) -> IO ()) -> IO ()
onViewGrab f = mkRectCb (rectCb f) >>= c_on_view_grab
onViewMoved f = mkRectCb (rectCb f) >>= c_on_view_moved

rectCb :: (ViewId -> (Int, Int, Int, Int) -> IO ()) -> CRectCb
rectCb f v x y w h = f v (fromIntegral x, fromIntegral y, fromIntegral w, fromIntegral h)

setMode :: ViewId -> ViewMode -> IO ()
setMode v m = c_set_mode v (fromIntegral (fromEnum m))

-- | The Wayland app_id, or the X11 window class; "" when there is none.
appId :: ViewId -> IO String
appId v = c_app_id v >>= peekCString

-- | Dialogs and fixed-size windows ask to float.
wantsFloat :: ViewId -> IO Bool
wantsFloat v = (/= 0) <$> c_wants_float v

-- | The size the client picked for itself, without borders.
naturalSize :: ViewId -> IO (Int, Int)
naturalSize v = do
  w <- c_natural_width v
  h <- c_natural_height v
  pure (fromIntegral w, fromIntegral h)

setMouseMod :: Word32 -> IO ()
setMouseMod = c_set_mouse_mod

-- | A bar asked to switch to this workspace.
onWorkspaceActivate :: (Int -> IO ()) -> IO ()
onWorkspaceActivate f = mkTagCb (f . fromIntegral) >>= c_on_workspace_activate

-- | Announce a workspace to bars (ext-workspace-v1).
addWorkspace :: Int -> String -> IO ()
addWorkspace tag name = withCString name (c_workspace_add (fromIntegral tag))

-- | A window asks for attention (True), or an X11 one drops the hint.
onViewUrgent :: (ViewId -> Bool -> IO ()) -> IO ()
onViewUrgent f = mkStateCb (\v on -> f v (on /= 0)) >>= c_on_view_urgent

-- | Mark a workspace urgent in bars (ext-workspace).
setWorkspaceUrgent :: Int -> Bool -> IO ()
setWorkspaceUrgent t on = c_workspace_set_urgent (fromIntegral t) (if on then 1 else 0)

setActiveWorkspace :: Int -> IO ()
setActiveWorkspace = c_workspace_set_active . fromIntegral

-- | Start the backend. Returns False when wlroots could not start.
heartInit :: IO Bool
heartInit = (== 0) <$> c_init

-- | The watched config file was written.
onConfigChanged :: IO () -> IO ()
onConfigChanged f = mkVoidCb f >>= c_on_config_changed

-- | Layout, variant, options, repeat rate (Hz) and delay (ms) for physical
-- keyboards; applies at once to those already plugged in.
setKeyboard :: String -> String -> String -> Int -> Int -> IO ()
setKeyboard layout variant options rate delay =
  withCString layout $ \l -> withCString variant $ \v -> withCString options $ \o ->
    c_set_keyboard l v o (fromIntegral rate) (fromIntegral delay)

-- | Touchpad settings: tap, tap-and-drag, drag lock, natural scroll,
-- disable while typing, pointer speed (-1..1), acceleration profile
-- (0 default, 1 adaptive, 2 flat) and click method (0 default, 1 button
-- areas, 2 clickfinger). Applies at once to touchpads already plugged in.
setTouchpad :: Bool -> Bool -> Bool -> Bool -> Bool -> Double -> Int -> Int -> IO ()
setTouchpad tap drag dragLock natural dwt speed profile click =
  c_set_touchpad (flag tap) (flag drag) (flag dragLock) (flag natural) (flag dwt)
    (realToFrac speed) (fromIntegral profile) (fromIntegral click)
  where
    flag b = if b then 1 else 0

-- | Look a key name up in xkbcommon ("Return", "XF86AudioMute", ...).
keysymFromName :: String -> IO (Maybe Word32)
keysymFromName name = do
  sym <- withCString name c_keysym_from_name
  pure (if sym == 0 then Nothing else Just sym)

-- | Watch @dir/name@ for writes; False if inotify could not be set up.
watchConfig :: FilePath -> String -> IO Bool
watchConfig dir name =
  withCString dir $ \d -> withCString name $ \n -> (== 0) <$> c_watch_config d n

heartRun, heartQuit, heartShutdown :: IO ()
heartRun = c_run
heartQuit = c_quit
heartShutdown = c_shutdown

setGeometry :: ViewId -> Int -> Int -> Int -> Int -> IO ()
setGeometry v x y w h =
  c_set_geometry v (fromIntegral x) (fromIntegral y) (fromIntegral w) (fromIntegral h)

setVisible :: ViewId -> Bool -> IO ()
setVisible v b = c_set_visible v (if b then 1 else 0)

-- | Focus a view; 'Nothing' clears keyboard focus.
focusView :: Maybe ViewId -> IO ()
focusView = c_focus . maybe 0 id

closeView :: ViewId -> IO ()
closeView = c_close

setBorder :: Int -> Word32 -> Word32 -> IO ()
setBorder w = c_set_border (fromIntegral w)

spawnCommand :: String -> IO ()
spawnCommand cmd = withCString cmd c_spawn
