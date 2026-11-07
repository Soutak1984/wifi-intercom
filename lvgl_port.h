#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

bool lvgl_port_start(void);
bool lvgl_port_ready(void);
void lvgl_port_lock(void);
void lvgl_port_unlock(void);
void lvgl_port_handler(void);

#ifdef __cplusplus
}
#endif
