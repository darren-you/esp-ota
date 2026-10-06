#pragma once
#include <stdint.h>
#define ESP_IMAGE_HEADER_MAGIC 0xE9
#define ESP_CHIP_ID_INVALID 0xffff
typedef struct { uint8_t magic; uint8_t rest[11]; uint16_t chip_id; uint8_t tail[10]; } esp_image_header_t;
typedef struct { uint32_t load_addr, data_len; } esp_image_segment_header_t;
