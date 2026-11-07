#pragma once

#include "config_store.h"
#include "ic_core.h"

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct app {
    ic_core_t core;
    ic_config_t cfg;
    SemaphoreHandle_t lock;
    QueueHandle_t sig_q;
    int sig_fd;
    int media_fd;
    volatile uint32_t tone_frames;
    volatile uint32_t mic_rms;
    volatile uint32_t usb_rx_err;
    volatile uint32_t usb_tx_err;
    volatile uint32_t media_tx;
    volatile uint32_t media_rx;
    volatile uint32_t media_drop;
    volatile uint32_t plc_frames;
    volatile uint32_t sig_drop;
    volatile uint32_t sig_tx_err;
    bool audio_ready;
} app_t;

extern app_t g_app;

uint32_t app_now_ms(void);
void app_lock(void);
void app_unlock(void);
void app_submit_sig(const ic_out_t *out);

#ifdef __cplusplus
}
#endif
