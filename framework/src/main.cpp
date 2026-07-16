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

#include <math.h>
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

#include "driver/i2s_std.h"

static const char *TAG = "conduit";

// ---- Audio config -----------------------------------------------------------
#define I2S_DOUT_GPIO   GPIO_NUM_5   // -> PCM5102A DIN
#define I2S_BCLK_GPIO   GPIO_NUM_6   // -> PCM5102A BCK
#define I2S_LRCK_GPIO   GPIO_NUM_7   // -> PCM5102A LRCK (LCK/WS)

#define SAMPLE_RATE_HZ  44100
#define TONE_HZ         440.0f
#define AMPLITUDE       (0.60f * 32767.0f)  // headroom below full scale, no clip

// Frames per I2S write. One "frame" = one left sample + one right sample.
#define FRAMES_PER_CHUNK 256

static i2s_chan_handle_t s_tx_chan = NULL;

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
static void i2s_init(void)
{
    // Allocate a TX channel. I2S_NUM_AUTO lets the driver pick a free port.
    i2s_chan_config_t chan_cfg =
        I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_AUTO, I2S_ROLE_MASTER);
    ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, &s_tx_chan, NULL));

    // Standard (Philips) I2S. 16-bit stereo is plenty for a test tone and keeps
    // the buffer math trivial. Philips format lines up with the PCM5102A when its
    // FMT config pad is set Low (the default on most breakouts).
    i2s_std_config_t std_cfg = {
        .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(SAMPLE_RATE_HZ),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(
                        I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,   // no MCLK wired -> PCM5102A uses SCK->GND PLL
            .bclk = I2S_BCLK_GPIO,
            .ws   = I2S_LRCK_GPIO,
            .dout = I2S_DOUT_GPIO,
            .din  = I2S_GPIO_UNUSED,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv   = false,
            },
        },
    };
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(s_tx_chan, &std_cfg));
    ESP_ERROR_CHECK(i2s_channel_enable(s_tx_chan));

    ESP_LOGI(TAG, "i2s: std TX up @ %d Hz, 16-bit stereo "
                  "(BCLK=%d, LRCK=%d, DOUT=%d, MCLK=unused)",
             SAMPLE_RATE_HZ, I2S_BCLK_GPIO, I2S_LRCK_GPIO, I2S_DOUT_GPIO);
}

// -----------------------------------------------------------------------------
// Continuous-phase sine. We keep a running phase across chunks so there is never
// a discontinuity at the buffer boundary (a per-buffer reset would click, since
// 44100/440 is not a whole number of samples).
static void tone_task(void *arg)
{
    static int16_t buf[FRAMES_PER_CHUNK * 2];  // interleaved L,R
    const float phase_inc = 2.0f * (float) M_PI * TONE_HZ / (float) SAMPLE_RATE_HZ;
    float phase = 0.0f;

    ESP_LOGI(TAG, "tone: emitting %.0f Hz sine (both channels)", TONE_HZ);

    while (true) {
        for (int i = 0; i < FRAMES_PER_CHUNK; ++i) {
            int16_t s = (int16_t) (AMPLITUDE * sinf(phase));
            buf[2 * i]     = s;   // left
            buf[2 * i + 1] = s;   // right
            phase += phase_inc;
            if (phase >= 2.0f * (float) M_PI) {
                phase -= 2.0f * (float) M_PI;
            }
        }
        size_t written = 0;
        esp_err_t err = i2s_channel_write(s_tx_chan, buf, sizeof(buf),
                                          &written, portMAX_DELAY);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "i2s write failed: %s", esp_err_to_name(err));
            vTaskDelay(pdMS_TO_TICKS(100));
        }
    }
}

// -----------------------------------------------------------------------------
extern "C" void app_main(void)
{
    log_boot_banner();
    i2s_init();
    xTaskCreate(tone_task, "tone", 4096, NULL, 5, NULL);
    ESP_LOGI(TAG, "boot complete. if the Aura is silent, check SCK->GND and XSMT->3.3V.");
}
