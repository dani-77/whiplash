/*
 * whiplash heart: wlroots plumbing for the Haskell brain.
 *
 * Derived from tinywl (wlroots 0.20.2), which is dedicated to the public
 * domain under CC0. Interactive move/resize and the built-in keybindings were
 * removed: every window-management decision belongs to the brain.
 */
#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <signal.h>
#include <stdbool.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/wait.h>
#include <wayland-server-core.h>
#include <wlr/backend.h>
#include <wlr/backend/session.h>
#include <wlr/render/allocator.h>
#include <wlr/render/wlr_renderer.h>
#include <wlr/types/wlr_cursor.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_data_device.h>
#include <wlr/types/wlr_input_device.h>
#include <wlr/types/wlr_keyboard.h>
#include <wlr/types/wlr_layer_shell_v1.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/types/wlr_pointer.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_screencopy_v1.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/types/wlr_subcompositor.h>
#include <wlr/types/wlr_xcursor_manager.h>
#include <wlr/types/wlr_virtual_keyboard_v1.h>
#include <wlr/types/wlr_xdg_decoration_v1.h>
#include <wlr/types/wlr_xdg_output_v1.h>
#include <wlr/types/wlr_xdg_shell.h>
#include <wlr/util/edges.h>
#include <wlr/util/log.h>
#include <xkbcommon/xkbcommon.h>

#include "heart.h"

/* Scene stacking order, bottom to top. Tiled windows live between the
 * layer-shell "bottom" and "top" layers, so bars draw above them. */
enum wh_scene_layer {
	WH_LAYER_BACKGROUND,
	WH_LAYER_BOTTOM,
	WH_LAYER_TILE,
	WH_LAYER_TOP,
	WH_LAYER_OVERLAY,
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
	/* Output area left after exclusive zones (bars), as reported. */
	struct wlr_box usable;

	struct wlr_xdg_shell *xdg_shell;
	struct wl_listener new_xdg_toplevel;
	struct wl_listener new_xdg_popup;
	struct wl_list views;

	struct wlr_xdg_decoration_manager_v1 *decoration_mgr;
	struct wl_listener new_decoration;

	struct wlr_cursor *cursor;
	struct wlr_xcursor_manager *cursor_mgr;
	struct wl_listener cursor_motion;
	struct wl_listener cursor_motion_absolute;
	struct wl_listener cursor_button;
	struct wl_listener cursor_axis;
	struct wl_listener cursor_frame;

	struct wlr_seat *seat;
	struct wl_listener new_input;
	struct wl_listener request_cursor;
	struct wl_listener pointer_focus_change;
	struct wl_listener request_set_selection;
	struct wl_list keyboards;
	struct wlr_virtual_keyboard_manager_v1 *virtual_keyboard_mgr;
	struct wl_listener new_virtual_keyboard;

	struct wlr_output_layout *output_layout;
	struct wl_list outputs;
	struct wl_listener new_output;

	const char *socket;
	char *xkb_layout;
	wh_view_id next_id;

	int border_width;
	float border_focused[4];
	float border_normal[4];
};

struct wh_output {
	struct wl_list link;
	struct wh_server *server;
	struct wlr_output *wlr_output;
	struct wl_listener frame;
	struct wl_listener request_state;
	struct wl_listener destroy;
};

struct wh_view {
	struct wl_list link;
	struct wh_server *server;
	wh_view_id id;
	bool mapped;
	struct wlr_xdg_toplevel *xdg_toplevel;
	/* container holds the four border rects and the surface tree */
	struct wlr_scene_tree *container;
	struct wlr_scene_tree *surface_tree;
	struct wlr_scene_rect *border[4];
	int width, height;
	struct wl_listener map;
	struct wl_listener unmap;
	struct wl_listener commit;
	struct wl_listener destroy;
	struct wl_listener request_maximize;
	struct wl_listener request_fullscreen;
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

struct wh_popup {
	struct wlr_xdg_popup *xdg_popup;
	struct wl_listener commit;
	struct wl_listener destroy;
};

struct wh_keyboard {
	struct wl_list link;
	struct wh_server *server;
	struct wlr_keyboard *wlr_keyboard;
	struct wl_listener modifiers;
	struct wl_listener key;
	struct wl_listener destroy;
};

static struct wh_server server;

static wh_view_cb cb_view_mapped;
static wh_view_cb cb_view_unmapped;
static wh_view_cb cb_view_clicked;
static wh_key_cb cb_key;
static wh_output_cb cb_output;

void wh_on_view_mapped(wh_view_cb cb) { cb_view_mapped = cb; }
void wh_on_view_unmapped(wh_view_cb cb) { cb_view_unmapped = cb; }
void wh_on_view_clicked(wh_view_cb cb) { cb_view_clicked = cb; }
void wh_on_key(wh_key_cb cb) { cb_key = cb; }
void wh_on_output(wh_output_cb cb) { cb_output = cb; }

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
	int bw = server.border_width;
	int w = view->width, h = view->height;
	wlr_scene_rect_set_size(view->border[0], w, bw);           /* top */
	wlr_scene_node_set_position(&view->border[0]->node, 0, 0);
	wlr_scene_rect_set_size(view->border[1], w, bw);           /* bottom */
	wlr_scene_node_set_position(&view->border[1]->node, 0, h - bw);
	wlr_scene_rect_set_size(view->border[2], bw, h - 2 * bw);  /* left */
	wlr_scene_node_set_position(&view->border[2]->node, 0, bw);
	wlr_scene_rect_set_size(view->border[3], bw, h - 2 * bw);  /* right */
	wlr_scene_node_set_position(&view->border[3]->node, w - bw, bw);
	wlr_scene_node_set_position(&view->surface_tree->node, bw, bw);
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

	if (event->state == WL_KEYBOARD_KEY_STATE_PRESSED && cb_key) {
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

/* Virtual keyboards (wtype, on-screen keyboards) bring their own keymap and
 * repeat settings, so only physical ones get ours. */
static void server_new_keyboard(struct wlr_input_device *device, bool is_virtual) {
	struct wlr_keyboard *wlr_keyboard = wlr_keyboard_from_input_device(device);

	struct wh_keyboard *keyboard = calloc(1, sizeof(*keyboard));
	keyboard->server = &server;
	keyboard->wlr_keyboard = wlr_keyboard;

	if (!is_virtual) {
		struct xkb_rule_names names = { .layout = server.xkb_layout };
		struct xkb_context *context = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
		struct xkb_keymap *keymap = xkb_keymap_new_from_names(context, &names,
			XKB_KEYMAP_COMPILE_NO_FLAGS);
		if (keymap == NULL) {
			wlr_log(WLR_ERROR, "invalid xkb layout '%s', using defaults",
				server.xkb_layout ? server.xkb_layout : "(none)");
			keymap = xkb_keymap_new_from_names(context, NULL,
				XKB_KEYMAP_COMPILE_NO_FLAGS);
		}
		wlr_keyboard_set_keymap(wlr_keyboard, keymap);
		xkb_keymap_unref(keymap);
		xkb_context_unref(context);
		wlr_keyboard_set_repeat_info(wlr_keyboard, 25, 600);
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

static void server_new_input(struct wl_listener *listener, void *data) {
	struct wlr_input_device *device = data;
	switch (device->type) {
	case WLR_INPUT_DEVICE_KEYBOARD:
		server_new_keyboard(device, false);
		break;
	case WLR_INPUT_DEVICE_POINTER:
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
	server.focused_layer = layer_surface;
	keyboard_enter(layer_surface->surface);
}

static void process_cursor_motion(uint32_t time) {
	double sx, sy;
	struct wlr_surface *surface = NULL;
	struct wh_view *view = view_at(server.cursor->x, server.cursor->y,
		&surface, &sx, &sy);
	if (!view) {
		wlr_cursor_set_xcursor(server.cursor, server.cursor_mgr, "default");
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

static void server_cursor_button(struct wl_listener *listener, void *data) {
	struct wlr_pointer_button_event *event = data;
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
	wlr_seat_pointer_notify_axis(server.seat, event->time_msec,
		event->orientation, event->delta, event->delta_discrete,
		event->source, event->relative_direction);
}

static void server_cursor_frame(struct wl_listener *listener, void *data) {
	wlr_seat_pointer_notify_frame(server.seat);
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
	struct wh_output *output = wl_container_of(server.outputs.prev, output, link);
	return output->wlr_output;
}

/* Place layer surfaces (exclusive ones first, so bars carve out their space
 * before anything else is anchored) and tell the brain what area is left. */
static void arrange_layers(void) {
	struct wlr_output *output = first_output();
	if (!output) {
		return;
	}
	struct wlr_box full;
	wlr_output_layout_get_box(server.output_layout, output, &full);
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
	server.usable = usable;
	if (cb_output) {
		cb_output(usable.x, usable.y, usable.width, usable.height);
	}
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
	wl_list_remove(&output->frame.link);
	wl_list_remove(&output->request_state.link);
	wl_list_remove(&output->destroy.link);
	wl_list_remove(&output->link);
	free(output);
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
	wl_list_insert(&server.outputs, &output->link);

	struct wlr_output_layout_output *l_output =
		wlr_output_layout_add_auto(server.output_layout, wlr_output);
	struct wlr_scene_output *scene_output =
		wlr_scene_output_create(server.scene, wlr_output);
	wlr_scene_output_layout_add_output(server.scene_layout, l_output, scene_output);

	arrange_layers();
}

/* ---- views (xdg toplevels) ---------------------------------------------- */

static void xdg_toplevel_map(struct wl_listener *listener, void *data) {
	struct wh_view *view = wl_container_of(listener, view, map);
	view->mapped = true;
	if (cb_view_mapped) {
		cb_view_mapped(view->id);
	}
}

static void xdg_toplevel_unmap(struct wl_listener *listener, void *data) {
	struct wh_view *view = wl_container_of(listener, view, unmap);
	view->mapped = false;
	if (cb_view_unmapped) {
		cb_view_unmapped(view->id);
	}
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

/* ---- commands from the brain -------------------------------------------- */

void wh_view_set_geometry(wh_view_id id, int x, int y, int width, int height) {
	struct wh_view *view = view_by_id(id);
	if (!view) {
		return;
	}
	int bw = server.border_width;
	if (width <= 2 * bw || height <= 2 * bw) {
		return;
	}
	view->width = width;
	view->height = height;
	wlr_scene_node_set_position(&view->container->node, x, y);
	view_apply_frame(view);
	wlr_xdg_toplevel_set_size(view->xdg_toplevel, width - 2 * bw,
		height - 2 * bw);
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
	struct wlr_surface *next =
		view && view->mapped ? view->xdg_toplevel->base->surface : NULL;

	server.focused_view = id;
	struct wh_view *other;
	wl_list_for_each(other, &server.views, link) {
		view_paint_border(other, other == view);
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
		struct wlr_xdg_toplevel *prev_toplevel =
			wlr_xdg_toplevel_try_from_wlr_surface(prev);
		if (prev_toplevel != NULL) {
			wlr_xdg_toplevel_set_activated(prev_toplevel, false);
		}
	}
	if (!next) {
		wlr_seat_keyboard_notify_clear_focus(seat);
		return;
	}
	wlr_scene_node_raise_to_top(&view->container->node);
	wlr_xdg_toplevel_set_activated(view->xdg_toplevel, true);
	struct wlr_keyboard *keyboard = wlr_seat_get_keyboard(seat);
	if (keyboard != NULL) {
		wlr_seat_keyboard_notify_enter(seat, next, keyboard->keycodes,
			keyboard->num_keycodes, &keyboard->modifiers);
	}
}

void wh_view_close(wh_view_id id) {
	struct wh_view *view = view_by_id(id);
	if (view) {
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

/* ---- lifecycle ---------------------------------------------------------- */

int wh_init(const char *xkb_layout) {
	wlr_log_init(WLR_INFO, NULL);
	wl_list_init(&server.views);
	wl_list_init(&server.outputs);
	wl_list_init(&server.keyboards);
	server.xkb_layout = xkb_layout ? strdup(xkb_layout) : NULL;
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

	wlr_compositor_create(server.wl_display, 5, server.renderer);
	wlr_subcompositor_create(server.wl_display);
	wlr_data_device_manager_create(server.wl_display);

	server.output_layout = wlr_output_layout_create(server.wl_display);
	server.new_output.notify = server_new_output;
	wl_signal_add(&server.backend->events.new_output, &server.new_output);

	server.scene = wlr_scene_create();
	server.scene_layout = wlr_scene_attach_output_layout(server.scene,
		server.output_layout);
	for (int i = 0; i < WH_LAYER_COUNT; i++) {
		server.layers[i] = wlr_scene_tree_create(&server.scene->tree);
	}

	wl_list_init(&server.layer_surfaces);
	server.layer_shell = wlr_layer_shell_v1_create(server.wl_display, 4);
	server.new_layer_surface.notify = server_new_layer_surface;
	wl_signal_add(&server.layer_shell->events.new_surface, &server.new_layer_surface);

	/* grim and friends; quickshell reads output names through xdg-output. */
	wlr_screencopy_manager_v1_create(server.wl_display);
	wlr_xdg_output_manager_v1_create(server.wl_display, server.output_layout);

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

	server.new_input.notify = server_new_input;
	wl_signal_add(&server.backend->events.new_input, &server.new_input);
	server.seat = wlr_seat_create(server.wl_display, "seat0");
	server.virtual_keyboard_mgr =
		wlr_virtual_keyboard_manager_v1_create(server.wl_display);
	server.new_virtual_keyboard.notify = server_new_virtual_keyboard;
	wl_signal_add(&server.virtual_keyboard_mgr->events.new_virtual_keyboard,
		&server.new_virtual_keyboard);
	server.request_cursor.notify = seat_request_cursor;
	wl_signal_add(&server.seat->events.request_set_cursor, &server.request_cursor);
	server.pointer_focus_change.notify = seat_pointer_focus_change;
	wl_signal_add(&server.seat->pointer_state.events.focus_change,
		&server.pointer_focus_change);
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
	wl_display_destroy_clients(server.wl_display);

	wl_list_remove(&server.new_xdg_toplevel.link);
	wl_list_remove(&server.new_xdg_popup.link);
	wl_list_remove(&server.new_decoration.link);
	wl_list_remove(&server.new_layer_surface.link);
	wl_list_remove(&server.cursor_motion.link);
	wl_list_remove(&server.cursor_motion_absolute.link);
	wl_list_remove(&server.cursor_button.link);
	wl_list_remove(&server.cursor_axis.link);
	wl_list_remove(&server.cursor_frame.link);
	wl_list_remove(&server.new_input.link);
	wl_list_remove(&server.new_virtual_keyboard.link);
	wl_list_remove(&server.request_cursor.link);
	wl_list_remove(&server.pointer_focus_change.link);
	wl_list_remove(&server.request_set_selection.link);
	wl_list_remove(&server.new_output.link);

	wlr_scene_node_destroy(&server.scene->tree.node);
	wlr_xcursor_manager_destroy(server.cursor_mgr);
	wlr_cursor_destroy(server.cursor);
	wlr_allocator_destroy(server.allocator);
	wlr_renderer_destroy(server.renderer);
	wlr_backend_destroy(server.backend);
	wl_display_destroy(server.wl_display);
	free(server.xkb_layout);
}
