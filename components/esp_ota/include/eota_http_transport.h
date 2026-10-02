// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_transport.h"

/* One monotonic origin and the last accepted network progress. The caller
 * initializes this object once and keeps it alive, without modifying it,
 * until its single transport has been destroyed. */
typedef struct {
    int64_t started_us;
    int64_t last_progress_us;
    uint32_t total_timeout_ms;
    uint32_t idle_timeout_ms;
} eota_http_deadline_t;

bool eota_http_deadline_init(eota_http_deadline_t *deadline,
    uint32_t total_timeout_ms, uint32_t idle_timeout_ms);
int64_t eota_http_deadline_remaining_us(const eota_http_deadline_t *deadline);

/* A single-use TLS transport for esp_http_client_config_t.transport. This
 * reuses the OTA DNS/TCP/TLS deadline mechanism without firmware or package
 * policy. trusted_time must reflect the caller's current-boot clock proof.
 * Default CA bundle, certificate dates and original hostname are mandatory.
 * Connect, total, idle and each SDK read/write operation share absolute
 * deadlines; callbacks after a DNS timeout own no caller deadline memory.
 *
 * The caller owns the returned handle. esp_http_client only borrows it:
 * clean up the HTTP client, then esp_transport_destroy(), then release the
 * deadline owner. close alone does not destroy this single-use transport.
 * No caller may refresh an expired idle deadline. The implementation bounds
 * waits; it cannot preempt a crypto step or SDK cleanup/Flash operation. */
esp_transport_handle_t eota_http_transport_create(eota_http_deadline_t *deadline,
    uint32_t connect_timeout_ms, bool trusted_time);
