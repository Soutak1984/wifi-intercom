#include "audio_port.h"

#include "app.h"
#include "ic_audio.h"
#include "usb_uac.h"

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_log.h"
#include "esp_task_wdt.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"

/* This core sets LWIP_COMPAT_SOCKETS to 0. recvfrom/sendto are not lwIP. */

static const char *TAG = "ic_audio";

static ic_jitter_t s_jitter;
static ic_aec_t s_aec;
static int32_t s_dc_x;
static int32_t s_dc_y;
static volatile int s_aec_samples;
static volatile int s_aec_apply;
static bool s_usb_audio_up;

static int16_t s_mic8[IC_FRAME];
static int16_t s_spk8[IC_FRAME];
static int16_t s_clean[IC_FRAME];
static uint8_t s_med_pkt[IC_MED_MAX_PACKET];

typedef struct med_out {
    uint8_t pkt[IC_MED_MAX_PACKET];
    uint16_t len;
    uint32_t ip;
} med_out_t;

static med_out_t s_med_tx[3];

static uint32_t isqrt(uint32_t x) {
    uint32_t bit = 1u << 30;
    uint32_t result = 0;
    while (bit > x) {
        bit >>= 2;
    }
    while (bit != 0) {
        if (x >= result + bit) {
            x -= result + bit;
            result = (result >> 1) + bit;
        } else {
            result >>= 1;
        }
        bit >>= 2;
    }
    return result;
}

static int16_t sat16(int32_t v) {
    if (v > 32767) {
        return 32767;
    }
    if (v < -32768) {
        return -32768;
    }
    return (int16_t)v;
}

static int16_t dc_block(int16_t x) {
    int32_t y = (int32_t)x - s_dc_x + (s_dc_y * 995) / 1000;
    s_dc_x = x;
    s_dc_y = y;
    return sat16(y);
}

static int16_t apply_gain(int16_t s, int gain) {
    return sat16(((int32_t)s * gain) / 25);
}

static int16_t apply_volume(int16_t s, int volume) {
    return sat16(((int32_t)s * volume) / 100);
}

static uint32_t frame_rms(const int16_t *pcm) {
    uint64_t acc = 0;
    int i;
    for (i = 0; i < IC_FRAME; i++) {
        acc += (int32_t)pcm[i] * (int32_t)pcm[i];
    }
    return isqrt((uint32_t)(acc / IC_FRAME));
}

static void gate(int16_t *pcm) {
    uint32_t rms = frame_rms(pcm);
    int i;
    if (rms >= 180) {
        return;
    }
    for (i = 0; i < IC_FRAME; i++) {
        pcm[i] = (int16_t)(pcm[i] / 4);
    }
}

static int16_t tone_sample(void) {
    static int level;
    static int dir = 1;
    level += dir * 1800;
    if (level > 10000) {
        level = 10000;
        dir = -1;
    } else if (level < -10000) {
        level = -10000;
        dir = 1;
    }
    return (int16_t)level;
}

static void set_audio_flag(bool ok) {
    ic_out_t out;
    app_lock();
    ic_core_set_audio_ok(&g_app.core, ok, app_now_ms(), &out);
    app_unlock();
    app_submit_sig(&out);
}

static uint32_t rd32le(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void drain_media(void) {
    int n;
    for (n = 0; n < 16 && g_app.media_fd >= 0; n++) {
        struct sockaddr_in from;
        socklen_t flen = sizeof(from);
        int r = lwip_recvfrom(g_app.media_fd, s_med_pkt, sizeof(s_med_pkt), 0,
                              (struct sockaddr *)&from, &flen);
        int play;
        if (r < 0) {
            break;
        }
        app_lock();
        play = ic_core_ingest_media(&g_app.core, s_med_pkt, (size_t)r,
                                    ntohl(from.sin_addr.s_addr), app_now_ms(), s_clean);
        app_unlock();
        if (play == 1 && r >= 16) {
            ic_jitter_push(&s_jitter, rd32le(s_med_pkt + 12), s_clean);
            g_app.media_rx++;
        } else {
            g_app.media_drop++;
        }
    }
}

static void send_media(int nlegs) {
    int i;
    for (i = 0; i < nlegs; i++) {
        struct sockaddr_in to;
        int n;
        memset(&to, 0, sizeof(to));
        to.sin_family = AF_INET;
        to.sin_port = htons(IC_MED_PORT);
        to.sin_addr.s_addr = htonl(s_med_tx[i].ip);
        n = lwip_sendto(g_app.media_fd, s_med_tx[i].pkt, s_med_tx[i].len, 0,
                        (struct sockaddr *)&to, sizeof(to));
        if (n == (int)s_med_tx[i].len) {
            g_app.media_tx++;
        } else {
            g_app.media_drop++;
        }
    }
}

static void capture_mic(int gain) {
    int i;
    for (i = 0; i < IC_FRAME; i++) {
        s_mic8[i] = apply_gain(dc_block(s_mic8[i]), gain);
    }
    g_app.mic_rms = frame_rms(s_mic8);
}

static void audio_task(void *arg) {
    int rx_bad = 0;
    bool watch = false;
    (void)arg;
    if (esp_task_wdt_add(NULL) == ESP_OK) {
        watch = true;
    }
    while (1) {
        bool reset;
        bool hear;
        bool local_tone = false;
        int gain;
        int volume;
        int popped;
        ic_state_t state;
        ic_media_plan_t plan;
        int leg;
        int ntx = 0;

        if (watch) {
            esp_task_wdt_reset();
        }
        if (s_aec_apply) {
            ic_aec_init(&s_aec, s_aec_samples);
            s_aec_apply = 0;
        }
        app_lock();
        reset = ic_core_consume_audio_reset(&g_app.core);
        app_unlock();
        if (reset) {
            ic_jitter_reset(&s_jitter);
            ic_aec_reset(&s_aec);
            s_dc_x = 0;
            s_dc_y = 0;
        }

        drain_media();
        if (!usb_uac_read(s_mic8, IC_FRAME, 80)) {
            g_app.usb_rx_err++;
            rx_bad++;
            if (rx_bad == 25) {
                int cbs = 0;
                int bytes = 0;
                int st = -1;
                usb_uac_mic_io(&cbs, &bytes, &st);
                ESP_LOGE(TAG, "USB microphone read failed callbacks %d bytes %d status %d",
                         cbs, bytes, st);
                s_usb_audio_up = false;
                set_audio_flag(false);
            }
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }
        if (!s_usb_audio_up) {
            if (rx_bad >= 25) {
                ESP_LOGI(TAG, "USB microphone read recovered");
            }
            set_audio_flag(true);
            s_usb_audio_up = true;
        }
        rx_bad = 0;

        app_lock();
        gain = ic_core_mic_gain(&g_app.core);
        volume = ic_core_volume(&g_app.core);
        state = ic_core_state(&g_app.core);
        {
            ic_view_t view;
            ic_core_view(&g_app.core, app_now_ms(), &view);
            hear = view.speaker;
        }
        app_unlock();

        capture_mic(gain);
        popped = ic_jitter_pop(&s_jitter, s_spk8);
        if (popped < 0) {
            g_app.plc_frames++;
        }
        for (leg = 0; leg < IC_FRAME; leg++) {
            s_spk8[leg] = apply_volume(s_spk8[leg], volume);
        }
        if (g_app.tone_frames > 0) {
            if (state != IC_ST_IDLE) {
                g_app.tone_frames = 0;
            } else {
                for (leg = 0; leg < IC_FRAME; leg++) {
                    s_spk8[leg] = tone_sample();
                }
                g_app.tone_frames--;
                local_tone = true;
            }
        }
        if (!hear && !local_tone) {
            memset(s_spk8, 0, sizeof(s_spk8));
        }

        ic_aec_play(&s_aec, s_spk8, IC_FRAME);
        ic_aec_capture(&s_aec, s_mic8, s_clean, IC_FRAME);
        gate(s_clean);
        if (!usb_uac_write(s_spk8, IC_FRAME, 80)) {
            g_app.usb_tx_err++;
        }

        if (g_app.media_fd < 0) {
            continue;
        }
        app_lock();
        ic_core_plan_tx(&g_app.core, &plan);
        for (leg = 0; leg < plan.nlegs && leg < 3; leg++) {
            int n = ic_core_build_media(&g_app.core, &plan.legs[leg], s_clean, false,
                                        s_med_tx[ntx].pkt, sizeof(s_med_tx[ntx].pkt));
            if (n > 0) {
                s_med_tx[ntx].len = (uint16_t)n;
                s_med_tx[ntx].ip = plan.legs[leg].ip;
                ntx++;
            }
        }
        app_unlock();
        send_media(ntx);
    }
}

void audio_set_aec_delay(int samples) {
    if (samples < 0) {
        samples = 0;
    }
    if (samples > 1500) {
        samples = 1500;
    }
    s_aec_samples = samples;
    s_aec_apply = 1;
}

bool audio_stack_ok(void) {
    return usb_uac_installed();
}

bool audio_init(int aec_samples) {
    TickType_t start;
    if (aec_samples <= 0) {
        aec_samples = 320;
    }
    ic_jitter_init(&s_jitter, 4);
    ic_aec_init(&s_aec, aec_samples);
    s_aec_samples = aec_samples;
    s_aec_apply = 0;
    if (!usb_uac_start()) {
        ESP_LOGE(TAG, "USB host did not start");
        return false;
    }
    start = xTaskGetTickCount();
    while (!usb_uac_streaming() && (xTaskGetTickCount() - start) < pdMS_TO_TICKS(2500)) {
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    if (!usb_uac_streaming()) {
        ESP_LOGW(TAG, "headset adapter not streaming yet");
        return false;
    }
    ESP_LOGI(TAG, "USB headset ready, AEC delay %d samples at 8 kHz", aec_samples);
    return true;
}

void audio_start_task(void) {
    if (!usb_uac_installed()) {
        g_app.audio_ready = false;
        return;
    }
    if (xTaskCreate(audio_task, "ic_audio", 10240, NULL, 10, NULL) != pdPASS) {
        ESP_LOGE(TAG, "audio task create failed");
        g_app.audio_ready = false;
    }
}
