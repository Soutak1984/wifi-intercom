#include "ic_audio.h"

#include <string.h>

void ic_jitter_init(ic_jitter_t *j, int target_frames) {
    memset(j, 0, sizeof(*j));
    if (target_frames < 1) {
        target_frames = 1;
    }
    if (target_frames > IC_JITTER_SLOTS) {
        target_frames = IC_JITTER_SLOTS;
    }
    j->target = target_frames;
}

void ic_jitter_reset(ic_jitter_t *j) {
    int target = j->target;
    ic_jitter_init(j, target);
}

static int valid_count(const ic_jitter_t *j) {
    int n = 0;
    int i;
    for (i = 0; i < IC_JITTER_SLOTS; i++) {
        if (j->valid[i]) {
            n++;
        }
    }
    return n;
}

static uint32_t min_valid_seq(const ic_jitter_t *j) {
    uint32_t min = 0;
    bool any = false;
    int i;
    for (i = 0; i < IC_JITTER_SLOTS; i++) {
        if (!j->valid[i]) {
            continue;
        }
        if (!any || j->seq[i] < min) {
            min = j->seq[i];
            any = true;
        }
    }
    return min;
}

void ic_jitter_push(ic_jitter_t *j, uint32_t seq, const int16_t *pcm) {
    int slot = (int)(seq % IC_JITTER_SLOTS);
    j->seq[slot] = seq;
    memcpy(j->pcm[slot], pcm, sizeof(j->pcm[slot]));
    j->valid[slot] = 1;
    j->pushed++;
    if (!j->primed && valid_count(j) >= j->target) {
        j->primed = true;
        j->next_seq = min_valid_seq(j);
        j->loss_streak = 0;
    }
}

int ic_jitter_pop(ic_jitter_t *j, int16_t *pcm) {
    int slot;
    int i;

    if (!j->primed) {
        memset(pcm, 0, IC_FRAME * sizeof(int16_t));
        return 0;
    }

    slot = (int)(j->next_seq % IC_JITTER_SLOTS);
    if (j->valid[slot] && j->seq[slot] == j->next_seq) {
        memcpy(pcm, j->pcm[slot], IC_FRAME * sizeof(int16_t));
        memcpy(j->last, pcm, sizeof(j->last));
        j->have_last = true;
        j->valid[slot] = 0;
        j->next_seq++;
        j->popped++;
        j->loss_streak = 0;
        return 1;
    }

    if (j->have_last) {
        for (i = 0; i < IC_FRAME; i++) {
            j->last[i] = (int16_t)((j->last[i] * 7) / 8);
            pcm[i] = j->last[i];
        }
    } else {
        memset(pcm, 0, IC_FRAME * sizeof(int16_t));
    }
    j->next_seq++;
    j->lost++;
    j->popped++;
    j->loss_streak++;
    if (j->loss_streak > IC_JITTER_SLOTS + 4) {
        int target = j->target;
        uint32_t pushed = j->pushed;
        uint32_t lost = j->lost;
        ic_jitter_init(j, target);
        j->pushed = pushed;
        j->lost = lost;
    }
    return -1;
}
