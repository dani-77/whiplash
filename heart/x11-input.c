#include "x11-input.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <xcb/xinput.h>

enum input_atom {
	TAPPING, TAP_DRAG, TAP_DRAG_LOCK, TAP_BUTTON_MAP, NATURAL_SCROLL, DWT, SPEED,
	ACCEL, ACCEL_DEFAULT, ACCEL_AVAILABLE, CLICK, CLICK_DEFAULT, CLICK_AVAILABLE,
	FLOAT_TYPE, INPUT_ATOM_COUNT,
};

static const char *const property_names[INPUT_ATOM_COUNT] = {
	[TAPPING] = "libinput Tapping Enabled",
	[TAP_DRAG] = "libinput Tapping Drag Enabled",
	[TAP_DRAG_LOCK] = "libinput Tapping Drag Lock Enabled",
	[TAP_BUTTON_MAP] = "libinput Tapping Button Mapping Enabled",
	[NATURAL_SCROLL] = "libinput Natural Scrolling Enabled",
	[DWT] = "libinput Disable While Typing Enabled",
	[SPEED] = "libinput Accel Speed",
	[ACCEL] = "libinput Accel Profile Enabled",
	[ACCEL_DEFAULT] = "libinput Accel Profile Enabled Default",
	[ACCEL_AVAILABLE] = "libinput Accel Profiles Available",
	[CLICK] = "libinput Click Method Enabled",
	[CLICK_DEFAULT] = "libinput Click Method Enabled Default",
	[CLICK_AVAILABLE] = "libinput Click Methods Available",
	[FLOAT_TYPE] = "FLOAT",
};

static struct {
	xcb_connection_t *connection;
	xcb_window_t root;
	uint8_t extension;
	xcb_atom_t atoms[INPUT_ATOM_COUNT];
	struct wh_x11_touchpad_config touchpad;
	wh_swipe_cb swipe_callback;
	bool swipe_supported, swipe_enabled, swipe_grabbed;
	struct {
		xcb_input_device_id_t device, source;
		double dx, dy;
	} swipe;
} input;

static void load_atoms(void) {
	xcb_intern_atom_cookie_t cookies[INPUT_ATOM_COUNT];
	for (int i = 0; i < INPUT_ATOM_COUNT; i++) {
		cookies[i] = xcb_intern_atom(input.connection, false,
			strlen(property_names[i]), property_names[i]);
	}
	for (int i = 0; i < INPUT_ATOM_COUNT; i++) {
		xcb_intern_atom_reply_t *reply = xcb_intern_atom_reply(input.connection, cookies[i], NULL);
		input.atoms[i] = reply ? reply->atom : XCB_ATOM_NONE;
		free(reply);
	}
}

static void set_property(xcb_input_device_id_t device, enum input_atom property,
		xcb_atom_t type, uint8_t format, uint32_t count, const void *values) {
	xcb_input_xi_change_property(input.connection, device, XCB_PROP_MODE_REPLACE,
		format, input.atoms[property], type, count, values);
}

static size_t read_bytes(xcb_input_device_id_t device, enum input_atom property, uint8_t values[3]) {
	xcb_input_xi_get_property_cookie_t cookie = xcb_input_xi_get_property(
		input.connection, device, false, input.atoms[property], XCB_ATOM_INTEGER, 0, 1);
	xcb_input_xi_get_property_reply_t *reply = xcb_input_xi_get_property_reply(input.connection,
		cookie, NULL);
	size_t count = 0;
	if (reply && reply->type == XCB_ATOM_INTEGER && reply->format == 8 &&
			reply->num_items <= 3 && reply->bytes_after == 0) {
		count = reply->num_items;
		memcpy(values, xcb_input_xi_get_property_items(reply), count);
	}
	free(reply);
	return count;
}

static void set_choice(xcb_input_device_id_t device, enum input_atom property,
		enum input_atom defaults, enum input_atom available, int choice) {
	uint8_t values[3] = {0};
	size_t count = read_bytes(device, choice < 0 ? defaults : available, values);
	if (!count) {
		return;
	}
	if (choice >= 0) {
		if ((size_t)choice >= count || !values[choice]) {
			return;
		}
		memset(values, 0, sizeof(values));
		values[choice] = 1;
	}
	set_property(device, property, XCB_ATOM_INTEGER, 8, count, values);
}

static void apply_touchpad(xcb_input_device_id_t device) {
	xcb_input_xi_list_properties_reply_t *reply = xcb_input_xi_list_properties_reply(
		input.connection, xcb_input_xi_list_properties(input.connection, device), NULL);
	if (!reply) {
		return;
	}
	bool supported[INPUT_ATOM_COUNT] = {0};
	const xcb_atom_t *atoms = xcb_input_xi_list_properties_properties(reply);
	for (int i = 0; i < xcb_input_xi_list_properties_properties_length(reply); i++) {
		for (int j = 0; j < INPUT_ATOM_COUNT; j++) {
			supported[j] |= atoms[i] == input.atoms[j];
		}
	}
	free(reply);
	/* The libinput tapping property identifies touchpads, with or without tapping enabled. */
	if (!supported[TAPPING]) {
		return;
	}
	const struct wh_x11_touchpad_config *config = &input.touchpad;
	const struct { enum input_atom property; uint8_t value; } flags[] = {
		{TAPPING, config->tap}, {TAP_DRAG, config->drag}, {TAP_DRAG_LOCK, config->drag_lock},
		{NATURAL_SCROLL, config->natural_scroll}, {DWT, config->disable_while_typing},
	};
	for (size_t i = 0; i < sizeof(flags) / sizeof(flags[0]); i++) {
		if (supported[flags[i].property]) {
			set_property(device, flags[i].property, XCB_ATOM_INTEGER, 8, 1, &flags[i].value);
		}
	}
	if (supported[TAP_BUTTON_MAP]) {
		const uint8_t lrm[] = {1, 0};
		set_property(device, TAP_BUTTON_MAP, XCB_ATOM_INTEGER, 8, 2, lrm);
	}
	if (supported[ACCEL]) {
		int profile = config->accel_profile == WH_ACCEL_DEFAULT ? -1 :
			config->accel_profile == WH_ACCEL_FLAT ? 1 : 0;
		set_choice(device, ACCEL, ACCEL_DEFAULT, ACCEL_AVAILABLE, profile);
	}
	if (supported[SPEED] && input.atoms[FLOAT_TYPE]) {
		set_property(device, SPEED, input.atoms[FLOAT_TYPE], 32, 1, &config->speed);
	}
	if (supported[CLICK]) {
		int method = config->click_method == WH_CLICK_DEFAULT ? -1 :
			config->click_method == WH_CLICK_CLICKFINGER ? 1 : 0;
		set_choice(device, CLICK, CLICK_DEFAULT, CLICK_AVAILABLE, method);
	}
}

static void apply_touchpads(void) {
	xcb_input_xi_query_device_reply_t *reply = xcb_input_xi_query_device_reply(input.connection,
		xcb_input_xi_query_device(input.connection, XCB_INPUT_DEVICE_ALL), NULL);
	if (!reply) {
		return;
	}
	xcb_input_xi_device_info_iterator_t devices = xcb_input_xi_query_device_infos_iterator(reply);
	for (; devices.rem; xcb_input_xi_device_info_next(&devices)) {
		const xcb_input_xi_device_info_t *device = devices.data;
		if (device->enabled && (device->type == XCB_INPUT_DEVICE_TYPE_SLAVE_POINTER ||
				device->type == XCB_INPUT_DEVICE_TYPE_FLOATING_SLAVE)) {
			apply_touchpad(device->deviceid);
		}
	}
	free(reply);
	xcb_flush(input.connection);
}

static void update_swipe_grab(void) {
	if (!input.swipe_supported || !input.swipe_callback ||
			input.swipe_enabled == input.swipe_grabbed) {
		return;
	}
	const uint32_t modifiers = XCB_INPUT_MODIFIER_MASK_ANY;
	const uint8_t type = XCB_INPUT_GRAB_TYPE_GESTURE_SWIPE_BEGIN;
	if (!input.swipe_enabled) {
		xcb_input_xi_passive_ungrab_device(input.connection, input.root, 0,
			XCB_INPUT_DEVICE_ALL_MASTER, 1, type, &modifiers);
		input.swipe_grabbed = false;
		xcb_flush(input.connection);
		return;
	}
	const uint32_t mask[] = {
		(UINT32_C(1) << XCB_INPUT_GESTURE_SWIPE_BEGIN) |
			(UINT32_C(1) << XCB_INPUT_GESTURE_SWIPE_UPDATE),
		UINT32_C(1) << (XCB_INPUT_GESTURE_SWIPE_END - 32),
	};
	xcb_input_xi_passive_grab_device_reply_t *reply = xcb_input_xi_passive_grab_device_reply(
		input.connection, xcb_input_xi_passive_grab_device(input.connection, XCB_CURRENT_TIME,
			input.root, XCB_NONE, 0, XCB_INPUT_DEVICE_ALL_MASTER, 1, 2, type,
			XCB_GRAB_MODE_SYNC, XCB_GRAB_MODE_ASYNC, false, mask, &modifiers), NULL);
	input.swipe_grabbed = reply && !reply->num_modifiers;
	free(reply);
	if (!input.swipe_grabbed) {
		fprintf(stderr, "whiplash: could not grab X11 touchpad swipes\n");
	}
}

static void handle_swipe(const xcb_input_gesture_swipe_begin_event_t *event) {
	if (event->event_type == XCB_INPUT_GESTURE_SWIPE_BEGIN) {
		/* Replay unbound finger counts so the application sees the entire swipe. */
		bool take = input.swipe_enabled &&
			(!input.swipe.device || input.swipe.device == event->deviceid) &&
			input.swipe_callback((int)event->detail, WH_SWIPE_QUERY);
		xcb_input_xi_allow_events(input.connection, event->time, event->deviceid,
			take ? XCB_INPUT_EVENT_MODE_ASYNC_DEVICE : XCB_INPUT_EVENT_MODE_REPLAY_DEVICE,
			0, XCB_NONE);
		xcb_flush(input.connection);
		if (take) {
			input.swipe.device = event->deviceid;
			input.swipe.source = event->sourceid;
			input.swipe.dx = input.swipe.dy = 0;
		}
		return;
	}
	if (input.swipe.device != event->deviceid || input.swipe.source != event->sourceid) {
		return;
	}
	if (event->event_type == XCB_INPUT_GESTURE_SWIPE_UPDATE) {
		/* XInput encodes the unaccelerated deltas as signed 16.16 fixed point. */
		input.swipe.dx += event->delta_unaccel_x / 65536.0;
		input.swipe.dy += event->delta_unaccel_y / 65536.0;
		return;
	}
	input.swipe.device = 0;
	double dx = input.swipe.dx, dy = input.swipe.dy;
	if (!input.swipe_enabled ||
			(event->flags & XCB_INPUT_GESTURE_SWIPE_EVENT_FLAGS_GESTURE_SWIPE_CANCELLED) ||
			dx * dx + dy * dy < WH_SWIPE_MIN * WH_SWIPE_MIN) {
		return;
	}
	int direction = fabs(dx) > fabs(dy)
		? (dx < 0 ? WH_SWIPE_LEFT : WH_SWIPE_RIGHT)
		: (dy < 0 ? WH_SWIPE_UP : WH_SWIPE_DOWN);
	input.swipe_callback((int)event->detail, direction);
}

void wh_x11_input_init(xcb_connection_t *connection, xcb_window_t root, wh_swipe_cb swipe) {
	const xcb_query_extension_reply_t *extension = xcb_get_extension_data(connection, &xcb_input_id);
	if (!extension || !extension->present) {
		return;
	}
	xcb_input_xi_query_version_reply_t *version = xcb_input_xi_query_version_reply(connection,
		xcb_input_xi_query_version(connection, 2, 4), NULL);
	bool supported = version && version->major_version >= 2;
	input.swipe_supported = supported && (version->major_version > 2 || version->minor_version >= 4);
	free(version);
	if (!supported) {
		return;
	}
	input.connection = connection;
	input.root = root;
	input.extension = extension->major_opcode;
	input.swipe_callback = swipe;
	load_atoms();
	const struct { xcb_input_event_mask_t header; uint32_t mask; } hierarchy = {
		{XCB_INPUT_DEVICE_ALL, 1}, XCB_INPUT_XI_EVENT_MASK_HIERARCHY,
	};
	xcb_input_xi_select_events(connection, root, 1, &hierarchy.header);
	apply_touchpads();
	update_swipe_grab();
}

void wh_x11_input_event(const xcb_ge_generic_event_t *event) {
	if (!input.connection || event->extension != input.extension ||
			event->response_type != XCB_GE_GENERIC) {
		return;
	}
	if (event->event_type == XCB_INPUT_HIERARCHY) {
		const xcb_input_hierarchy_event_t *change = (const void *)event;
		if (change->flags & (XCB_INPUT_HIERARCHY_MASK_SLAVE_ADDED | XCB_INPUT_HIERARCHY_MASK_DEVICE_ENABLED)) {
			apply_touchpads();
		}
		xcb_input_hierarchy_info_iterator_t devices = xcb_input_hierarchy_infos_iterator(change);
		for (; input.swipe.device && devices.rem; xcb_input_hierarchy_info_next(&devices)) {
			const xcb_input_hierarchy_info_t *device = devices.data;
			if ((device->deviceid == input.swipe.device || device->deviceid == input.swipe.source) &&
					(device->flags & (XCB_INPUT_HIERARCHY_MASK_MASTER_REMOVED |
						XCB_INPUT_HIERARCHY_MASK_SLAVE_REMOVED | XCB_INPUT_HIERARCHY_MASK_DEVICE_DISABLED))) {
				input.swipe.device = 0;
			}
		}
	} else if (input.swipe_callback && event->event_type >= XCB_INPUT_GESTURE_SWIPE_BEGIN &&
			event->event_type <= XCB_INPUT_GESTURE_SWIPE_END &&
			event->length >= (sizeof(xcb_input_gesture_swipe_begin_event_t) -
				sizeof(xcb_ge_generic_event_t)) / 4) {
		handle_swipe((const void *)event);
	}
}

void wh_x11_input_finish(void) {
	input.connection = NULL;
	input.swipe_supported = input.swipe_grabbed = false;
	input.swipe.device = 0;
}

void wh_x11_input_set_swipes_enabled(bool enabled) {
	input.swipe_enabled = enabled;
	if (!enabled) {
		input.swipe.device = 0;
	}
	update_swipe_grab();
}

void wh_x11_input_set_touchpad(struct wh_x11_touchpad_config config) {
	input.touchpad = config;
	if (input.connection) {
		apply_touchpads();
	}
}
