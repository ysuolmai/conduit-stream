# AirPlay 1 (RAOP) — Phase 3: RTP receive + AES-CBC decrypt + ALAC decode → PCM → playback

**Goal (spec §5c + §6d + phasing row 3):** "RTP receive + AES decrypt + ALAC decode → PCM →
playback = **first AirPlay audio**." Stand up the RTP audio receive path in `components/raop`, vendor a
real ALAC decoder as a new `components/alac`, hand decoded PCM to the existing `audio_play_pcm()`, and
arbitrate the single-producer audio contract so the 440 Hz diag tone and the RAOP decoder never run at
once.

**Branch:** `feat/airplay-phase1-wifi-mdns` (phases stacked here).

**Architecture:** one new pure-C component `components/alac` (vendored decoder + a tiny pure
fmtp→config mapper); three new **pure** units inside `components/raop` (`rtp_parser`, `aes_frame`,
`alac_config`) that are host-unit-tested with zero ESP-IDF/PSA includes; and one **glue** unit
(`raop_rtp.c`) that owns the receive/decrypt/decode/feed task on the target only. Phase-0-review debt on
`components/audio` is paid here: `AUDIO_PIN_CORE` is promoted into the public header and the SPSC ring
indices are made `volatile`.

**Tech stack:** ESP-IDF 6.0.1, PlatformIO, C11, Unity host tests. AES-128-CBC via **PSA Crypto**
(`psa_cipher_decrypt_setup(PSA_ALG_CBC_NO_PADDING)` + `psa_cipher_set_iv` + `psa_cipher_update`) — the
legacy `mbedtls/aes.h` is treated as unavailable per the Phase-2 toolchain finding; verify against
`~/.platformio/packages/framework-espidf/components/mbedtls/.../psa/crypto.h` at implementation time and
fall back to `mbedtls_aes_crypt_cbc` **only** if that header is confirmed public in this build.

**Do NOT in Phase 3:** RTP **retransmit** / resend-request (0x55/0x56) — that is Phase 4. No timing
channel (0x52/0x53) and no sync (0x54): the receiver free-runs (spec §5c). No jitter-buffer reorder-by-seq
yet — Phase 3 decodes packets in arrival order straight into the existing PCM ring (the ring *is* the
buffer for now; reorder is Phase 4). Do not touch wifi/discovery/rtsp negotiation logic beyond the
RECORD/TEARDOWN handoff hooks. Keep the existing **31** host tests green.

---

## ⚠️ Interop-critical wire format — exactly what we implement (cited)

**Sources:** shairport-sync (Mike Brady) `rtp.c` / `player.c`; philippe44 `RAOP-Player`; the verified
research capture in
`/private/tmp/claude-501/-Users-uziiuzair-ooozzy-conduit-stream/9e1f7bb1-1393-4743-b79a-d21eb5991c91/scratchpad/research-phase3-5.md`
(agent 3 "RTP audio + retransmit + timing", **with the critic corrections G1/G2 applied**).

### RTP audio packet (audio UDP socket) — 12-byte header + encrypted ALAC

```
offset  size  field
  0     1     0x80   V=2,P=0,X=0,CC=0. (First-sync variants use 0x90; audio stays 0x80.)
  1     1     0x60   payload type 96. MARKER bit is the HIGH bit of THIS byte:
                     first audio packet = 0xE0 (0x60|0x80).  ← critic G1/G2: marker is in
                     byte[1], NOT byte[0]; audio byte[1] has the high bit CLEAR (0x60),
                     control types (0x52..0x56) have it SET (0x80|type).
  2     2     sequence number     big-endian (ntohs)
  4     4     RTP timestamp       big-endian (ntohl); +frameLength (352) per packet @44100
  8     4     SSRC                big-endian (ignored)
 12     N     AES-128-CBC( ALAC frame )   N = datagram_len - 12
```

**Dispatch:** `type = packet[1] & 0x7f` (equivalently `& ~0x80`). Phase 3 handles only `0x60` (audio).
Anything else is logged-and-dropped (resend-response `0x56`, sync `0x54`, timing `0x53` are Phase 4+).

### AES-128-CBC decrypt (per packet) — the framing we host-test

```
N        = datagram_len - 12                 (payload length; require datagram_len >= 12)
cipher_n = N & ~(size_t)0x0f                  (largest multiple of 16 — ONLY these are ciphertext)
plain_n  = N - cipher_n                       (trailing remainder is PLAINTEXT, copied verbatim)

for each packet:
    iv := session.aesiv           (16 bytes, RESET from the session IV EVERY packet — CBC
                                    chains within a packet, NEVER across packets)
    AES-128-CBC-decrypt(key=session.aeskey, iv, in=payload[0..cipher_n), out[0..cipher_n))
    memcpy(out + cipher_n, payload + cipher_n, plain_n)     (tail passthrough)
    # out[0..N) is now one raw ALAC frame → alac_decode_frame → interleaved LE int16 PCM
```

Key (16 B) and IV (16 B) are the session values already produced in Phase 2 (`raop_session_t.aeskey`,
`.aesiv`): key = RSA/OAEP-decrypted `rsaaeskey`, IV = base64-decoded `aesiv` from ANNOUNCE. They are
constant for the whole session. The **pure** part we test is the `cipher_n`/`plain_n`/passthrough split
(`aes_frame`); the PSA crypto call itself is glue.

### ALAC config from the SDP `a=fmtp` (12 ints)

`a=fmtp:96 352 0 16 40 10 14 2 255 0 0 44100` → `[pt=96, frameLength=352, compatVer=0, bitDepth=16,
pb=40, mb=10, kb=14, numChannels=2, maxRun=255, maxFrameBytes=0, avgBitRate=0, sampleRate=44100]`.
Mapping onto David Hammerton's `alac_file` (index → field, per research + shairport `init_alac_decoder`):

| fmtp idx | value | `alac_file` field                    |
|---------:|------:|--------------------------------------|
| 1        | 352   | `setinfo_max_samples_per_frame`      |
| 2        | 0     | `setinfo_7a`                         |
| 3        | 16    | `setinfo_sample_size`                |
| 4        | 40    | `setinfo_rice_historymult`           |
| 5        | 10    | `setinfo_rice_initialhistory`        |
| 6        | 14    | `setinfo_rice_kmodifier`             |
| 7        | 2     | `setinfo_7f`                         |
| 8        | 255   | `setinfo_80`                         |
| 9        | 0     | `setinfo_82`                         |
| 10       | 0     | `setinfo_86`                         |
| 11       | 44100 | `setinfo_8a_rate`                    |

`alac_create(samplesize=fmtp[3], numchannels=fmtp[7])` then set the fields above and call
`alac_allocate_buffers()`. Index 0 (payload type) is **not** a codec field. **Decision:** set the
`setinfo_*` fields directly (the `alac_file` struct is fully public in `alac.h`), matching shairport —
*not* the `alac_set_info` "kuki" cookie path. The pure unit `alac_config` parses the 12 ints into a POD
struct we host-test; the glue copies that POD into the decoder.

---

## File structure

```
components/alac/                             # NEW component (pure C decoder + config mapper)
  CMakeLists.txt                             # CREATE
  NOTICE                                     # CREATE: attribution (Hammerton, upstream URLs)
  README.md                                  # CREATE
  include/alac.h                             # VENDOR verbatim (Hammerton, keep license header)
  include/alac_config.h                      # CREATE: pure fmtp→POD mapper (no alac.h dep)
  src/alac.c                                 # VENDOR verbatim (Hammerton, keep license header)
  src/alac_config.c                          # CREATE: pure parser

components/raop/
  src/rtp_parser.h / rtp_parser.c            # CREATE: pure 12-byte RTP header parse + dispatch
  src/aes_frame.h  / aes_frame.c             # CREATE: pure AES-CBC block/remainder framing
  src/raop_rtp.h   / raop_rtp.c              # CREATE: GLUE — receive task, PSA decrypt, decode, feed
  src/rtsp_session.h                         # EDIT: add alac handle + rtp task handle to session
  src/raop.c                                 # EDIT: start rtp task on RECORD; stop on TEARDOWN
  CMakeLists.txt                             # EDIT: add rtp_parser/aes_frame/raop_rtp; REQUIRES alac,audio
  README.md                                  # EDIT

components/audio/
  include/audio.h                            # EDIT: promote AUDIO_PIN_CORE + audio_producer_core();
                                             #        add audio_diag_tone_stop(); doc pin contract
  src/audio_i2s.h                            # EDIT: AUDIO_PIN_CORE now lives in audio.h (re-include)
  src/audio_ringbuf.h                        # EDIT: head/tail → volatile
  src/audio_diag.c                           # EDIT: store task handle; implement stop
  src/audio.c                                # EDIT: single-producer guard + audio_producer_core()

platformio.ini                               # EDIT: add native -I for components/alac/{src,include}

test/test_rtp_parser/test_rtp_parser.c       # CREATE: Unity host tests
test/test_aes_frame/test_aes_frame.c         # CREATE: Unity host tests
test/test_alac_config/test_alac_config.c     # CREATE: Unity host tests
```

**Pure vs glue split (house rule):** `rtp_parser`, `aes_frame`, `alac_config` are pure C, zero
ESP-IDF/PSA/lwip includes — host-unit-tested by `#include`-ing the `.c` directly (as `test_sdp.c` does).
`raop_rtp.c` is target-only glue (sockets + PSA + FreeRTOS task). The vendored `alac.c` is pure C but not
newly tested by us (it is a verified upstream decoder); we test only *our* config mapper feeding it.

---

## Task 1: Vendor the ALAC decoder → `components/alac` (target build only)

**Files:** create `components/alac/{CMakeLists.txt,NOTICE,README.md}`,
`components/alac/include/alac.h`, `components/alac/src/alac.c`.

- [ ] **Step 1:** Copy verbatim (keep the MIT-style license header intact, byte-for-byte):
  - `scratchpad/alac_vendor/alac.h` → `components/alac/include/alac.h`
  - `scratchpad/alac_vendor/alac.c` → `components/alac/src/alac.c`
- [ ] **Step 2: `NOTICE`** — attribution: "ALAC decoder © 2005 David Hammerton
  (http://crazney.net/programs/itunes/alac.html), MIT-style license; vendored via shairport-sync
  `apple_alac/`, https://github.com/mikebrady/shairport-sync. Vendored verbatim; do not rewrite."
- [ ] **Step 3: `CMakeLists.txt`** — pure C, no ESP deps:
  ```cmake
  # alac: vendored David Hammerton ALAC decoder (pure C) + pure fmtp→config mapper.
  # No ESP-IDF deps. Buffers are small (frameLength=352 → a few KB) so malloc in
  # internal RAM is fine; do NOT PSRAM-cap.
  idf_component_register(
      SRCS "src/alac.c" "src/alac_config.c"
      INCLUDE_DIRS "include"
      REQUIRES ""
  )
  ```
- [ ] **Step 4: Build sanity** — `~/.platformio/penv/bin/pio run -e esp32-s3-n16r8` still green (nothing
  references alac yet; this just proves the vendored TU compiles under the Xtensa toolchain). If
  `alac.c` emits warnings-as-errors, add `-Wno-...` **only** for this component via
  `target_compile_options`, never edit the vendored source.
- [ ] **Step 5: Commit** `feat(alac): vendor David Hammerton ALAC decoder (verbatim, MIT)`.

## Task 2: `alac_config` — pure fmtp→POD mapper (host-unit-tested)

**Files:** create `components/alac/include/alac_config.h`, `components/alac/src/alac_config.c`,
`test/test_alac_config/test_alac_config.c`.

- [ ] **Step 1: Header** — a pure POD + parser, **no `alac.h` include** (keeps the unit dependency-free):
  ```c
  #pragma once
  #include <stdint.h>
  // Parsed ALAC "magic cookie" params from the SDP a=fmtp line (11 codec ints).
  typedef struct {
      uint32_t frame_length;      // fmtp[1]  setinfo_max_samples_per_frame
      uint8_t  compat_version;    // fmtp[2]  setinfo_7a
      uint8_t  bit_depth;         // fmtp[3]  setinfo_sample_size
      uint8_t  pb;                // fmtp[4]  setinfo_rice_historymult
      uint8_t  mb;                // fmtp[5]  setinfo_rice_initialhistory
      uint8_t  kb;                // fmtp[6]  setinfo_rice_kmodifier
      uint8_t  num_channels;      // fmtp[7]  setinfo_7f
      uint16_t max_run;           // fmtp[8]  setinfo_80
      uint32_t max_frame_bytes;   // fmtp[9]  setinfo_82
      uint32_t avg_bitrate;       // fmtp[10] setinfo_86
      uint32_t sample_rate;       // fmtp[11] setinfo_8a_rate
  } alac_cfg_t;
  // Parse 12 whitespace-separated ints (optionally prefixed "a=fmtp:") into *out.
  // Uses ints[1..11]; ints[0] (payload type) is ignored. Returns 0 on success,
  // -1 if fewer than 12 ints or any int is out of its field's range.
  int alac_cfg_from_fmtp(const char *fmtp, alac_cfg_t *out);
  ```
- [ ] **Step 2: Write failing host tests** `test/test_alac_config/test_alac_config.c`
  (`#include "alac_config.c"`), covering the canonical line **with and without** the `a=fmtp:` prefix
  and the exact session-stored form (what `sdp.c` keeps in `raop_session_t.fmtp`):
  ```c
  #include <unity.h>
  #include "alac_config.c"
  void setUp(void){} void tearDown(void){}

  void test_canonical_with_pt(void){
      alac_cfg_t c;
      TEST_ASSERT_EQUAL_INT(0, alac_cfg_from_fmtp("96 352 0 16 40 10 14 2 255 0 0 44100", &c));
      TEST_ASSERT_EQUAL_UINT32(352, c.frame_length);
      TEST_ASSERT_EQUAL_UINT8(0,   c.compat_version);
      TEST_ASSERT_EQUAL_UINT8(16,  c.bit_depth);
      TEST_ASSERT_EQUAL_UINT8(40,  c.pb);
      TEST_ASSERT_EQUAL_UINT8(10,  c.mb);
      TEST_ASSERT_EQUAL_UINT8(14,  c.kb);
      TEST_ASSERT_EQUAL_UINT8(2,   c.num_channels);
      TEST_ASSERT_EQUAL_UINT16(255,c.max_run);
      TEST_ASSERT_EQUAL_UINT32(0,  c.max_frame_bytes);
      TEST_ASSERT_EQUAL_UINT32(0,  c.avg_bitrate);
      TEST_ASSERT_EQUAL_UINT32(44100, c.sample_rate);
  }
  void test_accepts_afmtp_prefix(void){
      alac_cfg_t c;
      TEST_ASSERT_EQUAL_INT(0, alac_cfg_from_fmtp("a=fmtp:96 352 0 16 40 10 14 2 255 0 0 44100", &c));
      TEST_ASSERT_EQUAL_UINT32(352, c.frame_length);
      TEST_ASSERT_EQUAL_UINT8(2, c.num_channels);
  }
  void test_rejects_short(void){
      alac_cfg_t c;
      TEST_ASSERT_EQUAL_INT(-1, alac_cfg_from_fmtp("96 352 0 16 40 10 14 2 255 0", &c));
  }
  void test_rejects_garbage(void){
      alac_cfg_t c;
      TEST_ASSERT_EQUAL_INT(-1, alac_cfg_from_fmtp("", &c));
      TEST_ASSERT_EQUAL_INT(-1, alac_cfg_from_fmtp("hello world", &c));
  }
  int main(void){ UNITY_BEGIN();
      RUN_TEST(test_canonical_with_pt); RUN_TEST(test_accepts_afmtp_prefix);
      RUN_TEST(test_rejects_short); RUN_TEST(test_rejects_garbage);
      return UNITY_END(); }
  ```
- [ ] **Step 3: Implement** `alac_config.c` — skip an optional leading `a=fmtp:`, then parse 12
  `strtol` ints; require all 12; range-check each into its field width; assign per the table. Pure C,
  only `<stdint.h>`/`<stdlib.h>`/`<string.h>`.
- [ ] **Step 4: platformio.ini** — add `-I components/alac/include -I components/alac/src` to
  `[env:native] build_flags`. Run `~/.platformio/penv/bin/pio test -e native` → new tests pass, prior
  31 still green.
- [ ] **Step 5: Commit** `feat(alac): pure fmtp→ALAC-config mapper with host tests`.

## Task 3: `rtp_parser` — pure RTP header parse + dispatch (host-unit-tested)

**Files:** create `components/raop/src/rtp_parser.{h,c}`, `test/test_rtp_parser/test_rtp_parser.c`.

- [ ] **Step 1: Header:**
  ```c
  #pragma once
  #include <stdint.h>
  #include <stddef.h>
  #include <stdbool.h>
  #define RTP_HEADER_LEN 12
  #define RTP_PT_AUDIO   0x60   // payload type 96, marker clear
  typedef struct {
      uint8_t  payload_type;    // packet[1] & 0x7f
      bool     marker;          // packet[1] & 0x80  (set on first audio packet)
      uint16_t seq;             // big-endian @2
      uint32_t timestamp;       // big-endian @4
      uint32_t ssrc;            // big-endian @8
      const uint8_t *payload;   // packet + 12
      size_t   payload_len;     // len - 12
  } rtp_header_t;
  // Parse a datagram. Returns 0 on success (len >= 12), -1 otherwise. Does NOT
  // validate payload_type — caller dispatches on out->payload_type.
  int rtp_parse(const uint8_t *pkt, size_t len, rtp_header_t *out);
  ```
- [ ] **Step 2: Failing tests** — canonical audio header, first-packet marker (`0xE0`), big-endian seq
  and timestamp, `+352` timestamp cadence between two packets, and length guards:
  ```c
  #include <unity.h>
  #include "rtp_parser.c"
  void setUp(void){} void tearDown(void){}
  // 0x80 0x60 seq=0x0102 ts=0x0A0B0C0D ssrc=... + 4 payload bytes
  static const uint8_t P[] = {0x80,0x60,0x01,0x02,0x0A,0x0B,0x0C,0x0D,
                              0x11,0x22,0x33,0x44, 0xDE,0xAD,0xBE,0xEF};
  void test_parse_audio(void){
      rtp_header_t h;
      TEST_ASSERT_EQUAL_INT(0, rtp_parse(P, sizeof P, &h));
      TEST_ASSERT_EQUAL_UINT8(RTP_PT_AUDIO, h.payload_type);
      TEST_ASSERT_FALSE(h.marker);
      TEST_ASSERT_EQUAL_UINT16(0x0102, h.seq);
      TEST_ASSERT_EQUAL_UINT32(0x0A0B0C0D, h.timestamp);
      TEST_ASSERT_EQUAL_size_t(4, h.payload_len);
      TEST_ASSERT_EQUAL_UINT8(0xDE, h.payload[0]);
  }
  void test_marker_first_packet(void){
      uint8_t p[16]; memcpy(p,P,sizeof P); p[1]=0xE0;   // 0x60|0x80
      rtp_header_t h; rtp_parse(p,sizeof p,&h);
      TEST_ASSERT_TRUE(h.marker);
      TEST_ASSERT_EQUAL_UINT8(RTP_PT_AUDIO, h.payload_type); // marker stripped
  }
  void test_rejects_short(void){
      rtp_header_t h;
      TEST_ASSERT_EQUAL_INT(-1, rtp_parse(P, 11, &h));
  }
  void test_zero_len_payload_ok(void){
      rtp_header_t h;
      TEST_ASSERT_EQUAL_INT(0, rtp_parse(P, 12, &h));
      TEST_ASSERT_EQUAL_size_t(0, h.payload_len);
  }
  // + RUN_TEST all, main() UNITY_BEGIN/END
  ```
- [ ] **Step 3: Implement** with explicit big-endian reads (`(p[2]<<8)|p[3]`, etc.), no `ntohs` (keeps
  it host-portable/pure). `payload_type = pkt[1] & 0x7f; marker = pkt[1] & 0x80`.
- [ ] **Step 4: Test** `pio test -e native` green.
- [ ] **Step 5: Commit** `feat(raop): pure RTP header parse + dispatch with host tests`.

## Task 4: `aes_frame` — pure AES-CBC block/remainder framing (host-unit-tested)

**Files:** create `components/raop/src/aes_frame.{h,c}`, `test/test_aes_frame/test_aes_frame.c`.

- [ ] **Step 1: Header** — the *framing decision only* (which bytes are ciphertext vs plaintext-tail);
  no crypto:
  ```c
  #pragma once
  #include <stddef.h>
  #include <stdint.h>
  typedef struct {
      size_t cipher_len;   // payload_len & ~0xf   — feed to AES-128-CBC decrypt
      size_t plain_off;    // == cipher_len        — start of passthrough tail
      size_t plain_len;    // payload_len - cipher_len — copied verbatim (plaintext)
  } aes_frame_split_t;
  // Compute the RAOP AES-CBC split for a payload of payload_len bytes.
  void aes_frame_split(size_t payload_len, aes_frame_split_t *out);
  ```
- [ ] **Step 2: Failing tests** — boundaries at 0/15/16/17, a real 1416-byte payload (→1408+8), and an
  exact multiple (tail == 0):
  ```c
  #include <unity.h>
  #include "aes_frame.c"
  void setUp(void){} void tearDown(void){}
  static void chk(size_t n,size_t c,size_t p){
      aes_frame_split_t s; aes_frame_split(n,&s);
      TEST_ASSERT_EQUAL_size_t(c, s.cipher_len);
      TEST_ASSERT_EQUAL_size_t(c, s.plain_off);
      TEST_ASSERT_EQUAL_size_t(p, s.plain_len);
  }
  void test_boundaries(void){ chk(0,0,0); chk(15,0,15); chk(16,16,0); chk(17,16,1); }
  void test_real_frame(void){ chk(1416,1408,8); }
  void test_aligned_no_tail(void){ chk(1408,1408,0); chk(352,352,0); }
  // + RUN_TEST all, main()
  ```
- [ ] **Step 3: Implement** — `out->cipher_len = payload_len & ~(size_t)0x0f; out->plain_off =
  out->cipher_len; out->plain_len = payload_len - out->cipher_len;`.
- [ ] **Step 4: Test** green.
- [ ] **Step 5: Commit** `feat(raop): pure AES-CBC frame split (block vs plaintext tail) with host tests`.

## Task 5: Audio — promote pin contract, `volatile` ring, single-producer handoff (Phase-0 debt)

**Files:** edit `components/audio/include/audio.h`, `src/audio_i2s.h`, `src/audio_ringbuf.h`,
`src/audio_diag.c`, `src/audio.c`.

- [ ] **Step 1 (4a — public pin contract):** move `#define AUDIO_PIN_CORE 1` into the public
  `audio.h`; have `audio_i2s.h` drop its copy and `#include "audio.h"` (or keep the macro only in
  `audio.h`). Add and document:
  ```c
  // The FreeRTOS core that BOTH the playback consumer and any PCM producer MUST
  // pin to. audio_play_pcm() is single-producer and lock-free; its SPSC ring has
  // no cross-core barriers, so every producer task must be created with
  // xTaskCreatePinnedToCore(..., audio_producer_core()). See audio_ringbuf.h.
  int  audio_producer_core(void);          // returns AUDIO_PIN_CORE
  ```
  Update the `audio_play_pcm()` doc comment to state the pin-to-`audio_producer_core()` precondition and
  that **only one** producer may be active at a time.
- [ ] **Step 2 (4b — ring safety):** qualify the SPSC indices `volatile` in `audio_ringbuf.h`:
  ```c
  volatile size_t head;   // producer publishes; volatile closes the compiler
  volatile size_t tail;   // reorder/cache hole for the same-core SPSC contract
  ```
  Rationale to record in the header comment: producer and consumer share `AUDIO_PIN_CORE`, so a
  context switch is a full hardware barrier; `volatile` is the minimum needed to stop the *compiler*
  from caching an index in a register across the write/read. (Stronger `_Atomic` acquire/release is the
  cross-core upgrade path, noted but not required now.) The host tests are single-threaded so `volatile`
  is behavior-neutral — re-run `test/test_ringbuf` to confirm still green.
- [ ] **Step 3 (single-producer handoff):** add `void audio_diag_tone_stop(void);` to `audio.h`. In
  `audio_diag.c` store the task handle and add a stop flag the loop checks each chunk, self-`vTaskDelete`s,
  and clears the handle (start→stop→start must be re-entrant). In `audio.c` add a lightweight guard: an
  `_Atomic int s_producer_gen` (or a claimed-owner tag) so a *second* concurrent producer is detected and
  `ESP_LOGE`'d — belt-and-suspenders behind the contract, not a per-sample lock.
- [ ] **Step 4:** build `pio run -e esp32-s3-n16r8` green; `pio test -e native` — all prior tests
  (incl. `test_ringbuf`) green.
- [ ] **Step 5: Commit** `refactor(audio): public AUDIO_PIN_CORE + volatile SPSC indices + diag stop`.

## Task 6: `raop_rtp` — receive/decrypt/decode/feed task (target-only glue)

**Files:** create `components/raop/src/raop_rtp.{h,c}`; edit `rtsp_session.h` (hold `alac_file *alac` +
`TaskHandle_t rtp_task` + a `volatile bool rtp_run` stop flag).

- [ ] **Step 1: Header** — `int raop_rtp_start(raop_session_t *s);` and `void raop_rtp_stop(raop_session_t
  *s);`. Start: build the ALAC decoder from `s->fmtp` (`alac_cfg_from_fmtp` → `alac_create` → set fields
  → `alac_allocate_buffers`), init a PSA cipher context from `s->aeskey`, then
  `xTaskCreatePinnedToCore(rtp_task, "raop_rtp", 8192, s, prio>diag, &s->rtp_task,
  audio_producer_core())`. Stop: clear `rtp_run`, join/delete task, `alac_free`, destroy PSA key.
- [ ] **Step 2: Receive loop** (`rtp_task`), per the framing table above, all lengths bounded (spec §9,
  untrusted input):
  1. `recvfrom(s->audio_fd, buf, sizeof buf, ...)` with `sizeof buf` = a fixed cap (e.g. 2048;
     RAOP frames are ~1.4 KB). `n <= 0` → continue.
  2. `rtp_parse(buf, n, &h)`; if `!= 0` or `h.payload_type != RTP_PT_AUDIO` → drop + continue.
  3. `aes_frame_split(h.payload_len, &split)`. Guard `split.cipher_len <= sizeof(plainbuf)`.
  4. **PSA AES-128-CBC** into `plainbuf`: `psa_cipher_decrypt_setup(&op, key,
     PSA_ALG_CBC_NO_PADDING)` → `psa_cipher_set_iv(&op, s->aesiv, 16)` **(fresh session IV every
     packet)** → `psa_cipher_update(&op, payload, split.cipher_len, plainbuf, ...)` →
     `psa_cipher_finish`. (Verify the installed `psa/crypto.h` supports this at implementation time;
     fall back to `mbedtls_aes_crypt_cbc(MBEDTLS_AES_DECRYPT, cipher_len, iv_copy, ...)` only if
     `mbedtls/aes.h` is confirmed public.) Then `memcpy(plainbuf + split.cipher_len, payload +
     split.plain_off, split.plain_len)`.
  5. `alac_decode_frame(s->alac, plainbuf, pcmbuf, &pcm_bytes)`. `pcmbuf` sized for
     `frameLength * numChannels * sizeof(int16)` (352·2·2 = 1408 B; static/stack, internal RAM).
  6. Feed with backpressure like `audio_diag`: `n_frames = pcm_bytes/4; off=0; while(off<n_frames){
     off += audio_play_pcm(pcmbuf+off*2, n_frames-off); if(off<n_frames) vTaskDelay(1);}`.
- [ ] **Step 3: Interop notes** to embed as comments: IV reset per packet (never chained); decrypt only
  `cipher_len`, tail is plaintext; free-run (no timing/sync/reorder in Phase 3); decode in arrival order.
- [ ] **Step 4:** cannot host-test glue; correctness rests on the three pure units + build. Build green.
- [ ] **Step 5: Commit** `feat(raop): RTP receive + PSA AES-CBC decrypt + ALAC decode → PCM feed`.

## Task 7: Wire the handoff into the session lifecycle + CMake/component deps

**Files:** edit `components/raop/src/raop.c`, `components/raop/CMakeLists.txt`, `README.md`.

- [ ] **Step 1: RECORD** handler (`raop.c` ~line 190): after setting `RAOP_RECORDING`, perform the
  **single-producer handoff** — `audio_diag_tone_stop();` then `raop_rtp_start(&s_session);`. Guard for
  missing key/fmtp (`have_key` + non-empty fmtp) → 400 if absent.
- [ ] **Step 2: TEARDOWN** (and the silent-peer reclaim path + `raop_session_reset`): call
  `raop_rtp_stop(&s_session)` **before** closing the UDP sockets, then resume the pre-stream producer
  with `audio_diag_tone_start()` (or leave silence — pick tone to keep the known-good path warm, per
  Phase-0 intent). Ensure FLUSH does **not** tear the decoder down (it only affects buffering, Phase 4).
- [ ] **Step 3: CMakeLists** — add `src/rtp_parser.c src/aes_frame.c src/raop_rtp.c` to SRCS; add
  `alac` and `audio` to `PRIV_REQUIRES` (raop now calls `alac_*`, `audio_*`). Because REQUIRES changed:
  `rm -rf framework/.pio/build/esp32-s3-n16r8` then rebuild.
- [ ] **Step 4:** `pio run -e esp32-s3-n16r8` green; `pio test -e native` — all tests green
  (31 prior + rtp_parser + aes_frame + alac_config).
- [ ] **Step 5: README** — document the Phase 3 receive path, the exact framing (link this plan), and
  the single-producer handoff. **Commit** `feat(raop): start RTP decode on RECORD, hand off diag tone`.

---

## Success criteria

- `~/.platformio/penv/bin/pio run -e esp32-s3-n16r8` builds green (fresh `.pio/build` after the
  REQUIRES change).
- `~/.platformio/penv/bin/pio test -e native` passes: the prior **31** tests **plus** the new
  `test_rtp_parser`, `test_aes_frame`, `test_alac_config` (all pure, zero ESP-IDF/PSA includes).
- Static review confirms the wire behavior matches the cited framing: dispatch on `packet[1] & 0x7f`,
  marker in byte[1], per-packet IV reset, `cipher_len = N & ~0xf` with plaintext tail passthrough,
  fmtp indices 1..11 → the ALAC `setinfo_*` table.
- Single-producer invariant holds: `audio_diag_tone_stop()` runs before `raop_rtp_start()` on RECORD;
  `raop_rtp_stop()` + `audio_diag_tone_start()` on TEARDOWN; both producers pinned to
  `audio_producer_core()`.
- Vendored `alac.c`/`alac.h` are byte-identical to source with license header intact; `NOTICE` present.

## Risks / interop honesty (no real iPhone here)

- **No hardware / no live sender.** Verification is build + host tests only; correctness of the receive
  path rests on matching shairport-sync's proven receive+decrypt+decode exactly (framing table above) and
  on the pure-unit tests for the parsing/boundary logic. The decrypt and decode are **not** stubbed.
- **PSA vs legacy AES:** the plan defaults to PSA `PSA_ALG_CBC_NO_PADDING`; at implementation time grep
  the installed `psa/crypto.h` to confirm `psa_cipher_*` availability (Phase-2 precedent) before wiring,
  and only use `mbedtls_aes_crypt_cbc` if `mbedtls/aes.h` is confirmed public.
- **Free-run, no reorder (Phase 3):** packets decoded in arrival order into the existing PCM ring; UDP
  reordering/loss will click until Phase 4 adds the seq-indexed jitter buffer + retransmit. Acceptable
  for "first audio."
- **Sender teardown risk:** some senders may drop a receiver that never sends timing requests (0x52).
  Documented as a known Phase-4 follow-up (add the request-sender if empirically needed); not built now.
- **ALAC output width:** Hammerton writes host-endian int16 (LE on Xtensa) interleaved L/R — matches the
  I2S 16-bit stereo expectation; the feed loop uses the decoder's returned byte count, not a hardcoded
  352, to size the final/partial frame.
