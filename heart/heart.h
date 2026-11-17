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
typedef void (*wh_output_cb)(int x, int y, int width, int height);

void wh_on_view_mapped(wh_view_cb cb);
void wh_on_view_unmapped(wh_view_cb cb);
void wh_on_view_clicked(wh_view_cb cb);
void wh_on_key(wh_key_cb cb);
void wh_on_output(wh_output_cb cb);

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
void wh_set_border(int width, uint32_t focused_rgba, uint32_t normal_rgba);
void wh_spawn(const char *command);

#endif
