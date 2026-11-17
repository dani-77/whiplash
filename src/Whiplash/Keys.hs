-- | Modifier masks and the handful of keysyms a config usually needs.
-- Bindings see the unshifted keysym (see heart.c), so letters are always
-- lowercase and digits stay digits on every keyboard layout.
module Whiplash.Keys
  ( KeyMask
  , KeySym
  , shiftMask
  , controlMask
  , mod1Mask
  , mod4Mask
  , xK
  , xK_Return
  , xK_space
  , xK_Tab
  , xK_Escape
  , xK_BackSpace
  , xK_Print
  , xF86AudioRaiseVolume
  , xF86AudioLowerVolume
  , xF86AudioMute
  ) where

import Data.Char (ord)
import Data.Word (Word32)

type KeyMask = Word32
type KeySym = Word32

-- | Same bit values as wlroots' enum wlr_keyboard_modifier.
shiftMask, controlMask, mod1Mask, mod4Mask :: KeyMask
shiftMask = 1
controlMask = 4
mod1Mask = 8 -- Alt
mod4Mask = 64 -- Super / Logo

-- | Keysym of a printable ASCII key: @xK 'j'@, @xK '1'@, @xK ','@.
xK :: Char -> KeySym
xK = fromIntegral . ord

xK_Return, xK_space, xK_Tab, xK_Escape, xK_BackSpace, xK_Print :: KeySym
xK_Return = 0xff0d
xK_space = 0x0020
xK_Tab = 0xff09
xK_Escape = 0xff1b
xK_BackSpace = 0xff08
xK_Print = 0xff61

xF86AudioRaiseVolume, xF86AudioLowerVolume, xF86AudioMute :: KeySym
xF86AudioRaiseVolume = 0x1008ff13
xF86AudioLowerVolume = 0x1008ff11
xF86AudioMute = 0x1008ff12
