#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_app_desc.h"
#include "esp_app_format.h"
#include "esp_partition.h"
typedef uint32_t esp_ota_handle_t;
typedef enum {
    ESP_OTA_IMG_NEW,
    ESP_OTA_IMG_PENDING_VERIFY,
    ESP_OTA_IMG_VALID,
    ESP_OTA_IMG_INVALID,
    ESP_OTA_IMG_ABORTED,
    ESP_OTA_IMG_UNDEFINED = 0xFFFFFFFFU,
} esp_ota_img_states_t;
#define ESP_ERR_OTA_VALIDATE_FAILED 0x1503
const esp_partition_t *esp_ota_get_running_partition(void);
const esp_partition_t *esp_ota_get_boot_partition(void);
const esp_partition_t *esp_ota_get_next_update_partition(const esp_partition_t *partition);
esp_err_t esp_ota_get_state_partition(const esp_partition_t *partition, esp_ota_img_states_t *state);
esp_err_t esp_ota_check_image_validity(int type, const esp_image_header_t *header, const esp_app_desc_t *desc);
esp_err_t esp_ota_begin(const esp_partition_t *partition, size_t size, esp_ota_handle_t *handle);
esp_err_t esp_ota_write(esp_ota_handle_t handle, const void *data, size_t size);
esp_err_t esp_ota_end(esp_ota_handle_t handle);
esp_err_t esp_ota_abort(esp_ota_handle_t handle);
esp_err_t esp_ota_set_boot_partition(const esp_partition_t *partition);
bool esp_ota_check_rollback_is_possible(void);
