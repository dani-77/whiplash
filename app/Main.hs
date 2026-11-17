-- | The default whiplash configuration. Like xmonad.hs, a config is just a
-- Haskell program: copy this file, change what you like, rebuild.
module Main (main) where

import Whiplash

main :: IO ()
main = whiplash defaultConfig

defaultConfig :: Config
defaultConfig =
  Config
    { terminal = "alacritty"
    , modMask = mod4Mask
    , borderWidth = 2
    , focusedBorderColor = 0xaa96daff -- d77 lilac
    , normalBorderColor = 0x3b3b4fff
    , gaps = 4
    , layouts = [tall, wide, full]
    , workspaceTags = [1 .. 9]
    , keyboardLayout = Just "pt"
    , keys = defaultKeys
    , startupHook = pure ()
    }

defaultKeys :: Config -> [((KeyMask, KeySym), X ())]
defaultKeys cfg =
  [ ((m .|. shiftMask, xK_Return), spawn (terminal cfg))
  , ((m .|. shiftMask, xK 'c'), kill)
  , ((m, xK_space), nextLayout)
  , ((m, xK 'j'), windows focusDown)
  , ((m, xK 'k'), windows focusUp)
  , ((m, xK_Tab), windows focusDown)
  , ((m .|. shiftMask, xK 'j'), windows swapDown)
  , ((m .|. shiftMask, xK 'k'), windows swapUp)
  , ((m, xK_Return), windows swapMaster)
  , ((m, xK 'h'), incRatio (-0.05))
  , ((m, xK 'l'), incRatio 0.05)
  , ((m .|. shiftMask, xK 'q'), quit)
  ]
    ++ [ ((m .|. extra, xK c), windows (f t))
       | (t, c) <- zip (workspaceTags cfg) ['1' .. '9']
       , (extra, f) <- [(0, view), (shiftMask, shift)]
       ]
  where
    m = modMask cfg
