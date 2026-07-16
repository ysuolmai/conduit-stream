#pragma once
#include <stddef.h>
#include <stdint.h>

#define AUDIO_SAMPLE_RATE_HZ 44100

// Core that BOTH the playback (consumer) and producer (diag/decoder) tasks pin
// to. The SPSC ring buffer has no memory barriers, so its producer and consumer
// must share a core (single-core context switches are full barriers). See the
// concurrency precondition in audio_ringbuf.h.
#define AUDIO_PIN_CORE 1

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
