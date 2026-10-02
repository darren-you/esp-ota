// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "eota_http_transport.h"

/* Private network progress operations; callers cannot refresh the deadline. */
int64_t eota_http_deadline_remaining_us_at(const eota_http_deadline_t *deadline,
    int64_t now_us);
bool eota_http_deadline_progress(eota_http_deadline_t *deadline);
