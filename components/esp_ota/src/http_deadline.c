// SPDX-License-Identifier: Apache-2.0
#include "http_deadline.h"

#include <stdlib.h>

#include "freertos/FreeRTOS.h"

static void expire_deadline(void *argument)
{
    eota_http_deadline_t *deadline = argument;
    /* lwIP shutdown waits for the TCP/IP thread without a deadline. Timer
     * callbacks must not enter that path; nonblocking I/O and finite select
     * waits observe the same absolute expiry on the owner task. */
    atomic_store_explicit(&deadline->expired, true, memory_order_release);
}

bool eota_http_deadline_init(eota_http_deadline_t *deadline)
{
    if (deadline == NULL) return false;
    deadline->timer = NULL;
    atomic_init(&deadline->expired, false);
    const esp_timer_create_args_t args = {
        .callback = expire_deadline,
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
    /* This locked SDK API waits for any in-flight TASK callback before its
     * stack-owned argument can be released. */
    if (esp_timer_stop_blocking(deadline->timer, portMAX_DELAY) != ESP_OK) return false;
    return !atomic_load_explicit(&deadline->expired, memory_order_acquire);
}

bool eota_http_deadline_alive(const eota_http_deadline_t *deadline)
{
    return deadline != NULL && deadline->timer != NULL &&
           !atomic_load_explicit(&deadline->expired, memory_order_acquire);
}

bool eota_http_deadline_progress(eota_http_deadline_t *deadline, int64_t started_us,
                                int64_t *last_progress_us, uint32_t total_ms, uint32_t idle_ms)
{
    if (last_progress_us == NULL || !eota_http_deadline_stop(deadline)) return false;
    const int64_t now_us = esp_timer_get_time();
    /* A delayed timer callback must not let a late byte erase the old idle
     * deadline. Check the old timestamp before recording this progress. */
    if (now_us < started_us || now_us < *last_progress_us ||
        now_us - started_us >= (int64_t)total_ms * 1000 ||
        now_us - *last_progress_us >= (int64_t)idle_ms * 1000) return false;
    *last_progress_us = now_us;
    return eota_http_deadline_arm(deadline, started_us, *last_progress_us, total_ms, idle_ms);
}

void eota_http_deadline_destroy(eota_http_deadline_t *deadline)
{
    if (deadline == NULL || deadline->timer == NULL) return;
    /* Releasing this stack-owned callback argument before the callback exits
     * is unsafe; fail closed if the SDK cannot join and delete the timer. */
    if (esp_timer_stop_blocking(deadline->timer, portMAX_DELAY) != ESP_OK ||
        esp_timer_delete(deadline->timer) != ESP_OK) abort();
    deadline->timer = NULL;
}
