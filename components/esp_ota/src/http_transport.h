// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <stdint.h>

#include "esp_transport.h"
#include "http_deadline.h"

/* The transport borrows the deadline and progress timestamp during I/O.
 * Stop and destroy the deadline before HTTP cleanup and transport destruction:
 * its callback may still use the socket, which the transport owns. */
esp_transport_handle_t eota_http_transport_create(eota_http_deadline_t *deadline,
                                                   int64_t started_us,
                                                   int64_t *last_progress_us,
                                                   uint32_t total_timeout_ms,
                                                   uint32_t idle_timeout_ms,
                                                   uint32_t connect_timeout_ms);
