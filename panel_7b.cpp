#include "panel_7b.h"

#include "pins.h"

#include <Arduino.h>
#include <Wire.h>

#include "esp_cache.h"
#include "esp_heap_caps.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_rgb.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

/* The IDE default is PSRAM Disabled, which links the quad-PSRAM library and
 * leaves this octal module with no framebuffer. Flash Mode OPI is a different
 * menu and will not boot the WROOM-1 quad flash. */
#if !defined(CONFIG_SPIRAM_MODE_OCT) || defined(CONFIG_ESPTOOLPY_FLASHMODE_OPI)
#error "7B LCD needs Tools > PSRAM > OPI PSRAM, Flash Size 16MB, and Flash Mode QIO (not OPI). Disabled or QSPI PSRAM leaves the glass dark."
#endif

static const char *TAG = "ic_panel";

/* Register chip at 0x24. Not a CH422G. Bit 5 is the USB/CAN mux and must
 * stay clear: high would move GPIO 19/20 onto the CAN transceiver. */
static const uint8_t EXP_ADDR = 0x24;
static const uint8_t EXP_MODE = 0x02;
static const uint8_t EXP_OUT = 0x03;
static const uint8_t EXP_PWM = 0x05;

static esp_lcd_panel_handle_t s_panel;
static uint16_t *s_fb[2];
static int s_back;
static uint8_t s_io;
static uint8_t s_touch_addr;
static bool s_touch;
static int s_touch_xmax = PANEL_W;
static int s_touch_ymax = PANEL_H;
static int s_touch_logs = 8;
static bool s_ok;
static volatile TaskHandle_t s_frame_task;

static bool exp_raw(uint8_t reg, uint8_t val) {
    Wire.beginTransmission(EXP_ADDR);
    Wire.write(reg);
    Wire.write(val);
    return Wire.endTransmission() == 0;
}

static bool exp_commit(void) {
    s_io = (uint8_t)(s_io & ~(1u << 5));
    return exp_raw(EXP_OUT, s_io);
}

static bool exp_pin(int pin, int high) {
    if (pin == 5) {
        high = 0;
    }
    if (high) {
        s_io = (uint8_t)(s_io | (1u << pin));
    } else {
        s_io = (uint8_t)(s_io & ~(1u << pin));
    }
    return exp_commit();
}

bool panel_7b_usb_socket(bool *level_low) {
    uint8_t in = 0xFF;
    /* 0xDF keeps panel power and the backlight pin high. Bit 5 is the mux. */
    s_io = (uint8_t)((s_io | 0xDF) & ~(1u << 5));
    if (!exp_raw(EXP_MODE, 0xFF) || !exp_commit()) {
        return false;
    }
    Wire.beginTransmission(EXP_ADDR);
    Wire.write(0x04);
    if (Wire.endTransmission(false) != 0 || Wire.requestFrom(EXP_ADDR, (uint8_t)1) != 1) {
        return false;
    }
    in = (uint8_t)Wire.read();
    if (level_low) {
        *level_low = (in & (1u << 5)) == 0;
    }
    return true;
}

static bool gt_read(uint8_t addr, uint16_t reg, uint8_t *dst, int n) {
    int i;
    Wire.beginTransmission(addr);
    Wire.write((uint8_t)(reg >> 8));
    Wire.write((uint8_t)(reg & 0xFF));
    if (Wire.endTransmission(false) != 0) {
        return false;
    }
    if (Wire.requestFrom((uint8_t)addr, (uint8_t)n) != n) {
        return false;
    }
    for (i = 0; i < n; i++) {
        dst[i] = (uint8_t)Wire.read();
    }
    return true;
}

static bool gt_write(uint8_t addr, uint16_t reg, uint8_t val) {
    Wire.beginTransmission(addr);
    Wire.write((uint8_t)(reg >> 8));
    Wire.write((uint8_t)(reg & 0xFF));
    Wire.write(val);
    return Wire.endTransmission() == 0;
}

static bool IRAM_ATTR on_frame_done(esp_lcd_panel_handle_t panel,
                                    const esp_lcd_rgb_panel_event_data_t *edata,
                                    void *user_ctx) {
    BaseType_t wake = pdFALSE;
    TaskHandle_t task;
    (void)panel;
    (void)edata;
    (void)user_ctx;
    task = s_frame_task;
    if (task) {
        vTaskNotifyGiveFromISR(task, &wake);
    }
    return wake == pdTRUE;
}

/* Active-low boost. Duty 1 is full bright. 0 and 255 drop the converter,
 * which washes the edges and leaves a shadow of the last picture. */
static void exp_backlight(void) {
    const uint8_t percent = 100;
    uint8_t duty = (uint8_t)(1u + ((100u - percent) * 246u) / 100u);
    if (!exp_raw(EXP_PWM, duty)) {
        Serial0.println("panel: backlight pwm write failed");
        return;
    }
    Serial0.printf("panel: backlight %u%% pwm %u\n", percent, duty);
}

static void fill_fb(uint16_t *fb, uint16_t color) {
    size_t i;
    size_t n = (size_t)PANEL_W * PANEL_H;
    if (!fb) {
        return;
    }
    for (i = 0; i < n; i++) {
        fb[i] = color;
    }
}

static void writeback(void *fb) {
    esp_cache_msync(fb, (size_t)PANEL_W * PANEL_H * sizeof(uint16_t),
                    ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_UNALIGNED);
}

static bool rgb_start(void) {
    esp_lcd_rgb_panel_config_t cfg = {
        .clk_src = LCD_CLK_SRC_DEFAULT,
        .timings = {
            .pclk_hz = 30 * 1000 * 1000,
            .h_res = PANEL_W,
            .v_res = PANEL_H,
            .hsync_pulse_width = 162,
            .hsync_back_porch = 152,
            .hsync_front_porch = 48,
            .vsync_pulse_width = 45,
            .vsync_back_porch = 13,
            .vsync_front_porch = 3,
            .flags = {
                .pclk_active_neg = 1,
            },
        },
        .data_width = 16,
        .bits_per_pixel = 16,
        .num_fbs = 2,
        /* 20 lines gives the PSRAM-to-DRAM bounce-buffer DMA more time
         * between refills. A missed refill can desynchronize RGB scanout
         * and make the entire image jump vertically. */
        .bounce_buffer_size_px = PANEL_W * 20,
        .sram_trans_align = 4,
        .psram_trans_align = 64,
        .hsync_gpio_num = PIN_LCD_HSYNC,
        .vsync_gpio_num = PIN_LCD_VSYNC,
        .de_gpio_num = PIN_LCD_DE,
        .pclk_gpio_num = PIN_LCD_PCLK,
        .disp_gpio_num = GPIO_NUM_NC,
        .data_gpio_nums = {
            PIN_LCD_D0, PIN_LCD_D1, PIN_LCD_D2, PIN_LCD_D3, PIN_LCD_D4,
            PIN_LCD_D5, PIN_LCD_D6, PIN_LCD_D7, PIN_LCD_D8, PIN_LCD_D9,
            PIN_LCD_D10, PIN_LCD_D11, PIN_LCD_D12, PIN_LCD_D13, PIN_LCD_D14,
            PIN_LCD_D15,
        },
        .flags = {
            .fb_in_psram = 1,
            .bb_invalidate_cache = 1,
        },
    };
    esp_err_t err = esp_lcd_new_rgb_panel(&cfg, &s_panel);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "RGB panel create failed (%s)", esp_err_to_name(err));
        s_panel = NULL;
        return false;
    }
    err = esp_lcd_panel_reset(s_panel);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "RGB panel reset failed (%s)", esp_err_to_name(err));
        return false;
    }
    err = esp_lcd_panel_init(s_panel);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "RGB panel init failed (%s)", esp_err_to_name(err));
        return false;
    }
    {
        esp_lcd_rgb_panel_event_callbacks_t cbs = {};
        cbs.on_frame_buf_complete = on_frame_done;
        err = esp_lcd_rgb_panel_register_event_callbacks(s_panel, &cbs, NULL);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "frame callback not registered (%s)", esp_err_to_name(err));
        }
    }
    return true;
}

static bool gt_probe(uint8_t addr) {
    uint8_t id[4];
    int attempt;
    for (attempt = 0; attempt < 5; attempt++) {
        if (gt_read(addr, 0x8140, id, 4) && id[0] == '9') {
            s_touch_addr = addr;
            Serial0.printf("GT911 at 0x%02X id %c%c%c%c\n", addr, id[0], id[1], id[2], id[3]);
            return true;
        }
        delay(30);
    }
    return false;
}

static void gt_reset(int int_level) {
    pinMode(PIN_TOUCH_INT, OUTPUT);
    digitalWrite(PIN_TOUCH_INT, LOW);
    exp_pin(1, 0);
    delay(100);
    digitalWrite(PIN_TOUCH_INT, int_level ? HIGH : LOW);
    delay(100);
    exp_pin(1, 1);
    delay(200);
    pinMode(PIN_TOUCH_INT, INPUT_PULLUP);
    delay(50);
}

static void gt_read_resolution(void) {
    uint8_t cfg[5];
    int xr;
    int yr;
    if (!gt_read(s_touch_addr, 0x8047, cfg, 5)) {
        Serial0.println("GT911 config unread, using 1024 x 600");
        return;
    }
    xr = (int)cfg[1] | ((int)cfg[2] << 8);
    yr = (int)cfg[3] | ((int)cfg[4] << 8);
    if (xr >= 100 && xr <= 4096) {
        s_touch_xmax = xr;
    }
    if (yr >= 100 && yr <= 4096) {
        s_touch_ymax = yr;
    }
    Serial0.printf("GT911 config %u, resolution %d x %d\n", cfg[0], s_touch_xmax, s_touch_ymax);
}

static int map_touch(int raw, int raw_max, int screen) {
    if (raw < 0) {
        raw = 0;
    }
    if (raw_max < 2) {
        raw_max = screen;
    }
    if (raw > raw_max) {
        raw = raw_max;
    }
    return (raw * (screen - 1)) / raw_max;
}

static bool touch_start(void) {
    /* INT low during reset selects 0x5D. INT high selects 0x14. */
    gt_reset(0);
    if (!gt_probe(0x5D)) {
        gt_reset(1);
        if (!gt_probe(0x14)) {
            ESP_LOGW(TAG, "GT911 did not answer. The picture still draws.");
            Serial0.println("GT911 did not answer. Touch is off.");
            return false;
        }
    }
    s_touch = true;
    gt_read_resolution();
    return true;
}

bool panel_7b_start(void) {
    size_t spiram;
    void *p0 = NULL;
    void *p1 = NULL;
    s_ok = false;
    s_touch = false;
    s_panel = NULL;
    s_fb[0] = NULL;
    s_fb[1] = NULL;
    s_back = 1;
    s_frame_task = NULL;

    /* Same expander sequence as the Fire Alarm panel on this glass.
     * 0xDF: LCD power and backlight pin high, USB select low.
     * The PWM register is what actually runs the boost. */
    if (!exp_raw(EXP_MODE, 0xFF)) {
        ESP_LOGE(TAG, "expander mode write failed");
        Serial0.println("panel: IO expander did not answer");
        return false;
    }
    s_io = 0xDF;
    if (!exp_commit()) {
        ESP_LOGE(TAG, "expander output write failed");
        Serial0.println("panel: IO expander did not answer");
        return false;
    }
    delay(10);
    exp_pin(6, 1);
    exp_pin(2, 1);
    delay(10);
    exp_pin(3, 0);
    delay(20);
    exp_pin(3, 1);
    delay(120);
    exp_backlight();

    spiram = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    if (spiram < 4u * 1024u * 1024u) {
        exp_pin(2, 1);
        ESP_LOGE(TAG, "OPI PSRAM required, free %u. Tools > PSRAM > OPI PSRAM.",
                 (unsigned)spiram);
        Serial0.printf("panel: backlight on, no picture. OPI PSRAM required, free %u\n",
                       (unsigned)spiram);
        return false;
    }

    if (!rgb_start()) {
        exp_pin(2, 1);
        Serial0.println("panel: ST7262 init failed");
        return false;
    }
    if (esp_lcd_rgb_panel_get_frame_buffer(s_panel, 2, &p0, &p1) != ESP_OK || !p0 || !p1) {
        ESP_LOGE(TAG, "framebuffer missing");
        Serial0.println("panel: framebuffer alloc failed");
        exp_pin(2, 1);
        return false;
    }
    s_fb[0] = (uint16_t *)p0;
    s_fb[1] = (uint16_t *)p1;
    /* #14110E. Both buffers, so a swap cannot flash an uncached corner. */
    fill_fb(s_fb[0], 0x1081);
    fill_fb(s_fb[1], 0x1081);
    writeback(s_fb[0]);
    writeback(s_fb[1]);
    s_back = 1;

    touch_start();
    s_ok = true;
    ESP_LOGI(TAG, "ST7262 1024x600 up, backlight pwm on, EXIO5 held low");
    Serial0.println("panel: ST7262 up, backlight on");
    return true;
}

bool panel_7b_ok(void) {
    return s_ok && s_panel && s_fb[0] && s_fb[1];
}

uint16_t *panel_7b_canvas(void) {
    if (s_back != 0 && s_back != 1) {
        return NULL;
    }
    return s_fb[s_back];
}

void panel_7b_buffers(void **fb0, void **fb1) {
    if (fb0) {
        *fb0 = s_fb[0];
    }
    if (fb1) {
        *fb1 = s_fb[1];
    }
}

bool panel_7b_draw(const void *fb) {
    esp_err_t err;
    uint32_t got;
    if (!s_panel || !fb) {
        return false;
    }
    /* Drop a notify left over from the frame already on glass. */
    ulTaskNotifyTake(pdTRUE, 0);
    err = esp_lcd_panel_draw_bitmap(s_panel, 0, 0, PANEL_W, PANEL_H, fb);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "draw failed (%s)", esp_err_to_name(err));
        return false;
    }
    /* Bounce mode switches the scanned buffer at the next frame end.
     * Arm the wait after the index change, or the frame that just ended
     * would count and the old buffer would still be on the glass. */
    s_frame_task = xTaskGetCurrentTaskHandle();
    got = ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(70));
    s_frame_task = NULL;
    if (got == 0) {
        /* The index is already changed. Keep going so the next paint does
         * not draw into the buffer the panel is about to scan. */
        ESP_LOGW(TAG, "frame switch timed out");
    }
    return true;
}

void panel_7b_present(void) {
    uint16_t *fb;
    if (!s_ok || !s_panel) {
        return;
    }
    fb = panel_7b_canvas();
    if (!fb) {
        return;
    }
    /* Hand the finished buffer to the scanner. The next paint uses the
     * other one, which the DMA is no longer reading. */
    writeback(fb);
    if (!panel_7b_draw(fb)) {
        return;
    }
    s_back ^= 1;
}

bool panel_7b_touch(int *x, int *y) {
    uint8_t st = 0;
    uint8_t pt[8];
    int n;
    int rx;
    int ry;
    if (!s_touch || !x || !y) {
        return false;
    }
    if (!gt_read(s_touch_addr, 0x814E, &st, 1)) {
        return false;
    }
    if ((st & 0x80) == 0) {
        return false;
    }
    n = (int)(st & 0x0F);
    /* Point 1 starts at 0x8150. 0x814F is the track id, not the X coordinate. */
    if (n >= 1 && n <= 5 && gt_read(s_touch_addr, 0x8150, pt, 8)) {
        rx = (int)pt[0] | ((int)pt[1] << 8);
        ry = (int)pt[2] | ((int)pt[3] << 8);
        *x = map_touch(rx, s_touch_xmax, PANEL_W);
        *y = map_touch(ry, s_touch_ymax, PANEL_H);
        if (s_touch_logs > 0) {
            s_touch_logs--;
            Serial0.printf("touch raw %d,%d -> %d,%d\n", rx, ry, *x, *y);
        }
        gt_write(s_touch_addr, 0x814E, 0);
        return true;
    }
    gt_write(s_touch_addr, 0x814E, 0);
    return false;
}
