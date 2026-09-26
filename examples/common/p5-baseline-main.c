// SPDX-License-Identifier: Apache-2.0
#include "eota.h"
#include "sdkconfig.h"

#include <stdbool.h>
#include <stdint.h>

#include "esp_app_desc.h"
#include "esp_app_format.h"
#include "esp_image_format.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_system.h"

#ifndef EOTA_P5_ROLE
#error "P5 baseline role is required"
#endif

static const char *TAG = "p5_baseline";

static const char *state_name(esp_ota_img_states_t state)
{
    switch (state) {
    case ESP_OTA_IMG_NEW: return "new";
    case ESP_OTA_IMG_PENDING_VERIFY: return "pending";
    case ESP_OTA_IMG_VALID: return "valid";
    case ESP_OTA_IMG_INVALID: return "invalid";
    case ESP_OTA_IMG_ABORTED: return "aborted";
    case ESP_OTA_IMG_UNDEFINED: return "undefined";
    default: return "other";
    }
}

static bool verify_slot(uint8_t subtype, uint32_t address, uint32_t size,
                        const esp_partition_t **partition_out,
                        esp_ota_img_states_t *state_out, bool *tracked_out)
{
    const esp_partition_t *partition = esp_partition_find_first(
        ESP_PARTITION_TYPE_APP, subtype, NULL);
    if (partition == NULL || partition->address != address || partition->size != size) {
        ESP_LOGE(TAG, "P5_SLOT subtype=%u geometry=bad", subtype);
        return false;
    }
    const esp_partition_pos_t position = {.offset = address, .size = size};
    esp_image_metadata_t metadata = {0};
    const esp_err_t verify = esp_image_verify(ESP_IMAGE_VERIFY, &position, &metadata);
    esp_ota_img_states_t state = ESP_OTA_IMG_UNDEFINED;
    const esp_err_t state_result = esp_ota_get_state_partition(partition, &state);
    *tracked_out = state_result == ESP_OK;
    *state_out = state;
    *partition_out = partition;
    ESP_LOGI(TAG, "P5_SLOT subtype=%u addr=0x%lx size=0x%lx verify=%s image_len=%lu state=%s",
             subtype, (unsigned long)address, (unsigned long)size,
             esp_err_to_name(verify), (unsigned long)metadata.image_len,
             *tracked_out ? state_name(state) : "untracked");
    return verify == ESP_OK && metadata.image_len > 0 && metadata.image_len <= size &&
           (state_result == ESP_OK || state_result == ESP_ERR_NOT_FOUND);
}

static void select_and_restart(const esp_partition_t *partition, const char *action)
{
    const esp_err_t result = esp_ota_set_boot_partition(partition);
    ESP_LOGI(TAG, "P5_ACTION %s result=%s", action, esp_err_to_name(result));
    if (result == ESP_OK) esp_restart();
}

void app_main(void)
{
#if CONFIG_IDF_TARGET_ESP32C3
    const uint32_t ota0_addr = 0x20000U;
    const uint32_t ota1_addr = 0x200000U;
    const uint32_t ota_size = 0x1e0000U;
#elif CONFIG_IDF_TARGET_ESP32
    const uint32_t ota0_addr = 0x100000U;
    const uint32_t ota1_addr = 0x280000U;
    const uint32_t ota_size = 0x180000U;
#else
#error "Unsupported P5 target"
#endif
    const esp_partition_t *ota0 = NULL;
    const esp_partition_t *ota1 = NULL;
    esp_ota_img_states_t ota0_state = ESP_OTA_IMG_UNDEFINED;
    esp_ota_img_states_t ota1_state = ESP_OTA_IMG_UNDEFINED;
    bool ota0_tracked = false;
    bool ota1_tracked = false;
    ESP_LOGI(TAG, "P5_BOOT role=%u app=%s signed_update=%u",
             EOTA_P5_ROLE, esp_app_get_description()->version, eota_available());
    if (!eota_available() ||
        !verify_slot(ESP_PARTITION_SUBTYPE_APP_OTA_0, ota0_addr, ota_size,
                     &ota0, &ota0_state, &ota0_tracked) ||
        !verify_slot(ESP_PARTITION_SUBTYPE_APP_OTA_1, ota1_addr, ota_size,
                     &ota1, &ota1_state, &ota1_tracked)) return;

    eota_current_t current = {0};
    const esp_err_t inspect = eota_inspect(&current);
    const esp_partition_t *running = esp_ota_get_running_partition();
    const esp_partition_t *boot = esp_ota_get_boot_partition();
    ESP_LOGI(TAG, "P5_CURRENT inspect=%s running=%s boot=%s state=%s",
             esp_err_to_name(inspect), running != NULL ? running->label : "none",
             boot != NULL ? boot->label : "none", eota_state_name(current.state));
    if (inspect != ESP_OK || running == NULL || boot == NULL || (running->address != boot->address || running->size != boot->size)) return;

#if EOTA_P5_ROLE == 0
    if (running != ota0) return;
    if (current.state == EOTA_STATE_UNTRACKED) {
        select_and_restart(ota0, "initialize_ota0");
        return;
    }
    if (current.state == EOTA_STATE_PENDING_VERIFY) {
        if (ota1_tracked && ota1_state == ESP_OTA_IMG_VALID) {
            ESP_LOGI(TAG, "P5_ACTION reject_ota0_for_rollback");
            (void)eota_reject_pending(&current);
        } else {
            const esp_err_t result = eota_confirm_pending(&current);
            ESP_LOGI(TAG, "P5_ACTION confirm_initial_ota0 result=%s state=%s",
                     esp_err_to_name(result), eota_state_name(current.state));
        }
        return;
    }
    if (current.state == EOTA_STATE_VALID &&
        (!ota1_tracked || ota1_state == ESP_OTA_IMG_UNDEFINED)) {
        select_and_restart(ota1, "select_ota1");
    }
#elif EOTA_P5_ROLE == 1
    if (running != ota1) return;
    if (current.state == EOTA_STATE_PENDING_VERIFY) {
        const esp_err_t result = eota_confirm_pending(&current);
        ESP_LOGI(TAG, "P5_ACTION confirm_ota1 result=%s state=%s",
                 esp_err_to_name(result), eota_state_name(current.state));
        return;
    }
    if (current.state == EOTA_STATE_VALID && ota0_tracked &&
        ota0_state == ESP_OTA_IMG_VALID) {
        select_and_restart(ota0, "select_ota0_rollback_trial");
    }
#else
#error "P5 baseline role must be 0 or 1"
#endif
    ESP_LOGI(TAG, "P5_IDLE role=%u", EOTA_P5_ROLE);
}
