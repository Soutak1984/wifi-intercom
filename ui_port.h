#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void ui_start_task(void);

/* Touchscreen actions. They replace the old buttons. */
void ui_select(uint16_t station);
void ui_call(void);
void ui_end(void);
void ui_mic(void);
void ui_page(bool all_floors);
void ui_volume_step(int delta);

typedef struct ui_link {
    bool up;
    int rssi_dbm;
    int bars;
} ui_link_t;

typedef struct ui_battery {
    bool present;
    int percent;
    int millivolts;
} ui_battery_t;

void ui_link_read(ui_link_t *out);
void ui_battery_read(ui_battery_t *out);

/* One RSSI and pack reading. The UI task also does this on its own timer. */
void ui_meters_sample(void);

#ifdef __cplusplus
}
#endif
