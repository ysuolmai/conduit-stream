# system

Cross-cutting glue: boot sequencing, config/NVS storage, logging setup, LED
status, health. The stuff every other component leans on but none of them owns.

## Config store (Phase 1)

`system_config_init()` loads the device configuration:

- **NVS namespace `conduit`**, keys: `wifi_ssid`, `wifi_pass`, `name`.
  NVS is the source of truth for Wi-Fi after first boot.
- **Kconfig seeding** — on first boot each missing key is seeded from
  `CONFIG_CONDUIT_WIFI_SSID` / `CONFIG_CONDUIT_WIFI_PASSWORD` and persisted.
- **Device id** — derived from the base MAC at boot (e.g. `E83DC1F2AC6C`);
  **not stored**. The visible name is always `MiniSpeaker-XXX`, using the final
  three MAC digits. The RAOP instance is `<deviceid>@<name>` (for example,
  `E83DC1F2AC6C@MiniSpeaker-C6C`).
- **No creds** — the device starts the open `MiniSpeaker-XXX` captive setup AP.
  The submitted SSID and password are saved to NVS before restart.

Pure string logic (`src/device_id.c`) has zero ESP-IDF deps and is host-tested
under `pio test -e native`; the NVS/MAC glue (`src/system_config.c`) is verified
by the target build.

## RGB status LED (Phase 5, spec §7)

The onboard WS2812 on **GPIO48** (ESP32-S3-DevKitC-1) reflects device state via the
`espressif/led_strip` managed component (RMT backend, declared in `idf_component.yml`):

| state | colour | when |
|---|---|---|
| `LED_ST_NEEDS_CREDS` | red | boot, no Wi-Fi credentials |
| `LED_ST_WIFI_CONNECTING` | amber | station connecting |
| `LED_ST_CONNECTED_IDLE` | blue | Wi-Fi up (GOT_IP), no live stream |
| `LED_ST_STREAMING` | green | RAOP `RECORD` live |

After the station receives an IP address, the programmable GPIO48 LED remains
blue for five seconds and is then cleared and disabled until reboot. This avoids
continuous distraction and prevents the companion LED found on some Super Mini
boards from flickering when WS2812 data is sent.

- `src/led_state.{c}` + `include/led_state.h` — **pure** state→(r,g,b) map (brightness
  ≤16; status indicator, not lighting), host-tested (`test/test_led_state`).
- `src/system_led.c` + `include/system_led.h` — **guarded** led_strip glue:
  `system_led_init()` / `system_led_set_state()`. If `led_strip_new_rmt_device` fails
  (LED not wired, wrong board revision, RMT unavailable) the handle stays `NULL` and
  every set becomes a logged no-op — **it never crashes boot**.
- GPIO is a compile-time `#define STATUS_LED_GPIO 48` (a few DevKitC-1 revisions route
  it to 38); a wrong pin is a silently-dark, non-crashing LED.
- `main` drives the transitions (NEEDS_CREDS/CONNECTING at boot, CONNECTED_IDLE on
  GOT_IP); the RAOP event callback drives STREAMING/IDLE. All callers are low-rate
  tasks — never the audio drain/ISR (`led_strip_refresh` blocks ~30 µs on RMT).

## Carried-forward note (Phase 0 review item)

`audio_play_pcm()` has a **same-core producer contract** (SPSC ring pinned to
`AUDIO_PIN_CORE`). Phase 1 adds **no** audio producer and must not break it — the
440 Hz diagnostic tone remains the sole producer. A future out-of-component
producer (the RAOP decoder, Phase 3) must respect that contract.
