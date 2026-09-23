// SPDX-License-Identifier: Apache-2.0
#include "eota.h"
#include "esp_ota_ops.h"

#include <assert.h>
#include <stdbool.h>
#include <stdio.h>

static esp_partition_t running = {.label = "ota_1"};
static esp_ota_img_states_t image_state = ESP_OTA_IMG_PENDING_VERIFY;
static esp_err_t inspect_result = ESP_OK;
static esp_err_t mark_result = ESP_OK;
static unsigned mark_calls;
static esp_ota_img_states_t state_after_mark = ESP_OTA_IMG_VALID;
static esp_err_t rollback_result = ESP_ERR_OTA_ROLLBACK_FAILED;
static esp_ota_img_states_t state_after_rollback = ESP_OTA_IMG_PENDING_VERIFY;
static unsigned rollback_calls;
static bool partition_available = true;

const esp_partition_t *esp_ota_get_running_partition(void)
{
    return partition_available ? &running : NULL;
}

esp_err_t esp_ota_get_state_partition(const esp_partition_t *partition, esp_ota_img_states_t *state)
{
    assert(partition == &running);
    *state = image_state;
    return inspect_result;
}

esp_err_t esp_ota_mark_app_valid_cancel_rollback(void)
{
    ++mark_calls;
    image_state = state_after_mark;
    return mark_result;
}

esp_err_t esp_ota_mark_app_invalid_rollback_and_reboot(void)
{
    ++rollback_calls;
    image_state = state_after_rollback;
    return rollback_result;
}

int main(void)
{
    eota_current_t ota = {0};
    assert(eota_inspect(NULL) == ESP_ERR_INVALID_ARG);
    partition_available = false;
    assert(eota_inspect(&ota) == ESP_ERR_NOT_FOUND);
    assert(ota.state == EOTA_STATE_UNKNOWN && ota.running_partition == NULL);
    partition_available = true;

    inspect_result = ESP_FAIL;
    assert(eota_inspect(&ota) == ESP_FAIL);
    inspect_result = ESP_ERR_NOT_FOUND;
    assert(eota_inspect(&ota) == ESP_OK && ota.state == EOTA_STATE_UNTRACKED);
    inspect_result = ESP_ERR_NOT_SUPPORTED;
    assert(eota_inspect(&ota) == ESP_ERR_NOT_SUPPORTED && ota.state == EOTA_STATE_UNKNOWN);
    inspect_result = ESP_OK;
    image_state = ESP_OTA_IMG_VALID;
    assert(eota_inspect(&ota) == ESP_OK && ota.state == EOTA_STATE_VALID);
    assert(eota_confirm_pending(&ota) == ESP_OK && mark_calls == 0);

    image_state = ESP_OTA_IMG_PENDING_VERIFY;
    assert(eota_inspect(&ota) == ESP_OK && ota.state == EOTA_STATE_PENDING_VERIFY);
    assert(eota_confirm_pending(NULL) == ESP_ERR_INVALID_ARG);
    assert(mark_calls == 0 && ota.state == EOTA_STATE_PENDING_VERIFY);

    mark_result = ESP_FAIL;
    state_after_mark = ESP_OTA_IMG_PENDING_VERIFY;
    assert(eota_confirm_pending(&ota) == ESP_FAIL);
    assert(mark_calls == 1 && ota.state == EOTA_STATE_PENDING_VERIFY);
    state_after_mark = ESP_OTA_IMG_VALID;
    assert(eota_confirm_pending(&ota) == ESP_OK);
    assert(mark_calls == 2 && ota.state == EOTA_STATE_VALID);

    image_state = ESP_OTA_IMG_PENDING_VERIFY;
    assert(eota_inspect(&ota) == ESP_OK);
    mark_result = ESP_OK;
    state_after_mark = ESP_OTA_IMG_PENDING_VERIFY;
    assert(eota_confirm_pending(&ota) == ESP_ERR_INVALID_STATE);
    assert(mark_calls == 3 && ota.state == EOTA_STATE_PENDING_VERIFY);
    state_after_mark = ESP_OTA_IMG_VALID;
    assert(eota_confirm_pending(&ota) == ESP_OK);
    assert(mark_calls == 4 && ota.state == EOTA_STATE_VALID);
    assert(eota_confirm_pending(&ota) == ESP_OK && mark_calls == 4);

    image_state = ESP_OTA_IMG_PENDING_VERIFY;
    assert(eota_inspect(&ota) == ESP_OK);
    assert(eota_reject_pending(&ota) == ESP_ERR_OTA_ROLLBACK_FAILED);
    assert(rollback_calls == 1 && ota.state == EOTA_STATE_PENDING_VERIFY);
    rollback_result = ESP_FAIL;
    state_after_rollback = ESP_OTA_IMG_INVALID;
    assert(eota_reject_pending(&ota) == ESP_FAIL);
    assert(rollback_calls == 2 && ota.state == EOTA_STATE_INVALID);

    puts("  ota_confirmation passed (SDK failures injected; no device write)");
}
