# system

Cross-cutting glue: boot sequencing, config/NVS storage, logging setup, LED
status, health. The stuff every other component leans on but none of them owns.

## Config store (Phase 1)

`system_config_init()` loads the device configuration:

- **NVS namespace `conduit`**, keys: `wifi_ssid`, `wifi_pass`, `name`
  (default `"Conduit"`). NVS is the source of truth after first boot.
- **Kconfig seeding** — on first boot each missing key is seeded from
  `CONFIG_CONDUIT_WIFI_SSID` / `CONFIG_CONDUIT_WIFI_PASSWORD` /
  `CONFIG_CONDUIT_DEVICE_NAME` (see `Kconfig.projbuild`, set via `menuconfig`)
  and persisted, so later config changes need no reflash.
- **Device id** — derived from the base MAC at boot (e.g. `E83DC1F2AC6C`);
  **not stored**. The RAOP service instance name is `<deviceid>@<name>`
  (e.g. `E83DC1F2AC6C@Conduit`).
- **No creds** — if `wifi_ssid` is empty, `system_config_has_credentials()`
  returns false; `main` logs one clear line and sits idle (no boot-loop).

Pure string logic (`src/device_id.c`) has zero ESP-IDF deps and is host-tested
under `pio test -e native`; the NVS/MAC glue (`src/system_config.c`) is verified
by the target build.

## Carried-forward note (Phase 0 review item)

`audio_play_pcm()` has a **same-core producer contract** (SPSC ring pinned to
`AUDIO_PIN_CORE`). Phase 1 adds **no** audio producer and must not break it — the
440 Hz diagnostic tone remains the sole producer. A future out-of-component
producer (the RAOP decoder, Phase 3) must respect that contract.
