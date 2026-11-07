#include "ic_proto.h"

#include "ic_crypto.h"

#include <string.h>

static void wr16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)(v & 0xff);
    p[1] = (uint8_t)((v >> 8) & 0xff);
}

static void wr32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v & 0xff);
    p[1] = (uint8_t)((v >> 8) & 0xff);
    p[2] = (uint8_t)((v >> 16) & 0xff);
    p[3] = (uint8_t)((v >> 24) & 0xff);
}

static uint16_t rd16(const uint8_t *p) {
    return (uint16_t)(p[0] | ((uint16_t)p[1] << 8));
}

static uint32_t rd32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* Domain byte keeps signalling and media from reusing a CTR nonce. */
static void fill_nonce(uint8_t nonce[16], uint32_t session, uint32_t seq,
                       uint32_t boot, uint8_t domain) {
    memset(nonce, 0, 16);
    wr32(nonce + 0, session);
    wr32(nonce + 4, seq);
    wr32(nonce + 8, boot);
    nonce[12] = domain;
}

static int finish_mac(uint8_t *out, size_t body, const uint8_t key[16]) {
    uint8_t mac[IC_SHA256_LEN];
    ic_hmac_sha256(key, IC_KEY_LEN, out, body, mac);
    memcpy(out + body, mac, IC_HMAC_TRUNC);
    return (int)(body + IC_HMAC_TRUNC);
}

static int check_mac(const uint8_t *in, size_t body, const uint8_t key[16]) {
    uint8_t mac[IC_SHA256_LEN];
    ic_hmac_sha256(key, IC_KEY_LEN, in, body, mac);
    if (!ic_ct_equal(mac, in + body, IC_HMAC_TRUNC)) {
        return -3;
    }
    return 0;
}

int ic_sig_seal(uint8_t type, uint16_t flags, uint32_t session_id,
                uint16_t src, uint16_t dst, uint32_t seq, uint32_t boot_id,
                uint32_t timestamp_ms, const uint8_t mac[6],
                const uint8_t *payload, uint16_t payload_len,
                const uint8_t key[16], uint8_t *out, size_t cap) {
    uint8_t nonce[16];
    size_t body;

    if (payload_len > IC_SIG_MAX_PAYLOAD) {
        return -1;
    }
    body = (size_t)IC_SIG_HDR + payload_len;
    if (cap < body + IC_HMAC_TRUNC) {
        return -1;
    }
    memset(out, 0, body);
    wr32(out + 0, IC_SIG_MAGIC);
    out[4] = IC_PROTO_VERSION;
    out[5] = type;
    wr16(out + 6, flags);
    wr32(out + 8, session_id);
    wr16(out + 12, src);
    wr16(out + 14, dst);
    wr32(out + 16, seq);
    wr32(out + 20, boot_id);
    wr32(out + 24, timestamp_ms);
    if (mac) {
        memcpy(out + 28, mac, 6);
    }
    wr16(out + 34, payload_len);
    if (payload_len && payload) {
        memcpy(out + IC_SIG_HDR, payload, payload_len);
        fill_nonce(nonce, session_id, seq, boot_id, 1);
        ic_aes128_ctr(key, nonce, out + IC_SIG_HDR, payload_len);
    }
    return finish_mac(out, body, key);
}

int ic_sig_open(const uint8_t *in, size_t len, const uint8_t key[16], ic_sig_msg_t *msg) {
    uint16_t plen;
    size_t body;
    uint8_t nonce[16];
    int mac_rc;

    if (len < (size_t)IC_SIG_HDR + IC_HMAC_TRUNC) {
        return -1;
    }
    if (rd32(in) != IC_SIG_MAGIC || in[4] != IC_PROTO_VERSION) {
        return -1;
    }
    plen = rd16(in + 34);
    if (plen > IC_SIG_MAX_PAYLOAD) {
        return -2;
    }
    body = (size_t)IC_SIG_HDR + plen;
    if (len != body + IC_HMAC_TRUNC) {
        return -2;
    }
    mac_rc = check_mac(in, body, key);
    if (mac_rc != 0) {
        return mac_rc;
    }
    memset(msg, 0, sizeof(*msg));
    msg->type = in[5];
    msg->flags = rd16(in + 6);
    msg->session_id = rd32(in + 8);
    msg->src = rd16(in + 12);
    msg->dst = rd16(in + 14);
    msg->seq = rd32(in + 16);
    msg->boot_id = rd32(in + 20);
    msg->timestamp_ms = rd32(in + 24);
    memcpy(msg->mac, in + 28, 6);
    msg->payload_len = plen;
    if (plen) {
        memcpy(msg->payload, in + IC_SIG_HDR, plen);
        fill_nonce(nonce, msg->session_id, msg->seq, msg->boot_id, 1);
        ic_aes128_ctr(key, nonce, msg->payload, plen);
    }
    return 0;
}

int ic_media_seal(uint32_t session_id, uint32_t seq, uint32_t boot_id,
                  uint32_t timestamp, uint16_t src, uint8_t mute, uint8_t flags,
                  const uint8_t *payload, uint16_t payload_len,
                  const uint8_t key[16], uint8_t *out, size_t cap) {
    uint8_t nonce[16];
    size_t body;

    if (payload_len > IC_MED_PAYLOAD) {
        return -1;
    }
    body = (size_t)IC_MED_HDR + payload_len;
    if (cap < body + IC_HMAC_TRUNC) {
        return -1;
    }
    memset(out, 0, body);
    wr32(out + 0, IC_MED_MAGIC);
    out[4] = IC_PROTO_VERSION;
    out[5] = IC_CODEC_PCMU;
    out[6] = mute ? 1 : 0;
    out[7] = flags;
    wr32(out + 8, session_id);
    wr32(out + 12, seq);
    wr32(out + 16, timestamp);
    wr16(out + 20, src);
    wr16(out + 22, payload_len);
    wr32(out + 24, boot_id);
    if (payload_len && payload) {
        memcpy(out + IC_MED_HDR, payload, payload_len);
        fill_nonce(nonce, session_id, seq, boot_id, 2);
        ic_aes128_ctr(key, nonce, out + IC_MED_HDR, payload_len);
    }
    return finish_mac(out, body, key);
}

int ic_media_open(const uint8_t *in, size_t len, const uint8_t key[16], ic_media_msg_t *msg) {
    uint16_t plen;
    size_t body;
    uint8_t nonce[16];
    int mac_rc;

    if (len < (size_t)IC_MED_HDR + IC_HMAC_TRUNC) {
        return -1;
    }
    if (rd32(in) != IC_MED_MAGIC || in[4] != IC_PROTO_VERSION) {
        return -1;
    }
    if (in[5] != IC_CODEC_PCMU) {
        return -2;
    }
    plen = rd16(in + 22);
    if (plen > IC_MED_PAYLOAD) {
        return -2;
    }
    body = (size_t)IC_MED_HDR + plen;
    if (len != body + IC_HMAC_TRUNC) {
        return -2;
    }
    mac_rc = check_mac(in, body, key);
    if (mac_rc != 0) {
        return mac_rc;
    }
    memset(msg, 0, sizeof(*msg));
    msg->codec = in[5];
    msg->mute = in[6];
    msg->flags = in[7];
    msg->session_id = rd32(in + 8);
    msg->seq = rd32(in + 12);
    msg->timestamp = rd32(in + 16);
    msg->src = rd16(in + 20);
    msg->payload_len = plen;
    msg->boot_id = rd32(in + 24);
    if (plen) {
        memcpy(msg->payload, in + IC_MED_HDR, plen);
        fill_nonce(nonce, msg->session_id, msg->seq, msg->boot_id, 2);
        ic_aes128_ctr(key, nonce, msg->payload, plen);
    }
    return 0;
}
