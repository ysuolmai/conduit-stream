#include "system_config.h"
#include "device_id.h"

#include "nvs.h"
#include "nvs_flash.h"
#include "esp_mac.h"
#include "esp_log.h"
#include <stdio.h>
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
