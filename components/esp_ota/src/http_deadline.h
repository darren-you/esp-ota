// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stdatomic.h>

#include "esp_timer.h"

/* Owns only the timer. The HTTP owner must stop it before closing its socket. */
typedef struct {
    esp_timer_handle_t timer;
    atomic_int socket_fd;
    atomic_bool expired;
} eota_http_deadline_t;

bool eota_http_deadline_init(eota_http_deadline_t *deadline, int socket_fd);
void eota_http_deadline_set_socket(eota_http_deadline_t *deadline, int socket_fd);
bool eota_http_deadline_arm(eota_http_deadline_t *deadline, int64_t started_us,
                            int64_t last_progress_us, uint32_t total_ms, uint32_t idle_ms);
bool eota_http_deadline_stop(eota_http_deadline_t *deadline);
bool eota_http_deadline_alive(const eota_http_deadline_t *deadline);
bool eota_http_deadline_progress(eota_http_deadline_t *deadline, int64_t started_us,
                                int64_t *last_progress_us, uint32_t total_ms, uint32_t idle_ms);
void eota_http_deadline_destroy(eota_http_deadline_t *deadline);
