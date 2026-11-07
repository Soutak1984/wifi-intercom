#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

bool audio_init(int aec_samples);
bool audio_stack_ok(void);
void audio_start_task(void);
void audio_set_aec_delay(int samples);

#ifdef __cplusplus
}
#endif
