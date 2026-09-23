// SPDX-License-Identifier: Apache-2.0
#include "http_deadline.h"

#include <stddef.h>

#include "esp_timer.h"

bool eota_http_deadline_init(eota_http_deadline_t *deadline, uint32_t total_timeout_ms,
                             uint32_t idle_timeout_ms)
{
    if (deadline == NULL || total_timeout_ms == 0 || idle_timeout_ms == 0) return false;
    const int64_t started_us = esp_timer_get_time();
    if (started_us < 0) return false;
    *deadline = (eota_http_deadline_t){
        .started_us = started_us,
        .last_progress_us = started_us,
        .total_timeout_ms = total_timeout_ms,
        .idle_timeout_ms = idle_timeout_ms,
    };
    return true;
}

int64_t eota_http_deadline_remaining_us_at(const eota_http_deadline_t *deadline,
                                           int64_t now_us)
{
    if (deadline == NULL || now_us < deadline->started_us ||
        now_us < deadline->last_progress_us) return 0;
    const int64_t total_left = (int64_t)deadline->total_timeout_ms * 1000 -
                               (now_us - deadline->started_us);
    const int64_t idle_left = (int64_t)deadline->idle_timeout_ms * 1000 -
                              (now_us - deadline->last_progress_us);
    const int64_t left = total_left < idle_left ? total_left : idle_left;
    return left > 0 ? left : 0;
}

int64_t eota_http_deadline_remaining_us(const eota_http_deadline_t *deadline)
{
    return eota_http_deadline_remaining_us_at(deadline, esp_timer_get_time());
}

bool eota_http_deadline_progress(eota_http_deadline_t *deadline)
{
    if (deadline == NULL) return false;
    const int64_t now_us = esp_timer_get_time();
    /* Check against the old idle deadline before accepting a late byte. */
    if (eota_http_deadline_remaining_us_at(deadline, now_us) <= 0) return false;
    deadline->last_progress_us = now_us;
    return true;
}
