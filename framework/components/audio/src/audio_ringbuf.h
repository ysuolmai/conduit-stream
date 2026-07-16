// Single-producer / single-consumer PCM ring buffer. Pure C, no ESP-IDF deps,
// so it is unit-testable on the host. Storage is caller-owned (target: PSRAM;
// host test: a static array), which keeps this file allocator-free.
//
// Concurrency precondition: the index publish/observe here uses plain size_t
// with NO memory barriers. Safe only when producer and consumer are memory-
// synchronized by the caller. On the ESP32-S3 this means pinning the producer
// and consumer tasks to the SAME core (a single-core context switch is a full
// barrier). Do not run them on different cores without adding acquire/release
// on head/tail.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// One frame = one stereo sample pair (L,R) = 2 * int16_t.
typedef struct {
    int16_t *storage;      // caller-owned, capacity_frames * 2 int16_t
    size_t   capacity;     // total frame slots (usable = capacity - 1; one reserved for full/empty)
    size_t   head;         // producer writes here (frame index)
    size_t   tail;         // consumer reads here (frame index)
} audio_ringbuf_t;

// storage must hold (capacity_frames * 2) int16_t. Usable capacity is
// capacity_frames - 1 (one reserved slot). Initializes empty.
void   audio_ringbuf_init(audio_ringbuf_t *rb, int16_t *storage, size_t capacity_frames);

// Frames currently available to read.
size_t audio_ringbuf_available(const audio_ringbuf_t *rb);

// Free frame slots available to write.
size_t audio_ringbuf_free_space(const audio_ringbuf_t *rb);

// Write up to n_frames from `frames`. Returns frames actually written
// (< n_frames when the buffer fills). Never overwrites unread data.
size_t audio_ringbuf_write(audio_ringbuf_t *rb, const int16_t *frames, size_t n_frames);

// Read up to n_frames into `out`. Returns frames actually read
// (< n_frames on underrun, 0 when empty).
size_t audio_ringbuf_read(audio_ringbuf_t *rb, int16_t *out, size_t n_frames);
