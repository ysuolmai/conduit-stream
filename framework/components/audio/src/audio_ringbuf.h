// Single-producer / single-consumer PCM ring buffer. Pure C, no ESP-IDF deps,
// so it is unit-testable on the host. Storage is caller-owned (target: PSRAM;
// host test: a static array), which keeps this file allocator-free.
//
// Concurrency precondition: head/tail are `volatile` so the compiler cannot cache
// a producer- or consumer-owned index in a register across the publish/observe
// (that would let the drain task read a stale head, or the producer a stale tail).
// `volatile` closes only the COMPILER-reordering hole — it is NOT a hardware
// barrier. Correctness therefore still requires the producer and consumer to be
// memory-synchronized by the caller: on the ESP32-S3, pin BOTH tasks to the SAME
// core (audio_producer_core()); a single-core context switch is a full barrier.
// Cross-core use would need _Atomic head/tail with acquire/release (future work).
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// One frame = one stereo sample pair (L,R) = 2 * int16_t.
typedef struct {
    int16_t *storage;      // caller-owned, capacity_frames * 2 int16_t
    size_t   capacity;     // total frame slots (usable = capacity - 1; one reserved for full/empty)
    volatile size_t head;  // producer publishes here (frame index); volatile: no compiler caching
    volatile size_t tail;  // consumer publishes here (frame index); volatile: no compiler caching
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

// Discard up to n readable frames without emitting them (drift DROP, spec §6f).
// Returns frames dropped (< n on underrun). Consumer-side: advances tail only, so
// the SPSC contract is preserved (only the drain task may call this).
size_t audio_ringbuf_drop(audio_ringbuf_t *rb, size_t n);

// Copy the most recently readable frame into out[2] (drift DUP source). Returns
// false if empty. Does NOT advance tail. Consumer-side: reads head-1 without
// touching either index.
bool   audio_ringbuf_last_frame(const audio_ringbuf_t *rb, int16_t out[2]);
