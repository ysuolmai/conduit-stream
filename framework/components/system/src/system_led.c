#include "system_led.h"
#include "led_strip.h"
#include "esp_log.h"

// DevKitC-1 onboard WS2812. A few early/variant revisions route it to GPIO38;
// change this #define if the status LED stays dark on your board (it fails safe:
// a wrong pin is a silently-dark, non-crashing LED — see the guard in _init).
#define STATUS_LED_GPIO 48

static const char *TAG = "led";
static led_strip_handle_t s_led = NULL;

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
    if (!s_led) return;   // guarded no-op (init failed / no LED)
    led_rgb_t c = led_state_color(st);
    led_strip_set_pixel(s_led, 0, c.r, c.g, c.b);
    led_strip_refresh(s_led);
}
