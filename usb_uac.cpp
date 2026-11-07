#include "usb_uac.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include <Arduino.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_log.h"
#include "esp_private/usb_phy.h"
#include "panel_7b.h"
#include "usb/usb_host.h"

/* GPIO 19 (D-) and GPIO 20 (D+) are the Waveshare board's USB Type-C socket.
 * EXIO5 low connects those pins to the socket. High connects them to CAN.
 * The chip powers up in CAN mode, so the pin is driven low again here,
 * just before the host starts. USB-Serial-JTAG owns the internal PHY until
 * an OTG host PHY is installed. USB CDC stays off. The UART Type-C port
 * is the console.
 *
 * The ESP32-S3 USB FIFO is 200 lines of 4 bytes. 96+32+80 is 208 and the
 * host rejects it, then the root port reset fails. 72+16+96 is 184 and
 * still holds a 192-byte full-speed packet in each direction.
 */

static const char *TAG = "ic_usb";
static usb_phy_handle_t s_phy = NULL;

#define RING 1024
#define RING_MASK (RING - 1)
#define ISOC_PKTS 4
#define URB_COUNT 2
#define MAX_PAYLOAD 288
#define MAX_ALTS 12
#define MAX_UNITS 6
#define MAX_FREQS 8

extern "C" void USBHostBoardInit(void) __attribute__((weak));

struct alt_cand {
    uint8_t iface;
    uint8_t alt;
    uint8_t ep;
    uint8_t channels;
    uint8_t sub;
    uint8_t bits;
    uint8_t interval;
    uint16_t mps;
    bool in_dir;
    bool continuous;
    uint8_t nfreq;
    uint32_t freq[MAX_FREQS];
    uint32_t fmin;
    uint32_t fmax;
    int score;
    int rate;
};

struct stream_cfg {
    bool ok;
    uint8_t iface;
    uint8_t alt;
    uint8_t ep;
    uint8_t channels;
    uint8_t sub;
    uint16_t mps;
    int rate;
};

static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static char s_status[120] = "usb waiting";

static int16_t s_mic_ring[RING];
static int16_t s_spk_ring[RING];
static uint16_t s_mic_r;
static uint16_t s_mic_w;
static uint16_t s_spk_r;
static uint16_t s_spk_w;

static usb_host_client_handle_t s_client;
static usb_device_handle_t s_dev;
static usb_transfer_t *s_ctrl;
static usb_transfer_t *s_in_urb[URB_COUNT];
static usb_transfer_t *s_out_urb[URB_COUNT];

static stream_cfg s_in;
static stream_cfg s_out;
static bool s_in_claimed;
static bool s_out_claimed;
static volatile bool s_streaming;
static volatile bool s_installed;
static volatile int s_inflight;
static int s_max_payload = 288;
/* IN must be asked for a whole max packet. 256 leaves a status word in the
 * 72-line RX FIFO. OUT may send a shorter packet than the endpoint allows. */
static int s_in_limit = 256;
static int s_out_limit = 288;
static volatile int s_in_cbs;
static volatile int s_in_bytes;
static volatile int s_in_status = -1;
/* Set from the microphone callback. The client task re-issues SET_INTERFACE. */
static volatile bool s_alt_retry;
static bool s_alt_retried;
static bool s_out_logged;
static bool s_total_logged;
static bool s_log_next_in;

static volatile uint8_t s_new_addr;
static volatile bool s_have_new;
static volatile bool s_gone;
static usb_device_handle_t s_gone_hdl;

static volatile bool s_ctrl_done = true;
static volatile bool s_ctrl_busy;
static volatile usb_transfer_status_t s_ctrl_status;

static int32_t s_dec_sum;
static int s_dec_n;
static uint32_t s_dec_acc;
static uint32_t s_rep_acc;
static uint32_t s_rep_left;
static int16_t s_hold;
static uint32_t s_pkt_acc;

static uint8_t s_units[MAX_UNITS];
static uint8_t s_unit_iface[MAX_UNITS];
static int s_nunits;

#define NOTE_MAX 28
static char s_notes[NOTE_MAX][96];
static int s_nnotes;
static char s_why[60];

static void set_status(const char *text) {
    portENTER_CRITICAL(&s_mux);
    snprintf(s_status, sizeof(s_status), "%s", text);
    portEXIT_CRITICAL(&s_mux);
}

void usb_uac_status(char *dst, size_t cap) {
    if (!dst || cap == 0) {
        return;
    }
    portENTER_CRITICAL(&s_mux);
    snprintf(dst, cap, "%s", s_status);
    portEXIT_CRITICAL(&s_mux);
}

bool usb_uac_installed(void) {
    return s_installed;
}

bool usb_uac_streaming(void) {
    return s_streaming;
}

void usb_uac_mic_io(int *callbacks, int *bytes, int *status) {
    if (callbacks) {
        *callbacks = s_in_cbs;
    }
    if (bytes) {
        *bytes = s_in_bytes;
    }
    if (status) {
        *status = s_in_status;
    }
}

static int ring_used(uint16_t w, uint16_t r) {
    return (int)((w - r) & RING_MASK);
}

static void mic_push_one(int16_t s) {
    uint16_t next = (uint16_t)((s_mic_w + 1) & RING_MASK);
    if (next == s_mic_r) {
        s_mic_r = (uint16_t)((s_mic_r + 1) & RING_MASK);
    }
    s_mic_ring[s_mic_w] = s;
    s_mic_w = next;
}

static void mic_feed(int16_t sample) {
    int16_t out;
    bool emit = false;
    if (s_in.rate < 8000) {
        return;
    }
    s_dec_sum += sample;
    s_dec_n++;
    s_dec_acc += 8000;
    if (s_dec_acc >= (uint32_t)s_in.rate) {
        s_dec_acc -= (uint32_t)s_in.rate;
        out = s_dec_n > 0 ? (int16_t)(s_dec_sum / s_dec_n) : 0;
        s_dec_sum = 0;
        s_dec_n = 0;
        emit = true;
    }
    if (!emit) {
        return;
    }
    portENTER_CRITICAL(&s_mux);
    mic_push_one(out);
    portEXIT_CRITICAL(&s_mux);
}

static int16_t spk_take(bool *ok) {
    int16_t s = 0;
    portENTER_CRITICAL(&s_mux);
    if (s_spk_r == s_spk_w) {
        *ok = false;
    } else {
        s = s_spk_ring[s_spk_r];
        s_spk_r = (uint16_t)((s_spk_r + 1) & RING_MASK);
        *ok = true;
    }
    portEXIT_CRITICAL(&s_mux);
    return s;
}

static void rings_reset(void) {
    portENTER_CRITICAL(&s_mux);
    s_mic_r = 0;
    s_mic_w = 0;
    s_spk_r = 0;
    s_spk_w = 0;
    portEXIT_CRITICAL(&s_mux);
    s_dec_sum = 0;
    s_dec_n = 0;
    s_dec_acc = 0;
    s_rep_acc = 0;
    s_rep_left = 0;
    s_hold = 0;
    s_pkt_acc = 0;
}

bool usb_uac_read(int16_t *mono, int n, int timeout_ms) {
    TickType_t start;
    int got = 0;
    if (!mono || n <= 0 || !s_streaming) {
        return false;
    }
    start = xTaskGetTickCount();
    while (got < n) {
        int chunk;
        int i;
        portENTER_CRITICAL(&s_mux);
        chunk = ring_used(s_mic_w, s_mic_r);
        if (chunk > n - got) {
            chunk = n - got;
        }
        for (i = 0; i < chunk; i++) {
            mono[got++] = s_mic_ring[s_mic_r];
            s_mic_r = (uint16_t)((s_mic_r + 1) & RING_MASK);
        }
        portEXIT_CRITICAL(&s_mux);
        if (got >= n) {
            return true;
        }
        if (!s_streaming) {
            return false;
        }
        if ((xTaskGetTickCount() - start) >= pdMS_TO_TICKS(timeout_ms)) {
            return false;
        }
        vTaskDelay(pdMS_TO_TICKS(2));
    }
    return true;
}

bool usb_uac_write(const int16_t *mono, int n, int timeout_ms) {
    TickType_t start;
    int put = 0;
    if (!mono || n <= 0 || !s_streaming) {
        return false;
    }
    start = xTaskGetTickCount();
    while (put < n) {
        int used;
        int space;
        int chunk;
        int i;
        portENTER_CRITICAL(&s_mux);
        used = ring_used(s_spk_w, s_spk_r);
        space = (RING - 1) - used;
        chunk = n - put;
        if (chunk > space) {
            chunk = space;
        }
        for (i = 0; i < chunk; i++) {
            s_spk_ring[s_spk_w] = mono[put++];
            s_spk_w = (uint16_t)((s_spk_w + 1) & RING_MASK);
        }
        portEXIT_CRITICAL(&s_mux);
        if (put >= n) {
            return true;
        }
        if (!s_streaming) {
            return false;
        }
        if ((xTaskGetTickCount() - start) >= pdMS_TO_TICKS(timeout_ms)) {
            return false;
        }
        vTaskDelay(pdMS_TO_TICKS(2));
    }
    return true;
}

static int32_t load_sub(const uint8_t *p, int sub) {
    if (sub == 2) {
        return (int16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
    }
    if (sub == 3) {
        int32_t w = (int32_t)p[0] | ((int32_t)p[1] << 8) | ((int32_t)p[2] << 16);
        if (w & 0x800000) {
            w |= ~0xFFFFFF;
        }
        return w >> 8;
    }
    {
        int32_t w = (int32_t)p[0] | ((int32_t)p[1] << 8) | ((int32_t)p[2] << 16) | ((int32_t)p[3] << 24);
        return w >> 16;
    }
}

static void store_sub(uint8_t *p, int sub, int16_t s) {
    if (sub == 2) {
        p[0] = (uint8_t)s;
        p[1] = (uint8_t)((uint16_t)s >> 8);
        return;
    }
    if (sub == 3) {
        int32_t w = (int32_t)s << 8;
        p[0] = (uint8_t)w;
        p[1] = (uint8_t)(w >> 8);
        p[2] = (uint8_t)(w >> 16);
        return;
    }
    {
        int32_t w = (int32_t)s << 16;
        p[0] = (uint8_t)w;
        p[1] = (uint8_t)(w >> 8);
        p[2] = (uint8_t)(w >> 16);
        p[3] = (uint8_t)(w >> 24);
    }
}

static int16_t mix_frame(const uint8_t *p, int sub, int channels) {
    int32_t acc = 0;
    int c;
    for (c = 0; c < channels; c++) {
        acc += load_sub(p + (c * sub), sub);
    }
    return (int16_t)(acc / channels);
}

static int16_t next_out_sample(void) {
    if (s_rep_left == 0) {
        bool ok = false;
        int16_t s = spk_take(&ok);
        uint32_t reps;
        s_hold = ok ? s : 0;
        s_rep_acc += (uint32_t)s_out.rate;
        reps = s_rep_acc / 8000u;
        s_rep_acc %= 8000u;
        if (reps == 0) {
            reps = 1;
        }
        s_rep_left = reps;
    }
    s_rep_left--;
    return s_hold;
}

static int frames_this_ms(void) {
    int n;
    int maxn;
    int frame;
    s_pkt_acc += (uint32_t)s_out.rate;
    n = (int)(s_pkt_acc / 1000u);
    s_pkt_acc %= 1000u;
    if (n < 1) {
        n = 1;
    }
    frame = (int)s_out.channels * (int)s_out.sub;
    maxn = (int)s_out.mps;
    if (maxn > s_out_limit) {
        maxn = s_out_limit;
    }
    if (maxn > MAX_PAYLOAD) {
        maxn = MAX_PAYLOAD;
    }
    maxn = frame > 0 ? maxn / frame : 1;
    if (maxn < 1) {
        maxn = 1;
    }
    if (n > maxn) {
        n = maxn;
    }
    return n;
}

extern "C" void ic_usb_in_cb(usb_transfer_t *t);
extern "C" void ic_usb_out_cb(usb_transfer_t *t);

static void submit_in(usb_transfer_t *t) {
    int i;
    if (!s_streaming || s_dev == NULL || s_in.mps == 0 || s_in.mps > MAX_PAYLOAD) {
        return;
    }
    t->device_handle = s_dev;
    t->bEndpointAddress = s_in.ep;
    t->callback = ic_usb_in_cb;
    t->context = NULL;
    t->num_bytes = ISOC_PKTS * (int)s_in.mps;
    for (i = 0; i < ISOC_PKTS; i++) {
        t->isoc_packet_desc[i].num_bytes = (int)s_in.mps;
    }
    if (usb_host_transfer_submit(t) == ESP_OK) {
        s_inflight++;
    } else if (s_in_cbs == 0) {
        Serial0.printf("usb: mic submit failed ep %02x mps %u\n", s_in.ep, (unsigned)s_in.mps);
        ESP_LOGE(TAG, "microphone submit failed");
    }
}

static void submit_out(usb_transfer_t *t) {
    int off = 0;
    int i;
    if (!s_streaming || s_dev == NULL || s_out.sub == 0 || s_out.channels == 0) {
        return;
    }
    t->device_handle = s_dev;
    t->bEndpointAddress = s_out.ep;
    t->callback = ic_usb_out_cb;
    t->context = NULL;
    for (i = 0; i < ISOC_PKTS; i++) {
        int frames = frames_this_ms();
        int frame = (int)s_out.channels * (int)s_out.sub;
        int nbytes = frames * frame;
        int s;
        int c;
        uint8_t *p = t->data_buffer + off;
        for (s = 0; s < frames; s++) {
            int16_t sample = next_out_sample();
            for (c = 0; c < s_out.channels; c++) {
                store_sub(p + ((s * s_out.channels + c) * s_out.sub), s_out.sub, sample);
            }
        }
        t->isoc_packet_desc[i].num_bytes = nbytes;
        off += nbytes;
    }
    t->num_bytes = off;
    if (off <= 0 || off > (int)t->data_buffer_size) {
        ESP_LOGE(TAG, "speaker packet %d does not fit", off);
        return;
    }
    {
        esp_err_t err = usb_host_transfer_submit(t);
        if (!s_out_logged) {
            s_out_logged = true;
            Serial0.printf("usb: spk submit %s bytes %d mps %u\n",
                           err == ESP_OK ? "ok" : "failed", off, (unsigned)s_out.mps);
        }
        if (err == ESP_OK) {
            s_inflight++;
        } else {
            ESP_LOGE(TAG, "speaker submit failed");
        }
    }
}

static void mic_take(const uint8_t *p, int nbytes) {
    int frame = (int)s_in.channels * (int)s_in.sub;
    int samples;
    int s;
    if (nbytes <= 0 || frame <= 0) {
        return;
    }
    s_in_bytes = s_in_bytes + nbytes;
    samples = nbytes / frame;
    for (s = 0; s < samples; s++) {
        mic_feed(mix_frame(p + (s * frame), s_in.sub, s_in.channels));
    }
}

extern "C" void ic_usb_in_cb(usb_transfer_t *t) {
    int off = 0;
    int i;
    int nbytes0 = 0;
    int total = t->actual_num_bytes;
    bool any = false;
    s_inflight--;
    s_in_cbs = s_in_cbs + 1;
    s_in_status = (int)t->status;
    if (t->num_isoc_packets > 0) {
        nbytes0 = t->isoc_packet_desc[0].actual_num_bytes;
    }
    if (s_log_next_in && t->num_isoc_packets > 0) {
        s_log_next_in = false;
        Serial0.printf("usb: mic urb %d total %d packet %d bytes %d mps %u\n",
                       (int)t->status, total, (int)t->isoc_packet_desc[0].status, nbytes0,
                       (unsigned)s_in.mps);
    }
    if (s_streaming && t->status == USB_TRANSFER_STATUS_COMPLETED && s_in.sub > 0) {
        int nominal = ((s_in.rate + 999) / 1000) * (int)s_in.channels * (int)s_in.sub;
        for (i = 0; i < t->num_isoc_packets; i++) {
            if (t->isoc_packet_desc[i].actual_num_bytes > 0) {
                any = true;
                break;
            }
        }
        if (any) {
            for (i = 0; i < t->num_isoc_packets; i++) {
                int nbytes = t->isoc_packet_desc[i].actual_num_bytes;
                if (t->isoc_packet_desc[i].status == USB_TRANSFER_STATUS_COMPLETED && nbytes > 0) {
                    mic_take(t->data_buffer + off, nbytes);
                }
                off += t->isoc_packet_desc[i].num_bytes;
            }
        } else if (total > 0 && nominal > 0 && nominal <= (int)s_in.mps && (total % nominal) == 0) {
            int left = total;
            /* The host summed the payload onto the transfer and left each
             * packet count at zero. Samples sit at the requested stride. */
            if (!s_total_logged) {
                s_total_logged = true;
                Serial0.printf("usb: mic packet counts empty, urb total %d\n", total);
            }
            for (i = 0; i < t->num_isoc_packets && left > 0; i++) {
                int n = nominal;
                if (n > left) {
                    n = left;
                }
                mic_take(t->data_buffer + off, n);
                left -= n;
                off += t->isoc_packet_desc[i].num_bytes;
            }
        }
    }
    if (!s_alt_retried && s_in_cbs >= 25 && s_in_bytes == 0) {
        s_alt_retry = true;
    }
    if (s_streaming && s_dev != NULL) {
        submit_in(t);
    }
}

extern "C" void ic_usb_out_cb(usb_transfer_t *t) {
    s_inflight--;
    if (s_streaming && s_dev != NULL) {
        submit_out(t);
    }
}

extern "C" void ic_usb_ctrl_cb(usb_transfer_t *t) {
    s_ctrl_status = t->status;
    s_ctrl_busy = false;
    s_ctrl_done = true;
}

static esp_err_t ctrl_xfer(uint8_t bm, uint8_t req, uint16_t val, uint16_t idx, const uint8_t *data, uint16_t len) {
    uint8_t *b;
    TickType_t start;
    esp_err_t err;
    if (!s_ctrl || !s_dev || s_ctrl_busy) {
        return ESP_ERR_INVALID_STATE;
    }
    b = s_ctrl->data_buffer;
    b[0] = bm;
    b[1] = req;
    b[2] = (uint8_t)val;
    b[3] = (uint8_t)(val >> 8);
    b[4] = (uint8_t)idx;
    b[5] = (uint8_t)(idx >> 8);
    b[6] = (uint8_t)len;
    b[7] = (uint8_t)(len >> 8);
    if (len > 0 && data != NULL) {
        memcpy(b + 8, data, len);
    }
    s_ctrl->num_bytes = 8 + (int)len;
    s_ctrl->device_handle = s_dev;
    s_ctrl->bEndpointAddress = 0;
    s_ctrl->callback = ic_usb_ctrl_cb;
    s_ctrl->context = NULL;
    s_ctrl_done = false;
    s_ctrl_busy = true;
    err = usb_host_transfer_submit_control(s_client, s_ctrl);
    if (err != ESP_OK) {
        s_ctrl_busy = false;
        s_ctrl_done = true;
        return err;
    }
    start = xTaskGetTickCount();
    while (!s_ctrl_done) {
        if ((xTaskGetTickCount() - start) > pdMS_TO_TICKS(800)) {
            ESP_LOGE(TAG, "control transfer still pending");
            return ESP_ERR_TIMEOUT;
        }
        usb_host_client_handle_events(s_client, pdMS_TO_TICKS(20));
    }
    if (s_ctrl_status != USB_TRANSFER_STATUS_COMPLETED) {
        return ESP_FAIL;
    }
    return ESP_OK;
}

static esp_err_t set_interface(uint8_t iface, uint8_t alt) {
    return ctrl_xfer(0x01, 0x0B, alt, iface, NULL, 0);
}

/* Mute off and 0 dB on the master and the first two channels. A stall means
 * that channel has no such control. Volume 0x0000 is 0 dB. 0x8000 is mute. */
static void arm_units(void) {
    int i;
    int ch;
    for (i = 0; i < s_nunits; i++) {
        uint16_t idx = (uint16_t)((s_units[i] << 8) | s_unit_iface[i]);
        for (ch = 0; ch <= 2; ch++) {
            uint8_t mute = 0;
            uint8_t vol[2] = {0, 0};
            uint16_t mute_cs = (uint16_t)((0x01 << 8) | ch);
            uint16_t vol_cs = (uint16_t)((0x02 << 8) | ch);
            ctrl_xfer(0x21, 0x01, mute_cs, idx, &mute, 1);
            ctrl_xfer(0x21, 0x01, vol_cs, idx, vol, 2);
        }
    }
    Serial0.println("usb: mute and volume sent");
}

static uint32_t rd24(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16);
}

static int choose_rate(const alt_cand *c) {
    static const int pref[] = {16000, 8000, 32000, 48000, 24000, 44100, 22050, 12000, 96000};
    int i;
    int j;
    for (i = 0; i < (int)(sizeof(pref) / sizeof(pref[0])); i++) {
        if (c->continuous) {
            if ((uint32_t)pref[i] >= c->fmin && (uint32_t)pref[i] <= c->fmax) {
                return pref[i];
            }
        } else {
            for (j = 0; j < c->nfreq; j++) {
                if (c->freq[j] == (uint32_t)pref[i]) {
                    return pref[i];
                }
            }
        }
    }
    if (!c->continuous && c->nfreq > 0) {
        return (int)c->freq[0];
    }
    if (c->continuous && c->fmin > 0) {
        return (int)c->fmin;
    }
    return 0;
}

static int packet_bytes(int rate, int channels, int sub) {
    int samples = (rate + 999) / 1000;
    return samples * channels * sub;
}

static void add_note(const char *fmt, ...) {
    va_list ap;
    if (s_nnotes >= NOTE_MAX) {
        return;
    }
    va_start(ap, fmt);
    vsnprintf(s_notes[s_nnotes], sizeof(s_notes[0]), fmt, ap);
    va_end(ap);
    s_nnotes++;
}

static bool rate_listed(const alt_cand *c, int rate) {
    int j;
    if (rate < 8000 || rate > 96000) {
        return false;
    }
    if (c->continuous) {
        return (uint32_t)rate >= c->fmin && (uint32_t)rate <= c->fmax;
    }
    for (j = 0; j < c->nfreq; j++) {
        if (c->freq[j] == (uint32_t)rate) {
            return true;
        }
    }
    return false;
}

/* room is the bytes this host can move in one millisecond: the endpoint max,
 * the transfer buffer, and the FIFO limit. A 48 kHz 16-bit stereo packet is
 * 192 bytes. An endpoint may advertise more than that. */
static int payload_room(const alt_cand *c) {
    int room = (int)c->mps;
    int limit = c->in_dir ? s_in_limit : s_out_limit;
    if (c->in_dir) {
        /* A short IN request is not a multiple of the endpoint max packet. */
        if (room > MAX_PAYLOAD || room > limit) {
            return 0;
        }
        return room;
    }
    if (room > MAX_PAYLOAD) {
        room = MAX_PAYLOAD;
    }
    if (room > limit) {
        room = limit;
    }
    return room;
}

static int score_alt(alt_cand *c) {
    static const int pref[] = {16000, 8000, 32000, 48000, 24000, 44100, 22050, 12000, 96000};
    int room;
    int rate = 0;
    int too_big = 0;
    int rank = (int)(sizeof(pref) / sizeof(pref[0]));
    int i;
    s_why[0] = 0;
    if (c->interval == 0) {
        c->interval = 1;
    }
    if (c->channels < 1 || c->channels > 2) {
        snprintf(s_why, sizeof(s_why), "channels %u", (unsigned)c->channels);
        return -1;
    }
    if (c->sub != 2 && c->sub != 3 && c->sub != 4) {
        snprintf(s_why, sizeof(s_why), "subframe %u", (unsigned)c->sub);
        return -1;
    }
    if (c->interval != 1) {
        snprintf(s_why, sizeof(s_why), "interval %u", (unsigned)c->interval);
        return -1;
    }
    if (c->mps == 0) {
        snprintf(s_why, sizeof(s_why), "mps 0");
        return -1;
    }
    if (c->in_dir && ((int)c->mps > MAX_PAYLOAD || (int)c->mps > s_in_limit)) {
        snprintf(s_why, sizeof(s_why), "mps %u > %d", (unsigned)c->mps, s_in_limit);
        return -1;
    }
    if (!c->continuous && c->nfreq == 0) {
        snprintf(s_why, sizeof(s_why), "no sample rate");
        return -1;
    }
    room = payload_room(c);
    for (i = 0; i < (int)(sizeof(pref) / sizeof(pref[0])); i++) {
        int b;
        if (!rate_listed(c, pref[i])) {
            continue;
        }
        b = packet_bytes(pref[i], c->channels, c->sub);
        if (b > 0 && b <= room) {
            rate = pref[i];
            rank = i;
            break;
        }
        if (too_big == 0 || b < too_big) {
            too_big = b;
        }
    }
    if (rate == 0 && !c->continuous) {
        for (i = 0; i < c->nfreq; i++) {
            int b = packet_bytes((int)c->freq[i], c->channels, c->sub);
            int hz = (int)c->freq[i];
            if (hz < 8000 || hz > 96000) {
                continue;
            }
            if (b > 0 && b <= room) {
                rate = hz;
                break;
            }
            if (too_big == 0 || (b > 0 && b < too_big)) {
                too_big = b;
            }
        }
    }
    if (rate == 0) {
        snprintf(s_why, sizeof(s_why), "packet %d > room %d", too_big, room);
        return -1;
    }
    c->rate = rate;
    return 1000 - (rank * 40) - ((c->sub == 2) ? 0 : 80) - (c->channels * 5);
}

struct parse_rep {
    int ac;
    int as_alts;
    int in_seen;
    int out_seen;
    char miss[80];
};

static void take_alt(alt_cand *cur, bool pcm, alt_cand *best_in, alt_cand *best_out, parse_rep *rep) {
    int sc;
    alt_cand *slot;
    const char *dir;
    if (cur->ep == 0) {
        return;
    }
    dir = cur->in_dir ? "mic" : "spk";
    if (!pcm) {
        snprintf(rep->miss, sizeof(rep->miss), "%s rejected: not PCM", dir);
        add_note("usb: %s if %u alt %u ep %02x not PCM",
                 dir, (unsigned)cur->iface, (unsigned)cur->alt, cur->ep);
        return;
    }
    sc = score_alt(cur);
    add_note("usb: %s if %u alt %u ep %02x %u ch %u-byte %d Hz mps %u %s",
             dir, (unsigned)cur->iface, (unsigned)cur->alt, cur->ep,
             (unsigned)cur->channels, (unsigned)cur->sub, cur->rate,
             (unsigned)cur->mps, sc >= 0 ? "ok" : s_why);
    if (sc < 0) {
        snprintf(rep->miss, sizeof(rep->miss), "%s rejected: %s", dir, s_why);
        return;
    }
    slot = cur->in_dir ? best_in : best_out;
    if (sc > slot->score) {
        cur->score = sc;
        *slot = *cur;
    }
}

static void keep_format(alt_cand *cur, const alt_cand *prev) {
    cur->channels = prev->channels;
    cur->sub = prev->sub;
    cur->bits = prev->bits;
    cur->continuous = prev->continuous;
    cur->nfreq = prev->nfreq;
    cur->fmin = prev->fmin;
    cur->fmax = prev->fmax;
    memcpy(cur->freq, prev->freq, sizeof(cur->freq));
}

static bool parse_device(const usb_config_desc_t *cfg, alt_cand *best_in, alt_cand *best_out,
                         bool *uac2, parse_rep *rep) {
    const uint8_t *p = (const uint8_t *)cfg;
    const uint8_t *end = p + cfg->wTotalLength;
    alt_cand cur;
    bool in_as = false;
    bool in_ac = false;
    bool pcm = false;
    bool tag_known = false;
    uint8_t ac_iface = 0;
    memset(&cur, 0, sizeof(cur));
    memset(best_in, 0, sizeof(*best_in));
    memset(best_out, 0, sizeof(*best_out));
    memset(rep, 0, sizeof(*rep));
    best_in->score = -1;
    best_out->score = -1;
    s_nunits = 0;
    s_nnotes = 0;
    *uac2 = false;
    if (cfg->wTotalLength < 9) {
        return false;
    }
    while (p + 1 < end && p[0] >= 2 && p + p[0] <= end) {
        uint8_t len = p[0];
        uint8_t type = p[1];
        if (type == 0x04 && len >= 9) {
            bool same = in_as && cur.iface == p[2];
            alt_cand prev = cur;
            bool keep_pcm = pcm;
            add_note("usb: if %u alt %u class %02x/%02x/%02x eps %u",
                     (unsigned)p[2], (unsigned)p[3], p[5], p[6], p[7], (unsigned)p[4]);
            take_alt(&cur, pcm, best_in, best_out, rep);
            in_as = false;
            in_ac = false;
            pcm = false;
            tag_known = false;
            memset(&cur, 0, sizeof(cur));
            if (p[5] == 0x01 && p[6] == 0x01) {
                in_ac = true;
                ac_iface = p[2];
                rep->ac++;
                if (p[7] == 0x20) {
                    *uac2 = true;
                }
            } else if (p[5] == 0x01 && p[6] == 0x02 && p[7] != 0x20) {
                in_as = true;
                cur.iface = p[2];
                cur.alt = p[3];
                cur.interval = 1;
                rep->as_alts++;
                /* A later alt of the same interface often repeats no format. */
                if (same) {
                    keep_format(&cur, &prev);
                    pcm = keep_pcm;
                }
            } else if (p[5] == 0x01 && p[7] == 0x20) {
                *uac2 = true;
            }
        } else if (in_ac && type == 0x24 && len >= 5 && p[2] == 0x01) {
            uint16_t adc = (uint16_t)p[3] | ((uint16_t)p[4] << 8);
            if (adc >= 0x0200) {
                *uac2 = true;
            }
        } else if (in_ac && type == 0x24 && len >= 4 && p[2] == 0x06) {
            if (s_nunits < MAX_UNITS) {
                s_units[s_nunits] = p[3];
                s_unit_iface[s_nunits] = ac_iface;
                s_nunits++;
            }
        } else if (in_as && type == 0x24 && len >= 7 && p[2] == 0x01) {
            uint16_t tag = (uint16_t)p[5] | ((uint16_t)p[6] << 8);
            tag_known = true;
            pcm = (tag == 0x0001);
        } else if (in_as && type == 0x24 && len >= 8 && p[2] == 0x02 && p[3] == 0x01) {
            int nfreq = p[7];
            const uint8_t *f = p + 8;
            cur.channels = p[4];
            cur.sub = p[5];
            cur.bits = p[6];
            cur.nfreq = 0;
            cur.continuous = (nfreq == 0);
            if (!tag_known) {
                pcm = true;
            }
            if (cur.continuous) {
                if (f + 6 <= p + len) {
                    cur.fmin = rd24(f);
                    cur.fmax = rd24(f + 3);
                }
            } else {
                int i;
                if (nfreq > MAX_FREQS) {
                    nfreq = MAX_FREQS;
                }
                for (i = 0; i < nfreq && f + 3 <= p + len; i++) {
                    cur.freq[i] = rd24(f);
                    f += 3;
                }
                cur.nfreq = (uint8_t)i;
            }
        } else if (in_as && type == 0x05 && len >= 7) {
            uint8_t attr = p[3];
            uint8_t xfer = attr & 0x03;
            uint8_t usage = (attr >> 4) & 0x03;
            uint16_t mps = (uint16_t)((uint16_t)p[4] | ((uint16_t)p[5] << 8));
            mps &= 0x7FF;
            /* Usage 0 is data. Usage 2 is implicit-feedback data. Usage 1 is
             * a feedback-only endpoint and is not a stream. */
            if (xfer == 0x01 && (usage == 0x00 || usage == 0x02)) {
                if (usage == 0x02) {
                    add_note("usb: ep %02x implicit feedback", p[2]);
                }
                if (cur.ep != 0) {
                    take_alt(&cur, pcm, best_in, best_out, rep);
                    cur.ep = 0;
                    cur.mps = 0;
                    cur.in_dir = false;
                    cur.interval = 1;
                    cur.score = -1;
                }
                cur.ep = p[2];
                cur.in_dir = (p[2] & 0x80) != 0;
                cur.mps = mps;
                cur.interval = p[6];
                if (cur.in_dir) {
                    rep->in_seen++;
                } else {
                    rep->out_seen++;
                }
            } else if (xfer == 0x01 && usage == 0x01) {
                add_note("usb: ep %02x feedback", p[2]);
            }
        }
        p += len;
    }
    take_alt(&cur, pcm && in_as, best_in, best_out, rep);
    return best_in->score >= 0 && best_out->score >= 0;
}

static void stop_audio(void) {
    TickType_t start;
    s_streaming = false;
    s_alt_retry = false;
    if (s_dev != NULL) {
        if (s_in.ep != 0) {
            usb_host_endpoint_halt(s_dev, s_in.ep);
        }
        if (s_out.ep != 0) {
            usb_host_endpoint_halt(s_dev, s_out.ep);
        }
    }
    start = xTaskGetTickCount();
    while ((s_inflight > 0 || s_ctrl_busy) && (xTaskGetTickCount() - start) < pdMS_TO_TICKS(500)) {
        usb_host_client_handle_events(s_client, pdMS_TO_TICKS(10));
    }
    if (s_inflight > 0 || s_ctrl_busy) {
        ESP_LOGE(TAG, "USB still busy, in flight %d", s_inflight);
        return;
    }
    if (s_in_claimed && s_dev != NULL) {
        usb_host_interface_release(s_client, s_dev, s_in.iface);
    }
    if (s_out_claimed && s_dev != NULL && s_out.iface != s_in.iface) {
        usb_host_interface_release(s_client, s_dev, s_out.iface);
    }
    s_in_claimed = false;
    s_out_claimed = false;
    if (s_dev != NULL) {
        usb_host_device_close(s_client, s_dev);
        s_dev = NULL;
    }
    memset(&s_in, 0, sizeof(s_in));
    memset(&s_out, 0, sizeof(s_out));
    rings_reset();
}

static void copy_stream(stream_cfg *dst, const alt_cand *src) {
    memset(dst, 0, sizeof(*dst));
    dst->ok = true;
    dst->iface = src->iface;
    dst->alt = src->alt;
    dst->ep = src->ep;
    dst->channels = src->channels;
    dst->sub = src->sub;
    dst->mps = src->mps;
    dst->rate = src->rate;
}

static bool claim_iface(const stream_cfg *st, bool *claimed) {
    esp_err_t err;
    if (*claimed) {
        return true;
    }
    err = usb_host_interface_claim(s_client, s_dev, st->iface, st->alt);
    if (err != ESP_OK) {
        Serial0.printf("usb: claim iface %u alt %u %s\n",
                       (unsigned)st->iface, (unsigned)st->alt, esp_err_to_name(err));
        ESP_LOGE(TAG, "claim iface %u alt %u: %s", st->iface, st->alt, esp_err_to_name(err));
        return false;
    }
    *claimed = true;
    return true;
}

static bool set_alt(uint8_t iface, uint8_t alt) {
    esp_err_t err = set_interface(iface, alt);
    Serial0.printf("usb: SET_INTERFACE %u alt %u %s\n",
                   (unsigned)iface, (unsigned)alt,
                   err == ESP_OK ? "ok" : esp_err_to_name(err));
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "SET_INTERFACE %u alt %u: %s", iface, alt, esp_err_to_name(err));
        return false;
    }
    /* GHW-123P (0020:0b21) produces no microphone data unless each
     * alternate change is followed by this wait. */
    vTaskDelay(pdMS_TO_TICKS(50));
    return true;
}

/* UAC1 powers up on alternate 0, which has no endpoint. Select that
 * first, then the streaming alternate. */
static bool select_alt(const stream_cfg *st) {
    if (st->alt != 0 && !set_alt(st->iface, 0)) {
        return false;
    }
    return set_alt(st->iface, st->alt);
}

static bool same_alt(void) {
    return s_in.iface == s_out.iface && s_in.alt == s_out.alt;
}

static bool select_alts(void) {
    if (!select_alt(&s_out)) {
        return false;
    }
    if (!same_alt() && !select_alt(&s_in)) {
        return false;
    }
    return true;
}

static void set_rate(const stream_cfg *st) {
    uint8_t rate[3];
    esp_err_t err;
    rate[0] = (uint8_t)st->rate;
    rate[1] = (uint8_t)(st->rate >> 8);
    rate[2] = (uint8_t)(st->rate >> 16);
    err = ctrl_xfer(0x22, 0x01, 0x0100, st->ep, rate, 3);
    Serial0.printf("usb: rate ep %02x %d %s\n", st->ep, st->rate, err == ESP_OK ? "ok" : "kept");
    if (err != ESP_OK) {
        ESP_LOGI(TAG, "endpoint 0x%02x kept its default rate", st->ep);
    }
}

static void prime_stream(void) {
    set_rate(&s_out);
    set_rate(&s_in);
    arm_units();
    vTaskDelay(pdMS_TO_TICKS(50));
}

static void submit_both(void) {
    int i;
    s_log_next_in = true;
    s_out_logged = false;
    for (i = 0; i < URB_COUNT; i++) {
        submit_out(s_out_urb[i]);
        submit_in(s_in_urb[i]);
    }
}

static void start_streams(void) {
    char line[140];
    rings_reset();
    s_in_cbs = 0;
    s_in_bytes = 0;
    s_in_status = -1;
    s_alt_retry = false;
    s_alt_retried = false;
    s_total_logged = false;
    if (!claim_iface(&s_out, &s_out_claimed)) {
        set_status("usb headset claim failed");
        stop_audio();
        return;
    }
    /* One alt can carry both the mic and the headphone endpoint. */
    if (same_alt()) {
        s_in_claimed = true;
    } else if (!claim_iface(&s_in, &s_in_claimed)) {
        set_status("usb headset claim failed");
        stop_audio();
        return;
    }
    /* Claim only allocates the host pipes. The device stays on alternate 0,
     * which answers isochronous IN with empty packets, until SET_INTERFACE. */
    if (!select_alts()) {
        set_status("usb interface did not start");
        stop_audio();
        return;
    }
    if (s_gone || s_dev == NULL) {
        set_status("usb unplugged");
        stop_audio();
        return;
    }
    prime_stream();
    if (s_gone || s_dev == NULL) {
        set_status("usb unplugged");
        stop_audio();
        return;
    }
    s_streaming = true;
    submit_both();
    snprintf(line, sizeof(line),
             "usb spk %dHz/%uch/%u if %u alt %u ep %02x sub %u mic %dHz/%uch/%u if %u alt %u ep %02x sub %u",
             s_out.rate, s_out.channels, (unsigned)s_out.mps,
             (unsigned)s_out.iface, (unsigned)s_out.alt, s_out.ep, (unsigned)s_out.sub,
             s_in.rate, s_in.channels, (unsigned)s_in.mps,
             (unsigned)s_in.iface, (unsigned)s_in.alt, s_in.ep, (unsigned)s_in.sub);
    set_status(line);
    Serial0.println(line);
    ESP_LOGI(TAG, "%s", line);
}

/* The first packets were empty. Stop the pipes, select the alternate again,
 * then submit. Runs on the client task, never from the isochronous callback. */
static void retry_interface(void) {
    TickType_t start;
    s_alt_retried = true;
    s_streaming = false;
    start = xTaskGetTickCount();
    while (s_inflight > 0 && (xTaskGetTickCount() - start) < pdMS_TO_TICKS(200)) {
        usb_host_client_handle_events(s_client, pdMS_TO_TICKS(10));
    }
    if (s_inflight > 0 || s_dev == NULL || s_gone) {
        Serial0.println("usb: interface retry skipped");
        if (s_dev != NULL && !s_gone) {
            s_streaming = true;
        }
        return;
    }
    Serial0.println("usb: microphone still empty, SET_INTERFACE again");
    if (!select_alts()) {
        set_status("usb interface did not start");
        stop_audio();
        return;
    }
    prime_stream();
    if (s_gone || s_dev == NULL) {
        set_status("usb unplugged");
        stop_audio();
        return;
    }
    s_streaming = true;
    submit_both();
}

static void reject_status(const alt_cand *in, const alt_cand *out, const parse_rep *rep,
                          bool uac2, unsigned total, char *dst, size_t cap) {
    if (uac2) {
        snprintf(dst, cap, "usb audio is UAC2, need UAC1");
        return;
    }
    if (total >= 250) {
        snprintf(dst, cap, "usb descriptor too large for this core");
        return;
    }
    if (out->score >= 0 && in->score < 0 && rep->in_seen == 0) {
        snprintf(dst, cap, "usb speaker only, no microphone");
        return;
    }
    if (in->score >= 0 && out->score < 0 && rep->out_seen == 0) {
        snprintf(dst, cap, "usb microphone only, no headphone");
        return;
    }
    if (rep->as_alts == 0) {
        snprintf(dst, cap, "usb device is not USB audio");
        return;
    }
    if (rep->miss[0] != 0) {
        snprintf(dst, cap, "usb %s", rep->miss);
        return;
    }
    snprintf(dst, cap, "usb device has no headset mic and headphone");
}

static void explain_once(const usb_device_desc_t *dd, const usb_config_desc_t *cfg, const parse_rep *rep) {
    static uint32_t told = 0xFFFFFFFFu;
    uint32_t id = ((uint32_t)dd->idVendor << 16) | dd->idProduct;
    int i;
    if (told == id) {
        return;
    }
    told = id;
    Serial0.printf("usb: adapter %04x:%04x class %02x cfgs %u len %u in %d out %d\n",
                   dd->idVendor, dd->idProduct, dd->bDeviceClass,
                   (unsigned)dd->bNumConfigurations, (unsigned)cfg->wTotalLength,
                   s_in_limit, s_out_limit);
    for (i = 0; i < s_nnotes; i++) {
        Serial0.println(s_notes[i]);
    }
    if (rep->as_alts == 0 && cfg->wTotalLength >= 9) {
        const uint8_t *raw = (const uint8_t *)cfg;
        unsigned n = cfg->wTotalLength;
        unsigned off;
        if (n > 96) {
            n = 96;
        }
        for (off = 0; off < n; off += 16) {
            char hex[80];
            unsigned k;
            int used = snprintf(hex, sizeof(hex), "usb:");
            for (k = 0; k < 16 && off + k < n && used > 0 && used < (int)sizeof(hex) - 4; k++) {
                used += snprintf(hex + used, sizeof(hex) - (size_t)used, " %02x", raw[off + k]);
            }
            Serial0.println(hex);
        }
    }
}

static void handle_new(uint8_t addr) {
    const usb_device_desc_t *dd = NULL;
    const usb_config_desc_t *cfg = NULL;
    alt_cand in_alt;
    alt_cand out_alt;
    parse_rep rep;
    bool uac2 = false;
    usb_device_handle_t dev = NULL;
    if (s_dev != NULL) {
        return;
    }
    if (usb_host_device_open(s_client, addr, &dev) != ESP_OK) {
        ESP_LOGE(TAG, "open address %u failed", addr);
        return;
    }
    s_dev = dev;
    if (usb_host_get_device_descriptor(dev, &dd) != ESP_OK ||
        usb_host_get_active_config_descriptor(dev, &cfg) != ESP_OK) {
        ESP_LOGE(TAG, "descriptor read failed");
        stop_audio();
        return;
    }
    ESP_LOGI(TAG, "device %04x:%04x config %u bytes", dd->idVendor, dd->idProduct,
             (unsigned)cfg->wTotalLength);
    if (!parse_device(cfg, &in_alt, &out_alt, &uac2, &rep)) {
        char line[120];
        reject_status(&in_alt, &out_alt, &rep, uac2, cfg->wTotalLength, line, sizeof(line));
        set_status(line);
        Serial0.println(line);
        ESP_LOGE(TAG, "%s", line);
        explain_once(dd, cfg, &rep);
        return;
    }
    if (in_alt.iface == out_alt.iface && in_alt.alt != out_alt.alt) {
        set_status("usb mic and headphone share one interface");
        Serial0.println("usb mic and headphone share one interface");
        ESP_LOGE(TAG, "mic alt %u and speaker alt %u are one interface",
                 (unsigned)in_alt.alt, (unsigned)out_alt.alt);
        return;
    }
    copy_stream(&s_in, &in_alt);
    copy_stream(&s_out, &out_alt);
    Serial0.printf("usb: adapter %04x:%04x\n", dd->idVendor, dd->idProduct);
    if (dd->idVendor == 0x0020 && dd->idProduct == 0x0b21) {
        Serial0.println("usb: GHW-123P");
    }
    {
        int note_i;
        for (note_i = 0; note_i < s_nnotes; note_i++) {
            Serial0.println(s_notes[note_i]);
        }
    }
    ESP_LOGI(TAG, "choose spk iface %u alt %u %d Hz %u ch mps %u",
             s_out.iface, s_out.alt, s_out.rate, s_out.channels, s_out.mps);
    ESP_LOGI(TAG, "choose mic iface %u alt %u %d Hz %u ch mps %u",
             s_in.iface, s_in.alt, s_in.rate, s_in.channels, s_in.mps);
    start_streams();
}

extern "C" void ic_usb_client_cb(const usb_host_client_event_msg_t *event, void *arg) {
    (void)arg;
    if (event->event == USB_HOST_CLIENT_EVENT_NEW_DEV) {
        s_new_addr = event->new_dev.address;
        s_have_new = true;
    } else if (event->event == USB_HOST_CLIENT_EVENT_DEV_GONE) {
        s_gone_hdl = event->dev_gone.dev_hdl;
        s_gone = true;
    }
}

/* This core's host library drops every device when this callback is null. */
extern "C" bool ic_usb_enum_filter(const usb_device_desc_t *dev_desc, uint8_t *bConfigurationValue) {
    static bool logged = false;
    if (bConfigurationValue) {
        *bConfigurationValue = 1;
    }
    if (!logged && dev_desc) {
        logged = true;
        ESP_LOGI(TAG, "adapter %04x:%04x", dev_desc->idVendor, dev_desc->idProduct);
    }
    return true;
}

static void fill_host_config(usb_host_config_t *cfg, bool skip_phy) {
    memset(cfg, 0, sizeof(*cfg));
    cfg->skip_phy_setup = skip_phy;
    cfg->intr_flags = ESP_INTR_FLAG_LEVEL1;
    cfg->enum_filter_cb = ic_usb_enum_filter;
}

/* Move GPIO 19/20 from USB-Serial-JTAG onto the OTG host. */
static bool host_phy(void) {
    usb_phy_config_t phy = {};
    esp_err_t err;
    phy.controller = USB_PHY_CTRL_OTG;
    phy.target = USB_PHY_TARGET_INT;
    phy.otg_mode = USB_OTG_MODE_HOST;
    phy.otg_speed = USB_PHY_SPEED_FULL;
    err = usb_new_phy(&phy, &s_phy);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "OTG PHY %s", esp_err_to_name(err));
        Serial0.printf("usb: OTG PHY %s\n", esp_err_to_name(err));
        return false;
    }
    ESP_LOGI(TAG, "OTG host PHY on GPIO 19/20");
    Serial0.println("usb: OTG host on GPIO 19/20");
    return true;
}

static void lib_task(void *arg) {
    (void)arg;
    while (1) {
        uint32_t flags = 0;
        usb_host_lib_handle_events(portMAX_DELAY, &flags);
        (void)flags;
    }
}

static void client_task(void *arg) {
    usb_host_client_config_t ccfg = {};
    (void)arg;
    ccfg.is_synchronous = false;
    ccfg.max_num_event_msg = 8;
    ccfg.async.client_event_callback = ic_usb_client_cb;
    ccfg.async.callback_arg = NULL;
    if (usb_host_client_register(&ccfg, &s_client) != ESP_OK) {
        ESP_LOGE(TAG, "USB client register failed");
        vTaskDelete(NULL);
        return;
    }
    while (1) {
        usb_host_client_handle_events(s_client, portMAX_DELAY);
        /* stop_audio and control transfers also pump events, which can set
         * s_gone or s_have_new. Drain those before blocking again.
         */
        for (;;) {
            bool busy = false;
            if (s_gone) {
                usb_device_handle_t gone = s_gone_hdl;
                s_gone = false;
                busy = true;
                if (s_dev != NULL && gone == s_dev) {
                    ESP_LOGW(TAG, "headset removed");
                    set_status("usb unplugged");
                    stop_audio();
                }
            }
            if (s_have_new) {
                uint8_t addr = s_new_addr;
                s_have_new = false;
                busy = true;
                handle_new(addr);
            }
            /* A failed open, or a stop that had to wait for a transfer, leaves
             * the device handle set. Close it once nothing is in flight so a
             * replug can enumerate. stop_audio returns without closing while a
             * transfer is still running.
             */
            if (s_dev != NULL && !s_streaming && s_inflight <= 0 && !s_ctrl_busy) {
                busy = true;
                stop_audio();
            }
            if (s_alt_retry && s_dev != NULL && s_streaming) {
                s_alt_retry = false;
                busy = true;
                retry_interface();
            }
            if (!busy) {
                break;
            }
        }
    }
}

static bool alloc_urbs(void) {
    int i;
    if (usb_host_transfer_alloc(64, 0, &s_ctrl) != ESP_OK) {
        return false;
    }
    for (i = 0; i < URB_COUNT; i++) {
        if (usb_host_transfer_alloc(ISOC_PKTS * MAX_PAYLOAD, ISOC_PKTS, &s_in_urb[i]) != ESP_OK) {
            return false;
        }
        if (usb_host_transfer_alloc(ISOC_PKTS * MAX_PAYLOAD, ISOC_PKTS, &s_out_urb[i]) != ESP_OK) {
            return false;
        }
    }
    return true;
}

bool usb_uac_start(void) {
    usb_host_config_t cfg;
    esp_err_t err;
    bool level_low = false;
    bool phy_ok;
    if (s_installed) {
        return true;
    }
    if (USBHostBoardInit != NULL) {
        USBHostBoardInit();
    }
    if (!panel_7b_usb_socket(&level_low)) {
        ESP_LOGE(TAG, "EXIO5 write failed");
        Serial0.println("usb: EXIO5 write failed");
    } else if (!level_low) {
        ESP_LOGE(TAG, "EXIO5 still high");
        Serial0.println("usb: EXIO5 still high, socket is CAN");
    } else {
        ESP_LOGI(TAG, "EXIO5 low, headset socket is USB");
        Serial0.println("usb: EXIO5 low, headset socket is USB");
    }
    vTaskDelay(pdMS_TO_TICKS(20));
    phy_ok = host_phy();
    fill_host_config(&cfg, phy_ok);
    cfg.fifo_settings_custom.rx_fifo_lines = 72;
    cfg.fifo_settings_custom.nptx_fifo_lines = 16;
    cfg.fifo_settings_custom.ptx_fifo_lines = 96;
    err = usb_host_install(&cfg);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "custom USB FIFO rejected (%s), using the core default", esp_err_to_name(err));
        Serial0.printf("usb: custom FIFO rejected (%s), packet limit 96\n", esp_err_to_name(err));
        fill_host_config(&cfg, phy_ok);
        err = usb_host_install(&cfg);
        s_max_payload = 96;
        s_in_limit = 96;
        s_out_limit = 96;
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "usb_host_install %s", esp_err_to_name(err));
        return false;
    }
    if (!alloc_urbs()) {
        ESP_LOGE(TAG, "USB transfer alloc failed");
        return false;
    }
    if (xTaskCreatePinnedToCore(lib_task, "usb_lib", 10240, NULL, 23, NULL, 0) != pdPASS) {
        ESP_LOGE(TAG, "usb lib task failed");
        return false;
    }
    if (xTaskCreatePinnedToCore(client_task, "usb_uac", 10240, NULL, 22, NULL, 0) != pdPASS) {
        ESP_LOGE(TAG, "usb client task failed");
        return false;
    }
    s_installed = true;
    set_status("usb waiting");
    Serial0.printf("usb: host ready, mic limit %d speaker limit %d\n", s_in_limit, s_out_limit);
    ESP_LOGI(TAG, "USB host ready, max packet %d", s_max_payload);
    return true;
}
