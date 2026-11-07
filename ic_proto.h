#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifndef IC_FRAME
#define IC_FRAME 160
#endif

#define IC_SIG_MAGIC 0x31434E49u /* 'INC1' little-endian */
#define IC_MED_MAGIC 0x3143454Du /* 'MEC1' little-endian */
#define IC_PROTO_VERSION 1

#define IC_SIG_HDR 36
#define IC_SIG_MAX_PAYLOAD 48
#define IC_SIG_MAX_PACKET (IC_SIG_HDR + IC_SIG_MAX_PAYLOAD + 16)

#define IC_MED_HDR 28
#define IC_MED_PAYLOAD IC_FRAME
#define IC_MED_MAX_PACKET (IC_MED_HDR + IC_MED_PAYLOAD + 16)

#define IC_SIG_PORT 47000
#define IC_MED_PORT 47010

#ifndef IC_CODEC_PCMU
#define IC_CODEC_PCMU 1
#endif

#define IC_HELLO_PAYLOAD 20
#define IC_CALL_PAYLOAD 4
#define IC_REJECT_PAYLOAD 2

enum {
    IC_HELLO = 1,
    IC_INVITE = 3,
    IC_ACCEPT = 4,
    IC_REJECT = 5,
    IC_ACK = 6,
    IC_BYE = 7,
    IC_BYE_ACK = 8,
    IC_KEEPALIVE = 9,
    IC_PAGE_START = 11,
    IC_PAGE_ACCEPT = 12,
    IC_PAGE_END = 13,
    IC_PAGE_END_ACK = 14
};

enum {
    IC_REJ_BUSY = 1,
    IC_REJ_GLARE = 2,
    IC_REJ_NOT_ALLOWED = 3,
    IC_REJ_UNKNOWN = 4,
    IC_REJ_AUDIO = 5
};

typedef struct ic_sig_msg {
    uint8_t type;
    uint16_t flags;
    uint32_t session_id;
    uint16_t src;
    uint16_t dst;
    uint32_t seq;
    uint32_t boot_id;
    uint32_t timestamp_ms;
    uint8_t mac[6];
    uint16_t payload_len;
    uint8_t payload[IC_SIG_MAX_PAYLOAD];
} ic_sig_msg_t;

int ic_sig_seal(uint8_t type, uint16_t flags, uint32_t session_id,
                uint16_t src, uint16_t dst, uint32_t seq, uint32_t boot_id,
                uint32_t timestamp_ms, const uint8_t mac[6],
                const uint8_t *payload, uint16_t payload_len,
                const uint8_t key[16], uint8_t *out, size_t cap);

/* 0 ok, -1 truncated or bad magic/version, -2 malformed length, -3 bad hmac. */
int ic_sig_open(const uint8_t *in, size_t len, const uint8_t key[16], ic_sig_msg_t *msg);

int ic_media_seal(uint32_t session_id, uint32_t seq, uint32_t boot_id,
                  uint32_t timestamp, uint16_t src, uint8_t mute, uint8_t flags,
                  const uint8_t *payload, uint16_t payload_len,
                  const uint8_t key[16], uint8_t *out, size_t cap);

typedef struct ic_media_msg {
    uint32_t session_id;
    uint32_t seq;
    uint32_t boot_id;
    uint32_t timestamp;
    uint16_t src;
    uint8_t mute;
    uint8_t flags;
    uint8_t codec;
    uint16_t payload_len;
    uint8_t payload[IC_MED_PAYLOAD];
} ic_media_msg_t;

int ic_media_open(const uint8_t *in, size_t len, const uint8_t key[16], ic_media_msg_t *msg);

#ifdef __cplusplus
}
#endif
