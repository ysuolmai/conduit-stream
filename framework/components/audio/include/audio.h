// Transport-agnostic audio core. Producers (RAOP decoder, diag tone) push PCM via
// audio_play_pcm(); a playback task drains it to the DAC. Knows nothing about
// any network transport.
#pragma once
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// The FreeRTOS core that BOTH the playback consumer and EVERY PCM producer MUST
// pin to. The SPSC ring behind audio_play_pcm() has no cross-core memory
// barriers (its indices are `volatile`, which stops only compiler reordering), so
// producer and consumer must share a core — on the ESP32-S3 a single-core context
// switch is a full hardware barrier. Create producer tasks with
// xTaskCreatePinnedToCore(..., audio_producer_core()). See audio_ringbuf.h.
#define AUDIO_PIN_CORE 1

// Returns AUDIO_PIN_CORE. Prefer this over the macro in out-of-component callers
// so the affinity contract is a linked symbol, not a copied constant.
int    audio_producer_core(void);

// Bring up I2S, allocate the PSRAM ring buffer, start the playback task.
void   audio_init(void);

// Enqueue up to n_frames of interleaved 16-bit stereo PCM. Returns frames
// accepted (< n_frames when the buffer is full — caller should retry the rest).
//
// SINGLE PRODUCER, lock-free: only ONE task may call this at a time, and it MUST
// be pinned to audio_producer_core(). Concurrent producers corrupt the ring —
// callers arbitrate a clean handoff (stop one producer before starting the next)
// and bracket their run with audio_producer_acquire()/audio_producer_release().
size_t audio_play_pcm(const int16_t *frames, size_t n_frames);

// Single-producer guard (belt-and-suspenders behind the pin/handoff contract, not
// a per-sample lock). A producer calls acquire() before its first audio_play_pcm()
// and release() when it stops. Returns true if the claim succeeded; a second
// concurrent acquire returns false and is logged as a contract violation.
bool   audio_producer_acquire(const char *who);
void   audio_producer_release(void);

// Set playback volume from an AirPlay dB value (-144 = mute .. 0 = full). Applied
// as software gain on the int16 PCM in the playback drain (the PCM5102A has no gain
// pin). Bypassed at unity (0 dB) to save cycles. Transport-agnostic: RAOP calls
// this; the audio core never learns what RAOP is. Thread-safe: stores a single
// aligned int32 read by the drain task (both pinned to audio_producer_core(), so an
// aligned 32-bit load/store is atomic — no torn read, no lock).
void   audio_set_volume(float db);

// Phase 0 diagnostic: start a task that streams a 440 Hz sine through
// audio_play_pcm() (the retained v0.0.1 known-good path). It is the pre-stream
// producer — the RAOP path stops it (audio_diag_tone_stop) before it begins
// streaming and resumes it on TEARDOWN. start() is idempotent; a second call
// while the tone is already running is a no-op.
void   audio_diag_tone_start(void);

// Stop the diagnostic tone producer and BLOCK until its task has fully exited
// (so the caller may immediately claim the single-producer slot). No-op if the
// tone is not running. start()/stop() are re-entrant across repeated handoffs.
void   audio_diag_tone_stop(void);

#ifdef __cplusplus
}
#endif
