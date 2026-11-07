#include "ui_port.h"

#include "app.h"
#include "board_power.h"
#include "net_arduino.h"
#include "ui_lvgl.h"

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_task_wdt.h"

static const char *TAG = "ic_ui";

enum {
    UI_SELECT = 1,
    UI_CALL,
    UI_END,
    UI_MIC,
    UI_PAGE,
    UI_PAGE_ALL,
    UI_VOL
};

typedef struct ui_cmd {
    int kind;
    int value;
} ui_cmd_t;

static QueueHandle_t s_cmd_q;
static ui_link_t s_link;
static ui_battery_t s_bat;

static int bars_for(int rssi) {
    if (rssi >= -55) {
        return 4;
    }
    if (rssi >= -65) {
        return 3;
    }
    if (rssi >= -75) {
        return 2;
    }
    if (rssi >= -85) {
        return 1;
    }
    return 0;
}

static void sample_meters(void) {
    int rssi = net_rssi_dbm();
    int mv = 0;
    int pct = -1;
    s_link.up = rssi != 0;
    s_link.rssi_dbm = rssi;
    s_link.bars = rssi != 0 ? bars_for(rssi) : 0;
    if (board_battery_sample(&mv, &pct)) {
        s_bat.millivolts = mv;
        s_bat.percent = pct;
        s_bat.present = pct >= 0;
    }
}

static void post(int kind, int value) {
    ui_cmd_t cmd;
    if (!s_cmd_q) {
        return;
    }
    cmd.kind = kind;
    cmd.value = value;
    (void)xQueueSend(s_cmd_q, &cmd, 0);
}

void ui_select(uint16_t station) {
    post(UI_SELECT, (int)station);
}

void ui_call(void) {
    post(UI_CALL, 0);
}

void ui_end(void) {
    post(UI_END, 0);
}

void ui_mic(void) {
    post(UI_MIC, 0);
}

void ui_page(bool all_floors) {
    post(all_floors ? UI_PAGE_ALL : UI_PAGE, 0);
}

void ui_volume_step(int delta) {
    post(UI_VOL, delta);
}

void ui_link_read(ui_link_t *out) {
    if (out) {
        *out = s_link;
    }
}

void ui_battery_read(ui_battery_t *out) {
    if (out) {
        *out = s_bat;
    }
}

void ui_meters_sample(void) {
    sample_meters();
}

static void apply_cmd(const ui_cmd_t *cmd) {
    ic_out_t out;
    ic_out_clear(&out);
    app_lock();
    if (cmd->kind == UI_SELECT) {
        ic_core_ui_select(&g_app.core, (uint16_t)cmd->value, &out);
    } else if (cmd->kind == UI_CALL) {
        ic_core_ui_call(&g_app.core, app_now_ms(), &out);
    } else if (cmd->kind == UI_MIC) {
        ic_core_ui_mic(&g_app.core, app_now_ms(), &out);
    } else if (cmd->kind == UI_PAGE) {
        ic_core_ui_page(&g_app.core, app_now_ms(), false, &out);
    } else if (cmd->kind == UI_PAGE_ALL) {
        ic_core_ui_page(&g_app.core, app_now_ms(), true, &out);
    } else if (cmd->kind == UI_VOL) {
        ic_core_ui_volume_step(&g_app.core, cmd->value);
    } else {
        ic_core_ui_end(&g_app.core, app_now_ms(), &out);
    }
    app_unlock();
    app_submit_sig(&out);
}

static void ui_task(void *arg) {
    int since_tick = 0;
    int since_sense = 0;
    bool watch = false;
    (void)arg;
    if (esp_task_wdt_add(NULL) == ESP_OK) {
        watch = true;
    }
    sample_meters();
    while (1) {
        ui_cmd_t cmd;
        if (watch) {
            esp_task_wdt_reset();
        }
        net_apply_link();
        while (xQueueReceive(s_cmd_q, &cmd, 0) == pdTRUE) {
            apply_cmd(&cmd);
        }
        since_tick += 10;
        since_sense += 10;
        if (since_tick >= 50) {
            ic_out_t out;
            since_tick = 0;
            app_lock();
            ic_core_tick(&g_app.core, app_now_ms(), &out);
            app_unlock();
            app_submit_sig(&out);
        }
        if (since_sense >= 2000) {
            since_sense = 0;
            sample_meters();
        }
        ui_lvgl_poll(app_now_ms());
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

static bool start_ui_task(uint32_t stack) {
    if (xTaskCreate(ui_task, "ic_ui", stack, NULL, 5, NULL) != pdPASS) {
        return false;
    }
    ESP_LOGI(TAG, "ui task stack %u", (unsigned)stack);
    return true;
}

void ui_start_task(void) {
    s_cmd_q = xQueueCreate(8, sizeof(ui_cmd_t));
    if (!s_cmd_q) {
        ESP_LOGE(TAG, "ui queue create failed");
        return;
    }
    if (start_ui_task(32768) || start_ui_task(20480) || start_ui_task(16384)) {
        return;
    }
    ESP_LOGE(TAG, "ui task create failed, internal free %u",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
}
