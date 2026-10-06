// SPDX-License-Identifier: Apache-2.0
#include "eota.h"
#include "eota_http_transport.h"
#include "http_deadline.h"

#include <stddef.h>
#include <string.h>

#include "sdkconfig.h"

#if (defined(CONFIG_IDF_TARGET_ESP32C3) && \
     defined(CONFIG_SECURE_SIGNED_APPS_RSA_SCHEME)) || \
    (defined(CONFIG_IDF_TARGET_ESP32) && \
     defined(CONFIG_SECURE_SIGNED_APPS_ECDSA_SCHEME))
#define EOTA_SIGNED_SCHEME_SUPPORTED 1
#else
#define EOTA_SIGNED_SCHEME_SUPPORTED 0
#endif

#if EOTA_SIGNED_SCHEME_SUPPORTED && \
    defined(CONFIG_SECURE_SIGNED_APPS_NO_SECURE_BOOT) && \
    defined(CONFIG_SECURE_SIGNED_ON_UPDATE_NO_SECURE_BOOT) && \
    defined(CONFIG_MBEDTLS_CERTIFICATE_BUNDLE) && \
    defined(CONFIG_MBEDTLS_HAVE_TIME_DATE) && \
    defined(CONFIG_ESP_HTTP_CLIENT_ENABLE_HTTPS) && \
    defined(CONFIG_ESP_HTTP_CLIENT_ENABLE_CUSTOM_TRANSPORT) && \
    defined(CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE) && \
    !defined(CONFIG_BOOTLOADER_APP_ANTI_ROLLBACK)
#define EOTA_SIGNED_ENABLED 1
#else
#define EOTA_SIGNED_ENABLED 0
#endif

#if EOTA_SIGNED_ENABLED
#include "esp_app_desc.h"
#include "esp_app_format.h"
#include "esp_image_format.h"
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
        policy->chip_id == ESP_CHIP_ID_INVALID || policy->ota_size_bytes == 0 ||
        policy->ota_0_address_bytes == policy->ota_1_address_bytes ||
        policy->connect_timeout_ms == 0 || policy->read_timeout_ms == 0 ||
        policy->idle_timeout_ms == 0 || policy->total_timeout_ms == 0 ||
        policy->connect_timeout_ms > 60000 || policy->read_timeout_ms > 60000 ||
        policy->total_timeout_ms > 3600000 ||
        policy->connect_timeout_ms > policy->total_timeout_ms ||
        policy->read_timeout_ms > policy->idle_timeout_ms ||
        policy->idle_timeout_ms > policy->total_timeout_ms ||
        (policy->flash_io.acquire == NULL) != (policy->flash_io.release == NULL)) return false;
    return true;
}

static bool flash_io_acquire(const eota_policy_t *policy)
{
    return policy->flash_io.acquire == NULL ||
           policy->flash_io.acquire(policy->flash_io.context);
}

static bool flash_io_release(const eota_policy_t *policy)
{
    return policy->flash_io.release == NULL ||
           policy->flash_io.release(policy->flash_io.context);
}

static eota_result_t flash_read_partition(const eota_policy_t *policy,
                                          const esp_partition_t *partition,
                                          size_t offset, void *data, size_t size)
{
    if (!flash_io_acquire(policy)) return EOTA_UPDATE_RESOURCE_FAILURE;
    const esp_err_t read = esp_partition_read(partition, offset, data, size);
    if (!flash_io_release(policy)) return EOTA_UPDATE_BOOT_STATE_UNKNOWN;
    return read == ESP_OK ? EOTA_UPDATE_OK : EOTA_UPDATE_RESOURCE_FAILURE;
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

static eota_result_t observe_slots_unlocked(const eota_policy_t *policy,
                                            eota_slots_t *slots,
                                            const esp_partition_t **running_out,
                                            const esp_partition_t **target_out)
{
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

static eota_result_t observe_slots(const eota_policy_t *policy, eota_slots_t *slots,
                                   const esp_partition_t **running_out,
                                   const esp_partition_t **target_out)
{
    if (!valid_policy(policy) || slots == NULL) return EOTA_UPDATE_INVALID_REQUEST;
    if (!flash_io_acquire(policy)) return EOTA_UPDATE_RESOURCE_FAILURE;
    const eota_result_t result = observe_slots_unlocked(policy, slots,
                                                         running_out, target_out);
    return flash_io_release(policy) ? result : EOTA_UPDATE_BOOT_STATE_UNKNOWN;
}

static eota_result_t boot_matches(const eota_policy_t *policy,
                                  const esp_partition_t *partition, bool *matches)
{
    if (!flash_io_acquire(policy)) return EOTA_UPDATE_BOOT_STATE_UNKNOWN;
    *matches = same_partition(esp_ota_get_boot_partition(), partition);
    return flash_io_release(policy) ? EOTA_UPDATE_OK : EOTA_UPDATE_BOOT_STATE_UNKNOWN;
}

static eota_result_t read_raw_state(const eota_policy_t *policy,
                                    const esp_partition_t *partition,
                                    esp_ota_img_states_t *state,
                                    esp_err_t *sdk_result)
{
    if (!flash_io_acquire(policy)) return EOTA_UPDATE_BOOT_STATE_UNKNOWN;
    *sdk_result = esp_ota_get_state_partition(partition, state);
    if (!flash_io_release(policy)) return EOTA_UPDATE_BOOT_STATE_UNKNOWN;
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

static bool valid_authority(const char *host, const char *end)
{
    const char *port = NULL;
    if (*host == '[') {
        const char *close = memchr(host + 1, ']', (size_t)(end - host - 1));
        if (close == NULL || close == host + 1) return false;
        port = close + 1;
    } else {
        port = memchr(host, ':', (size_t)(end - host));
        if (port == NULL) port = end;
        if (port == host || memchr(host, '[', (size_t)(port - host)) != NULL ||
            memchr(host, ']', (size_t)(port - host)) != NULL) return false;
    }
    if (port == end) return true;
    if (*port++ != ':' || port == end) return false;
    unsigned value = 0;
    for (; port < end; ++port) {
        if (*port < '0' || *port > '9') return false;
        value = value * 10 + (unsigned)(*port - '0');
        if (value > 65535) return false;
    }
    return value != 0;
}

static bool valid_url(const char *url)
{
    if (url == NULL || strncmp(url, "https://", 8) != 0) return false;
    const size_t length = strnlen(url, EOTA_URL_BYTES + 1);
    if (length <= 8 || length > EOTA_URL_BYTES) return false;
    const char *authority_end = strpbrk(url + 8, "/?#");
    if (authority_end == NULL) authority_end = url + length;
    if (authority_end == url + 8 || *authority_end == '?' || *authority_end == '#' ||
        !valid_authority(url + 8, authority_end)) return false;
    for (const char *p = url + 8; p < authority_end; ++p) if (*p == '@') return false;
    for (const char *p = url + 8; p < url + length; ++p) {
        if ((unsigned char)*p <= 0x20 || *p == '#') return false;
    }
    return true;
}

static bool matches_image_target(const eota_policy_t *policy,
                                 const uint8_t prefix[EOTA_PREFIX_BYTES])
{
    esp_image_header_t image_header;
    esp_app_desc_t app_desc;
    memcpy(&image_header, prefix, sizeof image_header);
    memcpy(&app_desc, prefix + sizeof image_header + sizeof(esp_image_segment_header_t),
           sizeof app_desc);
    return image_header.magic == ESP_IMAGE_HEADER_MAGIC &&
           image_header.chip_id == policy->chip_id &&
           app_desc.magic_word == ESP_APP_DESC_MAGIC_WORD &&
           strncmp(app_desc.project_name, policy->project_name,
                   sizeof app_desc.project_name) == 0 &&
           esp_ota_check_image_validity(ESP_PARTITION_TYPE_APP, &image_header,
                                        &app_desc) == ESP_OK;
}

static eota_result_t hash_partition(const eota_policy_t *policy,
                                    const esp_partition_t *partition, uint32_t size,
                                    uint8_t digest[EOTA_SHA256_BYTES],
                                    const eota_http_deadline_t *deadline)
{
    uint8_t buffer[1024];
    psa_hash_operation_t hash = PSA_HASH_OPERATION_INIT;
    if (psa_crypto_init() != PSA_SUCCESS ||
        psa_hash_setup(&hash, PSA_ALG_SHA_256) != PSA_SUCCESS) return EOTA_UPDATE_RESOURCE_FAILURE;
    for (uint32_t offset = 0; offset < size;) {
        if (deadline != NULL && eota_http_deadline_remaining_us(deadline) <= 0) {
            (void)psa_hash_abort(&hash);
            return EOTA_UPDATE_DOWNLOAD_FAILED;
        }
        const size_t chunk = size - offset < sizeof buffer ? size - offset : sizeof buffer;
        const eota_result_t read = flash_read_partition(policy, partition, offset,
                                                        buffer, chunk);
        if (read != EOTA_UPDATE_OK) {
            (void)psa_hash_abort(&hash);
            return read;
        }
        if (psa_hash_update(&hash, buffer, chunk) != PSA_SUCCESS) {
            (void)psa_hash_abort(&hash);
            return EOTA_UPDATE_RESOURCE_FAILURE;
        }
        if (deadline != NULL && eota_http_deadline_remaining_us(deadline) <= 0) {
            (void)psa_hash_abort(&hash);
            return EOTA_UPDATE_DOWNLOAD_FAILED;
        }
        offset += (uint32_t)chunk;
    }
    size_t actual_size = 0;
    if (psa_hash_finish(&hash, digest, EOTA_SHA256_BYTES, &actual_size) != PSA_SUCCESS ||
        actual_size != EOTA_SHA256_BYTES) {
        (void)psa_hash_abort(&hash);
        return EOTA_UPDATE_RESOURCE_FAILURE;
    }
    if (deadline != NULL && eota_http_deadline_remaining_us(deadline) <= 0) {
        return EOTA_UPDATE_DOWNLOAD_FAILED;
    }
    return EOTA_UPDATE_OK;
}

static eota_result_t verify_image_size(const eota_policy_t *policy,
                                       const esp_partition_t *partition, uint32_t size,
                                       const eota_http_deadline_t *deadline)
{
    const esp_partition_pos_t position = {
        .offset = partition->address,
        .size = partition->size,
    };
    esp_image_metadata_t metadata = {0};
    /* end/set_boot verify the whole partition but do not report the signed
     * image length. The requested prefix can otherwise borrow a valid old
     * tail beyond esp_ota_begin's erased range. Use verified SDK metadata;
     * esp_image_get_metadata omits the signature from its image_len. */
    if (!flash_io_acquire(policy)) return EOTA_UPDATE_RESOURCE_FAILURE;
    const esp_err_t verified = esp_image_verify(ESP_IMAGE_VERIFY, &position, &metadata);
    if (!flash_io_release(policy)) return EOTA_UPDATE_BOOT_STATE_UNKNOWN;
    if (deadline != NULL && eota_http_deadline_remaining_us(deadline) <= 0) {
        return EOTA_UPDATE_DOWNLOAD_FAILED;
    }
    if (verified != ESP_OK) {
        return verified == ESP_ERR_NO_MEM || verified == ESP_ERR_IMAGE_FLASH_FAIL ?
               EOTA_UPDATE_RESOURCE_FAILURE : EOTA_UPDATE_SIGNATURE_INVALID;
    }
    return metadata.image_len == size ? EOTA_UPDATE_OK : EOTA_UPDATE_INVALID_REQUEST;
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

eota_result_t eota_validate_image_request(const eota_image_t *image)
{
#if !EOTA_SIGNED_ENABLED
    (void)image;
    return EOTA_UPDATE_UNSUPPORTED;
#else
    if (image == NULL || !valid_url(image->image_url) ||
        image->image_size_bytes < EOTA_PREFIX_BYTES) {
        return EOTA_UPDATE_INVALID_REQUEST;
    }
    return EOTA_UPDATE_OK;
#endif
}

eota_result_t eota_validate_stream_request(const eota_stream_t *stream)
{
#if !EOTA_SIGNED_ENABLED
    (void)stream;
    return EOTA_UPDATE_UNSUPPORTED;
#else
    return stream != NULL && stream->read != NULL &&
           stream->image_size_bytes >= EOTA_PREFIX_BYTES ?
           EOTA_UPDATE_OK : EOTA_UPDATE_INVALID_REQUEST;
#endif
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
    case EOTA_UPDATE_IMAGE_INVALID: return "ota_image_invalid";
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

eota_result_t eota_retire_inactive(const eota_policy_t *policy,
                                   uint8_t expected_target_subtype,
                                   const uint8_t expected_running_sha256[EOTA_SHA256_BYTES])
{
#if !EOTA_SIGNED_ENABLED
    (void)policy; (void)expected_target_subtype; (void)expected_running_sha256;
    return EOTA_UPDATE_UNSUPPORTED;
#else
    if (!valid_policy(policy) || expected_running_sha256 == NULL ||
        (expected_target_subtype != ESP_PARTITION_SUBTYPE_APP_OTA_0 &&
         expected_target_subtype != ESP_PARTITION_SUBTYPE_APP_OTA_1)) {
        return EOTA_UPDATE_INVALID_REQUEST;
    }
    uint8_t any = 0;
    for (size_t index = 0; index < EOTA_SHA256_BYTES; ++index) {
        any |= expected_running_sha256[index];
    }
    if (any == 0) return EOTA_UPDATE_INVALID_REQUEST;

    eota_slots_t before = {0};
    const esp_partition_t *running = NULL;
    const esp_partition_t *target = NULL;
    const eota_result_t observed = observe_slots(policy, &before, &running, &target);
    if (observed != EOTA_UPDATE_OK) return observed;
    if (before.running_subtype != before.boot_subtype ||
        before.target_subtype != expected_target_subtype ||
        before.running_state != EOTA_STATE_VALID ||
        (before.target_state != EOTA_STATE_UNTRACKED &&
         before.target_state != EOTA_STATE_UNDEFINED &&
         before.target_state != EOTA_STATE_VALID &&
         before.target_state != EOTA_STATE_NEW &&
         before.target_state != EOTA_STATE_PENDING_VERIFY &&
         before.target_state != EOTA_STATE_INVALID &&
         before.target_state != EOTA_STATE_ABORTED) ||
        target->erase_size == 0 || target->erase_size > target->size) {
        return EOTA_UPDATE_SLOT_UNAVAILABLE;
    }

    uint8_t prefix[EOTA_PREFIX_BYTES];
    uint8_t running_sha256[EOTA_SHA256_BYTES];
    uint32_t running_size = 0;
    eota_result_t read = flash_read_partition(policy, running, 0, prefix, sizeof prefix);
    if (read != EOTA_UPDATE_OK) return read;
    if (!matches_image_target(policy, prefix)) return EOTA_UPDATE_WRONG_TARGET;
    if (eota_sha256_verified_image(policy, before.running_subtype,
                                   &running_size, running_sha256) != EOTA_UPDATE_OK ||
        running_size == 0 ||
        memcmp(running_sha256, expected_running_sha256, EOTA_SHA256_BYTES) != 0) {
        return EOTA_UPDATE_IMAGE_INVALID;
    }
    const uint32_t expected_running_size = running_size;

    uint8_t target_magic = 0;
    read = flash_read_partition(policy, target, 0, &target_magic, sizeof target_magic);
    if (read != EOTA_UPDATE_OK) return EOTA_UPDATE_BOOT_STATE_UNKNOWN;
    /* The caller's durable prewrite receipt authorizes retiring this exact
     * inactive slot. App-side signature rejection alone does not prove the
     * bootloader will refuse the image under every signed-app configuration.
     * Only an erased first image sector (0xff magic) avoids a repeat erase
     * after reset. Invalidate otadata separately and inspect both facts. */
    if (!flash_io_acquire(policy)) return EOTA_UPDATE_BOOT_STATE_UNKNOWN;
    const esp_err_t erase = target_magic == 0xffU ? ESP_OK :
        esp_partition_erase_range(target, 0, target->erase_size);
    if (erase == ESP_OK) (void)esp_ota_invalidate_inactive_ota_data_slot();
    const bool released = flash_io_release(policy);
    if (erase != ESP_OK || !released) return EOTA_UPDATE_BOOT_STATE_UNKNOWN;

    if (flash_read_partition(policy, target, 0, &target_magic,
                             sizeof target_magic) != EOTA_UPDATE_OK ||
        target_magic != 0xffU) return EOTA_UPDATE_BOOT_STATE_UNKNOWN;

    eota_slots_t after = {0};
    if (observe_slots(policy, &after, NULL, NULL) != EOTA_UPDATE_OK ||
        after.running_subtype != before.running_subtype ||
        after.boot_subtype != before.boot_subtype ||
        after.target_subtype != before.target_subtype ||
        after.running_address_bytes != before.running_address_bytes ||
        after.boot_address_bytes != before.boot_address_bytes ||
        after.target_address_bytes != before.target_address_bytes ||
        after.running_size_bytes != before.running_size_bytes ||
        after.boot_size_bytes != before.boot_size_bytes ||
        after.target_size_bytes != before.target_size_bytes ||
        after.running_state != EOTA_STATE_VALID ||
        (after.target_state != EOTA_STATE_UNTRACKED &&
         after.target_state != EOTA_STATE_INVALID &&
         after.target_state != EOTA_STATE_ABORTED)) {
        return EOTA_UPDATE_BOOT_STATE_UNKNOWN;
    }
    uint32_t target_size = 0;
    uint8_t target_sha256[EOTA_SHA256_BYTES];
    if (eota_sha256_verified_image(policy, expected_target_subtype,
                                   &target_size, target_sha256) !=
        EOTA_UPDATE_IMAGE_INVALID) {
        return EOTA_UPDATE_BOOT_STATE_UNKNOWN;
    }
    if (eota_sha256_verified_image(policy, before.running_subtype,
                                   &running_size, running_sha256) != EOTA_UPDATE_OK ||
        running_size != expected_running_size ||
        memcmp(running_sha256, expected_running_sha256, EOTA_SHA256_BYTES) != 0) {
        return EOTA_UPDATE_BOOT_STATE_UNKNOWN;
    }
    return EOTA_UPDATE_OK;
#endif
}

#if EOTA_SIGNED_ENABLED
typedef struct {
    int (*read)(void *context, uint8_t *buffer, size_t capacity);
    bool (*finish)(void *context);
    void *context;
} eota_input_t;

static eota_result_t receive_image(const eota_policy_t *policy, uint32_t image_size_bytes,
                                   const uint8_t sha256[EOTA_SHA256_BYTES],
                                   eota_http_deadline_t *deadline, const eota_slots_t *slots,
                                   const esp_partition_t *target, const eota_input_t *input,
                                   eota_progress_t progress, void *context,
                                   eota_prepared_t *prepared)
{
    esp_ota_handle_t handle = 0;
    bool ota_started = false;
    eota_result_t result = EOTA_UPDATE_DOWNLOAD_FAILED;
    uint8_t buffer[EOTA_READ_BYTES];
    uint8_t prefix[EOTA_PREFIX_BYTES];
    uint8_t write_buffer[1024];
    size_t write_used = 0;
    uint32_t received = 0;
    while (received < image_size_bytes) {
        if (eota_http_deadline_remaining_us(deadline) <= 0) goto abort;
        const uint32_t left = image_size_bytes - received;
        size_t wanted = left < sizeof buffer ? left : sizeof buffer;
        if (received < sizeof prefix && wanted > sizeof prefix - received) {
            wanted = sizeof prefix - received;
        } else if (received >= sizeof prefix && wanted > sizeof write_buffer - write_used) {
            wanted = sizeof write_buffer - write_used;
        }
        const int count = input->read(input->context, buffer, wanted);
        if (eota_http_deadline_remaining_us(deadline) <= 0) goto abort;
        if (count == EOTA_STREAM_READ_TIMEOUT) continue;
        if (count <= 0 || (size_t)count > wanted) goto abort;
        if (received < sizeof prefix) {
            memcpy(prefix + received, buffer, (size_t)count);
        } else {
            memcpy(write_buffer + write_used, buffer, (size_t)count);
            write_used += (size_t)count;
            if (write_used == sizeof write_buffer) {
                if (!flash_io_acquire(policy)) {
                    result = EOTA_UPDATE_RESOURCE_FAILURE;
                    goto abort;
                }
                const esp_err_t write = esp_ota_write(handle, write_buffer, write_used);
                if (!flash_io_release(policy)) {
                    result = EOTA_UPDATE_BOOT_STATE_UNKNOWN;
                    goto abort;
                }
                if (write != ESP_OK) goto abort;
                if (eota_http_deadline_remaining_us(deadline) <= 0) goto abort;
                write_used = 0;
            }
        }
        received += (uint32_t)count;
        if (eota_http_deadline_remaining_us(deadline) <= 0) goto abort;
        if (received == sizeof prefix) {
            if (!matches_image_target(policy, prefix)) {
                result = EOTA_UPDATE_WRONG_TARGET;
                goto abort;
            }
            /* The image length was already checked against the exact slot and
             * input metadata. Let IDF erase one affected sector at each sequential
             * write instead of erasing the whole image in this call; Base can
             * then arbitrate each Flash I/O turn with FRP scratch. */
            if (!flash_io_acquire(policy)) {
                result = EOTA_UPDATE_RESOURCE_FAILURE;
                goto abort;
            }
            const esp_err_t begin = esp_ota_begin(target, OTA_WITH_SEQUENTIAL_WRITES, &handle);
            ota_started = begin == ESP_OK;
            if (!flash_io_release(policy)) {
                result = EOTA_UPDATE_BOOT_STATE_UNKNOWN;
                goto abort;
            }
            if (begin != ESP_OK) goto abort;
            if (eota_http_deadline_remaining_us(deadline) <= 0) goto abort;
            memcpy(write_buffer, prefix, sizeof prefix);
            write_used = sizeof prefix;
        }
        if (progress != NULL) progress(received, image_size_bytes, context);
    }
    if (!input->finish(input->context) ||
        eota_http_deadline_remaining_us(deadline) <= 0) goto abort;
    if (write_used > 0) {
        if (!flash_io_acquire(policy)) {
            result = EOTA_UPDATE_RESOURCE_FAILURE;
            goto abort;
        }
        const esp_err_t write = esp_ota_write(handle, write_buffer, write_used);
        if (!flash_io_release(policy)) {
            result = EOTA_UPDATE_BOOT_STATE_UNKNOWN;
            goto abort;
        }
        if (write != ESP_OK) goto abort;
    }
    if (eota_http_deadline_remaining_us(deadline) <= 0) goto abort;
    uint8_t digest[EOTA_SHA256_BYTES];
    result = hash_partition(policy, target, image_size_bytes, digest, deadline);
    if (result != EOTA_UPDATE_OK) goto abort;
    if (memcmp(digest, sha256, sizeof digest) != 0) {
        result = EOTA_UPDATE_HASH_MISMATCH;
        goto abort;
    }
    if (!flash_io_acquire(policy)) {
        result = EOTA_UPDATE_RESOURCE_FAILURE;
        goto abort;
    }
    const esp_err_t finish = esp_ota_end(handle);
    ota_started = false;
    if (!flash_io_release(policy)) return EOTA_UPDATE_BOOT_STATE_UNKNOWN;
    if (finish != ESP_OK) {
        return finish == ESP_ERR_OTA_VALIDATE_FAILED ? EOTA_UPDATE_SIGNATURE_INVALID :
               EOTA_UPDATE_DOWNLOAD_FAILED;
    }
    if (eota_http_deadline_remaining_us(deadline) <= 0) return EOTA_UPDATE_DOWNLOAD_FAILED;
    result = verify_image_size(policy, target, image_size_bytes, deadline);
    if (result != EOTA_UPDATE_OK) return result;
    *prepared = (eota_prepared_t){.slots = *slots, .image_size_bytes = image_size_bytes};
    memcpy(prepared->sha256, sha256, sizeof prepared->sha256);
    return EOTA_UPDATE_OK;
abort:
    if (ota_started) (void)esp_ota_abort(handle);
    return result;
}

typedef struct {
    esp_http_client_handle_t client;
    esp_transport_handle_t transport;
} eota_http_input_t;

static void close_http_input(eota_http_input_t *input)
{
    if (input->client != NULL) (void)esp_http_client_cleanup(input->client);
    input->client = NULL;
    if (input->transport != NULL) (void)esp_transport_destroy(input->transport);
    input->transport = NULL;
}

static int read_http_input(void *context, uint8_t *buffer, size_t capacity)
{
    eota_http_input_t *input = context;
    const int count = esp_http_client_read(input->client, (char *)buffer, (int)capacity);
    return count == -ESP_ERR_HTTP_EAGAIN ? EOTA_STREAM_READ_TIMEOUT : count;
}

static bool finish_http_input(void *context)
{
    eota_http_input_t *input = context;
    if (!esp_http_client_is_complete_data_received(input->client)) return false;
    /* Return TLS memory before partition hashing and SDK signature checks. */
    close_http_input(input);
    return true;
}

typedef struct {
    const eota_stream_t *stream;
    const eota_policy_t *policy;
    eota_http_deadline_t *deadline;
} eota_stream_input_t;

static int read_stream_input(void *context, uint8_t *buffer, size_t capacity)
{
    eota_stream_input_t *input = context;
    const int64_t left_us = eota_http_deadline_remaining_us(input->deadline);
    /* Whole milliseconds keep a callback wait inside the actual session
     * budget. A sub-millisecond remainder cannot authorize another wait. */
    if (left_us < 1000) return -1;
    const uint32_t remaining_ms = (uint32_t)(left_us / 1000);
    const uint32_t timeout_ms = remaining_ms < input->policy->read_timeout_ms ?
                               remaining_ms : input->policy->read_timeout_ms;
    const int64_t started_us = esp_timer_get_time();
    const int count = input->stream->read(input->stream->context, buffer, capacity, timeout_ms);
    const int64_t finished_us = esp_timer_get_time();
    const int64_t elapsed_us = finished_us - started_us;
    if (finished_us < started_us || elapsed_us > (int64_t)timeout_ms * 1000 ||
        eota_http_deadline_remaining_us(input->deadline) <= 0 ||
        (count != EOTA_STREAM_READ_TIMEOUT && elapsed_us >= (int64_t)timeout_ms * 1000) ||
        (count > 0 && (size_t)count > capacity)) return -1;
    if (count > 0 && !eota_http_deadline_progress(input->deadline)) return -1;
    return count;
}

static bool finish_stream_input(void *context)
{
    uint8_t trailing;
    int count;
    do {
        count = read_stream_input(context, &trailing, 1);
    } while (count == EOTA_STREAM_READ_TIMEOUT);
    return count == 0;
}
#endif

eota_result_t eota_prepare(const eota_policy_t *policy, const eota_image_t *image,
                           eota_progress_t progress, void *context, eota_prepared_t *prepared)
{
    if (prepared != NULL) memset(prepared, 0, sizeof *prepared);
#if !EOTA_SIGNED_ENABLED
    (void)policy; (void)image; (void)progress; (void)context; (void)prepared;
    return EOTA_UPDATE_UNSUPPORTED;
#else
    if (!valid_policy(policy) || !policy->trusted_time || image == NULL || prepared == NULL) {
        return EOTA_UPDATE_INVALID_REQUEST;
    }
    const eota_result_t request_result = eota_validate_image_request(image);
    if (request_result != EOTA_UPDATE_OK) return request_result;
    eota_http_deadline_t deadline = {0};
    if (!eota_http_deadline_init(&deadline, policy->total_timeout_ms,
                                 policy->idle_timeout_ms)) return EOTA_UPDATE_RESOURCE_FAILURE;
    eota_slots_t slots;
    const esp_partition_t *target = NULL;
    eota_result_t result = inspect_slots(policy, image->image_size_bytes, &slots, NULL, &target);
    if (eota_http_deadline_remaining_us(&deadline) <= 0) return EOTA_UPDATE_DOWNLOAD_FAILED;
    if (result != EOTA_UPDATE_OK) return result;
    eota_http_input_t source = {0};
    result = EOTA_UPDATE_RESOURCE_FAILURE;
    source.transport = eota_http_transport_create(&deadline, policy->connect_timeout_ms,
                                                   policy->trusted_time);
    if (source.transport == NULL) goto cleanup;
    const esp_http_client_config_t http = {
        .url = image->image_url,
        .transport = source.transport,
        .disable_auto_redirect = true,
        .timeout_ms = (int)policy->read_timeout_ms,
        .buffer_size = 1024,
    };
    source.client = esp_http_client_init(&http);
    if (source.client == NULL) goto cleanup;
    result = EOTA_UPDATE_DOWNLOAD_FAILED;
    if (eota_http_deadline_remaining_us(&deadline) <= 0 ||
        esp_http_client_open(source.client, 0) != ESP_OK ||
        eota_http_deadline_remaining_us(&deadline) <= 0) goto cleanup;
    int64_t content_length;
    do {
        if (eota_http_deadline_remaining_us(&deadline) <= 0) goto cleanup;
        content_length = esp_http_client_fetch_headers(source.client);
        if (eota_http_deadline_remaining_us(&deadline) <= 0) goto cleanup;
    } while (content_length == -ESP_ERR_HTTP_EAGAIN);
    if (content_length != image->image_size_bytes ||
        esp_http_client_get_status_code(source.client) != 200 ||
        esp_http_client_is_chunked_response(source.client) ||
        esp_http_client_get_content_length(source.client) != image->image_size_bytes) goto cleanup;
    const eota_input_t input = {
        .read = read_http_input, .finish = finish_http_input, .context = &source,
    };
    result = receive_image(policy, image->image_size_bytes, image->sha256, &deadline,
                             &slots, target, &input, progress, context, prepared);
cleanup:
    close_http_input(&source);
    return result;
#endif
}

eota_result_t eota_prepare_stream(const eota_policy_t *policy, const eota_stream_t *stream,
                                  eota_progress_t progress, void *context,
                                  eota_prepared_t *prepared)
{
    if (prepared != NULL) memset(prepared, 0, sizeof *prepared);
#if !EOTA_SIGNED_ENABLED
    (void)policy; (void)stream; (void)progress; (void)context; (void)prepared;
    return EOTA_UPDATE_UNSUPPORTED;
#else
    if (!valid_policy(policy) || !policy->trusted_time || prepared == NULL) {
        return EOTA_UPDATE_INVALID_REQUEST;
    }
    const eota_result_t request_result = eota_validate_stream_request(stream);
    if (request_result != EOTA_UPDATE_OK) return request_result;
    eota_http_deadline_t deadline = {0};
    if (!eota_http_deadline_init(&deadline, policy->total_timeout_ms,
                                 policy->idle_timeout_ms)) return EOTA_UPDATE_RESOURCE_FAILURE;
    eota_slots_t slots;
    const esp_partition_t *target = NULL;
    const eota_result_t result = inspect_slots(policy, stream->image_size_bytes,
                                                &slots, NULL, &target);
    if (eota_http_deadline_remaining_us(&deadline) <= 0) return EOTA_UPDATE_DOWNLOAD_FAILED;
    if (result != EOTA_UPDATE_OK) return result;
    eota_stream_input_t source = {.stream = stream, .policy = policy, .deadline = &deadline};
    const eota_input_t input = {
        .read = read_stream_input, .finish = finish_stream_input, .context = &source,
    };
    return receive_image(policy, stream->image_size_bytes, stream->sha256, &deadline,
                            &slots, target, &input, progress, context, prepared);
#endif
}

eota_result_t eota_select(const eota_policy_t *policy, const eota_prepared_t *prepared)
{
#if !EOTA_SIGNED_ENABLED
    (void)policy; (void)prepared;
    return EOTA_UPDATE_UNSUPPORTED;
#else
    if (prepared == NULL || prepared->image_size_bytes < EOTA_PREFIX_BYTES) {
        return EOTA_UPDATE_INVALID_REQUEST;
    }
    eota_slots_t slots;
    const esp_partition_t *running = NULL;
    const esp_partition_t *target = NULL;
    eota_result_t result = inspect_slots(policy, prepared->image_size_bytes, &slots,
                                         &running, &target);
    if (result != EOTA_UPDATE_OK) return result;
    if (!same_slots(&slots, &prepared->slots)) return EOTA_UPDATE_SLOT_UNAVAILABLE;
    uint8_t digest[EOTA_SHA256_BYTES];
    result = hash_partition(policy, target, prepared->image_size_bytes, digest, NULL);
    if (result != EOTA_UPDATE_OK) return result;
    if (memcmp(digest, prepared->sha256, sizeof digest) != 0) return EOTA_UPDATE_HASH_MISMATCH;
    uint8_t prefix[EOTA_PREFIX_BYTES];
    result = flash_read_partition(policy, target, 0, prefix, sizeof prefix);
    if (result != EOTA_UPDATE_OK) return result;
    if (!matches_image_target(policy, prefix)) return EOTA_UPDATE_WRONG_TARGET;
    result = verify_image_size(policy, target, prepared->image_size_bytes, NULL);
    if (result != EOTA_UPDATE_OK) return result;
    if (!flash_io_acquire(policy)) return EOTA_UPDATE_BOOT_STATE_UNKNOWN;
    const esp_err_t select = esp_ota_set_boot_partition(target);
    if (!flash_io_release(policy)) return EOTA_UPDATE_BOOT_STATE_UNKNOWN;
    if (!flash_io_acquire(policy)) return EOTA_UPDATE_BOOT_STATE_UNKNOWN;
    const bool target_selected = same_partition(esp_ota_get_boot_partition(), target);
    const bool rollback_possible = select == ESP_OK && target_selected &&
        esp_ota_check_rollback_is_possible();
    if (!flash_io_release(policy)) return EOTA_UPDATE_BOOT_STATE_UNKNOWN;
    if (rollback_possible) {
        return EOTA_UPDATE_OK;
    }
    /* A failed selector write may have reached otadata even if readback still
     * names the running slot. Restore and inspect both durable slot states. */
    bool running_selected = false;
    if (boot_matches(policy, running, &running_selected) != EOTA_UPDATE_OK) {
        return EOTA_UPDATE_BOOT_STATE_UNKNOWN;
    }
    if (!running_selected) {
        if (!flash_io_acquire(policy)) return EOTA_UPDATE_BOOT_STATE_UNKNOWN;
        (void)esp_ota_set_boot_partition(running);
        if (!flash_io_release(policy)) return EOTA_UPDATE_BOOT_STATE_UNKNOWN;
    }
    if (boot_matches(policy, running, &running_selected) != EOTA_UPDATE_OK ||
        !running_selected) {
        return EOTA_UPDATE_BOOT_STATE_UNKNOWN;
    }
    esp_ota_img_states_t running_state;
    esp_err_t state_status;
    if (read_raw_state(policy, running, &running_state, &state_status) != EOTA_UPDATE_OK ||
        state_status != ESP_OK) {
        return EOTA_UPDATE_BOOT_STATE_UNKNOWN;
    }
    if (running_state == ESP_OTA_IMG_NEW) {
        /* ESP-IDF marks a selected app NEW, including the old running app
         * when it is reselected after a failed target selection. */
        if (!flash_io_acquire(policy)) return EOTA_UPDATE_BOOT_STATE_UNKNOWN;
        (void)esp_ota_mark_app_valid_cancel_rollback();
        if (!flash_io_release(policy)) return EOTA_UPDATE_BOOT_STATE_UNKNOWN;
        if (read_raw_state(policy, running, &running_state, &state_status) != EOTA_UPDATE_OK ||
            state_status != ESP_OK ||
            running_state != ESP_OTA_IMG_VALID) {
            return EOTA_UPDATE_BOOT_STATE_UNKNOWN;
        }
    } else if (running_state != ESP_OTA_IMG_VALID) {
        return EOTA_UPDATE_BOOT_STATE_UNKNOWN;
    }
    esp_ota_img_states_t target_state;
    if (read_raw_state(policy, target, &target_state, &state_status) != EOTA_UPDATE_OK) {
        return EOTA_UPDATE_BOOT_STATE_UNKNOWN;
    }
    if (state_status == ESP_OK &&
        (target_state == ESP_OTA_IMG_NEW || target_state == ESP_OTA_IMG_PENDING_VERIFY)) {
        /* The failed selection left an unbooted candidate in otadata. */
        if (!flash_io_acquire(policy)) return EOTA_UPDATE_BOOT_STATE_UNKNOWN;
        (void)esp_ota_invalidate_inactive_ota_data_slot();
        if (!flash_io_release(policy)) return EOTA_UPDATE_BOOT_STATE_UNKNOWN;
    }
    eota_slots_t restored_slots;
    if (inspect_slots(policy, prepared->image_size_bytes, &restored_slots,
                      NULL, NULL) != EOTA_UPDATE_OK) {
        return EOTA_UPDATE_BOOT_STATE_UNKNOWN;
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
    if (!flash_io_acquire(policy)) return EOTA_UPDATE_RESOURCE_FAILURE;
    const esp_partition_t *running = esp_ota_get_running_partition();
    if (!flash_io_release(policy)) return EOTA_UPDATE_BOOT_STATE_UNKNOWN;
    if (!expected_slot(running, policy)) return EOTA_UPDATE_SLOT_UNAVAILABLE;
    if (size_bytes > running->size) return EOTA_UPDATE_TOO_LARGE;
    const eota_result_t result = verify_image_size(policy, running, size_bytes, NULL);
    if (result != EOTA_UPDATE_OK) return result;
    return hash_partition(policy, running, size_bytes, digest, NULL);
#endif
}

eota_result_t eota_sha256_verified_image(const eota_policy_t *policy, uint8_t subtype,
                                         uint32_t *image_size_bytes,
                                         uint8_t digest[EOTA_SHA256_BYTES])
{
    if (image_size_bytes != NULL) *image_size_bytes = 0;
    if (digest != NULL) memset(digest, 0, EOTA_SHA256_BYTES);
    if (image_size_bytes == NULL || digest == NULL) return EOTA_UPDATE_INVALID_REQUEST;
#if !EOTA_SIGNED_ENABLED
    (void)policy; (void)subtype;
    return EOTA_UPDATE_UNSUPPORTED;
#else
    if (!valid_policy(policy) ||
        (subtype != ESP_PARTITION_SUBTYPE_APP_OTA_0 &&
         subtype != ESP_PARTITION_SUBTYPE_APP_OTA_1)) return EOTA_UPDATE_INVALID_REQUEST;
    const esp_partition_t *partition = esp_partition_find_first(
        ESP_PARTITION_TYPE_APP, subtype, NULL);
    if (!expected_slot(partition, policy) || partition->subtype != subtype) {
        return EOTA_UPDATE_SLOT_UNAVAILABLE;
    }
    const esp_partition_pos_t position = {
        .offset = partition->address,
        .size = partition->size,
    };
    esp_image_metadata_t metadata = {0};
    if (!flash_io_acquire(policy)) return EOTA_UPDATE_RESOURCE_FAILURE;
    const esp_err_t verified = esp_image_verify(ESP_IMAGE_VERIFY, &position, &metadata);
    if (!flash_io_release(policy)) return EOTA_UPDATE_BOOT_STATE_UNKNOWN;
    if (verified != ESP_OK) {
        return verified == ESP_ERR_NO_MEM || verified == ESP_ERR_IMAGE_FLASH_FAIL ?
               EOTA_UPDATE_RESOURCE_FAILURE : EOTA_UPDATE_IMAGE_INVALID;
    }
    if (metadata.image_len < EOTA_PREFIX_BYTES || metadata.image_len > partition->size) {
        return EOTA_UPDATE_IMAGE_INVALID;
    }
    uint8_t calculated[EOTA_SHA256_BYTES];
    const eota_result_t result = hash_partition(policy, partition, metadata.image_len,
                                                 calculated, NULL);
    if (result != EOTA_UPDATE_OK) return result;
    memcpy(digest, calculated, sizeof calculated);
    *image_size_bytes = metadata.image_len;
    return EOTA_UPDATE_OK;
#endif
}
