#include "audio.h"
#include "audio_i2s.h"
#include "audio_ringbuf.h"
#include "audio_playback.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_heap_caps.h"
#include "esp_log.h"

// ~2 s of 44.1 kHz stereo in PSRAM.
#define RING_CAPACITY_FRAMES (AUDIO_SAMPLE_RATE_HZ * 2)

static const char     *TAG = "audio";
static audio_ringbuf_t s_ring;

void audio_init(void) {
    audio_i2s_init();

    int16_t *storage = heap_caps_malloc(
        RING_CAPACITY_FRAMES * 2 * sizeof(int16_t), MALLOC_CAP_SPIRAM);
    if (storage == NULL) {
        ESP_LOGE(TAG, "PSRAM ring alloc failed (%u frames)", RING_CAPACITY_FRAMES);
        abort();  // fail loud: the buffer is mandatory
    }
    audio_ringbuf_init(&s_ring, storage, RING_CAPACITY_FRAMES);

    // Pin the consumer (playback) to the same core as the producer (diag/decoder,
    // AUDIO_PIN_CORE). The SPSC ring's plain-size_t indices have no memory
    // barriers, so producer and consumer MUST be single-core-synchronized;
    // co-pinning makes every context switch between them a full barrier.
    xTaskCreatePinnedToCore(audio_playback_task, "playback", 4096, &s_ring, 5,
                            NULL, AUDIO_PIN_CORE);
    ESP_LOGI(TAG, "audio: ring=%u frames (~2s PSRAM), playback task up on core %d",
             RING_CAPACITY_FRAMES, AUDIO_PIN_CORE);
}

size_t audio_play_pcm(const int16_t *frames, size_t n_frames) {
    return audio_ringbuf_write(&s_ring, frames, n_frames);
}
