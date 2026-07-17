#include "led_state.h"

led_rgb_t led_state_color(system_led_state_t st) {
    switch (st) {
        case LED_ST_NEEDS_CREDS:      return (led_rgb_t){16, 0, 0};   // red
        case LED_ST_WIFI_CONNECTING:  return (led_rgb_t){16, 6, 0};   // amber
        case LED_ST_CONNECTED_IDLE:   return (led_rgb_t){0, 0, 16};   // blue
        case LED_ST_STREAMING:        return (led_rgb_t){0, 16, 0};   // green
        default:                      return (led_rgb_t){0, 0, 0};    // off
    }
}
