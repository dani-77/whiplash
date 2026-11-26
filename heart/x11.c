/*
 * Native X11 window management. The X server owns rendering and input.
 * Haskell owns the window set, layouts, workspace state and key actions.
 */
#include "x11.h"

#include <stdbool.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <wayland-server-core.h>
#include <xcb/xcb.h>
#include <xcb/xcb_icccm.h>
#include <xkbcommon/xkbcommon-keysyms.h>

struct x11_view {
	struct x11_view *next;
	xcb_window_t id;
	bool mapped, wants_float, fullscreen_hint;
	bool urgency_hint, demands_attention;
	enum wh_view_mode mode;
	unsigned pending_unmaps;
	int natural_width, natural_height;
	char *app_id;
};

struct x11_workspace {
	int tag;
	char *name;
	uint32_t window_count;
	bool urgent;
};

struct x11_rect {
	int x, y, width, height;
};

struct x11_drag {
	wh_view_id id;
	uint8_t button;
	int pointer_x, pointer_y;
	struct x11_rect start;
};

/* All atoms before NET_WM_NAME are properties owned on the root window. */
enum ewmh_atom {
	NET_SUPPORTED, NET_SUPPORTING_WM_CHECK, NET_NUMBER_OF_DESKTOPS,
	NET_CURRENT_DESKTOP, NET_DESKTOP_NAMES, NET_DESKTOP_GEOMETRY,
	NET_DESKTOP_VIEWPORT, NET_WORKAREA, NET_WM_NAME,
	NET_WM_STATE, NET_WM_STATE_FULLSCREEN, NET_WM_STATE_DEMANDS_ATTENTION, UTF8_STRING,
	NET_WM_WINDOW_TYPE, NET_WM_WINDOW_TYPE_DIALOG, NET_WM_WINDOW_TYPE_UTILITY,
	NET_WM_WINDOW_TYPE_SPLASH, NET_WM_WINDOW_TYPE_TOOLBAR,
	NET_WM_STATE_MODAL, D77_WORKSPACE_COUNTS, D77_WORKSPACE_URGENT, EWMH_COUNT,
};

static const char *const ewmh_names[] = {
	"_NET_SUPPORTED", "_NET_SUPPORTING_WM_CHECK", "_NET_NUMBER_OF_DESKTOPS",
	"_NET_CURRENT_DESKTOP", "_NET_DESKTOP_NAMES", "_NET_DESKTOP_GEOMETRY",
	"_NET_DESKTOP_VIEWPORT", "_NET_WORKAREA", "_NET_WM_NAME",
	"_NET_WM_STATE", "_NET_WM_STATE_FULLSCREEN", "_NET_WM_STATE_DEMANDS_ATTENTION", "UTF8_STRING",
	"_NET_WM_WINDOW_TYPE", "_NET_WM_WINDOW_TYPE_DIALOG", "_NET_WM_WINDOW_TYPE_UTILITY",
	"_NET_WM_WINDOW_TYPE_SPLASH", "_NET_WM_WINDOW_TYPE_TOOLBAR",
	"_NET_WM_STATE_MODAL", "_D77_WORKSPACE_COUNTS", "_D77_WORKSPACE_URGENT",
};

static struct {
	xcb_connection_t *connection;
	xcb_window_t root;
	struct wl_display *display;
	struct wl_event_source *events;
	struct wh_x11_callbacks callbacks;
	struct x11_view *views;
	wh_view_id focused, hovered;
	struct x11_drag drag;
	bool focus_follows_mouse, hover_focus;
	xcb_get_keyboard_mapping_reply_t *keymap;
	uint8_t first_keycode, last_keycode;
	uint16_t lock_mask;
	uint32_t mouse_mod;
	uint32_t *bindings;
	unsigned binding_count;
	xcb_atom_t wm_state, wm_protocols, wm_delete_window;
	xcb_atom_t ewmh[EWMH_COUNT];
	xcb_window_t wm_check;
	struct x11_workspace *workspaces;
	uint32_t workspace_count, published_count, current_desktop;
	bool workspace_counts_dirty, workspace_urgency_dirty;
	int width, height, reserve_top;
	int border_width;
	uint32_t focused_border, normal_border;
} x11;

static struct x11_view *find_view(wh_view_id id) {
	for (struct x11_view *view = x11.views; view; view = view->next) {
		if (view->id == id) {
			return view;
		}
	}
	return NULL;
}

static wh_view_id view_under_pointer(void) {
	xcb_query_pointer_reply_t *pointer = xcb_query_pointer_reply(x11.connection,
		xcb_query_pointer(x11.connection, x11.root), NULL);
	wh_view_id id = pointer && pointer->same_screen ? pointer->child : XCB_NONE;
	free(pointer);
	struct x11_view *view = find_view(id);
	return view && view->mapped ? id : XCB_NONE;
}

static void pointer_crossed(const xcb_enter_notify_event_t *event, bool entered) {
	if (!x11.focus_follows_mouse || x11.drag.id || event->mode != XCB_NOTIFY_MODE_NORMAL ||
			event->detail == XCB_NOTIFY_DETAIL_INFERIOR) {
		return;
	}
	wh_view_id id = event->event;
	if (!entered) {
		if (x11.hovered == id && view_under_pointer() != id) {
			x11.hovered = XCB_NONE;
		}
		return;
	}
	if (x11.hovered == id || view_under_pointer() != id) {
		return;
	}
	x11.hovered = id;
	if (id != x11.focused && x11.callbacks.entered) {
		x11.hover_focus = true;
		x11.callbacks.entered(id);
		x11.hover_focus = false;
	}
}

void wh_x11_set_focus_follows_mouse(int enabled) {
	x11.focus_follows_mouse = enabled != 0;
}

static xcb_atom_t intern_atom(const char *name) {
	xcb_intern_atom_cookie_t cookie = xcb_intern_atom(x11.connection, false, strlen(name), name);
	xcb_intern_atom_reply_t *reply = xcb_intern_atom_reply(x11.connection, cookie, NULL);
	xcb_atom_t atom = reply ? reply->atom : XCB_ATOM_NONE;
	free(reply);
	return atom;
}

static void set_root_cardinals(enum ewmh_atom property, uint32_t count, const uint32_t *values) {
	xcb_change_property(x11.connection, XCB_PROP_MODE_REPLACE, x11.root,
		x11.ewmh[property], XCB_ATOM_CARDINAL, 32, count, values);
}

static int usable_top(void) {
	return x11.reserve_top < x11.height ? x11.reserve_top : x11.height - 1;
}

static int read_reserve_top(void) {
	const char *value = getenv("WHIPLASH_X11_RESERVE_TOP");
	if (!value || !*value) {
		return 0;
	}
	char *end;
	errno = 0;
	long top = strtol(value, &end, 10);
	if (errno || *end || top < 0 || top > UINT16_MAX) {
		fprintf(stderr, "whiplash: invalid WHIPLASH_X11_RESERVE_TOP: %s\n", value);
		return 0;
	}
	return top;
}

static void publish_desktop_geometry(void) {
	uint32_t size[] = {x11.width, x11.height};
	set_root_cardinals(NET_DESKTOP_GEOMETRY, 2, size);
	uint32_t count = x11.workspace_count;
	if (!count) {
		return;
	}
	uint32_t *rects = calloc(count, 4 * sizeof(*rects));
	if (!rects) {
		return;
	}
	set_root_cardinals(NET_DESKTOP_VIEWPORT, 2 * count, rects);
	int top = usable_top();
	for (uint32_t i = 0; i < count; i++) {
		rects[4 * i + 1] = top;
		rects[4 * i + 2] = x11.width;
		rects[4 * i + 3] = x11.height - top;
	}
	set_root_cardinals(NET_WORKAREA, 4 * count, rects);
	free(rects);
}

static bool init_ewmh(void) {
	for (unsigned i = 0; i < EWMH_COUNT; i++) {
		x11.ewmh[i] = intern_atom(ewmh_names[i]);
		if (!x11.ewmh[i]) {
			return false;
		}
	}
	x11.current_desktop = UINT32_MAX;
	x11.wm_check = xcb_generate_id(x11.connection);
	xcb_create_window(x11.connection, XCB_COPY_FROM_PARENT, x11.wm_check, x11.root,
		0, 0, 1, 1, 0, XCB_WINDOW_CLASS_INPUT_OUTPUT, XCB_COPY_FROM_PARENT, 0, NULL);
	const char wm_name[] = "Whiplash";
	xcb_change_property(x11.connection, XCB_PROP_MODE_REPLACE, x11.wm_check,
		x11.ewmh[NET_WM_NAME], x11.ewmh[UTF8_STRING], 8, sizeof(wm_name) - 1, wm_name);
	xcb_window_t check_windows[] = {x11.wm_check, x11.root};
	for (unsigned i = 0; i < 2; i++) {
		xcb_change_property(x11.connection, XCB_PROP_MODE_REPLACE, check_windows[i],
			x11.ewmh[NET_SUPPORTING_WM_CHECK], XCB_ATOM_WINDOW, 32, 1, &x11.wm_check);
	}
	xcb_change_property(x11.connection, XCB_PROP_MODE_REPLACE, x11.root,
		x11.ewmh[NET_SUPPORTED], XCB_ATOM_ATOM, 32, UTF8_STRING, x11.ewmh);
	return true;
}

void wh_x11_add_workspace(int tag, const char *name) {
	char *copy = strdup(name);
	if (!copy) {
		return;
	}
	struct x11_workspace *ws = realloc(x11.workspaces, (x11.workspace_count + 1) * sizeof(*ws));
	if (!ws) {
		free(copy);
		return;
	}
	x11.workspaces = ws;
	ws[x11.workspace_count++] = (struct x11_workspace){.tag = tag, .name = copy};
	x11.workspace_counts_dirty = true;
	x11.workspace_urgency_dirty = true;
}

static void publish_workspace_list(void) {
	size_t length = 0;
	for (uint32_t i = 0; i < x11.workspace_count; i++) {
		length += strlen(x11.workspaces[i].name) + 1;
	}
	char *names = malloc(length);
	if (!names) {
		return;
	}
	char *end = names;
	for (uint32_t i = 0; i < x11.workspace_count; i++) {
		const char *name = x11.workspaces[i].name;
		size_t n = strlen(name) + 1;
		memcpy(end, name, n);
		end += n;
	}
	xcb_change_property(x11.connection, XCB_PROP_MODE_REPLACE, x11.root,
		x11.ewmh[NET_DESKTOP_NAMES], x11.ewmh[UTF8_STRING], 8, length, names);
	free(names);
	publish_desktop_geometry();
	set_root_cardinals(NET_NUMBER_OF_DESKTOPS, 1, &x11.workspace_count);
	x11.published_count = x11.workspace_count;
}

void wh_x11_set_active_workspace(int tag) {
	if (x11.workspace_count != x11.published_count) {
		publish_workspace_list();
	}
	for (uint32_t i = 0; i < x11.workspace_count; i++) {
		if (x11.workspaces[i].tag == tag && i != x11.current_desktop) {
			x11.current_desktop = i;
			set_root_cardinals(NET_CURRENT_DESKTOP, 1, &i);
			xcb_flush(x11.connection);
			return;
		}
	}
}

void wh_x11_set_workspace_urgent(int tag, int enabled) {
	for (uint32_t i = 0; i < x11.workspace_count; i++) {
		struct x11_workspace *ws = &x11.workspaces[i];
		if (ws->tag == tag) {
			bool urgent = enabled != 0;
			x11.workspace_urgency_dirty |= ws->urgent != urgent;
			ws->urgent = urgent;
			return;
		}
	}
}

void wh_x11_set_workspace_counts(const int *pairs, unsigned count) {
	if (!x11.workspace_count) {
		return;
	}
	for (uint32_t i = 0; i < x11.workspace_count; i++) {
		struct x11_workspace *ws = &x11.workspaces[i];
		for (unsigned j = 0; j < count; j++) {
			if (pairs[2 * j] == ws->tag) {
				uint32_t windows = pairs[2 * j + 1];
				x11.workspace_counts_dirty |= ws->window_count != windows;
				ws->window_count = windows;
				break;
			}
		}
	}
	if (!x11.workspace_counts_dirty && !x11.workspace_urgency_dirty) {
		return;
	}
	uint32_t *values = calloc(x11.workspace_count, sizeof(*values));
	if (!values) {
		return;
	}
	if (x11.workspace_counts_dirty) {
		for (uint32_t i = 0; i < x11.workspace_count; i++) {
			values[i] = x11.workspaces[i].window_count;
		}
		set_root_cardinals(D77_WORKSPACE_COUNTS, x11.workspace_count, values);
		x11.workspace_counts_dirty = false;
	}
	if (x11.workspace_urgency_dirty) {
		for (uint32_t i = 0; i < x11.workspace_count; i++) {
			values[i] = x11.workspaces[i].urgent;
		}
		set_root_cardinals(D77_WORKSPACE_URGENT, x11.workspace_count, values);
		x11.workspace_urgency_dirty = false;
	}
	free(values);
	xcb_flush(x11.connection);
}

static void set_wm_state(xcb_window_t id, uint32_t state) {
	uint32_t values[] = {state, XCB_NONE};
	xcb_change_property(x11.connection, XCB_PROP_MODE_REPLACE, id,
		x11.wm_state, x11.wm_state, 32, 2, values);
}

/* Intercept clicks on unfocused clients, leaving wheel events and clicks
 * on the focused client to the application. */
static void grab_focus_clicks(xcb_window_t id) {
	for (uint8_t button = XCB_BUTTON_INDEX_1; button <= XCB_BUTTON_INDEX_3; button++) {
		xcb_grab_button(x11.connection, false, id, XCB_EVENT_MASK_BUTTON_PRESS,
			XCB_GRAB_MODE_SYNC, XCB_GRAB_MODE_ASYNC, XCB_NONE, XCB_NONE,
			button, XCB_MOD_MASK_ANY);
	}
}

static void ungrab_focus_clicks(xcb_window_t id) {
	for (uint8_t button = XCB_BUTTON_INDEX_1; button <= XCB_BUTTON_INDEX_3; button++) {
		xcb_ungrab_button(x11.connection, button, id, XCB_MOD_MASK_ANY);
	}
}

static void grab_drag_buttons(void) {
	xcb_ungrab_button(x11.connection, XCB_BUTTON_INDEX_ANY, x11.root, XCB_MOD_MASK_ANY);
	if (x11.mouse_mod) {
		for (uint16_t locks = x11.lock_mask;; locks = (locks - 1) & x11.lock_mask) {
			const uint8_t buttons[] = {XCB_BUTTON_INDEX_1, XCB_BUTTON_INDEX_3};
			const uint16_t events = XCB_EVENT_MASK_BUTTON_PRESS | XCB_EVENT_MASK_BUTTON_RELEASE |
				XCB_EVENT_MASK_POINTER_MOTION;
			for (size_t i = 0; i < sizeof(buttons) / sizeof(buttons[0]); i++) {
				xcb_grab_button(x11.connection, false, x11.root, events,
					XCB_GRAB_MODE_SYNC, XCB_GRAB_MODE_ASYNC, XCB_NONE, XCB_NONE,
					buttons[i], (x11.mouse_mod & ~x11.lock_mask) | locks);
			}
			if (!locks) {
				break;
			}
		}
	}
	xcb_flush(x11.connection);
}

void wh_x11_set_mouse_mod(uint32_t mods) {
	if (x11.mouse_mod != mods) {
		x11.mouse_mod = mods;
		if (x11.connection) {
			grab_drag_buttons();
		}
	}
}

static void cancel_drag(wh_view_id id) {
	if (x11.drag.id == id) {
		x11.drag.id = XCB_NONE;
		xcb_ungrab_pointer(x11.connection, XCB_CURRENT_TIME);
	}
}

static bool is_atom_list(const xcb_get_property_reply_t *reply) {
	return reply && reply->type == XCB_ATOM_ATOM && reply->format == 32;
}

static bool has_atom_hint(const xcb_get_property_reply_t *reply, enum ewmh_atom first,
		enum ewmh_atom last) {
	if (is_atom_list(reply)) {
		const xcb_atom_t *atoms = xcb_get_property_value(reply);
		for (uint32_t i = 0; i < reply->value_len; i++) {
			for (enum ewmh_atom hint = first; hint <= last; hint++) {
				if (atoms[i] == x11.ewmh[hint]) {
					return true;
				}
			}
		}
	}
	return false;
}

static void set_state_hint(struct x11_view *view, enum ewmh_atom hint, bool *current, bool enabled) {
	if (*current == enabled) {
		return;
	}
	xcb_atom_t property = x11.ewmh[NET_WM_STATE], atom = x11.ewmh[hint];
	xcb_get_property_cookie_t cookie = xcb_get_property(x11.connection, false, view->id,
		property, XCB_ATOM_ATOM, 0, UINT32_MAX);
	xcb_get_property_reply_t *reply = xcb_get_property_reply(x11.connection, cookie, NULL);
	bool present = has_atom_hint(reply, hint, hint);
	if (enabled && !present) {
		uint8_t mode = is_atom_list(reply) ? XCB_PROP_MODE_APPEND : XCB_PROP_MODE_REPLACE;
		xcb_change_property(x11.connection, mode, view->id, property, XCB_ATOM_ATOM, 32, 1, &atom);
	} else if (!enabled && present) {
		xcb_atom_t *atoms = xcb_get_property_value(reply);
		uint32_t kept = 0;
		for (uint32_t i = 0; i < reply->value_len; i++) {
			if (atoms[i] != atom) {
				atoms[kept++] = atoms[i];
			}
		}
		xcb_change_property(x11.connection, XCB_PROP_MODE_REPLACE, view->id,
			property, XCB_ATOM_ATOM, 32, kept, atoms);
	}
	free(reply);
	*current = enabled;
}

static bool read_urgency_hint(xcb_get_property_cookie_t cookie) {
	xcb_icccm_wm_hints_t hints;
	return xcb_icccm_get_wm_hints_reply(x11.connection, cookie, &hints, NULL) &&
		xcb_icccm_wm_hints_get_urgency(&hints);
}

static bool view_is_urgent(const struct x11_view *view) {
	return view->urgency_hint || view->demands_attention;
}

static void notify_urgency_change(struct x11_view *view, bool previous) {
	bool urgent = view_is_urgent(view);
	if (previous != urgent && x11.callbacks.urgent) {
		x11.callbacks.urgent(view->id, urgent);
	}
}

static void read_window_properties(struct x11_view *view) {
	xcb_window_t id = view->id;
	xcb_get_geometry_cookie_t geom = xcb_get_geometry(x11.connection, id);
	xcb_get_property_cookie_t class = xcb_icccm_get_wm_class(x11.connection, id);
	xcb_get_property_cookie_t transient = xcb_icccm_get_wm_transient_for(x11.connection, id);
	xcb_get_property_cookie_t normal = xcb_icccm_get_wm_normal_hints(x11.connection, id);
	xcb_get_property_cookie_t hints_cookie = xcb_icccm_get_wm_hints(x11.connection, id);
	xcb_get_property_cookie_t types = xcb_get_property(x11.connection, false, id,
		x11.ewmh[NET_WM_WINDOW_TYPE], XCB_ATOM_ATOM, 0, 32);
	xcb_get_property_cookie_t states = xcb_get_property(x11.connection, false, id,
		x11.ewmh[NET_WM_STATE], XCB_ATOM_ATOM, 0, UINT32_MAX);
	xcb_get_geometry_reply_t *geometry = xcb_get_geometry_reply(x11.connection, geom, NULL);
	if (geometry) {
		view->natural_width = geometry->width;
		view->natural_height = geometry->height;
		free(geometry);
	}
	xcb_icccm_get_wm_class_reply_t wm_class;
	if (xcb_icccm_get_wm_class_reply(x11.connection, class, &wm_class, NULL)) {
		view->app_id = strdup(wm_class.class_name);
		xcb_icccm_get_wm_class_reply_wipe(&wm_class);
	}
	xcb_window_t parent;
	bool has_parent = xcb_icccm_get_wm_transient_for_reply(x11.connection, transient,
		&parent, NULL) && parent != XCB_NONE && parent != id;
	xcb_size_hints_t hints;
	uint32_t size_flags = XCB_ICCCM_SIZE_HINT_P_MIN_SIZE | XCB_ICCCM_SIZE_HINT_P_MAX_SIZE;
	bool fixed_size = xcb_icccm_get_wm_normal_hints_reply(x11.connection, normal, &hints, NULL) &&
		(hints.flags & size_flags) == size_flags &&
		hints.min_width > 0 && hints.min_height > 0 &&
		hints.min_width == hints.max_width && hints.min_height == hints.max_height;
	view->urgency_hint = read_urgency_hint(hints_cookie);
	xcb_get_property_reply_t *type_reply = xcb_get_property_reply(x11.connection, types, NULL);
	xcb_get_property_reply_t *state_reply = xcb_get_property_reply(x11.connection, states, NULL);
	bool floating_type = has_atom_hint(type_reply, NET_WM_WINDOW_TYPE_DIALOG, NET_WM_WINDOW_TYPE_TOOLBAR);
	bool modal = has_atom_hint(state_reply, NET_WM_STATE_MODAL, NET_WM_STATE_MODAL);
	view->fullscreen_hint = has_atom_hint(state_reply, NET_WM_STATE_FULLSCREEN, NET_WM_STATE_FULLSCREEN);
	view->demands_attention = has_atom_hint(state_reply, NET_WM_STATE_DEMANDS_ATTENTION,
		NET_WM_STATE_DEMANDS_ATTENTION);
	view->wants_float = has_parent || fixed_size || floating_type || modal;
	free(type_reply);
	free(state_reply);
	if (fixed_size) {
		view->natural_width = hints.min_width;
		view->natural_height = hints.min_height;
	}
}

static void manage(xcb_window_t id, bool existing) {
	if (find_view(id)) {
		return;
	}
	xcb_get_window_attributes_reply_t *attr = xcb_get_window_attributes_reply(
		x11.connection, xcb_get_window_attributes(x11.connection, id), NULL);
	if (!attr) {
		return;
	}
	bool mapped = attr->map_state == XCB_MAP_STATE_VIEWABLE;
	bool normal = !attr->override_redirect && attr->_class == XCB_WINDOW_CLASS_INPUT_OUTPUT;
	free(attr);
	if (!normal || (existing && !mapped)) {
		if (!existing) {
			xcb_map_window(x11.connection, id);
		}
		return;
	}
	struct x11_view *view = calloc(1, sizeof(*view));
	if (!view) {
		xcb_map_window(x11.connection, id);
		return;
	}
	view->id = id;
	view->mapped = mapped;
	read_window_properties(view);
	view->next = x11.views;
	x11.views = view;
	set_wm_state(id, mapped ? XCB_ICCCM_WM_STATE_NORMAL : XCB_ICCCM_WM_STATE_ICONIC);
	uint32_t events = XCB_EVENT_MASK_ENTER_WINDOW | XCB_EVENT_MASK_LEAVE_WINDOW |
		XCB_EVENT_MASK_PROPERTY_CHANGE;
	xcb_change_window_attributes(x11.connection, id, XCB_CW_EVENT_MASK, &events);
	grab_focus_clicks(id);
	/* Seed the Haskell state before the first placement to avoid a tiled frame. */
	if (view->fullscreen_hint && x11.callbacks.fullscreen) {
		x11.callbacks.fullscreen(id, true);
	}
	if (x11.callbacks.mapped) {
		x11.callbacks.mapped(id);
	}
	notify_urgency_change(view, false);
}

static void unmanage(xcb_window_t id, bool destroyed) {
	struct x11_view **link = &x11.views;
	while (*link && (*link)->id != id) {
		link = &(*link)->next;
	}
	struct x11_view *view = *link;
	if (!view) {
		return;
	}
	cancel_drag(id);
	*link = view->next;
	if (id == x11.focused) {
		x11.focused = XCB_NONE;
	}
	free(view->app_id);
	free(view);
	if (!destroyed) {
		uint32_t mask = XCB_EVENT_MASK_NO_EVENT;
		xcb_change_window_attributes(x11.connection, id, XCB_CW_EVENT_MASK, &mask);
		ungrab_focus_clicks(id);
		xcb_delete_property(x11.connection, id, x11.wm_state);
		xcb_delete_property(x11.connection, id, x11.ewmh[NET_WM_STATE]);
	}
	if (x11.callbacks.unmapped) {
		x11.callbacks.unmapped(id);
	}
}

static xcb_keysym_t key_symbol(xcb_keycode_t code) {
	if (!x11.keymap || !x11.keymap->keysyms_per_keycode ||
			code < x11.first_keycode || code > x11.last_keycode) {
		return XKB_KEY_NoSymbol;
	}
	return xcb_get_keyboard_mapping_keysyms(x11.keymap)
		[(code - x11.first_keycode) * x11.keymap->keysyms_per_keycode];
}

static void grab_keys(void) {
	xcb_ungrab_key(x11.connection, XCB_GRAB_ANY, x11.root, XCB_MOD_MASK_ANY);
	for (unsigned key = x11.first_keycode; key <= x11.last_keycode; key++) {
		xcb_keysym_t sym = key_symbol(key);
		if (sym == XKB_KEY_NoSymbol) {
			continue;
		}
		for (unsigned i = 0; i < x11.binding_count; i++) {
			uint32_t mods = x11.bindings[2 * i];
			if (sym != x11.bindings[2 * i + 1] || (mods & x11.lock_mask)) {
				continue;
			}
			/* Grab every combination of the ignored lock modifiers. */
			for (uint16_t locks = x11.lock_mask;; locks = (locks - 1) & x11.lock_mask) {
				xcb_grab_key(x11.connection, false, x11.root, mods | locks,
					key, XCB_GRAB_MODE_ASYNC, XCB_GRAB_MODE_ASYNC);
				if (!locks) {
					break;
				}
			}
		}
	}
	xcb_flush(x11.connection);
}

static void update_keymap(void) {
	xcb_get_keyboard_mapping_cookie_t keys = xcb_get_keyboard_mapping(x11.connection,
		x11.first_keycode, x11.last_keycode - x11.first_keycode + 1);
	xcb_get_modifier_mapping_cookie_t modifiers = xcb_get_modifier_mapping(x11.connection);

	free(x11.keymap);
	x11.keymap = xcb_get_keyboard_mapping_reply(x11.connection, keys, NULL);
	x11.lock_mask = XCB_MOD_MASK_LOCK;
	xcb_get_modifier_mapping_reply_t *map = xcb_get_modifier_mapping_reply(
		x11.connection, modifiers, NULL);
	if (map) {
		xcb_keycode_t *codes = xcb_get_modifier_mapping_keycodes(map);
		for (int mod = 0; mod < 8; mod++) {
			for (int k = 0; k < map->keycodes_per_modifier; k++) {
				if (key_symbol(codes[mod * map->keycodes_per_modifier + k]) == XKB_KEY_Num_Lock) {
					x11.lock_mask |= 1 << mod;
				}
			}
		}
		free(map);
	}
	grab_keys();
	grab_drag_buttons();
}

void wh_x11_set_bindings(const uint32_t *pairs, unsigned count) {
	uint32_t *bindings = NULL;
	if (count) {
		bindings = malloc(2 * (size_t)count * sizeof(*bindings));
		if (!bindings) {
			fprintf(stderr, "whiplash: cannot allocate X11 key bindings\n");
			return;
		}
		memcpy(bindings, pairs, 2 * (size_t)count * sizeof(*bindings));
	}
	free(x11.bindings);
	x11.bindings = bindings;
	x11.binding_count = count;
	grab_keys();
}

static void configure_request(const xcb_configure_request_event_t *event) {
	if (find_view(event->window)) {
		/* The Haskell layout owns the geometry. Report the current geometry
		 * when a managed client asks us to change it. */
		xcb_get_geometry_reply_t *geometry = xcb_get_geometry_reply(x11.connection,
			xcb_get_geometry(x11.connection, event->window), NULL);
		if (geometry) {
			union {
				char wire[32];
				xcb_configure_notify_event_t event;
			} reply = {.event = {
				.response_type = XCB_CONFIGURE_NOTIFY,
				.event = event->window,
				.window = event->window,
				.x = geometry->x,
				.y = geometry->y,
				.width = geometry->width,
				.height = geometry->height,
				.border_width = geometry->border_width,
			}};
			xcb_send_event(x11.connection, false, event->window,
				XCB_EVENT_MASK_STRUCTURE_NOTIFY, reply.wire);
			free(geometry);
		}
		return;
	}
	uint32_t requested[] = {event->x, event->y, event->width, event->height,
		event->border_width, event->sibling, event->stack_mode};
	uint32_t values[7];
	unsigned len = 0;
	for (unsigned i = 0; i < 7; i++) {
		if (event->value_mask & (1u << i)) {
			values[len++] = requested[i];
		}
	}
	xcb_configure_window(x11.connection, event->window, event->value_mask, values);
}

enum ewmh_state_action { STATE_REMOVE, STATE_ADD, STATE_TOGGLE };

static bool requested_state(uint32_t action, bool present) {
	return action == STATE_TOGGLE ? !present : action == STATE_ADD;
}

static void state_request(const xcb_client_message_event_t *message) {
	struct x11_view *view = find_view(message->window);
	uint32_t action = message->data.data32[0];
	if (!view || action > STATE_TOGGLE) {
		return;
	}
	xcb_atom_t first = message->data.data32[1], second = message->data.data32[2];
	xcb_atom_t fullscreen_atom = x11.ewmh[NET_WM_STATE_FULLSCREEN];
	xcb_atom_t attention_atom = x11.ewmh[NET_WM_STATE_DEMANDS_ATTENTION];
	if (x11.callbacks.fullscreen && (first == fullscreen_atom || second == fullscreen_atom)) {
		bool fullscreen = requested_state(action, view->fullscreen_hint);
		if (!view->mapped) {
			/* Geometry is applied when this workspace is shown. */
			set_state_hint(view, NET_WM_STATE_FULLSCREEN, &view->fullscreen_hint, fullscreen);
		}
		x11.callbacks.fullscreen(view->id, fullscreen);
	}
	if (first == attention_atom || second == attention_atom) {
		bool previous = view_is_urgent(view);
		bool attention = view->id != x11.focused && requested_state(action, view->demands_attention);
		set_state_hint(view, NET_WM_STATE_DEMANDS_ATTENTION, &view->demands_attention, attention);
		notify_urgency_change(view, previous);
	}
}

static void configure_view(const struct x11_view *view, struct x11_rect rect);

static bool begin_drag(const xcb_button_press_event_t *button) {
	struct x11_view *view = find_view(button->child);
	if (!view || !view->mapped || view->mode == WH_VIEW_FULLSCREEN || !x11.callbacks.grab) {
		return false;
	}
	xcb_get_geometry_reply_t *geometry = xcb_get_geometry_reply(x11.connection,
		xcb_get_geometry(x11.connection, view->id), NULL);
	if (!geometry) {
		return false;
	}
	struct x11_rect start = {
		geometry->x, geometry->y,
		geometry->width + 2 * geometry->border_width,
		geometry->height + 2 * geometry->border_width,
	};
	free(geometry);
	x11.callbacks.grab(view->id, start.x, start.y, start.width, start.height);
	if (view->mode != WH_VIEW_FLOATING) {
		return false;
	}
	x11.drag = (struct x11_drag){
		.id = view->id,
		.button = button->detail,
		.pointer_x = button->root_x,
		.pointer_y = button->root_y,
		.start = start,
	};
	return true;
}

static int clamp(int value, int min, int max) {
	return value < min ? min : value > max ? max : value;
}

static struct x11_rect drag_geometry(int root_x, int root_y) {
	struct x11_rect rect = x11.drag.start;
	int dx = root_x - x11.drag.pointer_x;
	int dy = root_y - x11.drag.pointer_y;
	if (x11.drag.button == XCB_BUTTON_INDEX_1) {
		rect.x = clamp(rect.x + dx, INT16_MIN, INT16_MAX);
		rect.y = clamp(rect.y + dy, INT16_MIN, INT16_MAX);
	} else {
		rect.width = clamp(rect.width + dx, 64, UINT16_MAX);
		rect.height = clamp(rect.height + dy, 48, UINT16_MAX);
	}
	return rect;
}

static void drag_motion(int root_x, int root_y) {
	struct x11_view *view = find_view(x11.drag.id);
	if (view) {
		configure_view(view, drag_geometry(root_x, root_y));
	}
}

static void end_drag(const xcb_button_release_event_t *button) {
	wh_view_id id = x11.drag.id;
	if (!id || button->detail != x11.drag.button) {
		return;
	}
	struct x11_rect rect = drag_geometry(button->root_x, button->root_y);
	drag_motion(button->root_x, button->root_y);
	cancel_drag(id);
	if (x11.callbacks.moved) {
		x11.callbacks.moved(id, rect.x, rect.y, rect.width, rect.height);
	}
}

static void dispatch(xcb_generic_event_t *event) {
	switch (event->response_type & 0x7f) {
	case XCB_MAP_REQUEST:
		manage(((xcb_map_request_event_t *)event)->window, false);
		break;
	case XCB_UNMAP_NOTIFY: {
		xcb_unmap_notify_event_t *unmap = (void *)event;
		struct x11_view *view = find_view(unmap->window);
		if (view && unmap->event == x11.root) {
			/* A synthetic ICCCM withdrawal counts even for a hidden client. */
			if (!(event->response_type & 0x80) && view->pending_unmaps) {
				view->pending_unmaps--;
			} else {
				unmanage(view->id, false);
			}
		}
		break;
	}
	case XCB_DESTROY_NOTIFY:
		unmanage(((xcb_destroy_notify_event_t *)event)->window, true);
		break;
	case XCB_CONFIGURE_REQUEST:
		configure_request((void *)event);
		break;
	case XCB_CONFIGURE_NOTIFY: {
		xcb_configure_notify_event_t *configure = (void *)event;
		if (configure->window == x11.root &&
				(configure->width != x11.width || configure->height != x11.height)) {
			x11.width = configure->width;
			x11.height = configure->height;
			publish_desktop_geometry();
			if (x11.callbacks.outputs_changed) {
				x11.callbacks.outputs_changed();
			}
		}
		break;
	}
	case XCB_CLIENT_MESSAGE: {
		xcb_client_message_event_t *message = (void *)event;
		if (message->format != 32) {
			break;
		}
		if (message->type == x11.ewmh[NET_WM_STATE]) {
			state_request(message);
		} else if (message->window == x11.root && message->type == x11.ewmh[NET_CURRENT_DESKTOP] &&
				message->data.data32[0] < x11.workspace_count &&
				x11.callbacks.workspace_activate) {
			x11.callbacks.workspace_activate(x11.workspaces[message->data.data32[0]].tag);
		}
		break;
	}
	case XCB_PROPERTY_NOTIFY: {
		xcb_property_notify_event_t *property = (void *)event;
		struct x11_view *view = property->atom == XCB_ATOM_WM_HINTS ? find_view(property->window) : NULL;
		if (view) {
			bool previous = view_is_urgent(view);
			view->urgency_hint = read_urgency_hint(xcb_icccm_get_wm_hints(x11.connection, view->id));
			notify_urgency_change(view, previous);
		}
		break;
	}
	case XCB_KEY_PRESS: {
		xcb_key_press_event_t *key = (void *)event;
		if (x11.callbacks.key) {
			x11.callbacks.key((key->state & 0xff) & ~x11.lock_mask, key_symbol(key->detail));
		}
		break;
	}
	case XCB_BUTTON_PRESS: {
		xcb_button_press_event_t *button = (void *)event;
		if (x11.drag.id) {
			break;
		}
		if (button->event == x11.root && begin_drag(button)) {
			xcb_allow_events(x11.connection, XCB_ALLOW_ASYNC_POINTER, button->time);
			break;
		}
		struct x11_view *view = find_view(button->event);
		if (view && view->mapped && x11.callbacks.clicked) {
			x11.callbacks.clicked(view->id);
		}
		/* Deliver the original click to the application after focusing it. */
		xcb_allow_events(x11.connection, XCB_ALLOW_REPLAY_POINTER, button->time);
		break;
	}
	case XCB_BUTTON_RELEASE:
		end_drag((void *)event);
		break;
	case XCB_MOTION_NOTIFY: {
		xcb_motion_notify_event_t *motion = (void *)event;
		if (x11.drag.id) {
			drag_motion(motion->root_x, motion->root_y);
		}
		break;
	}
	case XCB_ENTER_NOTIFY:
		pointer_crossed((void *)event, true);
		break;
	case XCB_LEAVE_NOTIFY:
		pointer_crossed((void *)event, false);
		break;
	case XCB_MAPPING_NOTIFY: {
		xcb_mapping_notify_event_t *mapping = (void *)event;
		if (mapping->request != XCB_MAPPING_POINTER) {
			update_keymap();
		}
		break;
	}
	default:
		break;
	}
}

static int events_ready(int fd, uint32_t mask, void *data) {
	xcb_generic_event_t *event;
	while ((event = xcb_poll_for_event(x11.connection))) {
		dispatch(event);
		free(event);
	}
	xcb_flush(x11.connection);
	if (xcb_connection_has_error(x11.connection)) {
		wl_display_terminate(x11.display);
	}
	return 0;
}

int wh_x11_init(struct wl_display *display, struct wh_x11_callbacks callbacks) {
	int screen_number;
	x11.connection = xcb_connect(NULL, &screen_number);
	if (xcb_connection_has_error(x11.connection)) {
		fprintf(stderr, "whiplash: cannot connect to the X11 display\n");
		goto failed;
	}
	const xcb_setup_t *setup = xcb_get_setup(x11.connection);
	xcb_screen_iterator_t screens = xcb_setup_roots_iterator(setup);
	for (int i = 0; i < screen_number && screens.rem; i++) {
		xcb_screen_next(&screens);
	}
	if (!screens.rem) {
		goto failed;
	}
	x11.root = screens.data->root;
	x11.width = screens.data->width_in_pixels;
	x11.height = screens.data->height_in_pixels;
	x11.reserve_top = read_reserve_top();

	uint32_t mask = XCB_EVENT_MASK_SUBSTRUCTURE_REDIRECT |
		XCB_EVENT_MASK_SUBSTRUCTURE_NOTIFY | XCB_EVENT_MASK_STRUCTURE_NOTIFY;
	xcb_generic_error_t *error = xcb_request_check(x11.connection,
		xcb_change_window_attributes_checked(x11.connection, x11.root, XCB_CW_EVENT_MASK, &mask));
	if (error) {
		if (error->error_code == XCB_ACCESS) {
			fprintf(stderr, "whiplash: another window manager already controls the X11 display\n");
		} else {
			fprintf(stderr, "whiplash: cannot select X11 root events (error %d)\n", error->error_code);
		}
		free(error);
		goto failed;
	}

	x11.wm_state = intern_atom("WM_STATE");
	x11.wm_protocols = intern_atom("WM_PROTOCOLS");
	x11.wm_delete_window = intern_atom("WM_DELETE_WINDOW");
	if (!x11.wm_state || !x11.wm_protocols || !x11.wm_delete_window) {
		goto failed;
	}
	x11.display = display;
	x11.callbacks = callbacks;
	x11.events = wl_event_loop_add_fd(wl_display_get_event_loop(display),
		xcb_get_file_descriptor(x11.connection), WL_EVENT_READABLE, events_ready, NULL);
	if (!x11.events) {
		goto failed;
	}
	if (!init_ewmh()) {
		wl_event_source_remove(x11.events);
		goto failed;
	}
	/* A synchronous XCB reply can already have pulled events off the fd. */
	wl_event_source_check(x11.events);
	x11.first_keycode = setup->min_keycode;
	x11.last_keycode = setup->max_keycode;
	update_keymap();
	if (x11.callbacks.outputs_changed) {
		x11.callbacks.outputs_changed();
	}
	xcb_query_tree_reply_t *tree = xcb_query_tree_reply(x11.connection,
		xcb_query_tree(x11.connection, x11.root), NULL);
	if (tree) {
		xcb_window_t *children = xcb_query_tree_children(tree);
		for (int i = 0; i < xcb_query_tree_children_length(tree); i++) {
			manage(children[i], true);
		}
		free(tree);
	}
	xcb_flush(x11.connection);
	fprintf(stderr, "whiplash: managing the X11 display\n");
	return 0;

failed:
	xcb_disconnect(x11.connection);
	x11.connection = NULL;
	return 1;
}

void wh_x11_shutdown(void) {
	wl_event_source_remove(x11.events);
	xcb_delete_property(x11.connection, x11.root, x11.ewmh[D77_WORKSPACE_COUNTS]);
	xcb_delete_property(x11.connection, x11.root, x11.ewmh[D77_WORKSPACE_URGENT]);
	for (unsigned i = 0; i < NET_WM_NAME; i++) {
		xcb_delete_property(x11.connection, x11.root, x11.ewmh[i]);
	}
	for (uint32_t i = 0; i < x11.workspace_count; i++) {
		free(x11.workspaces[i].name);
	}
	free(x11.workspaces);
	while (x11.views) {
		struct x11_view *view = x11.views;
		x11.views = view->next;
		/* Leave clients accessible to the next WM. */
		xcb_map_window(x11.connection, view->id);
		xcb_delete_property(x11.connection, view->id, x11.wm_state);
		free(view->app_id);
		free(view);
	}
	free(x11.keymap);
	free(x11.bindings);
	/* Wait for the server to apply property cleanup and remap hidden clients
	 * before closing the connection. */
	free(xcb_get_input_focus_reply(x11.connection, xcb_get_input_focus(x11.connection), NULL));
	xcb_disconnect(x11.connection);
}

int wh_x11_output_info(int index, int *out) {
	if (index != 0) {
		return -1;
	}
	int rect[] = {0, 0, x11.width, x11.height};
	memcpy(out, rect, sizeof(rect));
	int top = usable_top();
	int usable[] = {0, top, x11.width, x11.height - top};
	memcpy(out + 4, usable, sizeof(usable));
	return 0;
}

void wh_x11_set_border(int width, uint32_t focused, uint32_t normal) {
	x11.border_width = width < 0 ? 0 : width > UINT16_MAX / 2 ? UINT16_MAX / 2 : width;
	x11.focused_border = focused >> 8;
	x11.normal_border = normal >> 8;
}

static void raise_view(struct x11_view *view) {
	/* Raise within the mode's layer so a tiled parent cannot cover its dialog. */
	xcb_window_t sibling = XCB_NONE;
	for (struct x11_view *other = x11.views; other; other = other->next) {
		if (other->mapped && other->mode > view->mode) {
			sibling = other->id;
			break;
		}
	}
	if (sibling) {
		xcb_query_tree_reply_t *tree = xcb_query_tree_reply(x11.connection,
			xcb_query_tree(x11.connection, x11.root), NULL);
		if (tree) {
			/* Children are ordered bottom to top. Pick the lowest higher layer. */
			xcb_window_t *children = xcb_query_tree_children(tree);
			for (int i = 0; i < xcb_query_tree_children_length(tree); i++) {
				struct x11_view *other = find_view(children[i]);
				if (other && other->mapped && other->mode > view->mode) {
					sibling = other->id;
					break;
				}
			}
			free(tree);
		}
		uint32_t values[] = {sibling, XCB_STACK_MODE_BELOW};
		xcb_configure_window(x11.connection, view->id,
			XCB_CONFIG_WINDOW_SIBLING | XCB_CONFIG_WINDOW_STACK_MODE, values);
	} else {
		uint32_t above = XCB_STACK_MODE_ABOVE;
		xcb_configure_window(x11.connection, view->id, XCB_CONFIG_WINDOW_STACK_MODE, &above);
	}
}

void wh_x11_set_mode(wh_view_id id, int mode) {
	struct x11_view *view = find_view(id);
	if (!view) {
		return;
	}
	set_state_hint(view, NET_WM_STATE_FULLSCREEN, &view->fullscreen_hint, mode == WH_VIEW_FULLSCREEN);
	if ((int)view->mode != mode) {
		if (mode != WH_VIEW_FLOATING) {
			cancel_drag(id);
		}
		view->mode = mode;
		if (view->mapped) {
			raise_view(view);
		}
	}
}

static void configure_view(const struct x11_view *view, struct x11_rect rect) {
	int border = view->mode == WH_VIEW_FULLSCREEN ? 0 : x11.border_width;
	uint32_t values[] = {rect.x, rect.y, rect.width > border * 2 ? rect.width - border * 2 : 1,
		rect.height > border * 2 ? rect.height - border * 2 : 1, border};
	xcb_configure_window(x11.connection, view->id, XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y |
		XCB_CONFIG_WINDOW_WIDTH | XCB_CONFIG_WINDOW_HEIGHT | XCB_CONFIG_WINDOW_BORDER_WIDTH, values);
}

void wh_x11_set_geometry(wh_view_id id, int x, int y, int width, int height) {
	struct x11_view *view = find_view(id);
	if (view && x11.drag.id != id) {
		configure_view(view, (struct x11_rect){x, y, width, height});
	}
}

void wh_x11_set_visible(wh_view_id id, int visible) {
	struct x11_view *view = find_view(id);
	if (!view || view->mapped == (visible != 0)) {
		return;
	}
	if (visible) {
		raise_view(view);
		xcb_map_window(x11.connection, id);
		set_wm_state(id, XCB_ICCCM_WM_STATE_NORMAL);
	} else {
		cancel_drag(id);
		view->pending_unmaps++;
		xcb_unmap_window(x11.connection, id);
		set_wm_state(id, XCB_ICCCM_WM_STATE_ICONIC);
	}
	view->mapped = visible != 0;
}

void wh_x11_focus(wh_view_id id) {
	struct x11_view *focused = find_view(id);
	wh_view_id next = focused && focused->mapped ? id : XCB_NONE;
	if (next) {
		/* Haskell clears its urgency when the window receives focus. */
		focused->urgency_hint = false;
		set_state_hint(focused, NET_WM_STATE_DEMANDS_ATTENTION, &focused->demands_attention, false);
	}
	if (next != x11.focused) {
		if (x11.focused) {
			grab_focus_clicks(x11.focused);
		}
		if (next) {
			ungrab_focus_clicks(next);
		}
		x11.focused = next;
	}
	for (struct x11_view *view = x11.views; view; view = view->next) {
		uint32_t pixel = view == focused ? x11.focused_border : x11.normal_border;
		xcb_change_window_attributes(x11.connection, view->id, XCB_CW_BORDER_PIXEL, &pixel);
	}
	if (next && !x11.hover_focus) {
		raise_view(focused);
	}
	xcb_set_input_focus(x11.connection, XCB_INPUT_FOCUS_POINTER_ROOT,
		next ? next : x11.root, XCB_CURRENT_TIME);
	if (x11.focus_follows_mouse) {
		/* The layout can move a window under a still pointer. The crossings
		 * queued by that relayout must leave the chosen focus in place. */
		x11.hovered = view_under_pointer();
	}
	xcb_flush(x11.connection);
}

void wh_x11_close(wh_view_id id) {
	if (!find_view(id)) {
		return;
	}
	xcb_icccm_get_wm_protocols_reply_t protocols;
	xcb_get_property_cookie_t cookie = xcb_icccm_get_wm_protocols(x11.connection, id, x11.wm_protocols);
	bool supports_delete = false;
	if (xcb_icccm_get_wm_protocols_reply(x11.connection, cookie, &protocols, NULL)) {
		for (uint32_t i = 0; i < protocols.atoms_len; i++) {
			supports_delete |= protocols.atoms[i] == x11.wm_delete_window;
		}
		xcb_icccm_get_wm_protocols_reply_wipe(&protocols);
	}
	if (supports_delete) {
		xcb_client_message_event_t event = {
			.response_type = XCB_CLIENT_MESSAGE,
			.format = 32,
			.window = id,
			.type = x11.wm_protocols,
			.data.data32 = {x11.wm_delete_window, XCB_CURRENT_TIME},
		};
		xcb_send_event(x11.connection, false, id, XCB_EVENT_MASK_NO_EVENT, (const char *)&event);
	} else {
		xcb_kill_client(x11.connection, id);
	}
}

const char *wh_x11_app_id(wh_view_id id) {
	struct x11_view *view = find_view(id);
	return view && view->app_id ? view->app_id : "";
}

int wh_x11_wants_float(wh_view_id id) {
	struct x11_view *view = find_view(id);
	return view && view->wants_float;
}

int wh_x11_natural_width(wh_view_id id) {
	struct x11_view *view = find_view(id);
	return view ? view->natural_width : 0;
}

int wh_x11_natural_height(wh_view_id id) {
	struct x11_view *view = find_view(id);
	return view ? view->natural_height : 0;
}
