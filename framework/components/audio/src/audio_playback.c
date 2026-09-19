#include "audio_playback.h"
#include "audio_i2s.h"
#include "audio_drift.h"
#include "audio_volume.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"   // DEBUG: crackle diagnosis stats

#define PLAYBACK_CHUNK_FRAMES 256

static void audio_mix_mono(int16_t *samples, size_t frames) {
#ifdef CONFIG_CONDUIT_MONO_OUTPUT
    for (size_t i = 0; i < frames; ++i) {
        int32_t mixed = (int32_t)samples[i * 2] + samples[i * 2 + 1];
        int16_t mono = (int16_t)(mixed / 2);
        samples[i * 2] = mono;
        samples[i * 2 + 1] = mono;
    }
#else
    (void)samples;
    (void)frames;
#endif
}

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

    // RAOP senders buffer ~2 s ahead, and our ring is ~2 s, so during normal
    // streaming the buffer legitimately runs NEAR-FULL the entire time (measured
    // ~98 %). A ¾-capacity high watermark is therefore permanently exceeded, which
    // trips a single-frame DROP every ~6 ms drain cycle (~172×/s) — audible as a
    // constant crackle, NOT the rare ppm correction it was meant to be.
    //
    // Real overflow is already prevented by producer backpressure (raop_rtp retries
    // on a full ring) and real underflow by the silence path below. So drift
    // correction only needs to fire at the TRUE extremes: shed one frame if the
    // ring is brim-full (producer blocked = sender faster than us), pad one if it's
    // within a chunk of empty. Normal near-full operation triggers neither.
    const audio_drift_cfg_t drift_cfg = {
        .high = ring->capacity - 1,               // brim-full only (backpressure handles the rest)
        .low  = PLAYBACK_CHUNK_FRAMES,             // within ~6 ms of empty only
    };

    // DEBUG: crackle diagnosis — count underruns / drift corrections per ~2 s window.
    uint32_t dbg_cycles = 0, dbg_underrun = 0, dbg_drop = 0, dbg_dup = 0, dbg_min_avail = 0xFFFFFFFF;

    for (;;) {
        // Software volume (spec §6e): one Q16.16 gain read per cycle, applied to
        // whatever leaves toward I2S this cycle. AUDIO_VOL_UNITY (0 dB, the power-up
        // default) BYPASSES the multiply entirely — full scale, no needless math.
        // Applied AFTER the drift DROP/DUP so it never perturbs the Phase-4
        // watermark logic; the silence path is already zero, so no gain there.
        int32_t fix = audio_playback_fix_q16();

        // At most one single-frame drift correction per cycle, BEFORE the read.
        size_t avail = audio_ringbuf_available(ring);
        if (avail < dbg_min_avail) dbg_min_avail = avail;
        switch (audio_drift_decide(avail, &drift_cfg)) {
            case AUDIO_DRIFT_DROP:
                dbg_drop++;
                audio_ringbuf_drop(ring, 1);   // shed ~23 µs: buffer above high
                break;
            case AUDIO_DRIFT_DUP: {
                dbg_dup++;
                int16_t f[2];                  // pad ~23 µs: buffer below low
                // Repeat the NEXT-TO-PLAY frame (at tail), written just ahead of
                // the tail chunk below, so the pad is a true local frame-repeat.
                // (Not head-1: that far sample would splice a click while avail
                // sits below low — startup fill, post-underrun, sustained jitter.)
                if (audio_ringbuf_first_frame(ring, f)) {
                    if (fix != AUDIO_VOL_UNITY) audio_volume_apply(f, 2, fix);
                    audio_i2s_write(f, 1);
                }
                break;                         // DUP goes straight to I2S, not the ring
            }
            default:
                break;
        }

        size_t got = audio_ringbuf_read(ring, chunk, PLAYBACK_CHUNK_FRAMES);
        if (got > 0) {
            audio_mix_mono(chunk, got);
            if (fix != AUDIO_VOL_UNITY) audio_volume_apply(chunk, got * 2, fix);
            audio_i2s_write(chunk, got);
        } else {
            dbg_underrun++;
            audio_i2s_write(silence, PLAYBACK_CHUNK_FRAMES);
        }

        // DEBUG: per-window crackle stats (~2 s @ 256 frames / 44.1 kHz). Logs ONLY
        // when a correction actually fired, so a clean stream stays silent (no
        // periodic glitch from logging on the audio task). If crackle returns, this
        // shows whether it's underrun (empty ring) vs drift DROP/DUP over-firing.
        if (++dbg_cycles >= 344) {
            if (dbg_underrun || dbg_drop || dbg_dup) {
                ESP_LOGW("pb_stats", "2s: underrun=%lu drop=%lu dup=%lu min_avail=%lu cap=%u",
                         (unsigned long)dbg_underrun, (unsigned long)dbg_drop, (unsigned long)dbg_dup,
                         (unsigned long)(dbg_min_avail == 0xFFFFFFFFu ? 0 : dbg_min_avail),
                         (unsigned)ring->capacity);
            }
            dbg_cycles = dbg_underrun = dbg_drop = dbg_dup = 0;
            dbg_min_avail = 0xFFFFFFFFu;
        }
    }
}
