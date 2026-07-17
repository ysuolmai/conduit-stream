#include "audio_ringbuf.h"

void audio_ringbuf_init(audio_ringbuf_t *rb, int16_t *storage, size_t capacity_frames) {
    rb->storage  = storage;
    rb->capacity = capacity_frames;
    rb->head     = 0;
    rb->tail     = 0;
}

size_t audio_ringbuf_available(const audio_ringbuf_t *rb) {
    return (rb->head - rb->tail + rb->capacity) % rb->capacity;
}

size_t audio_ringbuf_free_space(const audio_ringbuf_t *rb) {
    // One slot reserved so head==tail always means "empty".
    return rb->capacity - 1 - audio_ringbuf_available(rb);
}

size_t audio_ringbuf_write(audio_ringbuf_t *rb, const int16_t *frames, size_t n_frames) {
    size_t space = audio_ringbuf_free_space(rb);
    if (n_frames > space) n_frames = space;
    for (size_t i = 0; i < n_frames; i++) {
        rb->storage[rb->head * 2]     = frames[i * 2];
        rb->storage[rb->head * 2 + 1] = frames[i * 2 + 1];
        rb->head = (rb->head + 1) % rb->capacity;
    }
    return n_frames;
}

size_t audio_ringbuf_read(audio_ringbuf_t *rb, int16_t *out, size_t n_frames) {
    size_t avail = audio_ringbuf_available(rb);
    if (n_frames > avail) n_frames = avail;
    for (size_t i = 0; i < n_frames; i++) {
        out[i * 2]     = rb->storage[rb->tail * 2];
        out[i * 2 + 1] = rb->storage[rb->tail * 2 + 1];
        rb->tail = (rb->tail + 1) % rb->capacity;
    }
    return n_frames;
}

size_t audio_ringbuf_drop(audio_ringbuf_t *rb, size_t n) {
    size_t avail = audio_ringbuf_available(rb);
    if (n > avail) n = avail;
    rb->tail = (rb->tail + n) % rb->capacity;   // consumer-side: advance tail only
    return n;
}

bool audio_ringbuf_first_frame(const audio_ringbuf_t *rb, int16_t out[2]) {
    if (audio_ringbuf_available(rb) == 0) return false;
    // Oldest readable frame (next to play) sits at `tail`; index only, no advance.
    out[0] = rb->storage[rb->tail * 2];
    out[1] = rb->storage[rb->tail * 2 + 1];
    return true;
}
