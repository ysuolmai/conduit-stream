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

#include <stdio.h>
#include <string.h>

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

static const char *TAG = "conduit";

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
extern "C" void app_main(void)
{
    log_boot_banner();
    audio_init();               // I2S + PSRAM ring + playback task
    audio_diag_tone_start();    // 440 Hz through the new path (diagnostic)
    ESP_LOGI(TAG, "boot complete. if the Aura is silent, check SCK->GND and XSMT->3.3V.");
}
