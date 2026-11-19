/*
 * whiplash heart: wlroots plumbing for the Haskell brain.
 *
 * Derived from tinywl (wlroots 0.20.2), which is dedicated to the public
 * domain under CC0. Interactive move/resize and the built-in keybindings were
 * removed: every window-management decision belongs to the brain.
 */
#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <math.h>
#include <signal.h>
#include <stdbool.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <linux/input-event-codes.h>
#include <sys/wait.h>
#include <sys/inotify.h>
#include <fcntl.h>
#include <wayland-server-core.h>
#include <libinput.h>
#include <wlr/backend.h>
#include <wlr/backend/libinput.h>
#include <wlr/backend/session.h>
#include <wlr/render/allocator.h>
#include <wlr/render/wlr_renderer.h>
#include <wlr/types/wlr_cursor.h>
#include <wlr/types/wlr_cursor_shape_v1.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_data_control_v1.h>
#include <wlr/types/wlr_data_device.h>
#include <wlr/types/wlr_ext_data_control_v1.h>
#include <wlr/types/wlr_export_dmabuf_v1.h>
#include <wlr/types/wlr_ext_workspace_v1.h>
#include <wlr/types/wlr_fractional_scale_v1.h>
#include <wlr/types/wlr_gamma_control_v1.h>
#include <wlr/types/wlr_idle_inhibit_v1.h>
#include <wlr/types/wlr_idle_notify_v1.h>
#include <wlr/types/wlr_input_device.h>
#include <wlr/types/wlr_keyboard.h>
#include <wlr/types/wlr_layer_shell_v1.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/types/wlr_output_management_v1.h>
#include <wlr/types/wlr_output_power_management_v1.h>
#include <wlr/types/wlr_pointer.h>
#include <wlr/types/wlr_pointer_gestures_v1.h>
#include <wlr/types/wlr_primary_selection.h>
#include <wlr/types/wlr_primary_selection_v1.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_screencopy_v1.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/types/wlr_single_pixel_buffer_v1.h>
#include <wlr/types/wlr_session_lock_v1.h>
#include <wlr/types/wlr_subcompositor.h>
#include <wlr/types/wlr_xcursor_manager.h>
#include <wlr/types/wlr_viewporter.h>
#include <wlr/types/wlr_virtual_keyboard_v1.h>
#include <wlr/types/wlr_virtual_pointer_v1.h>
#include <wlr/types/wlr_xdg_activation_v1.h>
#include <wlr/types/wlr_xdg_decoration_v1.h>
#include <wlr/types/wlr_xdg_output_v1.h>
#include <wlr/types/wlr_xdg_shell.h>
#include <wlr/util/edges.h>
#include <wlr/util/log.h>
#include <wlr/xwayland/xwayland.h>
#include <wlr/xcursor.h>
#include <xkbcommon/xkbcommon.h>

#include "heart.h"

/* Scene stacking order, bottom to top. Tiled windows live between the
 * layer-shell "bottom" and "top" layers, so bars draw above them. */
enum wh_scene_layer {
	WH_LAYER_BACKGROUND,
	WH_LAYER_BOTTOM,
	WH_LAYER_TILE,
	WH_LAYER_FLOAT,
	WH_LAYER_UNMANAGED, /* X11 menus and tooltips (override-redirect) */
	WH_LAYER_TOP,
	WH_LAYER_FULLSCREEN, /* above bars, below lock screens */
	WH_LAYER_OVERLAY,
	WH_LAYER_LOCK, /* ext-session-lock surfaces, above everything */
	WH_LAYER_COUNT,
};

struct wh_server {
	struct wl_display *wl_display;
	struct wlr_backend *backend;
	struct wlr_session *session; /* NULL when nested */
	struct wlr_renderer *renderer;
	struct wlr_allocator *allocator;
	struct wlr_scene *scene;
	struct wlr_scene_output_layout *scene_layout;
	struct wlr_scene_tree *layers[WH_LAYER_COUNT];

	struct wlr_layer_shell_v1 *layer_shell;
	struct wl_listener new_layer_surface;
	struct wl_list layer_surfaces;
	/* Layer surface holding keyboard focus (launcher, lock screen, ...). */
	struct wlr_layer_surface_v1 *focused_layer;
	/* Last view the brain focused, restored when a layer lets go. */
	wh_view_id focused_view;
	/* The view the pointer last moved onto (0: none), and whether a focus
	 * change is coming from that move (hover focus never raises). */
	wh_view_id hover_view;
	bool hover_focus;
	struct wl_list output_rules; /* struct wh_output_rule.link */
	struct wlr_output *workspace_group_output;

	struct wlr_xdg_shell *xdg_shell;
	struct wl_listener new_xdg_toplevel;

	struct wlr_compositor *compositor;
	struct wlr_xwayland *xwayland; /* NULL if XWayland could not start */
	/* Wayland windows asking for attention (xdg-activation-v1). */
	struct wlr_xdg_activation_v1 *activation;
	struct wl_listener request_activation;
	struct wl_listener xwayland_ready;
	struct wl_listener xwayland_new_surface;
	struct wl_listener new_xdg_popup;
	struct wl_list views;

	struct wlr_xdg_decoration_manager_v1 *decoration_mgr;
	struct wl_listener new_decoration;

	/* ext-workspace-v1: lets bars show and switch the brain's workspaces. */
	struct wlr_ext_workspace_manager_v1 *workspace_mgr;
	struct wlr_ext_workspace_group_handle_v1 *workspace_group;
	struct wl_listener workspace_commit;
	struct wl_list workspaces; /* struct wh_workspace.link */

	/* ext-session-lock-v1. While locked only lock surfaces get input and
	 * an opaque rect hides everything else, even if the locker dies. */
	struct wlr_session_lock_manager_v1 *lock_mgr;
	struct wl_listener new_lock;
	struct wlr_session_lock_v1 *lock; /* NULL once its client is gone */
	bool locked;
	struct wlr_scene_rect *lock_bg;
	struct wl_listener lock_new_surface;
	struct wl_listener lock_unlock;
	struct wl_listener lock_destroy;

	/* Idle: swayidle-style clients (ext-idle-notify), inhibitors such as
	 * video players (idle-inhibit), and wlopm (output power management). */
	struct wlr_idle_notifier_v1 *idle_notifier;
	struct wlr_idle_inhibit_manager_v1 *idle_inhibit_mgr;
	struct wl_listener new_idle_inhibitor;
	struct wlr_output_power_manager_v1 *output_power_mgr;
	struct wl_listener output_power_set_mode;

	struct wlr_cursor *cursor;
	struct wlr_xcursor_manager *cursor_mgr;
	struct wl_listener cursor_motion;
	struct wl_listener cursor_motion_absolute;
	struct wl_listener cursor_button;
	struct wl_listener cursor_axis;
	struct wl_listener cursor_frame;
	/* Touchpad gestures: swipes the brain binds are ours, everything else
	 * (pinch zoom, holds, unbound swipes) goes to clients. */
	struct wlr_pointer_gestures_v1 *pointer_gestures;
	struct wl_listener swipe_begin, swipe_update, swipe_end;
	struct wl_listener pinch_begin, pinch_update, pinch_end;
	struct wl_listener hold_begin, hold_end;
	bool swipe_ours;
	uint32_t swipe_fingers;
	double swipe_dx, swipe_dy;

	/* Interactive move/resize of a view with mouse_mod + drag. */
	uint32_t mouse_mod;
	struct wh_view *grab_view;
	enum { WH_GRAB_NONE, WH_GRAB_MOVE, WH_GRAB_RESIZE } grab_mode;
	double grab_cursor_x, grab_cursor_y;
	int grab_x, grab_y, grab_w, grab_h;

	struct wlr_seat *seat;
	struct wl_listener new_input;
	struct wl_listener request_cursor;
	struct wl_listener pointer_focus_change;
	struct wl_listener request_set_selection;
	struct wl_listener request_set_primary_selection;
	struct wlr_cursor_shape_manager_v1 *cursor_shape_mgr;
	struct wl_listener request_set_cursor_shape;
	struct wl_list keyboards;
	struct wl_list touchpads; /* struct wh_touchpad_dev, for hot reload */
	struct wlr_virtual_keyboard_manager_v1 *virtual_keyboard_mgr;
	struct wl_listener new_virtual_keyboard;
	struct wlr_virtual_pointer_manager_v1 *virtual_pointer_mgr;
	struct wl_listener new_virtual_pointer;

	struct wlr_output_layout *output_layout;
	struct wl_list outputs;
	struct wl_listener new_output;
	struct wl_listener layout_change;
	/* wlr-output-management (wlr-randr, kanshi, wdisplays). */
	struct wlr_output_manager_v1 *output_mgr;
	struct wl_listener output_mgr_apply;
	struct wl_listener output_mgr_test;

	const char *socket;
	/* Keyboard settings for physical keyboards (wh_set_keyboard). */
	char *xkb_layout, *xkb_variant, *xkb_options;
	int repeat_rate, repeat_delay;

	/* Config file watch (inotify on its directory, so editors that save
	 * by renaming a temp file are seen too). */
	int config_watch_fd;
	char *config_name;
	struct wl_event_source *config_watch_source;
	wh_view_id next_id;

	int border_width;
	float border_focused[4];
	float border_normal[4];
};

struct wh_output {
	struct wl_list link;
	struct wh_server *server;
	struct wlr_output *wlr_output;
	/* Layout box, and what is left of it after bars (exclusive zones). */
	struct wlr_box full, usable;
	/* Turned off through output management: out of the layout until a
	 * client turns it back on. (wlopm only blanks it and keeps it.) */
	bool disabled;
	struct wl_listener frame;
	struct wl_listener request_state;
	struct wl_listener destroy;
};

struct wh_view {
	struct wl_list link;
	struct wh_server *server;
	wh_view_id id;
	bool mapped;
	/* Exactly one of these is set: a Wayland (xdg) or an X11 window. */
	struct wlr_xdg_toplevel *xdg_toplevel;
	struct wlr_xwayland_surface *xsurface;
	bool associated; /* X11: map/unmap listeners are attached */
	/* container holds the four border rects and the surface tree */
	struct wlr_scene_tree *container;
	struct wlr_scene_tree *surface_tree;
	struct wlr_scene_rect *border[4];
	int x, y, width, height;
	enum wh_view_mode mode;
	struct wl_listener map;
	struct wl_listener unmap;
	struct wl_listener commit;
	struct wl_listener destroy;
	struct wl_listener request_maximize;
	struct wl_listener request_fullscreen;
	/* X11 only */
	struct wl_listener associate;
	struct wl_listener dissociate;
	struct wl_listener request_configure;
	struct wl_listener request_activate;
	struct wl_listener set_hints; /* X11: urgency hint */
};

/* An override-redirect X11 window (menu, tooltip, dropdown): placed where
 * it asks, never managed or tiled. */
struct wh_unmanaged {
	struct wlr_xwayland_surface *xsurface;
	struct wlr_scene_tree *tree;
	bool associated;
	struct wl_listener associate;
	struct wl_listener dissociate;
	struct wl_listener map;
	struct wl_listener unmap;
	struct wl_listener destroy;
	struct wl_listener set_geometry;
};

/* Clients that support xdg-decoration are told to leave decorations to us:
 * a tiling compositor draws only its borders. */
struct wh_decoration {
	struct wlr_xdg_toplevel_decoration_v1 *decoration;
	struct wl_listener request_mode;
	struct wl_listener commit;
	struct wl_listener destroy;
};

struct wh_layer {
	struct wl_list link;
	struct wlr_layer_surface_v1 *layer_surface;
	struct wlr_scene_layer_surface_v1 *scene;
	struct wl_listener map;
	struct wl_listener unmap;
	struct wl_listener commit;
	struct wl_listener destroy;
};

struct wh_workspace {
	struct wl_list link;
	int tag;
	struct wlr_ext_workspace_handle_v1 *handle;
};

struct wh_lock_surface {
	struct wlr_session_lock_surface_v1 *lock_surface;
	struct wl_listener map;
	struct wl_listener destroy;
};

struct wh_idle_inhibitor {
	struct wlr_idle_inhibitor_v1 *inhibitor;
	struct wl_listener destroy;
};

/* Per-output settings from the config, matched by connector name. */
struct wh_output_rule {
	struct wl_list link;
	char *name;
	float scale; /* <= 0: leave as is */
	int x, y;
	bool has_position;
};

struct wh_popup {
	struct wlr_xdg_popup *xdg_popup;
	struct wl_listener commit;
	struct wl_listener destroy;
};

/* Touchpad settings (wh_set_touchpad). Defaults match wasp, so a
 * brain that never calls wh_set_touchpad still gets tap-to-click. */
static struct {
	int tap, tap_drag, drag_lock, natural_scroll, dwt;
	double accel_speed;
	int accel_profile, click_method;
} touchpad_cfg = { 1, 1, 1, 0, 1, 0.0, WH_ACCEL_DEFAULT, WH_CLICK_DEFAULT };

struct wh_touchpad_dev {
	struct wl_list link;
	struct libinput_device *device;
	struct wl_listener destroy;
};

struct wh_keyboard {
	struct wl_list link;
	struct wh_server *server;
	struct wlr_keyboard *wlr_keyboard;
	bool is_virtual;
	struct wl_listener modifiers;
	struct wl_listener key;
	struct wl_listener destroy;
};

static struct wh_server server;

static wh_view_cb cb_view_mapped;
static wh_view_cb cb_view_unmapped;
static wh_view_cb cb_view_clicked;
static wh_view_cb cb_view_entered;
static wh_swipe_cb cb_swipe;
static wh_key_cb cb_key;
static wh_void_cb cb_outputs_changed;
static wh_view_state_cb cb_view_fullscreen;
static wh_view_state_cb cb_view_urgent;
static wh_view_rect_cb cb_view_grab;
static wh_view_rect_cb cb_view_moved;
static wh_workspace_cb cb_workspace_activate;
static wh_void_cb cb_config_changed;

void wh_on_view_mapped(wh_view_cb cb) { cb_view_mapped = cb; }
void wh_on_view_unmapped(wh_view_cb cb) { cb_view_unmapped = cb; }
void wh_on_view_clicked(wh_view_cb cb) { cb_view_clicked = cb; }
void wh_on_view_entered(wh_view_cb cb) { cb_view_entered = cb; }
void wh_on_swipe(wh_swipe_cb cb) { cb_swipe = cb; }
void wh_on_key(wh_key_cb cb) { cb_key = cb; }
void wh_on_outputs_changed(wh_void_cb cb) { cb_outputs_changed = cb; }
void wh_on_view_fullscreen(wh_view_state_cb cb) { cb_view_fullscreen = cb; }
void wh_on_view_urgent(wh_view_state_cb cb) { cb_view_urgent = cb; }
void wh_on_view_grab(wh_view_rect_cb cb) { cb_view_grab = cb; }
void wh_on_view_moved(wh_view_rect_cb cb) { cb_view_moved = cb; }
void wh_on_workspace_activate(wh_workspace_cb cb) { cb_workspace_activate = cb; }
void wh_on_config_changed(wh_void_cb cb) { cb_config_changed = cb; }

static void view_place(struct wh_view *view, int x, int y, int width, int height);
static void ensure_workspace_group(void);

static int view_border(struct wh_view *view) {
	return view->mode == WH_VIEW_FULLSCREEN ? 0 : server.border_width;
}

static struct wh_view *view_by_id(wh_view_id id) {
	struct wh_view *view;
	wl_list_for_each(view, &server.views, link) {
		if (view->id == id) {
			return view;
		}
	}
	return NULL;
}

static void rgba_to_floats(uint32_t rgba, float out[4]) {
	out[0] = ((rgba >> 24) & 0xff) / 255.0f;
	out[1] = ((rgba >> 16) & 0xff) / 255.0f;
	out[2] = ((rgba >> 8) & 0xff) / 255.0f;
	out[3] = (rgba & 0xff) / 255.0f;
}

static void view_paint_border(struct wh_view *view, bool focused) {
	const float *color = focused ? server.border_focused : server.border_normal;
	for (int i = 0; i < 4; i++) {
		wlr_scene_rect_set_color(view->border[i], color);
	}
}

/* Lay the border rects and the surface out inside a width x height frame. */
static void view_apply_frame(struct wh_view *view) {
	int bw = view_border(view);
	int w = view->width, h = view->height;
	if (w < 2 * bw || h < 2 * bw) {
		/* Not placed yet (an X11 window maps before the brain sizes it). */
		for (int i = 0; i < 4; i++) {
			wlr_scene_rect_set_size(view->border[i], 0, 0);
		}
		if (view->surface_tree) {
			wlr_scene_node_set_position(&view->surface_tree->node, bw, bw);
		}
		return;
	}
	wlr_scene_rect_set_size(view->border[0], w, bw);           /* top */
	wlr_scene_node_set_position(&view->border[0]->node, 0, 0);
	wlr_scene_rect_set_size(view->border[1], w, bw);           /* bottom */
	wlr_scene_node_set_position(&view->border[1]->node, 0, h - bw);
	wlr_scene_rect_set_size(view->border[2], bw, h - 2 * bw);  /* left */
	wlr_scene_node_set_position(&view->border[2]->node, 0, bw);
	wlr_scene_rect_set_size(view->border[3], bw, h - 2 * bw);  /* right */
	wlr_scene_node_set_position(&view->border[3]->node, w - bw, bw);
	if (view->surface_tree) {
		wlr_scene_node_set_position(&view->surface_tree->node, bw, bw);
	}
}

/* ---- per-kind view helpers (xdg or X11) --------------------------------- */

static struct wlr_surface *view_surface(struct wh_view *view) {
	return view->xsurface ? view->xsurface->surface : view->xdg_toplevel->base->surface;
}

static void view_set_activated(struct wh_view *view, bool on) {
	if (view->xsurface) {
		wlr_xwayland_surface_activate(view->xsurface, on);
		if (on) {
			wlr_xwayland_surface_restack(view->xsurface, NULL, XCB_STACK_MODE_ABOVE);
		}
	} else {
		wlr_xdg_toplevel_set_activated(view->xdg_toplevel, on);
	}
}

static struct wh_view *view_from_surface(struct wlr_surface *surface) {
	struct wh_view *view;
	wl_list_for_each(view, &server.views, link) {
		if (view->mapped && view_surface(view) == surface) {
			return view;
		}
	}
	return NULL;
}

/* Any real input counts as activity for idle clients (swayidle). */
static void notify_activity(void) {
	if (server.idle_notifier) {
		wlr_idle_notifier_v1_notify_activity(server.idle_notifier, server.seat);
	}
}

/* ---- keyboard ----------------------------------------------------------- */

static void keyboard_handle_modifiers(struct wl_listener *listener, void *data) {
	struct wh_keyboard *keyboard = wl_container_of(listener, keyboard, modifiers);
	wlr_seat_set_keyboard(keyboard->server->seat, keyboard->wlr_keyboard);
	wlr_seat_keyboard_notify_modifiers(keyboard->server->seat,
		&keyboard->wlr_keyboard->modifiers);
}

static void keyboard_handle_key(struct wl_listener *listener, void *data) {
	struct wh_keyboard *keyboard = wl_container_of(listener, keyboard, key);
	struct wlr_keyboard_key_event *event = data;
	struct wlr_seat *seat = keyboard->server->seat;

	uint32_t keycode = event->keycode + 8;
	bool handled = false;
	notify_activity();

	/* Ctrl+Alt+Fn always switches VT, whatever the brain does: it is the
	 * escape hatch when something goes wrong. The keymap turns Ctrl+Alt+Fn
	 * into XF86Switch_VT_n at the active level, so check those syms. */
	if (event->state == WL_KEYBOARD_KEY_STATE_PRESSED && server.session) {
		const xkb_keysym_t *active;
		int nactive = xkb_state_key_get_syms(keyboard->wlr_keyboard->xkb_state,
			keycode, &active);
		for (int i = 0; i < nactive; i++) {
			if (active[i] >= XKB_KEY_XF86Switch_VT_1 &&
					active[i] <= XKB_KEY_XF86Switch_VT_12) {
				wlr_session_change_vt(server.session,
					active[i] - XKB_KEY_XF86Switch_VT_1 + 1);
				return;
			}
		}
	}

	if (event->state == WL_KEYBOARD_KEY_STATE_PRESSED && cb_key && !server.locked) {
		/* Bindings see the unshifted (level 0) keysym, so "Mod+Shift+j" is
		 * written with a lowercase j and number keys stay numbers on every
		 * layout. Lock modifiers (Caps, Num) are ignored. */
		struct xkb_state *state = keyboard->wlr_keyboard->xkb_state;
		struct xkb_keymap *keymap = keyboard->wlr_keyboard->keymap;
		xkb_layout_index_t layout = xkb_state_key_get_layout(state, keycode);
		const xkb_keysym_t *syms;
		int nsyms = xkb_keymap_key_get_syms_by_level(keymap, keycode,
			layout, 0, &syms);
		uint32_t mods = wlr_keyboard_get_modifiers(keyboard->wlr_keyboard) &
			~(WLR_MODIFIER_CAPS | WLR_MODIFIER_MOD2);
		for (int i = 0; i < nsyms && !handled; i++) {
			handled = cb_key(mods, syms[i]) != 0;
		}
	}

	if (!handled) {
		wlr_seat_set_keyboard(seat, keyboard->wlr_keyboard);
		wlr_seat_keyboard_notify_key(seat, event->time_msec,
			event->keycode, event->state);
	}
}

static void keyboard_handle_destroy(struct wl_listener *listener, void *data) {
	struct wh_keyboard *keyboard = wl_container_of(listener, keyboard, destroy);
	wl_list_remove(&keyboard->modifiers.link);
	wl_list_remove(&keyboard->key.link);
	wl_list_remove(&keyboard->destroy.link);
	wl_list_remove(&keyboard->link);
	free(keyboard);
}

static void keyboard_apply_settings(struct wlr_keyboard *wlr_keyboard) {
	struct xkb_rule_names names = {
		.layout = server.xkb_layout,
		.variant = server.xkb_variant,
		.options = server.xkb_options,
	};
	struct xkb_context *context = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
	struct xkb_keymap *keymap = xkb_keymap_new_from_names(context, &names,
		XKB_KEYMAP_COMPILE_NO_FLAGS);
	if (keymap == NULL) {
		wlr_log(WLR_ERROR, "invalid xkb settings (layout '%s'), using defaults",
			server.xkb_layout ? server.xkb_layout : "(none)");
		keymap = xkb_keymap_new_from_names(context, NULL,
			XKB_KEYMAP_COMPILE_NO_FLAGS);
	}
	wlr_keyboard_set_keymap(wlr_keyboard, keymap);
	xkb_keymap_unref(keymap);
	xkb_context_unref(context);
	wlr_keyboard_set_repeat_info(wlr_keyboard,
		server.repeat_rate > 0 ? server.repeat_rate : 25,
		server.repeat_delay > 0 ? server.repeat_delay : 600);
}

static char *dup_or_null(const char *s) {
	return s && *s ? strdup(s) : NULL;
}

void wh_set_keyboard(const char *layout, const char *variant, const char *options,
		int repeat_rate, int repeat_delay) {
	free(server.xkb_layout);
	free(server.xkb_variant);
	free(server.xkb_options);
	server.xkb_layout = dup_or_null(layout);
	server.xkb_variant = dup_or_null(variant);
	server.xkb_options = dup_or_null(options);
	server.repeat_rate = repeat_rate;
	server.repeat_delay = repeat_delay;
	if (server.keyboards.next == NULL) {
		return; /* before wh_init: applied as keyboards appear */
	}
	struct wh_keyboard *keyboard;
	wl_list_for_each(keyboard, &server.keyboards, link) {
		if (!keyboard->is_virtual) {
			keyboard_apply_settings(keyboard->wlr_keyboard);
		}
	}
}

uint32_t wh_keysym_from_name(const char *name) {
	xkb_keysym_t sym = xkb_keysym_from_name(name, XKB_KEYSYM_NO_FLAGS);
	if (sym == XKB_KEY_NoSymbol) {
		sym = xkb_keysym_from_name(name, XKB_KEYSYM_CASE_INSENSITIVE);
	}
	return sym;
}

/* Virtual keyboards (wtype, on-screen keyboards) bring their own keymap and
 * repeat settings, so only physical ones get ours. */
static void server_new_keyboard(struct wlr_input_device *device, bool is_virtual) {
	struct wlr_keyboard *wlr_keyboard = wlr_keyboard_from_input_device(device);

	struct wh_keyboard *keyboard = calloc(1, sizeof(*keyboard));
	keyboard->server = &server;
	keyboard->wlr_keyboard = wlr_keyboard;

	keyboard->is_virtual = is_virtual;
	if (!is_virtual) {
		keyboard_apply_settings(wlr_keyboard);
	}

	keyboard->modifiers.notify = keyboard_handle_modifiers;
	wl_signal_add(&wlr_keyboard->events.modifiers, &keyboard->modifiers);
	keyboard->key.notify = keyboard_handle_key;
	wl_signal_add(&wlr_keyboard->events.key, &keyboard->key);
	keyboard->destroy.notify = keyboard_handle_destroy;
	wl_signal_add(&device->events.destroy, &keyboard->destroy);

	wlr_seat_set_keyboard(server.seat, keyboard->wlr_keyboard);
	wl_list_insert(&server.keyboards, &keyboard->link);
}

/* libinput leaves tapping off on touchpads; the brain's [touchpad]
 * settings (or the wasp defaults above) turn it on. */
static void touchpad_apply(struct libinput_device *dev) {
	if (libinput_device_config_tap_get_finger_count(dev) > 0) {
		libinput_device_config_tap_set_enabled(dev, touchpad_cfg.tap
			? LIBINPUT_CONFIG_TAP_ENABLED : LIBINPUT_CONFIG_TAP_DISABLED);
		libinput_device_config_tap_set_button_map(dev, LIBINPUT_CONFIG_TAP_MAP_LRM);
		libinput_device_config_tap_set_drag_enabled(dev, touchpad_cfg.tap_drag
			? LIBINPUT_CONFIG_DRAG_ENABLED : LIBINPUT_CONFIG_DRAG_DISABLED);
		libinput_device_config_tap_set_drag_lock_enabled(dev, touchpad_cfg.drag_lock
			? LIBINPUT_CONFIG_DRAG_LOCK_ENABLED : LIBINPUT_CONFIG_DRAG_LOCK_DISABLED);
	}
	if (libinput_device_config_scroll_has_natural_scroll(dev)) {
		libinput_device_config_scroll_set_natural_scroll_enabled(dev,
			touchpad_cfg.natural_scroll);
	}
	if (libinput_device_config_dwt_is_available(dev)) {
		libinput_device_config_dwt_set_enabled(dev, touchpad_cfg.dwt
			? LIBINPUT_CONFIG_DWT_ENABLED : LIBINPUT_CONFIG_DWT_DISABLED);
	}
	if (libinput_device_config_accel_is_available(dev)) {
		libinput_device_config_accel_set_speed(dev, touchpad_cfg.accel_speed);
		enum libinput_config_accel_profile profile =
			touchpad_cfg.accel_profile == WH_ACCEL_FLAT
				? LIBINPUT_CONFIG_ACCEL_PROFILE_FLAT
			: touchpad_cfg.accel_profile == WH_ACCEL_ADAPTIVE
				? LIBINPUT_CONFIG_ACCEL_PROFILE_ADAPTIVE
				: libinput_device_config_accel_get_default_profile(dev);
		if (libinput_device_config_accel_get_profiles(dev) & profile) {
			libinput_device_config_accel_set_profile(dev, profile);
		}
	}
	uint32_t methods = libinput_device_config_click_get_methods(dev);
	enum libinput_config_click_method method =
		touchpad_cfg.click_method == WH_CLICK_BUTTON_AREAS
			? LIBINPUT_CONFIG_CLICK_METHOD_BUTTON_AREAS
		: touchpad_cfg.click_method == WH_CLICK_CLICKFINGER
			? LIBINPUT_CONFIG_CLICK_METHOD_CLICKFINGER
			: libinput_device_config_click_get_default_method(dev);
	if (method == LIBINPUT_CONFIG_CLICK_METHOD_NONE || (methods & method)) {
		libinput_device_config_click_set_method(dev, method);
	}
}

static void touchpad_handle_destroy(struct wl_listener *listener, void *data) {
	struct wh_touchpad_dev *tp = wl_container_of(listener, tp, destroy);
	wl_list_remove(&tp->destroy.link);
	wl_list_remove(&tp->link);
	free(tp);
}

/* Only touchpads (devices that can tap) are configured; mice keep the
 * libinput defaults. */
static void configure_pointer(struct wlr_input_device *device) {
	if (!wlr_input_device_is_libinput(device)) {
		return;
	}
	struct libinput_device *dev = wlr_libinput_get_device_handle(device);
	if (!dev || libinput_device_config_tap_get_finger_count(dev) == 0) {
		return;
	}
	struct wh_touchpad_dev *tp = calloc(1, sizeof(*tp));
	tp->device = dev;
	tp->destroy.notify = touchpad_handle_destroy;
	wl_signal_add(&device->events.destroy, &tp->destroy);
	wl_list_insert(&server.touchpads, &tp->link);
	touchpad_apply(dev);
}

void wh_set_touchpad(int tap, int tap_drag, int drag_lock, int natural_scroll,
		int disable_while_typing, double accel_speed, int accel_profile,
		int click_method) {
	touchpad_cfg.tap = tap;
	touchpad_cfg.tap_drag = tap_drag;
	touchpad_cfg.drag_lock = drag_lock;
	touchpad_cfg.natural_scroll = natural_scroll;
	touchpad_cfg.dwt = disable_while_typing;
	touchpad_cfg.accel_speed = accel_speed < -1 ? -1 : accel_speed > 1 ? 1 : accel_speed;
	touchpad_cfg.accel_profile = accel_profile;
	touchpad_cfg.click_method = click_method;
	if (server.touchpads.next == NULL) {
		return; /* before wh_init: applied as touchpads appear */
	}
	struct wh_touchpad_dev *tp;
	wl_list_for_each(tp, &server.touchpads, link) {
		touchpad_apply(tp->device);
	}
}

static void server_new_input(struct wl_listener *listener, void *data) {
	struct wlr_input_device *device = data;
	switch (device->type) {
	case WLR_INPUT_DEVICE_KEYBOARD:
		server_new_keyboard(device, false);
		break;
	case WLR_INPUT_DEVICE_POINTER:
		configure_pointer(device);
		wlr_cursor_attach_input_device(server.cursor, device);
		break;
	default:
		break;
	}
	uint32_t caps = WL_SEAT_CAPABILITY_POINTER;
	if (!wl_list_empty(&server.keyboards)) {
		caps |= WL_SEAT_CAPABILITY_KEYBOARD;
	}
	wlr_seat_set_capabilities(server.seat, caps);
}

static void server_new_virtual_keyboard(struct wl_listener *listener, void *data) {
	struct wlr_virtual_keyboard_v1 *keyboard = data;
	server_new_keyboard(&keyboard->keyboard.base, true);
	wlr_seat_set_capabilities(server.seat,
		WL_SEAT_CAPABILITY_POINTER | WL_SEAT_CAPABILITY_KEYBOARD);
}

static void server_new_virtual_pointer(struct wl_listener *listener, void *data) {
	struct wlr_virtual_pointer_v1_new_pointer_event *event = data;
	wlr_cursor_attach_input_device(server.cursor, &event->new_pointer->pointer.base);
}

/* ---- seat and pointer --------------------------------------------------- */

static void seat_request_cursor(struct wl_listener *listener, void *data) {
	struct wlr_seat_pointer_request_set_cursor_event *event = data;
	if (server.seat->pointer_state.focused_client == event->seat_client) {
		wlr_cursor_set_surface(server.cursor, event->surface,
			event->hotspot_x, event->hotspot_y);
	}
}

static void seat_pointer_focus_change(struct wl_listener *listener, void *data) {
	struct wlr_seat_pointer_focus_change_event *event = data;
	if (event->new_surface == NULL) {
		wlr_cursor_set_xcursor(server.cursor, server.cursor_mgr, "default");
	}
}

static void seat_request_set_selection(struct wl_listener *listener, void *data) {
	struct wlr_seat_request_set_selection_event *event = data;
	wlr_seat_set_selection(server.seat, event->source, event->serial);
}

static void seat_request_set_primary_selection(struct wl_listener *listener, void *data) {
	struct wlr_seat_request_set_primary_selection_event *event = data;
	wlr_seat_set_primary_selection(server.seat, event->source, event->serial);
}

/* Clients may name a cursor shape instead of sending an image. */
static void request_set_cursor_shape(struct wl_listener *listener, void *data) {
	struct wlr_cursor_shape_manager_v1_request_set_shape_event *event = data;
	if (event->seat_client == server.seat->pointer_state.focused_client) {
		wlr_cursor_set_xcursor(server.cursor, server.cursor_mgr,
			wlr_cursor_shape_v1_name(event->shape));
	}
}

static struct wh_view *view_at(double lx, double ly,
		struct wlr_surface **surface, double *sx, double *sy) {
	struct wlr_scene_node *node = wlr_scene_node_at(
		&server.scene->tree.node, lx, ly, sx, sy);
	if (node == NULL || node->type != WLR_SCENE_NODE_BUFFER) {
		return NULL;
	}
	struct wlr_scene_buffer *scene_buffer = wlr_scene_buffer_from_node(node);
	struct wlr_scene_surface *scene_surface =
		wlr_scene_surface_try_from_buffer(scene_buffer);
	if (!scene_surface) {
		return NULL;
	}
	*surface = scene_surface->surface;
	/* Only a view's container carries node.data. */
	struct wlr_scene_tree *tree = node->parent;
	while (tree != NULL && tree->node.data == NULL) {
		tree = tree->node.parent;
	}
	if (tree == NULL) {
		return NULL;
	}
	struct wh_view *view;
	wl_list_for_each(view, &server.views, link) {
		if (view == tree->node.data) {
			return view;
		}
	}
	return NULL;
}

static void keyboard_enter(struct wlr_surface *surface) {
	struct wlr_keyboard *keyboard = wlr_seat_get_keyboard(server.seat);
	if (keyboard != NULL) {
		wlr_seat_keyboard_notify_enter(server.seat, surface, keyboard->keycodes,
			keyboard->num_keycodes, &keyboard->modifiers);
	} else {
		wlr_seat_keyboard_notify_enter(server.seat, surface, NULL, 0, NULL);
	}
}

static void focus_layer(struct wlr_layer_surface_v1 *layer_surface) {
	if (server.locked) {
		return;
	}
	server.focused_layer = layer_surface;
	keyboard_enter(layer_surface->surface);
}

static void process_grab(void) {
	struct wh_view *view = server.grab_view;
	int dx = (int)(server.cursor->x - server.grab_cursor_x);
	int dy = (int)(server.cursor->y - server.grab_cursor_y);
	if (server.grab_mode == WH_GRAB_MOVE) {
		view_place(view, server.grab_x + dx, server.grab_y + dy,
			server.grab_w, server.grab_h);
	} else {
		int w = server.grab_w + dx, h = server.grab_h + dy;
		view_place(view, server.grab_x, server.grab_y,
			w < 64 ? 64 : w, h < 48 ? 48 : h);
	}
}

static void end_grab(void) {
	struct wh_view *view = server.grab_view;
	server.grab_view = NULL;
	server.grab_mode = WH_GRAB_NONE;
	if (view && cb_view_moved) {
		cb_view_moved(view->id, view->x, view->y, view->width, view->height);
	}
}

static void process_cursor_motion(uint32_t time) {
	notify_activity();
	if (server.grab_view) {
		process_grab();
		return;
	}
	double sx, sy;
	struct wlr_surface *surface = NULL;
	struct wh_view *view = view_at(server.cursor->x, server.cursor->y,
		&surface, &sx, &sy);
	if (!view) {
		wlr_cursor_set_xcursor(server.cursor, server.cursor_mgr, "default");
	}
	/* Only real motion reports a new view: a window moving under a still
	 * pointer (a layout change) never takes focus. Leaving every view (gaps,
	 * bars) resets it, so coming back onto the same window counts again. */
	wh_view_id hovered = view && view->mapped && !server.locked ? view->id : 0;
	if (hovered != server.hover_view) {
		server.hover_view = hovered;
		if (hovered && hovered != server.focused_view && cb_view_entered) {
			server.hover_focus = true;
			cb_view_entered(hovered);
			server.hover_focus = false;
		}
	}
	if (surface) {
		wlr_seat_pointer_notify_enter(server.seat, surface, sx, sy);
		wlr_seat_pointer_notify_motion(server.seat, time, sx, sy);
	} else {
		wlr_seat_pointer_clear_focus(server.seat);
	}
}

static void server_cursor_motion(struct wl_listener *listener, void *data) {
	struct wlr_pointer_motion_event *event = data;
	wlr_cursor_move(server.cursor, &event->pointer->base,
		event->delta_x, event->delta_y);
	process_cursor_motion(event->time_msec);
}

static void server_cursor_motion_absolute(struct wl_listener *listener, void *data) {
	struct wlr_pointer_motion_absolute_event *event = data;
	wlr_cursor_warp_absolute(server.cursor, &event->pointer->base,
		event->x, event->y);
	process_cursor_motion(event->time_msec);
}

/* mouse_mod + left drag moves a view, + right drag resizes it. The brain
 * is told first (it floats tiled views) and gets the final rect at the end. */
static bool try_begin_grab(struct wlr_pointer_button_event *event) {
	struct wlr_keyboard *keyboard = wlr_seat_get_keyboard(server.seat);
	if (!server.mouse_mod || !keyboard ||
			(wlr_keyboard_get_modifiers(keyboard) & server.mouse_mod) != server.mouse_mod ||
			(event->button != BTN_LEFT && event->button != BTN_RIGHT)) {
		return false;
	}
	double sx, sy;
	struct wlr_surface *surface = NULL;
	struct wh_view *view = view_at(server.cursor->x, server.cursor->y,
		&surface, &sx, &sy);
	if (!view || view->mode == WH_VIEW_FULLSCREEN) {
		return false;
	}
	if (cb_view_grab) {
		cb_view_grab(view->id, view->x, view->y, view->width, view->height);
	}
	if (view->mode != WH_VIEW_FLOATING) {
		return true; /* the brain declined to float it */
	}
	server.grab_view = view;
	server.grab_mode = event->button == BTN_LEFT ? WH_GRAB_MOVE : WH_GRAB_RESIZE;
	server.grab_cursor_x = server.cursor->x;
	server.grab_cursor_y = server.cursor->y;
	server.grab_x = view->x;
	server.grab_y = view->y;
	server.grab_w = view->width;
	server.grab_h = view->height;
	return true;
}

static void server_cursor_button(struct wl_listener *listener, void *data) {
	struct wlr_pointer_button_event *event = data;
	notify_activity();
	if (server.grab_view) {
		if (event->state == WL_POINTER_BUTTON_STATE_RELEASED) {
			end_grab();
		}
		return;
	}
	if (server.locked) {
		wlr_seat_pointer_notify_button(server.seat, event->time_msec,
			event->button, event->state);
		return;
	}
	if (event->state == WL_POINTER_BUTTON_STATE_PRESSED && try_begin_grab(event)) {
		return;
	}
	wlr_seat_pointer_notify_button(server.seat, event->time_msec,
		event->button, event->state);
	if (event->state == WL_POINTER_BUTTON_STATE_PRESSED && cb_view_clicked) {
		double sx, sy;
		struct wlr_surface *surface = NULL;
		struct wh_view *view = view_at(server.cursor->x, server.cursor->y,
			&surface, &sx, &sy);
		if (view) {
			cb_view_clicked(view->id);
		} else if (surface) {
			struct wlr_layer_surface_v1 *layer_surface =
				wlr_layer_surface_v1_try_from_wlr_surface(surface);
			if (layer_surface && layer_surface->current.keyboard_interactive !=
					ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_NONE) {
				focus_layer(layer_surface);
			}
		}
	}
}

static void server_cursor_axis(struct wl_listener *listener, void *data) {
	struct wlr_pointer_axis_event *event = data;
	notify_activity();
	wlr_seat_pointer_notify_axis(server.seat, event->time_msec,
		event->orientation, event->delta, event->delta_discrete,
		event->source, event->relative_direction);
}

static void server_cursor_frame(struct wl_listener *listener, void *data) {
	wlr_seat_pointer_notify_frame(server.seat);
}

/* ---- touchpad gestures -------------------------------------------------- */

/* A swipe shorter than this (in libinput's unaccelerated units, roughly
 * pixels) is a slip of the fingers, not a gesture. */
#define WH_SWIPE_MIN 50.0

static void swipe_begin(struct wl_listener *listener, void *data) {
	struct wlr_pointer_swipe_begin_event *event = data;
	notify_activity();
	server.swipe_fingers = event->fingers;
	server.swipe_dx = server.swipe_dy = 0;
	/* Ask the brain once, at the start, whether it binds this many fingers. */
	server.swipe_ours = !server.locked && cb_swipe &&
		cb_swipe((int)event->fingers, WH_SWIPE_QUERY) != 0;
	if (!server.swipe_ours) {
		wlr_pointer_gestures_v1_send_swipe_begin(server.pointer_gestures,
			server.seat, event->time_msec, event->fingers);
	}
}

static void swipe_update(struct wl_listener *listener, void *data) {
	struct wlr_pointer_swipe_update_event *event = data;
	if (server.swipe_ours) {
		server.swipe_dx += event->dx;
		server.swipe_dy += event->dy;
		return;
	}
	wlr_pointer_gestures_v1_send_swipe_update(server.pointer_gestures,
		server.seat, event->time_msec, event->dx, event->dy);
}

static void swipe_end(struct wl_listener *listener, void *data) {
	struct wlr_pointer_swipe_end_event *event = data;
	if (!server.swipe_ours) {
		wlr_pointer_gestures_v1_send_swipe_end(server.pointer_gestures,
			server.seat, event->time_msec, event->cancelled);
		return;
	}
	server.swipe_ours = false;
	double dx = server.swipe_dx, dy = server.swipe_dy;
	if (event->cancelled || server.locked || dx * dx + dy * dy < WH_SWIPE_MIN * WH_SWIPE_MIN) {
		return;
	}
	/* The dominant axis decides the direction. */
	int direction = fabs(dx) > fabs(dy)
		? (dx < 0 ? WH_SWIPE_LEFT : WH_SWIPE_RIGHT)
		: (dy < 0 ? WH_SWIPE_UP : WH_SWIPE_DOWN);
	cb_swipe((int)server.swipe_fingers, direction);
}

static void pinch_begin(struct wl_listener *listener, void *data) {
	struct wlr_pointer_pinch_begin_event *event = data;
	notify_activity();
	wlr_pointer_gestures_v1_send_pinch_begin(server.pointer_gestures,
		server.seat, event->time_msec, event->fingers);
}

static void pinch_update(struct wl_listener *listener, void *data) {
	struct wlr_pointer_pinch_update_event *event = data;
	wlr_pointer_gestures_v1_send_pinch_update(server.pointer_gestures,
		server.seat, event->time_msec, event->dx, event->dy,
		event->scale, event->rotation);
}

static void pinch_end(struct wl_listener *listener, void *data) {
	struct wlr_pointer_pinch_end_event *event = data;
	wlr_pointer_gestures_v1_send_pinch_end(server.pointer_gestures,
		server.seat, event->time_msec, event->cancelled);
}

static void hold_begin(struct wl_listener *listener, void *data) {
	struct wlr_pointer_hold_begin_event *event = data;
	notify_activity();
	wlr_pointer_gestures_v1_send_hold_begin(server.pointer_gestures,
		server.seat, event->time_msec, event->fingers);
}

static void hold_end(struct wl_listener *listener, void *data) {
	struct wlr_pointer_hold_end_event *event = data;
	wlr_pointer_gestures_v1_send_hold_end(server.pointer_gestures,
		server.seat, event->time_msec, event->cancelled);
}

/* ---- outputs ------------------------------------------------------------ */

static struct wlr_scene_tree *layer_tree(enum zwlr_layer_shell_v1_layer layer) {
	switch (layer) {
	case ZWLR_LAYER_SHELL_V1_LAYER_BACKGROUND: return server.layers[WH_LAYER_BACKGROUND];
	case ZWLR_LAYER_SHELL_V1_LAYER_BOTTOM: return server.layers[WH_LAYER_BOTTOM];
	case ZWLR_LAYER_SHELL_V1_LAYER_TOP: return server.layers[WH_LAYER_TOP];
	default: return server.layers[WH_LAYER_OVERLAY];
	}
}

static struct wlr_output *first_output(void) {
	if (wl_list_empty(&server.outputs)) {
		return NULL;
	}
	/* Outputs are kept in the order they appeared. */
	struct wh_output *output = wl_container_of(server.outputs.next, output, link);
	return output->wlr_output;
}

/* Place layer surfaces (exclusive ones first, so bars carve out their space
 * before anything else is anchored) and tell the brain what area is left. */
static void arrange_output_layers(struct wh_output *wh_output) {
	struct wlr_output *output = wh_output->wlr_output;
	struct wlr_box full;
	wlr_output_layout_get_box(server.output_layout, output, &full);
	wh_output->full = full;
	wh_output->usable = full;
	if (wlr_box_empty(&full)) {
		return;
	}
	struct wlr_box usable = full;
	for (int exclusive = 1; exclusive >= 0; exclusive--) {
		for (int layer = ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY;
				layer >= ZWLR_LAYER_SHELL_V1_LAYER_BACKGROUND; layer--) {
			struct wh_layer *l;
			wl_list_for_each(l, &server.layer_surfaces, link) {
				struct wlr_layer_surface_v1 *ls = l->layer_surface;
				if (ls->output != output || !ls->initialized ||
						(int)ls->current.layer != layer ||
						(ls->current.exclusive_zone > 0) != exclusive) {
					continue;
				}
				wlr_scene_layer_surface_v1_configure(l->scene, &full, &usable);
			}
		}
	}
	wh_output->usable = usable;
}

static void arrange_layers(void) {
	struct wh_output *output;
	wl_list_for_each(output, &server.outputs, link) {
		arrange_output_layers(output);
	}
	if (cb_outputs_changed) {
		cb_outputs_changed();
	}
}

/* Outputs in screen order: left to right, then top to bottom. */
static int sorted_outputs(struct wh_output **out, int max) {
	int n = 0;
	struct wh_output *output;
	wl_list_for_each(output, &server.outputs, link) {
		if (n < max && !wlr_box_empty(&output->full)) {
			out[n++] = output;
		}
	}
	for (int i = 1; i < n; i++) {
		for (int j = i; j > 0 && (out[j]->full.x < out[j - 1]->full.x ||
				(out[j]->full.x == out[j - 1]->full.x &&
				 out[j]->full.y < out[j - 1]->full.y)); j--) {
			struct wh_output *t = out[j];
			out[j] = out[j - 1];
			out[j - 1] = t;
		}
	}
	return n;
}

int wh_output_count(void) {
	struct wh_output *outs[16];
	return sorted_outputs(outs, 16);
}

/* Fills full x,y,w,h then usable x,y,w,h for screen i; 0 on success. */
int wh_output_info(int i, int *out) {
	struct wh_output *outs[16];
	int n = sorted_outputs(outs, 16);
	if (i < 0 || i >= n) {
		return -1;
	}
	struct wlr_box f = outs[i]->full, u = outs[i]->usable;
	int v[8] = { f.x, f.y, f.width, f.height, u.x, u.y, u.width, u.height };
	memcpy(out, v, sizeof(v));
	return 0;
}

const char *wh_output_name(int i) {
	struct wh_output *outs[16];
	int n = sorted_outputs(outs, 16);
	return i >= 0 && i < n ? outs[i]->wlr_output->name : NULL;
}

static struct wh_output_rule *rule_for(struct wlr_output *output) {
	if (server.output_rules.next == NULL) {
		return NULL;
	}
	struct wh_output_rule *rule;
	wl_list_for_each(rule, &server.output_rules, link) {
		if (strcmp(rule->name, output->name) == 0) {
			return rule;
		}
	}
	return NULL;
}

/* Scale and position from the config (scale 1, auto position without a
 * rule). Re-adding to the layout moves an output already in it. */
static void apply_output_rule(struct wlr_output *output, bool commit_scale) {
	struct wh_output_rule *rule = rule_for(output);
	float scale = rule && rule->scale > 0 ? rule->scale : 1.0f;
	if (commit_scale && output->scale != scale) {
		struct wlr_output_state state;
		wlr_output_state_init(&state);
		wlr_output_state_set_scale(&state, scale);
		wlr_output_commit_state(output, &state);
		wlr_output_state_finish(&state);
	}
	wlr_xcursor_manager_load(server.cursor_mgr, scale);
	struct wlr_output_layout_output *l_output = rule && rule->has_position
		? wlr_output_layout_add(server.output_layout, output, rule->x, rule->y)
		: wlr_output_layout_add_auto(server.output_layout, output);
	(void)l_output;
}

void wh_output_rules_clear(void) {
	if (server.output_rules.next == NULL) {
		wl_list_init(&server.output_rules);
	}
	struct wh_output_rule *rule, *tmp;
	wl_list_for_each_safe(rule, tmp, &server.output_rules, link) {
		wl_list_remove(&rule->link);
		free(rule->name);
		free(rule);
	}
}

void wh_output_rule_add(const char *name, float scale, int x, int y, int has_position) {
	if (server.output_rules.next == NULL) {
		wl_list_init(&server.output_rules);
	}
	struct wh_output_rule *rule = calloc(1, sizeof(*rule));
	rule->name = strdup(name);
	rule->scale = scale;
	rule->x = x;
	rule->y = y;
	rule->has_position = has_position != 0;
	wl_list_insert(server.output_rules.prev, &rule->link);
}

/* Re-apply the rules to every output (after a config reload). */
void wh_output_rules_apply(void) {
	if (server.outputs.next == NULL) {
		return;
	}
	struct wh_output *output;
	wl_list_for_each(output, &server.outputs, link) {
		if (!output->disabled) {
			apply_output_rule(output->wlr_output, true);
		}
	}
	arrange_layers();
}

static void output_frame(struct wl_listener *listener, void *data) {
	struct wh_output *output = wl_container_of(listener, output, frame);
	struct wlr_scene_output *scene_output = wlr_scene_get_scene_output(
		server.scene, output->wlr_output);
	wlr_scene_output_commit(scene_output, NULL);
	struct timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);
	wlr_scene_output_send_frame_done(scene_output, &now);
}

static void output_request_state(struct wl_listener *listener, void *data) {
	/* Nested backends ask for a new mode when their window is resized. */
	struct wh_output *output = wl_container_of(listener, output, request_state);
	const struct wlr_output_event_request_state *event = data;
	wlr_output_commit_state(output->wlr_output, event->state);
	arrange_layers();
}

static void output_destroy(struct wl_listener *listener, void *data) {
	struct wh_output *output = wl_container_of(listener, output, destroy);
	if (server.workspace_group && server.workspace_group_output == output->wlr_output) {
		wlr_ext_workspace_group_handle_v1_output_leave(server.workspace_group,
			output->wlr_output);
		server.workspace_group_output = NULL;
	}
	wl_list_remove(&output->frame.link);
	wl_list_remove(&output->request_state.link);
	wl_list_remove(&output->destroy.link);
	wl_list_remove(&output->link);
	free(output);
	/* Bars and windows on it move to the outputs that are left. */
	struct wlr_output *first = first_output();
	if (server.workspace_group && !server.workspace_group_output && first) {
		wlr_ext_workspace_group_handle_v1_output_enter(server.workspace_group, first);
		server.workspace_group_output = first;
	}
	arrange_layers();
}

/* Show an output that is in the layout through the scene graph. */
static void attach_scene_output(struct wlr_output *wlr_output) {
	struct wlr_output_layout_output *l_output =
		wlr_output_layout_get(server.output_layout, wlr_output);
	/* Taking an output out of the layout keeps its scene output; one
	 * left over from before it was turned off must go first. */
	struct wlr_scene_output *old = wlr_scene_get_scene_output(server.scene, wlr_output);
	if (old) {
		wlr_scene_output_destroy(old);
	}
	struct wlr_scene_output *scene_output =
		wlr_scene_output_create(server.scene, wlr_output);
	wlr_scene_output_layout_add_output(server.scene_layout, l_output, scene_output);
}

static void server_new_output(struct wl_listener *listener, void *data) {
	struct wlr_output *wlr_output = data;
	wlr_output_init_render(wlr_output, server.allocator, server.renderer);

	struct wlr_output_state state;
	wlr_output_state_init(&state);
	wlr_output_state_set_enabled(&state, true);
	struct wlr_output_mode *mode = wlr_output_preferred_mode(wlr_output);
	if (mode != NULL) {
		wlr_output_state_set_mode(&state, mode);
	}
	struct wh_output_rule *rule = rule_for(wlr_output);
	if (rule && rule->scale > 0) {
		wlr_output_state_set_scale(&state, rule->scale);
	}
	wlr_output_commit_state(wlr_output, &state);
	wlr_output_state_finish(&state);

	struct wh_output *output = calloc(1, sizeof(*output));
	output->wlr_output = wlr_output;
	output->server = &server;
	output->frame.notify = output_frame;
	wl_signal_add(&wlr_output->events.frame, &output->frame);
	output->request_state.notify = output_request_state;
	wl_signal_add(&wlr_output->events.request_state, &output->request_state);
	output->destroy.notify = output_destroy;
	wl_signal_add(&wlr_output->events.destroy, &output->destroy);
	wl_list_insert(server.outputs.prev, &output->link);

	apply_output_rule(wlr_output, false);
	attach_scene_output(wlr_output);

	ensure_workspace_group();
	arrange_layers();
}

/* ---- output management (wlr-randr, kanshi) ------------------------------ */

static struct wh_output *output_from_wlr(struct wlr_output *wlr_output) {
	struct wh_output *output;
	wl_list_for_each(output, &server.outputs, link) {
		if (output->wlr_output == wlr_output) {
			return output;
		}
	}
	return NULL;
}

/* Tell output-management clients what the outputs look like now. Runs on
 * every layout change, which covers hotplug, mode, scale and position. */
static void update_output_manager(void) {
	if (!server.output_mgr) {
		return;
	}
	struct wlr_output_configuration_v1 *config = wlr_output_configuration_v1_create();
	struct wh_output *output;
	wl_list_for_each(output, &server.outputs, link) {
		struct wlr_output_configuration_head_v1 *head =
			wlr_output_configuration_head_v1_create(config, output->wlr_output);
		/* Blanked by wlopm still counts as on; only our own "off" is off. */
		head->state.enabled = !output->disabled;
		struct wlr_box box;
		wlr_output_layout_get_box(server.output_layout, output->wlr_output, &box);
		if (!wlr_box_empty(&box)) {
			head->state.x = box.x;
			head->state.y = box.y;
		}
	}
	wlr_output_manager_v1_set_configuration(server.output_mgr, config);
}

static void layout_change(struct wl_listener *listener, void *data) {
	update_output_manager();
}

/* Commit (or only test) a whole configuration at once, then move the
 * outputs in the layout to match. */
static void output_config_handle(struct wlr_output_configuration_v1 *config,
		bool test_only) {
	size_t n = 0;
	struct wlr_backend_output_state *states =
		wlr_output_configuration_v1_build_state(config, &n);
	bool ok = states != NULL;
	if (ok) {
		ok = test_only ? wlr_backend_test(server.backend, states, n)
			: wlr_backend_commit(server.backend, states, n);
		for (size_t i = 0; i < n; i++) {
			wlr_output_state_finish(&states[i].base);
		}
		free(states);
	}
	if (ok && !test_only) {
		struct wlr_output_configuration_head_v1 *head;
		wl_list_for_each(head, &config->heads, link) {
			struct wh_output *output = output_from_wlr(head->state.output);
			if (!output) {
				continue;
			}
			if (!head->state.enabled) {
				if (!output->disabled) {
					output->disabled = true;
					wlr_output_layout_remove(server.output_layout, output->wlr_output);
				}
				continue;
			}
			bool was_disabled = output->disabled;
			output->disabled = false;
			wlr_xcursor_manager_load(server.cursor_mgr, output->wlr_output->scale);
			/* Adding an output already in the layout moves it. */
			wlr_output_layout_add(server.output_layout, output->wlr_output,
				head->state.x, head->state.y);
			if (was_disabled) {
				attach_scene_output(output->wlr_output);
			}
		}
		arrange_layers();
	}
	if (ok) {
		wlr_output_configuration_v1_send_succeeded(config);
	} else {
		wlr_output_configuration_v1_send_failed(config);
	}
	wlr_output_configuration_v1_destroy(config);
	if (!test_only) {
		/* After a failure too, so clients see the state that stayed. */
		update_output_manager();
	}
}

static void output_mgr_apply(struct wl_listener *listener, void *data) {
	output_config_handle(data, false);
}

static void output_mgr_test(struct wl_listener *listener, void *data) {
	output_config_handle(data, true);
}

/* ---- views (xdg toplevels) ---------------------------------------------- */

static void xdg_toplevel_map(struct wl_listener *listener, void *data) {
	struct wh_view *view = wl_container_of(listener, view, map);
	view->mapped = true;
	if (cb_view_mapped) {
		cb_view_mapped(view->id);
	}
	/* Clients that start fullscreen (mpv --fs, games) asked before mapping. */
	if (view->xdg_toplevel->requested.fullscreen && cb_view_fullscreen) {
		cb_view_fullscreen(view->id, 1);
	}
}

static void xdg_toplevel_unmap(struct wl_listener *listener, void *data) {
	struct wh_view *view = wl_container_of(listener, view, unmap);
	view->mapped = false;
	if (server.grab_view == view) {
		server.grab_view = NULL;
		server.grab_mode = WH_GRAB_NONE;
	}
	if (cb_view_unmapped) {
		cb_view_unmapped(view->id);
	}
	/* Same as x11_unmap: a hidden window must not leave its borders. */
	wlr_scene_node_set_enabled(&view->container->node, false);
}

static void xdg_toplevel_commit(struct wl_listener *listener, void *data) {
	struct wh_view *view = wl_container_of(listener, view, commit);
	if (view->xdg_toplevel->base->initial_commit) {
		/* Tiled from the start: no client-side shadows or rounded corners. */
		wlr_xdg_toplevel_set_tiled(view->xdg_toplevel,
			WLR_EDGE_TOP | WLR_EDGE_BOTTOM | WLR_EDGE_LEFT | WLR_EDGE_RIGHT);
		wlr_xdg_toplevel_set_size(view->xdg_toplevel, 0, 0);
	}
}

static void xdg_toplevel_destroy(struct wl_listener *listener, void *data) {
	struct wh_view *view = wl_container_of(listener, view, destroy);
	wl_list_remove(&view->map.link);
	wl_list_remove(&view->unmap.link);
	wl_list_remove(&view->commit.link);
	wl_list_remove(&view->destroy.link);
	wl_list_remove(&view->request_maximize.link);
	wl_list_remove(&view->request_fullscreen.link);
	wl_list_remove(&view->link);
	wlr_scene_node_destroy(&view->container->node);
	free(view);
}

static void xdg_toplevel_request_state(struct wl_listener *listener, void *data) {
	/* Maximize/fullscreen are not supported yet; xdg-shell still requires a
	 * configure in reply once the surface is initialized. */
	struct wh_view *view = wl_container_of(listener, view, request_maximize);
	if (view->xdg_toplevel->base->initialized) {
		wlr_xdg_surface_schedule_configure(view->xdg_toplevel->base);
	}
}

static void xdg_toplevel_request_fullscreen(struct wl_listener *listener, void *data) {
	struct wh_view *view = wl_container_of(listener, view, request_fullscreen);
	if (view->mapped && cb_view_fullscreen) {
		cb_view_fullscreen(view->id, view->xdg_toplevel->requested.fullscreen);
	}
	/* xdg-shell wants a configure in reply even when nothing changes. */
	if (view->xdg_toplevel->base->initialized) {
		wlr_xdg_surface_schedule_configure(view->xdg_toplevel->base);
	}
}

static void server_new_xdg_toplevel(struct wl_listener *listener, void *data) {
	struct wlr_xdg_toplevel *xdg_toplevel = data;

	struct wh_view *view = calloc(1, sizeof(*view));
	view->server = &server;
	view->id = ++server.next_id;
	view->xdg_toplevel = xdg_toplevel;
	view->container = wlr_scene_tree_create(server.layers[WH_LAYER_TILE]);
	view->container->node.data = view;
	for (int i = 0; i < 4; i++) {
		view->border[i] = wlr_scene_rect_create(view->container, 0, 0,
			server.border_normal);
	}
	view->surface_tree = wlr_scene_xdg_surface_create(view->container,
		xdg_toplevel->base);
	xdg_toplevel->base->data = view->surface_tree;
	/* Hidden until the brain places it. */
	wlr_scene_node_set_enabled(&view->container->node, false);
	wl_list_insert(&server.views, &view->link);

	view->map.notify = xdg_toplevel_map;
	wl_signal_add(&xdg_toplevel->base->surface->events.map, &view->map);
	view->unmap.notify = xdg_toplevel_unmap;
	wl_signal_add(&xdg_toplevel->base->surface->events.unmap, &view->unmap);
	view->commit.notify = xdg_toplevel_commit;
	wl_signal_add(&xdg_toplevel->base->surface->events.commit, &view->commit);
	view->destroy.notify = xdg_toplevel_destroy;
	wl_signal_add(&xdg_toplevel->events.destroy, &view->destroy);
	view->request_maximize.notify = xdg_toplevel_request_state;
	wl_signal_add(&xdg_toplevel->events.request_maximize, &view->request_maximize);
	view->request_fullscreen.notify = xdg_toplevel_request_fullscreen;
	wl_signal_add(&xdg_toplevel->events.request_fullscreen, &view->request_fullscreen);
}

/* Find the scene tree a popup's parent draws into: an xdg surface (window
 * or another popup) or a layer surface (bar menus). */
static struct wlr_scene_tree *popup_parent_tree(struct wlr_surface *parent) {
	if (parent == NULL) {
		return NULL;
	}
	struct wlr_xdg_surface *xdg = wlr_xdg_surface_try_from_wlr_surface(parent);
	if (xdg) {
		return xdg->data;
	}
	struct wlr_layer_surface_v1 *ls = wlr_layer_surface_v1_try_from_wlr_surface(parent);
	return ls ? ls->data : NULL;
}

static void xdg_popup_commit(struct wl_listener *listener, void *data) {
	struct wh_popup *popup = wl_container_of(listener, popup, commit);
	struct wlr_xdg_popup *xdg_popup = popup->xdg_popup;
	if (!xdg_popup->base->initial_commit) {
		return;
	}
	/* Layer-shell popups get their parent after creation (get_popup), so
	 * the scene node is created here rather than in new_popup. */
	if (xdg_popup->base->data == NULL) {
		struct wlr_scene_tree *parent_tree = popup_parent_tree(xdg_popup->parent);
		if (parent_tree) {
			xdg_popup->base->data =
				wlr_scene_xdg_surface_create(parent_tree, xdg_popup->base);
		}
	}
	wlr_xdg_surface_schedule_configure(xdg_popup->base);
}

static void xdg_popup_destroy(struct wl_listener *listener, void *data) {
	struct wh_popup *popup = wl_container_of(listener, popup, destroy);
	wl_list_remove(&popup->commit.link);
	wl_list_remove(&popup->destroy.link);
	free(popup);
}

static void server_new_xdg_popup(struct wl_listener *listener, void *data) {
	struct wlr_xdg_popup *xdg_popup = data;
	struct wh_popup *popup = calloc(1, sizeof(*popup));
	popup->xdg_popup = xdg_popup;

	struct wlr_scene_tree *parent_tree = popup_parent_tree(xdg_popup->parent);
	if (parent_tree) {
		xdg_popup->base->data = wlr_scene_xdg_surface_create(parent_tree,
			xdg_popup->base);
	}

	popup->commit.notify = xdg_popup_commit;
	wl_signal_add(&xdg_popup->base->surface->events.commit, &popup->commit);
	popup->destroy.notify = xdg_popup_destroy;
	wl_signal_add(&xdg_popup->events.destroy, &popup->destroy);
}

static void decoration_apply(struct wh_decoration *deco) {
	if (deco->decoration->toplevel->base->initialized) {
		wlr_xdg_toplevel_decoration_v1_set_mode(deco->decoration,
			WLR_XDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE);
	}
}

static void decoration_request_mode(struct wl_listener *listener, void *data) {
	struct wh_decoration *deco = wl_container_of(listener, deco, request_mode);
	decoration_apply(deco);
}

static void decoration_commit(struct wl_listener *listener, void *data) {
	/* The mode can only be sent once the surface is initialized, which
	 * happens on its first commit. */
	struct wh_decoration *deco = wl_container_of(listener, deco, commit);
	if (deco->decoration->toplevel->base->initial_commit) {
		decoration_apply(deco);
	}
}

static void decoration_destroy(struct wl_listener *listener, void *data) {
	struct wh_decoration *deco = wl_container_of(listener, deco, destroy);
	wl_list_remove(&deco->request_mode.link);
	wl_list_remove(&deco->commit.link);
	wl_list_remove(&deco->destroy.link);
	free(deco);
}

static void server_new_decoration(struct wl_listener *listener, void *data) {
	struct wlr_xdg_toplevel_decoration_v1 *decoration = data;
	struct wh_decoration *deco = calloc(1, sizeof(*deco));
	deco->decoration = decoration;
	deco->request_mode.notify = decoration_request_mode;
	wl_signal_add(&decoration->events.request_mode, &deco->request_mode);
	deco->commit.notify = decoration_commit;
	wl_signal_add(&decoration->toplevel->base->surface->events.commit, &deco->commit);
	deco->destroy.notify = decoration_destroy;
	wl_signal_add(&decoration->events.destroy, &deco->destroy);
	decoration_apply(deco);
}

/* ---- workspaces (ext-workspace-v1) -------------------------------------- */

/* Workspaces belong to the first output's group, created once an output
 * exists; workspaces added before that join the group when it appears. */
static void ensure_workspace_group(void) {
	struct wlr_output *output = first_output();
	if (server.workspace_group || !output || !server.workspace_mgr) {
		return;
	}
	server.workspace_group =
		wlr_ext_workspace_group_handle_v1_create(server.workspace_mgr, 0);
	wlr_ext_workspace_group_handle_v1_output_enter(server.workspace_group, output);
	server.workspace_group_output = output;
	struct wh_workspace *ws;
	wl_list_for_each(ws, &server.workspaces, link) {
		wlr_ext_workspace_handle_v1_set_group(ws->handle, server.workspace_group);
	}
}

static void workspace_commit(struct wl_listener *listener, void *data) {
	struct wlr_ext_workspace_v1_commit_event *event = data;
	struct wlr_ext_workspace_v1_request *req;
	wl_list_for_each(req, event->requests, link) {
		if (req->type != WLR_EXT_WORKSPACE_V1_REQUEST_ACTIVATE ||
				!req->activate.workspace || !cb_workspace_activate) {
			continue;
		}
		struct wh_workspace *ws = req->activate.workspace->data;
		if (ws) {
			cb_workspace_activate(ws->tag);
		}
	}
}

void wh_workspace_add(int tag, const char *name) {
	if (!server.workspace_mgr) {
		return;
	}
	char id[32];
	snprintf(id, sizeof(id), "whiplash-%d", tag);
	struct wh_workspace *ws = calloc(1, sizeof(*ws));
	ws->tag = tag;
	ws->handle = wlr_ext_workspace_handle_v1_create(server.workspace_mgr, id,
		EXT_WORKSPACE_HANDLE_V1_WORKSPACE_CAPABILITIES_ACTIVATE);
	ws->handle->data = ws;
	wlr_ext_workspace_handle_v1_set_name(ws->handle, name);
	/* Bars such as Utumno sort by coordinates, so give each one. */
	uint32_t coord = (uint32_t)tag;
	wlr_ext_workspace_handle_v1_set_coordinates(ws->handle, &coord, 1);
	if (server.workspace_group) {
		wlr_ext_workspace_handle_v1_set_group(ws->handle, server.workspace_group);
	}
	wl_list_insert(server.workspaces.prev, &ws->link);
}

void wh_workspace_set_active(int tag) {
	struct wh_workspace *ws;
	wl_list_for_each(ws, &server.workspaces, link) {
		wlr_ext_workspace_handle_v1_set_active(ws->handle, ws->tag == tag);
	}
}

void wh_workspace_set_urgent(int tag, int on) {
	struct wh_workspace *ws;
	wl_list_for_each(ws, &server.workspaces, link) {
		if (ws->tag == tag) {
			wlr_ext_workspace_handle_v1_set_urgent(ws->handle, on != 0);
		}
	}
}

/* A Wayland window asks for attention (a link opened from a terminal, a
 * notification action). It is never given focus from here: the brain
 * marks it urgent, as wasp does. */
static void request_activation(struct wl_listener *listener, void *data) {
	struct wlr_xdg_activation_v1_request_activate_event *event = data;
	struct wh_view *view = view_from_surface(event->surface);
	if (view && cb_view_urgent) {
		cb_view_urgent(view->id, 1);
	}
}

/* ---- layer shell (bars, launchers, wallpapers, lock screens) ------------ */

static void layer_map(struct wl_listener *listener, void *data) {
	struct wh_layer *l = wl_container_of(listener, l, map);
	struct wlr_layer_surface_v1 *ls = l->layer_surface;
	arrange_layers();
	if (ls->current.keyboard_interactive ==
				ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_EXCLUSIVE ||
			(ls->current.keyboard_interactive ==
				ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_ON_DEMAND &&
			 ls->current.layer >= ZWLR_LAYER_SHELL_V1_LAYER_TOP)) {
		focus_layer(ls);
	}
}

static void restore_view_focus(void) {
	server.focused_layer = NULL;
	wh_view_focus(server.focused_view);
}

static void layer_unmap(struct wl_listener *listener, void *data) {
	struct wh_layer *l = wl_container_of(listener, l, unmap);
	arrange_layers();
	if (server.focused_layer == l->layer_surface) {
		restore_view_focus();
	}
}

static void layer_commit(struct wl_listener *listener, void *data) {
	struct wh_layer *l = wl_container_of(listener, l, commit);
	struct wlr_layer_surface_v1 *ls = l->layer_surface;
	if (!ls->initialized) {
		return;
	}
	struct wlr_scene_tree *tree = layer_tree(ls->current.layer);
	if (l->scene->tree->node.parent != tree) {
		wlr_scene_node_reparent(&l->scene->tree->node, tree);
	}
	if (ls->initial_commit || ls->current.committed) {
		arrange_layers();
	}
}

static void layer_destroy(struct wl_listener *listener, void *data) {
	struct wh_layer *l = wl_container_of(listener, l, destroy);
	bool had_focus = server.focused_layer == l->layer_surface;
	wl_list_remove(&l->map.link);
	wl_list_remove(&l->unmap.link);
	wl_list_remove(&l->commit.link);
	wl_list_remove(&l->destroy.link);
	wl_list_remove(&l->link);
	free(l);
	arrange_layers();
	if (had_focus) {
		restore_view_focus();
	}
}

static void server_new_layer_surface(struct wl_listener *listener, void *data) {
	struct wlr_layer_surface_v1 *ls = data;
	if (ls->output == NULL) {
		ls->output = wlr_output_layout_output_at(server.output_layout,
			server.cursor->x, server.cursor->y);
	}
	if (ls->output == NULL) {
		ls->output = first_output();
	}
	if (ls->output == NULL) {
		wlr_layer_surface_v1_destroy(ls);
		return;
	}
	struct wh_layer *l = calloc(1, sizeof(*l));
	l->layer_surface = ls;
	l->scene = wlr_scene_layer_surface_v1_create(layer_tree(ls->pending.layer), ls);
	/* Popups of this surface (bar menus) attach to this tree. */
	ls->data = l->scene->tree;
	wl_list_insert(&server.layer_surfaces, &l->link);

	l->map.notify = layer_map;
	wl_signal_add(&ls->surface->events.map, &l->map);
	l->unmap.notify = layer_unmap;
	wl_signal_add(&ls->surface->events.unmap, &l->unmap);
	l->commit.notify = layer_commit;
	wl_signal_add(&ls->surface->events.commit, &l->commit);
	l->destroy.notify = layer_destroy;
	wl_signal_add(&ls->events.destroy, &l->destroy);
}

/* ---- idle inhibit and output power ------------------------------------- */

/* Inhibited while any inhibitor exists (a destroyed one is still listed
 * during its destroy signal, so it can be excluded). */
static void update_idle_inhibit(struct wlr_idle_inhibitor_v1 *going) {
	bool inhibited = false;
	struct wlr_idle_inhibitor_v1 *inhibitor;
	wl_list_for_each(inhibitor, &server.idle_inhibit_mgr->inhibitors, link) {
		if (inhibitor != going) {
			inhibited = true;
			break;
		}
	}
	wlr_idle_notifier_v1_set_inhibited(server.idle_notifier, inhibited);
}

static void idle_inhibitor_destroy(struct wl_listener *listener, void *data) {
	struct wh_idle_inhibitor *i = wl_container_of(listener, i, destroy);
	update_idle_inhibit(i->inhibitor);
	wl_list_remove(&i->destroy.link);
	free(i);
}

static void server_new_idle_inhibitor(struct wl_listener *listener, void *data) {
	struct wh_idle_inhibitor *i = calloc(1, sizeof(*i));
	i->inhibitor = data;
	i->destroy.notify = idle_inhibitor_destroy;
	wl_signal_add(&i->inhibitor->events.destroy, &i->destroy);
	update_idle_inhibit(NULL);
}

static void output_power_set_mode(struct wl_listener *listener, void *data) {
	struct wlr_output_power_v1_set_mode_event *event = data;
	struct wlr_output_state state;
	wlr_output_state_init(&state);
	wlr_output_state_set_enabled(&state,
		event->mode == ZWLR_OUTPUT_POWER_V1_MODE_ON);
	wlr_output_commit_state(event->output, &state);
	wlr_output_state_finish(&state);
}

/* ---- XWayland ----------------------------------------------------------- */

static void x11_map(struct wl_listener *listener, void *data) {
	struct wh_view *view = wl_container_of(listener, view, map);
	view->surface_tree = wlr_scene_subsurface_tree_create(view->container,
		view->xsurface->surface);
	view_apply_frame(view);
	view->mapped = true;
	if (cb_view_mapped) {
		cb_view_mapped(view->id);
	}
	if (view->xsurface->fullscreen && cb_view_fullscreen) {
		cb_view_fullscreen(view->id, 1);
	}
}

static void x11_unmap(struct wl_listener *listener, void *data) {
	struct wh_view *view = wl_container_of(listener, view, unmap);
	view->mapped = false;
	if (server.grab_view == view) {
		server.grab_view = NULL;
		server.grab_mode = WH_GRAB_NONE;
	}
	if (cb_view_unmapped) {
		cb_view_unmapped(view->id);
	}
	/* Hidden, not destroyed (Steam closing to the tray): the brain forgets
	 * the view, so hide its borders here or they stay on screen. */
	wlr_scene_node_set_enabled(&view->container->node, false);
	if (view->surface_tree) {
		wlr_scene_node_destroy(&view->surface_tree->node);
		view->surface_tree = NULL;
	}
}

static void x11_associate(struct wl_listener *listener, void *data) {
	struct wh_view *view = wl_container_of(listener, view, associate);
	view->map.notify = x11_map;
	wl_signal_add(&view->xsurface->surface->events.map, &view->map);
	view->unmap.notify = x11_unmap;
	wl_signal_add(&view->xsurface->surface->events.unmap, &view->unmap);
	view->associated = true;
}

static void x11_dissociate(struct wl_listener *listener, void *data) {
	struct wh_view *view = wl_container_of(listener, view, dissociate);
	if (view->associated) {
		wl_list_remove(&view->map.link);
		wl_list_remove(&view->unmap.link);
		view->associated = false;
	}
}

static void x11_request_configure(struct wl_listener *listener, void *data) {
	struct wh_view *view = wl_container_of(listener, view, request_configure);
	struct wlr_xwayland_surface_configure_event *event = data;
	if (!view->mapped) {
		/* Not placed yet: let it have what it asks for. */
		wlr_xwayland_surface_configure(view->xsurface, event->x, event->y,
			event->width, event->height);
		return;
	}
	if (view->mode == WH_VIEW_FLOATING) {
		/* Floating windows may resize themselves; tell the brain. */
		int bw = view_border(view);
		view_place(view, view->x, view->y, event->width + 2 * bw,
			event->height + 2 * bw);
		if (cb_view_moved) {
			cb_view_moved(view->id, view->x, view->y, view->width, view->height);
		}
	} else {
		/* Tiled and fullscreen windows keep the brain's geometry. */
		view_place(view, view->x, view->y, view->width, view->height);
	}
}

static void x11_request_fullscreen(struct wl_listener *listener, void *data) {
	struct wh_view *view = wl_container_of(listener, view, request_fullscreen);
	if (view->mapped && cb_view_fullscreen) {
		cb_view_fullscreen(view->id, view->xsurface->fullscreen);
	}
}

static void x11_request_activate(struct wl_listener *listener, void *data) {
	struct wh_view *view = wl_container_of(listener, view, request_activate);
	if (view->mapped && cb_view_clicked) {
		cb_view_clicked(view->id);
	}
}

/* WM_HINTS changed: the urgency flag comes and goes with it. */
static void x11_set_hints(struct wl_listener *listener, void *data) {
	struct wh_view *view = wl_container_of(listener, view, set_hints);
	xcb_icccm_wm_hints_t *hints = view->xsurface->hints;
	bool urgent = hints && (hints->flags & XCB_ICCCM_WM_HINT_X_URGENCY);
	if (view->mapped && cb_view_urgent) {
		cb_view_urgent(view->id, urgent);
	}
}

static void x11_destroy(struct wl_listener *listener, void *data) {
	struct wh_view *view = wl_container_of(listener, view, destroy);
	if (view->associated) {
		wl_list_remove(&view->map.link);
		wl_list_remove(&view->unmap.link);
	}
	wl_list_remove(&view->associate.link);
	wl_list_remove(&view->dissociate.link);
	wl_list_remove(&view->destroy.link);
	wl_list_remove(&view->request_configure.link);
	wl_list_remove(&view->request_fullscreen.link);
	wl_list_remove(&view->request_activate.link);
	wl_list_remove(&view->set_hints.link);
	wl_list_remove(&view->link);
	if (server.grab_view == view) {
		server.grab_view = NULL;
		server.grab_mode = WH_GRAB_NONE;
	}
	wlr_scene_node_destroy(&view->container->node);
	free(view);
}

static void unmanaged_map(struct wl_listener *listener, void *data) {
	struct wh_unmanaged *u = wl_container_of(listener, u, map);
	u->tree = wlr_scene_subsurface_tree_create(server.layers[WH_LAYER_UNMANAGED],
		u->xsurface->surface);
	wlr_scene_node_set_position(&u->tree->node, u->xsurface->x, u->xsurface->y);
	if (!server.locked && wlr_xwayland_surface_override_redirect_wants_focus(u->xsurface)) {
		keyboard_enter(u->xsurface->surface);
	}
}

static void unmanaged_unmap(struct wl_listener *listener, void *data) {
	struct wh_unmanaged *u = wl_container_of(listener, u, unmap);
	if (server.seat->keyboard_state.focused_surface == u->xsurface->surface) {
		restore_view_focus();
	}
	if (u->tree) {
		wlr_scene_node_destroy(&u->tree->node);
		u->tree = NULL;
	}
}

static void unmanaged_set_geometry(struct wl_listener *listener, void *data) {
	struct wh_unmanaged *u = wl_container_of(listener, u, set_geometry);
	if (u->tree) {
		wlr_scene_node_set_position(&u->tree->node, u->xsurface->x, u->xsurface->y);
	}
}

static void unmanaged_associate(struct wl_listener *listener, void *data) {
	struct wh_unmanaged *u = wl_container_of(listener, u, associate);
	u->map.notify = unmanaged_map;
	wl_signal_add(&u->xsurface->surface->events.map, &u->map);
	u->unmap.notify = unmanaged_unmap;
	wl_signal_add(&u->xsurface->surface->events.unmap, &u->unmap);
	u->associated = true;
}

static void unmanaged_dissociate(struct wl_listener *listener, void *data) {
	struct wh_unmanaged *u = wl_container_of(listener, u, dissociate);
	if (u->associated) {
		wl_list_remove(&u->map.link);
		wl_list_remove(&u->unmap.link);
		u->associated = false;
	}
}

static void unmanaged_destroy(struct wl_listener *listener, void *data) {
	struct wh_unmanaged *u = wl_container_of(listener, u, destroy);
	if (u->associated) {
		wl_list_remove(&u->map.link);
		wl_list_remove(&u->unmap.link);
	}
	wl_list_remove(&u->associate.link);
	wl_list_remove(&u->dissociate.link);
	wl_list_remove(&u->destroy.link);
	wl_list_remove(&u->set_geometry.link);
	free(u);
}

static void xwayland_new_surface(struct wl_listener *listener, void *data) {
	struct wlr_xwayland_surface *xs = data;
	if (xs->override_redirect) {
		struct wh_unmanaged *u = calloc(1, sizeof(*u));
		u->xsurface = xs;
		u->associate.notify = unmanaged_associate;
		wl_signal_add(&xs->events.associate, &u->associate);
		u->dissociate.notify = unmanaged_dissociate;
		wl_signal_add(&xs->events.dissociate, &u->dissociate);
		u->destroy.notify = unmanaged_destroy;
		wl_signal_add(&xs->events.destroy, &u->destroy);
		u->set_geometry.notify = unmanaged_set_geometry;
		wl_signal_add(&xs->events.set_geometry, &u->set_geometry);
		return;
	}

	struct wh_view *view = calloc(1, sizeof(*view));
	view->server = &server;
	view->id = ++server.next_id;
	view->xsurface = xs;
	view->container = wlr_scene_tree_create(server.layers[WH_LAYER_TILE]);
	view->container->node.data = view;
	for (int i = 0; i < 4; i++) {
		view->border[i] = wlr_scene_rect_create(view->container, 0, 0,
			server.border_normal);
	}
	/* The surface tree is created on map; hidden until the brain places it. */
	wlr_scene_node_set_enabled(&view->container->node, false);
	wl_list_insert(&server.views, &view->link);

	view->associate.notify = x11_associate;
	wl_signal_add(&xs->events.associate, &view->associate);
	view->dissociate.notify = x11_dissociate;
	wl_signal_add(&xs->events.dissociate, &view->dissociate);
	view->destroy.notify = x11_destroy;
	wl_signal_add(&xs->events.destroy, &view->destroy);
	view->request_configure.notify = x11_request_configure;
	wl_signal_add(&xs->events.request_configure, &view->request_configure);
	view->request_fullscreen.notify = x11_request_fullscreen;
	wl_signal_add(&xs->events.request_fullscreen, &view->request_fullscreen);
	view->request_activate.notify = x11_request_activate;
	wl_signal_add(&xs->events.request_activate, &view->request_activate);
	view->set_hints.notify = x11_set_hints;
	wl_signal_add(&xs->events.set_hints, &view->set_hints);
}

static void xwayland_ready(struct wl_listener *listener, void *data) {
	wlr_xwayland_set_seat(server.xwayland, server.seat);
	/* X11 clients otherwise show no cursor (or X's black cross) at first. */
	wlr_xcursor_manager_load(server.cursor_mgr, 1);
	struct wlr_xcursor *xcursor =
		wlr_xcursor_manager_get_xcursor(server.cursor_mgr, "default", 1);
	if (xcursor) {
		struct wlr_xcursor_image *image = xcursor->images[0];
		wlr_xwayland_set_cursor(server.xwayland, wlr_xcursor_image_get_buffer(image),
			image->hotspot_x, image->hotspot_y);
	}
}

/* ---- session lock (ext-session-lock-v1) --------------------------------- */

static void lock_surface_map(struct wl_listener *listener, void *data) {
	struct wh_lock_surface *l = wl_container_of(listener, l, map);
	keyboard_enter(l->lock_surface->surface);
}

static void lock_surface_destroy(struct wl_listener *listener, void *data) {
	struct wh_lock_surface *l = wl_container_of(listener, l, destroy);
	wl_list_remove(&l->map.link);
	wl_list_remove(&l->destroy.link);
	free(l);
}

static void lock_new_surface(struct wl_listener *listener, void *data) {
	struct wlr_session_lock_surface_v1 *lock_surface = data;
	struct wh_lock_surface *l = calloc(1, sizeof(*l));
	l->lock_surface = lock_surface;

	struct wlr_scene_tree *tree = wlr_scene_subsurface_tree_create(
		server.layers[WH_LAYER_LOCK], lock_surface->surface);
	struct wlr_box box;
	wlr_output_layout_get_box(server.output_layout, lock_surface->output, &box);
	wlr_scene_node_set_position(&tree->node, box.x, box.y);
	wlr_session_lock_surface_v1_configure(lock_surface, box.width, box.height);

	l->map.notify = lock_surface_map;
	wl_signal_add(&lock_surface->surface->events.map, &l->map);
	l->destroy.notify = lock_surface_destroy;
	wl_signal_add(&lock_surface->events.destroy, &l->destroy);
}

static void lock_release_listeners(void) {
	wl_list_remove(&server.lock_new_surface.link);
	wl_list_remove(&server.lock_unlock.link);
	wl_list_remove(&server.lock_destroy.link);
	server.lock = NULL;
}

static void lock_unlock(struct wl_listener *listener, void *data) {
	lock_release_listeners();
	server.locked = false;
	wlr_scene_node_set_enabled(&server.lock_bg->node, false);
	restore_view_focus();
}

static void lock_destroy(struct wl_listener *listener, void *data) {
	/* The locker went away without unlocking (crashed or killed): the
	 * session stays locked until a new locker unlocks it. */
	lock_release_listeners();
	wlr_seat_keyboard_notify_clear_focus(server.seat);
}

static void server_new_lock(struct wl_listener *listener, void *data) {
	struct wlr_session_lock_v1 *lock = data;
	if (server.lock) {
		/* Another locker is already active. */
		wlr_session_lock_v1_destroy(lock);
		return;
	}
	server.lock = lock;
	server.locked = true;
	server.focused_layer = NULL;
	server.grab_view = NULL;
	server.grab_mode = WH_GRAB_NONE;
	wlr_scene_node_set_enabled(&server.lock_bg->node, true);
	wlr_seat_keyboard_notify_clear_focus(server.seat);
	wlr_seat_pointer_clear_focus(server.seat);

	server.lock_new_surface.notify = lock_new_surface;
	wl_signal_add(&lock->events.new_surface, &server.lock_new_surface);
	server.lock_unlock.notify = lock_unlock;
	wl_signal_add(&lock->events.unlock, &server.lock_unlock);
	server.lock_destroy.notify = lock_destroy;
	wl_signal_add(&lock->events.destroy, &server.lock_destroy);

	/* Everything is already hidden behind lock_bg. */
	wlr_session_lock_v1_send_locked(lock);
}

/* ---- commands from the brain -------------------------------------------- */

static void view_place(struct wh_view *view, int x, int y, int width, int height) {
	int bw = view_border(view);
	if (width <= 2 * bw || height <= 2 * bw) {
		return;
	}
	view->x = x;
	view->y = y;
	view->width = width;
	view->height = height;
	wlr_scene_node_set_position(&view->container->node, x, y);
	view_apply_frame(view);
	if (view->xsurface) {
		/* X11 clients need their absolute position (menus, drag-and-drop). */
		wlr_xwayland_surface_configure(view->xsurface, x + bw, y + bw,
			width - 2 * bw, height - 2 * bw);
	} else {
		wlr_xdg_toplevel_set_size(view->xdg_toplevel, width - 2 * bw,
			height - 2 * bw);
	}
}

void wh_view_set_geometry(wh_view_id id, int x, int y, int width, int height) {
	struct wh_view *view = view_by_id(id);
	if (view && view != server.grab_view) {
		view_place(view, x, y, width, height);
	}
}

void wh_view_set_mode(wh_view_id id, int mode) {
	struct wh_view *view = view_by_id(id);
	if (!view || (int)view->mode == mode) {
		return;
	}
	view->mode = mode;
	enum wh_scene_layer layer = mode == WH_VIEW_FLOATING ? WH_LAYER_FLOAT
		: mode == WH_VIEW_FULLSCREEN ? WH_LAYER_FULLSCREEN : WH_LAYER_TILE;
	wlr_scene_node_reparent(&view->container->node, server.layers[layer]);
	if (view->xsurface) {
		wlr_xwayland_surface_set_fullscreen(view->xsurface, mode == WH_VIEW_FULLSCREEN);
	} else {
		/* Tiled edges tell clients not to draw shadows or round corners. */
		wlr_xdg_toplevel_set_tiled(view->xdg_toplevel, mode == WH_VIEW_TILED
			? WLR_EDGE_TOP | WLR_EDGE_BOTTOM | WLR_EDGE_LEFT | WLR_EDGE_RIGHT
			: WLR_EDGE_NONE);
		wlr_xdg_toplevel_set_fullscreen(view->xdg_toplevel, mode == WH_VIEW_FULLSCREEN);
	}
	if (server.grab_view == view) {
		server.grab_view = NULL;
		server.grab_mode = WH_GRAB_NONE;
	}
}

const char *wh_view_app_id(wh_view_id id) {
	struct wh_view *view = view_by_id(id);
	const char *app_id = NULL;
	if (view && view->xsurface) {
		app_id = view->xsurface->class;
	} else if (view) {
		app_id = view->xdg_toplevel->app_id;
	}
	return app_id ? app_id : "";
}

int wh_view_wants_float(wh_view_id id) {
	struct wh_view *view = view_by_id(id);
	if (!view) {
		return 0;
	}
	/* Dialogs (windows with a parent) and fixed-size windows float. */
	if (view->xsurface) {
		struct wlr_xwayland_surface *xs = view->xsurface;
		xcb_size_hints_t *hints = xs->size_hints;
		return xs->modal || xs->parent != NULL ||
			wlr_xwayland_surface_has_window_type(xs, WLR_XWAYLAND_NET_WM_WINDOW_TYPE_DIALOG) ||
			wlr_xwayland_surface_has_window_type(xs, WLR_XWAYLAND_NET_WM_WINDOW_TYPE_UTILITY) ||
			wlr_xwayland_surface_has_window_type(xs, WLR_XWAYLAND_NET_WM_WINDOW_TYPE_SPLASH) ||
			wlr_xwayland_surface_has_window_type(xs, WLR_XWAYLAND_NET_WM_WINDOW_TYPE_TOOLBAR) ||
			(hints && hints->min_width > 0 && hints->min_height > 0 &&
			 hints->min_width == hints->max_width &&
			 hints->min_height == hints->max_height);
	}
	struct wlr_xdg_toplevel *t = view->xdg_toplevel;
	if (t->parent != NULL) {
		return 1;
	}
	return t->current.min_width > 0 && t->current.min_height > 0 &&
		t->current.min_width == t->current.max_width &&
		t->current.min_height == t->current.max_height;
}

int wh_view_natural_width(wh_view_id id) {
	struct wh_view *view = view_by_id(id);
	return !view ? 0 : view->xsurface ? view->xsurface->width
		: view->xdg_toplevel->base->geometry.width;
}

int wh_view_natural_height(wh_view_id id) {
	struct wh_view *view = view_by_id(id);
	return !view ? 0 : view->xsurface ? view->xsurface->height
		: view->xdg_toplevel->base->geometry.height;
}

void wh_set_mouse_mod(uint32_t mods) {
	server.mouse_mod = mods;
}

void wh_view_set_visible(wh_view_id id, int visible) {
	struct wh_view *view = view_by_id(id);
	if (view) {
		wlr_scene_node_set_enabled(&view->container->node, visible != 0);
	}
}

void wh_view_focus(wh_view_id id) {
	struct wlr_seat *seat = server.seat;
	struct wlr_surface *prev = seat->keyboard_state.focused_surface;
	struct wh_view *view = id ? view_by_id(id) : NULL;
	struct wlr_surface *next = view && view->mapped ? view_surface(view) : NULL;

	server.focused_view = id;
	struct wh_view *other;
	wl_list_for_each(other, &server.views, link) {
		view_paint_border(other, other == view);
	}
	if (server.locked) {
		return;
	}
	/* An exclusive layer (lock screen, open launcher) keeps the keyboard
	 * until it goes away; the choice is remembered for then. */
	if (server.focused_layer && server.focused_layer->current.keyboard_interactive ==
			ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_EXCLUSIVE) {
		return;
	}
	server.focused_layer = NULL;
	if (prev == next) {
		return;
	}
	if (prev) {
		struct wh_view *prev_view = view_from_surface(prev);
		if (prev_view) {
			view_set_activated(prev_view, false);
		}
	}
	if (!next) {
		wlr_seat_keyboard_notify_clear_focus(seat);
		return;
	}
	if (!server.hover_focus) {
		wlr_scene_node_raise_to_top(&view->container->node);
	}
	view_set_activated(view, true);
	struct wlr_keyboard *keyboard = wlr_seat_get_keyboard(seat);
	if (keyboard != NULL) {
		wlr_seat_keyboard_notify_enter(seat, next, keyboard->keycodes,
			keyboard->num_keycodes, &keyboard->modifiers);
	}
}

void wh_view_close(wh_view_id id) {
	struct wh_view *view = view_by_id(id);
	if (view && view->xsurface) {
		wlr_xwayland_surface_close(view->xsurface);
	} else if (view) {
		wlr_xdg_toplevel_send_close(view->xdg_toplevel);
	}
}

void wh_set_border(int width, uint32_t focused_rgba, uint32_t normal_rgba) {
	server.border_width = width < 0 ? 0 : width;
	rgba_to_floats(focused_rgba, server.border_focused);
	rgba_to_floats(normal_rgba, server.border_normal);
}

void wh_spawn(const char *command) {
	/* Double fork so the child is reparented to init and never becomes a
	 * zombie of the compositor. */
	pid_t pid = fork();
	if (pid == 0) {
		setsid();
		if (fork() == 0) {
			execl("/bin/sh", "/bin/sh", "-c", command, (void *)NULL);
			_exit(127);
		}
		_exit(0);
	}
	if (pid > 0) {
		waitpid(pid, NULL, 0);
	}
}

/* ---- config file watch ------------------------------------------------- */

static int config_watch_ready(int fd, uint32_t mask, void *data) {
	char buf[4096] __attribute__((aligned(__alignof__(struct inotify_event))));
	bool changed = false;
	ssize_t len;
	while ((len = read(fd, buf, sizeof(buf))) > 0) {
		for (char *p = buf; p < buf + len;) {
			struct inotify_event *event = (struct inotify_event *)p;
			if (event->len && strcmp(event->name, server.config_name) == 0) {
				changed = true;
			}
			p += sizeof(*event) + event->len;
		}
	}
	if (changed && cb_config_changed) {
		cb_config_changed();
	}
	return 0;
}

int wh_watch_config(const char *dir, const char *name) {
	if (server.config_watch_source) {
		return 0;
	}
	int fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
	if (fd < 0) {
		return -1;
	}
	if (inotify_add_watch(fd, dir, IN_CLOSE_WRITE | IN_MOVED_TO | IN_CREATE) < 0) {
		close(fd);
		return -1;
	}
	server.config_watch_fd = fd;
	free(server.config_name);
	server.config_name = strdup(name);
	server.config_watch_source = wl_event_loop_add_fd(
		wl_display_get_event_loop(server.wl_display), fd, WL_EVENT_READABLE,
		config_watch_ready, NULL);
	return 0;
}

/* ---- lifecycle ---------------------------------------------------------- */

int wh_init(void) {
	wlr_log_init(WLR_INFO, NULL);
	wl_list_init(&server.views);
	wl_list_init(&server.outputs);
	wl_list_init(&server.keyboards);
	wl_list_init(&server.touchpads);
	if (server.output_rules.next == NULL) {
		wl_list_init(&server.output_rules);
	}
	if (server.border_width == 0 && server.border_focused[3] == 0) {
		wh_set_border(2, 0xaa96daff, 0x444444ff);
	}

	server.wl_display = wl_display_create();
	server.backend = wlr_backend_autocreate(
		wl_display_get_event_loop(server.wl_display), &server.session);
	if (server.backend == NULL) {
		wlr_log(WLR_ERROR, "failed to create wlr_backend");
		return 1;
	}
	server.renderer = wlr_renderer_autocreate(server.backend);
	if (server.renderer == NULL) {
		wlr_log(WLR_ERROR, "failed to create wlr_renderer");
		return 1;
	}
	wlr_renderer_init_wl_display(server.renderer, server.wl_display);
	server.allocator = wlr_allocator_autocreate(server.backend, server.renderer);
	if (server.allocator == NULL) {
		wlr_log(WLR_ERROR, "failed to create wlr_allocator");
		return 1;
	}

	server.compositor = wlr_compositor_create(server.wl_display, 5, server.renderer);
	wlr_subcompositor_create(server.wl_display);
	wlr_data_device_manager_create(server.wl_display);
	/* Small protocols many clients take for granted (mpv needs viewporter);
	 * the scene graph handles them without further work. */
	wlr_viewporter_create(server.wl_display);
	wlr_single_pixel_buffer_manager_v1_create(server.wl_display);
	wlr_fractional_scale_manager_v1_create(server.wl_display, 1);
	wlr_primary_selection_v1_device_manager_create(server.wl_display);
	/* Clipboard managers (cliphist, wl-clipboard --watch). */
	wlr_data_control_manager_v1_create(server.wl_display);
	wlr_ext_data_control_manager_v1_create(server.wl_display, 1);

	server.output_layout = wlr_output_layout_create(server.wl_display);
	server.new_output.notify = server_new_output;
	wl_signal_add(&server.backend->events.new_output, &server.new_output);
	server.layout_change.notify = layout_change;
	wl_signal_add(&server.output_layout->events.change, &server.layout_change);

	server.output_mgr = wlr_output_manager_v1_create(server.wl_display);
	server.output_mgr_apply.notify = output_mgr_apply;
	wl_signal_add(&server.output_mgr->events.apply, &server.output_mgr_apply);
	server.output_mgr_test.notify = output_mgr_test;
	wl_signal_add(&server.output_mgr->events.test, &server.output_mgr_test);

	server.scene = wlr_scene_create();
	server.scene_layout = wlr_scene_attach_output_layout(server.scene,
		server.output_layout);
	for (int i = 0; i < WH_LAYER_COUNT; i++) {
		server.layers[i] = wlr_scene_tree_create(&server.scene->tree);
	}
	/* Night light (wlsunset, gammastep): the scene applies the gamma
	 * tables on its own commits. */
	wlr_scene_set_gamma_control_manager_v1(server.scene,
		wlr_gamma_control_manager_v1_create(server.wl_display));

	wl_list_init(&server.layer_surfaces);
	server.layer_shell = wlr_layer_shell_v1_create(server.wl_display, 4);
	server.new_layer_surface.notify = server_new_layer_surface;
	wl_signal_add(&server.layer_shell->events.new_surface, &server.new_layer_surface);

	/* grim and friends; quickshell reads output names through xdg-output.
	 * xdg-desktop-portal-wlr (screen sharing) uses screencopy and
	 * export-dmabuf. */
	wlr_screencopy_manager_v1_create(server.wl_display);
	wlr_export_dmabuf_manager_v1_create(server.wl_display);

	/* Covers every output while locked; 16k is larger than any layout. */
	static const float lock_color[4] = { 0.06f, 0.06f, 0.08f, 1.0f };
	server.lock_bg = wlr_scene_rect_create(server.layers[WH_LAYER_LOCK],
		16384, 16384, lock_color);
	wlr_scene_node_set_enabled(&server.lock_bg->node, false);
	server.idle_notifier = wlr_idle_notifier_v1_create(server.wl_display);
	server.idle_inhibit_mgr = wlr_idle_inhibit_v1_create(server.wl_display);
	server.new_idle_inhibitor.notify = server_new_idle_inhibitor;
	wl_signal_add(&server.idle_inhibit_mgr->events.new_inhibitor,
		&server.new_idle_inhibitor);
	server.output_power_mgr = wlr_output_power_manager_v1_create(server.wl_display);
	server.output_power_set_mode.notify = output_power_set_mode;
	wl_signal_add(&server.output_power_mgr->events.set_mode,
		&server.output_power_set_mode);

	server.lock_mgr = wlr_session_lock_manager_v1_create(server.wl_display);
	server.new_lock.notify = server_new_lock;
	wl_signal_add(&server.lock_mgr->events.new_lock, &server.new_lock);

	wl_list_init(&server.workspaces);
	server.workspace_mgr = wlr_ext_workspace_manager_v1_create(server.wl_display, 1);
	if (server.workspace_mgr) {
		server.workspace_commit.notify = workspace_commit;
		wl_signal_add(&server.workspace_mgr->events.commit, &server.workspace_commit);
	}
	wlr_xdg_output_manager_v1_create(server.wl_display, server.output_layout);
	server.activation = wlr_xdg_activation_v1_create(server.wl_display);
	server.request_activation.notify = request_activation;
	wl_signal_add(&server.activation->events.request_activate,
		&server.request_activation);

	server.xdg_shell = wlr_xdg_shell_create(server.wl_display, 3);
	server.new_xdg_toplevel.notify = server_new_xdg_toplevel;
	wl_signal_add(&server.xdg_shell->events.new_toplevel, &server.new_xdg_toplevel);
	server.new_xdg_popup.notify = server_new_xdg_popup;
	wl_signal_add(&server.xdg_shell->events.new_popup, &server.new_xdg_popup);

	server.decoration_mgr = wlr_xdg_decoration_manager_v1_create(server.wl_display);
	server.new_decoration.notify = server_new_decoration;
	wl_signal_add(&server.decoration_mgr->events.new_toplevel_decoration,
		&server.new_decoration);

	server.cursor = wlr_cursor_create();
	wlr_cursor_attach_output_layout(server.cursor, server.output_layout);
	server.cursor_mgr = wlr_xcursor_manager_create(NULL, 24);
	server.cursor_motion.notify = server_cursor_motion;
	wl_signal_add(&server.cursor->events.motion, &server.cursor_motion);
	server.cursor_motion_absolute.notify = server_cursor_motion_absolute;
	wl_signal_add(&server.cursor->events.motion_absolute,
		&server.cursor_motion_absolute);
	server.cursor_button.notify = server_cursor_button;
	wl_signal_add(&server.cursor->events.button, &server.cursor_button);
	server.cursor_axis.notify = server_cursor_axis;
	wl_signal_add(&server.cursor->events.axis, &server.cursor_axis);
	server.cursor_frame.notify = server_cursor_frame;
	wl_signal_add(&server.cursor->events.frame, &server.cursor_frame);

	server.pointer_gestures = wlr_pointer_gestures_v1_create(server.wl_display);
	server.swipe_begin.notify = swipe_begin;
	wl_signal_add(&server.cursor->events.swipe_begin, &server.swipe_begin);
	server.swipe_update.notify = swipe_update;
	wl_signal_add(&server.cursor->events.swipe_update, &server.swipe_update);
	server.swipe_end.notify = swipe_end;
	wl_signal_add(&server.cursor->events.swipe_end, &server.swipe_end);
	server.pinch_begin.notify = pinch_begin;
	wl_signal_add(&server.cursor->events.pinch_begin, &server.pinch_begin);
	server.pinch_update.notify = pinch_update;
	wl_signal_add(&server.cursor->events.pinch_update, &server.pinch_update);
	server.pinch_end.notify = pinch_end;
	wl_signal_add(&server.cursor->events.pinch_end, &server.pinch_end);
	server.hold_begin.notify = hold_begin;
	wl_signal_add(&server.cursor->events.hold_begin, &server.hold_begin);
	server.hold_end.notify = hold_end;
	wl_signal_add(&server.cursor->events.hold_end, &server.hold_end);

	server.new_input.notify = server_new_input;
	wl_signal_add(&server.backend->events.new_input, &server.new_input);
	server.seat = wlr_seat_create(server.wl_display, "seat0");
	server.virtual_keyboard_mgr =
		wlr_virtual_keyboard_manager_v1_create(server.wl_display);
	server.virtual_pointer_mgr = wlr_virtual_pointer_manager_v1_create(server.wl_display);
	server.new_virtual_pointer.notify = server_new_virtual_pointer;
	wl_signal_add(&server.virtual_pointer_mgr->events.new_virtual_pointer,
		&server.new_virtual_pointer);
	server.new_virtual_keyboard.notify = server_new_virtual_keyboard;
	wl_signal_add(&server.virtual_keyboard_mgr->events.new_virtual_keyboard,
		&server.new_virtual_keyboard);
	server.request_cursor.notify = seat_request_cursor;
	wl_signal_add(&server.seat->events.request_set_cursor, &server.request_cursor);
	server.pointer_focus_change.notify = seat_pointer_focus_change;
	wl_signal_add(&server.seat->pointer_state.events.focus_change,
		&server.pointer_focus_change);
	/* Lazy: Xwayland only starts when the first X11 client connects. */
	server.xwayland = wlr_xwayland_create(server.wl_display, server.compositor, true);
	if (server.xwayland) {
		server.xwayland_ready.notify = xwayland_ready;
		wl_signal_add(&server.xwayland->events.ready, &server.xwayland_ready);
		server.xwayland_new_surface.notify = xwayland_new_surface;
		wl_signal_add(&server.xwayland->events.new_surface, &server.xwayland_new_surface);
		setenv("DISPLAY", server.xwayland->display_name, true);
	} else {
		wlr_log(WLR_ERROR, "XWayland unavailable; X11 applications will not run");
	}
	server.request_set_primary_selection.notify = seat_request_set_primary_selection;
	wl_signal_add(&server.seat->events.request_set_primary_selection,
		&server.request_set_primary_selection);
	server.cursor_shape_mgr = wlr_cursor_shape_manager_v1_create(server.wl_display, 1);
	server.request_set_cursor_shape.notify = request_set_cursor_shape;
	wl_signal_add(&server.cursor_shape_mgr->events.request_set_shape,
		&server.request_set_cursor_shape);
	server.request_set_selection.notify = seat_request_set_selection;
	wl_signal_add(&server.seat->events.request_set_selection,
		&server.request_set_selection);

	server.socket = wl_display_add_socket_auto(server.wl_display);
	if (!server.socket) {
		wlr_backend_destroy(server.backend);
		return 1;
	}
	if (!wlr_backend_start(server.backend)) {
		wlr_backend_destroy(server.backend);
		wl_display_destroy(server.wl_display);
		return 1;
	}
	setenv("WAYLAND_DISPLAY", server.socket, true);
	wlr_log(WLR_INFO, "whiplash running on WAYLAND_DISPLAY=%s", server.socket);
	return 0;
}

const char *wh_socket(void) {
	return server.socket;
}

void wh_run(void) {
	wl_display_run(server.wl_display);
}

void wh_quit(void) {
	wl_display_terminate(server.wl_display);
}

void wh_shutdown(void) {
	if (server.xwayland) {
		wl_list_remove(&server.xwayland_ready.link);
		wl_list_remove(&server.xwayland_new_surface.link);
		wlr_xwayland_destroy(server.xwayland);
		server.xwayland = NULL;
	}
	wl_display_destroy_clients(server.wl_display);

	wl_list_remove(&server.new_xdg_toplevel.link);
	wl_list_remove(&server.new_xdg_popup.link);
	wl_list_remove(&server.new_decoration.link);
	wl_list_remove(&server.new_layer_surface.link);
	wl_list_remove(&server.new_lock.link);
	wl_list_remove(&server.new_idle_inhibitor.link);
	wl_list_remove(&server.output_power_set_mode.link);
	if (server.lock) {
		lock_release_listeners();
	}
	if (server.workspace_mgr) {
		wl_list_remove(&server.workspace_commit.link);
	}
	wl_list_remove(&server.cursor_motion.link);
	wl_list_remove(&server.cursor_motion_absolute.link);
	wl_list_remove(&server.cursor_button.link);
	wl_list_remove(&server.cursor_axis.link);
	wl_list_remove(&server.cursor_frame.link);
	wl_list_remove(&server.swipe_begin.link);
	wl_list_remove(&server.swipe_update.link);
	wl_list_remove(&server.swipe_end.link);
	wl_list_remove(&server.pinch_begin.link);
	wl_list_remove(&server.pinch_update.link);
	wl_list_remove(&server.pinch_end.link);
	wl_list_remove(&server.hold_begin.link);
	wl_list_remove(&server.hold_end.link);
	wl_list_remove(&server.new_input.link);
	wl_list_remove(&server.new_virtual_keyboard.link);
	wl_list_remove(&server.new_virtual_pointer.link);
	wl_list_remove(&server.request_cursor.link);
	wl_list_remove(&server.pointer_focus_change.link);
	wl_list_remove(&server.request_set_selection.link);
	wl_list_remove(&server.request_set_primary_selection.link);
	wl_list_remove(&server.request_set_cursor_shape.link);
	wl_list_remove(&server.new_output.link);
	wl_list_remove(&server.layout_change.link);
	wl_list_remove(&server.request_activation.link);
	wl_list_remove(&server.output_mgr_apply.link);
	wl_list_remove(&server.output_mgr_test.link);

	wlr_scene_node_destroy(&server.scene->tree.node);
	wlr_xcursor_manager_destroy(server.cursor_mgr);
	wlr_cursor_destroy(server.cursor);
	wlr_allocator_destroy(server.allocator);
	wlr_renderer_destroy(server.renderer);
	wlr_backend_destroy(server.backend);
	wl_display_destroy(server.wl_display);
	if (server.config_watch_source) {
		close(server.config_watch_fd);
	}
	free(server.config_name);
	free(server.xkb_layout);
	free(server.xkb_variant);
	free(server.xkb_options);
}
