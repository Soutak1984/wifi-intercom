#include "ic_crypto.h"

#include <string.h>

static uint32_t rotr(uint32_t x, uint32_t n) {
    return (x >> n) | (x << (32 - n));
}

static uint32_t load_be(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

static void store_be(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

static const uint32_t K[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2
};

static void transform(ic_sha256_t *ctx, const uint8_t block[64]) {
    uint32_t w[64];
    uint32_t a, b, c, d, e, f, g, h;
    int i;

    for (i = 0; i < 16; i++) {
        w[i] = load_be(block + (i * 4));
    }
    for (i = 16; i < 64; i++) {
        uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
        uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }

    a = ctx->state[0];
    b = ctx->state[1];
    c = ctx->state[2];
    d = ctx->state[3];
    e = ctx->state[4];
    f = ctx->state[5];
    g = ctx->state[6];
    h = ctx->state[7];

    for (i = 0; i < 64; i++) {
        uint32_t S1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
        uint32_t ch = (e & f) ^ ((~e) & g);
        uint32_t t1 = h + S1 + ch + K[i] + w[i];
        uint32_t S0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
        uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
        uint32_t t2 = S0 + maj;
        h = g;
        g = f;
        f = e;
        e = d + t1;
        d = c;
        c = b;
        b = a;
        a = t1 + t2;
    }

    ctx->state[0] += a;
    ctx->state[1] += b;
    ctx->state[2] += c;
    ctx->state[3] += d;
    ctx->state[4] += e;
    ctx->state[5] += f;
    ctx->state[6] += g;
    ctx->state[7] += h;
}

void ic_sha256_init(ic_sha256_t *ctx) {
    ctx->state[0] = 0x6a09e667;
    ctx->state[1] = 0xbb67ae85;
    ctx->state[2] = 0x3c6ef372;
    ctx->state[3] = 0xa54ff53a;
    ctx->state[4] = 0x510e527f;
    ctx->state[5] = 0x9b05688c;
    ctx->state[6] = 0x1f83d9ab;
    ctx->state[7] = 0x5be0cd19;
    ctx->bitlen = 0;
    ctx->buflen = 0;
}

void ic_sha256_update(ic_sha256_t *ctx, const uint8_t *data, size_t len) {
    size_t i = 0;
    while (i < len) {
        ctx->buf[ctx->buflen++] = data[i++];
        if (ctx->buflen == 64) {
            transform(ctx, ctx->buf);
            ctx->bitlen += 512;
            ctx->buflen = 0;
        }
    }
}

void ic_sha256_final(ic_sha256_t *ctx, uint8_t out[IC_SHA256_LEN]) {
    uint32_t i;
    uint64_t bits;

    ctx->bitlen += (uint64_t)ctx->buflen * 8;
    ctx->buf[ctx->buflen++] = 0x80;
    if (ctx->buflen > 56) {
        while (ctx->buflen < 64) {
            ctx->buf[ctx->buflen++] = 0;
        }
        transform(ctx, ctx->buf);
        ctx->buflen = 0;
    }
    while (ctx->buflen < 56) {
        ctx->buf[ctx->buflen++] = 0;
    }
    bits = ctx->bitlen;
    for (i = 0; i < 8; i++) {
        ctx->buf[63 - i] = (uint8_t)(bits & 0xff);
        bits >>= 8;
    }
    transform(ctx, ctx->buf);
    for (i = 0; i < 8; i++) {
        store_be(out + (i * 4), ctx->state[i]);
    }
}

void ic_hmac_sha256(const uint8_t *key, size_t key_len,
                    const uint8_t *msg, size_t msg_len,
                    uint8_t out[IC_SHA256_LEN]) {
    uint8_t k[64];
    uint8_t ipad[64];
    uint8_t opad[64];
    uint8_t inner[IC_SHA256_LEN];
    ic_sha256_t ctx;
    size_t i;

    memset(k, 0, sizeof(k));
    if (key_len > 64) {
        ic_sha256_init(&ctx);
        ic_sha256_update(&ctx, key, key_len);
        ic_sha256_final(&ctx, k);
    } else {
        memcpy(k, key, key_len);
    }
    for (i = 0; i < 64; i++) {
        ipad[i] = (uint8_t)(k[i] ^ 0x36);
        opad[i] = (uint8_t)(k[i] ^ 0x5c);
    }
    ic_sha256_init(&ctx);
    ic_sha256_update(&ctx, ipad, 64);
    ic_sha256_update(&ctx, msg, msg_len);
    ic_sha256_final(&ctx, inner);
    ic_sha256_init(&ctx);
    ic_sha256_update(&ctx, opad, 64);
    ic_sha256_update(&ctx, inner, IC_SHA256_LEN);
    ic_sha256_final(&ctx, out);
}

int ic_ct_equal(const uint8_t *a, const uint8_t *b, size_t n) {
    uint8_t d = 0;
    size_t i;
    for (i = 0; i < n; i++) {
        d |= (uint8_t)(a[i] ^ b[i]);
    }
    return d == 0;
}
