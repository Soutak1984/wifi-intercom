#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void ui_lvgl_build(void);
void ui_lvgl_boot(uint32_t now_ms);
void ui_lvgl_poll(uint32_t now_ms);
/* A finger on the glass. Used to leave the splash even before a widget click. */
void ui_lvgl_touch(int down);

#ifdef __cplusplus
}
#endif
