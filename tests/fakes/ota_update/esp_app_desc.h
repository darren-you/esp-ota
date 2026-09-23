#pragma once
#include <stdint.h>
#define ESP_APP_DESC_MAGIC_WORD 0xABCD5432
typedef struct {
    uint32_t magic_word;
    uint8_t reserved[44];
    char project_name[32];
    uint8_t tail[176];
} esp_app_desc_t;
