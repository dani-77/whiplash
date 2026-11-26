#ifndef WHIPLASH_X11_INPUT_H
#define WHIPLASH_X11_INPUT_H

#include <stdbool.h>
#include <xcb/xcb.h>

struct wh_x11_touchpad_config {
	bool tap, drag, drag_lock, natural_scroll, disable_while_typing;
	float speed;
	int accel_profile, click_method;
};

void wh_x11_input_init(xcb_connection_t *connection, xcb_window_t root);
void wh_x11_input_event(const xcb_ge_generic_event_t *event);
void wh_x11_input_finish(void);
void wh_x11_input_set_touchpad(struct wh_x11_touchpad_config config);

#endif
