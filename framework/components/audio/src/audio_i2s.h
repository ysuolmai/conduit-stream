#pragma once
#include <stddef.h>
#include <stdint.h>
#include "audio.h"   // AUDIO_PIN_CORE (public pin contract) lives here now

#define AUDIO_SAMPLE_RATE_HZ 44100

#ifdef __cplusplus
extern "C" {
#endif

// Bring up the standard-mode I2S TX channel (same config as v0.0.1).
void   audio_i2s_init(void);

// Blocking write of n_frames interleaved stereo frames. Returns frames written.
size_t audio_i2s_write(const int16_t *frames, size_t n_frames);

#ifdef __cplusplus
}
#endif
