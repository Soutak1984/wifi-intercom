#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Flat title while the headset host is still starting. Safe before app_lock exists. */
void ui_paint_banner(void);

/* Checklist splash. Call after the core and the network stack exist. */
void ui_paint_boot(void);

/* Touch and redraw. Call from the UI task only. */
void ui_paint_poll(uint32_t now_ms);

#ifdef __cplusplus
}
#endif
