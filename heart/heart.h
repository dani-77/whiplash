/*
 * whiplash heart: the C side of whiplash.
 *
 * The heart owns everything wlroots: outputs, input devices, the scene graph
 * and the xdg-shell protocol. It makes no window-management decisions. It
 * reports events to the brain (Haskell) through callbacks and carries out the
 * brain's commands through the wh_* functions below.
 *
 * Views are identified by small integers, never by pointers, so the brain
 * never touches a wlroots struct.
 */
#ifndef WHIPLASH_HEART_H
#define WHIPLASH_HEART_H

#include <stdint.h>

typedef uint32_t wh_view_id;
typedef void (*wh_void_cb)(void);

/* Callbacks into the brain. */
typedef void (*wh_view_cb)(wh_view_id id);
/* Return non-zero when the key was consumed as a compositor binding. */
typedef int (*wh_key_cb)(uint32_t mods, uint32_t keysym);
typedef void (*wh_view_state_cb)(wh_view_id id, int on);
typedef void (*wh_view_rect_cb)(wh_view_id id, int x, int y, int width, int height);
typedef void (*wh_workspace_cb)(int tag);

enum wh_view_mode {
	WH_VIEW_TILED = 0,
	WH_VIEW_FLOATING = 1,
	WH_VIEW_FULLSCREEN = 2,
};

void wh_on_view_mapped(wh_view_cb cb);
void wh_on_view_unmapped(wh_view_cb cb);
void wh_on_view_clicked(wh_view_cb cb);
/* The pointer moved onto another view (not when a view moves under a still
 * pointer). A focus change made from this callback does not raise. */
void wh_on_view_entered(wh_view_cb cb);
void wh_on_key(wh_key_cb cb);
/* Outputs appeared, went away, or changed size, scale or bars: query
 * them with wh_output_count/wh_output_info. */
void wh_on_outputs_changed(wh_void_cb cb);
/* A client asked to enter (on=1) or leave (on=0) fullscreen. */
void wh_on_view_fullscreen(wh_view_state_cb cb);
/* mouse_mod + drag started on a view: float it now if it should move. */
void wh_on_view_grab(wh_view_rect_cb cb);
/* A drag ended; this is the view's new outer rectangle. */
void wh_on_view_moved(wh_view_rect_cb cb);
/* A bar asked to switch to this workspace (ext-workspace-v1). */
void wh_on_workspace_activate(wh_workspace_cb cb);
/* The watched config file was written (see wh_watch_config). */
void wh_on_config_changed(wh_void_cb cb);

/* Lifecycle. wh_init returns 0 on success. */
int wh_init(void);
const char *wh_socket(void);
void wh_run(void);
void wh_quit(void);
void wh_shutdown(void);

/* Commands from the brain. */
void wh_view_set_geometry(wh_view_id id, int x, int y, int width, int height);
void wh_view_set_visible(wh_view_id id, int visible);
void wh_view_focus(wh_view_id id); /* 0 clears keyboard focus */
void wh_view_close(wh_view_id id);
void wh_view_set_mode(wh_view_id id, int mode); /* enum wh_view_mode */
/* The app_id of a Wayland window or the WM_CLASS class of an X11 one; ""
 * when it has none. Valid until the next call into the heart. */
const char *wh_view_app_id(wh_view_id id);
int wh_view_wants_float(wh_view_id id);
int wh_view_natural_width(wh_view_id id);
int wh_view_natural_height(wh_view_id id);
void wh_set_mouse_mod(uint32_t mods);
/* Announce a workspace to bars (after wh_init), and which one is shown. */
void wh_workspace_add(int tag, const char *name);
void wh_workspace_set_active(int tag);
void wh_set_border(int width, uint32_t focused_rgba, uint32_t normal_rgba);
void wh_spawn(const char *command);
/* Screens, left to right then top to bottom. wh_output_info fills
 * out[8] with the full box (x, y, w, h) and the usable box after bars. */
int wh_output_count(void);
int wh_output_info(int index, int *out);
const char *wh_output_name(int index);
/* Per-output scale and position by connector name (e.g. "eDP-1"). Rules
 * may be set before wh_init; wh_output_rules_apply re-applies them. */
void wh_output_rules_clear(void);
void wh_output_rule_add(const char *name, float scale, int x, int y, int has_position);
void wh_output_rules_apply(void);
/* Keyboard settings for physical keyboards; empty strings mean xkb
 * defaults, non-positive repeat values mean 25 Hz / 600 ms. Can be called
 * before wh_init and again at any time. */
void wh_set_keyboard(const char *layout, const char *variant, const char *options,
	int repeat_rate, int repeat_delay);
/* Touchpad settings (devices that can tap; mice are left alone). Flags
 * are 0/1, accel_speed is -1..1. Defaults: tap, tap-and-drag, drag lock
 * and disable-while-typing on, the rest libinput's. Can be called before
 * wh_init and again at any time. */
enum wh_accel_profile { WH_ACCEL_DEFAULT, WH_ACCEL_ADAPTIVE, WH_ACCEL_FLAT };
enum wh_click_method { WH_CLICK_DEFAULT, WH_CLICK_BUTTON_AREAS, WH_CLICK_CLICKFINGER };
void wh_set_touchpad(int tap, int tap_drag, int drag_lock, int natural_scroll,
	int disable_while_typing, double accel_speed, int accel_profile,
	int click_method);
/* XKB_KEY_NoSymbol (0) when the name is unknown. */
uint32_t wh_keysym_from_name(const char *name);
/* Watch dir/name for writes (after wh_init); 0 on success. */
int wh_watch_config(const char *dir, const char *name);

#endif
