<p align="center"><img src="../assets/logo/whiplash-192.png" width="96" alt="whiplash logo"></p>

# whiplash (developer notes)

This is the technical README: architecture, the C/Haskell boundary, how to build and
test, protocol coverage, and why things are the way they are. For installation,
session selection and user configuration, see the [root README](../README.md).

The **C heart** integrates wlroots 0.20 on Wayland and XCB on native X11.
The **Haskell core** owns layout, focus, workspaces, rules and user actions.
The original design kept wlroots calls in C to give Haskell a small interface for
window-management decisions. Native X11 uses the same interface.

## Layout of the source

```
heart/heart.c   Shared C entry points and wlroots support derived from tinywl (CC0).
heart/x11.c     Native X11 backend using XCB. Applies layout, visibility and focus.
heart/x11.h     Interface between the shared C entry points and the XCB backend.
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
data/           whiplash-session (login wrapper), X11 and Wayland session
                entries, whiplash-portals.conf.
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

Command-line flags

- `--backend wayland` starts the wlroots compositor and is the default.
- `--backend x11` manages the X server selected by `DISPLAY` using XCB.
- `-s CMD` runs `CMD` once whiplash is up.
- `-n` skips the config's `autostart` (nested and headless runs shouldn't start session
  services).

## Session startup

- [`data/whiplash.desktop`](../data/whiplash.desktop) installs in
  `/usr/share/wayland-sessions` and calls `whiplash-session`. The default backend
  is Wayland.
- [`data/whiplash-x11.desktop`](../data/whiplash-x11.desktop) installs in
  `/usr/share/xsessions` and calls `whiplash-session --backend x11`.
- [`data/whiplash-session`](../data/whiplash-session) runs the binary under
  `dbus-run-session`, exports `XDG_CURRENT_DESKTOP=whiplash` and rotates the
  current and previous session logs under
  `${XDG_STATE_HOME:-$HOME/.local/state}/whiplash`.

The wrapper enables automatic ATG startup when invoked with exactly
`--backend x11`, `/usr/share/quickshell/atg/shell.qml` is readable, and `qs`
is available. It prepends `/usr/share` to `XDG_CONFIG_DIRS` so Quickshell can
resolve the installed config, defaults `WHIPLASH_X11_RESERVE_TOP` to 48, and appends
`-s 'QT_QPA_PLATFORM=xcb qs -n -d -c atg'`. The wrapper is the place to edit
the ATG startup command. The example X11 profile lists the other services.

[`Run.hs`](../src/Whiplash/Run.hs) selects the backend before loading the config.
On X11 it clears inherited `WAYLAND_DISPLAY` and `WAYLAND_SOCKET` and sets
`XDG_SESSION_TYPE=x11`. After backend initialization it installs bindings, registers
workspaces, applies the initial layout, runs the selected TOML autostart and finally
spawns the `-s` command. `-n` skips the TOML autostart and an explicit `-s`
still runs. Passing extra arguments to the wrapper bypasses its automatic ATG launch.
The Wayland defaults in [`app/Main.hs`](../app/Main.hs) start Utumno from the
autostart hook.

## Native X11 backend

[`heart/x11.c`](../heart/x11.c) claims `SubstructureRedirect` on the root of the
selected `DISPLAY` using a checked request. It reports when another WM owns
the display. The XCB fd uses the shared event loop alongside inotify and shutdown
signals. The X server supplies the keymap. The backend grabs the keys registered by
Haskell and refreshes the grabs on keymap changes and TOML reload.

- **Workspaces and work area.** The C backend maps Haskell workspace tags to
  zero-based EWMH desktops. It publishes the workspace count, names, current desktop
  and work areas. A `_NET_CURRENT_DESKTOP` client message becomes the same workspace
  action that a keybinding invokes. ATG listens for the resulting root-property
  changes. ATG's override-redirect bar uses the top reservation set in the wrapper.
  The reservation affects the usable rectangle for tiling and `_NET_WORKAREA`.
  Fullscreen uses the full rectangle. Both rectangles describe one X screen today.
- **Visibility.** Hiding a client increments its pending-unmap count before issuing
  `UnmapWindow`. The corresponding event consumes that count and preserves the
  client in the `StackSet`. A client withdrawal or destruction invokes the shared
  unmap callback. Shutdown remaps hidden clients and removes the root properties
  published by Whiplash.
- **Floating hints and stacking.** On map, the backend reads `WM_TRANSIENT_FOR`,
  the client size hints, the modal state and `_NET_WM_WINDOW_TYPE`. Transients,
  modal windows, dialogs, utility windows, splash screens, toolbars and windows
  with equal positive minimum and maximum dimensions request floating placement.
  The shared Haskell code centers them and gives an explicit TOML `float` rule
  priority. The X11 backend raises windows within their layer, so floating dialogs
  stay above tiled windows and fullscreen windows stay above both.
- **Click focus.** Unfocused clients have passive grabs for buttons 1–3. A press is
  routed to the shared click callback. `ReplayPointer` then delivers the original
  click to the application. Wheel events and clicks on override-redirect surfaces
  stay with the client.
- **Hover focus.** `focus_follows_mouse` enables X11 crossing handling at startup
  and on reload. The backend checks the root child actually under the pointer before
  calling the shared enter callback. Hover focus skips raising the window.
  Sampling the pointer after a layout change prevents the resulting crossing
  events from taking focus while the pointer is stationary.

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

### X11 proof of concept

The same executable was exercised in a dedicated Xephyr display with two xterms and
as a nested Wayland compositor on Xvfb with two native Alacritty windows. Both runs
used `-n` and the same TOML with two workspaces and only `Tall` enabled.

- On both backends, tested tiling, focus, swapping with the master, switching to an
  empty workspace and back, closing the focused window and reloading a key binding.
- On X11, also tested `-s`, rejection when another WM owns the display, and
  withdrawal and remapping of a client. XTEST drove the bindings. X11 geometry,
  visibility and input focus were queried directly.
- On Wayland, tested the default invocation and startup with `--backend wayland`.
  The default run used wtype for the bindings and grim to check geometry, visibility
  and focus borders. XWayland was unavailable in the test environment.

The initial native Xorg session was also tested through greetd. The ATG integration
was tested on an isolated X server with `wmctrl -d`, `wmctrl -s`, direct clicks
on the bar and keyboard workspace changes. The panel highlight tracked the
active workspace, and normal windows were tiled below the 48-pixel reservation.
Xephyr tests checked first-click delivery to application controls, hover without
raising, hover changes on reload and focus with a stationary pointer during a
layout change. The Wayland tests above were repeated after the X11 focus changes.

On the test X11 display, use `wmctrl -d` to list desktops and
`wmctrl -s 1` to select the second desktop. Use
`xprop -root _NET_CURRENT_DESKTOP _NET_DESKTOP_NAMES _NET_WORKAREA`
to see what the WM publishes to ATG. Set `DISPLAY` to the dedicated Xephyr
display before running those commands.

## Wayland protocols and features

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
  (as in wasp); a window stops being urgent once it is focused. XWayland
  `_NET_ACTIVE_WINDOW` requests still focus the window.
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

`config.toml` sits on top of the defaults compiled into `app/Main.hs`. A top-level
`[keybinds]` table replaces the compiled key set. Add `[x11]` or `[wayland]` for
settings specific to the backend selected at launch:

```toml
[x11]
autostart = ["dunst", "pipewire", "pipewire-pulse"]

[x11.keybinds]
"Super+space" = "spawn qsd77 -c atg launcher"
"Super+a" = false
```

Each profile's `autostart` replaces the common list. An empty array disables
that list for the selected backend. A profile's `keybinds` adds or replaces
individual bindings, and `false` removes a binding. The example config uses ATG
and scrot for X11 and Utumno and grim for Wayland. Unknown keys and actions in
either profile are errors.

The file is watched with inotify on its directory (so editors that save by renaming a
temporary file are seen too) and reloaded on every write. A file that fails to parse
never replaces a working config: the error goes to the log and to `notify-send`.
Autostart still runs once per login. Binding changes take effect on reload.
[`Settings.hs`](../src/Whiplash/Settings.hs) validates both profiles after
applying the shared settings, then selects the configured backend's overrides.
Bindings merge by parsed modifier mask and keysym. `Run.hs` keeps the running
workspace tags when replacing the config, so changing the workspace count needs
a session restart.

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

On Wayland, the heart reports `view_entered` only when real pointer motion
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
- **`dbus-update-activation-environment` runs only when a Wayland session starts
  without an inherited graphical display.** This keeps a nested compositor from
  retargeting the host session's portals.
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
- Native X11 per-monitor work areas, mouse dragging, client fullscreen requests,
  and occupied and urgent hints for the ATG bar.
- D77 ISO integration for the two sessions.
