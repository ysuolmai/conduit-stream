#include "system_config.h"
#include "device_id.h"

#include "nvs.h"
#include "nvs_flash.h"
#include "esp_mac.h"
#include "esp_log.h"
#include <stdio.h>
#include <string.h>

#define NS "conduit"
#define KEY_SETUP_ONCE "setup_once"

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

    char generated_name[sizeof(s_name)];
    snprintf(generated_name, sizeof(generated_name), "MiniSpeaker-%s", s_device_id + 9);
    load_or_seed(h, "name", generated_name, s_name, sizeof(s_name));
    if (strcmp(s_name, generated_name) != 0) {
        snprintf(s_name, sizeof(s_name), "%s", generated_name);
        if (nvs_set_str(h, "name", s_name) == ESP_OK) nvs_commit(h);
        ESP_LOGI(TAG, "updated device name to '%s'", s_name);
    }
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

esp_err_t system_config_save_wifi(const char *ssid, const char *password) {
    if (!ssid || !password || ssid[0] == '\0' ||
        strlen(ssid) > 32 || strlen(password) > 63) {
        return ESP_ERR_INVALID_ARG;
    }

    nvs_handle_t h;
    esp_err_t err = nvs_open(NS, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;

    if ((err = nvs_set_str(h, "wifi_ssid", ssid)) == ESP_OK &&
        (err = nvs_set_str(h, "wifi_pass", password)) == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);

    if (err == ESP_OK) {
        snprintf(s_ssid, sizeof(s_ssid), "%s", ssid);
        snprintf(s_pass, sizeof(s_pass), "%s", password);
        ESP_LOGI(TAG, "saved Wi-Fi credentials for SSID '%s'", s_ssid);
    }
    return err;
}

esp_err_t system_config_request_setup_mode(void) {
    nvs_handle_t h;
    esp_err_t err = nvs_open(NS, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;

    err = nvs_set_u8(h, KEY_SETUP_ONCE, 1);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err;
}

bool system_config_take_setup_mode_request(void) {
    nvs_handle_t h;
    esp_err_t err = nvs_open(NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "could not read setup request: %s", esp_err_to_name(err));
        return false;
    }

    uint8_t requested = 0;
    err = nvs_get_u8(h, KEY_SETUP_ONCE, &requested);
    if (err == ESP_OK && requested == 1) {
        esp_err_t erase_err = nvs_erase_key(h, KEY_SETUP_ONCE);
        if (erase_err == ESP_OK) erase_err = nvs_commit(h);
        if (erase_err != ESP_OK) {
            ESP_LOGW(TAG, "could not consume setup request: %s",
                     esp_err_to_name(erase_err));
        }
    } else if (err != ESP_ERR_NVS_NOT_FOUND && err != ESP_OK) {
        ESP_LOGW(TAG, "could not read setup request: %s", esp_err_to_name(err));
    }
    nvs_close(h);
    return err == ESP_OK && requested == 1;
}
