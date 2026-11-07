#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define IC_SHA256_LEN 32
#define IC_HMAC_TRUNC 16
#define IC_KEY_LEN 16

typedef struct ic_sha256 {
    uint32_t state[8];
    uint64_t bitlen;
    uint8_t buf[64];
    uint32_t buflen;
} ic_sha256_t;

void ic_sha256_init(ic_sha256_t *ctx);
void ic_sha256_update(ic_sha256_t *ctx, const uint8_t *data, size_t len);
void ic_sha256_final(ic_sha256_t *ctx, uint8_t out[IC_SHA256_LEN]);

void ic_hmac_sha256(const uint8_t *key, size_t key_len,
                    const uint8_t *msg, size_t msg_len,
                    uint8_t out[IC_SHA256_LEN]);

void ic_aes128_encrypt_block(const uint8_t key[16], const uint8_t in[16], uint8_t out[16]);

void ic_aes128_ctr(const uint8_t key[16], const uint8_t nonce[16],
                   uint8_t *buf, size_t len);

int ic_ct_equal(const uint8_t *a, const uint8_t *b, size_t n);

/* Returns 0 when the built-in SHA-256, HMAC, and AES-128 vectors match. */
int ic_crypto_selftest(void);

#ifdef __cplusplus
}
#endif
