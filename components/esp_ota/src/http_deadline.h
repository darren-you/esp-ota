// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <stdbool.h>
#include <stdint.h>
/* One monotonic clock origin and the last accepted network byte. */
typedef struct {
    int64_t started_us;
    int64_t last_progress_us;
    uint32_t total_timeout_ms;
    uint32_t idle_timeout_ms;
} eota_http_deadline_t;

bool eota_http_deadline_init(eota_http_deadline_t *deadline, uint32_t total_timeout_ms,
                             uint32_t idle_timeout_ms);
int64_t eota_http_deadline_remaining_us_at(const eota_http_deadline_t *deadline,
                                           int64_t now_us);
int64_t eota_http_deadline_remaining_us(const eota_http_deadline_t *deadline);
bool eota_http_deadline_progress(eota_http_deadline_t *deadline);
