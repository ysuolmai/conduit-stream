#include "system_led.h"
#include "driver/gpio.h"
#include "led_strip.h"
#include "esp_log.h"
#include <stdbool.h>

// On the ESP32-S3 Super Mini, the WS2812 data input and a discrete red LED share
// GPIO48. WS2812 waveforms can visibly flicker the red LED, so disable() deletes
// the RMT device and holds the pin low after the short boot/status period.
#define STATUS_LED_GPIO GPIO_NUM_48

static const char *TAG = "led";
static led_strip_handle_t s_led = NULL;
static bool s_disabled = false;
static int s_last_state = -1;

void system_led_init(void) {
    if (s_led) return;   // idempotent
    led_strip_config_t sc = {
        .strip_gpio_num = STATUS_LED_GPIO,
        .max_leds = 1,
        .led_model = LED_MODEL_WS2812,
        .color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_GRB,
        .flags = { .invert_out = false },
    };
    led_strip_rmt_config_t rc = {
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = 10 * 1000 * 1000,   // 10 MHz
        .mem_block_symbols = 64,
        .flags = { .with_dma = false },      // single LED: no DMA
    };
    esp_err_t e = led_strip_new_rmt_device(&sc, &rc, &s_led);
    if (e != ESP_OK) {
        s_led = NULL;
        ESP_LOGW(TAG, "LED init failed (%s); status LED disabled", esp_err_to_name(e));
        return;
    }
    led_strip_clear(s_led);   // start dark
    ESP_LOGI(TAG, "status LED up on GPIO%d (WS2812)", STATUS_LED_GPIO);
}

void system_led_set_state(system_led_state_t st) {
    if (!s_led || s_disabled || s_last_state == (int)st) return;
    led_rgb_t c = led_state_color(st);
    led_strip_set_pixel(s_led, 0, c.r, c.g, c.b);
    led_strip_refresh(s_led);
    s_last_state = (int)st;
}

void system_led_disable(void) {
    if (!s_led || s_disabled) return;
    s_disabled = true;
    led_strip_clear(s_led);
    led_strip_del(s_led);
    s_led = NULL;

    gpio_reset_pin(STATUS_LED_GPIO);
    gpio_set_direction(STATUS_LED_GPIO, GPIO_MODE_OUTPUT);
    gpio_set_level(STATUS_LED_GPIO, 0);
    ESP_LOGI(TAG, "GPIO48 RGB/red LEDs disabled and held low");
}
