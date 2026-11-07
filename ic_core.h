#pragma once

#include "ic_crypto.h"
#include "ic_proto.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define IC_STATION_MIN 100
#define IC_STATION_MAX 103
#define IC_STATION_COUNT 4
#define IC_NAME_LEN 16
#define IC_OUT_MAX 8
#define IC_STATUS_LEN 40

typedef enum ic_state {
    IC_ST_IDLE = 0,
    IC_ST_CALLING = 1,
    IC_ST_CONNECTED = 2,
    IC_ST_PAGING = 3,
    IC_ST_PAGE_RX = 4
} ic_state_t;

typedef struct ic_identity {
    uint16_t station_id;
    char name[IC_NAME_LEN];
    uint8_t key[IC_KEY_LEN];
    uint32_t boot_id;
    uint8_t mac[6];
    uint32_t peer_ip[IC_STATION_COUNT];
    bool audio_ok;
} ic_identity_t;

typedef struct ic_tx_pkt {
    uint8_t pkt[IC_SIG_MAX_PACKET];
    uint16_t len;
    uint32_t dst_ip;
} ic_tx_pkt_t;

typedef struct ic_out {
    ic_tx_pkt_t pkts[IC_OUT_MAX];
    int npkts;
    char status[IC_STATUS_LEN];
} ic_out_t;

typedef struct ic_peer_view {
    uint16_t id;
    char name[IC_NAME_LEN];
    uint32_t ip;
    bool online;
} ic_peer_view_t;

typedef struct ic_view {
    ic_state_t state;
    uint16_t self_id;
    uint16_t selected;
    uint16_t remote;
    bool mic_armed;
    bool mic_tx;
    bool speaker;
    bool link_up;
    bool audio_ok;
    uint8_t volume;
    uint8_t mic_gain;
    char status[IC_STATUS_LEN];
    char remote_name[IC_NAME_LEN];
    ic_peer_view_t dir[IC_STATION_COUNT];
} ic_view_t;

typedef struct ic_stats {
    uint32_t bad_hmac;
    uint32_t malformed;
    uint32_t replay;
    uint32_t stale;
    uint32_t rate_limited;
    uint32_t rejected_busy;
    uint32_t calls_in;
    uint32_t calls_out;
    uint32_t pages_out;
    uint32_t pages_in;
} ic_stats_t;

typedef struct ic_media_leg {
    uint16_t station;
    uint32_t ip;
    uint32_t session_id;
    uint32_t boot_id;
    uint32_t seq;
    bool mute;
    bool page;
} ic_media_leg_t;

typedef struct ic_media_plan {
    int nlegs;
    ic_media_leg_t legs[3];
} ic_media_plan_t;

typedef struct ic_core ic_core_t;

void ic_out_clear(ic_out_t *out);
void ic_core_init(ic_core_t *core, const ic_identity_t *id);
void ic_core_set_link(ic_core_t *core, bool up, uint32_t now_ms, ic_out_t *out);
void ic_core_set_audio_ok(ic_core_t *core, bool ok, uint32_t now_ms, ic_out_t *out);
int ic_core_legs(const ic_core_t *core);
void ic_core_set_volume(ic_core_t *core, uint8_t volume);
void ic_core_set_mic_gain(ic_core_t *core, uint8_t gain);
uint8_t ic_core_volume(const ic_core_t *core);
uint8_t ic_core_mic_gain(const ic_core_t *core);

void ic_core_ui_select(ic_core_t *core, uint16_t station, ic_out_t *out);
void ic_core_ui_call(ic_core_t *core, uint32_t now_ms, ic_out_t *out);
void ic_core_ui_end(ic_core_t *core, uint32_t now_ms, ic_out_t *out);
void ic_core_ui_mic(ic_core_t *core, uint32_t now_ms, ic_out_t *out);
void ic_core_ui_page(ic_core_t *core, uint32_t now_ms, bool all_floors, ic_out_t *out);
void ic_core_ui_volume_step(ic_core_t *core, int delta);

void ic_core_on_sig(ic_core_t *core, const uint8_t *pkt, size_t len,
                    uint32_t src_ip, uint32_t now_ms, ic_out_t *out);
void ic_core_tick(ic_core_t *core, uint32_t now_ms, ic_out_t *out);

void ic_core_view(const ic_core_t *core, uint32_t now_ms, ic_view_t *view);
void ic_core_stats(const ic_core_t *core, ic_stats_t *stats);
ic_state_t ic_core_state(const ic_core_t *core);
uint32_t ic_core_session(const ic_core_t *core);

void ic_core_plan_tx(ic_core_t *core, ic_media_plan_t *plan);
int ic_core_build_media(ic_core_t *core, const ic_media_leg_t *leg,
                        const int16_t *pcm, bool force_mute,
                        uint8_t *out, size_t cap);
/* Writes IC_FRAME samples. Returns 1 to play, 0 to drop. */
int ic_core_ingest_media(ic_core_t *core, const uint8_t *pkt, size_t len,
                         uint32_t src_ip, uint32_t now_ms, int16_t *pcm);
bool ic_core_consume_audio_reset(ic_core_t *core);

/* Opaque size is fixed so the firmware can allocate the object statically. */
struct ic_core {
    ic_identity_t id;
    uint32_t rng;
    uint32_t tx_sig_seq;
    bool link_up;
    bool audio_ok;
    ic_state_t state;
    uint16_t selected;
    bool mic_armed;
    uint8_t volume;
    uint8_t mic_gain;
    char status[IC_STATUS_LEN];
    bool hello_started;
    uint32_t last_hello_ms;
    bool audio_reset;
    struct {
        uint16_t id;
        char name[IC_NAME_LEN];
        uint32_t ip;
        uint32_t last_hello_ms;
        uint32_t boot_id;
        bool boot_valid;
        bool hello_seen;
        uint32_t rx_sig_max;
        bool rx_sig_valid;
        uint32_t last_session;
        uint8_t last_reply;
        uint16_t last_reason;
    } peers[IC_STATION_COUNT];
    struct {
        uint8_t state;
        uint16_t peer;
        uint32_t session_id;
        uint32_t remote_boot;
        uint32_t invite_seq;
        uint32_t media_seq;
        uint32_t media_rx_max;
        bool media_rx_valid;
        uint32_t last_rx_ms;
        uint32_t last_tx_ms;
        uint32_t start_ms;
        bool page;
        bool caller;
    } legs[3];
    struct {
        uint32_t session_id;
        uint32_t until_ms;
        bool used;
    } tombs[8];
    int tomb_next;
    struct {
        uint32_t ip;
        uint32_t window_ms;
        uint16_t count;
        uint16_t bad_hmac;
        uint32_t block_until;
        bool used;
    } rate[8];
    ic_stats_t stats;
};

enum {
    IC_LEG_EMPTY = 0,
    IC_LEG_OUT = 1,
    IC_LEG_UP = 2
};

#ifdef __cplusplus
}
#endif
