#include "audio.h"
#include "audio_i2s.h"   // AUDIO_SAMPLE_RATE_HZ (AUDIO_PIN_CORE via audio.h)

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include <math.h>

#define DIAG_TONE_HZ   440.0f
#define DIAG_AMPLITUDE (0.60f * 32767.0f)
#define DIAG_CHUNK     256

static const char *TAG = "audio_diag";

// The diag tone is the pre-stream producer. The RAOP path hands off to/from it
// (stop before streaming, resume on TEARDOWN), so start/stop must be a clean,
// re-entrant pair: stop() BLOCKS until the task has fully exited and released the
// single-producer slot, so the next producer can claim it without overlap.
static TaskHandle_t     s_diag_task = NULL;
static volatile bool    s_diag_run  = false;   // producer clears -> loop exits
static volatile bool    s_diag_done = false;   // task sets just before it deletes

// Continuous-phase 440 Hz sine, pushed through audio_play_pcm() so the whole
// audio path (ring -> playback task -> I2S -> DAC) is exercised. Retries
// unwritten frames so a full ring applies backpressure instead of dropping
// samples (which would click).
static void diag_tone_task(void *arg) {
    (void)arg;
    static int16_t buf[DIAG_CHUNK * 2];
    const float inc = 2.0f * (float) M_PI * DIAG_TONE_HZ / (float) AUDIO_SAMPLE_RATE_HZ;
    float phase = 0.0f;

    ESP_LOGI(TAG, "diagnostic: emitting %.0f Hz via audio_play_pcm()", DIAG_TONE_HZ);

    while (s_diag_run) {
        for (int i = 0; i < DIAG_CHUNK; i++) {
            int16_t s = (int16_t) (DIAG_AMPLITUDE * sinf(phase));
            buf[2 * i] = s;
            buf[2 * i + 1] = s;
            phase += inc;
            if (phase >= 2.0f * (float) M_PI) phase -= 2.0f * (float) M_PI;
        }
        size_t off = 0;
        while (off < DIAG_CHUNK && s_diag_run) {
            off += audio_play_pcm(buf + off * 2, DIAG_CHUNK - off);
            if (off < DIAG_CHUNK) vTaskDelay(1);  // ring full -> yield, then retry
        }
    }

    audio_producer_release();
    s_diag_task = NULL;
    s_diag_done = true;   // publish AFTER clearing the handle: stop() waits on this
    vTaskDelete(NULL);
}

void audio_diag_tone_start(void) {
    if (s_diag_task != NULL) {
        ESP_LOGW(TAG, "diag tone already running");  // idempotent: never a 2nd producer
        return;
    }
    if (!audio_producer_acquire("diag_tone")) return;  // another producer owns the path
    s_diag_run  = true;
    s_diag_done = false;
    // Pinned to AUDIO_PIN_CORE — same core as the playback consumer, so the SPSC
    // ring stays memory-synchronized (see audio_ringbuf.h precondition).
    if (xTaskCreatePinnedToCore(diag_tone_task, "diag_tone", 4096, NULL, 4,
                                &s_diag_task, audio_producer_core()) != pdPASS) {
        ESP_LOGE(TAG, "diag tone task create failed");
        audio_producer_release();
        s_diag_task = NULL;
    }
}

void audio_diag_tone_stop(void) {
    if (s_diag_task == NULL) return;   // not running
    s_diag_run = false;                // ask the loop to exit
    while (!s_diag_done) vTaskDelay(1); // block until it has released the producer slot
}
