#include "audio_ringbuf.h"
#include <string.h>

void audio_ringbuf_init(audio_ringbuf_t *rb, int16_t *storage, size_t capacity_frames) {
    rb->storage  = storage;
    rb->capacity = capacity_frames;
    rb->head     = 0;
    rb->tail     = 0;
}

size_t audio_ringbuf_available(const audio_ringbuf_t *rb) {
    size_t head = rb->head;
    size_t tail = rb->tail;
    return head >= tail ? head - tail : rb->capacity - tail + head;
}

size_t audio_ringbuf_free_space(const audio_ringbuf_t *rb) {
    // One slot reserved so head==tail always means "empty".
    return rb->capacity - 1 - audio_ringbuf_available(rb);
}

size_t audio_ringbuf_write(audio_ringbuf_t *rb, const int16_t *frames, size_t n_frames) {
    size_t space = audio_ringbuf_free_space(rb);
    if (n_frames > space) n_frames = space;
    if (n_frames == 0) return 0;

    size_t head = rb->head;
    size_t first = rb->capacity - head;
    if (first > n_frames) first = n_frames;
    memcpy(rb->storage + head * 2, frames, first * 2 * sizeof(int16_t));
    if (n_frames > first) {
        memcpy(rb->storage, frames + first * 2,
               (n_frames - first) * 2 * sizeof(int16_t));
    }
    head += n_frames;
    if (head >= rb->capacity) head -= rb->capacity;
    rb->head = head;
    return n_frames;
}

size_t audio_ringbuf_read(audio_ringbuf_t *rb, int16_t *out, size_t n_frames) {
    size_t avail = audio_ringbuf_available(rb);
    if (n_frames > avail) n_frames = avail;
    if (n_frames == 0) return 0;

    size_t tail = rb->tail;
    size_t first = rb->capacity - tail;
    if (first > n_frames) first = n_frames;
    memcpy(out, rb->storage + tail * 2, first * 2 * sizeof(int16_t));
    if (n_frames > first) {
        memcpy(out + first * 2, rb->storage,
               (n_frames - first) * 2 * sizeof(int16_t));
    }
    tail += n_frames;
    if (tail >= rb->capacity) tail -= rb->capacity;
    rb->tail = tail;
    return n_frames;
}

size_t audio_ringbuf_drop(audio_ringbuf_t *rb, size_t n) {
    size_t avail = audio_ringbuf_available(rb);
    if (n > avail) n = avail;
    size_t tail = rb->tail + n;
    if (tail >= rb->capacity) tail -= rb->capacity;
    rb->tail = tail;                            // consumer-side: advance tail only
    return n;
}

bool audio_ringbuf_first_frame(const audio_ringbuf_t *rb, int16_t out[2]) {
    if (audio_ringbuf_available(rb) == 0) return false;
    // Oldest readable frame (next to play) sits at `tail`; index only, no advance.
    out[0] = rb->storage[rb->tail * 2];
    out[1] = rb->storage[rb->tail * 2 + 1];
    return true;
}
