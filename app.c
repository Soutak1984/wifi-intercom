#include "app.h"

#include "esp_timer.h"

app_t g_app;

uint32_t app_now_ms(void) {
    return (uint32_t)(esp_timer_get_time() / 1000);
}

void app_lock(void) {
    xSemaphoreTake(g_app.lock, portMAX_DELAY);
}

void app_unlock(void) {
    xSemaphoreGive(g_app.lock);
}

void app_submit_sig(const ic_out_t *out) {
    int i;
    if (!out || !g_app.sig_q) {
        return;
    }
    for (i = 0; i < out->npkts; i++) {
        if (xQueueSend(g_app.sig_q, &out->pkts[i], pdMS_TO_TICKS(20)) != pdTRUE) {
            g_app.sig_drop++;
        }
    }
}
