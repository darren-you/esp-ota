#pragma once
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#define ESP_PARTITION_TYPE_APP 0
#define ESP_PARTITION_SUBTYPE_APP_OTA_0 0x10
#define ESP_PARTITION_SUBTYPE_APP_OTA_1 0x11
typedef struct {
    int type, subtype;
    uint32_t address, size;
} esp_partition_t;
esp_err_t esp_partition_read(const esp_partition_t *partition, size_t offset, void *data, size_t size);
