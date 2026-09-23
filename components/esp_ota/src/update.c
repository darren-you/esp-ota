// SPDX-License-Identifier: Apache-2.0
#include "eota.h"

#include <stddef.h>
#include <string.h>

#include "sdkconfig.h"

#if defined(CONFIG_SECURE_SIGNED_APPS_NO_SECURE_BOOT) && \
    defined(CONFIG_SECURE_SIGNED_ON_UPDATE_NO_SECURE_BOOT) && \
    defined(CONFIG_SECURE_SIGNED_APPS_RSA_SCHEME) && \
    defined(CONFIG_MBEDTLS_CERTIFICATE_BUNDLE) && \
    defined(CONFIG_MBEDTLS_HAVE_TIME_DATE) && \
    defined(CONFIG_ESP_HTTP_CLIENT_ENABLE_HTTPS) && \
    defined(CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE) && \
    !defined(CONFIG_BOOTLOADER_APP_ANTI_ROLLBACK)
#define EOTA_SIGNED_ENABLED 1
#else
#define EOTA_SIGNED_ENABLED 0
#endif

#if EOTA_SIGNED_ENABLED
#include "esp_app_desc.h"
#include "esp_app_format.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_timer.h"
#include "psa/crypto.h"

#define EOTA_READ_BYTES 64
#define EOTA_PREFIX_BYTES \
    (sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t) + sizeof(esp_app_desc_t))

static bool valid_policy(const eota_policy_t *policy)
{
    if (policy == NULL || policy->project_name[0] == '\0' ||
        strnlen(policy->project_name, sizeof policy->project_name) == sizeof policy->project_name ||
        policy->chip_id == 0 || policy->ota_size_bytes == 0 ||
        policy->ota_0_address_bytes == policy->ota_1_address_bytes ||
        policy->connect_timeout_ms == 0 || policy->read_timeout_ms == 0 ||
        policy->idle_timeout_ms == 0 || policy->total_timeout_ms == 0 ||
        policy->connect_timeout_ms > 60000 || policy->read_timeout_ms > 60000 ||
        policy->total_timeout_ms > 3600000 ||
        policy->connect_timeout_ms > policy->total_timeout_ms ||
        policy->read_timeout_ms > policy->idle_timeout_ms ||
        policy->idle_timeout_ms > policy->total_timeout_ms) return false;
    return true;
}

static bool same_partition(const esp_partition_t *a, const esp_partition_t *b)
{
    return a != NULL && b != NULL && a->type == b->type && a->subtype == b->subtype &&
           a->address == b->address && a->size == b->size;
}

static bool expected_slot(const esp_partition_t *partition, const eota_policy_t *policy)
{
    if (partition == NULL || partition->type != ESP_PARTITION_TYPE_APP ||
        partition->size != policy->ota_size_bytes) return false;
    return (partition->subtype == ESP_PARTITION_SUBTYPE_APP_OTA_0 &&
            partition->address == policy->ota_0_address_bytes) ||
           (partition->subtype == ESP_PARTITION_SUBTYPE_APP_OTA_1 &&
            partition->address == policy->ota_1_address_bytes);
}

static bool read_state(const esp_partition_t *partition, eota_state_t *state)
{
    esp_ota_img_states_t raw;
    const esp_err_t result = esp_ota_get_state_partition(partition, &raw);
    if (result == ESP_ERR_NOT_FOUND) {
        *state = EOTA_STATE_UNTRACKED;
        return true;
    }
    if (result != ESP_OK) return false;
    *state = raw == ESP_OTA_IMG_PENDING_VERIFY ? EOTA_STATE_PENDING_VERIFY :
             raw == ESP_OTA_IMG_VALID ? EOTA_STATE_VALID :
             raw == ESP_OTA_IMG_NEW ? EOTA_STATE_NEW :
             raw == ESP_OTA_IMG_UNDEFINED ? EOTA_STATE_UNDEFINED :
             raw == ESP_OTA_IMG_INVALID ? EOTA_STATE_INVALID :
             raw == ESP_OTA_IMG_ABORTED ? EOTA_STATE_ABORTED : EOTA_STATE_OTHER;
    return true;
}

static eota_result_t observe_slots(const eota_policy_t *policy, eota_slots_t *slots,
                                   const esp_partition_t **running_out,
                                   const esp_partition_t **target_out)
{
    if (!valid_policy(policy) || slots == NULL) return EOTA_UPDATE_INVALID_REQUEST;
    memset(slots, 0, sizeof *slots);
    const esp_partition_t *running = esp_ota_get_running_partition();
    const esp_partition_t *boot = esp_ota_get_boot_partition();
    const esp_partition_t *target = esp_ota_get_next_update_partition(NULL);
    if (!expected_slot(running, policy) || !expected_slot(boot, policy) ||
        !expected_slot(target, policy) ||
        same_partition(running, target) ||
        !((running->subtype == ESP_PARTITION_SUBTYPE_APP_OTA_0 &&
           target->subtype == ESP_PARTITION_SUBTYPE_APP_OTA_1) ||
          (running->subtype == ESP_PARTITION_SUBTYPE_APP_OTA_1 &&
           target->subtype == ESP_PARTITION_SUBTYPE_APP_OTA_0))) {
        return EOTA_UPDATE_SLOT_UNAVAILABLE;
    }
    *slots = (eota_slots_t){
        .running_subtype = running->subtype,
        .boot_subtype = boot->subtype,
        .target_subtype = target->subtype,
        .running_address_bytes = running->address,
        .boot_address_bytes = boot->address,
        .target_address_bytes = target->address,
        .running_size_bytes = running->size,
        .boot_size_bytes = boot->size,
        .target_size_bytes = target->size,
    };
    if (!read_state(running, &slots->running_state) ||
        !read_state(target, &slots->target_state)) return EOTA_UPDATE_SLOT_UNAVAILABLE;
    if (running_out != NULL) *running_out = running;
    if (target_out != NULL) *target_out = target;
    return EOTA_UPDATE_OK;
}

static eota_result_t inspect_slots(const eota_policy_t *policy, uint32_t image_size_bytes,
                                   eota_slots_t *slots, const esp_partition_t **running_out,
                                   const esp_partition_t **target_out)
{
    if (image_size_bytes == 0) return EOTA_UPDATE_INVALID_REQUEST;
    eota_result_t result = observe_slots(policy, slots, running_out, target_out);
    if (result != EOTA_UPDATE_OK) return result;
    if (slots->running_subtype != slots->boot_subtype ||
        slots->running_address_bytes != slots->boot_address_bytes ||
        slots->running_size_bytes != slots->boot_size_bytes ||
        slots->running_state != EOTA_STATE_VALID ||
        (slots->target_state != EOTA_STATE_UNTRACKED &&
         slots->target_state != EOTA_STATE_UNDEFINED &&
         slots->target_state != EOTA_STATE_VALID &&
         slots->target_state != EOTA_STATE_INVALID &&
         slots->target_state != EOTA_STATE_ABORTED)) return EOTA_UPDATE_SLOT_UNAVAILABLE;
    return image_size_bytes <= slots->target_size_bytes ? EOTA_UPDATE_OK : EOTA_UPDATE_TOO_LARGE;
}

static bool valid_url(const char *url)
{
    if (url == NULL || strncmp(url, "https://", 8) != 0) return false;
    const size_t length = strnlen(url, EOTA_URL_BYTES + 1);
    if (length <= 8 || length > EOTA_URL_BYTES) return false;
    const char *authority_end = strpbrk(url + 8, "/?#");
    if (authority_end == NULL) authority_end = url + length;
    if (authority_end == url + 8 || *authority_end == '?' || *authority_end == '#') return false;
    for (const char *p = url + 8; p < authority_end; ++p) if (*p == '@') return false;
    for (const char *p = url + 8; p < url + length; ++p) {
        if ((unsigned char)*p <= 0x20 || *p == '#') return false;
    }
    return true;
}

static bool within_download_deadline(const eota_policy_t *policy, int64_t started_us,
                                     int64_t last_progress_us)
{
    const int64_t now_us = esp_timer_get_time();
    return now_us >= started_us && now_us - started_us < (int64_t)policy->total_timeout_ms * 1000 &&
           now_us >= last_progress_us &&
           now_us - last_progress_us < (int64_t)policy->idle_timeout_ms * 1000;
}

static eota_result_t hash_partition(const esp_partition_t *partition, uint32_t size,
                                    uint8_t digest[EOTA_SHA256_BYTES])
{
    uint8_t buffer[1024];
    psa_hash_operation_t hash = PSA_HASH_OPERATION_INIT;
    if (psa_crypto_init() != PSA_SUCCESS ||
        psa_hash_setup(&hash, PSA_ALG_SHA_256) != PSA_SUCCESS) return EOTA_UPDATE_RESOURCE_FAILURE;
    for (uint32_t offset = 0; offset < size;) {
        const size_t chunk = size - offset < sizeof buffer ? size - offset : sizeof buffer;
        if (esp_partition_read(partition, offset, buffer, chunk) != ESP_OK ||
            psa_hash_update(&hash, buffer, chunk) != PSA_SUCCESS) {
            (void)psa_hash_abort(&hash);
            return EOTA_UPDATE_RESOURCE_FAILURE;
        }
        offset += (uint32_t)chunk;
    }
    size_t actual_size = 0;
    if (psa_hash_finish(&hash, digest, EOTA_SHA256_BYTES, &actual_size) != PSA_SUCCESS ||
        actual_size != EOTA_SHA256_BYTES) {
        (void)psa_hash_abort(&hash);
        return EOTA_UPDATE_RESOURCE_FAILURE;
    }
    return EOTA_UPDATE_OK;
}

static bool same_slots(const eota_slots_t *a, const eota_slots_t *b)
{
    return a->running_subtype == b->running_subtype && a->boot_subtype == b->boot_subtype &&
           a->target_subtype == b->target_subtype &&
           a->running_address_bytes == b->running_address_bytes &&
           a->boot_address_bytes == b->boot_address_bytes &&
           a->target_address_bytes == b->target_address_bytes &&
           a->running_size_bytes == b->running_size_bytes &&
           a->boot_size_bytes == b->boot_size_bytes &&
           a->target_size_bytes == b->target_size_bytes;
}
#endif

bool eota_available(void)
{
    return EOTA_SIGNED_ENABLED;
}

const char *eota_error(eota_result_t result)
{
    switch (result) {
    case EOTA_UPDATE_OK: return "ok";
    case EOTA_UPDATE_UNSUPPORTED: return "ota_signing_unavailable";
    case EOTA_UPDATE_INVALID_REQUEST: return "invalid_request";
    case EOTA_UPDATE_SLOT_UNAVAILABLE: return "ota_slot_unavailable";
    case EOTA_UPDATE_TOO_LARGE: return "ota_image_too_large";
    case EOTA_UPDATE_WRONG_TARGET: return "ota_wrong_target";
    case EOTA_UPDATE_DOWNLOAD_FAILED: return "ota_download_failed";
    case EOTA_UPDATE_HASH_MISMATCH: return "ota_hash_mismatch";
    case EOTA_UPDATE_SIGNATURE_INVALID: return "ota_signature_invalid";
    case EOTA_UPDATE_BOOT_STATE_UNKNOWN: return "ota_boot_state_unknown";
    case EOTA_UPDATE_RESOURCE_FAILURE: return "resource_failure";
    default: return NULL;
    }
}

eota_result_t eota_preflight(const eota_policy_t *policy, uint32_t image_size_bytes,
                             eota_slots_t *slots)
{
#if !EOTA_SIGNED_ENABLED
    (void)policy; (void)image_size_bytes; (void)slots;
    return EOTA_UPDATE_UNSUPPORTED;
#else
    return inspect_slots(policy, image_size_bytes, slots, NULL, NULL);
#endif
}

eota_result_t eota_observe_slots(const eota_policy_t *policy, eota_slots_t *slots)
{
#if !EOTA_SIGNED_ENABLED
    (void)policy; (void)slots;
    return EOTA_UPDATE_UNSUPPORTED;
#else
    return observe_slots(policy, slots, NULL, NULL);
#endif
}

eota_result_t eota_prepare(const eota_policy_t *policy, const eota_image_t *image,
                           eota_progress_t progress, void *context, eota_prepared_t *prepared)
{
#if !EOTA_SIGNED_ENABLED
    (void)policy; (void)image; (void)progress; (void)context; (void)prepared;
    return EOTA_UPDATE_UNSUPPORTED;
#else
    if (policy == NULL || !policy->trusted_time || image == NULL || prepared == NULL ||
        !valid_url(image->image_url)) return EOTA_UPDATE_INVALID_REQUEST;
    memset(prepared, 0, sizeof *prepared);
    eota_slots_t slots;
    const esp_partition_t *target = NULL;
    eota_result_t result = inspect_slots(policy, image->image_size_bytes, &slots, NULL, &target);
    if (result != EOTA_UPDATE_OK) return result;
    if (image->image_size_bytes < EOTA_PREFIX_BYTES) return EOTA_UPDATE_INVALID_REQUEST;

    const esp_http_client_config_t http = {
        .url = image->image_url,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .disable_auto_redirect = true,
        .timeout_ms = (int)policy->connect_timeout_ms,
        .buffer_size = 1024,
    };
    esp_http_client_handle_t client = esp_http_client_init(&http);
    if (client == NULL) return EOTA_UPDATE_RESOURCE_FAILURE;
    esp_ota_handle_t handle = 0;
    bool ota_started = false;
    result = EOTA_UPDATE_DOWNLOAD_FAILED;
    const int64_t started_us = esp_timer_get_time();
    int64_t last_progress_us = started_us;
    if (!within_download_deadline(policy, started_us, last_progress_us) ||
        esp_http_client_open(client, 0) != ESP_OK ||
        !within_download_deadline(policy, started_us, last_progress_us)) goto abort;
    if (esp_http_client_set_timeout_ms(client, (int)policy->read_timeout_ms) != ESP_OK) goto abort;
    int64_t content_length;
    do {
        if (!within_download_deadline(policy, started_us, last_progress_us)) goto abort;
        content_length = esp_http_client_fetch_headers(client);
        if (!within_download_deadline(policy, started_us, last_progress_us)) goto abort;
    } while (content_length == -ESP_ERR_HTTP_EAGAIN);
    if (content_length != image->image_size_bytes ||
        esp_http_client_get_status_code(client) != 200 ||
        esp_http_client_is_chunked_response(client) ||
        esp_http_client_get_content_length(client) != image->image_size_bytes) goto abort;

    uint8_t buffer[EOTA_READ_BYTES];
    uint8_t prefix[EOTA_PREFIX_BYTES];
    uint8_t write_buffer[1024];
    size_t write_used = 0;
    uint32_t received = 0;
    while (received < image->image_size_bytes) {
        if (!within_download_deadline(policy, started_us, last_progress_us)) goto abort;
        const uint32_t left = image->image_size_bytes - received;
        size_t wanted = left < sizeof buffer ? left : sizeof buffer;
        if (received < sizeof prefix && wanted > sizeof prefix - received) {
            wanted = sizeof prefix - received;
        } else if (received >= sizeof prefix && wanted > sizeof write_buffer - write_used) {
            wanted = sizeof write_buffer - write_used;
        }
        const int count = esp_http_client_read(client, (char *)buffer, (int)wanted);
        if (!within_download_deadline(policy, started_us, last_progress_us)) goto abort;
        if (count == -ESP_ERR_HTTP_EAGAIN) continue;
        if (count <= 0 || (size_t)count > wanted) goto abort;
        if (received < sizeof prefix) {
            memcpy(prefix + received, buffer, (size_t)count);
        } else {
            memcpy(write_buffer + write_used, buffer, (size_t)count);
            write_used += (size_t)count;
            if (write_used == sizeof write_buffer) {
                if (esp_ota_write(handle, write_buffer, write_used) != ESP_OK) goto abort;
                write_used = 0;
            }
        }
        received += (uint32_t)count;
        last_progress_us = esp_timer_get_time();
        if (!within_download_deadline(policy, started_us, last_progress_us)) goto abort;
        if (received == sizeof prefix) {
            esp_image_header_t image_header;
            esp_app_desc_t app_desc;
            memcpy(&image_header, prefix, sizeof image_header);
            memcpy(&app_desc, prefix + sizeof image_header + sizeof(esp_image_segment_header_t),
                   sizeof app_desc);
            if (image_header.magic != ESP_IMAGE_HEADER_MAGIC ||
                image_header.chip_id != policy->chip_id ||
                app_desc.magic_word != ESP_APP_DESC_MAGIC_WORD ||
                strncmp(app_desc.project_name, policy->project_name, sizeof app_desc.project_name) != 0 ||
                esp_ota_check_image_validity(ESP_PARTITION_TYPE_APP, &image_header, &app_desc) != ESP_OK) {
                result = EOTA_UPDATE_WRONG_TARGET;
                goto abort;
            }
            if (esp_ota_begin(target, image->image_size_bytes, &handle) != ESP_OK) goto abort;
            ota_started = true;
            memcpy(write_buffer, prefix, sizeof prefix);
            write_used = sizeof prefix;
        }
        if (progress != NULL) progress(received, image->image_size_bytes, context);
    }
    if (!esp_http_client_is_complete_data_received(client) ||
        (write_used > 0 && esp_ota_write(handle, write_buffer, write_used) != ESP_OK)) goto abort;
    uint8_t digest[EOTA_SHA256_BYTES];
    result = hash_partition(target, image->image_size_bytes, digest);
    if (result != EOTA_UPDATE_OK) goto abort;
    if (memcmp(digest, image->sha256, sizeof digest) != 0) {
        result = EOTA_UPDATE_HASH_MISMATCH;
        goto abort;
    }
    const esp_err_t finish = esp_ota_end(handle);
    ota_started = false;
    (void)esp_http_client_cleanup(client);
    if (finish != ESP_OK) {
        return finish == ESP_ERR_OTA_VALIDATE_FAILED ? EOTA_UPDATE_SIGNATURE_INVALID :
               EOTA_UPDATE_DOWNLOAD_FAILED;
    }
    *prepared = (eota_prepared_t){.slots = slots, .image_size_bytes = image->image_size_bytes};
    memcpy(prepared->sha256, image->sha256, sizeof prepared->sha256);
    return EOTA_UPDATE_OK;
abort:
    if (ota_started) (void)esp_ota_abort(handle);
    (void)esp_http_client_cleanup(client);
    return result;
#endif
}

eota_result_t eota_select(const eota_policy_t *policy, const eota_prepared_t *prepared)
{
#if !EOTA_SIGNED_ENABLED
    (void)policy; (void)prepared;
    return EOTA_UPDATE_UNSUPPORTED;
#else
    if (prepared == NULL) return EOTA_UPDATE_INVALID_REQUEST;
    eota_slots_t slots;
    const esp_partition_t *running = NULL;
    const esp_partition_t *target = NULL;
    eota_result_t result = inspect_slots(policy, prepared->image_size_bytes, &slots,
                                         &running, &target);
    if (result != EOTA_UPDATE_OK) return result;
    if (!same_slots(&slots, &prepared->slots)) return EOTA_UPDATE_SLOT_UNAVAILABLE;
    uint8_t digest[EOTA_SHA256_BYTES];
    result = hash_partition(target, prepared->image_size_bytes, digest);
    if (result != EOTA_UPDATE_OK) return result;
    if (memcmp(digest, prepared->sha256, sizeof digest) != 0) return EOTA_UPDATE_HASH_MISMATCH;
    const esp_err_t select = esp_ota_set_boot_partition(target);
    const bool target_selected = same_partition(esp_ota_get_boot_partition(), target);
    if (select == ESP_OK && target_selected && esp_ota_check_rollback_is_possible()) {
        return EOTA_UPDATE_OK;
    }
    if (target_selected || !same_partition(esp_ota_get_boot_partition(), running)) {
        /* A failed selector write may have reached otadata. Restore the
         * previous signed image, then read it back before reporting. */
        (void)esp_ota_set_boot_partition(running);
        if (!same_partition(esp_ota_get_boot_partition(), running)) {
            return EOTA_UPDATE_BOOT_STATE_UNKNOWN;
        }
    }
    return select == ESP_ERR_OTA_VALIDATE_FAILED ? EOTA_UPDATE_SIGNATURE_INVALID :
           EOTA_UPDATE_SLOT_UNAVAILABLE;
#endif
}

eota_result_t eota_sha256_running(const eota_policy_t *policy, uint32_t size_bytes,
                                  uint8_t digest[EOTA_SHA256_BYTES])
{
#if !EOTA_SIGNED_ENABLED
    (void)policy; (void)size_bytes; (void)digest;
    return EOTA_UPDATE_UNSUPPORTED;
#else
    if (!valid_policy(policy) || digest == NULL || size_bytes == 0) return EOTA_UPDATE_INVALID_REQUEST;
    memset(digest, 0, EOTA_SHA256_BYTES);
    const esp_partition_t *running = esp_ota_get_running_partition();
    if (!expected_slot(running, policy)) return EOTA_UPDATE_SLOT_UNAVAILABLE;
    if (size_bytes > running->size) return EOTA_UPDATE_TOO_LARGE;
    return hash_partition(running, size_bytes, digest);
#endif
}
