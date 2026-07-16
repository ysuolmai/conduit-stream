// Wi-Fi station bring-up. Connects using credentials from system_config,
// disables modem power-save (WIFI_PS_NONE — its latency spikes cause RTP
// jitter; the top ESP32-AirPlay stutter fix), and reconnects with backoff.
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

// Invoked (once per acquisition) from the Wi-Fi event task when an IPv4 address
// is obtained. main uses it to start mDNS. Keep the callback short & non-blocking.
typedef void (*wifi_got_ip_cb_t)(void);

// Bring up station mode and start connecting. `on_got_ip` may be NULL.
// Precondition: nvs_flash_init() and system_config_init() have run, and an SSID
// is present (caller checks system_config_has_credentials()).
void wifi_start(wifi_got_ip_cb_t on_got_ip);

#ifdef __cplusplus
}
#endif
