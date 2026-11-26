#ifndef WHIPLASH_X11_H
#define WHIPLASH_X11_H

#include "heart.h"

struct wl_display;

struct wh_x11_callbacks {
	wh_view_cb mapped, unmapped, clicked, entered;
	wh_key_cb key;
	wh_void_cb outputs_changed;
	wh_workspace_cb workspace_activate;
};

int wh_x11_init(struct wl_display *display, struct wh_x11_callbacks callbacks);
void wh_x11_shutdown(void);
void wh_x11_set_bindings(const uint32_t *pairs, unsigned count);
void wh_x11_set_focus_follows_mouse(int enabled);
void wh_x11_add_workspace(int tag, const char *name);
void wh_x11_set_active_workspace(int tag);
int wh_x11_output_info(int index, int *out);
void wh_x11_set_border(int width, uint32_t focused, uint32_t normal);
void wh_x11_set_mode(wh_view_id id, int mode);
void wh_x11_set_geometry(wh_view_id id, int x, int y, int width, int height);
void wh_x11_set_visible(wh_view_id id, int visible);
void wh_x11_focus(wh_view_id id);
void wh_x11_close(wh_view_id id);
const char *wh_x11_app_id(wh_view_id id);
int wh_x11_natural_width(wh_view_id id);
int wh_x11_natural_height(wh_view_id id);

#endif
