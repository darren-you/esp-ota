#include "eota.h"
#include "esp_app_format.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "http_transport.h"
#include "esp_timer.h"
#include "esp_ota_ops.h"
#include "psa/crypto.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

#define IMAGE_BYTES 1152
#define CHIP_ID 5
#define PREFIX_BYTES (sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t) + sizeof(esp_app_desc_t))

static eota_policy_t policy = {
    .project_name = "esp_base",
    .chip_id = CHIP_ID,
    .ota_0_address_bytes = 0x20000,
    .ota_1_address_bytes = 0x200000,
    .ota_size_bytes = 0x1e0000,
    .connect_timeout_ms = 5000,
    .read_timeout_ms = 1000,
    .idle_timeout_ms = 30000,
    .total_timeout_ms = 300000,
    .trusted_time = true,
};

static const esp_partition_t old_slot = {.type = ESP_PARTITION_TYPE_APP, .subtype = ESP_PARTITION_SUBTYPE_APP_OTA_0, .address = 0x20000, .size = 0x1e0000};
static const esp_partition_t new_slot = {.type = ESP_PARTITION_TYPE_APP, .subtype = ESP_PARTITION_SUBTYPE_APP_OTA_1, .address = 0x200000, .size = 0x1e0000};
static const esp_partition_t wrong_slot = {.type = ESP_PARTITION_TYPE_APP, .subtype = ESP_PARTITION_SUBTYPE_APP_OTA_1, .address = 0x210000, .size = 0x1e0000};
static const esp_partition_t *boot, *selected_slot;
static uint8_t image_bytes[IMAGE_BYTES], staged_bytes[IMAGE_BYTES];
static bool valid_old, complete, rollback_possible, bad_chip, fail_restore, fail_read, fail_select;
static bool fail_mark, mark_then_fail, fail_invalidate;
static esp_ota_img_states_t old_state;
static esp_ota_img_states_t target_state;
static esp_err_t target_lookup;
static bool stall_headers, stall_first_byte, stall_midbody, early_fin, fin_midbody, select_then_fail;
static bool select_then_fail_without_switch;
static bool slow_drip_headers, slow_drip_body;
static int status_code, init_calls, open_calls, header_calls, read_calls, cleanup_calls;
static int transport_create_calls, transport_destroy_calls;
static int begin_calls, write_calls, end_calls, abort_calls, select_calls, restore_calls, mark_calls, invalidate_calls, partition_reads;
static int64_t content_length, now_us, read_advance_us;
static esp_err_t end_result;
static size_t stream_offset, staged_size;
static uint32_t last_progress;
struct esp_timer_fake {
    void (*callback)(void *);
    void *argument;
    int64_t alarm_us;
    bool active;
};
static struct esp_timer_fake fake_timer;
static int deadline_callbacks;
struct esp_transport_fake {
    bool alive;
    eota_http_deadline_t *deadline;
    int64_t started_us;
    int64_t *last_progress_us;
};
static struct esp_transport_fake fake_transport;

static void advance_time(int64_t delta_us)
{
    now_us += delta_us;
    if (fake_timer.active && now_us >= fake_timer.alarm_us) {
        fake_timer.active = false;
        deadline_callbacks++;
        fake_timer.callback(fake_timer.argument);
    }
}

static void reset(void)
{
    assert(!fake_transport.alive && transport_create_calls == transport_destroy_calls);
    boot = &old_slot;
    selected_slot = &new_slot;
    valid_old = complete = rollback_possible = true;
    old_state = ESP_OTA_IMG_VALID;
    target_state = ESP_OTA_IMG_UNDEFINED;
    target_lookup = ESP_ERR_NOT_FOUND;
    bad_chip = fail_restore = fail_read = fail_select = false;
    fail_mark = mark_then_fail = fail_invalidate = false;
    stall_headers = stall_first_byte = stall_midbody = early_fin = fin_midbody = select_then_fail = false;
    select_then_fail_without_switch = false;
    slow_drip_headers = slow_drip_body = false;
    status_code = 200;
    content_length = IMAGE_BYTES;
    end_result = ESP_OK;
    init_calls = open_calls = header_calls = read_calls = cleanup_calls = 0;
    transport_create_calls = transport_destroy_calls = 0;
    begin_calls = write_calls = end_calls = abort_calls = select_calls = restore_calls = mark_calls = invalidate_calls = partition_reads = 0;
    now_us = read_advance_us = 0;
    stream_offset = staged_size = 0;
    last_progress = 0;
    deadline_callbacks = 0;
    memset(&fake_timer, 0, sizeof fake_timer);
    memset(&fake_transport, 0, sizeof fake_transport);
    memset(image_bytes, 0x5a, sizeof image_bytes);
    memset(staged_bytes, 0, sizeof staged_bytes);
    esp_image_header_t header = {.magic = ESP_IMAGE_HEADER_MAGIC, .chip_id = CHIP_ID};
    memcpy(image_bytes, &header, sizeof header);
    esp_app_desc_t desc = {.magic_word = ESP_APP_DESC_MAGIC_WORD};
    strcpy(desc.project_name, "esp_base");
    memcpy(image_bytes + sizeof header + sizeof(esp_image_segment_header_t), &desc, sizeof desc);
}

static void digest(eota_image_t *request)
{
    uint32_t sum = 0;
    for (size_t i = 0; i < sizeof image_bytes; ++i) sum += image_bytes[i];
    for (size_t i = 0; i < sizeof request->sha256; ++i) request->sha256[i] = (uint8_t)(sum + i);
}

int64_t esp_timer_get_time(void) { return now_us; }
esp_err_t esp_timer_create(const esp_timer_create_args_t *args, esp_timer_handle_t *out)
{
    assert(args && args->callback && args->dispatch_method == ESP_TIMER_TASK && out);
    fake_timer.callback = args->callback;
    fake_timer.argument = args->arg;
    *out = &fake_timer;
    return ESP_OK;
}
esp_err_t esp_timer_start_once(esp_timer_handle_t timer, uint64_t timeout_us)
{
    assert(timer == &fake_timer && !timer->active && timeout_us > 0);
    timer->active = true;
    timer->alarm_us = now_us + (int64_t)timeout_us;
    return ESP_OK;
}
esp_err_t esp_timer_stop_blocking(esp_timer_handle_t timer, uint32_t timeout_ticks)
{
    assert(timer == &fake_timer && timeout_ticks == UINT32_MAX);
    timer->active = false;
    return ESP_OK;
}
esp_err_t esp_timer_delete(esp_timer_handle_t timer)
{ assert(timer == &fake_timer && !timer->active && transport_destroy_calls == 0); return ESP_OK; }
int esp_crt_bundle_attach(void *config) { (void)config; return 0; }
const esp_partition_t *esp_ota_get_running_partition(void) { return &old_slot; }
const esp_partition_t *esp_ota_get_boot_partition(void) { return boot; }
const esp_partition_t *esp_ota_get_next_update_partition(const esp_partition_t *partition)
{ assert(partition == NULL); return selected_slot; }
esp_err_t esp_ota_get_state_partition(const esp_partition_t *partition, esp_ota_img_states_t *state)
{
    assert(partition == &old_slot || partition == &new_slot);
    if (partition == &old_slot) { *state = valid_old ? old_state : ESP_OTA_IMG_PENDING_VERIFY; return ESP_OK; }
    *state = target_state;
    return target_lookup;
}
bool esp_ota_check_rollback_is_possible(void) { return rollback_possible; }
esp_err_t esp_ota_check_image_validity(int type, const esp_image_header_t *header, const esp_app_desc_t *desc)
{
    assert(type == ESP_PARTITION_TYPE_APP && header->magic == ESP_IMAGE_HEADER_MAGIC);
    assert(desc->magic_word == ESP_APP_DESC_MAGIC_WORD);
    return bad_chip ? ESP_FAIL : ESP_OK;
}
esp_err_t esp_ota_begin(const esp_partition_t *partition, size_t size, esp_ota_handle_t *handle)
{
    assert(partition == &new_slot && size == IMAGE_BYTES && staged_size == 0);
    ++begin_calls;
    *handle = 1;
    return ESP_OK;
}
esp_err_t esp_ota_write(esp_ota_handle_t handle, const void *data, size_t size)
{
    assert(handle == 1 && staged_size + size <= sizeof staged_bytes);
    memcpy(staged_bytes + staged_size, data, size);
    staged_size += size;
    ++write_calls;
    return ESP_OK;
}
esp_err_t esp_ota_end(esp_ota_handle_t handle)
{ assert(handle == 1 && staged_size == IMAGE_BYTES); ++end_calls; return end_result; }
esp_err_t esp_ota_abort(esp_ota_handle_t handle)
{ assert(handle == 1); ++abort_calls; return ESP_OK; }
esp_err_t esp_ota_set_boot_partition(const esp_partition_t *partition)
{
    if (partition == &new_slot) {
        ++select_calls;
        if (select_then_fail_without_switch) {
            target_state = ESP_OTA_IMG_NEW;
            target_lookup = ESP_OK;
            return ESP_FAIL;
        }
        if (select_then_fail) { boot = &new_slot; target_state = ESP_OTA_IMG_NEW; target_lookup = ESP_OK; return ESP_FAIL; }
        if (fail_select) return ESP_FAIL;
        boot = &new_slot;
        target_state = ESP_OTA_IMG_NEW;
        target_lookup = ESP_OK;
        return ESP_OK;
    }
    assert(partition == &old_slot);
    ++restore_calls;
    if (fail_restore) return ESP_FAIL;
    boot = &old_slot;
    old_state = ESP_OTA_IMG_NEW;
    return ESP_OK;
}
esp_err_t esp_ota_mark_app_valid_cancel_rollback(void)
{
    ++mark_calls;
    if (!fail_mark) old_state = ESP_OTA_IMG_VALID;
    return fail_mark || mark_then_fail ? ESP_FAIL : ESP_OK;
}
esp_err_t esp_ota_invalidate_inactive_ota_data_slot(void)
{
    ++invalidate_calls;
    if (fail_invalidate) return ESP_FAIL;
    target_state = ESP_OTA_IMG_UNDEFINED;
    target_lookup = ESP_ERR_NOT_FOUND;
    return ESP_OK;
}
esp_err_t esp_partition_read(const esp_partition_t *partition, size_t offset, void *data, size_t size)
{
    assert(partition == &old_slot || partition == &new_slot);
    ++partition_reads;
    if (fail_read) return ESP_FAIL;
    if (partition == &old_slot) {
        assert(offset + size <= sizeof image_bytes);
        memcpy(data, image_bytes + offset, size);
    } else {
        assert(offset + size <= staged_size);
        memcpy(data, staged_bytes + offset, size);
    }
    return ESP_OK;
}
esp_http_client_handle_t esp_http_client_init(const esp_http_client_config_t *config)
{
    ++init_calls;
    assert(config->url != NULL && config->transport == &fake_transport);
    assert(fake_transport.alive && fake_timer.active && transport_create_calls == 1);
    assert(config->disable_auto_redirect && config->timeout_ms == (int)policy.read_timeout_ms);
    assert(config->buffer_size == 1024);
    return (void *)1;
}
esp_transport_handle_t eota_http_transport_create(eota_http_deadline_t *deadline,
                                                   int64_t started_us,
                                                   int64_t *last_progress_us,
                                                   uint32_t total_timeout_ms,
                                                   uint32_t idle_timeout_ms,
                                                   uint32_t connect_timeout_ms)
{
    assert(deadline && deadline->timer == &fake_timer && fake_timer.active);
    assert(started_us == now_us && last_progress_us && *last_progress_us == started_us);
    assert(total_timeout_ms == policy.total_timeout_ms &&
           idle_timeout_ms == policy.idle_timeout_ms &&
           connect_timeout_ms == policy.connect_timeout_ms);
    assert(!fake_transport.alive && transport_create_calls == 0);
    ++transport_create_calls;
    fake_transport.alive = true;
    fake_transport.deadline = deadline;
    fake_transport.started_us = started_us;
    fake_transport.last_progress_us = last_progress_us;
    return &fake_transport;
}
esp_err_t esp_transport_destroy(esp_transport_handle_t transport)
{
    assert(transport == &fake_transport && fake_transport.alive);
    assert(!fake_timer.active && cleanup_calls == 1 && transport_destroy_calls == 0);
    fake_transport.alive = false;
    ++transport_destroy_calls;
    return ESP_OK;
}
esp_err_t esp_http_client_open(esp_http_client_handle_t client, int write_len)
{ assert(client && write_len == 0); ++open_calls; return ESP_OK; }
int64_t esp_http_client_fetch_headers(esp_http_client_handle_t client)
{
    assert(client);
    ++header_calls;
    if (slow_drip_headers) {
        /* Model one SDK call that never yields while receiving a byte/sec. */
        for (int i = 0; i < 600 && deadline_callbacks == 0; ++i) advance_time(INT64_C(1000000));
        return -ESP_ERR_HTTP_EAGAIN;
    }
    if (stall_headers) { advance_time(INT64_C(1000000)); return -ESP_ERR_HTTP_EAGAIN; }
    return content_length;
}
int esp_http_client_get_status_code(esp_http_client_handle_t client) { assert(client); return status_code; }
int64_t esp_http_client_get_content_length(esp_http_client_handle_t client) { assert(client); return content_length; }
bool esp_http_client_is_chunked_response(esp_http_client_handle_t client) { assert(client); return content_length < 0; }
int esp_http_client_read(esp_http_client_handle_t client, char *buffer, int len)
{
    assert(client && buffer && len > 0 && len <= 64);
    ++read_calls;
    advance_time(read_advance_us);
    if (slow_drip_body && stream_offset >= PREFIX_BYTES) {
        for (int i = 0; i < 600 && deadline_callbacks == 0; ++i) advance_time(INT64_C(1000000));
        return -ESP_ERR_HTTP_EAGAIN;
    }
    if ((stall_first_byte && stream_offset == 0) || (stall_midbody && stream_offset >= PREFIX_BYTES)) {
        advance_time(INT64_C(1000000));
        return -ESP_ERR_HTTP_EAGAIN;
    }
    if (early_fin || (fin_midbody && stream_offset >= PREFIX_BYTES) || stream_offset >= sizeof image_bytes) return 0;
    size_t count = (size_t)len < 16 ? (size_t)len : 16;
    if (count > sizeof image_bytes - stream_offset) count = sizeof image_bytes - stream_offset;
    memcpy(buffer, image_bytes + stream_offset, count);
    stream_offset += count;
    if (!eota_http_deadline_progress(fake_transport.deadline, fake_transport.started_us,
                                     fake_transport.last_progress_us,
                                     policy.total_timeout_ms, policy.idle_timeout_ms)) {
        return -ESP_ERR_HTTP_EAGAIN;
    }
    return (int)count;
}
bool esp_http_client_is_complete_data_received(esp_http_client_handle_t client)
{ assert(client); return complete && stream_offset == sizeof image_bytes; }
esp_err_t esp_http_client_cleanup(esp_http_client_handle_t client)
{ assert(client && fake_transport.deadline->timer == NULL && !fake_timer.active && fake_transport.alive && cleanup_calls == 0); ++cleanup_calls; return ESP_OK; }
psa_status_t psa_hash_setup(psa_hash_operation_t *operation, int algorithm)
{ assert(algorithm == PSA_ALG_SHA_256); operation->sum = 0; return PSA_SUCCESS; }
psa_status_t psa_crypto_init(void) { return PSA_SUCCESS; }
psa_status_t psa_hash_update(psa_hash_operation_t *operation, const uint8_t *bytes, size_t length)
{ for (size_t i = 0; i < length; ++i) operation->sum += bytes[i]; return PSA_SUCCESS; }
psa_status_t psa_hash_finish(psa_hash_operation_t *operation, uint8_t *out, size_t out_size, size_t *actual)
{ assert(out_size == 32); for (size_t i = 0; i < 32; ++i) out[i] = (uint8_t)(operation->sum + i); *actual = 32; return PSA_SUCCESS; }
psa_status_t psa_hash_abort(psa_hash_operation_t *operation) { (void)operation; return PSA_SUCCESS; }
static void progress(uint32_t received, uint32_t total, void *context)
{ (void)context; assert(total == IMAGE_BYTES && received <= total); last_progress = received; }

static eota_result_t run_update(const eota_image_t *request)
{
    eota_prepared_t prepared;
    eota_result_t result = eota_prepare(&policy, request, progress, NULL, &prepared);
    return result == EOTA_UPDATE_OK ? eota_select(&policy, &prepared) : result;
}

int main(void)
{
    eota_image_t request = {.image_url = "https://example.test/esp-base.bin", .image_size_bytes = IMAGE_BYTES};
    assert(eota_available());
    reset(); digest(&request);
    eota_slots_t slots;
    assert(eota_preflight(&policy, IMAGE_BYTES, &slots) == EOTA_UPDATE_OK);
    assert(slots.running_subtype == ESP_PARTITION_SUBTYPE_APP_OTA_0 &&
           slots.boot_subtype == ESP_PARTITION_SUBTYPE_APP_OTA_0 &&
           slots.target_subtype == ESP_PARTITION_SUBTYPE_APP_OTA_1 &&
           slots.running_address_bytes == 0x20000 && slots.target_address_bytes == 0x200000 &&
           slots.target_size_bytes == 0x1e0000 && slots.running_state == EOTA_STATE_VALID &&
           slots.target_state == EOTA_STATE_UNTRACKED);
    eota_prepared_t prepared;
    assert(eota_prepare(&policy, &request, progress, NULL, &prepared) == EOTA_UPDATE_OK);
    assert(boot == &old_slot && select_calls == 0 && prepared.image_size_bytes == IMAGE_BYTES);
    assert(eota_select(&policy, &prepared) == EOTA_UPDATE_OK);
    assert(begin_calls == 1 && write_calls > 1 && staged_size == IMAGE_BYTES);
    assert(memcmp(staged_bytes, image_bytes, IMAGE_BYTES) == 0);
    assert(partition_reads == 4 && end_calls == 1 && abort_calls == 0 && cleanup_calls == 1);
    assert(select_calls == 1 && last_progress == IMAGE_BYTES && boot == &new_slot);
    reset(); boot = &new_slot;
    assert(eota_observe_slots(&policy, &slots) == EOTA_UPDATE_OK &&
           slots.boot_subtype == ESP_PARTITION_SUBTYPE_APP_OTA_1 &&
           slots.running_subtype == ESP_PARTITION_SUBTYPE_APP_OTA_0);
    assert(eota_preflight(&policy, IMAGE_BYTES, &slots) == EOTA_UPDATE_SLOT_UNAVAILABLE);
    reset(); digest(&request);
    policy.trusted_time = false;
    assert(eota_prepare(&policy, &request, progress, NULL, &prepared) == EOTA_UPDATE_INVALID_REQUEST &&
           init_calls == 0);
    policy.trusted_time = true;
    policy.project_name[0] = '\0';
    assert(eota_preflight(&policy, IMAGE_BYTES, &slots) == EOTA_UPDATE_INVALID_REQUEST);
    strcpy(policy.project_name, "esp_base");
    policy.ota_1_address_bytes++;
    assert(eota_preflight(&policy, IMAGE_BYTES, &slots) == EOTA_UPDATE_SLOT_UNAVAILABLE);
    policy.ota_1_address_bytes--;
    assert(eota_prepare(&policy, &request, progress, NULL, &prepared) == EOTA_UPDATE_OK);
    staged_bytes[IMAGE_BYTES - 1] ^= 1;
    assert(eota_select(&policy, &prepared) == EOTA_UPDATE_HASH_MISMATCH &&
           boot == &old_slot && select_calls == 0);
    reset(); digest(&request);
    uint8_t running_digest[32];
    assert(eota_sha256_running(&policy, IMAGE_BYTES, running_digest) == EOTA_UPDATE_OK &&
           memcmp(running_digest, request.sha256, sizeof running_digest) == 0);
    assert(eota_sha256_running(&policy, old_slot.size + 1, running_digest) == EOTA_UPDATE_TOO_LARGE);
    reset(); valid_old = false;
    assert(run_update(&request) == EOTA_UPDATE_SLOT_UNAVAILABLE && init_calls == 0);
    reset(); target_lookup = ESP_OK; target_state = ESP_OTA_IMG_NEW;
    assert(run_update(&request) == EOTA_UPDATE_SLOT_UNAVAILABLE && init_calls == 0);
    reset(); target_lookup = ESP_OK; target_state = ESP_OTA_IMG_PENDING_VERIFY;
    assert(run_update(&request) == EOTA_UPDATE_SLOT_UNAVAILABLE && init_calls == 0);
    reset(); target_lookup = ESP_FAIL;
    assert(run_update(&request) == EOTA_UPDATE_SLOT_UNAVAILABLE && init_calls == 0);
    reset(); target_lookup = ESP_OK; target_state = ESP_OTA_IMG_VALID;
    assert(run_update(&request) == EOTA_UPDATE_OK && boot == &new_slot);
    reset(); selected_slot = &wrong_slot;
    assert(run_update(&request) == EOTA_UPDATE_SLOT_UNAVAILABLE && init_calls == 0);
    reset(); request.image_size_bytes = new_slot.size + 1;
    assert(run_update(&request) == EOTA_UPDATE_TOO_LARGE && init_calls == 0);
    request.image_size_bytes = IMAGE_BYTES;
    reset(); image_bytes[sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t) + 48] = 'x';
    assert(run_update(&request) == EOTA_UPDATE_WRONG_TARGET && begin_calls == 0 && select_calls == 0);
    reset(); bad_chip = true;
    assert(run_update(&request) == EOTA_UPDATE_WRONG_TARGET && begin_calls == 0);
    reset(); image_bytes[0] = 0;
    assert(run_update(&request) == EOTA_UPDATE_WRONG_TARGET && begin_calls == 0);
    reset(); content_length = -1;
    assert(run_update(&request) == EOTA_UPDATE_DOWNLOAD_FAILED && read_calls == 0 && begin_calls == 0);
    reset(); status_code = 302;
    assert(run_update(&request) == EOTA_UPDATE_DOWNLOAD_FAILED && begin_calls == 0);
    reset(); stall_headers = true;
    assert(run_update(&request) == EOTA_UPDATE_DOWNLOAD_FAILED && header_calls == 30 && read_calls == 0 && cleanup_calls == 1);
    reset(); slow_drip_headers = true;
    assert(run_update(&request) == EOTA_UPDATE_DOWNLOAD_FAILED && header_calls == 1 &&
           deadline_callbacks == 1 && now_us == INT64_C(30000000) && cleanup_calls == 1);
    reset(); stall_first_byte = true;
    assert(run_update(&request) == EOTA_UPDATE_DOWNLOAD_FAILED && read_calls == 30 && begin_calls == 0 && cleanup_calls == 1);
    reset(); stall_midbody = true;
    assert(run_update(&request) == EOTA_UPDATE_DOWNLOAD_FAILED && begin_calls == 1 && abort_calls == 1 && end_calls == 0);
    reset(); slow_drip_body = true;
    assert(run_update(&request) == EOTA_UPDATE_DOWNLOAD_FAILED && deadline_callbacks == 1 &&
           now_us == INT64_C(30000000) && begin_calls == 1 && abort_calls == 1);
    reset(); early_fin = true;
    assert(run_update(&request) == EOTA_UPDATE_DOWNLOAD_FAILED && begin_calls == 0 && cleanup_calls == 1);
    reset(); fin_midbody = true;
    assert(run_update(&request) == EOTA_UPDATE_DOWNLOAD_FAILED && begin_calls == 1 && abort_calls == 1 && select_calls == 0);
    reset(); complete = false;
    assert(run_update(&request) == EOTA_UPDATE_DOWNLOAD_FAILED && abort_calls == 1 && end_calls == 0);
    reset(); read_advance_us = INT64_C(25000000);
    assert(run_update(&request) == EOTA_UPDATE_DOWNLOAD_FAILED && now_us >= INT64_C(300000000) && end_calls == 0);
    reset(); request.sha256[0] ^= 1;
    assert(run_update(&request) == EOTA_UPDATE_HASH_MISMATCH && abort_calls == 1 && end_calls == 0);
    request.sha256[0] ^= 1;
    reset(); fail_read = true;
    assert(run_update(&request) == EOTA_UPDATE_RESOURCE_FAILURE && abort_calls == 1 && end_calls == 0);
    reset(); end_result = ESP_ERR_OTA_VALIDATE_FAILED;
    assert(run_update(&request) == EOTA_UPDATE_SIGNATURE_INVALID && end_calls == 1 && select_calls == 0 && boot == &old_slot);
    reset(); fail_select = true;
    assert(run_update(&request) == EOTA_UPDATE_SLOT_UNAVAILABLE && select_calls == 1 && boot == &old_slot);
    reset(); select_then_fail = true;
    assert(run_update(&request) == EOTA_UPDATE_SLOT_UNAVAILABLE && restore_calls == 1 && boot == &old_slot &&
           old_state == ESP_OTA_IMG_VALID && mark_calls == 1 && invalidate_calls == 1);
    assert(eota_preflight(&policy, IMAGE_BYTES, &slots) == EOTA_UPDATE_OK);
    reset(); select_then_fail_without_switch = true;
    assert(run_update(&request) == EOTA_UPDATE_SLOT_UNAVAILABLE && restore_calls == 0 &&
           boot == &old_slot && old_state == ESP_OTA_IMG_VALID && mark_calls == 0 &&
           invalidate_calls == 1);
    assert(eota_preflight(&policy, IMAGE_BYTES, &slots) == EOTA_UPDATE_OK);
    reset(); rollback_possible = false;
    assert(run_update(&request) == EOTA_UPDATE_SLOT_UNAVAILABLE && restore_calls == 1 && boot == &old_slot &&
           old_state == ESP_OTA_IMG_VALID && mark_calls == 1 && invalidate_calls == 1);
    reset(); select_then_fail = true; fail_mark = true;
    assert(run_update(&request) == EOTA_UPDATE_BOOT_STATE_UNKNOWN && boot == &old_slot &&
           old_state == ESP_OTA_IMG_NEW && mark_calls == 1);
    reset(); select_then_fail = true; mark_then_fail = true;
    assert(run_update(&request) == EOTA_UPDATE_SLOT_UNAVAILABLE && boot == &old_slot &&
           old_state == ESP_OTA_IMG_VALID && mark_calls == 1 && invalidate_calls == 1);
    reset(); select_then_fail = true; fail_invalidate = true;
    assert(run_update(&request) == EOTA_UPDATE_BOOT_STATE_UNKNOWN && boot == &old_slot &&
           old_state == ESP_OTA_IMG_VALID && target_state == ESP_OTA_IMG_NEW && invalidate_calls == 1);
    reset(); rollback_possible = false; fail_restore = true;
    assert(run_update(&request) == EOTA_UPDATE_BOOT_STATE_UNKNOWN && restore_calls == 1 && boot == &new_slot);
    assert(!fake_transport.alive && transport_create_calls == transport_destroy_calls);

    /* Timer task scheduling may lag behind a byte delivered after the old
     * idle deadline. Such a byte cannot reset the idle clock. */
    reset();
    eota_http_deadline_t delayed_timer = {0};
    assert(eota_http_deadline_init(&delayed_timer, -1));
    int64_t delayed_progress_us = now_us;
    assert(eota_http_deadline_arm(&delayed_timer, now_us, delayed_progress_us, 300000, 30000));
    now_us = INT64_C(30000001);
    assert(!eota_http_deadline_progress(&delayed_timer, 0, &delayed_progress_us,
                                        300000, 30000));
    assert(delayed_progress_us == 0 && deadline_callbacks == 0);
    eota_http_deadline_destroy(&delayed_timer);
    puts("  ota_update passed (EAGAIN return, prefix, hash, signature, selector recovery; fake SDK)");
}
