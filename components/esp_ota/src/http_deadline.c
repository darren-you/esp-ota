// SPDX-License-Identifier: Apache-2.0
#include "http_deadline.h"

#include <stdlib.h>
#include <sys/socket.h>

#include "freertos/FreeRTOS.h"

static void expire_socket(void *argument)
{
    eota_http_deadline_t *deadline = argument;
    atomic_store_explicit(&deadline->expired, true, memory_order_release);
    /* shutdown interrupts an in-flight SDK transport read without freeing the
     * client or the socket. The owner joins this callback before cleanup. */
    (void)shutdown(deadline->socket_fd, SHUT_RDWR);
}

bool eota_http_deadline_init(eota_http_deadline_t *deadline, int socket_fd)
{
    if (deadline == NULL || socket_fd < 0) return false;
    deadline->timer = NULL;
    deadline->socket_fd = socket_fd;
    atomic_init(&deadline->expired, false);
    const esp_timer_create_args_t args = {
        .callback = expire_socket,
        .arg = deadline,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "eota_http",
    };
    return esp_timer_create(&args, &deadline->timer) == ESP_OK;
}

bool eota_http_deadline_arm(eota_http_deadline_t *deadline, int64_t started_us,
                            int64_t last_progress_us, uint32_t total_ms, uint32_t idle_ms)
{
    if (deadline == NULL || deadline->timer == NULL ||
        atomic_load_explicit(&deadline->expired, memory_order_acquire)) return false;
    const int64_t now_us = esp_timer_get_time();
    const int64_t total_left = (int64_t)total_ms * 1000 - (now_us - started_us);
    const int64_t idle_left = (int64_t)idle_ms * 1000 - (now_us - last_progress_us);
    if (now_us < started_us || now_us < last_progress_us || total_left <= 0 || idle_left <= 0) {
        return false;
    }
    const uint64_t remaining_us = (uint64_t)(total_left < idle_left ? total_left : idle_left);
    return esp_timer_start_once(deadline->timer, remaining_us) == ESP_OK;
}

bool eota_http_deadline_stop(eota_http_deadline_t *deadline)
{
    if (deadline == NULL || deadline->timer == NULL) return false;
    /* This locked SDK API waits for any in-flight TASK callback. Socket FD
     * reuse after HTTP cleanup is impossible once this call succeeds. */
    if (esp_timer_stop_blocking(deadline->timer, portMAX_DELAY) != ESP_OK) return false;
    return !atomic_load_explicit(&deadline->expired, memory_order_acquire);
}

void eota_http_deadline_destroy(eota_http_deadline_t *deadline)
{
    if (deadline == NULL || deadline->timer == NULL) return;
    /* Failing closed is required here: cleanup of the HTTP socket or this
     * stack-owned callback argument before the callback exits is unsafe. */
    if (esp_timer_stop_blocking(deadline->timer, portMAX_DELAY) != ESP_OK ||
        esp_timer_delete(deadline->timer) != ESP_OK) abort();
    deadline->timer = NULL;
}
