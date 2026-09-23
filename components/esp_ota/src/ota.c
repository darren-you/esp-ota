#include "eota.h"

#include <stddef.h>

#include "esp_log.h"
#include "esp_ota_ops.h"

#if defined(ESP_PLATFORM) && !CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE
#error "ESP OTA pending-slot confirmation requires bootloader rollback"
#endif

#if defined(ESP_PLATFORM) && defined(CONFIG_BOOTLOADER_APP_ANTI_ROLLBACK)
#error "ESP OTA does not support eFuse anti-rollback"
#endif

static const char *TAG = "eota";

esp_err_t eota_inspect(eota_current_t *ota)
{
    if (ota == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    ota->running_partition = NULL;
    ota->state = EOTA_STATE_UNKNOWN;

    const esp_partition_t *running = esp_ota_get_running_partition();
    if (running == NULL) {
        return ESP_ERR_NOT_FOUND;
    }
    ota->running_partition = running->label;

    esp_ota_img_states_t state;
    const esp_err_t result = esp_ota_get_state_partition(running, &state);
    if (result == ESP_ERR_NOT_FOUND) {
        ota->state = EOTA_STATE_UNTRACKED;
        return ESP_OK;
    }
    if (result != ESP_OK) {
        return result;
    }
    ota->state = state == ESP_OTA_IMG_PENDING_VERIFY ? EOTA_STATE_PENDING_VERIFY :
                 state == ESP_OTA_IMG_VALID ? EOTA_STATE_VALID :
                 state == ESP_OTA_IMG_NEW ? EOTA_STATE_NEW :
                 state == ESP_OTA_IMG_UNDEFINED ? EOTA_STATE_UNDEFINED :
                 state == ESP_OTA_IMG_INVALID ? EOTA_STATE_INVALID :
                 state == ESP_OTA_IMG_ABORTED ? EOTA_STATE_ABORTED : EOTA_STATE_OTHER;
    return ESP_OK;
}

const char *eota_state_name(eota_state_t state)
{
    switch (state) {
    case EOTA_STATE_UNTRACKED: return "untracked";
    case EOTA_STATE_PENDING_VERIFY: return "pending_verify";
    case EOTA_STATE_VALID: return "valid";
    case EOTA_STATE_NEW: return "new";
    case EOTA_STATE_UNDEFINED: return "undefined";
    case EOTA_STATE_INVALID: return "invalid";
    case EOTA_STATE_ABORTED: return "aborted";
    case EOTA_STATE_OTHER: return "other";
    default: return "unknown";
    }
}

esp_err_t eota_reject_pending(eota_current_t *ota)
{
    if (ota == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    const esp_err_t current_result = eota_inspect(ota);
    if (current_result != ESP_OK) return current_result;
    if (ota->state != EOTA_STATE_PENDING_VERIFY) {
        return ESP_ERR_INVALID_STATE;
    }
    const esp_err_t result = esp_ota_mark_app_invalid_rollback_and_reboot();
    /* A successful SDK call reboots. A returned error may follow a partial
     * otadata write, so observe the durable state before reporting it. */
    (void)eota_inspect(ota);
    return result == ESP_OK ? ESP_ERR_INVALID_STATE : result;
}

esp_err_t eota_confirm_pending(eota_current_t *ota)
{
    if (ota == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    const esp_err_t current_result = eota_inspect(ota);
    if (current_result != ESP_OK) return current_result;
    if (ota->state == EOTA_STATE_VALID) {
        return ESP_OK;
    }
    if (ota->state != EOTA_STATE_PENDING_VERIFY) {
        return ESP_ERR_INVALID_STATE;
    }
    const esp_err_t result = esp_ota_mark_app_valid_cancel_rollback();
    const esp_err_t inspect_result = eota_inspect(ota);
    /* A write may have reached otadata even when the SDK reports failure.
     * The durable VALID state is the only successful confirmation fact. */
    if (inspect_result == ESP_OK && ota->state == EOTA_STATE_VALID) {
        ESP_LOGI(TAG, "pending slot %s confirmed valid", ota->running_partition);
        return ESP_OK;
    }
    if (result != ESP_OK) {
        return result;
    }
    if (inspect_result != ESP_OK) {
        return inspect_result;
    }
    return ESP_ERR_INVALID_STATE;
}
