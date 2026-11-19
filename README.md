<p align="center"><img src="assets/logo/whiplash-256.png" width="160" alt="whiplash logo"></p>

<h1 align="center">whiplash</h1>

<p align="center">A tiling Wayland compositor for Linux, in the spirit of <a href="https://xmonad.org/">xmonad</a> —
with a Haskell brain, and one config file that reloads the moment you save it.</p>

---

whiplash is a window manager/compositor for Wayland. If you've used **xmonad**, it will
feel like home: a master window and a stack beside it, workspaces that swap between
screens, new windows opening right where you're working. The difference is the config:
instead of a Haskell file you recompile, whiplash reads a plain **TOML** file, and every
time you save it the change is live — no rebuild, no restart, no logout.

Under the hood it's two pieces: a small core written in C that talks to the screen,
keyboard and mouse (built on [wlroots]), and the window-management logic written in
Haskell, the same language xmonad uses. You don't need to know either to use it.

whiplash doesn't come with a bar, launcher or wallpaper app. It works with the ones you
already have — any bar or shell made for Wayland compositors like sway or Hyprland
(waybar, eww, Quickshell shells such as [Utumno], ...) runs on whiplash too.

The name comes from Metallica's *Whiplash*.

## Features

- **Three layouts**, switchable per workspace: `tall` (master on the left, the rest
  stacked on the right), `wide` (master on top), and `full` (one window at a time).
- **One config file** (`~/.config/whiplash/config.toml`) — keybindings, colors, gaps,
  keyboard layout, touchpad, monitors, autostart apps. Save it and it's live. A typo
  never breaks your session: whiplash keeps the last working config and shows you what's
  wrong in a notification.
- **xmonad-style workspaces and monitors** — up to 9 workspaces shared by all your
  screens; asking for a workspace that's showing on another screen swaps the two.
- **Floating windows** — dialogs float on their own; hold the modifier and drag any
  window with the left button to move it, or with the right button to resize it.
- **Fullscreen** for videos, games and presentations, on request from the app or with a
  key.
- **Touchpad that just works** — tap to click, tap and drag, and no stray clicks while
  you type, all on by default and adjustable in the config.
- **Workspaces in your bar** — bars that support Wayland's standard workspace protocol
  show and switch whiplash's workspaces with no extra setup.
- **A lock screen that actually works** — compatible with swaylock and any other
  standard Wayland lock screen. If the lock screen crashes, the session stays locked.
- **Idle and power** — swayidle can lock and turn the screen off when you step away,
  and a playing video keeps that from happening.
- **Screenshots and screen sharing** — grim and friends work, and so does sharing your
  screen in a browser or video call.
- **Monitors and night light** — set up screens on the fly with wlr-randr, kanshi or
  wdisplays, and warm the colors at night with wlsunset or gammastep.
- **Runs your existing Linux apps**, X11 ones included, through XWayland.
- Works as a real login-screen session (greetd, SDDM, GDM, ...) or nested inside your
  current desktop for trying it out first.

## Installing

There are no prebuilt packages yet — whiplash is built from source. You'll need the
Haskell compiler (GHC) and cabal, plus a handful of development packages:

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
git clone https://github.com/dani-77/whiplash.git
cd whiplash
make
sudo make install
```

This builds whiplash and installs it along with a session entry, so your login screen
(greetd, SDDM, GDM, ...) offers **whiplash** next to whatever else you have installed.
It also drops in a small file that tells your system which app handles screen sharing
and file-open dialogs while running whiplash — nothing for you to touch. Log out, pick
whiplash from the session list, log back in.

To try it first without logging out — nested inside your current desktop, in a window:

```sh
make run
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

The example is the author's own setup: it starts the [Utumno] shell, a wallpaper daemon,
notifications and so on, and the `Super` keys open Utumno's launcher, lock screen and
menus. Swap the `autostart` list and those keys for the programs you use, and change
`terminal` if you don't have alacritty.

### Default keybindings

`Mod` is the `Alt` key by default (`mod = "Alt"` in the config). Every one of these is
just a line in `config.toml`, so rebind anything to anything.

| Keys | Action |
| --- | --- |
| `Mod` + `Shift` + `Return` | Open a terminal |
| `Mod` + `P` | Open the run menu (wmenu) |
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
| `Mod` + `,` / `.` | Focus the previous / next monitor |
| `Mod` + `Shift` + `,` / `.` | Move the focused window to the previous / next monitor |
| `Mod` + left drag | Move a window (it starts floating) |
| `Mod` + right drag | Resize a window |
| `Mod` + `Shift` + `C` | Close the focused window |
| `Mod` + `Shift` + `R` | Reload the config (saving the file already does this) |
| `Mod` + `Shift` + `Q` | Quit whiplash (ends the session) |
| `Ctrl` + `Alt` + `F1`-`F12` | Switch to another virtual terminal |

## Configuring

Everything lives in `~/.config/whiplash/config.toml`. Anything you leave out keeps its
default. A few of the things you can set:

```toml
terminal = "foot"
mod = "Super"            # Alt, Super, Ctrl or Shift
gaps = 4
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

[keybinds]
"Mod+Shift+Return" = "terminal"
"Mod+j" = "focus-down"
"Super+l" = "spawn swaylock"
```

If `[keybinds]` is in the file, it replaces the whole default set, so the file always
lists every binding that's active. The example config lists every action a key can be
bound to.

Something not working? whiplash keeps its log in `~/.local/state/whiplash/whiplash.log`
(and the previous session's in `whiplash.log.old`).

## License

MIT — see [LICENSE](LICENSE). The C core started from wlroots' tinywl example (CC0).

Looking to build or change whiplash itself, rather than just use it? See
[`doc/README.md`](doc/README.md) for the technical overview.

[wlroots]: https://gitlab.freedesktop.org/wlroots/wlroots
[Utumno]: https://github.com/dani-77/utumno
