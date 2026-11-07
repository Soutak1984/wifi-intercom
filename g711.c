#include "ic_audio.h"

/* ITU-T G.711 µ-law. Segment ends follow the usual linear-to-µ-law search. */
static const int16_t SEG_END[8] = {
    0xFF, 0x1FF, 0x3FF, 0x7FF, 0xFFF, 0x1FFF, 0x3FFF, 0x7FFF
};

uint8_t ic_ulaw_encode(int16_t pcm) {
    int sample = pcm;
    int mask;
    int seg;
    int uval;

    if (sample < 0) {
        if (sample < -32635) {
            sample = -32635;
        }
        sample = -sample;
        mask = 0x7F;
    } else {
        if (sample > 32635) {
            sample = 32635;
        }
        mask = 0xFF;
    }
    sample += 0x84;
    for (seg = 0; seg < 8; seg++) {
        if (sample <= SEG_END[seg]) {
            break;
        }
    }
    if (seg > 7) {
        seg = 7;
    }
    uval = (seg << 4) | ((sample >> (seg + 3)) & 0x0F);
    return (uint8_t)(uval ^ mask);
}

int16_t ic_ulaw_decode(uint8_t ulaw) {
    int u = (uint8_t)~ulaw;
    int t = ((u & 0x0F) << 3) + 0x84;
    t <<= (u & 0x70) >> 4;
    if (u & 0x80) {
        return (int16_t)(0x84 - t);
    }
    return (int16_t)(t - 0x84);
}

void ic_ulaw_encode_frame(const int16_t *pcm, uint8_t *ulaw, int n) {
    int i;
    for (i = 0; i < n; i++) {
        ulaw[i] = ic_ulaw_encode(pcm[i]);
    }
}

void ic_ulaw_decode_frame(const uint8_t *ulaw, int16_t *pcm, int n) {
    int i;
    for (i = 0; i < n; i++) {
        pcm[i] = ic_ulaw_decode(ulaw[i]);
    }
}
