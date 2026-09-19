#include "audio_i2s.h"

#include "driver/i2s_std.h"
#include "freertos/FreeRTOS.h"
#include "esp_log.h"

#define I2S_DOUT_GPIO ((gpio_num_t)CONFIG_CONDUIT_I2S_DOUT_GPIO)
#define I2S_BCLK_GPIO ((gpio_num_t)CONFIG_CONDUIT_I2S_BCLK_GPIO)
#define I2S_LRCK_GPIO ((gpio_num_t)CONFIG_CONDUIT_I2S_LRCK_GPIO)

static const char *TAG = "audio_i2s";
static i2s_chan_handle_t s_tx = NULL;

void audio_i2s_init(void) {
    i2s_chan_config_t chan_cfg =
        I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_AUTO, I2S_ROLE_MASTER);
    ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, &s_tx, NULL));

    i2s_std_config_t std_cfg = {
        .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(AUDIO_SAMPLE_RATE_HZ),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(
                        I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,   // MAX98357A does not need MCLK
            .bclk = I2S_BCLK_GPIO,
            .ws   = I2S_LRCK_GPIO,
            .dout = I2S_DOUT_GPIO,
            .din  = I2S_GPIO_UNUSED,
            .invert_flags = { .mclk_inv = false, .bclk_inv = false, .ws_inv = false },
        },
    };
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(s_tx, &std_cfg));
    ESP_ERROR_CHECK(i2s_channel_enable(s_tx));

    ESP_LOGI(TAG, "i2s: std TX up @ %d Hz, 16-bit %s "
                  "(BCLK=%d, LRCK=%d, DOUT=%d, MCLK=unused)",
             AUDIO_SAMPLE_RATE_HZ,
#ifdef CONFIG_CONDUIT_MONO_OUTPUT
             "mono-mix",
#else
             "stereo",
#endif
             (int)I2S_BCLK_GPIO, (int)I2S_LRCK_GPIO, (int)I2S_DOUT_GPIO);
}

size_t audio_i2s_write(const int16_t *frames, size_t n_frames) {
    size_t bytes = 0;
    esp_err_t err = i2s_channel_write(s_tx, frames,
                                      n_frames * 2 * sizeof(int16_t),
                                      &bytes, portMAX_DELAY);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2s write failed: %s", esp_err_to_name(err));
        return 0;
    }
    return bytes / (2 * sizeof(int16_t));
}
