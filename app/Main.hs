-- | The d77 whiplash configuration. Like xmonad.hs, a config is just a
-- Haskell program: copy this file, change what you like, rebuild.
--
-- Keys follow the wasp config: Alt drives window management, Super opens
-- the d77 desktop pieces (Utumno through qsd77, d77run). Alt+left drag
-- moves a window (floating it), Alt+right drag resizes it.
module Main (main) where

import Whiplash

main :: IO ()
main = whiplash d77Config

d77Config :: Config
d77Config =
  Config
    { terminal = "alacritty"
    , modMask = mod1Mask -- Alt, as in wasp
    , borderWidth = 2
    , focusedBorderColor = 0xaa96daff -- d77 lilac
    , normalBorderColor = 0x3b3b4fff
    , gaps = 4
    , layouts = [tall, wide, full]
    , workspaceTags = [1 .. 9]
    , keyboard = KeyboardSettings "pt" "" "" 25 600
    , touchpad = defaultTouchpad -- tap to click, tap-and-drag, as in wasp
    , keys = d77Keys
    , startupHook = d77Autostart
    , outputRules = []
    , windowRules = []
    , focusFollowsMouse = False
    , scratchpads = []
    }

-- | Same session services as wasp's autostart, plus the Utumno shell.
d77Autostart :: X ()
d77Autostart =
  mapM_
    spawn
    [ "awww-daemon"
    , "/usr/share/quickshell/utumno/wallpaper/set-wallpaper.sh"
    , "dunst"
    , "pipewire"
    , "pipewire-pulse"
    , "udiskie -a"
    , "brightnessctl set 50%"
    , "/usr/libexec/polkit-mate-authentication-agent-1"
    , "qsd77 run -c utumno"
    , -- Idle: lock with Utumno after 5 minutes, screen off after 5.5 and
      -- back on with any input, and lock before suspending. A playing
      -- video (idle-inhibit) holds all of this off.
      "swayidle -w timeout 300 'qsd77 -c utumno locker'"
        ++ " timeout 330 \"wlopm --off '*'\" resume \"wlopm --on '*'\""
        ++ " before-sleep 'qsd77 -c utumno locker'"
    ]

d77Keys :: Config -> [((KeyMask, KeySym), X ())]
d77Keys cfg =
  [ -- Launchers
    ((m .|. shiftMask, xK_Return), spawn (terminal cfg))
  , ((m, xK 'p'), spawn "wmenu-run")
  , -- D77 (Super)
    ((mod4Mask, xK_space), utumno "launcher")
  , ((mod4Mask, xK 'a'), utumno "ollama")
  , ((mod4Mask, xK 't'), utumno "locker")
  , ((mod4Mask, xK 'x'), utumno "session")
  , ((mod4Mask, xK 'y'), utumno "wallpaper")
  , ((mod4Mask, xK 'r'), spawn "d77run")
  , -- Media keys and screenshot
    ((0, xF86AudioRaiseVolume), spawn "amixer -q set Master 5%+")
  , ((0, xF86AudioLowerVolume), spawn "amixer -q set Master 5%-")
  , ((0, xF86AudioMute), spawn "amixer -q set Master toggle")
  , ((0, xK_Print), spawn "grim ~/screenshot-$(date +%Y%m%d-%H%M%S).png")
  , -- Window navigation
    ((m, xK 'j'), windows focusDown)
  , ((m, xK 'k'), windows focusUp)
  , ((m, xK_Tab), windows toggleWS)
  , ((m, xK_Return), windows swapMaster)
  , ((m .|. shiftMask, xK 'j'), windows swapDown)
  , ((m .|. shiftMask, xK 'k'), windows swapUp)
  , -- Resize
    ((m, xK 'h'), incRatio (-0.05))
  , ((m, xK 'l'), incRatio 0.05)
  , -- Layouts
    ((m, xK 't'), jumpToLayout "Tall")
  , ((m, xK 'w'), jumpToLayout "Wide")
  , ((m, xK 'm'), jumpToLayout "Full")
  , ((m, xK_space), nextLayout)
  , ((m .|. shiftMask, xK_space), toggleFloat)
  , ((m, xK 'e'), toggleFullscreen)
  , -- Session
    ((m .|. shiftMask, xK 'c'), kill)
  , ((m .|. shiftMask, xK 'q'), quit)
  ]
    ++ [ ((m .|. extra, xK c), windows (f t))
       | (t, c) <- zip (workspaceTags cfg) ['1' .. '9']
       , (extra, f) <- [(0, view), (shiftMask, shift)]
       ]
  where
    m = modMask cfg
    utumno action = spawn ("qsd77 -c utumno " ++ action)
