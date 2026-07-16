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
- `src/audio_i2s.{h,c}` — I2S std-mode TX wrapper (44.1 kHz / 16-bit stereo, PCM5102A
  pins). Defines `AUDIO_SAMPLE_RATE_HZ` and `AUDIO_PIN_CORE`.
- `src/audio_playback.{h,c}` — playback task: drains the ring into I2S, writes silence
  on underrun.
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

## Later

Audio Decoder (ALAC) and RTP-sequence-indexed jitter buffering arrive with the
AirPlay work (v0.2 Phase 3+). The Playback Manager interface stays the same.
