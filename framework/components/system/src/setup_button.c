#include "setup_button.h"
#include "system_config.h"

#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define BOOT_BUTTON_GPIO GPIO_NUM_0
#define POLL_INTERVAL_MS 50
#define LONG_PRESS_MS 3000

static const char *TAG = "setup_button";
static bool s_started;

static void setup_button_task(void *arg) {
    (void)arg;
    bool armed = false;
    TickType_t pressed_at = 0;

    while (true) {
        bool pressed = gpio_get_level(BOOT_BUTTON_GPIO) == 0;
        if (!armed) {
            if (!pressed) armed = true;
        } else if (!pressed) {
            pressed_at = 0;
        } else if (pressed_at == 0) {
            pressed_at = xTaskGetTickCount();
        } else if ((xTaskGetTickCount() - pressed_at) >= pdMS_TO_TICKS(LONG_PRESS_MS)) {
            ESP_LOGI(TAG, "BOOT held for %d ms; entering setup portal", LONG_PRESS_MS);
            esp_err_t err = system_config_request_setup_mode();
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "could not request setup mode: %s", esp_err_to_name(err));
                pressed_at = 0;
                while (gpio_get_level(BOOT_BUTTON_GPIO) == 0) {
                    vTaskDelay(pdMS_TO_TICKS(POLL_INTERVAL_MS));
                }
            } else {
                vTaskDelay(pdMS_TO_TICKS(100));
                esp_restart();
            }
        }
        vTaskDelay(pdMS_TO_TICKS(POLL_INTERVAL_MS));
    }
}

esp_err_t setup_button_start(void) {
    if (s_started) return ESP_OK;

    gpio_config_t config = {
        .pin_bit_mask = 1ULL << BOOT_BUTTON_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    esp_err_t err = gpio_config(&config);
    if (err != ESP_OK) return err;

    // NVS commit is performed from this task after a long press. Keep enough
    // stack for the NVS/flash call chain; 2 KB can reset before the flag lands.
    if (xTaskCreate(setup_button_task, "setup_button", 4096, NULL, 3, NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    s_started = true;
    return ESP_OK;
}
