// SPDX-License-Identifier: Apache-2.0
#include "eota.h"
#include "lab_inputs.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "nvs_flash.h"

#define LAB_CONNECTED BIT0
#define LAB_FAILED BIT1

static const char *TAG = "eota_lab";
static EventGroupHandle_t wifi_events;
static unsigned wifi_attempts;

static void wifi_event(void *context, esp_event_base_t base, int32_t id, void *data)
{
    (void)context;
    (void)data;
    if (base == WIFI_EVENT && (id == WIFI_EVENT_STA_START || id == WIFI_EVENT_STA_DISCONNECTED)) {
        if (wifi_attempts++ < 5) {
            (void)esp_wifi_connect();
        } else {
            xEventGroupSetBits(wifi_events, LAB_FAILED);
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        xEventGroupSetBits(wifi_events, LAB_CONNECTED);
    }
}

static bool parse_sha256(const char *hex, uint8_t digest[EOTA_SHA256_BYTES])
{
    if (strlen(hex) != EOTA_SHA256_BYTES * 2) return false;
    for (size_t i = 0; i < EOTA_SHA256_BYTES; ++i) {
        unsigned byte = 0;
        for (unsigned j = 0; j < 2; ++j) {
            const char c = hex[i * 2 + j];
            unsigned nibble;
            if (c >= '0' && c <= '9') nibble = (unsigned)(c - '0');
            else if (c >= 'a' && c <= 'f') nibble = (unsigned)(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') nibble = (unsigned)(c - 'A' + 10);
            else return false;
            byte = (byte << 4) | nibble;
        }
        digest[i] = (uint8_t)byte;
    }
    return true;
}

static bool connect_network(void)
{
    const size_t ssid_size = strlen(EOTA_LAB_WIFI_SSID);
    const size_t password_size = strlen(EOTA_LAB_WIFI_PASSWORD);
    if (ssid_size == 0 || ssid_size > 32 || password_size > 63) return false;
    if (esp_netif_init() != ESP_OK || esp_event_loop_create_default() != ESP_OK ||
        esp_netif_create_default_wifi_sta() == NULL) return false;
    wifi_events = xEventGroupCreate();
    if (wifi_events == NULL) return false;
    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    if (esp_wifi_init(&init) != ESP_OK ||
        esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event, NULL) != ESP_OK ||
        esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event, NULL) != ESP_OK) return false;
    wifi_config_t config = {0};
    memcpy(config.sta.ssid, EOTA_LAB_WIFI_SSID, ssid_size);
    memcpy(config.sta.password, EOTA_LAB_WIFI_PASSWORD, password_size);
    if (esp_wifi_set_mode(WIFI_MODE_STA) != ESP_OK ||
        esp_wifi_set_config(WIFI_IF_STA, &config) != ESP_OK ||
        esp_wifi_start() != ESP_OK) return false;
    const EventBits_t result = xEventGroupWaitBits(wifi_events, LAB_CONNECTED | LAB_FAILED,
                                                    pdFALSE, pdFALSE, pdMS_TO_TICKS(30000));
    return (result & LAB_CONNECTED) != 0;
}

static bool synchronize_time(void)
{
    if (EOTA_LAB_SNTP_SERVER[0] == '\0') return false;
    esp_sntp_config_t config = ESP_NETIF_SNTP_DEFAULT_CONFIG(EOTA_LAB_SNTP_SERVER);
    if (esp_netif_sntp_init(&config) != ESP_OK) return false;
    return esp_netif_sntp_sync_wait(pdMS_TO_TICKS(30000)) == ESP_OK &&
           time(NULL) >= 1704067200;
}

static void progress(uint32_t received_bytes, uint32_t total_bytes, void *context)
{
    (void)context;
    if (received_bytes == total_bytes || received_bytes % (64U * 1024U) == 0) {
        ESP_LOGI(TAG, "received %lu/%lu bytes", (unsigned long)received_bytes,
                 (unsigned long)total_bytes);
    }
}

void app_main(void)
{
    /* An NVS error is never repaired by erasing the device identity. */
    const esp_err_t nvs_result = nvs_flash_init();
    if (nvs_result != ESP_OK) {
        ESP_LOGE(TAG, "NVS init failed: %s", esp_err_to_name(nvs_result));
        return;
    }
    eota_current_t current;
    const esp_err_t inspect_result = eota_inspect(&current);
    if (inspect_result != ESP_OK) {
        ESP_LOGE(TAG, "slot inspection failed: %s", esp_err_to_name(inspect_result));
        return;
    }
    ESP_LOGI(TAG, "running slot %s state %s", current.running_partition,
             eota_state_name(current.state));
    if (!EOTA_LAB_ARMED) {
        ESP_LOGW(TAG, "lab input is not armed; no Flash or otadata write");
        return;
    }
    if (current.state == EOTA_STATE_PENDING_VERIFY) {
        /* Lab local self-test: NVS and slot inspection succeeded. Product
         * firmware must provide its own checks and stable-window policy. */
        vTaskDelay(pdMS_TO_TICKS(30000));
        const esp_err_t confirm = eota_confirm_pending(&current);
        ESP_LOGI(TAG, "pending confirmation: %s", esp_err_to_name(confirm));
        return;
    }
    if (!eota_available()) {
        ESP_LOGE(TAG, "this build has no signed OTA support");
        return;
    }
    if (EOTA_LAB_IMAGE_SIZE_BYTES == 0 || EOTA_LAB_IMAGE_URL[0] == '\0' ||
        !connect_network() || !synchronize_time()) {
        ESP_LOGE(TAG, "lab input, network or trusted time unavailable");
        return;
    }
    const eota_policy_t policy = {
        .project_name = "esp_ota_lab",
        .chip_id = 0x0005, /* ESP32-C3 */
        .ota_0_address_bytes = 0x20000,
        .ota_1_address_bytes = 0x200000,
        .ota_size_bytes = 0x1e0000,
        .connect_timeout_ms = 5000,
        .read_timeout_ms = 1000,
        .idle_timeout_ms = 30000,
        .total_timeout_ms = 300000,
        .trusted_time = true,
    };
    eota_image_t image = {.image_url = EOTA_LAB_IMAGE_URL,
                          .image_size_bytes = EOTA_LAB_IMAGE_SIZE_BYTES};
    if (!parse_sha256(EOTA_LAB_IMAGE_SHA256_HEX, image.sha256)) {
        ESP_LOGE(TAG, "invalid image SHA-256 input");
        return;
    }
    eota_slots_t slots;
    eota_result_t result = eota_preflight(&policy, image.image_size_bytes, &slots);
    if (result == EOTA_UPDATE_OK) {
        ESP_LOGI(TAG, "preflight ota_%u -> ota_%u", slots.running_subtype - 0x10,
                 slots.target_subtype - 0x10);
        eota_prepared_t prepared;
        result = eota_prepare(&policy, &image, progress, NULL, &prepared);
        if (result == EOTA_UPDATE_OK) result = eota_select(&policy, &prepared);
    }
    ESP_LOGI(TAG, "OTA result: %s", eota_error(result));
    if (result == EOTA_UPDATE_OK) esp_restart();
}
