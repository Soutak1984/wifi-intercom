#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "ic_proto.h"

#ifdef __cplusplus
extern "C" {
#endif

uint8_t ic_ulaw_encode(int16_t pcm);
int16_t ic_ulaw_decode(uint8_t ulaw);
void ic_ulaw_encode_frame(const int16_t *pcm, uint8_t *ulaw, int n);
void ic_ulaw_decode_frame(const uint8_t *ulaw, int16_t *pcm, int n);

#define IC_JITTER_SLOTS 8

typedef struct ic_jitter {
    int16_t pcm[IC_JITTER_SLOTS][IC_FRAME];
    uint32_t seq[IC_JITTER_SLOTS];
    uint8_t valid[IC_JITTER_SLOTS];
    uint32_t next_seq;
    int target;
    bool primed;
    bool have_last;
    int loss_streak;
    int16_t last[IC_FRAME];
    uint32_t pushed;
    uint32_t popped;
    uint32_t lost;
} ic_jitter_t;

void ic_jitter_init(ic_jitter_t *j, int target_frames);
void ic_jitter_reset(ic_jitter_t *j);
void ic_jitter_push(ic_jitter_t *j, uint32_t seq, const int16_t *pcm);
/* Returns 1 played, 0 not primed yet (silence written), -1 concealment. */
int ic_jitter_pop(ic_jitter_t *j, int16_t *pcm);

#define IC_AEC_TAPS 128
#define IC_AEC_DLY 2048

typedef struct ic_aec {
    float w[IC_AEC_TAPS];
    int16_t dly[IC_AEC_DLY];
    int dpos;
    int delay_samples;
    float mu;
    uint32_t adapted;
} ic_aec_t;

void ic_aec_init(ic_aec_t *a, int delay_samples);
void ic_aec_reset(ic_aec_t *a);
void ic_aec_play(ic_aec_t *a, const int16_t *speaker, int n);
void ic_aec_capture(ic_aec_t *a, const int16_t *mic, int16_t *out, int n);

#ifdef __cplusplus
}
#endif
