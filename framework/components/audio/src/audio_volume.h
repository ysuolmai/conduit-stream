// Pure volume math (spec §6e): RAOP dB (-144 = mute .. 0 = full) -> Q16.16 linear
// gain, plus a per-sample int16 apply with sign-aware rounding + clamp. No ESP-IDF
// deps -> host-unit-testable. The playback drain caches one fix_q16 and BYPASSES
// the whole multiply when it equals AUDIO_VOL_UNITY (0 dB) — see audio_playback.c.
#pragma once
#include <stdint.h>
#include <stddef.h>

#define AUDIO_VOL_UNITY 65536   // Q16.16 representation of gain 1.0 (0 dB)

// Convert an AirPlay volume dB value to a Q16.16 linear gain.
//   db <= -144  -> 0            (mute sentinel; a flag, not a real level)
//   db clamped to [-30, 0]      (out-of-range senders can't amplify past unity)
//   otherwise   -> round(pow(10, db/20) * 65536)
// Mapping cited to shairport-sync player.c (65536.0 * pow(10, dB/20)).
int32_t audio_volume_db_to_q16(float db);

// Multiply n_samples int16 samples (interleaved L/R is irrelevant here; operates
// per-sample) in place by fix_q16, round-half-away-from-zero, clamp to int16.
// Caller SHOULD skip this call entirely when fix_q16 == AUDIO_VOL_UNITY.
void audio_volume_apply(int16_t *buf, size_t n_samples, int32_t fix_q16);
