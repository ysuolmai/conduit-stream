#include "ota_update.h"

#include "esp_app_format.h"
#include "esp_log.h"

#include <string.h>

static const char *TAG = "ota_update";

size_t ota_update_max_size(void) {
    const esp_partition_t *partition = esp_ota_get_next_update_partition(NULL);
    return partition ? partition->size : 0;
}

esp_err_t ota_update_validate_image_header(const uint8_t *data, size_t length) {
    const size_t descriptor_offset = sizeof(esp_image_header_t) +
                                     sizeof(esp_image_segment_header_t);
    const size_t required = descriptor_offset + sizeof(esp_app_desc_t);
    if (!data || length < required) return ESP_ERR_INVALID_SIZE;

    const esp_image_header_t *header = (const esp_image_header_t *)data;
    if (header->magic != ESP_IMAGE_HEADER_MAGIC) {
        ESP_LOGE(TAG, "uploaded file is not an ESP application image");
        return ESP_ERR_INVALID_ARG;
    }

    esp_app_desc_t descriptor;
    memcpy(&descriptor, data + descriptor_offset, sizeof(descriptor));
    if (descriptor.magic_word != ESP_APP_DESC_MAGIC_WORD) {
        ESP_LOGE(TAG, "uploaded file has no application descriptor; use an -ota.bin file");
        return ESP_ERR_INVALID_ARG;
    }
    ESP_LOGI(TAG, "incoming app: project='%s' version='%s'", descriptor.project_name,
             descriptor.version);
    return ESP_OK;
}

esp_err_t ota_update_begin(ota_update_t *update, size_t image_size) {
    if (!update || image_size == 0) return ESP_ERR_INVALID_ARG;
    memset(update, 0, sizeof(*update));

    update->partition = esp_ota_get_next_update_partition(NULL);
    if (!update->partition || image_size > update->partition->size) {
        return ESP_ERR_INVALID_SIZE;
    }

    esp_err_t err = esp_ota_begin(update->partition, image_size, &update->handle);
    if (err == ESP_OK) {
        update->active = true;
        ESP_LOGI(TAG, "writing %u bytes to OTA partition '%s'",
                 (unsigned)image_size, update->partition->label);
    }
    return err;
}

esp_err_t ota_update_write(ota_update_t *update, const void *data, size_t length) {
    if (!update || !update->active || !data || length == 0) return ESP_ERR_INVALID_STATE;
    return esp_ota_write(update->handle, data, length);
}

esp_err_t ota_update_finish(ota_update_t *update) {
    if (!update || !update->active) return ESP_ERR_INVALID_STATE;

    esp_err_t err = esp_ota_end(update->handle);
    update->active = false;
    if (err != ESP_OK) return err;

    err = esp_ota_set_boot_partition(update->partition);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "OTA image verified; next boot partition is '%s'",
                 update->partition->label);
    }
    return err;
}

void ota_update_abort(ota_update_t *update) {
    if (update && update->active) {
        esp_ota_abort(update->handle);
        update->active = false;
    }
}
