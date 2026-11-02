#include "http_deadline.h"
#include "esp_timer.h"

#include <assert.h>
#include <stdio.h>

static int64_t now_us;

int64_t esp_timer_get_time(void)
{
    return now_us;
}

int main(void)
{
    eota_http_deadline_t deadline;
    assert(!eota_http_deadline_init(NULL, 300, 30));
    assert(!eota_http_deadline_init(&deadline, 0, 30));
    assert(!eota_http_deadline_init(&deadline, 300, 0));
    assert(eota_http_deadline_init(&deadline, 30, 300));
    assert(eota_http_deadline_remaining_us(&deadline) == 30000);

    now_us = 0;
    assert(eota_http_deadline_init(&deadline, 300, 30));
    assert(eota_http_deadline_remaining_us(&deadline) == 30000);
    now_us = 29999;
    assert(eota_http_deadline_remaining_us(&deadline) == 1);
    assert(eota_http_deadline_progress(&deadline));
    assert(deadline.last_progress_us == 29999);
    now_us = 59999;
    assert(eota_http_deadline_remaining_us(&deadline) == 0);
    assert(!eota_http_deadline_progress(&deadline));
    assert(deadline.last_progress_us == 29999);

    /* Frequent bytes may refresh idle, but can never extend total expiry. */
    now_us = 0;
    assert(eota_http_deadline_init(&deadline, 300, 30));
    for (int i = 1; i <= 10; ++i) {
        now_us = (int64_t)i * 29000;
        assert(eota_http_deadline_progress(&deadline));
    }
    now_us = 300000;
    assert(eota_http_deadline_remaining_us(&deadline) == 0);
    assert(!eota_http_deadline_progress(&deadline));
    assert(deadline.last_progress_us == 290000);

    now_us = 0;
    assert(eota_http_deadline_init(&deadline, 300, 30));
    now_us = -1;
    assert(eota_http_deadline_remaining_us(&deadline) == 0);
    assert(!eota_http_deadline_progress(&deadline));

    /* The writer may opt in to 600-second renewable budgets. Ordinary
     * progress above still leaves the absolute total deadline unchanged. */
    now_us = 0;
    assert(eota_http_deadline_init(&deadline, 600000, 30000));
    for (int i = 1; i <= 30; ++i) {
        now_us = (int64_t)i * 29000000;
        assert(eota_http_deadline_written(&deadline));
    }
    assert(now_us > INT64_C(600000000));
    now_us += 30000000;
    assert(!eota_http_deadline_written(&deadline));
    assert(!eota_http_deadline_written(NULL));
    now_us = deadline.started_us - 1;
    assert(!eota_http_deadline_written(&deadline));

    puts("  monotonic idle, absolute and write-renewed budgets passed");
}
