# AirPlay 1 — Phase 0: Audio Refactor — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Lift the working v0.0.1 I2S + sine code out of `src/main.cpp` into a transport-agnostic `audio` component behind an `audio_play_pcm()` interface, backed by a PSRAM ring buffer and a playback task, with the 440 Hz sine retained as a diagnostic mode fed through the new interface.

**Architecture:** A single-producer/single-consumer PCM ring buffer (pure C, host-unit-tested) decouples producers (later: the RAOP decoder; now: a diagnostic tone) from a playback task that drains the ring into the I2S wrapper, writing silence on underrun. `main.cpp` shrinks to boot banner + `audio_init()` + `audio_diag_tone_start()`.

**Tech Stack:** ESP-IDF 6.0.1, PlatformIO, C11 for the `audio` component, Unity (PlatformIO `native` env) for host unit tests, `driver`/`esp_driver_i2s` for I2S, `esp_psram` for the buffer.

---

## File Structure

```
framework/
  platformio.ini                        # MODIFY: add [env:native] host-test env
  src/main.cpp                          # MODIFY: strip inline i2s/tone; call audio_*
  components/audio/
    CMakeLists.txt                      # MODIFY: register the new sources
    include/audio.h                     # CREATE: public API (audio_init/play_pcm/diag)
    src/audio_ringbuf.h                 # CREATE: pure-C SPSC ring buffer (private)
    src/audio_ringbuf.c                 # CREATE: ring buffer impl (host-testable)
    src/audio_i2s.h                     # CREATE: I2S wrapper interface (private)
    src/audio_i2s.c                     # CREATE: I2S wrapper (lifted from main.cpp)
    src/audio_playback.c                # CREATE: playback task (ring -> i2s, silence)
    src/audio.c                         # CREATE: audio_init/audio_play_pcm glue
    src/audio_diag.c                    # CREATE: 440 Hz diagnostic tone producer
  test/test_ringbuf/test_ringbuf.c      # CREATE: Unity host tests for the ring buffer
```

**Responsibilities:**
- `audio_ringbuf.*` — pure C, zero ESP-IDF deps, so it compiles and runs on the host. Owns FIFO/wrap/underrun logic. The only unit with real branching logic in Phase 0, so it gets the TDD focus.
- `audio_i2s.*` — thin wrapper over the ESP-IDF I2S std driver; identical config to v0.0.1. Not host-testable (hardware); verified by the on-target smoke test.
- `audio_playback.c` — one FreeRTOS task: read frames, write to I2S, write silence when empty.
- `audio.c` — allocates the PSRAM ring storage, wires ring↔task, exposes `audio_init`/`audio_play_pcm`.
- `audio_diag.c` — regenerates the v0.0.1 sine but pushes it through `audio_play_pcm`, proving the new path end-to-end.

---

## Task 1: Ring buffer (host-unit-tested, pure C)

**Files:**
- Create: `framework/components/audio/src/audio_ringbuf.h`
- Create: `framework/components/audio/src/audio_ringbuf.c`
- Create: `framework/test/test_ringbuf/test_ringbuf.c`
- Modify: `framework/platformio.ini` (add `[env:native]`)

- [ ] **Step 1: Add the host-test env to `platformio.ini`**

Append this block to `framework/platformio.ini` (after the existing `[env:esp32-s3-n16r8]` block):

```ini
; --- Host unit tests (pure-C logic only; no ESP-IDF). Run: pio test -e native ---
[env:native]
platform = native
; Exclude the ESP-IDF app sources; native tests compile only test/ + the pure unit
; they #include directly. Keeps host builds free of ESP-IDF headers.
build_src_filter = -<*>
build_flags = -I components/audio/src -std=gnu11
```

- [ ] **Step 2: Write the ring buffer header**

Create `framework/components/audio/src/audio_ringbuf.h`:

```c
// Single-producer / single-consumer PCM ring buffer. Pure C, no ESP-IDF deps,
// so it is unit-testable on the host. Storage is caller-owned (target: PSRAM;
// host test: a static array), which keeps this file allocator-free.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// One frame = one stereo sample pair (L,R) = 2 * int16_t.
typedef struct {
    int16_t *storage;      // caller-owned, capacity_frames * 2 int16_t
    size_t   capacity;     // usable frames (one slot is reserved to disambiguate full/empty)
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
```

- [ ] **Step 3: Write the failing host tests**

Create `framework/test/test_ringbuf/test_ringbuf.c`:

```c
#include <unity.h>
#include <string.h>
#include "audio_ringbuf.c"  // compile the pure unit directly into the test TU

static audio_ringbuf_t rb;
static int16_t storage[8 * 2];  // 8-frame capacity (7 usable)

void setUp(void)    { audio_ringbuf_init(&rb, storage, 8); }
void tearDown(void) {}

static void fill_frames(int16_t *buf, size_t n, int16_t base) {
    for (size_t i = 0; i < n; i++) { buf[2*i] = base + (int16_t)i; buf[2*i+1] = -(base + (int16_t)i); }
}

void test_starts_empty(void) {
    TEST_ASSERT_EQUAL_UINT(0, audio_ringbuf_available(&rb));
    TEST_ASSERT_EQUAL_UINT(7, audio_ringbuf_free_space(&rb));
}

void test_write_then_read_roundtrip(void) {
    int16_t in[3*2], out[3*2];
    fill_frames(in, 3, 100);
    TEST_ASSERT_EQUAL_UINT(3, audio_ringbuf_write(&rb, in, 3));
    TEST_ASSERT_EQUAL_UINT(3, audio_ringbuf_available(&rb));
    TEST_ASSERT_EQUAL_UINT(3, audio_ringbuf_read(&rb, out, 3));
    TEST_ASSERT_EQUAL_INT16_ARRAY(in, out, 3*2);
    TEST_ASSERT_EQUAL_UINT(0, audio_ringbuf_available(&rb));
}

void test_write_saturates_at_capacity(void) {
    int16_t in[10*2];
    fill_frames(in, 10, 1);
    // 7 usable slots -> only 7 accepted.
    TEST_ASSERT_EQUAL_UINT(7, audio_ringbuf_write(&rb, in, 10));
    TEST_ASSERT_EQUAL_UINT(0, audio_ringbuf_free_space(&rb));
}

void test_read_underrun_returns_partial(void) {
    int16_t in[2*2], out[5*2];
    fill_frames(in, 2, 50);
    audio_ringbuf_write(&rb, in, 2);
    TEST_ASSERT_EQUAL_UINT(2, audio_ringbuf_read(&rb, out, 5));  // only 2 available
    TEST_ASSERT_EQUAL_UINT(0, audio_ringbuf_read(&rb, out, 5));  // now empty
}

void test_wraparound_preserves_order(void) {
    int16_t in[5*2], out[5*2];
    // Advance head/tail near the end, then wrap.
    fill_frames(in, 5, 10);
    audio_ringbuf_write(&rb, in, 5);
    audio_ringbuf_read(&rb, out, 5);          // head=tail=5
    fill_frames(in, 5, 200);
    TEST_ASSERT_EQUAL_UINT(5, audio_ringbuf_write(&rb, in, 5));  // wraps past index 8
    TEST_ASSERT_EQUAL_UINT(5, audio_ringbuf_read(&rb, out, 5));
    TEST_ASSERT_EQUAL_INT16_ARRAY(in, out, 5*2);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_starts_empty);
    RUN_TEST(test_write_then_read_roundtrip);
    RUN_TEST(test_write_saturates_at_capacity);
    RUN_TEST(test_read_underrun_returns_partial);
    RUN_TEST(test_wraparound_preserves_order);
    return UNITY_END();
}
```

- [ ] **Step 4: Run the tests to verify they fail (no impl yet)**

Run: `cd framework && ~/.platformio/penv/bin/pio test -e native`
Expected: FAIL to compile/link — `audio_ringbuf.c` does not exist yet (`fatal error: audio_ringbuf.c: No such file`).

- [ ] **Step 5: Write the ring buffer implementation**

Create `framework/components/audio/src/audio_ringbuf.c`:

```c
#include "audio_ringbuf.h"
#include <string.h>

void audio_ringbuf_init(audio_ringbuf_t *rb, int16_t *storage, size_t capacity_frames) {
    rb->storage  = storage;
    rb->capacity = capacity_frames;
    rb->head     = 0;
    rb->tail     = 0;
}

size_t audio_ringbuf_available(const audio_ringbuf_t *rb) {
    return (rb->head - rb->tail + rb->capacity) % rb->capacity;
}

size_t audio_ringbuf_free_space(const audio_ringbuf_t *rb) {
    // One slot reserved so head==tail always means "empty".
    return rb->capacity - 1 - audio_ringbuf_available(rb);
}

size_t audio_ringbuf_write(audio_ringbuf_t *rb, const int16_t *frames, size_t n_frames) {
    size_t space = audio_ringbuf_free_space(rb);
    if (n_frames > space) n_frames = space;
    for (size_t i = 0; i < n_frames; i++) {
        rb->storage[rb->head * 2]     = frames[i * 2];
        rb->storage[rb->head * 2 + 1] = frames[i * 2 + 1];
        rb->head = (rb->head + 1) % rb->capacity;
    }
    return n_frames;
}

size_t audio_ringbuf_read(audio_ringbuf_t *rb, int16_t *out, size_t n_frames) {
    size_t avail = audio_ringbuf_available(rb);
    if (n_frames > avail) n_frames = avail;
    for (size_t i = 0; i < n_frames; i++) {
        out[i * 2]     = rb->storage[rb->tail * 2];
        out[i * 2 + 1] = rb->storage[rb->tail * 2 + 1];
        rb->tail = (rb->tail + 1) % rb->capacity;
    }
    return n_frames;
}
```

- [ ] **Step 6: Run the tests to verify they pass**

Run: `cd framework && ~/.platformio/penv/bin/pio test -e native`
Expected: PASS — `5 Tests 0 Failures 0 Ignored`.

- [ ] **Step 7: Commit**

```bash
cd /Users/uziiuzair/ooozzy/conduit-stream
git add framework/platformio.ini framework/components/audio/src/audio_ringbuf.h \
        framework/components/audio/src/audio_ringbuf.c framework/test/test_ringbuf/test_ringbuf.c
git commit -m "feat(audio): SPSC PCM ring buffer with host unit tests"
```

---

## Task 2: I2S wrapper (lifted from main.cpp)

**Files:**
- Create: `framework/components/audio/src/audio_i2s.h`
- Create: `framework/components/audio/src/audio_i2s.c`

No host test — this is the hardware driver, verified by the Task 6 smoke test. It is a
verbatim relocation of proven v0.0.1 code.

- [ ] **Step 1: Write the I2S wrapper header**

Create `framework/components/audio/src/audio_i2s.h`:

```c
#pragma once
#include <stddef.h>
#include <stdint.h>

#define AUDIO_SAMPLE_RATE_HZ 44100

#ifdef __cplusplus
extern "C" {
#endif

// Bring up the standard-mode I2S TX channel (same config as v0.0.1).
void   audio_i2s_init(void);

// Blocking write of n_frames interleaved stereo frames. Returns frames written.
size_t audio_i2s_write(const int16_t *frames, size_t n_frames);

#ifdef __cplusplus
}
#endif
```

- [ ] **Step 2: Write the I2S wrapper implementation**

Create `framework/components/audio/src/audio_i2s.c` (pins/config identical to the working `main.cpp`):

```c
#include "audio_i2s.h"

#include "driver/i2s_std.h"
#include "freertos/FreeRTOS.h"
#include "esp_log.h"

#define I2S_DOUT_GPIO GPIO_NUM_5   // -> PCM5102A DIN
#define I2S_BCLK_GPIO GPIO_NUM_6   // -> PCM5102A BCK
#define I2S_LRCK_GPIO GPIO_NUM_7   // -> PCM5102A LRCK

static const char *TAG = "audio_i2s";
static i2s_chan_handle_t s_tx = NULL;

void audio_i2s_init(void) {
    i2s_chan_config_t chan_cfg =
        I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_AUTO, I2S_ROLE_MASTER);
    ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, &s_tx, NULL));

    i2s_std_config_t std_cfg = {
        .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(AUDIO_SAMPLE_RATE_HZ),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(
                        I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,   // no MCLK -> PCM5102A uses SCK->GND PLL
            .bclk = I2S_BCLK_GPIO,
            .ws   = I2S_LRCK_GPIO,
            .dout = I2S_DOUT_GPIO,
            .din  = I2S_GPIO_UNUSED,
            .invert_flags = { .mclk_inv = false, .bclk_inv = false, .ws_inv = false },
        },
    };
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(s_tx, &std_cfg));
    ESP_ERROR_CHECK(i2s_channel_enable(s_tx));

    ESP_LOGI(TAG, "i2s: std TX up @ %d Hz, 16-bit stereo "
                  "(BCLK=%d, LRCK=%d, DOUT=%d, MCLK=unused)",
             AUDIO_SAMPLE_RATE_HZ, I2S_BCLK_GPIO, I2S_LRCK_GPIO, I2S_DOUT_GPIO);
}

size_t audio_i2s_write(const int16_t *frames, size_t n_frames) {
    size_t bytes = 0;
    esp_err_t err = i2s_channel_write(s_tx, frames,
                                      n_frames * 2 * sizeof(int16_t),
                                      &bytes, portMAX_DELAY);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2s write failed: %s", esp_err_to_name(err));
        return 0;
    }
    return bytes / (2 * sizeof(int16_t));
}
```

- [ ] **Step 3: Commit** (builds are verified in Task 5 once the component is registered)

```bash
cd /Users/uziiuzair/ooozzy/conduit-stream
git add framework/components/audio/src/audio_i2s.h framework/components/audio/src/audio_i2s.c
git commit -m "feat(audio): I2S std TX wrapper lifted from main.cpp"
```

---

## Task 3: Playback task + public API + component wiring

**Files:**
- Modify: `framework/components/audio/src/audio_i2s.h` (add `AUDIO_PIN_CORE`)
- Create: `framework/components/audio/include/audio.h`
- Create: `framework/components/audio/src/audio_playback.c`
- Create: `framework/components/audio/src/audio_playback.h`
- Create: `framework/components/audio/src/audio.c`
- Modify: `framework/components/audio/CMakeLists.txt`

- [ ] **Step 0: Add the shared core-affinity constant to `audio_i2s.h`**

Add this line to `framework/components/audio/src/audio_i2s.h`, right after `#define AUDIO_SAMPLE_RATE_HZ 44100`:

```c
// Core that BOTH the playback (consumer) and producer (diag/decoder) tasks pin
// to. The SPSC ring buffer has no memory barriers, so its producer and consumer
// must share a core (single-core context switches are full barriers). See the
// concurrency precondition in audio_ringbuf.h.
#define AUDIO_PIN_CORE 1
```

- [ ] **Step 1: Write the public API header**

Create `framework/components/audio/include/audio.h`:

```c
// Transport-agnostic audio core. Producers (RAOP decoder, later) push PCM via
// audio_play_pcm(); a playback task drains it to the DAC. Knows nothing about
// any network transport.
#pragma once
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Bring up I2S, allocate the PSRAM ring buffer, start the playback task.
void   audio_init(void);

// Enqueue up to n_frames of interleaved 16-bit stereo PCM. Returns frames
// accepted (< n_frames when the buffer is full — caller should retry the rest).
size_t audio_play_pcm(const int16_t *frames, size_t n_frames);

// Phase 0 diagnostic: start a task that streams a 440 Hz sine through
// audio_play_pcm() (the retained v0.0.1 known-good path).
void   audio_diag_tone_start(void);

#ifdef __cplusplus
}
#endif
```

- [ ] **Step 2: Write the playback task**

Create `framework/components/audio/src/audio_playback.c`:

```c
#include "audio_playback.h"
#include "audio_i2s.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define PLAYBACK_CHUNK_FRAMES 256

// Drains the ring into I2S. On underrun, writes a chunk of silence so the DAC
// keeps clocking cleanly instead of stalling or replaying stale samples.
void audio_playback_task(void *arg) {
    audio_ringbuf_t *ring = (audio_ringbuf_t *) arg;
    static int16_t chunk[PLAYBACK_CHUNK_FRAMES * 2];
    static const int16_t silence[PLAYBACK_CHUNK_FRAMES * 2] = {0};

    for (;;) {
        size_t got = audio_ringbuf_read(ring, chunk, PLAYBACK_CHUNK_FRAMES);
        if (got > 0) {
            audio_i2s_write(chunk, got);
        } else {
            audio_i2s_write(silence, PLAYBACK_CHUNK_FRAMES);
        }
    }
}
```

- [ ] **Step 3: Write the private playback header**

Create `framework/components/audio/src/audio_playback.h`:

```c
#pragma once
#include "audio_ringbuf.h"

// FreeRTOS task entry. arg is a pointer to the audio_ringbuf_t to drain.
void audio_playback_task(void *arg);
```

- [ ] **Step 4: Write the init/glue translation unit**

Create `framework/components/audio/src/audio.c`:

```c
#include "audio.h"
#include "audio_i2s.h"
#include "audio_ringbuf.h"
#include "audio_playback.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_heap_caps.h"
#include "esp_log.h"

// ~2 s of 44.1 kHz stereo in PSRAM.
#define RING_CAPACITY_FRAMES (AUDIO_SAMPLE_RATE_HZ * 2)

static const char     *TAG = "audio";
static audio_ringbuf_t s_ring;

void audio_init(void) {
    audio_i2s_init();

    int16_t *storage = heap_caps_malloc(
        RING_CAPACITY_FRAMES * 2 * sizeof(int16_t), MALLOC_CAP_SPIRAM);
    if (storage == NULL) {
        ESP_LOGE(TAG, "PSRAM ring alloc failed (%u frames)", RING_CAPACITY_FRAMES);
        abort();  // fail loud: the buffer is mandatory
    }
    audio_ringbuf_init(&s_ring, storage, RING_CAPACITY_FRAMES);

    // Pin the consumer (playback) to the same core as the producer (diag/decoder,
    // AUDIO_PIN_CORE). The SPSC ring's plain-size_t indices have no memory
    // barriers, so producer and consumer MUST be single-core-synchronized;
    // co-pinning makes every context switch between them a full barrier.
    xTaskCreatePinnedToCore(audio_playback_task, "playback", 4096, &s_ring, 5,
                            NULL, AUDIO_PIN_CORE);
    ESP_LOGI(TAG, "audio: ring=%u frames (~2s PSRAM), playback task up on core %d",
             RING_CAPACITY_FRAMES, AUDIO_PIN_CORE);
}

size_t audio_play_pcm(const int16_t *frames, size_t n_frames) {
    return audio_ringbuf_write(&s_ring, frames, n_frames);
}
```

- [ ] **Step 5: Register the component sources**

Replace the contents of `framework/components/audio/CMakeLists.txt` with:

```cmake
# audio: transport-agnostic Playback Manager, PCM ring buffer, I2S wrapper.
idf_component_register(
    SRCS "src/audio.c"
         "src/audio_i2s.c"
         "src/audio_ringbuf.c"
         "src/audio_playback.c"
         "src/audio_diag.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "src"
    PRIV_REQUIRES driver esp_driver_i2s esp_psram
)
```

> Note: `esp_driver_i2s` is required because ESP-IDF 6 split the I2S driver
> (`driver/i2s_std.h`) out of the `driver` meta-component. After changing a
> component's `PRIV_REQUIRES`, PlatformIO may reuse a stale CMake cache — wipe
> `framework/.pio/build/esp32-s3-n16r8` to force a reconfigure before rebuilding.

- [ ] **Step 6: Commit** (compiles after Task 4 adds `audio_diag.c`; build verified in Task 5)

```bash
cd /Users/uziiuzair/ooozzy/conduit-stream
git add framework/components/audio/include/audio.h \
        framework/components/audio/src/audio_playback.h \
        framework/components/audio/src/audio_playback.c \
        framework/components/audio/src/audio.c \
        framework/components/audio/CMakeLists.txt
git commit -m "feat(audio): playback task, public API, component registration"
```

---

## Task 4: Diagnostic 440 Hz tone through the new interface

**Files:**
- Create: `framework/components/audio/src/audio_diag.c`

- [ ] **Step 1: Write the diagnostic tone producer**

Create `framework/components/audio/src/audio_diag.c`:

```c
#include "audio.h"
#include "audio_i2s.h"   // AUDIO_SAMPLE_RATE_HZ

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include <math.h>

#define DIAG_TONE_HZ   440.0f
#define DIAG_AMPLITUDE (0.60f * 32767.0f)
#define DIAG_CHUNK     256

// Continuous-phase 440 Hz sine, pushed through audio_play_pcm() so the whole
// new audio path (ring -> playback task -> I2S -> DAC) is exercised. Retries
// unwritten frames so a full ring applies backpressure instead of dropping
// samples (which would click).
static void diag_tone_task(void *arg) {
    static int16_t buf[DIAG_CHUNK * 2];
    const float inc = 2.0f * (float) M_PI * DIAG_TONE_HZ / (float) AUDIO_SAMPLE_RATE_HZ;
    float phase = 0.0f;

    ESP_LOGI("audio_diag", "diagnostic: emitting %.0f Hz via audio_play_pcm()", DIAG_TONE_HZ);

    for (;;) {
        for (int i = 0; i < DIAG_CHUNK; i++) {
            int16_t s = (int16_t) (DIAG_AMPLITUDE * sinf(phase));
            buf[2 * i] = s;
            buf[2 * i + 1] = s;
            phase += inc;
            if (phase >= 2.0f * (float) M_PI) phase -= 2.0f * (float) M_PI;
        }
        size_t off = 0;
        while (off < DIAG_CHUNK) {
            off += audio_play_pcm(buf + off * 2, DIAG_CHUNK - off);
            if (off < DIAG_CHUNK) vTaskDelay(1);  // ring full -> yield, then retry
        }
    }
}

void audio_diag_tone_start(void) {
    // Pinned to AUDIO_PIN_CORE — same core as the playback consumer, so the
    // SPSC ring stays memory-synchronized (see audio_ringbuf.h precondition).
    xTaskCreatePinnedToCore(diag_tone_task, "diag_tone", 4096, NULL, 4,
                            NULL, AUDIO_PIN_CORE);
}
```

- [ ] **Step 2: Commit**

```bash
cd /Users/uziiuzair/ooozzy/conduit-stream
git add framework/components/audio/src/audio_diag.c
git commit -m "feat(audio): 440 Hz diagnostic tone fed through audio_play_pcm"
```

---

## Task 5: Refactor `main.cpp` onto the audio component

**Files:**
- Modify: `framework/src/main.cpp`

- [ ] **Step 1: Strip the inline I2S + tone code, call the audio API**

In `framework/src/main.cpp`:

1. **Delete** the audio-config macros and inline audio code: the `#include "driver/i2s_std.h"` line, the `I2S_DOUT_GPIO`/`I2S_BCLK_GPIO`/`I2S_LRCK_GPIO`/`SAMPLE_RATE_HZ`/`TONE_HZ`/`AMPLITUDE`/`FRAMES_PER_CHUNK` macros, the `static i2s_chan_handle_t s_tx_chan` global, and the entire `i2s_init()` and `tone_task()` functions.

2. **Add** the audio include near the other includes:

```c
#include "audio.h"
```

3. **Replace** the body of `app_main()` with:

```c
extern "C" void app_main(void)
{
    log_boot_banner();
    audio_init();               // I2S + PSRAM ring + playback task
    audio_diag_tone_start();    // 440 Hz through the new path (diagnostic)
    ESP_LOGI(TAG, "boot complete. if the Aura is silent, check SCK->GND and XSMT->3.3V.");
}
```

`log_boot_banner()` and its `#include`s (`esp_chip_info.h`, `esp_flash.h`, `esp_psram.h`, `esp_log.h`, etc.) stay exactly as they are.

- [ ] **Step 2: Build for the target**

Run: `cd framework && ~/.platformio/penv/bin/pio run -e esp32-s3-n16r8`
Expected: `[SUCCESS]`. If linking complains about an undefined `audio_*` symbol, confirm `components/audio/CMakeLists.txt` lists all five sources (Task 3 Step 5) and that `src/` code no longer references the removed `i2s_*`/`tone_*` symbols.

- [ ] **Step 3: Commit**

```bash
cd /Users/uziiuzair/ooozzy/conduit-stream
git add framework/src/main.cpp
git commit -m "refactor(main): drive audio via the audio component, keep boot banner"
```

---

## Task 6: On-target smoke test (proves the refactor didn't regress v0.0.1)

**Files:** none (verification only).

- [ ] **Step 1: Flash the refactored firmware**

Run: `cd framework && ~/.platformio/penv/bin/pio run -e esp32-s3-n16r8 -t upload --upload-port $(ls /dev/cu.usbmodem* | grep -v SN234567892 | head -1)`
Expected: `[SUCCESS]`, `Hash of data verified.`, `Hard resetting via RTS pin...`.
If it fails with `No serial data received`, hold BOOT, tap RESET, release BOOT, and retry (native-USB download mode).

- [ ] **Step 2: Capture the boot log and confirm the new path**

Run: `~/.platformio/penv/bin/python /private/tmp/claude-501/-Users-uziiuzair-ooozzy-conduit-stream/9e1f7bb1-1393-4743-b79a-d21eb5991c91/scratchpad/oneproc.py`
Expected log lines (order): the v0.0.1 banner + `chip`/`flash`/`psram`, then
`audio_i2s: i2s: std TX up @ 44100 Hz ...`, `audio: ring=88200 frames (~2s PSRAM), playback task up`,
and `audio_diag: diagnostic: emitting 440 Hz via audio_play_pcm()`.

- [ ] **Step 3: Confirm audio**

Listen: a clean 440 Hz "A" from the Aura, identical to v0.0.1 — now flowing ring → playback task → I2S rather than straight from `tone_task`. Silence would mean the playback task or ring wiring regressed (the diagnostic isolates it from any hardware, which is unchanged).

- [ ] **Step 4: Tag the phase complete**

```bash
cd /Users/uziiuzair/ooozzy/conduit-stream
git tag -a v0.2-phase0 -m "Phase 0: audio path refactored behind audio_play_pcm(); sine via new path"
```

---

## Self-Review

**Spec coverage (Phase 0 rows only):**
- Audio refactor: I2S → `audio` behind `audio_play_pcm()` — Tasks 2–5. ✓
- PSRAM jitter buffer — Task 1 (logic) + Task 3 Step 4 (PSRAM alloc). ✓ (Phase 0 uses a plain PCM FIFO; RTP-sequence-indexed reordering arrives in the Phase 3/4 plan, as the spec scopes it — noted so it is not mistaken for a gap.)
- Playback task, silence on underrun — Task 3 Step 2. ✓
- Retain v0.0.1 sine as diagnostic mode — Task 4. ✓
- Regression proof — Task 6. ✓

**Type consistency:** `audio_ringbuf_t`, `audio_ringbuf_init/available/free_space/write/read`, `audio_init/audio_play_pcm/audio_diag_tone_start`, `audio_i2s_init/audio_i2s_write`, `audio_playback_task`, `AUDIO_SAMPLE_RATE_HZ` — names and signatures match across all tasks. Ring capacity `AUDIO_SAMPLE_RATE_HZ * 2` = 88200 frames, matching the Task 6 expected log.

**Placeholder scan:** none — every step has concrete code or an exact command + expected output.

**Deferred to later-phase plans (not Phase 0):** volume scaling, RTP-seq reordering, retransmit, drift watermarks, metadata, RGB LED, Wi-Fi, mDNS, RTSP, crypto, ALAC. Each is a row in a later phase and will get its own plan.
