// Transport-agnostic audio core. Producers (RAOP decoder, later) push PCM via
// audio_play_pcm(); a playback task drains it to the DAC. Knows nothing about
// any network transport.
#pragma once
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Bring up I2S, allocate the PSRAM ring buffer, start the playback task.
void   audio_init(void);

// Enqueue up to n_frames of interleaved 16-bit stereo PCM. Returns frames
// accepted (< n_frames when the buffer is full — caller should retry the rest).
size_t audio_play_pcm(const int16_t *frames, size_t n_frames);

// Phase 0 diagnostic: start a task that streams a 440 Hz sine through
// audio_play_pcm() (the retained v0.0.1 known-good path).
void   audio_diag_tone_start(void);

#ifdef __cplusplus
}
#endif
