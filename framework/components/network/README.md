# network

Wi-Fi provisioning and the transport plugins (AirPlay receiver, REST API, later
Bluetooth / DLNA). Each transport is a plugin that hands decoded PCM to the
Playback Manager in `audio`.

## Wi-Fi station (Phase 1)

`wifi_start(on_got_ip)` brings up station mode using the credentials from
`system_config` (precondition: `nvs_flash_init()` + `system_config_init()` have
run and an SSID is present):

- **2.4 GHz station** — the ESP32-S3 has no 5 GHz radio.
- **`WIFI_PS_NONE`** — modem power-save is disabled; its latency spikes are the
  single most common cause of AirPlay RTP stutter on ESP32 (spec §7).
- **Events** (via `esp_event`): `WIFI_EVENT_STA_START` -> connect;
  `WIFI_EVENT_STA_DISCONNECTED` -> reconnect with a capped exponential backoff
  (500 ms doubling to 8 s); `IP_EVENT_STA_GOT_IP` -> log the IP, reset backoff,
  and fire the registered `wifi_got_ip_cb_t` (main uses it to start mDNS).
- **Boot fallback** — if the initial connection has not obtained an IPv4 address
  within 20 seconds, main stores a one-shot setup request and reboots. The next
  boot opens the captive SoftAP without erasing the saved credentials.

The RAOP RTSP/RTP transport plugin is still to come (Phase 2+).
