#include "audio.h"
#include "audio_i2s.h"
#include "audio_ringbuf.h"
#include "audio_playback.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_heap_caps.h"
#include "esp_log.h"

#include <stdlib.h>   // abort()
#include <stdatomic.h>

// ~2 s of 44.1 kHz stereo in PSRAM.
#define RING_CAPACITY_FRAMES (AUDIO_SAMPLE_RATE_HZ * 2)

static const char     *TAG = "audio";
static audio_ringbuf_t s_ring;

// Single-producer guard: at most one producer may feed audio_play_pcm() at a
// time. A test-and-set flag (not a per-sample lock) catches a second concurrent
// producer — a contract violation that would corrupt the lock-free SPSC ring.
static atomic_flag s_producer_held = ATOMIC_FLAG_INIT;
static const char *s_producer_owner = "none";

int audio_producer_core(void) {
    return AUDIO_PIN_CORE;
}

bool audio_producer_acquire(const char *who) {
    if (atomic_flag_test_and_set(&s_producer_held)) {
        // Already held: reject the second producer loudly (never silently share).
        ESP_LOGE(TAG, "producer contract violation: '%s' tried to start while '%s' holds the audio path",
                 who ? who : "?", s_producer_owner);
        return false;
    }
    s_producer_owner = who ? who : "?";
    ESP_LOGI(TAG, "audio producer acquired by '%s'", s_producer_owner);
    return true;
}

void audio_producer_release(void) {
    ESP_LOGI(TAG, "audio producer released by '%s'", s_producer_owner);
    s_producer_owner = "none";
    atomic_flag_clear(&s_producer_held);
}

void audio_init(void) {
    audio_i2s_init();

    int16_t *storage = heap_caps_malloc(
        RING_CAPACITY_FRAMES * 2 * sizeof(int16_t), MALLOC_CAP_SPIRAM);
    if (storage == NULL) {
        ESP_LOGE(TAG, "PSRAM ring alloc failed (%u frames)", (unsigned) RING_CAPACITY_FRAMES);
        abort();  // fail loud: the buffer is mandatory
    }
    audio_ringbuf_init(&s_ring, storage, RING_CAPACITY_FRAMES);

    // Pin the consumer (playback) to the same core as the producer (diag/decoder,
    // AUDIO_PIN_CORE). The SPSC ring's plain-size_t indices have no memory
    // barriers, so producer and consumer MUST be single-core-synchronized;
    // co-pinning makes every context switch between them a full barrier.
    BaseType_t ok = xTaskCreatePinnedToCore(audio_playback_task, "playback", 4096,
                                            &s_ring, 5, NULL, AUDIO_PIN_CORE);
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "playback task create failed");
        abort();  // fail loud: audio is dead without the drain task
    }
    ESP_LOGI(TAG, "audio: ring=%u frames (~2s PSRAM), playback task up on core %d",
             (unsigned) RING_CAPACITY_FRAMES, AUDIO_PIN_CORE);
}

size_t audio_play_pcm(const int16_t *frames, size_t n_frames) {
    return audio_ringbuf_write(&s_ring, frames, n_frames);
}
