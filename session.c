#include "ic_core.h"

#include "ic_audio.h"

#include <stdio.h>
#include <string.h>

#define HELLO_MS 2000u
#define ONLINE_MS 7000u
#define RETX_MS 300u
#define SETUP_MS 3000u
#define DEAD_MS 5000u
#define TOMB_MS 30000u
#define RATE_MAX 40u
#define BAD_HMAC_MAX 8u

static const char *k_names[IC_STATION_COUNT] = {
    "Reception", "Floor 1", "Floor 2", "Floor 3"
};

static int idx_of(uint16_t id) {
    if (id < IC_STATION_MIN || id > IC_STATION_MAX) {
        return -1;
    }
    return (int)(id - IC_STATION_MIN);
}

static void set_status(ic_core_t *c, const char *text) {
    snprintf(c->status, sizeof(c->status), "%s", text ? text : "");
}

static void set_status_name(ic_core_t *c, const char *prefix, uint16_t station) {
    int idx = idx_of(station);
    const char *name = (idx >= 0) ? c->peers[idx].name : "station";
    snprintf(c->status, sizeof(c->status), "%s%s", prefix, name);
}

static uint32_t rnd(ic_core_t *c) {
    uint32_t x = c->rng ? c->rng : 0x6d2b79f5u;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    if (x == 0) {
        x = 0x6d2b79f5u;
    }
    c->rng = x;
    return x;
}

static uint32_t alloc_seq(ic_core_t *c) {
    c->tx_sig_seq++;
    if (c->tx_sig_seq == 0) {
        c->tx_sig_seq = 1;
    }
    return c->tx_sig_seq;
}

static bool online_at(const ic_core_t *c, int idx, uint32_t now) {
    if (idx < 0 || !c->link_up || !c->peers[idx].hello_seen) {
        return false;
    }
    return (uint32_t)(now - c->peers[idx].last_hello_ms) <= ONLINE_MS;
}

void ic_out_clear(ic_out_t *out) {
    if (out) {
        memset(out, 0, sizeof(*out));
    }
}

static void remember(ic_core_t *c, int peer_i, uint32_t session, uint8_t reply, uint16_t reason) {
    if (peer_i < 0) {
        return;
    }
    c->peers[peer_i].last_session = session;
    c->peers[peer_i].last_reply = reply;
    c->peers[peer_i].last_reason = reason;
}

static int emit(ic_core_t *c, ic_out_t *out, uint32_t dst_ip, uint8_t type,
                uint32_t session, uint16_t dst, uint32_t seq, uint32_t now,
                const uint8_t *payload, uint16_t plen) {
    ic_tx_pkt_t *slot;
    int n;
    if (!out || dst_ip == 0 || out->npkts >= IC_OUT_MAX) {
        return -1;
    }
    slot = &out->pkts[out->npkts];
    n = ic_sig_seal(type, 0, session, c->id.station_id, dst, seq, c->id.boot_id,
                    now, c->id.mac, payload, plen, c->id.key, slot->pkt, sizeof(slot->pkt));
    if (n < 0) {
        return -1;
    }
    slot->len = (uint16_t)n;
    slot->dst_ip = dst_ip;
    out->npkts++;
    return 0;
}

static void put_call_payload(uint8_t p[IC_CALL_PAYLOAD]) {
    memset(p, 0, IC_CALL_PAYLOAD);
    p[0] = IC_CODEC_PCMU;
    p[1] = 20;
}

static void tomb_add(ic_core_t *c, uint32_t session, uint32_t until) {
    if (session == 0) {
        return;
    }
    c->tombs[c->tomb_next].session_id = session;
    c->tombs[c->tomb_next].until_ms = until;
    c->tombs[c->tomb_next].used = true;
    c->tomb_next = (c->tomb_next + 1) % 8;
}

static bool tomb_has(ic_core_t *c, uint32_t session, uint32_t now) {
    int i;
    if (session == 0) {
        return false;
    }
    for (i = 0; i < 8; i++) {
        if (!c->tombs[i].used) {
            continue;
        }
        if (now >= c->tombs[i].until_ms) {
            c->tombs[i].used = false;
            continue;
        }
        if (c->tombs[i].session_id == session) {
            return true;
        }
    }
    return false;
}

static int find_session(const ic_core_t *c, uint32_t session) {
    int i;
    if (session == 0) {
        return -1;
    }
    for (i = 0; i < 3; i++) {
        if (c->legs[i].state != IC_LEG_EMPTY && c->legs[i].session_id == session) {
            return i;
        }
    }
    return -1;
}

static int find_peer_leg(const ic_core_t *c, uint16_t peer) {
    int i;
    for (i = 0; i < 3; i++) {
        if (c->legs[i].state != IC_LEG_EMPTY && c->legs[i].peer == peer) {
            return i;
        }
    }
    return -1;
}

static int count_legs(const ic_core_t *c) {
    int i;
    int n = 0;
    for (i = 0; i < 3; i++) {
        if (c->legs[i].state != IC_LEG_EMPTY) {
            n++;
        }
    }
    return n;
}

static int count_up(const ic_core_t *c) {
    int i;
    int n = 0;
    for (i = 0; i < 3; i++) {
        if (c->legs[i].state == IC_LEG_UP) {
            n++;
        }
    }
    return n;
}

static void refresh_state(ic_core_t *c) {
    int n = count_legs(c);
    int up = count_up(c);
    int i;
    if (n == 0) {
        c->state = IC_ST_IDLE;
        c->mic_armed = false;
        return;
    }
    for (i = 0; i < 3; i++) {
        if (c->legs[i].state == IC_LEG_EMPTY) {
            continue;
        }
        if (c->legs[i].page && c->legs[i].caller) {
            c->state = IC_ST_PAGING;
            return;
        }
        if (c->legs[i].page && !c->legs[i].caller) {
            c->state = IC_ST_PAGE_RX;
            return;
        }
        if (!c->legs[i].page && c->legs[i].caller && c->legs[i].state == IC_LEG_OUT && up == 0) {
            c->state = IC_ST_CALLING;
            return;
        }
    }
    c->state = IC_ST_CONNECTED;
}

static void clear_leg(ic_core_t *c, int li, uint32_t now) {
    if (li < 0 || c->legs[li].state == IC_LEG_EMPTY) {
        return;
    }
    tomb_add(c, c->legs[li].session_id, now + TOMB_MS);
    memset(&c->legs[li], 0, sizeof(c->legs[li]));
    refresh_state(c);
}

static bool peer_is_rate_blocked(ic_core_t *c, uint32_t ip, uint32_t now, bool bad_hmac) {
    int free_i = -1;
    int i;
    for (i = 0; i < 8; i++) {
        if (c->rate[i].used && c->rate[i].ip == ip) {
            if (now < c->rate[i].block_until) {
                c->stats.rate_limited++;
                return true;
            }
            if ((uint32_t)(now - c->rate[i].window_ms) > 1000u) {
                c->rate[i].window_ms = now;
                c->rate[i].count = 0;
                c->rate[i].bad_hmac = 0;
            }
            c->rate[i].count++;
            if (bad_hmac) {
                c->rate[i].bad_hmac++;
            }
            if (c->rate[i].bad_hmac > BAD_HMAC_MAX) {
                c->rate[i].block_until = now + TOMB_MS;
                c->stats.rate_limited++;
                return true;
            }
            if (c->rate[i].count > RATE_MAX) {
                c->stats.rate_limited++;
                return true;
            }
            return false;
        }
        if (!c->rate[i].used && free_i < 0) {
            free_i = i;
        }
    }
    if (free_i < 0) {
        free_i = 0;
    }
    memset(&c->rate[free_i], 0, sizeof(c->rate[free_i]));
    c->rate[free_i].used = true;
    c->rate[free_i].ip = ip;
    c->rate[free_i].window_ms = now;
    c->rate[free_i].count = 1;
    c->rate[free_i].bad_hmac = bad_hmac ? 1 : 0;
    return false;
}

static void send_hello(ic_core_t *c, int peer_i, uint32_t now, ic_out_t *out) {
    uint8_t payload[IC_HELLO_PAYLOAD];
    uint32_t ip;
    uint32_t seq;
    int self;
    memset(payload, 0, sizeof(payload));
    memcpy(payload, c->peers[idx_of(c->id.station_id)].name, IC_NAME_LEN);
    self = idx_of(c->id.station_id);
    ip = (self >= 0) ? c->peers[self].ip : 0;
    payload[16] = (uint8_t)((ip >> 24) & 0xff);
    payload[17] = (uint8_t)((ip >> 16) & 0xff);
    payload[18] = (uint8_t)((ip >> 8) & 0xff);
    payload[19] = (uint8_t)(ip & 0xff);
    seq = alloc_seq(c);
    emit(c, out, c->peers[peer_i].ip, IC_HELLO, 0, c->peers[peer_i].id, seq, now,
         payload, IC_HELLO_PAYLOAD);
}

static void send_reject(ic_core_t *c, ic_out_t *out, int peer_i, uint32_t session,
                        uint16_t reason, uint32_t now) {
    uint8_t payload[2];
    uint32_t seq = alloc_seq(c);
    payload[0] = (uint8_t)(reason & 0xff);
    payload[1] = (uint8_t)((reason >> 8) & 0xff);
    emit(c, out, c->peers[peer_i].ip, IC_REJECT, session, c->peers[peer_i].id, seq, now,
         payload, 2);
    remember(c, peer_i, session, IC_REJECT, reason);
    if (reason == IC_REJ_BUSY) {
        c->stats.rejected_busy++;
    }
}

static void send_accept(ic_core_t *c, ic_out_t *out, int li, uint32_t now, bool page) {
    uint8_t payload[IC_CALL_PAYLOAD];
    int peer_i = idx_of(c->legs[li].peer);
    uint32_t seq = alloc_seq(c);
    put_call_payload(payload);
    emit(c, out, c->peers[peer_i].ip, page ? IC_PAGE_ACCEPT : IC_ACCEPT,
         c->legs[li].session_id, c->legs[li].peer, seq, now, payload, IC_CALL_PAYLOAD);
    remember(c, peer_i, c->legs[li].session_id, page ? IC_PAGE_ACCEPT : IC_ACCEPT, 0);
}

static void send_ack(ic_core_t *c, ic_out_t *out, int li, uint32_t now) {
    int peer_i = idx_of(c->legs[li].peer);
    uint32_t seq = alloc_seq(c);
    emit(c, out, c->peers[peer_i].ip, IC_ACK, c->legs[li].session_id, c->legs[li].peer,
         seq, now, NULL, 0);
}

static void send_bye_leg(ic_core_t *c, ic_out_t *out, int li, uint32_t now) {
    int peer_i;
    uint8_t type;
    uint32_t seq;
    if (li < 0 || c->legs[li].state == IC_LEG_EMPTY) {
        return;
    }
    peer_i = idx_of(c->legs[li].peer);
    if (peer_i < 0 || !c->link_up) {
        return;
    }
    type = c->legs[li].page ? IC_PAGE_END : IC_BYE;
    seq = alloc_seq(c);
    emit(c, out, c->peers[peer_i].ip, type, c->legs[li].session_id, c->legs[li].peer,
         seq, now, NULL, 0);
}

static void send_bye_ack(ic_core_t *c, ic_out_t *out, int peer_i, uint32_t session,
                         bool page, uint32_t now) {
    uint32_t seq = alloc_seq(c);
    emit(c, out, c->peers[peer_i].ip, page ? IC_PAGE_END_ACK : IC_BYE_ACK, session,
         c->peers[peer_i].id, seq, now, NULL, 0);
}

static void become_idle(ic_core_t *c, const char *status) {
    memset(c->legs, 0, sizeof(c->legs));
    c->state = IC_ST_IDLE;
    c->mic_armed = false;
    c->audio_reset = true;
    set_status(c, status);
}

static void end_all(ic_core_t *c, uint32_t now, ic_out_t *out, bool notify, const char *status) {
    int i;
    for (i = 0; i < 3; i++) {
        if (c->legs[i].state == IC_LEG_EMPTY) {
            continue;
        }
        if (notify) {
            send_bye_leg(c, out, i, now);
        }
        tomb_add(c, c->legs[i].session_id, now + TOMB_MS);
    }
    become_idle(c, status);
}

static void describe_up(ic_core_t *c) {
    int i;
    int n = count_legs(c);
    if (c->state == IC_ST_PAGING) {
        if (n > 1) {
            set_status(c, "Paging all floors");
        } else {
            for (i = 0; i < 3; i++) {
                if (c->legs[i].state != IC_LEG_EMPTY) {
                    set_status_name(c, "Paging ", c->legs[i].peer);
                    return;
                }
            }
        }
        return;
    }
    if (c->state == IC_ST_PAGE_RX) {
        for (i = 0; i < 3; i++) {
            if (c->legs[i].state != IC_LEG_EMPTY) {
                set_status_name(c, "Page from ", c->legs[i].peer);
                return;
            }
        }
    }
    if (c->state == IC_ST_CALLING) {
        for (i = 0; i < 3; i++) {
            if (c->legs[i].state == IC_LEG_OUT) {
                set_status_name(c, "Calling ", c->legs[i].peer);
                return;
            }
        }
    }
    if (c->state == IC_ST_CONNECTED) {
        for (i = 0; i < 3; i++) {
            if (c->legs[i].state != IC_LEG_EMPTY) {
                set_status_name(c, "In call with ", c->legs[i].peer);
                return;
            }
        }
    }
}

static int alloc_leg(ic_core_t *c) {
    int i;
    for (i = 0; i < 3; i++) {
        if (c->legs[i].state == IC_LEG_EMPTY) {
            return i;
        }
    }
    return -1;
}

static void open_outgoing(ic_core_t *c, uint16_t peer, uint32_t now, ic_out_t *out, bool page) {
    int li = alloc_leg(c);
    int peer_i = idx_of(peer);
    uint8_t payload[IC_CALL_PAYLOAD];
    uint32_t session;
    uint32_t seq;
    if (li < 0 || peer_i < 0) {
        return;
    }
    session = rnd(c);
    if (session == 0) {
        session = 1;
    }
    seq = alloc_seq(c);
    memset(&c->legs[li], 0, sizeof(c->legs[li]));
    c->legs[li].state = IC_LEG_OUT;
    c->legs[li].peer = peer;
    c->legs[li].session_id = session;
    c->legs[li].remote_boot = c->peers[peer_i].boot_id;
    c->legs[li].invite_seq = seq;
    c->legs[li].start_ms = now;
    c->legs[li].last_tx_ms = now;
    c->legs[li].last_rx_ms = now;
    c->legs[li].page = page;
    c->legs[li].caller = true;
    put_call_payload(payload);
    emit(c, out, c->peers[peer_i].ip, page ? IC_PAGE_START : IC_INVITE, session, peer, seq, now,
         payload, IC_CALL_PAYLOAD);
    refresh_state(c);
    c->audio_reset = true;
    describe_up(c);
}

static void accept_incoming(ic_core_t *c, const ic_sig_msg_t *msg, uint32_t now,
                            ic_out_t *out, bool page) {
    int li;
    int peer_i = idx_of(msg->src);
    if (peer_i < 0) {
        return;
    }
    li = alloc_leg(c);
    if (li < 0) {
        send_reject(c, out, peer_i, msg->session_id, IC_REJ_BUSY, now);
        return;
    }
    memset(&c->legs[li], 0, sizeof(c->legs[li]));
    c->legs[li].state = IC_LEG_UP;
    c->legs[li].peer = msg->src;
    c->legs[li].session_id = msg->session_id;
    c->legs[li].remote_boot = msg->boot_id;
    c->legs[li].start_ms = now;
    c->legs[li].last_rx_ms = now;
    c->legs[li].last_tx_ms = now;
    c->legs[li].page = page;
    c->legs[li].caller = false;
    c->peers[peer_i].boot_id = msg->boot_id;
    c->peers[peer_i].boot_valid = true;
    if (page) {
        c->mic_armed = false;
        c->stats.pages_in++;
    } else {
        c->stats.calls_in++;
    }
    refresh_state(c);
    c->audio_reset = true;
    describe_up(c);
    send_accept(c, out, li, now, page);
}

static bool expected_payload(const ic_sig_msg_t *msg) {
    switch (msg->type) {
    case IC_HELLO:
        return msg->payload_len == IC_HELLO_PAYLOAD;
    case IC_INVITE:
    case IC_ACCEPT:
    case IC_PAGE_START:
    case IC_PAGE_ACCEPT:
        return msg->payload_len == IC_CALL_PAYLOAD && msg->payload[0] == IC_CODEC_PCMU;
    case IC_REJECT:
        return msg->payload_len == IC_REJECT_PAYLOAD;
    case IC_ACK:
    case IC_BYE:
    case IC_BYE_ACK:
    case IC_KEEPALIVE:
    case IC_PAGE_END:
    case IC_PAGE_END_ACK:
        return msg->payload_len == 0;
    default:
        return false;
    }
}

static void note_boot_change(ic_core_t *c, int peer_i, uint32_t boot, uint32_t now, ic_out_t *out) {
    int i;
    bool dropped = false;
    if (!c->peers[peer_i].boot_valid) {
        c->peers[peer_i].boot_id = boot;
        c->peers[peer_i].boot_valid = true;
        return;
    }
    if (c->peers[peer_i].boot_id == boot) {
        return;
    }
    for (i = 0; i < 3; i++) {
        if (c->legs[i].state != IC_LEG_EMPTY && c->legs[i].peer == c->peers[peer_i].id) {
            send_bye_leg(c, out, i, now);
            tomb_add(c, c->legs[i].session_id, now + TOMB_MS);
            memset(&c->legs[i], 0, sizeof(c->legs[i]));
            dropped = true;
        }
    }
    c->peers[peer_i].boot_id = boot;
    c->peers[peer_i].boot_valid = true;
    c->peers[peer_i].rx_sig_valid = false;
    refresh_state(c);
    if (dropped && count_legs(c) == 0) {
        c->state = IC_ST_IDLE;
        c->mic_armed = false;
        c->audio_reset = true;
        set_status(c, "Idle");
    }
}

static void handle_hello(ic_core_t *c, const ic_sig_msg_t *msg, uint32_t src_ip,
                         uint32_t now, bool fresh) {
    int peer_i = idx_of(msg->src);
    char name[IC_NAME_LEN];
    (void)fresh;
    if (peer_i < 0) {
        return;
    }
    memcpy(name, msg->payload, IC_NAME_LEN);
    name[IC_NAME_LEN - 1] = 0;
    if (name[0] != 0) {
        memcpy(c->peers[peer_i].name, name, IC_NAME_LEN);
    }
    c->peers[peer_i].ip = src_ip;
    c->peers[peer_i].hello_seen = true;
    c->peers[peer_i].last_hello_ms = now;
}

static void handle_setup(ic_core_t *c, const ic_sig_msg_t *msg, uint32_t now,
                         ic_out_t *out, bool page, bool fresh) {
    int peer_i = idx_of(msg->src);
    int existing;
    int our;
    if (peer_i < 0) {
        return;
    }
    existing = find_session(c, msg->session_id);
    if (existing >= 0 && !c->legs[existing].caller) {
        c->legs[existing].last_rx_ms = now;
        send_accept(c, out, existing, now, page);
        return;
    }
    if (tomb_has(c, msg->session_id, now)) {
        c->stats.stale++;
        return;
    }
    if (!fresh) {
        if (c->peers[peer_i].last_reply == IC_REJECT &&
            c->peers[peer_i].last_session == msg->session_id) {
            send_reject(c, out, peer_i, msg->session_id, c->peers[peer_i].last_reason, now);
        }
        return;
    }
    if (page && msg->src != IC_STATION_MIN) {
        send_reject(c, out, peer_i, msg->session_id, IC_REJ_NOT_ALLOWED, now);
        return;
    }
    if (!c->audio_ok) {
        send_reject(c, out, peer_i, msg->session_id, IC_REJ_AUDIO, now);
        return;
    }
    our = find_peer_leg(c, msg->src);
    if (c->state == IC_ST_CALLING && our >= 0 && c->legs[our].caller && !page) {
        if (c->id.station_id < msg->src) {
            send_reject(c, out, peer_i, msg->session_id, IC_REJ_GLARE, now);
            return;
        }
        /* Higher station id loses glare and takes the lower station's session. */
        {
            bool armed = c->mic_armed;
            tomb_add(c, c->legs[our].session_id, now + TOMB_MS);
            memset(&c->legs[our], 0, sizeof(c->legs[our]));
            refresh_state(c);
            c->mic_armed = armed;
        }
    }
    if (c->state != IC_ST_IDLE || count_legs(c) != 0) {
        send_reject(c, out, peer_i, msg->session_id, IC_REJ_BUSY, now);
        return;
    }
    accept_incoming(c, msg, now, out, page);
}

static void handle_answer(ic_core_t *c, const ic_sig_msg_t *msg, uint32_t now, ic_out_t *out) {
    int li = find_session(c, msg->session_id);
    if (tomb_has(c, msg->session_id, now) && li < 0) {
        c->stats.stale++;
        return;
    }
    if (li < 0 || !c->legs[li].caller) {
        return;
    }
    if (c->legs[li].peer != msg->src) {
        return;
    }
    c->legs[li].state = IC_LEG_UP;
    c->legs[li].remote_boot = msg->boot_id;
    c->legs[li].last_rx_ms = now;
    c->legs[li].last_tx_ms = now;
    refresh_state(c);
    c->audio_reset = true;
    describe_up(c);
    send_ack(c, out, li, now);
}

static void handle_reject(ic_core_t *c, const ic_sig_msg_t *msg, uint32_t now) {
    int li = find_session(c, msg->session_id);
    uint16_t reason;
    bool any_left;
    if (li < 0) {
        return;
    }
    reason = (uint16_t)(msg->payload[0] | ((uint16_t)msg->payload[1] << 8));
    clear_leg(c, li, now);
    any_left = count_legs(c) > 0;
    if (!any_left) {
        c->mic_armed = false;
        c->audio_reset = true;
        if (reason == IC_REJ_BUSY) {
            set_status(c, "Busy");
        } else if (reason == IC_REJ_NOT_ALLOWED) {
            set_status(c, "Rejected");
        } else if (reason == IC_REJ_AUDIO) {
            set_status(c, "Audio fault");
        } else if (reason == IC_REJ_GLARE) {
            set_status(c, "No answer");
        } else {
            set_status(c, "Rejected");
        }
    }
}

static void handle_ack(ic_core_t *c, const ic_sig_msg_t *msg, uint32_t now) {
    int li = find_session(c, msg->session_id);
    if (li >= 0 && c->legs[li].state == IC_LEG_UP) {
        c->legs[li].last_rx_ms = now;
    }
}

static void handle_end(ic_core_t *c, const ic_sig_msg_t *msg, uint32_t now, ic_out_t *out, bool page) {
    int peer_i = idx_of(msg->src);
    int li = find_session(c, msg->session_id);
    if (peer_i < 0) {
        return;
    }
    if (li < 0) {
        /* Ack a repeated end so the peer stops, but do not create a call. */
        send_bye_ack(c, out, peer_i, msg->session_id, page, now);
        c->stats.stale++;
        return;
    }
    if (c->legs[li].peer != msg->src) {
        return;
    }
    send_bye_ack(c, out, peer_i, msg->session_id, page, now);
    clear_leg(c, li, now);
    if (count_legs(c) == 0) {
        c->mic_armed = false;
        c->audio_reset = true;
        set_status(c, "Call ended");
        c->state = IC_ST_IDLE;
    } else {
        describe_up(c);
    }
}

static void handle_keepalive(ic_core_t *c, const ic_sig_msg_t *msg, uint32_t now) {
    int li = find_session(c, msg->session_id);
    if (li >= 0 && c->legs[li].peer == msg->src) {
        c->legs[li].last_rx_ms = now;
    }
}

void ic_core_init(ic_core_t *core, const ic_identity_t *id) {
    int self;
    int i;
    memset(core, 0, sizeof(*core));
    core->id = *id;
    self = idx_of(id->station_id);
    if (self < 0) {
        core->id.station_id = IC_STATION_MIN;
        self = 0;
    }
    if (core->id.name[0] == 0) {
        snprintf(core->id.name, sizeof(core->id.name), "%s", k_names[self]);
    }
    core->id.name[IC_NAME_LEN - 1] = 0;
    core->rng = id->boot_id ? id->boot_id : 1u;
    core->audio_ok = id->audio_ok;
    core->volume = 60;
    core->mic_gain = 40;
    core->state = IC_ST_IDLE;
    for (i = 0; i < IC_STATION_COUNT; i++) {
        core->peers[i].id = (uint16_t)(IC_STATION_MIN + i);
        core->peers[i].ip = id->peer_ip[i];
        snprintf(core->peers[i].name, sizeof(core->peers[i].name), "%s", k_names[i]);
    }
    snprintf(core->peers[self].name, sizeof(core->peers[self].name), "%s", core->id.name);
    set_status(core, "Idle");
}

void ic_core_set_link(ic_core_t *core, bool up, uint32_t now_ms, ic_out_t *out) {
    int i;
    ic_out_clear(out);
    if (!up) {
        if (core->link_up) {
            end_all(core, now_ms, out, true, "Link down");
        } else {
            set_status(core, "Link down");
            core->mic_armed = false;
        }
        core->link_up = false;
        for (i = 0; i < IC_STATION_COUNT; i++) {
            core->peers[i].hello_seen = false;
        }
        core->hello_started = false;
        return;
    }
    core->link_up = true;
    core->hello_started = false;
    if (core->state == IC_ST_IDLE) {
        set_status(core, "Idle");
    }
}

void ic_core_set_audio_ok(ic_core_t *core, bool ok, uint32_t now_ms, ic_out_t *out) {
    ic_out_clear(out);
    core->audio_ok = ok;
    core->id.audio_ok = ok;
    if (!ok && core->state != IC_ST_IDLE) {
        end_all(core, now_ms, out, true, "Audio fault");
    }
}

void ic_core_set_volume(ic_core_t *core, uint8_t volume) {
    if (volume > 100) {
        volume = 100;
    }
    core->volume = volume;
}

void ic_core_set_mic_gain(ic_core_t *core, uint8_t gain) {
    if (gain > 100) {
        gain = 100;
    }
    core->mic_gain = gain;
}

uint8_t ic_core_volume(const ic_core_t *core) {
    return core->volume;
}

uint8_t ic_core_mic_gain(const ic_core_t *core) {
    return core->mic_gain;
}

void ic_core_ui_volume_step(ic_core_t *core, int delta) {
    int v = (int)core->volume + delta;
    if (v < 0) {
        v = 0;
    }
    if (v > 100) {
        v = 100;
    }
    core->volume = (uint8_t)v;
}

void ic_core_ui_select(ic_core_t *core, uint16_t station, ic_out_t *out) {
    ic_out_clear(out);
    if (station == core->id.station_id) {
        if (core->state == IC_ST_IDLE) {
            core->selected = 0;
            set_status(core, "This station");
        }
        return;
    }
    if (idx_of(station) < 0) {
        set_status(core, "Select a station");
        return;
    }
    core->selected = station;
    if (core->state == IC_ST_IDLE) {
        set_status_name(core, "Selected ", station);
    }
}

void ic_core_ui_call(ic_core_t *core, uint32_t now_ms, ic_out_t *out) {
    int peer_i;
    ic_out_clear(out);
    if (!core->link_up) {
        set_status(core, "Link down");
        return;
    }
    if (!core->audio_ok) {
        set_status(core, "Audio fault");
        return;
    }
    if (core->state != IC_ST_IDLE) {
        return;
    }
    if (core->selected == 0) {
        set_status(core, "Select a station");
        return;
    }
    if (core->selected == core->id.station_id) {
        set_status(core, "This station");
        return;
    }
    peer_i = idx_of(core->selected);
    if (peer_i < 0) {
        set_status(core, "Select a station");
        return;
    }
    if (!online_at(core, peer_i, now_ms)) {
        set_status(core, "Offline");
        return;
    }
    open_outgoing(core, core->selected, now_ms, out, false);
    core->stats.calls_out++;
}

void ic_core_ui_end(ic_core_t *core, uint32_t now_ms, ic_out_t *out) {
    ic_out_clear(out);
    if (core->state == IC_ST_IDLE && count_legs(core) == 0) {
        return;
    }
    end_all(core, now_ms, out, true, "Call ended");
}

void ic_core_ui_mic(ic_core_t *core, uint32_t now_ms, ic_out_t *out) {
    (void)now_ms;
    ic_out_clear(out);
    if (core->state == IC_ST_PAGE_RX) {
        set_status(core, "Mic locked during page");
        return;
    }
    core->mic_armed = !core->mic_armed;
    if (core->state == IC_ST_IDLE) {
        set_status(core, core->mic_armed ? "Mic armed" : "Mic off");
    }
}

void ic_core_ui_page(ic_core_t *core, uint32_t now_ms, bool all_floors, ic_out_t *out) {
    uint16_t targets[3];
    int n = 0;
    int i;
    ic_out_clear(out);
    if (core->id.station_id != IC_STATION_MIN) {
        set_status(core, "Paging is reception only");
        return;
    }
    if (!core->link_up) {
        set_status(core, "Link down");
        return;
    }
    if (!core->audio_ok) {
        set_status(core, "Audio fault");
        return;
    }
    if (core->state != IC_ST_IDLE) {
        return;
    }
    if (all_floors) {
        targets[n++] = 101;
        targets[n++] = 102;
        targets[n++] = 103;
    } else {
        if (core->selected < 101 || core->selected > 103) {
            set_status(core, "Select a floor");
            return;
        }
        targets[n++] = core->selected;
    }
    for (i = 0; i < n; i++) {
        int peer_i = idx_of(targets[i]);
        if (peer_i >= 0 && online_at(core, peer_i, now_ms)) {
            open_outgoing(core, targets[i], now_ms, out, true);
        }
    }
    if (count_legs(core) == 0) {
        set_status(core, "Offline");
        core->state = IC_ST_IDLE;
        return;
    }
    core->stats.pages_out++;
    describe_up(core);
}

void ic_core_on_sig(ic_core_t *core, const uint8_t *pkt, size_t len,
                    uint32_t src_ip, uint32_t now_ms, ic_out_t *out) {
    ic_sig_msg_t msg;
    int rc;
    int peer_i;
    bool fresh = false;
    ic_out_clear(out);
    if (!core->link_up || pkt == NULL) {
        return;
    }
    if (peer_is_rate_blocked(core, src_ip, now_ms, false)) {
        return;
    }
    rc = ic_sig_open(pkt, len, core->id.key, &msg);
    if (rc == -3) {
        core->stats.bad_hmac++;
        peer_is_rate_blocked(core, src_ip, now_ms, true);
        return;
    }
    if (rc != 0) {
        core->stats.malformed++;
        return;
    }
    if (msg.dst != core->id.station_id || msg.src == core->id.station_id) {
        core->stats.malformed++;
        return;
    }
    if (!expected_payload(&msg)) {
        core->stats.malformed++;
        return;
    }
    peer_i = idx_of(msg.src);
    if (peer_i < 0) {
        uint8_t payload[2] = {IC_REJ_UNKNOWN, 0};
        uint32_t seq = alloc_seq(core);
        emit(core, out, src_ip, IC_REJECT, msg.session_id, msg.src, seq, now_ms, payload, 2);
        return;
    }
    if (msg.type != IC_HELLO && core->peers[peer_i].ip != 0 && src_ip != core->peers[peer_i].ip) {
        core->stats.stale++;
        return;
    }
    if (core->peers[peer_i].boot_valid && core->peers[peer_i].boot_id != msg.boot_id) {
        bool recent = core->peers[peer_i].hello_seen &&
                      (uint32_t)(now_ms - core->peers[peer_i].last_hello_ms) <= ONLINE_MS;
        /* A live peer keeps announcing. Only a hello may replace its boot id,
         * so a captured invite cannot ride an old boot back into a live call. */
        if (msg.type != IC_HELLO && recent) {
            core->stats.stale++;
            return;
        }
        note_boot_change(core, peer_i, msg.boot_id, now_ms, out);
    } else {
        note_boot_change(core, peer_i, msg.boot_id, now_ms, out);
    }
    if (core->peers[peer_i].rx_sig_valid && core->peers[peer_i].boot_id == msg.boot_id) {
        if (msg.seq < core->peers[peer_i].rx_sig_max) {
            core->stats.replay++;
            return;
        }
        if (msg.seq == core->peers[peer_i].rx_sig_max) {
            fresh = false;
        } else {
            fresh = true;
            core->peers[peer_i].rx_sig_max = msg.seq;
        }
    } else {
        fresh = true;
        core->peers[peer_i].boot_id = msg.boot_id;
        core->peers[peer_i].boot_valid = true;
        core->peers[peer_i].rx_sig_max = msg.seq;
        core->peers[peer_i].rx_sig_valid = true;
    }

    switch (msg.type) {
    case IC_HELLO:
        handle_hello(core, &msg, src_ip, now_ms, fresh);
        break;
    case IC_INVITE:
        handle_setup(core, &msg, now_ms, out, false, fresh);
        break;
    case IC_PAGE_START:
        handle_setup(core, &msg, now_ms, out, true, fresh);
        break;
    case IC_ACCEPT:
    case IC_PAGE_ACCEPT:
        handle_answer(core, &msg, now_ms, out);
        break;
    case IC_REJECT:
        handle_reject(core, &msg, now_ms);
        break;
    case IC_ACK:
        handle_ack(core, &msg, now_ms);
        break;
    case IC_BYE:
        handle_end(core, &msg, now_ms, out, false);
        break;
    case IC_PAGE_END:
        handle_end(core, &msg, now_ms, out, true);
        break;
    case IC_BYE_ACK:
    case IC_PAGE_END_ACK:
        handle_ack(core, &msg, now_ms);
        break;
    case IC_KEEPALIVE:
        handle_keepalive(core, &msg, now_ms);
        break;
    default:
        core->stats.malformed++;
        break;
    }
}

static void service_legs(ic_core_t *c, uint32_t now, ic_out_t *out) {
    int i;
    for (i = 0; i < 3; i++) {
        int peer_i;
        uint8_t payload[IC_CALL_PAYLOAD];
        if (c->legs[i].state == IC_LEG_EMPTY) {
            continue;
        }
        peer_i = idx_of(c->legs[i].peer);
        if (peer_i < 0) {
            continue;
        }
        if (c->legs[i].state == IC_LEG_OUT) {
            if ((uint32_t)(now - c->legs[i].start_ms) >= SETUP_MS) {
                send_bye_leg(c, out, i, now);
                clear_leg(c, i, now);
                if (count_legs(c) == 0) {
                    set_status(c, "No answer");
                    c->mic_armed = false;
                    c->audio_reset = true;
                } else {
                    describe_up(c);
                }
                continue;
            }
            if ((uint32_t)(now - c->legs[i].last_tx_ms) >= RETX_MS) {
                uint32_t seq = alloc_seq(c);
                put_call_payload(payload);
                c->legs[i].invite_seq = seq;
                c->legs[i].last_tx_ms = now;
                emit(c, out, c->peers[peer_i].ip,
                     c->legs[i].page ? IC_PAGE_START : IC_INVITE,
                     c->legs[i].session_id, c->legs[i].peer, seq, now,
                     payload, IC_CALL_PAYLOAD);
            }
        } else if (c->legs[i].state == IC_LEG_UP) {
            if ((uint32_t)(now - c->legs[i].last_rx_ms) >= DEAD_MS) {
                send_bye_leg(c, out, i, now);
                clear_leg(c, i, now);
                if (count_legs(c) == 0) {
                    set_status(c, "Call ended");
                    c->mic_armed = false;
                    c->audio_reset = true;
                } else {
                    describe_up(c);
                }
                continue;
            }
            if ((uint32_t)(now - c->legs[i].last_tx_ms) >= 1000u) {
                uint32_t seq = alloc_seq(c);
                c->legs[i].last_tx_ms = now;
                emit(c, out, c->peers[peer_i].ip, IC_KEEPALIVE, c->legs[i].session_id,
                     c->legs[i].peer, seq, now, NULL, 0);
            }
        }
    }
}

void ic_core_tick(ic_core_t *core, uint32_t now_ms, ic_out_t *out) {
    int self;
    int i;
    ic_out_clear(out);
    if (!core->link_up) {
        return;
    }
    service_legs(core, now_ms, out);
    if (!core->hello_started || (uint32_t)(now_ms - core->last_hello_ms) >= HELLO_MS) {
        self = idx_of(core->id.station_id);
        for (i = 0; i < IC_STATION_COUNT; i++) {
            if (i == self || core->peers[i].ip == 0) {
                continue;
            }
            send_hello(core, i, now_ms, out);
        }
        core->hello_started = true;
        core->last_hello_ms = now_ms;
    }
}

static bool mic_is_live(const ic_core_t *c) {
    if (!c->link_up || !c->audio_ok || !c->mic_armed) {
        return false;
    }
    if (c->state != IC_ST_CONNECTED && c->state != IC_ST_PAGING) {
        return false;
    }
    return count_up(c) > 0;
}

void ic_core_view(const ic_core_t *core, uint32_t now_ms, ic_view_t *view) {
    int i;
    memset(view, 0, sizeof(*view));
    view->state = core->state;
    view->self_id = core->id.station_id;
    view->selected = core->selected;
    view->mic_armed = core->mic_armed;
    view->mic_tx = mic_is_live(core);
    view->speaker = core->link_up && core->audio_ok &&
                    (core->state == IC_ST_CONNECTED || core->state == IC_ST_PAGE_RX);
    view->link_up = core->link_up;
    view->audio_ok = core->audio_ok;
    view->volume = core->volume;
    view->mic_gain = core->mic_gain;
    snprintf(view->status, sizeof(view->status), "%s", core->status);
    if (count_legs(core) == 1) {
        for (i = 0; i < 3; i++) {
            if (core->legs[i].state != IC_LEG_EMPTY) {
                int p = idx_of(core->legs[i].peer);
                view->remote = core->legs[i].peer;
                if (p >= 0) {
                    memcpy(view->remote_name, core->peers[p].name, IC_NAME_LEN);
                }
            }
        }
    }
    for (i = 0; i < IC_STATION_COUNT; i++) {
        view->dir[i].id = core->peers[i].id;
        view->dir[i].ip = core->peers[i].ip;
        memcpy(view->dir[i].name, core->peers[i].name, IC_NAME_LEN);
        view->dir[i].online = (core->peers[i].id == core->id.station_id) ? core->link_up
                                                                         : online_at(core, i, now_ms);
    }
}

void ic_core_stats(const ic_core_t *core, ic_stats_t *stats) {
    *stats = core->stats;
}

ic_state_t ic_core_state(const ic_core_t *core) {
    return core->state;
}

uint32_t ic_core_session(const ic_core_t *core) {
    int i;
    for (i = 0; i < 3; i++) {
        if (core->legs[i].state != IC_LEG_EMPTY) {
            return core->legs[i].session_id;
        }
    }
    return 0;
}

int ic_core_legs(const ic_core_t *core) {
    return count_legs(core);
}

void ic_core_plan_tx(ic_core_t *core, ic_media_plan_t *plan) {
    int i;
    memset(plan, 0, sizeof(*plan));
    if (!core->link_up || !core->audio_ok) {
        return;
    }
    if (core->state != IC_ST_CONNECTED && core->state != IC_ST_PAGING) {
        return;
    }
    for (i = 0; i < 3 && plan->nlegs < 3; i++) {
        ic_media_leg_t *leg;
        int peer_i;
        if (core->legs[i].state != IC_LEG_UP) {
            continue;
        }
        peer_i = idx_of(core->legs[i].peer);
        if (peer_i < 0) {
            continue;
        }
        core->legs[i].media_seq++;
        if (core->legs[i].media_seq == 0) {
            core->legs[i].media_seq = 1;
        }
        leg = &plan->legs[plan->nlegs++];
        leg->station = core->legs[i].peer;
        leg->ip = core->peers[peer_i].ip;
        leg->session_id = core->legs[i].session_id;
        leg->boot_id = core->id.boot_id;
        leg->seq = core->legs[i].media_seq;
        leg->mute = !core->mic_armed;
        leg->page = core->legs[i].page;
    }
}

int ic_core_build_media(ic_core_t *core, const ic_media_leg_t *leg,
                        const int16_t *pcm, bool force_mute,
                        uint8_t *out, size_t cap) {
    uint8_t payload[IC_FRAME];
    uint8_t mute;
    int i;
    if (!leg) {
        return -1;
    }
    mute = (force_mute || leg->mute) ? 1 : 0;
    if (mute || pcm == NULL) {
        for (i = 0; i < IC_FRAME; i++) {
            payload[i] = 0xFF;
        }
        mute = 1;
    } else {
        ic_ulaw_encode_frame(pcm, payload, IC_FRAME);
    }
    return ic_media_seal(leg->session_id, leg->seq, leg->boot_id, leg->seq * IC_FRAME,
                         core->id.station_id, mute, leg->page ? 1 : 0, payload, IC_FRAME,
                         core->id.key, out, cap);
}

int ic_core_ingest_media(ic_core_t *core, const uint8_t *pkt, size_t len,
                         uint32_t src_ip, uint32_t now_ms, int16_t *pcm) {
    ic_media_msg_t msg;
    int rc;
    int li;
    int peer_i;
    if (!core->link_up || pcm == NULL) {
        return 0;
    }
    rc = ic_media_open(pkt, len, core->id.key, &msg);
    if (rc == -3) {
        core->stats.bad_hmac++;
        return 0;
    }
    if (rc != 0) {
        core->stats.malformed++;
        return 0;
    }
    if (tomb_has(core, msg.session_id, now_ms)) {
        core->stats.stale++;
        return 0;
    }
    li = find_session(core, msg.session_id);
    if (li < 0 || core->legs[li].state != IC_LEG_UP) {
        return 0;
    }
    if (msg.src != core->legs[li].peer || msg.boot_id != core->legs[li].remote_boot) {
        core->stats.stale++;
        return 0;
    }
    peer_i = idx_of(msg.src);
    if (peer_i < 0 || src_ip != core->peers[peer_i].ip) {
        core->stats.stale++;
        return 0;
    }
    if (core->legs[li].media_rx_valid && (msg.seq + 16) < core->legs[li].media_rx_max) {
        core->stats.replay++;
        return 0;
    }
    if (!core->legs[li].media_rx_valid || msg.seq > core->legs[li].media_rx_max) {
        core->legs[li].media_rx_max = msg.seq;
        core->legs[li].media_rx_valid = true;
    }
    core->legs[li].last_rx_ms = now_ms;
    if (msg.mute || msg.payload_len != IC_FRAME) {
        memset(pcm, 0, IC_FRAME * sizeof(int16_t));
    } else {
        ic_ulaw_decode_frame(msg.payload, pcm, IC_FRAME);
    }
    return 1;
}

bool ic_core_consume_audio_reset(ic_core_t *core) {
    bool reset = core->audio_reset;
    core->audio_reset = false;
    return reset;
}
