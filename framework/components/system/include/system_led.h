// Status LED (spec §7): drives the ESP32-S3-DevKitC-1 onboard WS2812 on GPIO48 via
// the espressif/led_strip managed component. Re-exports the pure state enum from
// led_state.h so callers (main, the raop event cb) speak in states, not colours.
#pragma once
#include "led_state.h"     // re-export system_led_state_t

#ifdef __cplusplus
extern "C" {
#endif

// Init the onboard WS2812. GUARDED: if the LED / RMT is unavailable (not wired,
// wrong board revision, RMT channel exhausted) the handle stays NULL and every
// system_led_set_state() becomes a logged no-op — it NEVER crashes the receiver.
// Idempotent; safe to call once from the boot task.
void system_led_init(void);

// Set the current status state. No-op (not a crash) if init failed. Called from
// low-rate tasks only (boot, Wi-Fi event, RTSP) — never from the audio drain/ISR.
void system_led_set_state(system_led_state_t st);

// Turn the programmable LED off and ignore all later state updates. This also
// prevents the GPIO48 companion LED from flickering on shared-data boards.
void system_led_disable(void);

#ifdef __cplusplus
}
#endif
