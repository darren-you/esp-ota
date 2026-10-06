#pragma once
#include <stdint.h>
#include "esp_err.h"
#define ESP_ERR_IMAGE_FLASH_FAIL 0x2001
#define ESP_ERR_IMAGE_INVALID 0x2002
typedef enum { ESP_IMAGE_VERIFY } esp_image_load_mode_t;
typedef struct { uint32_t offset, size; } esp_partition_pos_t;
typedef struct { uint32_t image_len; } esp_image_metadata_t;
esp_err_t esp_image_verify(esp_image_load_mode_t mode,
                           const esp_partition_pos_t *part,
                           esp_image_metadata_t *metadata);
