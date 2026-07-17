#include "audio_playback.h"
#include "audio_i2s.h"
#include "audio_drift.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define PLAYBACK_CHUNK_FRAMES 256

// Drains the ring into I2S. On underrun, writes a chunk of silence so the DAC
// keeps clocking cleanly instead of stalling or replaying stale samples.
//
// Free-run drift safeguard (spec §6f): the sender's 44.1 kHz clock and our DAC's
// differ by a few ppm, so over a multi-hour session the ring slowly fills or
// drains. Once per drain cycle we ask audio_drift_decide() whether the fill has
// crossed a generous high/low watermark and, if so, shed OR pad exactly one frame
// (~23 µs — inaudible). Equilibrium parks `avail` just inside the band, so the
// correction fires rarely and never fights normal fill/backpressure. avail==0 is a
// true underrun (NONE), handled by the silence path below — not by drift.
void audio_playback_task(void *arg) {
    audio_ringbuf_t *ring = (audio_ringbuf_t *) arg;
    // static (off the 4 KB task stack). Single-instance task: these buffers are
    // NOT reentrant — do not spawn a second audio_playback_task.
    static int16_t chunk[PLAYBACK_CHUNK_FRAMES * 2];
    static const int16_t silence[PLAYBACK_CHUNK_FRAMES * 2] = {0};

    // Band derived once from ring capacity: target ~50 % depth, high/low at
    // 3/4 and 1/4 of capacity so drift correction is rare and never competes with
    // normal fill. Capacity is fixed for the ring's lifetime.
    const audio_drift_cfg_t drift_cfg = {
        .high = ring->capacity * 3 / 4,
        .low  = ring->capacity * 1 / 4,
    };

    for (;;) {
        // At most one single-frame drift correction per cycle, BEFORE the read.
        switch (audio_drift_decide(audio_ringbuf_available(ring), &drift_cfg)) {
            case AUDIO_DRIFT_DROP:
                audio_ringbuf_drop(ring, 1);   // shed ~23 µs: buffer above high
                break;
            case AUDIO_DRIFT_DUP: {
                int16_t f[2];                  // pad ~23 µs: buffer below low
                // Repeat the NEXT-TO-PLAY frame (at tail), written just ahead of
                // the tail chunk below, so the pad is a true local frame-repeat.
                // (Not head-1: that far sample would splice a click while avail
                // sits below low — startup fill, post-underrun, sustained jitter.)
                if (audio_ringbuf_first_frame(ring, f)) audio_i2s_write(f, 1);
                break;                         // DUP goes straight to I2S, not the ring
            }
            default:
                break;
        }

        size_t got = audio_ringbuf_read(ring, chunk, PLAYBACK_CHUNK_FRAMES);
        if (got > 0) {
            audio_i2s_write(chunk, got);
        } else {
            audio_i2s_write(silence, PLAYBACK_CHUNK_FRAMES);
        }
    }
}
