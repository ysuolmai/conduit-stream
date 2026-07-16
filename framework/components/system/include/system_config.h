// System config store. Namespace "conduit" in NVS: wifi_ssid, wifi_pass, name.
// Seeded from Kconfig on first boot, then NVS is source of truth. The device id
// is derived from the base MAC at boot (not stored). Getters return pointers to
// internal cached strings, valid after a successful system_config_init().
#pragma once

#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// Load config: derive device id from MAC, open NVS, seed missing keys from
// Kconfig, cache ssid/pass/name + instance name. Returns ESP_OK on success.
esp_err_t   system_config_init(void);

const char *system_config_get_ssid(void);          // "" if unset
const char *system_config_get_password(void);       // "" if unset
const char *system_config_get_name(void);           // e.g. "Conduit"
const char *system_config_get_device_id(void);      // e.g. "E83DC1F2AC6C"
const char *system_config_get_instance_name(void);  // "E83DC1F2AC6C@Conduit"

// True only when an SSID is present — main uses this to decide whether to bring
// Wi-Fi up or log the "no creds" message and sit idle (no boot-loop).
bool        system_config_has_credentials(void);

#ifdef __cplusplus
}
#endif
