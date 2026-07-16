# AirPlay 1 — Phase 1: Wi-Fi Station + mDNS `_raop._tcp` Advertise — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal (spec §7 + phasing row 1):** bring the ESP32-S3 up as a Wi-Fi **station** using
NVS-stored credentials (seeded from Kconfig on first boot, `WIFI_PS_NONE`, 2.4 GHz), and
advertise `_raop._tcp` over mDNS with the RAOP TXT records — so the speaker **appears in
the iPhone/Mac AirPlay menu**. Nothing listens on the RTSP port yet (that is Phase 2); the
440 Hz diagnostic tone keeps running to prove the audio path still works.

**Architecture:** three components move from empty stubs to real code — `system` (NVS config
store + device-id/instance-name derivation), `network` (Wi-Fi station bring-up + event
handling + reconnect backoff), `mdns` (thin wrapper over IDF's managed `espressif/mdns`).
Following the Phase 0 discipline, every branch of *pure* logic lives in ESP-IDF-free `.c`
files that compile and run on the host under `pio test -e native`: the **device-id / instance-name
formatter** (`system`) and the **RAOP TXT-record builder** (`mdns`). The ESP-IDF glue (NVS,
esp_wifi, esp_event, esp_netif, mdns_*) is verified by target build only.

**Tech Stack:** ESP-IDF 6.0.1, PlatformIO, C11 for the new components, Unity (`native` env)
for host unit tests. IDF components: `nvs_flash`, `esp_hw_support` (MAC), `esp_wifi`,
`esp_event`, `esp_netif`, `esp_timer`, and the managed `espressif/mdns`.

**Do NOT touch the `audio` component** (Phase 0, merged). It carries a **same-core producer
contract** for `audio_play_pcm()` (SPSC ring, `AUDIO_PIN_CORE`); Phase 1 adds no producer, so
nothing there changes — just don't break it. This is the carried-forward Phase 0 review item;
it is re-noted in the `system` README (Task 6).

---

## File Structure

```
framework/
  platformio.ini                          # MODIFY: native env -I system/src + mdns/src
  src/main.cpp                            # MODIFY: nvs + system + wifi + mdns orchestration
  components/system/
    CMakeLists.txt                        # MODIFY: register sources, REQUIRES
    Kconfig.projbuild                     # CREATE: CONFIG_CONDUIT_WIFI_SSID/PASSWORD/DEVICE_NAME
    README.md                             # MODIFY: document config store + producer-contract note
    include/system_config.h               # CREATE: public API (init + getters)
    src/device_id.h                       # CREATE: pure formatter decls (no ESP-IDF)
    src/device_id.c                       # CREATE: pure MAC->id + instance-name (host-testable)
    src/system_config.c                   # CREATE: NVS glue (seed from Kconfig, getters, MAC)
  components/network/
    CMakeLists.txt                        # MODIFY: register wifi.c, REQUIRES
    README.md                             # MODIFY: document wifi station bring-up
    include/wifi.h                        # CREATE: public API (wifi_start + got-ip callback)
    src/wifi.c                            # CREATE: station bring-up, events, reconnect backoff
  components/mdns/
    CMakeLists.txt                        # MODIFY: register mdns_service.c, REQUIRES mdns
    idf_component.yml                     # CREATE: depend on espressif/mdns (managed)
    README.md                             # MODIFY: document _raop._tcp advertise
    include/mdns_service.h                # CREATE: public API + RAOP_RTSP_PORT
    src/raop_txt.h                        # CREATE: pure RAOP TXT builder decls (no ESP-IDF)
    src/raop_txt.c                        # CREATE: pure TXT builder (host-testable)
    src/mdns_service.c                    # CREATE: IDF mdns wrapper glue
  test/test_device_id/test_device_id.c    # CREATE: Unity host tests (id + instance name)
  test/test_raop_txt/test_raop_txt.c      # CREATE: Unity host tests (TXT records)
```

**Responsibilities:**
- `device_id.*` — pure C: 6 MAC bytes → `"E83DC1F2AC6C"`, and `(id,name)` → `"E83DC1F2AC6C@Conduit"`. Zero ESP-IDF deps → host-tested (the branchy string logic, so it gets TDD focus).
- `system_config.c` — NVS namespace `conduit`; seed `wifi_ssid`/`wifi_pass`/`name` from Kconfig on first boot, then serve from NVS; derive `device_id` from the base MAC at boot; cache the instance name. Not host-testable (NVS/MAC) → target build only.
- `raop_txt.*` — pure C: fills a caller-owned `raop_txt_item_t[]` with the exact RAOP key/value strings. Zero ESP-IDF deps → host-tested against exact strings.
- `mdns_service.c` — `mdns_init` + hostname + instance name + `_raop._tcp` service with the TXT records from `raop_txt_build`. Thin IDF glue → target build only.
- `wifi.c` — station bring-up from `system_config`, `WIFI_PS_NONE`, event handlers, reconnect with capped backoff, GOT_IP → log IP + fire the registered callback. IDF glue → target build only.

---

## Task 1: Device-id / instance-name formatter (host-unit-tested, pure C)

**Files:**
- Create: `framework/components/system/src/device_id.h`
- Create: `framework/components/system/src/device_id.c`
- Create: `framework/test/test_device_id/test_device_id.c`
- Modify: `framework/platformio.ini` (extend the `native` env include path)

- [ ] **Step 1: Extend the host-test include path in `platformio.ini`**

Replace the `build_flags` line in the existing `[env:native]` block with (adds the two new
pure-source dirs; keeps `audio/src` so `test_ringbuf` still builds):

```ini
build_flags = -I components/audio/src -I components/system/src -I components/mdns/src -std=gnu11
```

- [ ] **Step 2: Write the pure formatter header**

Create `framework/components/system/src/device_id.h`:

```c
// Pure-C identity string formatters. No ESP-IDF deps, so host-unit-testable.
// The device id is derived from the base MAC at boot (not stored); the RAOP
// service instance name is "<deviceid>@<name>" (e.g. "E83DC1F2AC6C@Conduit").
#pragma once

#include <stddef.h>
#include <stdint.h>

// A 6-byte MAC -> 12 uppercase hex chars + NUL. `out` must hold >= 13 bytes.
// e.g. {0xE8,0x3D,0xC1,0xF2,0xAC,0x6C} -> "E83DC1F2AC6C".
void device_id_format(const uint8_t mac[6], char out[13]);

// "<device_id>@<name>" into `out` (truncated to out_size, always NUL-terminated).
void device_instance_name_format(const char *device_id, const char *name,
                                  char *out, size_t out_size);
```

- [ ] **Step 3: Write the failing host tests**

Create `framework/test/test_device_id/test_device_id.c`:

```c
#include <unity.h>
#include <string.h>
#include "device_id.c"  // compile the pure unit directly into the test TU

void setUp(void)    {}
void tearDown(void) {}

void test_device_id_format_uppercase_hex(void) {
    const uint8_t mac[6] = {0xE8, 0x3D, 0xC1, 0xF2, 0xAC, 0x6C};
    char out[13];
    device_id_format(mac, out);
    TEST_ASSERT_EQUAL_STRING("E83DC1F2AC6C", out);
}

void test_device_id_format_zero_pads(void) {
    const uint8_t mac[6] = {0x00, 0x0A, 0x00, 0x0B, 0x00, 0x0C};
    char out[13];
    device_id_format(mac, out);
    TEST_ASSERT_EQUAL_STRING("000A000B000C", out);
}

void test_instance_name_joins_with_at(void) {
    char out[32];
    device_instance_name_format("E83DC1F2AC6C", "Conduit", out, sizeof(out));
    TEST_ASSERT_EQUAL_STRING("E83DC1F2AC6C@Conduit", out);
}

void test_instance_name_truncates_safely(void) {
    char out[8];  // too small; must stay NUL-terminated, never overflow
    device_instance_name_format("E83DC1F2AC6C", "Conduit", out, sizeof(out));
    TEST_ASSERT_EQUAL_UINT(7, strlen(out));
    TEST_ASSERT_EQUAL_CHAR('\0', out[7]);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_device_id_format_uppercase_hex);
    RUN_TEST(test_device_id_format_zero_pads);
    RUN_TEST(test_instance_name_joins_with_at);
    RUN_TEST(test_instance_name_truncates_safely);
    return UNITY_END();
}
```

- [ ] **Step 4: Run the tests to verify they fail (no impl yet)**

Run: `cd framework && ~/.platformio/penv/bin/pio test -e native`
Expected: FAIL to compile — `device_id.c: No such file or directory`.

- [ ] **Step 5: Write the pure formatter implementation**

Create `framework/components/system/src/device_id.c`:

```c
#include "device_id.h"
#include <stdio.h>

void device_id_format(const uint8_t mac[6], char out[13]) {
    snprintf(out, 13, "%02X%02X%02X%02X%02X%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

void device_instance_name_format(const char *device_id, const char *name,
                                 char *out, size_t out_size) {
    snprintf(out, out_size, "%s@%s", device_id, name);
}
```

- [ ] **Step 6: Run the tests to verify they pass**

Run: `cd framework && ~/.platformio/penv/bin/pio test -e native`
Expected: PASS — both `test_ringbuf` and `test_device_id` suites green (`4 Tests 0 Failures` for the new one).

- [ ] **Step 7: Commit**

```bash
cd /Users/uziiuzair/ooozzy/conduit-stream
git add framework/platformio.ini \
        framework/components/system/src/device_id.h \
        framework/components/system/src/device_id.c \
        framework/test/test_device_id/test_device_id.c
git commit -m "feat(system): pure device-id + instance-name formatters with host tests"
```

---

## Task 2: RAOP TXT-record builder (host-unit-tested, pure C)

**Files:**
- Create: `framework/components/mdns/src/raop_txt.h`
- Create: `framework/components/mdns/src/raop_txt.c`
- Create: `framework/test/test_raop_txt/test_raop_txt.c`

The RAOP TXT records (spec §5a) are what make the speaker appear in the AirPlay menu. They are
constant capability advertisements, so the builder is a pure function — exactly the kind of
logic the plan isolates for host testing.

- [ ] **Step 1: Write the pure builder header**

Create `framework/components/mdns/src/raop_txt.h`:

```c
// Pure-C RAOP mDNS TXT-record builder. No ESP-IDF deps, so host-unit-testable.
// The values are the AirPlay-1 capability advertisement from the design spec
// (§5a): ALAC, 44100/16/stereo, encryption none+RSA/AES, text metadata.
#pragma once

#include <stddef.h>

// Layout mirrors IDF's mdns_txt_item_t (key/value C strings) but stays const &
// ESP-IDF-free; mdns_service.c copies these into the IDF struct.
typedef struct {
    const char *key;
    const char *value;
} raop_txt_item_t;

// Number of TXT records produced (size caller-owned arrays with this).
#define RAOP_TXT_COUNT 10

// Fill `items` (caller-owned, >= RAOP_TXT_COUNT entries) with the RAOP TXT set.
// Returns the number written (== RAOP_TXT_COUNT when max_items is sufficient,
// else the number that fit).
size_t raop_txt_build(raop_txt_item_t *items, size_t max_items);
```

- [ ] **Step 2: Write the failing host tests**

Create `framework/test/test_raop_txt/test_raop_txt.c`:

```c
#include <unity.h>
#include <string.h>
#include "raop_txt.c"  // compile the pure unit directly into the test TU

static raop_txt_item_t items[RAOP_TXT_COUNT];

void setUp(void)    { memset(items, 0, sizeof(items)); }
void tearDown(void) {}

// Look a key up in the built array; NULL if absent.
static const char *find(const char *key) {
    for (size_t i = 0; i < RAOP_TXT_COUNT; i++)
        if (items[i].key && strcmp(items[i].key, key) == 0) return items[i].value;
    return NULL;
}

void test_builds_full_record_count(void) {
    TEST_ASSERT_EQUAL_UINT(RAOP_TXT_COUNT, raop_txt_build(items, RAOP_TXT_COUNT));
}

void test_exact_capability_values(void) {
    raop_txt_build(items, RAOP_TXT_COUNT);
    TEST_ASSERT_EQUAL_STRING("UDP",   find("tp"));
    TEST_ASSERT_EQUAL_STRING("44100", find("sr"));
    TEST_ASSERT_EQUAL_STRING("16",    find("ss"));
    TEST_ASSERT_EQUAL_STRING("2",     find("ch"));
    TEST_ASSERT_EQUAL_STRING("1",     find("cn"));   // ALAC
    TEST_ASSERT_EQUAL_STRING("0,1",   find("et"));   // none + RSA/AES
    TEST_ASSERT_EQUAL_STRING("false", find("sv"));
    TEST_ASSERT_EQUAL_STRING("true",  find("da"));
    TEST_ASSERT_EQUAL_STRING("3",     find("vn"));
    TEST_ASSERT_EQUAL_STRING("0,1,2", find("md"));   // text metadata honored
}

void test_respects_max_items_bound(void) {
    // Never writes past the caller's array.
    TEST_ASSERT_EQUAL_UINT(3, raop_txt_build(items, 3));
    TEST_ASSERT_NOT_NULL(items[2].key);
    TEST_ASSERT_NULL(items[3].key);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_builds_full_record_count);
    RUN_TEST(test_exact_capability_values);
    RUN_TEST(test_respects_max_items_bound);
    return UNITY_END();
}
```

- [ ] **Step 3: Run the tests to verify they fail**

Run: `cd framework && ~/.platformio/penv/bin/pio test -e native`
Expected: FAIL to compile — `raop_txt.c: No such file or directory`.

- [ ] **Step 4: Write the pure builder implementation**

Create `framework/components/mdns/src/raop_txt.c`:

```c
#include "raop_txt.h"

size_t raop_txt_build(raop_txt_item_t *items, size_t max_items) {
    static const raop_txt_item_t kRaop[RAOP_TXT_COUNT] = {
        {"tp", "UDP"},    // transport: audio over UDP/RTP
        {"sr", "44100"},  // sample rate
        {"ss", "16"},     // sample size (bits)
        {"ch", "2"},      // channels (stereo)
        {"cn", "1"},      // codecs: ALAC
        {"et", "0,1"},    // encryption: none + RSA/AES
        {"sv", "false"},  // not a "server" advertising extra features
        {"da", "true"},   // digest auth available
        {"vn", "3"},      // AirTunes protocol version
        {"md", "0,1,2"},  // metadata: text (0), artwork(1)/progress(2) declared
    };
    size_t n = (max_items < RAOP_TXT_COUNT) ? max_items : RAOP_TXT_COUNT;
    for (size_t i = 0; i < n; i++) items[i] = kRaop[i];
    return n;
}
```

- [ ] **Step 5: Run the tests to verify they pass**

Run: `cd framework && ~/.platformio/penv/bin/pio test -e native`
Expected: PASS — `test_raop_txt` green (`3 Tests 0 Failures`), all three host suites pass.

- [ ] **Step 6: Commit**

```bash
cd /Users/uziiuzair/ooozzy/conduit-stream
git add framework/components/mdns/src/raop_txt.h \
        framework/components/mdns/src/raop_txt.c \
        framework/test/test_raop_txt/test_raop_txt.c
git commit -m "feat(mdns): pure RAOP TXT-record builder with host tests"
```

---

## Task 3: `system` config store (NVS + Kconfig seed + MAC device id)

**Files:**
- Create: `framework/components/system/Kconfig.projbuild`
- Create: `framework/components/system/include/system_config.h`
- Create: `framework/components/system/src/system_config.c`
- Modify: `framework/components/system/CMakeLists.txt`

No host test — NVS + MAC are ESP-IDF-only; verified by the Task 6 target build. The pure string
logic it leans on is already covered by Task 1.

- [ ] **Step 1: Write the Kconfig seed defaults**

Create `framework/components/system/Kconfig.projbuild`:

```
menu "Conduit Stream Configuration"

    config CONDUIT_WIFI_SSID
        string "Wi-Fi SSID (seed for NVS on first boot)"
        default ""
        help
            2.4 GHz SSID seeded into NVS namespace "conduit" on first boot.
            Left empty means "no creds": the device logs a clear message and
            sits idle (no boot-loop). Change later in NVS without reflashing.

    config CONDUIT_WIFI_PASSWORD
        string "Wi-Fi password (seed for NVS on first boot)"
        default ""
        help
            WPA2 password seeded into NVS on first boot. Empty = open network
            or "no creds" (see SSID).

    config CONDUIT_DEVICE_NAME
        string "Speaker name shown in the AirPlay menu"
        default "Conduit"
        help
            Seeds the "name" key in NVS; used in the RAOP instance name
            "<deviceid>@<name>".

endmenu
```

- [ ] **Step 2: Write the public config API header**

Create `framework/components/system/include/system_config.h`:

```c
// System config store. Namespace "conduit" in NVS: wifi_ssid, wifi_pass, name.
// Seeded from Kconfig on first boot, then NVS is source of truth. The device id
// is derived from the base MAC at boot (not stored). Getters return pointers to
// internal cached strings, valid after a successful system_config_init().
#pragma once

#include <stdbool.h>
#include "esp_err.h"

// Load config: derive device id from MAC, open NVS, seed missing keys from
// Kconfig, cache ssid/pass/name + instance name. Returns ESP_OK on success.
esp_err_t   system_config_init(void);

const char *system_config_get_ssid(void);       // "" if unset
const char *system_config_get_password(void);    // "" if unset
const char *system_config_get_name(void);        // e.g. "Conduit"
const char *system_config_get_device_id(void);   // e.g. "E83DC1F2AC6C"
const char *system_config_get_instance_name(void); // "E83DC1F2AC6C@Conduit"

// True only when an SSID is present — main uses this to decide whether to bring
// Wi-Fi up or log the "no creds" message and sit idle (no boot-loop).
bool        system_config_has_credentials(void);
```

- [ ] **Step 3: Write the NVS glue implementation**

Create `framework/components/system/src/system_config.c`:

```c
#include "system_config.h"
#include "device_id.h"

#include "nvs.h"
#include "nvs_flash.h"
#include "esp_mac.h"
#include "esp_log.h"
#include <string.h>

#define NS "conduit"

static const char *TAG = "sys_cfg";

static char s_ssid[64];
static char s_pass[64];
static char s_name[33];
static char s_device_id[13];
static char s_instance[64];

// Read `key` into buf; if absent, seed it from `seed` (persisting to NVS) so the
// NVS copy becomes the source of truth for later boots.
static void load_or_seed(nvs_handle_t h, const char *key, const char *seed,
                         char *buf, size_t buf_len) {
    size_t len = buf_len;
    esp_err_t err = nvs_get_str(h, key, buf, &len);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        snprintf(buf, buf_len, "%s", seed);
        if (nvs_set_str(h, key, buf) == ESP_OK) nvs_commit(h);
        ESP_LOGI(TAG, "seeded NVS '%s' from Kconfig", key);
    } else if (err != ESP_OK) {
        ESP_LOGW(TAG, "nvs_get_str('%s') failed: %s; using seed",
                 key, esp_err_to_name(err));
        snprintf(buf, buf_len, "%s", seed);
    }
}

esp_err_t system_config_init(void) {
    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    device_id_format(mac, s_device_id);

    nvs_handle_t h;
    esp_err_t err = nvs_open(NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_open('%s') failed: %s", NS, esp_err_to_name(err));
        return err;
    }
    load_or_seed(h, "wifi_ssid", CONFIG_CONDUIT_WIFI_SSID, s_ssid, sizeof(s_ssid));
    load_or_seed(h, "wifi_pass", CONFIG_CONDUIT_WIFI_PASSWORD, s_pass, sizeof(s_pass));
    load_or_seed(h, "name",      CONFIG_CONDUIT_DEVICE_NAME,  s_name, sizeof(s_name));
    nvs_close(h);

    device_instance_name_format(s_device_id, s_name, s_instance, sizeof(s_instance));
    ESP_LOGI(TAG, "device id %s, name '%s', instance '%s'",
             s_device_id, s_name, s_instance);
    return ESP_OK;
}

const char *system_config_get_ssid(void)          { return s_ssid; }
const char *system_config_get_password(void)      { return s_pass; }
const char *system_config_get_name(void)          { return s_name; }
const char *system_config_get_device_id(void)     { return s_device_id; }
const char *system_config_get_instance_name(void) { return s_instance; }
bool        system_config_has_credentials(void)   { return s_ssid[0] != '\0'; }
```

- [ ] **Step 4: Register the component sources**

Replace `framework/components/system/CMakeLists.txt` with:

```cmake
# system: NVS config store, device identity. (Later: LED, health, RAOP RSA key.)
idf_component_register(
    SRCS "src/system_config.c"
         "src/device_id.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "src"
    PRIV_REQUIRES nvs_flash esp_hw_support
)
```

> `esp_read_mac`/`esp_mac.h` live in `esp_hw_support`; NVS in `nvs_flash`. The public
> header only exposes `esp_err_t` (from the always-available `esp_common`), so both go in
> `PRIV_REQUIRES`. After editing REQUIRES, wipe `framework/.pio/build/esp32-s3-n16r8`
> before rebuilding (stale CMake cache keeps old include paths).

- [ ] **Step 5: Build for the target**

```bash
rm -rf framework/.pio/build/esp32-s3-n16r8
cd framework && ~/.platformio/penv/bin/pio run -e esp32-s3-n16r8
```
Expected: `[SUCCESS]`. (main.cpp doesn't call these yet — this just proves the component
compiles and links.)

- [ ] **Step 6: Commit**

```bash
cd /Users/uziiuzair/ooozzy/conduit-stream
git add framework/components/system/Kconfig.projbuild \
        framework/components/system/include/system_config.h \
        framework/components/system/src/system_config.c \
        framework/components/system/CMakeLists.txt
git commit -m "feat(system): NVS config store seeded from Kconfig, MAC device id"
```

---

## Task 4: `network` Wi-Fi station (bring-up, events, reconnect backoff)

**Files:**
- Create: `framework/components/network/include/wifi.h`
- Create: `framework/components/network/src/wifi.c`
- Modify: `framework/components/network/CMakeLists.txt`

- [ ] **Step 1: Write the public Wi-Fi API header**

Create `framework/components/network/include/wifi.h` (callback is argument-free so the public
header stays ESP-IDF-type-free; `wifi.c` logs the acquired IP itself):

```c
// Wi-Fi station bring-up. Connects using credentials from system_config,
// disables modem power-save (WIFI_PS_NONE — its latency spikes cause RTP
// jitter; the top ESP32-AirPlay stutter fix), and reconnects with backoff.
#pragma once

// Invoked (once per acquisition) from the Wi-Fi event task when an IPv4 address
// is obtained. main uses it to start mDNS. Keep the callback short & non-blocking.
typedef void (*wifi_got_ip_cb_t)(void);

// Bring up station mode and start connecting. `on_got_ip` may be NULL.
// Precondition: nvs_flash_init() and system_config_init() have run, and an SSID
// is present (caller checks system_config_has_credentials()).
void wifi_start(wifi_got_ip_cb_t on_got_ip);
```

- [ ] **Step 2: Write the Wi-Fi station implementation**

Create `framework/components/network/src/wifi.c`:

```c
#include "wifi.h"
#include "system_config.h"

#include "freertos/FreeRTOS.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_log.h"
#include <string.h>

static const char *TAG = "wifi";

static wifi_got_ip_cb_t s_on_got_ip = NULL;
static esp_timer_handle_t s_reconnect_timer = NULL;
static uint32_t s_backoff_ms = 500;   // grows to a cap on repeated failures

#define BACKOFF_MAX_MS 8000

static void reconnect_cb(void *arg) { esp_wifi_connect(); }

static void schedule_reconnect(void) {
    uint32_t delay = s_backoff_ms;
    s_backoff_ms = (s_backoff_ms < BACKOFF_MAX_MS) ? s_backoff_ms * 2 : BACKOFF_MAX_MS;
    ESP_LOGW(TAG, "disconnected; reconnecting in %u ms", (unsigned) delay);
    esp_timer_start_once(s_reconnect_timer, (uint64_t) delay * 1000);
}

static void on_wifi_event(void *arg, esp_event_base_t base,
                          int32_t id, void *data) {
    if (id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (id == WIFI_EVENT_STA_DISCONNECTED) {
        schedule_reconnect();
    }
}

static void on_ip_event(void *arg, esp_event_base_t base,
                        int32_t id, void *data) {
    if (id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *e = (ip_event_got_ip_t *) data;
        ESP_LOGI(TAG, "got IP " IPSTR, IP2STR(&e->ip_info.ip));
        s_backoff_ms = 500;  // reset backoff on success
        if (s_on_got_ip) s_on_got_ip();
    }
}

void wifi_start(wifi_got_ip_cb_t on_got_ip) {
    s_on_got_ip = on_got_ip;

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, &on_wifi_event, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, &on_ip_event, NULL, NULL));

    const esp_timer_create_args_t targs = {
        .callback = &reconnect_cb, .name = "wifi_reconnect" };
    ESP_ERROR_CHECK(esp_timer_create(&targs, &s_reconnect_timer));

    wifi_config_t cfg = {0};
    snprintf((char *) cfg.sta.ssid, sizeof(cfg.sta.ssid), "%s",
             system_config_get_ssid());
    snprintf((char *) cfg.sta.password, sizeof(cfg.sta.password), "%s",
             system_config_get_password());

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &cfg));
    ESP_ERROR_CHECK(esp_wifi_start());
    // Disable modem power-save AFTER start: latency spikes it introduces are the
    // single most common cause of AirPlay RTP stutter on ESP32 (spec §7).
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));

    ESP_LOGI(TAG, "station starting: SSID '%s', WIFI_PS_NONE, 2.4 GHz",
             system_config_get_ssid());
}
```

- [ ] **Step 3: Register the component sources**

Replace `framework/components/network/CMakeLists.txt` with:

```cmake
# network: Wi-Fi station bring-up. (Later: RAOP RTSP/RTP transport plugin.)
idf_component_register(
    SRCS "src/wifi.c"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES esp_wifi esp_event esp_netif esp_timer system
)
```

> The public `wifi.h` exposes no ESP-IDF types, so every IDF dep (plus `system` for the
> credentials) is private. Wipe `framework/.pio/build/esp32-s3-n16r8` after this edit.

- [ ] **Step 4: Build for the target**

```bash
rm -rf framework/.pio/build/esp32-s3-n16r8
cd framework && ~/.platformio/penv/bin/pio run -e esp32-s3-n16r8
```
Expected: `[SUCCESS]`.

- [ ] **Step 5: Commit**

```bash
cd /Users/uziiuzair/ooozzy/conduit-stream
git add framework/components/network/include/wifi.h \
        framework/components/network/src/wifi.c \
        framework/components/network/CMakeLists.txt
git commit -m "feat(network): Wi-Fi station bring-up, WIFI_PS_NONE, reconnect backoff"
```

---

## Task 5: `mdns` wrapper — advertise `_raop._tcp`

**Files:**
- Create: `framework/components/mdns/idf_component.yml`
- Create: `framework/components/mdns/include/mdns_service.h`
- Create: `framework/components/mdns/src/mdns_service.c`
- Modify: `framework/components/mdns/CMakeLists.txt`

- [ ] **Step 1: Declare the managed `espressif/mdns` dependency**

Create `framework/components/mdns/idf_component.yml` (the component manager downloads it during
the build):

```yaml
dependencies:
  espressif/mdns: "^1.2.0"
```

- [ ] **Step 2: Write the public mDNS API header**

Create `framework/components/mdns/include/mdns_service.h`:

```c
// mDNS advertisement of the AirPlay-1 (RAOP) service so the speaker appears in
// the iOS/macOS AirPlay menu. Thin wrapper over IDF's managed `espressif/mdns`.
#pragma once

#include <stdint.h>
#include "esp_err.h"

// RTSP control port advertised for _raop._tcp. Nothing listens here in Phase 1
// (the RTSP state machine is Phase 2); the advert alone makes us discoverable.
#define RAOP_RTSP_PORT 5000

// Init mDNS, set `hostname` (e.g. "conduit"), and advertise _raop._tcp on `port`
// with the RAOP TXT records and service instance name `instance_name`
// (e.g. "E83DC1F2AC6C@Conduit"). Precondition: esp_netif/Wi-Fi are up.
esp_err_t mdns_advertise_raop(const char *hostname, const char *instance_name,
                              uint16_t port);
```

- [ ] **Step 3: Write the mDNS glue implementation**

Create `framework/components/mdns/src/mdns_service.c`:

```c
#include "mdns_service.h"
#include "raop_txt.h"

#include "mdns.h"      // managed espressif/mdns
#include "esp_log.h"

static const char *TAG = "mdns";

esp_err_t mdns_advertise_raop(const char *hostname, const char *instance_name,
                              uint16_t port) {
    esp_err_t err = mdns_init();
    if (err != ESP_OK) { ESP_LOGE(TAG, "mdns_init: %s", esp_err_to_name(err)); return err; }

    ESP_ERROR_CHECK(mdns_hostname_set(hostname));
    ESP_ERROR_CHECK(mdns_instance_name_set(instance_name));

    raop_txt_item_t items[RAOP_TXT_COUNT];
    size_t n = raop_txt_build(items, RAOP_TXT_COUNT);

    // Copy the pure const items into IDF's (non-const) txt struct.
    mdns_txt_item_t txt[RAOP_TXT_COUNT];
    for (size_t i = 0; i < n; i++) {
        txt[i].key   = (char *) items[i].key;
        txt[i].value = (char *) items[i].value;
    }

    err = mdns_service_add(instance_name, "_raop", "_tcp", port, txt, n);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "service_add _raop._tcp: %s", esp_err_to_name(err));
        return err;
    }
    ESP_LOGI(TAG, "advertising _raop._tcp '%s' on %s.local:%u (%u TXT records)",
             instance_name, hostname, port, (unsigned) n);
    return ESP_OK;
}
```

- [ ] **Step 4: Register the component sources**

Replace `framework/components/mdns/CMakeLists.txt` with:

```cmake
# mdns: advertise the AirPlay (_raop._tcp) service via IDF's managed mdns.
idf_component_register(
    SRCS "src/mdns_service.c"
         "src/raop_txt.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "src"
    PRIV_REQUIRES mdns
)
```

> `mdns` here refers to the managed `espressif/mdns` component pulled in by `idf_component.yml`.
> The first build after adding it downloads the package (needs network). Wipe
> `framework/.pio/build/esp32-s3-n16r8` after this edit to force the manager + a reconfigure.

- [ ] **Step 5: Build for the target**

```bash
rm -rf framework/.pio/build/esp32-s3-n16r8
cd framework && ~/.platformio/penv/bin/pio run -e esp32-s3-n16r8
```
Expected: `[SUCCESS]`; the log shows the manager fetching `espressif/mdns` on the first run.

- [ ] **Step 6: Commit**

```bash
cd /Users/uziiuzair/ooozzy/conduit-stream
git add framework/components/mdns/idf_component.yml \
        framework/components/mdns/include/mdns_service.h \
        framework/components/mdns/src/mdns_service.c \
        framework/components/mdns/CMakeLists.txt
git commit -m "feat(mdns): advertise _raop._tcp with RAOP TXT records"
```

---

## Task 6: `main.cpp` orchestration + READMEs

**Files:**
- Modify: `framework/src/main.cpp`
- Modify: `framework/components/system/README.md`
- Modify: `framework/components/network/README.md`
- Modify: `framework/components/mdns/README.md`

- [ ] **Step 1: Wire the boot sequence in `main.cpp`**

Add the new includes near `#include "audio.h"`:

```c
#include "nvs_flash.h"
#include "system_config.h"
#include "wifi.h"
#include "mdns_service.h"
```

Add a GOT_IP callback above `app_main` (fires from the Wi-Fi event task; starts mDNS):

```c
static void on_got_ip(void)
{
    // esp_netif is up now — safe to advertise. Hostname "conduit" -> conduit.local.
    mdns_advertise_raop("conduit", system_config_get_instance_name(), RAOP_RTSP_PORT);
}
```

Replace the body of `app_main()` with (keep the banner + the diagnostic tone):

```c
extern "C" void app_main(void)
{
    log_boot_banner();
    audio_init();               // I2S + PSRAM ring + playback task (Phase 0)
    audio_diag_tone_start();    // 440 Hz through the audio path (still proves audio works)

    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(system_config_init());

    if (system_config_has_credentials()) {
        wifi_start(on_got_ip);  // on GOT_IP -> mdns_advertise_raop(...)
    } else {
        // Spec §8: no creds -> one clear line, DO NOT boot-loop. Sit idle; the
        // 440 Hz tone keeps playing so the device is obviously alive.
        ESP_LOGW(TAG, "no Wi-Fi credentials: set CONFIG_CONDUIT_WIFI_SSID (menuconfig) "
                      "or write nvs 'conduit/wifi_ssid'. Idling; audio path still runs.");
    }

    ESP_LOGI(TAG, "boot complete.");
}
```

> `nvs_flash_init()` may return `ESP_ERR_NVS_NO_FREE_PAGES`/`NEW_VERSION_FOUND` after a
> partition change; if the target build ever hits that, erase+retry. For Phase 1 the `nvs`
> partition is fresh from Phase 0, so the plain `ESP_ERROR_CHECK` is fine — but note it here.
> `main` implicitly depends on all components, so no `src/CMakeLists.txt` edit is needed.

- [ ] **Step 2: Update the three component READMEs**

- `components/system/README.md` — document the NVS config store (namespace `conduit`, keys,
  Kconfig seeding, MAC-derived device id) AND re-note the carried-forward Phase 0 item:
  *"`audio_play_pcm()` has a same-core producer contract (SPSC ring, `AUDIO_PIN_CORE`);
  Phase 1 adds no audio producer and must not break it."*
- `components/network/README.md` — Wi-Fi station bring-up, `WIFI_PS_NONE`, reconnect backoff,
  GOT_IP callback; RAOP transport plugin still to come (Phase 2+).
- `components/mdns/README.md` — advertises `_raop._tcp` on `RAOP_RTSP_PORT` with the RAOP TXT
  records; note the RTSP port has no listener until Phase 2.

- [ ] **Step 3: Build for the target**

```bash
rm -rf framework/.pio/build/esp32-s3-n16r8
cd framework && ~/.platformio/penv/bin/pio run -e esp32-s3-n16r8
```
Expected: `[SUCCESS]`.

- [ ] **Step 4: Re-run the full host test suite (guard against regressions)**

Run: `cd framework && ~/.platformio/penv/bin/pio test -e native`
Expected: PASS — `test_ringbuf`, `test_device_id`, `test_raop_txt` all green.

- [ ] **Step 5: Commit**

```bash
cd /Users/uziiuzair/ooozzy/conduit-stream
git add framework/src/main.cpp \
        framework/components/system/README.md \
        framework/components/network/README.md \
        framework/components/mdns/README.md
git commit -m "feat(main): orchestrate NVS+config+wifi+mdns; keep diag tone"
```

---

## Task 7: On-target verification (proves discovery)

**Files:** none (verification only). No hardware flashing in this workflow — record the steps
for whoever has the board.

- [ ] **Step 1:** Set real creds via `~/.platformio/penv/bin/pio run -e esp32-s3-n16r8 -t menuconfig`
  → *Conduit Stream Configuration* → SSID/password (2.4 GHz), or leave empty to test the
  "no creds → idle, no boot-loop" path. Flash and open the serial log.
- [ ] **Step 2:** Expected log order: boot banner → `audio_*` (Phase 0) → `audio_diag: emitting 440 Hz` →
  `sys_cfg: device id … instance '…@Conduit'` → `wifi: station starting …` →
  `wifi: got IP <addr>` → `mdns: advertising _raop._tcp '…@Conduit' on conduit.local:5000 (10 TXT records)`.
- [ ] **Step 3:** On a Mac on the same LAN: `dns-sd -B _raop._tcp` lists the `…@Conduit` instance;
  `dns-sd -L "<deviceid>@Conduit" _raop._tcp` shows the TXT records. The speaker appears in the
  iPhone/Mac **AirPlay menu** (selecting it won't play — Phase 2/3). The 440 Hz tone still sounds.
- [ ] **Step 4:** Tag the phase complete:

```bash
cd /Users/uziiuzair/ooozzy/conduit-stream
git tag -a v0.2-phase1 -m "Phase 1: Wi-Fi station + mDNS _raop._tcp advertise; speaker discoverable"
```

---

## Self-Review

**Deliverable coverage (Phase 1 goal, 5 items):**
1. `system` NVS config store, Kconfig seed, MAC device id, no-creds idle — Task 3 (+ Task 1 pure formatters). ✓
2. `network` Wi-Fi station, `WIFI_PS_NONE`, events, reconnect backoff, GOT_IP callback, logs IP — Task 4. ✓
3. `mdns` wrapper over managed `espressif/mdns`, `_raop._tcp`, RAOP TXT set, instance `<deviceid>@<name>`, `RAOP_RTSP_PORT=5000` — Task 5 (+ Task 2 pure builder). ✓
4. `main.cpp` orchestration (nvs → config → wifi → GOT_IP → mdns), diag tone + banner kept — Task 6. ✓
5. Host tests for pure logic — device-id/instance-name (Task 1) + RAOP TXT builder (Task 2), ESP-IDF-free files `#include`d directly, mirroring `audio_ringbuf.c`. ✓

**Type/name consistency:** `system_config_init/get_ssid/get_password/get_name/get_device_id/get_instance_name/has_credentials`; `device_id_format`, `device_instance_name_format`; `raop_txt_build`, `raop_txt_item_t`, `RAOP_TXT_COUNT`; `wifi_start`, `wifi_got_ip_cb_t`; `mdns_advertise_raop`, `RAOP_RTSP_PORT` — consistent across every task and the host tests.

**REQUIRES review:** public headers expose only `esp_err_t`/plain C, so all IDF deps are `PRIV_REQUIRES` (system: `nvs_flash esp_hw_support`; network: `esp_wifi esp_event esp_netif esp_timer system`; mdns: `mdns`). Each REQUIRES edit is paired with a build-dir wipe per the CLAUDE.md gotcha.

**Carried-forward Phase 0 item:** `audio` untouched; the same-core `audio_play_pcm` producer
contract is re-documented in `system/README.md` (Task 6). No new audio producer in Phase 1.

**Deferred to later phases (not Phase 1):** RTSP state machine + RSA challenge (Phase 2),
RTP/AES/ALAC → PCM (Phase 3), retransmit/underrun (Phase 4), volume/metadata/RGB LED (Phase 5).
The RTSP port is advertised but unmanned in Phase 1 — expected per the spec.
