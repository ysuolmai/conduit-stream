#include "audio_playback.h"
#include "audio_i2s.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define PLAYBACK_CHUNK_FRAMES 256

// Drains the ring into I2S. On underrun, writes a chunk of silence so the DAC
// keeps clocking cleanly instead of stalling or replaying stale samples.
void audio_playback_task(void *arg) {
    audio_ringbuf_t *ring = (audio_ringbuf_t *) arg;
    // static (off the 4 KB task stack). Single-instance task: these buffers are
    // NOT reentrant — do not spawn a second audio_playback_task.
    static int16_t chunk[PLAYBACK_CHUNK_FRAMES * 2];
    static const int16_t silence[PLAYBACK_CHUNK_FRAMES * 2] = {0};

    for (;;) {
        size_t got = audio_ringbuf_read(ring, chunk, PLAYBACK_CHUNK_FRAMES);
        if (got > 0) {
            audio_i2s_write(chunk, got);
        } else {
            audio_i2s_write(silence, PLAYBACK_CHUNK_FRAMES);
        }
    }
}
