// Phase 3 RTP audio receive path (target-only glue). Owns a FreeRTOS task that
// reads the bound audio UDP socket, parses the RTP header, AES-128-CBC-decrypts
// the payload with the session key/IV, ALAC-decodes the frame, and feeds the
// resulting interleaved LE int16 stereo PCM to audio_play_pcm().
//
// Single-producer contract: raop_rtp_start() claims the audio producer slot and
// pins its task to audio_producer_core(); the caller MUST stop the diagnostic
// tone (the other producer) before calling it. raop_rtp_stop() blocks until the
// task has fully exited and released the producer slot.
#pragma once

#include "rtsp_session.h"

// Build the ALAC decoder from s->fmtp, import the AES key, claim the audio
// producer, and spawn the receive/decrypt/decode task on audio_producer_core().
// Requires s->have_key and a valid s->fmtp. Returns 0 on success, -1 on failure
// (on failure the producer slot is NOT held, so the caller can resume the tone).
int  raop_rtp_start(raop_session_t *s);

// Stop the receive task and BLOCK until it has exited and released the producer
// slot; free the decoder and destroy the AES key. Idempotent — a no-op if the
// task is not running. Returns true if a running stream was actually stopped
// (so the caller knows to resume the pre-stream producer), false otherwise.
bool raop_rtp_stop(void);
