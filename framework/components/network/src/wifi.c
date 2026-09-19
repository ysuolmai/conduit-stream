#include "wifi.h"
#include "system_config.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_log.h"
#include <stdio.h>
#include <string.h>

static const char *TAG = "wifi";

static wifi_got_ip_cb_t s_on_got_ip = NULL;
static esp_timer_handle_t s_reconnect_timer = NULL;
static uint32_t s_backoff_ms = 500;   // grows to a cap on repeated failures
static bool s_ready_callback_started = false;

#define BACKOFF_MAX_MS 8000

static void reconnect_cb(void *arg) { esp_wifi_connect(); }

static void ready_callback_task(void *arg) {
    (void)arg;
    if (s_on_got_ip) s_on_got_ip();
    vTaskDelete(NULL);
}

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
        // mDNS and the AirPlay server need much more stack than ESP-IDF's
        // sys_evt task. Dispatch once onto a dedicated task so GOT_IP cannot
        // overflow the system event stack and reboot the device.
        if (s_on_got_ip && !s_ready_callback_started) {
            s_ready_callback_started = true;
            if (xTaskCreate(ready_callback_task, "network_ready", 8192,
                            NULL, 5, NULL) != pdPASS) {
                s_ready_callback_started = false;
                ESP_LOGE(TAG, "could not start network-ready task");
            }
        }
    }
}

void wifi_start(wifi_got_ip_cb_t on_got_ip) {
    s_on_got_ip = on_got_ip;
    s_ready_callback_started = false;

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
