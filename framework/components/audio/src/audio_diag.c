#include "audio.h"
#include "audio_i2s.h"   // AUDIO_SAMPLE_RATE_HZ, AUDIO_PIN_CORE

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include <math.h>

#define DIAG_TONE_HZ   440.0f
#define DIAG_AMPLITUDE (0.60f * 32767.0f)
#define DIAG_CHUNK     256

// Continuous-phase 440 Hz sine, pushed through audio_play_pcm() so the whole
// new audio path (ring -> playback task -> I2S -> DAC) is exercised. Retries
// unwritten frames so a full ring applies backpressure instead of dropping
// samples (which would click).
static void diag_tone_task(void *arg) {
    static int16_t buf[DIAG_CHUNK * 2];
    const float inc = 2.0f * (float) M_PI * DIAG_TONE_HZ / (float) AUDIO_SAMPLE_RATE_HZ;
    float phase = 0.0f;

    ESP_LOGI("audio_diag", "diagnostic: emitting %.0f Hz via audio_play_pcm()", DIAG_TONE_HZ);

    for (;;) {
        for (int i = 0; i < DIAG_CHUNK; i++) {
            int16_t s = (int16_t) (DIAG_AMPLITUDE * sinf(phase));
            buf[2 * i] = s;
            buf[2 * i + 1] = s;
            phase += inc;
            if (phase >= 2.0f * (float) M_PI) phase -= 2.0f * (float) M_PI;
        }
        size_t off = 0;
        while (off < DIAG_CHUNK) {
            off += audio_play_pcm(buf + off * 2, DIAG_CHUNK - off);
            if (off < DIAG_CHUNK) vTaskDelay(1);  // ring full -> yield, then retry
        }
    }
}

void audio_diag_tone_start(void) {
    // Pinned to AUDIO_PIN_CORE — same core as the playback consumer, so the
    // SPSC ring stays memory-synchronized (see audio_ringbuf.h precondition).
    xTaskCreatePinnedToCore(diag_tone_task, "diag_tone", 4096, NULL, 4,
                            NULL, AUDIO_PIN_CORE);
}
