#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// Monitor the active-low BOOT button on GPIO0. After the button has first been
// released, holding it for three seconds requests the setup portal and reboots.
esp_err_t setup_button_start(void);

#ifdef __cplusplus
}
#endif
