// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <stdint.h>

#include "esp_transport.h"
#include "http_deadline.h"

/* The transport borrows the caller's single monotonic deadline during I/O. */
esp_transport_handle_t eota_http_transport_create(eota_http_deadline_t *deadline,
                                                   uint32_t connect_timeout_ms);
