# AirPlay 1 (RAOP) — Phase 5: volume software gain + DAAP text metadata + RGB status LED

**Goal (spec §5b + §6b + §6e + §7 + phasing row 5):** "Volume + DAAP metadata + RGB LED states →
**volume slider works, track logs, LED reflects state.**" Parse the sender's `SET_PARAMETER` control
messages — dispatch on `Content-Type`: `text/parameters` → `volume:` (and optional `progress:`);
`application/x-dmap-tagged` → DMAP/DAAP TLV metadata — apply volume as a **software gain** on the int16
PCM in the playback drain, hold the current title/artist/album in a small metadata store and **log on
change**, and drive a **WS2812 RGB status LED on GPIO48** through four states
(NEEDS_CREDS / WIFI_CONNECTING / CONNECTED_IDLE / STREAMING).

**Branch:** `feat/airplay-phase1-wifi-mdns` (phases stacked here).

**Architecture:** four new **pure** units (zero ESP-IDF/PSA/lwip includes, `#include`-the-`.c` host tests
like `test_sdp.c`):

- `components/audio/src/audio_volume.{h,c}` — dB→Q16.16 fixed-point gain + per-sample apply+clamp.
- `components/raop/src/raop_volume.{h,c}` — the `volume: <float>\r\n` line parser (+ `progress:`).
- `components/raop/src/dmap.{h,c}` — DMAP/DAAP TLV walker → `{title,artist,album}` bounded struct.
- `components/raop/src/setparam.{h,c}` — `SET_PARAMETER` `Content-Type` → dispatch-kind decision.
- `components/system/src/led_state.{h,c}` — LED state → (r,g,b) colour map (pure).

…and the **glue** that wires them: the audio drain (`audio_playback.c`) grows a bypass-at-unity gain
step reading a `volatile int32_t` set by the new public `audio_set_volume(float db)`; `raop.c`'s
`SET_PARAMETER` branch dispatches into the pure parsers and updates a `raop_metadata` store + calls
`audio_set_volume`; a new `system_led.c` drives `espressif/led_strip` (guarded so a missing LED never
crashes); and `main.cpp` + a small raop event callback wire the four state transitions.

**Tech stack:** ESP-IDF 6.0.1, PlatformIO, C11, Unity host tests. WS2812 via the managed component
**`espressif/led_strip`** (RMT backend) declared in `components/system/idf_component.yml`.

**Keep intact:** Phases 0–4 — the first-audio decrypt+decode pipeline, seq reorder/retransmit/timing, the
drift drop/dup safeguard, and the single-producer diag-tone handoff. The existing **75** host tests MUST
stay green; the firmware MUST build green (`-e esp32-s3-n16r8`). The audio component stays
transport-agnostic (it gains a *gain*, never any DMAP/RAOP knowledge); DMAP parsing + the metadata store
live in `raop`.

---

## ⚠️ Interop-critical formats — exactly what we implement (cited)

**Sources** (quoted in the research capture at
`/private/tmp/claude-501/-Users-uziiuzair-ooozzy-conduit-stream/9e1f7bb1-1393-4743-b79a-d21eb5991c91/scratchpad/research-phase3-5.md`,
agent 2 "RAOP volume/DAAP/LED", critic notes G11/G12/G13 applied):
- philippe44 `RAOP-Player` `raop_client.c`: `#define VOLUME_MIN -30`, `#define VOLUME_MAX 0`, mute
  sentinel `-144.0`, wire format `sprintf(a, "volume: %f\r\n", vol)` and
  `sprintf(a, "progress: %u/%u/%u\r\n", started_ts, now, end)`.
- shairport-sync `rtsp.c`: `SET_PARAMETER` dispatch on `Content-Type` (`text/parameters` vs
  `application/x-dmap-tagged`); `volume:`/`progress:` prefix matching; DMAP TLV loop = 4-byte code +
  `ntohl` 4-byte length + value. `player.c`: software volume as `65536.0 * gain` Q16 fixed-point,
  `pow(10, dB/20)` mapping.
- espressif/led_strip component + `led_strip_types.h`: `led_strip_new_rmt_device`,
  `LED_MODEL_WS2812`, `LED_STRIP_COLOR_COMPONENT_FMT_GRB`, `led_strip_set_pixel`, `led_strip_refresh`.

The research already quotes these byte-for-byte, so **no live WebFetch is needed** — nothing here is
ambiguous. (Design forks resolved below: G12 volume curve, G11 `mlit`, G13 LED GPIO.)

### VOLUME — `SET_PARAMETER`, `Content-Type: text/parameters`

```
body line:  "volume: -14.500000\r\n"      float dB,  -30.0 (quiet) .. 0.0 (full)
                                           exactly -144.0 == MUTE sentinel
parse:      prefix "volume: " (8 chars, note the space), then a float.
```

**dB → linear → Q16.16 gain** (what we implement, cited to shairport `player.c`):

```
if (db <= -144.0f)   gain = 0.0f;                 // AirPlay mute sentinel (a flag, NOT a real dB)
else {
    if (db >  0.0f)   db =  0.0f;                 // clamp out-of-range senders to [-30, 0]
    if (db < -30.0f)  db = -30.0f;
    gain = powf(10.0f, db / 20.0f);               // == powf(10, 0.05*db)
}
fix_q16 = (int32_t)(gain * 65536.0f + 0.5f);      // Q16.16; 1.0 == 65536
```

**Design fork (critic G12):** we use the **plain `pow(10, dB/20)`** mapping (the raw AirPlay dB → PCM
attenuation), **not** shairport's perceptual `vol2attn` curve. Rationale: it is the mathematically
correct attenuation for the on-wire dB and is the simplest faithful mapping; documented as an assumption.

**Per-sample apply** (Q16 fixed-point, no float in the hot path, sign-aware round-to-nearest + clamp):

```
static inline int16_t apply(int16_t s, int32_t fix_q16) {
    int32_t prod = (int32_t)s * fix_q16;          // |s|<=32768, fix<=65536 → fits int32? see note
    int32_t half = prod >= 0 ? 0x8000 : -0x8000;  // round half away from zero
    int32_t v    = (prod + half) >> 16;
    if (v >  32767) v =  32767;
    if (v < -32768) v = -32768;
    return (int16_t)v;
}
```

> **Overflow note:** `32768 * 65536 = 2^31` overflows signed int32. Since we **clamp gain to ≤ 0 dB**
> (`fix_q16 ≤ 65536`) and int16 magnitude ≤ 32768, the product is ≤ `2^31`; to be safe the multiply uses
> `int64_t prod = (int64_t)s * fix_q16;` — one 32×32 MAC on the LX7, negligible. Gain never exceeds unity
> so amplification/overflow is structurally impossible; the clamp guards only rounding at the extreme.

**Unity bypass (spec §6e):** when `fix_q16 == 65536` (0 dB, the power-up default), the drain skips the
multiply entirely and writes the chunk unmodified — "bypassed at full scale to avoid needless math."

**Test vectors** (research agent 2, `test_vectors`): dB→fix_q16: `-144→0`, `-30→2072`, `-20→6554`,
`-14.5→12346`, `-6→32846`, `0→65536`. apply(20000): `@0dB→20000`, `@-6dB→10024`, `@-30dB→632`,
`@mute→0`.

### METADATA — `SET_PARAMETER`, `Content-Type: application/x-dmap-tagged`

Binary body = a sequence of **DMAP TLVs**, each: `[4 ASCII code][4-byte BE uint32 length][length bytes
value]`. Text values are **UTF-8, NOT NUL-terminated**. Codes (read big-endian 32-bit):

| code (4-char) | DMAP name | meaning |
|---|---|---|
| `minm` | dmap.itemname | **track title** |
| `asar` | daap.songartist | **artist** |
| `asal` | daap.songalbum | **album** |
| `mlit` | dmap.listingitem | **container** — value is itself a sub-TLV sequence (recurse) |

**Design fork (critic G11):** whether the sender wraps fields in an outer `mlit` container is
sender-dependent; the **recursive** walker handles both flat and `mlit`-wrapped bodies with no branch,
so it is robust either way. Every length is bounded against the body (untrusted input, spec §9): stop if
`off + 8 > len` or `off + vlen > len`; copy at most `DMAP_STR_MAX-1` bytes then NUL-terminate; `vlen == 0`
= a cleared field (empty string).

**Test vector** (research): title "Hi" = `6D 69 6E 6D 00 00 00 02 48 69`; artist "AB" =
`61 73 61 72 00 00 00 02 41 42`.

### PROGRESS (optional, `text/parameters`)

`progress: <start>/<current>/<end>\r\n` — three 32-bit RTP timestamps @44100 Hz. We parse it, log
elapsed/total once, and otherwise ignore it (free-run receiver; no seek UI). Parsing is folded into
`raop_volume.c` (same `text/parameters` line scan) but is **optional** — not required by the phasing row.

### RGB STATUS LED — WS2812 on GPIO48 (spec §7)

Managed component `espressif/led_strip` (RMT backend), single LED, GRB order. States → colours:

| state | colour (r,g,b) | when |
|---|---|---|
| `LED_ST_NEEDS_CREDS` | red `(16,0,0)` | boot, no Wi-Fi credentials |
| `LED_ST_WIFI_CONNECTING` | amber `(16,6,0)` | station connecting |
| `LED_ST_CONNECTED_IDLE` | blue `(0,0,16)` | Wi-Fi up (GOT_IP), no live stream |
| `LED_ST_STREAMING` | green `(0,16,0)` | RAOP `RECORD` live |

Low brightness (≤16/255) — the onboard LED is bright and this is a status indicator, not lighting.
Solid colours, no animation (KISS per task). **Design fork (critic G13):** GPIO is a compile-time
`#define STATUS_LED_GPIO 48` (a few DevKitC-1 revisions route it to 38); default 48, documented. If
`led_strip_new_rmt_device` fails (LED not wired / RMT unavailable) the handle stays `NULL` and every
`system_led_set_state` becomes a logged no-op — **it must never crash the receiver**.

---

## File structure

```
components/audio/
  src/audio_volume.h / audio_volume.c   # CREATE pure: dB→Q16 gain + per-sample apply+clamp (bypass @unity)
  src/audio_playback.c                  # EDIT glue: read volatile g_fix_q16; bypass-or-apply per chunk
  src/audio.c                           # EDIT glue: g_fix_q16 storage + audio_set_volume()
  include/audio.h                       # EDIT: declare audio_set_volume(float db)
  CMakeLists.txt                        # EDIT: add src/audio_volume.c

components/raop/
  src/raop_volume.h / raop_volume.c     # CREATE pure: "volume: %f" + "progress: %u/%u/%u" line parsers
  src/dmap.h / dmap.c                    # CREATE pure: DMAP/DAAP TLV walker → dmap_meta_t (bounded)
  src/setparam.h / setparam.c            # CREATE pure: Content-Type → SETPARAM_{VOLUME,METADATA,OTHER}
  src/raop_metadata.h / raop_metadata.c  # CREATE glue-ish: current title/artist/album store; log on change
  src/raop.c                             # EDIT glue: SET_PARAMETER → dispatch → audio_set_volume + metadata
  src/raop.h (include/raop.h)            # EDIT: raop_set_event_cb() for STREAMING/IDLE LED transitions
  CMakeLists.txt                         # EDIT: add raop_volume.c dmap.c setparam.c raop_metadata.c; REQUIRES audio (already)

components/system/
  src/led_state.h / led_state.c          # CREATE pure: system_led_state_t → led_rgb_t colour map
  src/system_led.h / system_led.c        # CREATE glue: led_strip init (guarded) + system_led_set_state()
  include/system_led.h                   # (public wrapper re-exporting the state enum + setter)
  idf_component.yml                      # CREATE: dependency espressif/led_strip ^3.0.3
  CMakeLists.txt                         # EDIT: add led_state.c system_led.c; PRIV_REQUIRES driver esp_driver_rmt led_strip

src/main.cpp                             # EDIT: system_led_init(); NEEDS_CREDS/CONNECTING/IDLE transitions; raop event cb

test/test_audio_volume/test_audio_volume.c   # CREATE Unity host tests (dB→q16 + apply/clamp)
test/test_raop_volume/test_raop_volume.c     # CREATE Unity host tests (volume/progress line parse)
test/test_dmap/test_dmap.c                   # CREATE Unity host tests (TLV walk + truncated/malformed)
test/test_setparam/test_setparam.c           # CREATE Unity host tests (Content-Type dispatch)
test/test_led_state/test_led_state.c         # CREATE Unity host tests (state→colour map)

platformio.ini                           # EDIT [env:native] build_flags: add -I components/system/src is already present;
                                         #   audio/src + raop/src already on the include path — confirm, add if missing.
```

**Pure vs glue (house rule):** `audio_volume`, `raop_volume`, `dmap`, `setparam`, `led_state` are pure C,
host-tested by `#include`-ing the `.c` directly. `audio_playback.c`, `audio.c`, `raop.c`,
`raop_metadata.c`, `system_led.c`, `main.cpp` are target-only glue. `[env:native]` already lists
`-I components/audio/src -I components/system/src -I components/raop/src` — **no new `-I` needed**; the
plan's platformio edit is only a verification step (add nothing if already covering these dirs).

**After the `idf_component.yml` add:** `rm -rf framework/.pio/build/esp32-s3-n16r8` before rebuilding so
the managed `led_strip` component downloads (per repo CLAUDE.md).

---

## Task 1: `audio_volume` — pure dB→Q16 gain + per-sample apply (host-tested)

**TDD:** write `test_audio_volume.c` first (RED), then the unit.

`components/audio/src/audio_volume.h`
```c
// Pure volume math (spec §6e): RAOP dB (-144=mute .. 0=full) → Q16.16 linear gain,
// and per-sample int16 apply with sign-aware rounding + clamp. No ESP-IDF deps →
// host-unit-testable. The playback drain caches one fix_q16 and bypasses the whole
// multiply when it equals AUDIO_VOL_UNITY (0 dB) — see audio_playback.c.
#pragma once
#include <stdint.h>
#include <stddef.h>

#define AUDIO_VOL_UNITY 65536   // Q16.16 representation of gain 1.0 (0 dB)

// Convert an AirPlay volume dB value to a Q16.16 linear gain.
//   db <= -144  → 0            (mute sentinel; a flag, not a real level)
//   db clamped to [-30, 0]     (out-of-range senders can't amplify)
//   otherwise   → round(pow(10, db/20) * 65536)
int32_t audio_volume_db_to_q16(float db);

// Multiply n_samples int16 samples (interleaved L/R = 2 per frame is irrelevant
// here; operates per-sample) in place by fix_q16, round-to-nearest, clamp to
// int16. Caller SHOULD skip this call when fix_q16 == AUDIO_VOL_UNITY.
void audio_volume_apply(int16_t *buf, size_t n_samples, int32_t fix_q16);
```

`components/audio/src/audio_volume.c`
```c
#include "audio_volume.h"
#include <math.h>

int32_t audio_volume_db_to_q16(float db) {
    float gain;
    if (db <= -144.0f) {
        gain = 0.0f;                       // AirPlay mute sentinel
    } else {
        if (db >  0.0f)  db =  0.0f;       // clamp to [-30, 0]
        if (db < -30.0f) db = -30.0f;
        gain = powf(10.0f, db / 20.0f);    // == powf(10, 0.05*db)
    }
    return (int32_t)(gain * 65536.0f + 0.5f);
}

void audio_volume_apply(int16_t *buf, size_t n_samples, int32_t fix_q16) {
    for (size_t i = 0; i < n_samples; i++) {
        int64_t prod = (int64_t)buf[i] * fix_q16;     // 64-bit: 32768*65536 == 2^31
        int64_t half = prod >= 0 ? 0x8000 : -0x8000;  // round half away from zero
        int32_t v    = (int32_t)((prod + half) >> 16);
        if (v >  32767) v =  32767;
        if (v < -32768) v = -32768;
        buf[i] = (int16_t)v;
    }
}
```

`test/test_audio_volume/test_audio_volume.c`
```c
#include <unity.h>
#include "audio_volume.c"
void setUp(void){} void tearDown(void){}

void test_db_to_q16_vectors(void){
    TEST_ASSERT_EQUAL_INT32(0,     audio_volume_db_to_q16(-144.0f)); // mute
    TEST_ASSERT_EQUAL_INT32(2072,  audio_volume_db_to_q16(-30.0f));
    TEST_ASSERT_EQUAL_INT32(6554,  audio_volume_db_to_q16(-20.0f));
    TEST_ASSERT_EQUAL_INT32(12346, audio_volume_db_to_q16(-14.5f));
    TEST_ASSERT_EQUAL_INT32(32846, audio_volume_db_to_q16(-6.0f));
    TEST_ASSERT_EQUAL_INT32(65536, audio_volume_db_to_q16(0.0f));    // unity
}
void test_db_clamp_out_of_range(void){
    TEST_ASSERT_EQUAL_INT32(65536, audio_volume_db_to_q16(6.0f));    // >0 clamps to unity
    TEST_ASSERT_EQUAL_INT32(2072,  audio_volume_db_to_q16(-50.0f));  // <-30 clamps to -30
    TEST_ASSERT_EQUAL_INT32(0,     audio_volume_db_to_q16(-200.0f)); // below sentinel = mute
}
void test_apply_unity_is_identity(void){
    int16_t b[4] = {20000, -20000, 32767, -32768};
    audio_volume_apply(b, 4, AUDIO_VOL_UNITY);
    TEST_ASSERT_EQUAL_INT16(20000, b[0]);
    TEST_ASSERT_EQUAL_INT16(-20000, b[1]);
    TEST_ASSERT_EQUAL_INT16(32767, b[2]);
    TEST_ASSERT_EQUAL_INT16(-32768, b[3]);
}
void test_apply_gain_vectors(void){
    int16_t b0 = 20000; audio_volume_apply(&b0, 1, audio_volume_db_to_q16(0.0f));
    TEST_ASSERT_EQUAL_INT16(20000, b0);
    int16_t b6 = 20000; audio_volume_apply(&b6, 1, audio_volume_db_to_q16(-6.0f));
    TEST_ASSERT_EQUAL_INT16(10024, b6);
    int16_t b30 = 20000; audio_volume_apply(&b30, 1, audio_volume_db_to_q16(-30.0f));
    TEST_ASSERT_EQUAL_INT16(632, b30);
    int16_t bm = 20000; audio_volume_apply(&bm, 1, 0 /*mute*/);
    TEST_ASSERT_EQUAL_INT16(0, bm);
}
void test_apply_clamp_and_symmetry(void){
    // full-scale at unity stays in range (never overflows)
    int16_t hi = 32767, lo = -32768;
    audio_volume_apply(&hi, 1, AUDIO_VOL_UNITY); TEST_ASSERT_EQUAL_INT16(32767, hi);
    audio_volume_apply(&lo, 1, AUDIO_VOL_UNITY); TEST_ASSERT_EQUAL_INT16(-32768, lo);
    // negative sample attenuates symmetrically to positive
    int16_t np = 20000, nn = -20000; int32_t q = audio_volume_db_to_q16(-6.0f);
    audio_volume_apply(&np, 1, q); audio_volume_apply(&nn, 1, q);
    TEST_ASSERT_EQUAL_INT16(-np, nn);   // 10024 vs -10024
}
int main(void){ UNITY_BEGIN();
    RUN_TEST(test_db_to_q16_vectors); RUN_TEST(test_db_clamp_out_of_range);
    RUN_TEST(test_apply_unity_is_identity); RUN_TEST(test_apply_gain_vectors);
    RUN_TEST(test_apply_clamp_and_symmetry);
    return UNITY_END(); }
```
> Verify the exact expected int16 values by running the test — floating-point rounding at the `+0.5`
> boundary can shift a unit; if a vector is off by 1, adjust the expected constant to the computed value
> and note it (the research vectors were computed with truncation, ours rounds — expect ±1 on a couple).

---

## Task 2: wire volume into the drain + `audio_set_volume` public API (glue)

**`audio.h`** — declare the public setter:
```c
// Set playback volume from an AirPlay dB value (-144 = mute .. 0 = full). Applied
// as software gain on the int16 PCM in the playback drain (PCM5102A has no gain
// pin). Bypassed at unity (0 dB). Transport-agnostic: RAOP calls this; audio never
// learns what RAOP is. Thread-safe: stores a single int32 read by the drain task.
void audio_set_volume(float db);
```

**`audio.c`** — storage + setter (place near the ring/task globals):
```c
#include "audio_volume.h"
static volatile int32_t s_fix_q16 = AUDIO_VOL_UNITY;   // 0 dB until a sender sets it
int32_t audio_playback_fix_q16(void) { return s_fix_q16; }   // read by the drain
void audio_set_volume(float db) {
    int32_t q = audio_volume_db_to_q16(db);
    s_fix_q16 = q;
    ESP_LOGI(TAG, "volume %.1f dB -> gain q16=%ld%s", db, (long)q,
             q == AUDIO_VOL_UNITY ? " (unity, bypass)" : (q == 0 ? " (mute)" : ""));
}
```
> `s_fix_q16` is a plain `volatile int32_t` written by the RTSP task and read by the drain task. Both are
> pinned to `audio_producer_core()` in this design; a 32-bit aligned load/store is atomic on the LX7, so
> no torn read. (Declare `int32_t audio_playback_fix_q16(void);` in `audio_playback.h` so the drain sees
> it without exposing the storage.)

**`audio_playback.c`** — the drain applies gain per chunk, bypassing at unity (edit the `got > 0` path):
```c
#include "audio_volume.h"
// ...
size_t got = audio_ringbuf_read(ring, chunk, PLAYBACK_CHUNK_FRAMES);
if (got > 0) {
    int32_t fix = audio_playback_fix_q16();
    if (fix != AUDIO_VOL_UNITY) {
        audio_volume_apply(chunk, got * 2, fix);   // got frames * 2 samples/frame
    }
    audio_i2s_write(chunk, got);
} else {
    audio_i2s_write(silence, PLAYBACK_CHUNK_FRAMES);
}
```
> Gain is applied to the **already-drained chunk** (after drift DROP/DUP), so it never interferes with the
> Phase-4 watermark logic. The DUP path plays a single raw frame straight to I2S (`audio_i2s_write(f,1)`);
> for exactness we also gain that frame — apply `audio_volume_apply(f, 2, fix)` there when `fix != unity`.
> (Optional; one inaudible ~23 µs frame at slightly wrong level is harmless, but do it for correctness.)

**`audio/CMakeLists.txt`** — add `"src/audio_volume.c"` to `SRCS`. No new REQUIRES (math is libc/`-lm`;
host tests link `-lm` — add `build_flags` `-lm` if the native link fails, though Unity's harness usually
pulls it).

---

## Task 3: `raop_volume` — pure `volume:`/`progress:` line parser (host-tested)

`components/raop/src/raop_volume.h`
```c
// Pure parsers for RAOP SET_PARAMETER text/parameters bodies. No ESP-IDF deps.
// Bodies are untrusted: bounded, no allocation, tolerant of missing \r\n.
#pragma once
#include <stddef.h>
#include <stdbool.h>

// Scan a text/parameters body for a "volume: <float>\r\n" line. On match writes
// the dB value and returns true. shairport matches prefix "volume: " (8 bytes).
bool raop_parse_volume(const char *body, size_t len, float *out_db);

// Scan for "progress: <u32>/<u32>/<u32>". On match writes the three RTP timestamps
// and returns true. Optional (logged only); free-run receiver ignores the values.
bool raop_parse_progress(const char *body, size_t len,
                         uint32_t *start, uint32_t *cur, uint32_t *end);
```

`components/raop/src/raop_volume.c`
```c
#include "raop_volume.h"
#include <string.h>
#include <stdlib.h>
#include <stdint.h>

// Find a line beginning with `prefix` within [body, body+len); return a pointer
// just past the prefix, or NULL. Lines are \r\n- or \n-separated; the body is NOT
// guaranteed NUL-terminated, so we never call str* past `len`.
static const char *find_line(const char *body, size_t len, const char *prefix) {
    size_t plen = strlen(prefix);
    size_t i = 0;
    while (i < len) {
        size_t j = i;
        while (j < len && body[j] != '\n' && body[j] != '\r') j++;
        size_t line_len = j - i;
        if (line_len >= plen && memcmp(body + i, prefix, plen) == 0) {
            return body + i + plen;
        }
        while (j < len && (body[j] == '\n' || body[j] == '\r')) j++;
        i = j;
    }
    return NULL;
}

bool raop_parse_volume(const char *body, size_t len, float *out_db) {
    if (!body || !out_db) return false;
    const char *p = find_line(body, len, "volume: ");
    if (!p) return false;
    // Copy the remainder of the (bounded) value into a small NUL-terminated scratch
    // so strtof has a terminator; value is short (e.g. "-14.500000").
    char tmp[32]; size_t n = 0;
    for (const char *q = p; q < body + len && *q != '\r' && *q != '\n' && n < sizeof(tmp)-1; q++)
        tmp[n++] = *q;
    tmp[n] = '\0';
    char *endp = NULL;
    float v = strtof(tmp, &endp);
    if (endp == tmp) return false;     // no number
    *out_db = v;
    return true;
}

bool raop_parse_progress(const char *body, size_t len,
                         uint32_t *start, uint32_t *cur, uint32_t *end) {
    if (!body) return false;
    const char *p = find_line(body, len, "progress: ");
    if (!p) return false;
    char tmp[48]; size_t n = 0;
    for (const char *q = p; q < body + len && *q != '\r' && *q != '\n' && n < sizeof(tmp)-1; q++)
        tmp[n++] = *q;
    tmp[n] = '\0';
    unsigned long a=0,b=0,c=0; int got = sscanf(tmp, "%lu/%lu/%lu", &a,&b,&c);
    if (got != 3) return false;
    if (start) *start=(uint32_t)a; if (cur) *cur=(uint32_t)b; if (end) *end=(uint32_t)c;
    return true;
}
```
> `strtof`/`sscanf` on a bounded, locally-NUL-terminated copy avoids reading past `body+len` (the RTSP
> body pointer is NOT NUL-terminated — it points into the recv buffer). Include `<stdio.h>` for `sscanf`.

`test/test_raop_volume/test_raop_volume.c`
```c
#include <unity.h>
#include "raop_volume.c"
void setUp(void){} void tearDown(void){}

void test_volume_basic(void){
    const char *b = "volume: -14.500000\r\n"; float db=99;
    TEST_ASSERT_TRUE(raop_parse_volume(b, strlen(b), &db));
    TEST_ASSERT_FLOAT_WITHIN(0.001f, -14.5f, db);
}
void test_volume_mute_sentinel(void){
    const char *b = "volume: -144.000000\r\n"; float db=0;
    TEST_ASSERT_TRUE(raop_parse_volume(b, strlen(b), &db));
    TEST_ASSERT_FLOAT_WITHIN(0.001f, -144.0f, db);
}
void test_volume_full(void){
    const char *b = "volume: 0.000000\r\n"; float db=99;
    TEST_ASSERT_TRUE(raop_parse_volume(b, strlen(b), &db));
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 0.0f, db);
}
void test_volume_no_crlf(void){                       // tolerate missing terminator
    const char *b = "volume: -6.0"; float db=99;
    TEST_ASSERT_TRUE(raop_parse_volume(b, strlen(b), &db));
    TEST_ASSERT_FLOAT_WITHIN(0.001f, -6.0f, db);
}
void test_volume_absent(void){
    const char *b = "progress: 1/2/3\r\n"; float db=99;
    TEST_ASSERT_FALSE(raop_parse_volume(b, strlen(b), &db));
}
void test_volume_not_bare_prefix(void){               // "volumes:" must not match
    const char *b = "volumes: 5\r\n"; float db=99;
    TEST_ASSERT_FALSE(raop_parse_volume(b, strlen(b), &db));
}
void test_progress_parse(void){
    const char *b = "progress: 3839844000/3839863488/3856224000\r\n";
    uint32_t s=0,c=0,e=0;
    TEST_ASSERT_TRUE(raop_parse_progress(b, strlen(b), &s,&c,&e));
    TEST_ASSERT_EQUAL_UINT32(3839844000u, s);
    TEST_ASSERT_EQUAL_UINT32(3839863488u, c);
    TEST_ASSERT_EQUAL_UINT32(3856224000u, e);
}
void test_progress_absent(void){
    const char *b = "volume: 0.0\r\n"; uint32_t s,c,e;
    TEST_ASSERT_FALSE(raop_parse_progress(b, strlen(b), &s,&c,&e));
}
int main(void){ UNITY_BEGIN();
    RUN_TEST(test_volume_basic); RUN_TEST(test_volume_mute_sentinel);
    RUN_TEST(test_volume_full); RUN_TEST(test_volume_no_crlf);
    RUN_TEST(test_volume_absent); RUN_TEST(test_volume_not_bare_prefix);
    RUN_TEST(test_progress_parse); RUN_TEST(test_progress_absent);
    return UNITY_END(); }
```

---

## Task 4: `dmap` — pure DMAP/DAAP TLV walker (host-tested, incl. malformed)

`components/raop/src/dmap.h`
```c
// Pure DMAP/DAAP TLV parser (spec §5b metadata). Walks an application/x-dmap-tagged
// SET_PARAMETER body — [4 ASCII code][BE32 length][value] tuples — extracting
// minm=title, asar=artist, asal=album as bounded UTF-8 strings. Recurses into the
// mlit listing-item container. Every length is bounded against the body (untrusted,
// spec §9): truncated/overrunning tags stop the walk, no read past the buffer, no
// allocation. No ESP-IDF deps → host-unit-testable.
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#define DMAP_STR_MAX 128    // bounded copy; sender strings are truncated to fit

typedef struct {
    char title [DMAP_STR_MAX];
    char artist[DMAP_STR_MAX];
    char album [DMAP_STR_MAX];
    bool has_title, has_artist, has_album;   // set true when the tag was present
} dmap_meta_t;

// Parse `buf`/`len` into `out` (zeroed by the callee first). Returns the number of
// recognized text tags captured (0..3). Robust to flat OR mlit-wrapped bodies and
// to truncation/overflow (stops cleanly). vlen==0 yields an empty string with the
// has_* flag set (a deliberate field clear).
int dmap_parse(const uint8_t *buf, size_t len, dmap_meta_t *out);
```

`components/raop/src/dmap.c`
```c
#include "dmap.h"
#include <string.h>

#define DMAP_CODE(a,b,c,d) \
  (((uint32_t)(a)<<24)|((uint32_t)(b)<<16)|((uint32_t)(c)<<8)|(uint32_t)(d))
enum { TAG_minm = DMAP_CODE('m','i','n','m'),
       TAG_asar = DMAP_CODE('a','s','a','r'),
       TAG_asal = DMAP_CODE('a','s','a','l'),
       TAG_mlit = DMAP_CODE('m','l','i','t') };

static uint32_t rd_be32(const uint8_t *p) {
    return ((uint32_t)p[0]<<24)|((uint32_t)p[1]<<16)|((uint32_t)p[2]<<8)|p[3];
}
static void copy_str(char *dst, const uint8_t *val, uint32_t vlen) {
    uint32_t n = vlen < (DMAP_STR_MAX-1) ? vlen : (DMAP_STR_MAX-1);
    memcpy(dst, val, n);
    dst[n] = '\0';
}
static int walk(const uint8_t *buf, size_t len, dmap_meta_t *out) {
    int count = 0;
    size_t off = 0;
    while (off + 8 <= len) {                       // need code(4)+len(4)
        uint32_t code = rd_be32(buf + off); off += 4;
        uint32_t vlen = rd_be32(buf + off); off += 4;
        if (vlen > len - off) break;               // overrun guard (no underflow: off<=len)
        const uint8_t *val = buf + off;
        switch (code) {
            case TAG_mlit: count += walk(val, vlen, out); break;      // recurse
            case TAG_minm: copy_str(out->title,  val, vlen); out->has_title=true;  count++; break;
            case TAG_asar: copy_str(out->artist, val, vlen); out->has_artist=true; count++; break;
            case TAG_asal: copy_str(out->album,  val, vlen); out->has_album=true;  count++; break;
            default: break;                        // ignore unknown tag
        }
        off += vlen;
    }
    return count;
}
int dmap_parse(const uint8_t *buf, size_t len, dmap_meta_t *out) {
    memset(out, 0, sizeof(*out));
    if (!buf) return 0;
    return walk(buf, len, out);
}
```

`test/test_dmap/test_dmap.c`
```c
#include <unity.h>
#include "dmap.c"
void setUp(void){} void tearDown(void){}

void test_flat_minm(void){
    const uint8_t b[] = {'m','i','n','m',0,0,0,2,'H','i'};
    dmap_meta_t m; int n = dmap_parse(b, sizeof(b), &m);
    TEST_ASSERT_EQUAL_INT(1, n);
    TEST_ASSERT_TRUE(m.has_title);
    TEST_ASSERT_EQUAL_STRING("Hi", m.title);
}
void test_flat_all_three(void){
    const uint8_t b[] = {
        'm','i','n','m',0,0,0,2,'H','i',
        'a','s','a','r',0,0,0,2,'A','B',
        'a','s','a','l',0,0,0,3,'X','Y','Z'};
    dmap_meta_t m; int n = dmap_parse(b, sizeof(b), &m);
    TEST_ASSERT_EQUAL_INT(3, n);
    TEST_ASSERT_EQUAL_STRING("Hi",  m.title);
    TEST_ASSERT_EQUAL_STRING("AB",  m.artist);
    TEST_ASSERT_EQUAL_STRING("XYZ", m.album);
}
void test_mlit_wrapped(void){
    // mlit container (len=10) holding one minm "Hi"
    const uint8_t b[] = {'m','l','i','t',0,0,0,10,
                         'm','i','n','m',0,0,0,2,'H','i'};
    dmap_meta_t m; int n = dmap_parse(b, sizeof(b), &m);
    TEST_ASSERT_EQUAL_INT(1, n);
    TEST_ASSERT_EQUAL_STRING("Hi", m.title);
}
void test_unknown_tags_skipped(void){
    const uint8_t b[] = {'a','s','g','n',0,0,0,3,'p','o','p',   // genre: ignored
                         'm','i','n','m',0,0,0,1,'Q'};
    dmap_meta_t m; int n = dmap_parse(b, sizeof(b), &m);
    TEST_ASSERT_EQUAL_INT(1, n);
    TEST_ASSERT_EQUAL_STRING("Q", m.title);
}
void test_truncated_header(void){          // only 6 bytes: can't read code+len
    const uint8_t b[] = {'m','i','n','m',0,0};
    dmap_meta_t m; int n = dmap_parse(b, sizeof(b), &m);
    TEST_ASSERT_EQUAL_INT(0, n);
    TEST_ASSERT_FALSE(m.has_title);
}
void test_length_overruns_body(void){      // claims len=200 but body short
    const uint8_t b[] = {'m','i','n','m',0,0,0,200,'H','i'};
    dmap_meta_t m; int n = dmap_parse(b, sizeof(b), &m);
    TEST_ASSERT_EQUAL_INT(0, n);           // overrun guard fires, no read past buf
    TEST_ASSERT_FALSE(m.has_title);
}
void test_empty_value_clears(void){        // vlen==0 = field cleared
    const uint8_t b[] = {'m','i','n','m',0,0,0,0};
    dmap_meta_t m; int n = dmap_parse(b, sizeof(b), &m);
    TEST_ASSERT_EQUAL_INT(1, n);
    TEST_ASSERT_TRUE(m.has_title);
    TEST_ASSERT_EQUAL_STRING("", m.title);
}
void test_long_value_bounded(void){        // > DMAP_STR_MAX truncates, no overflow
    uint8_t b[8 + 300]; memcpy(b, (const uint8_t[]){'m','i','n','m',0,0,1,44}, 8);
    for (int i=0;i<300;i++) b[8+i]='A';
    dmap_meta_t m; int n = dmap_parse(b, sizeof(b), &m);
    TEST_ASSERT_EQUAL_INT(1, n);
    TEST_ASSERT_EQUAL_INT(DMAP_STR_MAX-1, (int)strlen(m.title));  // bounded + NUL
}
void test_empty_body(void){
    dmap_meta_t m; TEST_ASSERT_EQUAL_INT(0, dmap_parse((const uint8_t*)"", 0, &m));
}
int main(void){ UNITY_BEGIN();
    RUN_TEST(test_flat_minm); RUN_TEST(test_flat_all_three); RUN_TEST(test_mlit_wrapped);
    RUN_TEST(test_unknown_tags_skipped); RUN_TEST(test_truncated_header);
    RUN_TEST(test_length_overruns_body); RUN_TEST(test_empty_value_clears);
    RUN_TEST(test_long_value_bounded); RUN_TEST(test_empty_body);
    return UNITY_END(); }
```
> The `0,0,1,44` length in `test_long_value_bounded` is BE32 `0x0000012C = 300`. Confirm the constant
> when writing (300 = `0x012C` → bytes `00 00 01 2C`).

---

## Task 5: `setparam` — pure Content-Type dispatch decision (host-tested)

`components/raop/src/setparam.h`
```c
// Pure decision: given a SET_PARAMETER Content-Type header value, decide how the
// body is handled. Keeps the dispatch policy out of the socket glue and unit-tests
// it directly. Matching is case-insensitive on the media type; parameters after a
// ';' (e.g. "; charset=...") are ignored.
#pragma once

typedef enum {
    SETPARAM_VOLUME = 0,   // text/parameters       -> volume:/progress: line parse
    SETPARAM_METADATA,     // application/x-dmap-tagged -> DMAP TLV parse
    SETPARAM_OTHER,        // image/jpeg, image/png, unknown, or absent -> ignore
} setparam_kind_t;

// content_type may be NULL (absent header) -> SETPARAM_OTHER.
setparam_kind_t setparam_classify(const char *content_type);
```

`components/raop/src/setparam.c`
```c
#include "setparam.h"
#include <string.h>
#include <ctype.h>

// Case-insensitive compare of the media type up to ';' or whitespace.
static int media_is(const char *ct, const char *want) {
    while (*want) {
        if (!*ct) return 0;
        if (tolower((unsigned char)*ct) != tolower((unsigned char)*want)) return 0;
        ct++; want++;
    }
    // matched `want`; remainder of ct must be end / ws / ';'
    return (*ct == '\0' || *ct == ';' || *ct == ' ' || *ct == '\t' ||
            *ct == '\r' || *ct == '\n');
}
setparam_kind_t setparam_classify(const char *content_type) {
    if (!content_type) return SETPARAM_OTHER;
    while (*content_type == ' ' || *content_type == '\t') content_type++;
    if (media_is(content_type, "text/parameters"))          return SETPARAM_VOLUME;
    if (media_is(content_type, "application/x-dmap-tagged")) return SETPARAM_METADATA;
    return SETPARAM_OTHER;
}
```

`test/test_setparam/test_setparam.c`
```c
#include <unity.h>
#include "setparam.c"
void setUp(void){} void tearDown(void){}

void test_text_parameters(void){
    TEST_ASSERT_EQUAL_INT(SETPARAM_VOLUME, setparam_classify("text/parameters"));
}
void test_dmap(void){
    TEST_ASSERT_EQUAL_INT(SETPARAM_METADATA, setparam_classify("application/x-dmap-tagged"));
}
void test_case_insensitive(void){
    TEST_ASSERT_EQUAL_INT(SETPARAM_VOLUME, setparam_classify("Text/Parameters"));
}
void test_trailing_params_ignored(void){
    TEST_ASSERT_EQUAL_INT(SETPARAM_VOLUME, setparam_classify("text/parameters; charset=utf-8"));
}
void test_leading_ws(void){
    TEST_ASSERT_EQUAL_INT(SETPARAM_METADATA, setparam_classify("  application/x-dmap-tagged"));
}
void test_image_is_other(void){
    TEST_ASSERT_EQUAL_INT(SETPARAM_OTHER, setparam_classify("image/jpeg"));
}
void test_null_is_other(void){
    TEST_ASSERT_EQUAL_INT(SETPARAM_OTHER, setparam_classify(NULL));
}
void test_prefix_not_matched(void){        // "text/parametersX" must not match
    TEST_ASSERT_EQUAL_INT(SETPARAM_OTHER, setparam_classify("text/parametersX"));
}
int main(void){ UNITY_BEGIN();
    RUN_TEST(test_text_parameters); RUN_TEST(test_dmap); RUN_TEST(test_case_insensitive);
    RUN_TEST(test_trailing_params_ignored); RUN_TEST(test_leading_ws);
    RUN_TEST(test_image_is_other); RUN_TEST(test_null_is_other);
    RUN_TEST(test_prefix_not_matched);
    return UNITY_END(); }
```

---

## Task 6: `raop_metadata` store + `raop.c` SET_PARAMETER wiring (glue)

`components/raop/src/raop_metadata.h`
```c
// Current-track metadata store (spec §5b: "log it (display later)"). Holds the last
// title/artist/album and logs a single line WHEN A FIELD CHANGES (senders re-send
// metadata redundantly; we don't spam the log). Lives in raop so the audio core
// stays transport-agnostic. Single writer (the RTSP task); not reentrant.
#pragma once
#include "dmap.h"

// Merge parsed metadata into the store; log "now playing" if anything changed.
void raop_metadata_update(const dmap_meta_t *m);

// Reset to empty (TEARDOWN).
void raop_metadata_clear(void);
```

`components/raop/src/raop_metadata.c` (glue — uses `ESP_LOG`, holds static strings, compares for change):
```c
#include "raop_metadata.h"
#include "esp_log.h"
#include <string.h>
static const char *TAG = "raop-meta";
static dmap_meta_t s_cur;   // zeroed at load

static bool upd(char *dst, bool has, const char *src) {   // returns changed
    if (!has) return false;
    if (strcmp(dst, src) == 0) return false;
    strncpy(dst, src, DMAP_STR_MAX-1); dst[DMAP_STR_MAX-1]='\0';
    return true;
}
void raop_metadata_update(const dmap_meta_t *m) {
    bool ch = false;
    ch |= upd(s_cur.title,  m->has_title,  m->title);
    ch |= upd(s_cur.artist, m->has_artist, m->artist);
    ch |= upd(s_cur.album,  m->has_album,  m->album);
    if (ch) ESP_LOGI(TAG, "now playing: \"%s\" — %s — %s",
                     s_cur.title, s_cur.artist, s_cur.album);
}
void raop_metadata_clear(void) { memset(&s_cur, 0, sizeof(s_cur)); }
```

**`raop.c` — replace the shared `SET_PARAMETER` ack** with a real handler. Split the current combined
`FLUSH/PAUSE/SET_PARAMETER/GET_PARAMETER` branch so `SET_PARAMETER` runs the dispatch:
```c
#include "setparam.h"
#include "raop_volume.h"
#include "dmap.h"
#include "raop_metadata.h"
#include "audio.h"          // audio_set_volume (already included for diag handoff)

static void handle_set_parameter(int fd, const rtsp_request_t *req, int cseq) {
    const char *ct = rtsp_header_get(req, "Content-Type");
    switch (setparam_classify(ct)) {
        case SETPARAM_VOLUME: {
            float db;
            if (req->body && raop_parse_volume(req->body, req->body_len, &db)) {
                audio_set_volume(db);            // software gain in the drain
            }
            uint32_t s,c,e;
            if (req->body && raop_parse_progress(req->body, req->body_len, &s,&c,&e)) {
                ESP_LOGI(TAG, "progress %us / %us", (c-s)/44100, (e-s)/44100);
            }
            break;
        }
        case SETPARAM_METADATA: {
            if (req->body && req->body_len) {
                dmap_meta_t m;
                dmap_parse((const uint8_t*)req->body, req->body_len, &m);
                raop_metadata_update(&m);
            }
            break;
        }
        case SETPARAM_OTHER:
        default: break;                          // artwork/unknown: ignore (Phase 5 scope)
    }
    send_response(fd, 200, "OK", cseq, NULL, NULL, 0);
}
```
Then in `dispatch()`:
```c
} else if (strcmp(req->method, "SET_PARAMETER") == 0) {
    handle_set_parameter(fd, req, cseq);
} else if (strcmp(req->method, "FLUSH") == 0 ||
           strcmp(req->method, "PAUSE") == 0 ||
           strcmp(req->method, "GET_PARAMETER") == 0) {
    send_response(fd, 200, "OK", cseq, NULL, NULL, 0);   // unchanged: ack only
}
```
And `session_teardown_full()` also calls `raop_metadata_clear()`.

**`raop/CMakeLists.txt`** — add `"src/raop_volume.c" "src/dmap.c" "src/setparam.c" "src/raop_metadata.c"`
to `SRCS`. `audio` is already in `PRIV_REQUIRES` (Phase 3), so `audio_set_volume` links with no REQUIRES
change → **no `.pio` wipe needed for this component** (only the `system` `idf_component.yml` add forces
the wipe).

---

## Task 7: `led_state` (pure) + `system_led` (guarded glue) + `idf_component.yml`

**Pure colour map** — `components/system/src/led_state.h`:
```c
// Pure map from status-LED state to an (r,g,b) triple (spec §7). No ESP-IDF deps →
// host-unit-testable. Brightness kept low (<=16) — status indicator, not lighting.
#pragma once
#include <stdint.h>

typedef enum {
    LED_ST_NEEDS_CREDS = 0,   // boot, no Wi-Fi credentials
    LED_ST_WIFI_CONNECTING,   // station connecting
    LED_ST_CONNECTED_IDLE,    // Wi-Fi up, no live stream
    LED_ST_STREAMING,         // RAOP RECORD live
} system_led_state_t;

typedef struct { uint8_t r, g, b; } led_rgb_t;

// Total, deterministic; an out-of-range state maps to off (0,0,0).
led_rgb_t led_state_color(system_led_state_t st);
```
`components/system/src/led_state.c`:
```c
#include "led_state.h"
led_rgb_t led_state_color(system_led_state_t st) {
    switch (st) {
        case LED_ST_NEEDS_CREDS:      return (led_rgb_t){16, 0, 0};   // red
        case LED_ST_WIFI_CONNECTING:  return (led_rgb_t){16, 6, 0};   // amber
        case LED_ST_CONNECTED_IDLE:   return (led_rgb_t){0, 0, 16};   // blue
        case LED_ST_STREAMING:        return (led_rgb_t){0, 16, 0};   // green
        default:                      return (led_rgb_t){0, 0, 0};    // off
    }
}
```
`test/test_led_state/test_led_state.c`:
```c
#include <unity.h>
#include "led_state.c"
void setUp(void){} void tearDown(void){}
void test_colors_distinct(void){
    led_rgb_t nc = led_state_color(LED_ST_NEEDS_CREDS);
    led_rgb_t wc = led_state_color(LED_ST_WIFI_CONNECTING);
    led_rgb_t ci = led_state_color(LED_ST_CONNECTED_IDLE);
    led_rgb_t st = led_state_color(LED_ST_STREAMING);
    TEST_ASSERT_EQUAL_UINT8(16, nc.r); TEST_ASSERT_EQUAL_UINT8(0, nc.g);
    TEST_ASSERT_EQUAL_UINT8(16, ci.b); TEST_ASSERT_EQUAL_UINT8(0, ci.r);
    TEST_ASSERT_EQUAL_UINT8(16, st.g); TEST_ASSERT_EQUAL_UINT8(0, st.b);
    TEST_ASSERT_EQUAL_UINT8(16, wc.r); TEST_ASSERT_EQUAL_UINT8(6, wc.g);
    // all four differ pairwise
    TEST_ASSERT_TRUE(memcmp(&nc,&wc,sizeof nc) && memcmp(&nc,&ci,sizeof nc) &&
                     memcmp(&nc,&st,sizeof nc) && memcmp(&wc,&ci,sizeof nc) &&
                     memcmp(&wc,&st,sizeof nc) && memcmp(&ci,&st,sizeof nc));
}
void test_out_of_range_off(void){
    led_rgb_t o = led_state_color((system_led_state_t)99);
    TEST_ASSERT_EQUAL_UINT8(0, o.r); TEST_ASSERT_EQUAL_UINT8(0, o.g); TEST_ASSERT_EQUAL_UINT8(0, o.b);
}
int main(void){ UNITY_BEGIN();
    RUN_TEST(test_colors_distinct); RUN_TEST(test_out_of_range_off);
    return UNITY_END(); }
```
> Add `#include <string.h>` for `memcmp` in the test.

**Guarded glue** — `components/system/include/system_led.h` (public):
```c
#pragma once
#include "led_state.h"     // re-export system_led_state_t
// Init the onboard WS2812 (GPIO48). Guarded: if the LED/RMT is unavailable the
// handle stays NULL and every set becomes a no-op (never crashes). Idempotent.
void system_led_init(void);
// Set the current status state (thread-safe enough for low-rate callers).
void system_led_set_state(system_led_state_t st);
```
`components/system/src/system_led.c`:
```c
#include "system_led.h"
#include "led_strip.h"
#include "esp_log.h"
#define STATUS_LED_GPIO 48   // DevKitC-1 onboard WS2812 (a few revisions: 38)
static const char *TAG = "led";
static led_strip_handle_t s_led = NULL;

void system_led_init(void) {
    if (s_led) return;
    led_strip_config_t sc = {
        .strip_gpio_num = STATUS_LED_GPIO, .max_leds = 1,
        .led_model = LED_MODEL_WS2812,
        .color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_GRB,
        .flags = { .invert_out = false },
    };
    led_strip_rmt_config_t rc = {
        .clk_src = RMT_CLK_SRC_DEFAULT, .resolution_hz = 10*1000*1000,
        .mem_block_symbols = 64, .flags = { .with_dma = false },
    };
    esp_err_t e = led_strip_new_rmt_device(&sc, &rc, &s_led);
    if (e != ESP_OK) { s_led = NULL; ESP_LOGW(TAG, "LED init failed (%s); status LED disabled",
                                              esp_err_to_name(e)); return; }
    led_strip_clear(s_led);
}
void system_led_set_state(system_led_state_t st) {
    if (!s_led) return;                       // guarded no-op
    led_rgb_t c = led_state_color(st);
    led_strip_set_pixel(s_led, 0, c.r, c.g, c.b);
    led_strip_refresh(s_led);
}
```
`components/system/idf_component.yml` (**CREATE** — triggers the `.pio` wipe):
```yaml
dependencies:
  espressif/led_strip: "^3.0.3"
  idf: ">=5.0"
```
`components/system/CMakeLists.txt` — add sources + REQUIRES:
```cmake
idf_component_register(
    SRCS "src/system_config.c"
         "src/device_id.c"
         "src/led_state.c"
         "src/system_led.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "src"
    PRIV_REQUIRES nvs_flash esp_hw_support driver esp_driver_rmt led_strip)
```
> `led_strip.h` needs its include dir; the managed component provides it and CMake resolves it via the
> `led_strip` REQUIRES. `esp_driver_rmt` is the split RMT driver on IDF 6.0. **After creating
> `idf_component.yml`: `rm -rf framework/.pio/build/esp32-s3-n16r8` then rebuild** (managed component
> downloads on first build).

---

## Task 8: state-transition wiring — `main.cpp` + raop event callback

**raop event callback** — so `raop.c` (which owns RECORD/TEARDOWN) can drive the LED without depending on
`system`. Add to `include/raop.h`:
```c
typedef enum { RAOP_EV_STREAMING, RAOP_EV_IDLE } raop_event_t;
typedef void (*raop_event_cb_t)(raop_event_t ev);
// Register a callback fired on RECORD (STREAMING) and TEARDOWN/dead-peer (IDLE).
// main wires this to system_led_set_state. NULL clears it. Call before start.
void raop_set_event_cb(raop_event_cb_t cb);
```
In `raop.c`: store `static raop_event_cb_t s_ev_cb;`; on successful RECORD start fire
`if (s_ev_cb) s_ev_cb(RAOP_EV_STREAMING);`; in `session_teardown_full()` (covers TEARDOWN, dead-peer
reclaim, and RTP-start failure) fire `if (s_ev_cb) s_ev_cb(RAOP_EV_IDLE);`.

**`main.cpp`** transitions:
```c
#include "system_led.h"
// ... in app_main, after system_config_init():
system_led_init();

static void on_raop_event(raop_event_t ev) {
    system_led_set_state(ev == RAOP_EV_STREAMING ? LED_ST_STREAMING : LED_ST_CONNECTED_IDLE);
}
// on_got_ip():
system_led_set_state(LED_ST_CONNECTED_IDLE);   // Wi-Fi up, no stream yet
mdns_advertise_raop(...);
raop_set_event_cb(on_raop_event);              // (register once; safe before start)
raop_server_start();

// credentials branch:
if (system_config_has_credentials()) {
    system_led_set_state(LED_ST_WIFI_CONNECTING);
    wifi_start(on_got_ip);
} else {
    system_led_set_state(LED_ST_NEEDS_CREDS);
    ESP_LOGW(TAG, "no Wi-Fi credentials: ... Idling; audio path still runs.");
}
```
> `system_led_init()` runs on the boot task; `system_led_set_state` is called from the boot task
> (NEEDS_CREDS/CONNECTING), the Wi-Fi event task (via `on_got_ip` → IDLE), and the RTSP task (via
> `on_raop_event`). `led_strip_refresh` blocks ~30 µs on RMT — fine off any of these low-rate tasks,
> never from the audio drain/ISR. Concurrent callers are serialized only loosely; a double-refresh is
> harmless (last write wins). If stricter serialization is wanted, guard with a mutex — deferred (KISS).

---

## Task 9: platformio native env + READMEs + build/test verify

- **`platformio.ini` `[env:native]`:** `-I components/audio/src`, `-I components/system/src`,
  `-I components/raop/src` are already present → the five new pure `.c` files are reachable by their
  `test_*` includes with **no change**. Confirm; add only if a dir is missing. (`audio_volume.c` uses
  `<math.h>` → if the native link errors on `powf`, add `-lm` to `[env:native] build_flags`.)
- **READMEs:** append Phase 5 notes to `components/audio/README.md` (volume gain + `audio_set_volume`),
  `components/raop/README.md` (SET_PARAMETER dispatch, volume/DMAP/progress, metadata store), and
  `components/system/README.md` (status LED states + GPIO48/led_strip guard).
- **Verify (evidence before claims):**
  1. `cd framework && ~/.platformio/penv/bin/pio test -e native` → **80/80** (75 existing + 5 new suites;
     count the new test *cases*, not suites, when reporting).
  2. `rm -rf framework/.pio/build/esp32-s3-n16r8` (led_strip managed-component download).
  3. `cd framework && ~/.platformio/penv/bin/pio run -e esp32-s3-n16r8` → **green**.
  4. Sanity-read the boot log for the LED init line and, if a sender is available, the `now playing` +
     `volume … dB` lines. (No real sender in this environment — interop honesty below.)

---

## Success criteria

- [ ] Five new pure units (`audio_volume`, `raop_volume`, `dmap`, `setparam`, `led_state`) with Unity host
      tests; existing 75 stay green; total host tests pass.
- [ ] `audio_set_volume(float db)` on the public audio API; drain applies Q16 gain per chunk, **bypasses
      at unity**, clamps to int16; mute (`-144`) and 0 dB behave per the vectors.
- [ ] `SET_PARAMETER` dispatches on `Content-Type`: `text/parameters` → volume (+progress log);
      `application/x-dmap-tagged` → DMAP parse → metadata store logs **on change**; other → ignored. All
      lengths bounded (untrusted input).
- [ ] WS2812 status LED on GPIO48 via `espressif/led_strip`, four distinct states, wired transitions
      (NEEDS_CREDS / WIFI_CONNECTING / CONNECTED_IDLE / STREAMING); **guarded** so a missing LED never
      crashes.
- [ ] Firmware builds green `-e esp32-s3-n16r8` after the `.pio` wipe; Phases 0–4 intact (first-audio
      pipeline, reorder/retransmit/timing, diag-tone handoff, drift safeguard).
- [ ] Conventional-commits history; commit ends `Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>`.

## Risks / interop honesty (no real sender here)

- **No live AirPlay sender** in this environment: volume/metadata/LED transitions are exercised by host
  unit tests + boot-log inspection, not an end-to-end iOS stream. The wire formats are matched byte-for-
  byte to shairport-sync/philippe44 (cited), but real-sender confirmation is deferred to hardware.
- **Volume curve (G12):** plain `pow(10, dB/20)`, not shairport's perceptual `vol2attn`. Loudness *feel*
  at a given slider position may differ from an Apple device; documented, revisit if it feels wrong.
- **`mlit` wrapping (G11):** sender-dependent; the recursive walker handles flat and wrapped identically,
  so the recursion branch may be dead against some senders — covered by a unit test either way.
- **LED GPIO (G13):** hard-coded 48; a DevKitC-1 revision on GPIO38 shows a dark (guarded, non-crashing)
  LED. Change the `#define` if the board differs.
- **`s_fix_q16` cross-task read:** relies on 32-bit aligned atomicity on the LX7 (both tasks pinned to
  `audio_producer_core()`); a torn read is structurally impossible for an aligned int32. No lock added.
```
