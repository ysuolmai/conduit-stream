# audio

Transport-agnostic audio core. Producers push decoded PCM in via
`audio_play_pcm()`; a playback task drains it to the DAC. The component knows
nothing about any transport — AirPlay / REST / Bluetooth all feed the same
interface (the diagnostic 440 Hz tone is just another producer).

## Structure (delivered in v0.2 Phase 0)

- `include/audio.h` — public API: `audio_init()`, `audio_play_pcm()`, `audio_diag_tone_start()`.
- `src/audio_ringbuf.{h,c}` — pure-C single-producer/single-consumer PCM ring buffer
  (host-unit-tested via `pio test -e native`). Indices have no memory barriers, so
  producer and consumer MUST share a core — see the precondition in the header.
  Phase 4 adds two consumer-side primitives, `audio_ringbuf_drop()` /
  `audio_ringbuf_last_frame()`, used by the drift step (tail-only / index-only, so
  the SPSC contract is preserved).
- `src/audio_i2s.{h,c}` — I2S std-mode TX wrapper (44.1 kHz / 16-bit stereo, PCM5102A
  pins). Defines `AUDIO_SAMPLE_RATE_HZ` and `AUDIO_PIN_CORE`.
- `src/audio_drift.{h,c}` — pure free-run drift watermark decision (spec §6f),
  host-unit-tested (`test/test_audio_drift`). Above the high / below the low mark
  (3/4 and 1/4 of ring capacity) it returns DROP / DUP; `avail == 0` is NONE (the
  underrun silence path owns it, not drift).
- `src/audio_playback.{h,c}` — playback task: prebuffers 250 ms before first output
  and after an underrun, drains the ring into I2S, writes silence while buffering,
  and applies at most one single-frame drift DROP/DUP (~23 µs) per drain cycle so a
  multi-hour session never slowly underruns from sender/DAC ppm mismatch.
- `src/audio.c` — `audio_init()` (brings up I2S, allocates the ~2 s PSRAM ring, starts
  the playback task pinned to `AUDIO_PIN_CORE`) and `audio_play_pcm()`.
- `src/audio_diag.c` — 440 Hz diagnostic sine pushed through `audio_play_pcm()`, proving
  the full ring → playback → I2S → DAC path. Retained from v0.0.1 as the
  audio-path-vs-transport regression divider.

## Producer contract

`audio_play_pcm()` is single-producer: exactly one task may call it, and that task
must be pinned to `AUDIO_PIN_CORE` (the SPSC ring is not cross-core safe). Phase 1
will surface this affinity through the public API before the out-of-component RAOP
decoder becomes the producer.

## Software volume (Phase 5, spec §6e)

`audio_set_volume(float db)` takes an AirPlay dB value (`-144` = mute .. `0` = full)
and stores a Q16.16 linear gain (`pow(10, dB/20) * 65536`, cited to shairport
`player.c`). The playback drain reads it once per cycle and applies it per int16
sample with round-half-away-from-zero + int16 clamp, **bypassing the multiply
entirely at unity** (0 dB, the power-up default) so full-scale playback costs no math.

- `src/audio_volume.{h,c}` — pure dB→Q16 conversion + per-sample apply, host-tested
  (`test/test_audio_volume`): dB→gain vectors, clamp to `[-30, 0]`, the mute sentinel,
  and +/- rounding symmetry.
- Gain is applied **after** the drift DROP/DUP so it never perturbs the Phase-4
  watermark; the DUP pad frame is gained too for exactness; the underrun silence path
  is already zero.
- Transport-agnostic: RAOP calls `audio_set_volume`; the audio core never learns what
  RAOP is. The gain is a plain aligned `volatile int32_t` written by the RTSP task and
  read by the drain task (both pinned to `AUDIO_PIN_CORE`) — atomic, no lock.

## Later

Audio Decoder (ALAC) and RTP-sequence-indexed jitter buffering arrive with the
AirPlay work (v0.2 Phase 3+). The Playback Manager interface stays the same.
