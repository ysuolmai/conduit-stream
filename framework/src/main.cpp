// Conduit Stream - firmware v0.0.1
// -----------------------------------------------------------------------------
// Goal for this build (per the Vision doc, "Version 0.0.1"):
//   ESP32-S3 boots -> initializes I2S -> feeds a clean 440 Hz test tone through
//   the PCM5102A DAC -> out the DAC's 3.5mm jack -> AUX into the Aura Studio 3.
//
// No networking yet. This is the "hear first audio" milestone (spec steps 1-6).
//
// Signal chain:
//   ESP32-S3 (I2S master) --BCLK/LRCK/DIN--> PCM5102A --line level--> Aura AUX in
//
// Pin map (matches the Vision doc; change GPIO_* below if you re-wire):
//   ESP32 GPIO5 -> PCM5102A DIN   (data out of the ESP32)
//   ESP32 GPIO6 -> PCM5102A BCK   (bit clock)
//   ESP32 GPIO7 -> PCM5102A LRCK  (word/left-right clock, a.k.a. LCK/WS)
//   ESP32 5V    -> PCM5102A VIN   (onboard regulator makes 3.3V for the chip)
//   ESP32 GND   -> PCM5102A GND
//   PCM5102A SCK -> GND  <-- REQUIRED. No master clock is wired, so the DAC must
//                            be told to run its internal PLL by grounding SCK.
//                            Leave SCK floating and you get silence. This is the
//                            #1 gotcha on this board.
//   PCM5102A XSMT (soft mute) -> 3.3V (unmute). Most breakouts pull this high by
//                            default; verify it, because low = muted = silence.
//
// We deliberately do NOT wire MCLK: gpio_cfg.mclk = I2S_GPIO_UNUSED. The PCM5102A
// synthesizes its own clock from BCLK via its internal PLL (that's why SCK->GND).

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_chip_info.h"
#include "esp_flash.h"
#include "esp_log.h"
#include "esp_system.h"
#if CONFIG_SPIRAM
#include "esp_psram.h"
#endif

#include "audio.h"
#include "nvs_flash.h"
#include "system_config.h"
#include "system_led.h"
#include "setup_button.h"
#include "wifi.h"
#include "mdns_service.h"
#include "raop.h"
#include "udp_log.h"   // DEBUG: mirror logs over UDP (serial console is unreliable)

#include <atomic>

static const char *TAG = "conduit";
static bool s_led_off_task_started = false;
static std::atomic_bool s_wifi_connected{false};

#define WIFI_CONNECT_TIMEOUT_MS 20000

static void led_off_task(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(5000));
    system_led_disable();
    vTaskDelete(NULL);
}

static void wifi_fallback_task(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(WIFI_CONNECT_TIMEOUT_MS));
    if (!s_wifi_connected.load(std::memory_order_relaxed)) {
        ESP_LOGW(TAG, "no IP after %d ms; rebooting into setup portal",
                 WIFI_CONNECT_TIMEOUT_MS);
        esp_err_t err = system_config_request_setup_mode();
        if (err == ESP_OK) {
            vTaskDelay(pdMS_TO_TICKS(100));
            esp_restart();
        }
        ESP_LOGE(TAG, "could not request setup portal: %s", esp_err_to_name(err));
    }
    vTaskDelete(NULL);
}

// -----------------------------------------------------------------------------
static void log_boot_banner(void)
{
    ESP_LOGI(TAG, "========================================");
    ESP_LOGI(TAG, " Conduit Stream  |  firmware v0.0.1");
    ESP_LOGI(TAG, " target: hear a clean 440 Hz test tone");
    ESP_LOGI(TAG, "========================================");

    // ---- Chip ----
    esp_chip_info_t chip;
    esp_chip_info(&chip);
    ESP_LOGI(TAG, "chip: %s, %d core(s), silicon rev v%d.%d",
             CONFIG_IDF_TARGET, chip.cores,
             chip.revision / 100, chip.revision % 100);
    ESP_LOGI(TAG, "features:%s%s%s%s",
             (chip.features & CHIP_FEATURE_WIFI_BGN) ? " WiFi" : "",
             (chip.features & CHIP_FEATURE_BT)       ? " BT"   : "",
             (chip.features & CHIP_FEATURE_BLE)      ? " BLE"  : "",
             (chip.features & CHIP_FEATURE_IEEE802154) ? " 802.15.4" : "");

    // ---- Flash (expect 16 MB on the N16R8) ----
    uint32_t flash_bytes = 0;
    if (esp_flash_get_size(NULL, &flash_bytes) == ESP_OK) {
        ESP_LOGI(TAG, "flash: %.1f MB", flash_bytes / (1024.0 * 1024.0));
    } else {
        ESP_LOGW(TAG, "flash: size read failed");
    }

    // ---- PSRAM (expect ~8 MB octal on the N16R8) ----
#if CONFIG_SPIRAM
    size_t psram_bytes = esp_psram_get_size();
    ESP_LOGI(TAG, "psram: %.1f MB (octal)", psram_bytes / (1024.0 * 1024.0));
#else
    ESP_LOGW(TAG, "psram: not enabled in sdkconfig (CONFIG_SPIRAM off)");
#endif

    ESP_LOGI(TAG, "free heap at boot: %lu bytes",
             (unsigned long) esp_get_free_heap_size());
}

// -----------------------------------------------------------------------------
// Status-LED transition for RAOP session events (spec §7). Fired from the RTSP
// task: RECORD -> STREAMING (green), TEARDOWN / idle-reclaim -> CONNECTED_IDLE (blue).
static void on_raop_event(raop_event_t ev)
{
    system_led_set_state(ev == RAOP_EV_STREAMING ? LED_ST_STREAMING
                                                  : LED_ST_CONNECTED_IDLE);
}

// -----------------------------------------------------------------------------
// Fires from the Wi-Fi event task once an IPv4 address is up. esp_netif is ready
// now, so it is safe to advertise. Hostname "conduit" -> conduit.local.
static void on_got_ip(void)
{
    s_wifi_connected.store(true, std::memory_order_relaxed);
    udp_log_init();   // DEBUG: start mirroring logs over UDP now that we have an IP
    system_led_set_state(LED_ST_CONNECTED_IDLE);   // Wi-Fi up, no stream yet (blue)
    mdns_advertise_raop("conduit", system_config_get_instance_name(), RAOP_RTSP_PORT);
    // Phase 2: now actually man the advertised RTSP port so a sender can connect
    // and negotiate (OPTIONS -> ANNOUNCE -> SETUP -> RECORD -> Phase 3 audio).
    raop_set_event_cb(on_raop_event);   // LED reflects streaming/idle (register once)
    raop_server_start();
    if (!s_led_off_task_started) {
        s_led_off_task_started = true;
        if (xTaskCreate(led_off_task, "led_off", 2048, NULL, 2, NULL) != pdPASS) {
            ESP_LOGW(TAG, "could not schedule status LED shutdown");
            s_led_off_task_started = false;
        }
    }
}

// -----------------------------------------------------------------------------
extern "C" void app_main(void)
{
    log_boot_banner();
    audio_init();               // I2S + PSRAM ring + playback task (Phase 0)
    // audio_diag_tone_start();  // DEBUG: diag tone disabled to isolate RAOP audio-path streaming

    // nvs_flash_init() can return ESP_ERR_NVS_NO_FREE_PAGES / NEW_VERSION_FOUND
    // after a partition-table change; erase + retry so we never brick on that.
    esp_err_t nvs_err = nvs_flash_init();
    if (nvs_err == ESP_ERR_NVS_NO_FREE_PAGES || nvs_err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        nvs_err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(nvs_err);
    ESP_ERROR_CHECK(system_config_init());
    const bool setup_requested = system_config_take_setup_mode_request();

    // Status LED (spec §7). Guarded: a missing/unwired WS2812 never crashes boot.
    system_led_init();

    if (system_config_has_credentials() && !setup_requested) {
        system_led_set_state(LED_ST_WIFI_CONNECTING);   // amber while connecting
        ESP_ERROR_CHECK(setup_button_start());
        wifi_start(on_got_ip);  // on GOT_IP -> LED blue + mdns_advertise_raop(...)
        if (xTaskCreate(wifi_fallback_task, "wifi_fallback", 2048, NULL, 3, NULL) != pdPASS) {
            ESP_LOGE(TAG, "could not start Wi-Fi fallback timer");
        }
    } else {
        system_led_set_state(LED_ST_NEEDS_CREDS);
        if (setup_requested) {
            ESP_LOGI(TAG, "BOOT setup request; keeping saved Wi-Fi until new settings are saved");
        } else {
            ESP_LOGW(TAG, "no Wi-Fi credentials; starting captive setup portal");
        }
        wifi_start_provisioning();
    }

    ESP_LOGI(TAG, "boot complete.");
}
