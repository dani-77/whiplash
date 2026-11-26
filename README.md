<p align="center"><img src="assets/logo/whiplash-256.png" width="160" alt="whiplash logo"></p>

<h1 align="center">whiplash</h1>

<p align="center">An xmonad-style tiling window manager for Xorg and compositor for Wayland,
with a Haskell core and one TOML config that reloads when you save it.</p>

---

Whiplash uses the same Haskell layouts, window-management actions and workspaces
on both backends. The Wayland compositor uses [wlroots] and starts by default.
The native X11 backend uses XCB to manage windows in an existing Xorg session.
The D77 shells for the two sessions are [Utumno] on Wayland and [ATG] on Xorg.

The name comes from Metallica's *Whiplash*.

## Features

- **Three layouts**, switchable per workspace: `tall` (master on the left, the rest
  stacked on the right), `wide` (master on top), and `full` (one window at a time).
- **One config file** (`~/.config/whiplash/config.toml`) for keybindings, colors,
  gaps and autostart. Save it to reload settings and bindings. Whiplash keeps the last
  working config if a save contains an error.
- **Up to 9 workspaces** with keyboard and bar controls.
- **Window rules** — send an app to its own workspace every time it opens, or make it
  float (or never float), by its name.
- **Dialogs and fixed-size windows** automatically open as floating windows.
- **Mouse dragging** — hold `Mod` and drag with the left button to move a window
  or with the right button to resize it. Dragging a tiled window makes it float.
- **Click to focus** on both backends. Set `focus_follows_mouse = true` to focus
  windows as the pointer enters them.
- **Touchpad settings** — tap to click, tap and drag, natural scrolling and
  disable while typing. X11 applies the settings exposed by the Xorg libinput
  driver on each touchpad.
- **Scratchpads** — a drop-down terminal (or any app) that one key shows and hides,
  without closing it, wherever you are.
- **Fullscreen** from a shortcut or at the application's request.
- Works as a real login-screen session (greetd, SDDM, GDM, ...) or on a nested
  display for trying it out.

### Wayland features

- **Multiple monitors** — asking for a workspace that's showing on another screen
  swaps the two.
- **Touchpad gestures** — swipe with three fingers to change workspace, or bind any
  swipe to anything a key can do. Pinch zoom and other gestures still reach your apps.
- **Workspaces in your bar** — bars that support Wayland's standard workspace protocol
  show and switch whiplash's workspaces with no extra setup, and see which one holds a
  window asking for your attention.
- **A lock screen that actually works** — compatible with swaylock and any other
  standard Wayland lock screen. If the lock screen crashes, the session stays locked.
- **Idle and power** — swayidle can lock and turn the screen off when you step away,
  and a playing video keeps that from happening.
- **Screenshots and screen sharing** — grim and friends work, and so does sharing your
  screen in a browser or video call.
- **Monitors and night light** — set up screens on the fly with wlr-randr, kanshi or
  wdisplays, and warm the colors at night with wlsunset or gammastep.
- **Runs your existing Linux apps**, X11 ones included, through XWayland.

The X11 backend currently manages one X screen. ATG can switch workspaces and show
the active workspace. With the Whiplash workspace hints, ATG also dims empty inactive
workspaces and marks urgent workspaces in red. Per-monitor work areas are still
pending on X11.

## Installing

To build this branch, install GHC and Cabal along with the development packages
for both backends.

<details>
<summary><b>Void Linux</b></summary>

```sh
sudo xbps-install -S base-devel ghc cabal-install pkg-config wayland-devel \
    wayland-protocols wlroots0.20-devel libxkbcommon-devel libinput-devel \
    libxcb-devel xcb-util-wm-devel xorg-server-xwayland xdg-desktop-portal-wlr \
    xdg-desktop-portal-gtk
```
</details>

<details>
<summary><b>Arch Linux</b></summary>

```sh
sudo pacman -S base-devel ghc ghc-static cabal-install pkgconf wayland \
    wayland-protocols wlroots0.20 libxkbcommon libinput libxcb xcb-util-wm \
    xorg-xwayland xdg-desktop-portal-wlr xdg-desktop-portal-gtk
```
</details>

whiplash only uses the libraries that come with GHC itself, so the build never needs to
download anything.

Then clone and install:

```sh
git clone --branch x11 https://github.com/dani-77/whiplash.git
cd whiplash
make
sudo make install
```

### Choosing a login session

| Login entry | Session file installed under `/usr/share` | Backend |
| --- | --- | --- |
| **Whiplash** | `wayland-sessions/whiplash.desktop` | Wayland |
| **Whiplash · X11** | `xsessions/whiplash-x11.desktop` | Xorg |

Both entries call the [session script](data/whiplash-session), installed as
`/usr/bin/whiplash-session`. The X11 entry passes `--backend x11`. When the
system ATG configuration and `qs` are available, the script starts ATG via
`-s 'QT_QPA_PLATFORM=xcb qs -n -d -c atg'` and reserves 48 pixels at the top.
Edit the session script to change this automatic launch. Set
`WHIPLASH_X11_RESERVE_TOP` in the session environment to choose a different
reservation. The launch command lives in the script and the rest of your
autostart list lives in `config.toml`.

Utumno on Wayland is started by the built-in autostart or your TOML autostart.
If you launch the X11 binary directly, you can still select your own startup
command with `-s`.

To try Wayland inside your current desktop:

```sh
make run
```

For X11, start Xephyr on a free display. This example uses `:99`.

```sh
Xephyr :99 -screen 1280x720 -nolisten tcp
```

Once the Xephyr window appears, run Whiplash in another terminal with `-n` to
skip the TOML autostart.

```sh
DISPLAY=:99 ./build/whiplash --backend x11 -n -s xterm
```

To uninstall later: `sudo make uninstall`.

## Getting started

whiplash starts with a working set of keybindings even without a config file. To make
it yours, copy the example config, which is also the full reference with every option
explained:

```sh
mkdir -p ~/.config/whiplash
cp /usr/share/whiplash/config.toml ~/.config/whiplash/config.toml
```

The [example config](examples/config.toml) includes an X11 profile with ATG
shortcuts and `scrot` for screenshots. The Wayland defaults use Utumno and
`grim`. Set `terminal` to the terminal you use.

### Default keybindings

`Mod` is the `Alt` key by default (`mod = "Alt"` in the config). Every one of these is
just a line in `config.toml`, so rebind anything to anything.

| Keys | Action |
| --- | --- |
| `Mod` + `Shift` + `Return` | Open a terminal |
| `Mod` + `P` | Open the run menu (wmenu on Wayland, dmenu on X11) |
| `Mod` + `J` / `K` | Move focus to the next / previous window |
| `Mod` + `Shift` + `J` / `K` | Swap the focused window with the next / previous one |
| `Mod` + `Return` | Swap the focused window with the master |
| `Mod` + `H` / `L` | Shrink / grow the master area |
| `Mod` + `T` / `W` / `M` | Switch layout: tall / wide / full |
| `Mod` + `Space` | Cycle through layouts |
| `Mod` + `Shift` + `Space` | Float / tile the focused window |
| `Mod` + `E` | Fullscreen the focused window |
| `Mod` + `1`-`9` | Switch to workspace 1-9 |
| `Mod` + `Shift` + `1`-`9` | Move the focused window to workspace 1-9 |
| `Mod` + `Tab` | Back to the previous workspace |
| `Mod` + `,` / `.` | Focus the previous / next monitor (Wayland, example config) |
| `Mod` + `Shift` + `,` / `.` | Move the focused window to the previous / next monitor (Wayland, example config) |
| `Mod` + left drag | Float and move a window |
| `Mod` + right drag | Float and resize a window |
| `Mod` + `Shift` + `C` | Close the focused window |
| `Mod` + `Shift` + `R` | Reload the config (example config) |
| `Mod` + `Shift` + `Q` | Quit whiplash (ends the session) |
| `Ctrl` + `Alt` + `F1`-`F12` | Switch to another virtual terminal |

## Configuring

Your Whiplash settings live in `~/.config/whiplash/config.toml`. Anything you leave
out keeps its default. This example uses Wayland apps and input settings.

```toml
terminal = "foot"
mod = "Super"            # Alt, Super, Ctrl or Shift
gaps = 4
focus_follows_mouse = true   # point at a window to focus it
layouts = ["tall", "wide", "full"]
border = { width = 2, focused = "#aa96da", normal = "#3b3b4f" }

# Started once at login
autostart = ["waybar", "mako", "swaybg -i ~/wallpaper.png"]

# Monitors, by connector name (see wlr-randr)
[outputs."eDP-1"]
scale = 1.25

[keyboard]
layout = "pt"            # or "us", "de,us", whatever setxkbmap would take
options = "caps:escape"
repeat_rate = 25         # repeats per second
repeat_delay = 600       # ms before a held key starts repeating

[touchpad]
tap = true
natural_scroll = true
speed = 0.2              # -1 (slow) .. 1 (fast)

# Window rules, by app name (the log lists the name of every new window)
[rules."firefox"]
workspace = 2

[rules."pavucontrol"]
float = true

# A drop-down terminal on Mod+backslash
[scratchpads."term"]
command = "foot --app-id scratch-term"
app_id = "scratch-term"

# Three-finger swipes change workspace
[gestures]
"3-left" = "workspace-prev"
"3-right" = "workspace-next"

[keybinds]
"Mod+backslash" = "toggle-scratchpad term"
"Mod+Shift+Return" = "terminal"
"Mod+j" = "focus-down"
"Super+l" = "spawn swaylock"
```

The top-level `[keybinds]` table replaces the compiled bindings. Add
`[x11.keybinds]` or `[wayland.keybinds]` to override individual bindings in one
session. Set a binding in one of those tables to `false` to remove it for that
backend. An `autostart` list under `[x11]` or `[wayland]` replaces the shared
list at the next login.

The login script keeps its log in
`${XDG_STATE_HOME:-$HOME/.local/state}/whiplash/whiplash.log`. The previous
session's log is `whiplash.log.old` in the same directory.

## License

MIT — see [LICENSE](LICENSE). The C core started from wlroots' tinywl example (CC0).

Looking to build or change whiplash itself, rather than just use it? See
[`doc/README.md`](doc/README.md) for the technical overview.

[wlroots]: https://gitlab.freedesktop.org/wlroots/wlroots
[Utumno]: https://github.com/dani-77/utumno
[ATG]: https://github.com/d77void/ATG
