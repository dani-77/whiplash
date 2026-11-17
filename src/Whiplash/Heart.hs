{-# LANGUAGE ForeignFunctionInterface #-}
-- | Raw bindings to the C heart (heart/heart.h). Nothing here makes a
-- decision; this module only moves values across the FFI boundary.
module Whiplash.Heart
  ( ViewId
  , ViewCallback
  , KeyCallback
  , OutputCallback
  , onViewMapped
  , onViewUnmapped
  , onViewClicked
  , onKey
  , onOutput
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
import Foreign.C.String (CString, withCString)
import Foreign.C.Types (CInt (..))
import Foreign.Ptr (FunPtr, nullPtr)

type ViewId = Word32

type ViewCallback = ViewId -> IO ()
type KeyCallback = Word32 -> Word32 -> IO Bool
type OutputCallback = Int -> Int -> Int -> Int -> IO ()

type CViewCb = Word32 -> IO ()
type CKeyCb = Word32 -> Word32 -> IO CInt
type COutputCb = CInt -> CInt -> CInt -> CInt -> IO ()

foreign import ccall "wrapper" mkViewCb :: CViewCb -> IO (FunPtr CViewCb)
foreign import ccall "wrapper" mkKeyCb :: CKeyCb -> IO (FunPtr CKeyCb)
foreign import ccall "wrapper" mkOutputCb :: COutputCb -> IO (FunPtr COutputCb)

foreign import ccall unsafe "wh_on_view_mapped" c_on_view_mapped :: FunPtr CViewCb -> IO ()
foreign import ccall unsafe "wh_on_view_unmapped" c_on_view_unmapped :: FunPtr CViewCb -> IO ()
foreign import ccall unsafe "wh_on_view_clicked" c_on_view_clicked :: FunPtr CViewCb -> IO ()
foreign import ccall unsafe "wh_on_key" c_on_key :: FunPtr CKeyCb -> IO ()
foreign import ccall unsafe "wh_on_output" c_on_output :: FunPtr COutputCb -> IO ()

-- Calls that can re-enter Haskell through callbacks must be "safe".
foreign import ccall safe "wh_init" c_init :: CString -> IO CInt
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

onKey :: KeyCallback -> IO ()
onKey f = mkKeyCb (\m s -> fromBool <$> f m s) >>= c_on_key
  where
    fromBool b = if b then 1 else 0

onOutput :: OutputCallback -> IO ()
onOutput f = mkOutputCb cb >>= c_on_output
  where
    cb x y w h = f (fromIntegral x) (fromIntegral y) (fromIntegral w) (fromIntegral h)

-- | Start the backend. Returns False when wlroots could not start.
heartInit :: Maybe String -> IO Bool
heartInit layout = (== 0) <$> maybe (c_init nullPtr) (`withCString` c_init) layout

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
