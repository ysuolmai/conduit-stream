#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "esp_ota_ops.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    const esp_partition_t *partition;
    esp_ota_handle_t handle;
    bool active;
} ota_update_t;

size_t ota_update_max_size(void);
esp_err_t ota_update_validate_image_header(const uint8_t *data, size_t length);
esp_err_t ota_update_begin(ota_update_t *update, size_t image_size);
esp_err_t ota_update_write(ota_update_t *update, const void *data, size_t length);
esp_err_t ota_update_finish(ota_update_t *update);
void ota_update_abort(ota_update_t *update);

#ifdef __cplusplus
}
#endif
