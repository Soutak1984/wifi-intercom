#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PANEL_W 1024
#define PANEL_H 600

/* Starts the ST7262 and the GT911. Wire must already be up. EXIO5 stays low.
 * A false return leaves the intercom running and the glass dark. */
bool panel_7b_start(void);
bool panel_7b_ok(void);

/* Drive EXIO5 low again so GPIO 19/20 stay on the USB socket, then read the
 * pin. level_low is true when that input bit is clear. Wire must already be up. */
bool panel_7b_usb_socket(bool *level_low);

uint16_t *panel_7b_canvas(void);
void panel_7b_present(void);

/* The two scan buffers. LVGL draws into the one that is not on screen. */
void panel_7b_buffers(void **fb0, void **fb1);
/* Show one full frame and wait until the panel has taken it. */
bool panel_7b_draw(const void *fb);

/* One finger, panel coordinates. False when the finger is up or touch is absent. */
bool panel_7b_touch(int *x, int *y);

#ifdef __cplusplus
}
#endif
