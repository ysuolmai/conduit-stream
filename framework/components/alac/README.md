# `alac` — vendored Apple Lossless decoder + pure fmtp mapper

Decodes one ALAC frame per RAOP RTP audio packet into interleaved little-endian
16-bit stereo PCM, ready for `audio_play_pcm()`. Two pieces:

| File | Origin | Role |
|---|---|---|
| `src/alac.c`, `include/alac.h` | David Hammerton (2005), MIT — see `NOTICE` | **Vendored verbatim** decoder. Do not rewrite. |
| `src/alac_config.c`, `include/alac_config.h` | This project | **Pure** SDP `a=fmtp` → `alac_cfg_t` mapper (host-tested). |

## Decoder API (Hammerton)

```c
alac_file *alac_create(int samplesize, int numchannels);   // e.g. (16, 2)
void alac_allocate_buffers(alac_file *alac);               // after setinfo_* set
void alac_decode_frame(alac_file *alac, unsigned char *in, void *out, int *outsize);
void alac_free(alac_file *alac);
```

**Critical `alac_decode_frame` contract:** `*outsize` must be set to the output
buffer's *capacity in bytes* **before** the call. On return it holds the number of
PCM bytes written (`out_samples * bytespersample`, where `bytespersample =
(samplesize/8) * numchannels = 4` for 16-bit stereo). Frames = `*outsize / 4`.
Output is interleaved LE `int16` L,R — matches the I2S 16-bit stereo path.

## Configuring the decoder from `a=fmtp`

`alac_cfg_from_fmtp("96 352 0 16 40 10 14 2 255 0 0 44100", &cfg)` parses the 12
ints (an optional `a=fmtp:` prefix is skipped) into `alac_cfg_t`. The target glue
(`components/raop/src/raop_rtp.c`) copies those fields onto the `alac_file`
`setinfo_*` members — the same path shairport-sync's `init_alac_decoder` takes —
then calls `alac_allocate_buffers()`. Field mapping is documented in
`include/alac_config.h`.

`alac_cfg_from_fmtp` is pure (no `alac.h`/ESP-IDF deps) and unit-tested by
`test/test_alac_config`.

## Build

Pure C, no ESP-IDF dependencies (`REQUIRES ""`). Buffers are small at the RAOP
default `frameLength=352` (a few KB), so `malloc` in internal RAM is fine — no
PSRAM cap.
