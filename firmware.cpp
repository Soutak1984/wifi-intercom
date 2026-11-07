#include "firmware.h"

#include "app.h"
#include "audio_port.h"
#include "console_arduino.h"
#include "ic_crypto.h"
#include "net_arduino.h"
#include "lvgl_port.h"
#include "panel_7b.h"
#include "pins.h"
#include "ui_lvgl.h"
#include "ui_port.h"
#include "web_arduino.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <Arduino.h>
#include <Wire.h>

#include "driver/uart.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_random.h"
#include "esp_system.h"
#include "esp_task_wdt.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"

#include "esp_attr.h"

/* Wi-Fi setup needs more than the default 8 KB loop task. */
SET_LOOP_TASK_STACK_SIZE(32768);

static const char *TAG = "ic_main";

#define BOOT_MAGIC 0x1C0B0711u

static RTC_NOINIT_ATTR uint32_t s_boot_magic;
static RTC_NOINIT_ATTR uint32_t s_boot_stage;

static const char *const k_stages[] = {
    "none",
    "setup",
    "board",
    "serial",
    "nvs",
    "config",
    "crypto",
    "audio",
    "core",
    "net",
    "web",
    "tasks",
    "console",
};

static void boot_mark(uint32_t stage) {
    s_boot_magic = BOOT_MAGIC;
    s_boot_stage = stage;
}

static const char *reset_name(esp_reset_reason_t reason) {
    switch (reason) {
    case ESP_RST_POWERON:
        return "power";
    case ESP_RST_SW:
        return "software";
    case ESP_RST_PANIC:
        return "panic";
    case ESP_RST_INT_WDT:
        return "int-wdt";
    case ESP_RST_TASK_WDT:
        return "task-wdt";
    case ESP_RST_WDT:
        return "wdt";
    case ESP_RST_BROWNOUT:
        return "brownout";
    case ESP_RST_USB:
        return "usb";
    default:
        return "other";
    }
}

static int con_log(const char *fmt, va_list ap) {
    char buf[192];
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    if (n > 0) {
        size_t w = (size_t)n;
        if (w >= sizeof(buf)) {
            w = sizeof(buf) - 1;
        }
        uart_write_bytes(UART_NUM_0, buf, w);
    }
    return n;
}

static void serial_open(void) {
    /* UART0 is the CH343 Type-C port. The native USB port is not a console. */
    Serial0.begin(115200);
    esp_log_set_vprintf(con_log);
    delay(20);
}

/* EXIO5 on the board IO expander. Low connects GPIO 19/20 to the USB socket.
 * All eight pins are outputs. 0xDF keeps EXIO5 low and leaves EXIO2 (backlight)
 * high. These ST7262 panels keep a shadow of the last picture, and the edges
 * go pale, when the backlight is switched off. EXIO6 stays high (panel power).
 * 0xFF would flip the mux to CAN. */
static bool board_usb_select(void) {
    uint8_t err;
    Wire.begin(PIN_IOEXP_SDA, PIN_IOEXP_SCL);
    Wire.setClock(100000);
    /* The GT911 stretches SCL while it samples. A short wait looks like no touch. */
    Wire.setTimeOut(100);
    Wire.beginTransmission(0x24);
    Wire.write(0x02);
    Wire.write(0xFF);
    err = Wire.endTransmission();
    if (err != 0) {
        ESP_LOGE(TAG, "USB socket select mode failed (%u)", err);
        Serial0.println("panel: IO expander did not answer");
        return false;
    }
    Wire.beginTransmission(0x24);
    Wire.write(0x03);
    Wire.write(0xDF);
    err = Wire.endTransmission();
    if (err != 0) {
        ESP_LOGE(TAG, "USB socket select failed (%u)", err);
        Serial0.println("panel: IO expander did not answer");
        return false;
    }
    ESP_LOGI(TAG, "onboard USB socket selected for the headset");
    Serial0.println("panel: expander up, USB selected");
    return true;
}

static void halt_crypto(void) {
    ESP_LOGE(TAG, "crypto self-test failed");
    Serial0.println("crypto self-test failed");
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(200));
        esp_task_wdt_reset();
    }
}

static void load_nvs(void) {
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "erasing NVS");
        nvs_flash_erase();
        err = nvs_flash_init();
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs init failed");
        abort();
    }
}

void ic_setup(void) {
    ic_identity_t id;
    ic_out_t out;
    int self;
    int i;
    char ip_text[16];
    bool audio_ok;
    uint32_t prev_stage = 0;
    const char *prev_name = k_stages[0];

    if (s_boot_magic == BOOT_MAGIC && s_boot_stage < (sizeof(k_stages) / sizeof(k_stages[0]))) {
        prev_stage = s_boot_stage;
        prev_name = k_stages[prev_stage];
    }
    boot_mark(1);
    boot_mark(2);
    serial_open();
    boot_mark(3);
    Serial0.printf("ic boot previous %s (%u) reset %s\n", prev_name, (unsigned)prev_stage,
                   reset_name(esp_reset_reason()));
    Serial0.flush();

    load_nvs();
    boot_mark(4);
    ic_config_load(&g_app.cfg);
    boot_mark(5);
    if (ic_crypto_selftest() != 0) {
        halt_crypto();
    }
    boot_mark(6);

    board_usb_select();
    if (!panel_7b_start()) {
        ESP_LOGE(TAG, "panel is dark");
    } else if (!lvgl_port_start()) {
        ESP_LOGE(TAG, "lvgl did not start");
        Serial0.println("lvgl did not start");
    } else {
        /* Splash is drawn before the core mutex exists. ui_lvgl_build does not take it. */
        lvgl_port_lock();
        ui_lvgl_build();
        lvgl_port_unlock();
        lvgl_port_handler();
    }
    audio_ok = audio_init(g_app.cfg.aec_samples);
    boot_mark(7);
    g_app.audio_ready = audio_stack_ok();
    g_app.sig_fd = -1;
    g_app.media_fd = -1;

    memset(&id, 0, sizeof(id));
    id.station_id = g_app.cfg.station_id;
    snprintf(id.name, sizeof(id.name), "%s", g_app.cfg.name);
    memcpy(id.key, g_app.cfg.key, IC_KEY_LEN);
    id.boot_id = esp_random();
    if (id.boot_id == 0) {
        id.boot_id = 1;
    }
    esp_read_mac(id.mac, ESP_MAC_BASE);
    self = (int)(id.station_id - IC_STATION_MIN);
    for (i = 0; i < IC_STATION_COUNT; i++) {
        id.peer_ip[i] = g_app.cfg.peer_ip[i];
    }
    if (self >= 0 && self < IC_STATION_COUNT) {
        id.peer_ip[self] = g_app.cfg.ip;
    }
    /* Ready on the glass follows a real microphone read, not the claim. */
    id.audio_ok = false;

    g_app.lock = xSemaphoreCreateMutex();
    g_app.sig_q = xQueueCreate(32, sizeof(ic_tx_pkt_t));
    if (!g_app.lock || !g_app.sig_q) {
        ESP_LOGE(TAG, "queue allocation failed");
        abort();
    }
    ic_core_init(&g_app.core, &id);
    boot_mark(8);
    ic_core_set_volume(&g_app.core, g_app.cfg.volume);
    ic_core_set_mic_gain(&g_app.core, g_app.cfg.mic_gain);
    ic_core_set_link(&g_app.core, false, app_now_ms(), &out);

    ic_format_ip(g_app.cfg.ip, ip_text, sizeof(ip_text));
    ESP_LOGI(TAG, "station %u %s %s boot %08x audio %s reset %d",
             (unsigned)id.station_id, id.name, ip_text, (unsigned)id.boot_id,
             audio_ok ? "streaming" : "waiting", (int)esp_reset_reason());
    if (ic_config_key_is_default(&g_app.cfg) || ic_config_pin_is_default(&g_app.cfg)) {
        ESP_LOGW(TAG, "default key or PIN is still in use");
    }

    if (net_start(&g_app.cfg) != 0) {
        ESP_LOGE(TAG, "network did not start");
        Serial0.println("ic network did not start");
    }
    ui_meters_sample();
    if (lvgl_port_ready()) {
        lvgl_port_lock();
        ui_lvgl_boot(app_now_ms());
        lvgl_port_unlock();
        lvgl_port_handler();
    }
    boot_mark(9);
    ui_start_task();
    if (web_start() != 0) {
        ESP_LOGE(TAG, "web did not start");
        Serial0.println("ic web did not start");
    }
    boot_mark(10);
    if (g_app.audio_ready) {
        audio_start_task();
    }
    boot_mark(11);
    console_start();
    boot_mark(12);
}

void ic_loop(void) {
    console_poll();
    delay(10);
}
