// Pure free-run drift safeguard (spec §6f). Over a multi-hour session the sender
// and DAC clocks differ by a few ppm; the PCM ring slowly fills or drains. When
// it crosses a high/low watermark we drop or duplicate ONE frame (~23 µs @44.1k,
// inaudible) to hold the buffer near its target depth. No ESP-IDF deps.
#pragma once
#include <stddef.h>

typedef enum {
    AUDIO_DRIFT_NONE = 0,
    AUDIO_DRIFT_DROP,   // buffer above high watermark: discard one frame
    AUDIO_DRIFT_DUP,    // buffer below low watermark (and non-empty): repeat one frame
} audio_drift_action_t;

typedef struct {
    size_t high;        // drop when avail >= high
    size_t low;         // dup  when 0 < avail <= low
} audio_drift_cfg_t;

// Decide at most one correction for this drain cycle from the current readable
// frame count. avail==0 always yields NONE (true underrun → silence path owns it).
audio_drift_action_t audio_drift_decide(size_t avail, const audio_drift_cfg_t *cfg);
