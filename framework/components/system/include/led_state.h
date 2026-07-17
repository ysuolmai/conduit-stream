// Pure map from status-LED state to an (r,g,b) triple (spec §7). No ESP-IDF deps ->
// host-unit-testable. Brightness kept low (<=16) — the onboard WS2812 is bright and
// this is a status indicator, not lighting. Lives in include/ so the public
// system_led.h can re-export the state enum to main without exposing the driver glue.
#pragma once
#include <stdint.h>

typedef enum {
    LED_ST_NEEDS_CREDS = 0,   // boot, no Wi-Fi credentials
    LED_ST_WIFI_CONNECTING,   // station connecting
    LED_ST_CONNECTED_IDLE,    // Wi-Fi up (GOT_IP), no live stream
    LED_ST_STREAMING,         // RAOP RECORD live
} system_led_state_t;

typedef struct { uint8_t r, g, b; } led_rgb_t;

// Total, deterministic; an out-of-range state maps to off (0,0,0).
led_rgb_t led_state_color(system_led_state_t st);
