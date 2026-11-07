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

/* LVGL renders every dirty region into the back framebuffer. After the final
 * flush, panel_7b_draw switches the RGB scanout to that completed page. */

static lv_color_t *s_partial = nullptr;
static uint16_t *s_shown = nullptr;
static uint16_t *s_off = nullptr;
static bool s_flip = false;
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
    if (!p || bytes == 0) {
        return;
    }
    esp_cache_msync(p, bytes, ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_UNALIGNED);
}

static void writeback_area(uint16_t *base, const lv_area_t *area) {
    if (!base || area->x2 < area->x1 || area->y2 < area->y1) {
        return;
    }
    size_t start = (size_t)area->y1 * PANEL_W + area->x1;
    size_t end = (size_t)area->y2 * PANEL_W + area->x2 + 1;
    writeback(&base[start], (end - start) * sizeof(uint16_t));
}

static void blit_partial(uint16_t *fb, const lv_area_t *area, const lv_color_t *src) {
    int w = area->x2 - area->x1 + 1;
    if (w <= 0 || area->y2 < area->y1 || !fb || !src) {
        return;
    }
    for (int y = area->y1; y <= area->y2; y++) {
        memcpy(&fb[(size_t)y * PANEL_W + area->x1], src, (size_t)w * sizeof(lv_color_t));
        src += w;
    }
}

/* Keep the LCD scanout buffer immutable while LVGL renders.  Copy each
 * dirty rectangle from the visible page to the back page first; LVGL then
 * paints every invalidated rectangle into the back page and we swap only
 * after the final flush.  Writing partial updates directly into s_shown
 * causes tearing because the RGB DMA may be reading those same pixels. */
static void copy_area(uint16_t *dst, const uint16_t *src, const lv_area_t *area) {
    int x1 = area->x1 < 0 ? 0 : area->x1;
    int y1 = area->y1 < 0 ? 0 : area->y1;
    int x2 = area->x2 >= PANEL_W ? PANEL_W - 1 : area->x2;
    int y2 = area->y2 >= PANEL_H ? PANEL_H - 1 : area->y2;
    int y;
    size_t width;
    if (!dst || !src || x2 < x1 || y2 < y1) {
        return;
    }
    width = (size_t)(x2 - x1 + 1);
    for (y = y1; y <= y2; y++) {
        size_t offset = (size_t)y * PANEL_W + (size_t)x1;
        memcpy(&dst[offset], &src[offset], width * sizeof(uint16_t));
    }
}

static void render_start_cb(lv_disp_drv_t *drv) {
    uint16_t i;
    (void)drv;
    s_flip = false;
    if (!s_disp || !s_off || !s_shown || s_disp->inv_p == 0) {
        return;
    }
    for (i = 0; i < s_disp->inv_p; i++) {
        if (s_disp->inv_area_joined[i]) {
            continue;
        }
        copy_area(s_off, s_shown, &s_disp->inv_areas[i]);
        s_flip = true;
    }
}

static void flush_cb(lv_disp_drv_t *drv, const lv_area_t *area, lv_color_t *color_map) {
    uint16_t *dst = s_flip ? s_off : s_shown;
    blit_partial(dst, area, color_map);
    writeback_area(dst, area);
    if (s_flip && lv_disp_flush_is_last(drv)) {
        /* Hand the finished page to the LCD DMA. The glass keeps the
         * previous page until this returns. */
        if (panel_7b_draw(s_off)) {
            uint16_t *swap = s_shown;
            s_shown = s_off;
            s_off = swap;
        }
        s_flip = false;
    }
    lv_disp_flush_ready(drv);
}

static lv_color_t *alloc_partial(uint32_t *out_px) {
    size_t largest = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
    size_t bytes;
    lv_color_t *buf;
    /* Drawing the page in internal RAM is much faster than drawing it in
     * PSRAM. 40 lines is 80 KB and previously left no room for the UI task,
     * so this is 8 lines (16 KB) and only when a large block is still free. */
    if (largest > 120u * 1024u) {
        bytes = (size_t)PANEL_W * 8 * sizeof(lv_color_t);
        bytes = (bytes + 63u) & ~63u;
        buf = (lv_color_t *)heap_caps_aligned_alloc(64, bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        if (buf) {
            *out_px = (uint32_t)PANEL_W * 8;
            Serial0.printf("lvgl draw buffer 8 lines internal, largest %u\n", (unsigned)largest);
            return buf;
        }
    }
    bytes = (size_t)PANEL_W * 40 * sizeof(lv_color_t);
    bytes = (bytes + 63u) & ~63u;
    buf = (lv_color_t *)heap_caps_aligned_alloc(64, bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (buf) {
        *out_px = (uint32_t)PANEL_W * 40;
        Serial0.println("lvgl draw buffer 40 lines in PSRAM");
        return buf;
    }
    bytes = (size_t)PANEL_W * 8 * sizeof(lv_color_t);
    bytes = (bytes + 63u) & ~63u;
    buf = (lv_color_t *)heap_caps_aligned_alloc(64, bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (buf) {
        *out_px = (uint32_t)PANEL_W * 8;
        Serial0.println("lvgl draw buffer 8 lines internal");
    }
    return buf;
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
    uint32_t partial_px = 0;
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
    partial_px = 0;
    s_partial = alloc_partial(&partial_px);
    if (!s_partial || partial_px == 0) {
        Serial0.println("lvgl draw buffer alloc failed");
        return false;
    }

    lv_disp_draw_buf_init(&s_draw_buf, s_partial, nullptr, partial_px);
    lv_disp_drv_init(&s_disp_drv);
    s_disp_drv.hor_res = PANEL_W;
    s_disp_drv.ver_res = PANEL_H;
    s_disp_drv.flush_cb = flush_cb;
    s_disp_drv.draw_buf = &s_draw_buf;
    s_disp_drv.render_start_cb = render_start_cb;
    s_disp_drv.direct_mode = 0;
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
