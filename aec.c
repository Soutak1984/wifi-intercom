#include "ic_audio.h"

#include <string.h>

void ic_aec_init(ic_aec_t *a, int delay_samples) {
    memset(a, 0, sizeof(*a));
    if (delay_samples < 0) {
        delay_samples = 0;
    }
    if (delay_samples > (IC_AEC_DLY - IC_AEC_TAPS - 1)) {
        delay_samples = IC_AEC_DLY - IC_AEC_TAPS - 1;
    }
    a->delay_samples = delay_samples;
    a->mu = 0.35f;
}

void ic_aec_reset(ic_aec_t *a) {
    int delay = a->delay_samples;
    float mu = a->mu;
    ic_aec_init(a, delay);
    a->mu = mu;
}

void ic_aec_play(ic_aec_t *a, const int16_t *speaker, int n) {
    int i;
    for (i = 0; i < n; i++) {
        a->dly[a->dpos] = speaker[i];
        a->dpos++;
        if (a->dpos >= IC_AEC_DLY) {
            a->dpos = 0;
        }
    }
}

static int16_t sat16(float v) {
    if (v > 32767.0f) {
        return 32767;
    }
    if (v < -32768.0f) {
        return -32768;
    }
    return (int16_t)v;
}

/* ago=0 is the most recently played sample. */
static int16_t dly_at(const ic_aec_t *a, int ago) {
    int idx = a->dpos - 1 - ago;
    while (idx < 0) {
        idx += IC_AEC_DLY;
    }
    return a->dly[idx % IC_AEC_DLY];
}

void ic_aec_capture(ic_aec_t *a, const int16_t *mic, int16_t *out, int n) {
    int i;
    for (i = 0; i < n; i++) {
        float x[IC_AEC_TAPS];
        float y = 0.0f;
        float power = 1.0f;
        float err;
        float mic_f;
        int k;
        int base = a->delay_samples + (n - 1 - i);

        for (k = 0; k < IC_AEC_TAPS; k++) {
            x[k] = (float)dly_at(a, base + k);
            y += a->w[k] * x[k];
            power += x[k] * x[k];
        }
        mic_f = (float)mic[i];
        err = mic_f - y;
        /* Adapt while the far end is active and local speech does not dominate it. */
        if (x[0] > 80.0f || x[0] < -80.0f) {
            float ref = x[0] >= 0 ? x[0] : -x[0];
            float mic_abs = mic_f >= 0 ? mic_f : -mic_f;
            if (mic_abs < ref * 4.0f) {
                float g = a->mu * err / power;
                for (k = 0; k < IC_AEC_TAPS; k++) {
                    a->w[k] += g * x[k];
                }
                a->adapted++;
            }
        }
        out[i] = sat16(err);
    }
}
