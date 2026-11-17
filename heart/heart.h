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

/* Callbacks into the brain. */
typedef void (*wh_view_cb)(wh_view_id id);
/* Return non-zero when the key was consumed as a compositor binding. */
typedef int (*wh_key_cb)(uint32_t mods, uint32_t keysym);
/* The full output box and the part of it left after bars (exclusive
 * layer-shell zones). */
typedef void (*wh_output_cb)(int x, int y, int width, int height,
	int ux, int uy, int uwidth, int uheight);
typedef void (*wh_view_state_cb)(wh_view_id id, int on);
typedef void (*wh_view_rect_cb)(wh_view_id id, int x, int y, int width, int height);

enum wh_view_mode {
	WH_VIEW_TILED = 0,
	WH_VIEW_FLOATING = 1,
	WH_VIEW_FULLSCREEN = 2,
};

void wh_on_view_mapped(wh_view_cb cb);
void wh_on_view_unmapped(wh_view_cb cb);
void wh_on_view_clicked(wh_view_cb cb);
void wh_on_key(wh_key_cb cb);
void wh_on_output(wh_output_cb cb);
/* A client asked to enter (on=1) or leave (on=0) fullscreen. */
void wh_on_view_fullscreen(wh_view_state_cb cb);
/* mouse_mod + drag started on a view: float it now if it should move. */
void wh_on_view_grab(wh_view_rect_cb cb);
/* A drag ended; this is the view's new outer rectangle. */
void wh_on_view_moved(wh_view_rect_cb cb);

/* Lifecycle. wh_init returns 0 on success. xkb_layout may be NULL. */
int wh_init(const char *xkb_layout);
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
int wh_view_wants_float(wh_view_id id);
int wh_view_natural_width(wh_view_id id);
int wh_view_natural_height(wh_view_id id);
void wh_set_mouse_mod(uint32_t mods);
void wh_set_border(int width, uint32_t focused_rgba, uint32_t normal_rgba);
void wh_spawn(const char *command);

#endif
