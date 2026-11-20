<p align="center"><img src="../assets/logo/whiplash-192.png" width="96" alt="whiplash logo"></p>

# whiplash (developer notes)

This is the technical README: architecture, the C/Haskell boundary, how to build and
test, protocol coverage, and why things are the way they are. If you just want to *use*
whiplash as your Wayland session, see the [root README](../README.md).

whiplash is an xmonad-style Wayland compositor with a **C heart** on wlroots 0.20 and a
**Haskell brain**. Haskell had no living Wayland compositor: waymonad and hsroots
stopped in 2019, and wlhs (Haskell wlroots bindings) is incomplete and stuck on
wlroots 0.17. Binding all of wlroots to Haskell is where those projects stalled, so
whiplash doesn't try: the wlroots plumbing stays in a small C layer, and the
window-management decisions live in Haskell, much like somewm pairs C with Lua.

## Layout of the source

```
heart/heart.c   C, derived from tinywl (CC0). Owns everything wlroots:
                outputs, input, scene graph, protocols. Makes NO decisions.
heart/heart.h   The whole C <-> Haskell contract. Read this first.
src/Whiplash/
  Heart.hs      FFI bindings to heart.h, nothing else.
  StackSet.hs   Pure xmonad-like model: workspaces (zippers), screens,
                focus, greedy view, shift. No IO.
  Layout.hs     Pure layouts: tall, wide, full.
  Core.hs       Config, the X monad, refresh (places every window on every
                screen), actions (spawn, kill, float, fullscreen, ...).
  Keys.hs       Modifier masks and keysyms.
  Toml.hs       Hand-written TOML reader (see below).
  Settings.hs   config.toml -> Config; key combos; the action vocabulary.
  Run.hs        Entry point: loads the config, wires heart callbacks, hot reload.
app/Main.hs     Compiled-in defaults; config.toml overrides them.
examples/       config.toml: the defaults written out, installed to
                share/whiplash/config.toml.
data/           whiplash-session (login wrapper), whiplash.desktop,
                whiplash-portals.conf.
protocols/      wlr-layer-shell and wlr-output-power-management XML.
```

## The heart/brain boundary

The heart identifies windows by integer view ids and reports events through callbacks
registered from Haskell: map, unmap, click, key, fullscreen request, drag grab and move,
outputs changed, workspace activation (from a bar), config file changed. The brain
answers with commands: set geometry, mode, visibility, focus, close, border, keyboard,
touchpad, output rules, and so on. Haskell never touches a wlroots struct.

Rules of the boundary:

- **Exceptions never cross back into C.** Every callback goes through `guarded` in
  `Run.hs`, which logs the exception and returns a fallback value.
- **C functions that can call back into Haskell are imported `safe`** (`wh_init`,
  `wh_run`, `wh_shutdown`, `wh_spawn`, `wh_output_rules_apply`); the rest are `unsafe`.
- **Set the mode before the geometry.** The mode (tiled, floating, fullscreen) decides
  the border width that the geometry subtracts.
- **Settings calls work before `wh_init`.** `wh_set_keyboard`, `wh_set_touchpad` and
  the output rules store their values and apply them to devices and outputs as they
  appear, and again to everything present when called later (hot reload).

## Building

```sh
make            # wayland-scanner headers + cabal build --offline, copied to build/
make run        # nested in the current Wayland session, without the autostart
sudo make install
```

Only GHC boot packages are used (`base`, `containers`, `mtl`, `directory`), so the build
needs no network. That's also what `xbps-src` and similar package builders require.
The C side links against wlroots-0.20, wayland-server, xkbcommon, libinput, xcb and
xcb-icccm through `pkgconfig-depends` in `whiplash.cabal`.

`build/whiplash` only rebuilds when a source file is newer than it, so `make && sudo
make install` never runs cabal as root.

Command-line flags, mostly for testing:

- `-s CMD` runs `CMD` once whiplash is up;
- `-n` skips the config's `autostart` (nested and headless runs shouldn't start session
  services).

## Testing without touching your session

Run whiplash headless, drive it with virtual input and capture it with grim:

```sh
XDG_CONFIG_HOME=/tmp/some-dir \
WLR_BACKENDS=headless WLR_HEADLESS_OUTPUTS=1 \
  ./build/whiplash -n -s "alacritty"
# its WAYLAND_DISPLAY is printed in the log ("whiplash running on ...")
WAYLAND_DISPLAY=wayland-N wtype -M alt -k j -m alt   # keys
WAYLAND_DISPLAY=wayland-N wlrctl pointer move 245 31 # pointer
WAYLAND_DISPLAY=wayland-N grim -s 0.5 shot.png       # capture
```

- `WLR_HEADLESS_OUTPUTS=2` gives two 1280x720 screens.
- Point `XDG_CONFIG_HOME` at a temporary directory to test config changes; saving the
  file there exercises hot reload and error reporting.
- wlrctl only clicks (press and release), so drags can't be tested headless; nor can
  touchpads.
- wtype drops or garbles the first characters while it uploads its keymap, and garbles
  text typed into XWayland apps. That's wtype, not whiplash.
- When killing a headless instance, check `/proc/PID/environ` for
  `WLR_BACKENDS=headless` so you don't kill your real session.

## Protocols and features

- **Shells:** xdg-shell (with xdg-decoration, negotiated server side, and tiled edges so
  clients drop their own shadows), wlr-layer-shell (bars, launchers, menus; popups
  included), XWayland (managed and override-redirect windows).
- **Input:** physical and virtual keyboards (`wtype`), virtual pointer (`wlrctl`),
  cursor-shape, libinput touchpad settings, pointer-gestures-v1 (unbound swipes,
  pinches and holds reach clients). Ctrl+Alt+F1..F12 is handled in C before the
  brain sees the key, so a broken config can't trap the user.
- **Workspaces:** ext-workspace-v1, so bars can show and switch them; the urgent state
  is set for workspaces holding a window that asks for attention, through
  xdg-activation-v1 (Wayland) or the X11 urgency hint. Activation never moves focus
  (as in wasp); a window stops being urgent once it is focused. X11
  `_NET_ACTIVE_WINDOW` requests still focus the window, as before.
- **Session:** ext-session-lock-v1 (if the locker dies, the session stays locked;
  while locked only lock surfaces get input), idle notify, idle inhibit, wlr output
  power management (`wlopm`).
- **Outputs:** multiple monitors xmonad-style (each screen shows one workspace;
  `workspace N` swaps screens if N is visible elsewhere), per-output scale and position,
  xdg-output. wlr-output-management (wlr-randr, kanshi, wdisplays): a configuration is
  tested or committed for all outputs at once (`wlr_backend_test`/`commit`), then the
  layout follows. Turning an output off this way takes it out of the layout; wlopm only
  blanks it. The config's `[outputs]` rules are applied again on reload and override
  what a client set. Gamma control (wlsunset, gammastep) is handed to the scene graph
  (`wlr_scene_set_gamma_control_manager_v1`).
- **Capture:** wlr-screencopy and export-dmabuf; `whiplash-portals.conf` points
  xdg-desktop-portal at the wlr backend for screen sharing and the gtk backend for the
  rest.
- **Taken for granted by clients:** viewporter (mpv refuses to start without it),
  fractional scale, single-pixel buffer, primary selection, data control.

## Configuration

`config.toml` sits on top of the defaults compiled into `app/Main.hs`: every key the
file sets replaces the default, anything left out keeps it. A `[keybinds]` table
replaces the whole default binding set, so the file always shows every active binding.
Unknown keys and unknown actions are errors.

The file is watched with inotify on its directory (so editors that save by renaming a
temporary file are seen too) and reloaded on every write. A file that fails to parse
never replaces a working config: the error goes to the log and to `notify-send`.

Touchpad gestures: at `swipe_begin` the heart asks the brain (`wh_on_swipe` with
`WH_SWIPE_QUERY`) whether any `[gestures]` entry uses that many fingers. If so the
swipe is the compositor's: deltas are summed, and at `swipe_end` the dominant axis
picks the direction, ignoring cancelled swipes and ones shorter than `WH_SWIPE_MIN`.
Otherwise the whole swipe, like every pinch and hold, is forwarded to clients through
pointer-gestures-v1. Gestures can't be driven headless; the config parsing and the
`workspace-next`/`workspace-prev` actions can.

Floating windows belong to their workspace, not to a screen. Their rects are kept in
layout coordinates, and every `refresh` starts with `relocateFloats`: a float whose
centre is not on the screen now showing its workspace is moved over from the screen it
is on, keeping its offset from that screen's corner, or centred when it is on no screen
(its monitor went away), and then fitted inside. Dropping a float on another screen
(`floatMoved`, at the end of a drag) moves it to the workspace shown there, as in
xmonad, so it doesn't jump back. Dragging can't be driven headless.

### Occupied workspaces: `d77-workspaces-$WAYLAND_DISPLAY`

ext-workspace-v1 has no "occupied" state, so the d77 compositors (whiplash, wasp,
spitfire) publish it the same way for the d77 shells (Utumno, quickshell-d77,
fabric-d77, helium-d77). The file is `$XDG_RUNTIME_DIR/d77-workspaces-$WAYLAND_DISPLAY`,
with one line per workspace:

```
<ext-workspace id> <number of windows>
```

The id is the one the compositor sends in ext-workspace's `id` event
(`whiplash-<n>` here), so a shell matches each workspace it shows without knowing
output names. The brain rewrites the file only when a count changes, writing beside
it and renaming so readers never see half a file, and removes it on exit. Hidden
scratchpads are not counted. A shell that finds no file keeps its old behaviour.

`WAYLAND_DISPLAY` is set before the backend starts: outputs appear inside
`wlr_backend_start` and the brain already runs then, so it must never see the parent
session's value. SIGTERM and SIGINT stop the event loop, so the shutdown path runs;
they are blocked for the loop's signalfd, and `wh_spawn` unblocks them in children.

Scratchpads live entirely in the brain. `WState.scratch` maps a slot name to its
view. A new window whose app_id matches a slot that has no window yet is caught
(`captureScratchpad`) before the window rules run. Hiding a scratchpad deletes it from
the StackSet and disables it in the heart (`setVisible False`): refresh only touches
windows in the StackSet, so it stays hidden until toggled again, and then it is
re-floated centred on the focused screen and inserted on the current workspace.
`forget` drops a slot whose window unmaps, so the next toggle runs the command again.

Focus-follows-mouse: the heart reports `view_entered` only when real pointer motion
lands on a different view than the last one (`hover_view`), so a window moving under
a still pointer never takes focus, and moving inside the focused window after a
keyboard focus change doesn't steal focus back. Leaving every view (gaps, bars) resets
`hover_view`. The brain focuses on that event only when `focus_follows_mouse` is on.
A focus change made during the callback sets `hover_focus`, which skips the raise, so
hovering never reorders floating windows; clicking still raises.

`[rules."<name>"]` matches a new window's Wayland app_id or X11 `WM_CLASS` class,
ignoring case (`wh_view_app_id` in the heart). `workspace = n` places it with
`StackSet.insertOn`, which leaves focus where it is (xmonad's `doShift`); `float`
overrides the heart's dialog guess either way. Every new window logs its app_id, so
users can find the name to write. Rules apply on every map, so a window an app hides
and shows again (Steam from the tray) is placed again.

Bindings see the unshifted (level-0) keysym, so `Mod+Shift+j` uses a lowercase `j` and
digits stay digits on layouts such as `pt`, where they're shifted symbols.

## Design decisions

- **C heart + Haskell brain** rather than pure Haskell over wlroots: see the top of
  this file.
- **TOML with hot reload**, not an xmonad-style `whiplash.hs` that gets recompiled.
  It's lighter than embedding Lua, matches the author's other window managers, and
  makes the config easy for other tools to edit.
- **No TOML library**: none ships with GHC and the build must stay offline, so
  `Toml.hs` reads keys, tables, strings, numbers, booleans, arrays and inline tables.
  Multi-line strings, dates and `[[arrays of tables]]` are rejected with a clear error.
  Remember that top-level keys such as `autostart` must come before the first `[table]`.
- **xmonad semantics**: new windows take focus above the focused one; `workspace N` is
  greedyView; Mod+drag on a tiled window floats it where it stands.
- **Alt as Mod, Super for the desktop shell**: the default config leaves Super for the
  bar/launcher/locker shell, so it never collides with window management.
- **`dbus-update-activation-environment` runs only in a real session**, never when
  started inside another graphical session: from a nested run it would retarget the
  host session's portals.
- **Touchpad settings only touch devices that can tap**, so a mouse keeps libinput's
  defaults.

## Problems met along the way

- **XWayland crash**: X11 windows map before the brain sizes them, so the border rects
  got negative sizes and wlroots asserted. `view_apply_frame` now skips views that
  haven't been placed yet.
- **Layer-shell popups** get their parent after creation (`get_popup`), so the popup's
  scene node is created on its first commit, and the parent may be a layer surface.
- **Output order flipped on reload**: outputs were kept newest-first while the rules
  re-added them to the layout; they're now kept in creation order.
- **Hidden windows left their borders behind**: an app that hides its window instead
  of closing it (Steam going to the tray) unmaps it, and the brain forgets unmapped
  views, so it never hid their border rects. The heart now disables the whole view on
  unmap; the brain shows it again when it maps.
- **Turning an output back on aborted**: taking an output out of the layout does not
  destroy its scene output, so creating a new one hit wlroots' "two addons of the same
  type" assertion. The old one is destroyed first.
- **The first screen lock locked nothing**: lockers need ext-session-lock-v1, and
  without it the screen silently stayed open.
- **The lock left monitors left of or above 0,0 open**: the lock's opaque backdrop
  was a fixed 16384×16384 rect at 0,0, so an output placed at negative coordinates
  by an output rule kept showing the session, on screen and in screencopy. The
  backdrop now follows the output layout's bounding box, and lock surfaces are
  moved and reconfigured on every layout change (hotplug, mode, scale, position).

## Not done yet

- Animations, rounded corners, blur.
- An "occupied" hint for bars (ext-workspace has no such state).
- An AUR PKGBUILD (the xbps template lives in d77void/srcpkgs-d77).
