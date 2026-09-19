#include "audio.h"
#include "audio_i2s.h"
#include "audio_ringbuf.h"
#include "audio_playback.h"
#include "audio_volume.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_heap_caps.h"
#include "esp_log.h"

#include <stdlib.h>   // abort()
#include <stdatomic.h>

// Two seconds of stereo PCM is ~353 KB. Together with the reduced 128-slot RTP
// reorder window this fits comfortably in 2 MB PSRAM and prevents decoder
// backpressure from stalling UDP receive during sender bursts.
#define RING_CAPACITY_FRAMES (AUDIO_SAMPLE_RATE_HZ * 2)

static const char     *TAG = "audio";
static audio_ringbuf_t s_ring;

// Single-producer guard: at most one producer may feed audio_play_pcm() at a
// time. A test-and-set flag (not a per-sample lock) catches a second concurrent
// producer — a contract violation that would corrupt the lock-free SPSC ring.
static atomic_flag s_producer_held = ATOMIC_FLAG_INIT;
static const char *s_producer_owner = "none";

// Playback volume as a Q16.16 linear gain, written by the RTSP task (audio_set_volume)
// and read by the drain task (audio_playback_fix_q16). Both pinned to
// audio_producer_core(); an aligned int32 load/store is atomic on the LX7, so a
// plain `volatile` (compiler-reorder barrier only) is sufficient — no torn read.
// Powers up at unity (0 dB): the drain bypasses the multiply until a sender sets it.
static volatile int32_t s_fix_q16 = AUDIO_VOL_UNITY;

int32_t audio_playback_fix_q16(void) { return s_fix_q16; }

void audio_set_volume(float db) {
    int32_t q = audio_volume_db_to_q16(db);
    s_fix_q16 = q;
    ESP_LOGI(TAG, "volume %.1f dB -> gain q16=%ld%s", db, (long)q,
             q == AUDIO_VOL_UNITY ? " (unity, bypass)" : (q == 0 ? " (mute)" : ""));
}

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
    ESP_LOGI(TAG, "audio: ring=%u frames, playback task up on core %d",
             (unsigned) RING_CAPACITY_FRAMES, AUDIO_PIN_CORE);
}

size_t audio_play_pcm(const int16_t *frames, size_t n_frames) {
    return audio_ringbuf_write(&s_ring, frames, n_frames);
}
