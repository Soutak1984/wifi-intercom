#include "lvgl_port.h"

#include "panel_7b.h"
#include "ui_lvgl.h"

#include <Arduino.h>
#include <string.h>

#include "esp_cache.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "lvgl.h"

/* Full pages are drawn into the framebuffer the panel is not scanning.
 * panel_7b_draw then points the LCD DMA at that buffer, so the new page
 * appears in one frame. A slider only touches the buffer already on glass,
 * and does not wait for a DMA switch. */

static lv_disp_t *s_disp = nullptr;
static lv_disp_draw_buf_t s_draw_buf;
static lv_disp_drv_t s_disp_drv;
static lv_indev_drv_t s_indev_drv;
static SemaphoreHandle_t s_lock = nullptr;
static bool s_ready = false;
static bool s_touch_logged = false;
static int s_touch_x = 0;
static int s_touch_y = 0;
static uint32_t s_touch_ms = 0;
static bool s_touch_held = false;

static void writeback(void *p, size_t bytes) {
    if (!p || bytes == 0) return;
    esp_cache_msync(p, bytes, ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_UNALIGNED);
}

static void writeback_area(uint16_t *base, const lv_area_t *area) {
    if (!base || area->x2 < area->x1 || area->y2 < area->y1) return;
    for (int y = area->y1; y <= area->y2; y++) {
        size_t start = (size_t)y * PANEL_W + area->x1;
        size_t bytes = (size_t)(area->x2 - area->x1 + 1) * sizeof(uint16_t);
        writeback(&base[start], bytes);
    }
}

/* LVGL direct mode owns the two complete RGB framebuffers. It renders into
 * the back buffer, synchronizes invalidated regions between buffers, then
 * hands the finished framebuffer to the RGB panel at the frame boundary. */
static void flush_cb(lv_disp_drv_t *drv, const lv_area_t *area, lv_color_t *color_map) {
    if (color_map && area) {
        writeback_area((uint16_t *)color_map, area);
    }
    if (lv_disp_flush_is_last(drv) && color_map) {
        if (!panel_7b_draw(color_map)) {
            Serial0.println("lvgl: RGB framebuffer switch failed");
        }
    }
    lv_disp_flush_ready(drv);
}

static void touch_cb(lv_indev_drv_t *drv, lv_indev_data_t *data) {
    int x = 0;
    int y = 0;
    bool down = panel_7b_touch(&x, &y);
    uint32_t now = millis();
    (void)drv;
    /* The GT911 raises its status bit per sample, then we clear it. Hold the
     * last point across that gap so a slider drag does not drop. */
    if (down) {
        s_touch_x = x;
        s_touch_y = y;
        s_touch_ms = now;
        s_touch_held = true;
        if (!s_touch_logged) {
            s_touch_logged = true;
            Serial0.printf("touch %d,%d\n", x, y);
        }
        ui_lvgl_touch(1);
    } else if (s_touch_held && (uint32_t)(now - s_touch_ms) < 40u) {
        down = true;
        x = s_touch_x;
        y = s_touch_y;
    } else {
        s_touch_held = false;
        x = s_touch_x;
        y = s_touch_y;
    }
    data->point.x = (lv_coord_t)x;
    data->point.y = (lv_coord_t)y;
    data->state = down ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
}

bool lvgl_port_start(void) {
    void *fb0 = nullptr;
    void *fb1 = nullptr;
    if (s_ready) {
        return true;
    }
    if (!panel_7b_ok()) {
        return false;
    }
    s_lock = xSemaphoreCreateRecursiveMutex();
    if (!s_lock) {
        Serial0.println("lvgl mutex failed");
        return false;
    }
    panel_7b_buffers(&fb0, &fb1);
    if (!fb0 || !fb1) {
        Serial0.println("lvgl framebuffers missing");
        return false;
    }
    /* The panel starts on framebuffer 0. Full pages land in the other one. */
    s_shown = (uint16_t *)fb0;
    s_off = (uint16_t *)fb1;

    lv_init();
    lv_disp_draw_buf_init(&s_draw_buf, (lv_color_t *)fb0, (lv_color_t *)fb1,
                          (uint32_t)PANEL_W * PANEL_H);
    lv_disp_drv_init(&s_disp_drv);
    s_disp_drv.hor_res = PANEL_W;
    s_disp_drv.ver_res = PANEL_H;
    s_disp_drv.flush_cb = flush_cb;
    s_disp_drv.draw_buf = &s_draw_buf;
    s_disp_drv.direct_mode = 1;
    s_disp_drv.full_refresh = 0;
    s_disp = lv_disp_drv_register(&s_disp_drv);
    if (!s_disp) {
        Serial0.println("lvgl display register failed");
        return false;
    }

    lv_theme_t *theme = lv_theme_default_init(s_disp, lv_color_hex(0xE4A04A), lv_color_hex(0xD4654E),
                                              true, &lv_font_montserrat_16);
    if (theme) {
        lv_disp_set_theme(s_disp, theme);
    }

    lv_indev_drv_init(&s_indev_drv);
    s_indev_drv.type = LV_INDEV_TYPE_POINTER;
    s_indev_drv.read_cb = touch_cb;
    s_indev_drv.disp = s_disp;
    lv_indev_drv_register(&s_indev_drv);

    s_ready = true;
    Serial0.println("lvgl ready");
    return true;
}

bool lvgl_port_ready(void) {
    return s_ready;
}

void lvgl_port_lock(void) {
    if (s_lock) {
        xSemaphoreTakeRecursive(s_lock, portMAX_DELAY);
    }
}

void lvgl_port_unlock(void) {
    if (s_lock) {
        xSemaphoreGiveRecursive(s_lock);
    }
}

void lvgl_port_handler(void) {
    if (!s_ready) {
        return;
    }
    lvgl_port_lock();
    (void)lv_timer_handler();
    lvgl_port_unlock();
}
