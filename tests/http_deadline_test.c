#include "http_deadline.h"

#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdlib.h>
#include <stdio.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

struct esp_timer_fake {
    pthread_t thread;
    pthread_mutex_t mutex;
    pthread_cond_t changed;
    void (*callback)(void *);
    void *argument;
    int64_t alarm_us;
    bool active;
    bool executing;
    bool closed;
};

int64_t esp_timer_get_time(void)
{
    struct timespec now;
    assert(clock_gettime(CLOCK_MONOTONIC, &now) == 0);
    return (int64_t)now.tv_sec * INT64_C(1000000) + now.tv_nsec / 1000;
}

static void sleep_ms(long ms)
{
    const struct timespec pause = {.tv_sec = ms / 1000, .tv_nsec = ms % 1000 * 1000000};
    (void)nanosleep(&pause, NULL);
}

static void *timer_worker(void *argument)
{
    struct esp_timer_fake *timer = argument;
    pthread_mutex_lock(&timer->mutex);
    while (!timer->closed) {
        if (!timer->active) {
            pthread_cond_wait(&timer->changed, &timer->mutex);
            continue;
        }
        if (esp_timer_get_time() < timer->alarm_us) {
            pthread_mutex_unlock(&timer->mutex);
            sleep_ms(1);
            pthread_mutex_lock(&timer->mutex);
            continue;
        }
        timer->active = false;
        timer->executing = true;
        pthread_mutex_unlock(&timer->mutex);
        timer->callback(timer->argument);
        pthread_mutex_lock(&timer->mutex);
        timer->executing = false;
        pthread_cond_broadcast(&timer->changed);
    }
    pthread_mutex_unlock(&timer->mutex);
    return NULL;
}

esp_err_t esp_timer_create(const esp_timer_create_args_t *args, esp_timer_handle_t *out)
{
    assert(args && args->callback && args->dispatch_method == ESP_TIMER_TASK && out);
    struct esp_timer_fake *timer = calloc(1, sizeof *timer);
    if (!timer) return ESP_ERR_NO_MEM;
    timer->callback = args->callback;
    timer->argument = args->arg;
    assert(pthread_mutex_init(&timer->mutex, NULL) == 0);
    assert(pthread_cond_init(&timer->changed, NULL) == 0);
    assert(pthread_create(&timer->thread, NULL, timer_worker, timer) == 0);
    *out = timer;
    return ESP_OK;
}

esp_err_t esp_timer_start_once(esp_timer_handle_t timer, uint64_t timeout_us)
{
    assert(timer && timeout_us > 0);
    pthread_mutex_lock(&timer->mutex);
    assert(!timer->active && !timer->executing);
    timer->alarm_us = esp_timer_get_time() + (int64_t)timeout_us;
    timer->active = true;
    pthread_cond_signal(&timer->changed);
    pthread_mutex_unlock(&timer->mutex);
    return ESP_OK;
}

esp_err_t esp_timer_stop_blocking(esp_timer_handle_t timer, uint32_t timeout_ticks)
{
    assert(timer && timeout_ticks == UINT32_MAX);
    pthread_mutex_lock(&timer->mutex);
    timer->active = false;
    while (timer->executing) pthread_cond_wait(&timer->changed, &timer->mutex);
    pthread_mutex_unlock(&timer->mutex);
    return ESP_OK;
}

esp_err_t esp_timer_delete(esp_timer_handle_t timer)
{
    assert(timer);
    pthread_mutex_lock(&timer->mutex);
    assert(!timer->active && !timer->executing);
    timer->closed = true;
    pthread_cond_signal(&timer->changed);
    pthread_mutex_unlock(&timer->mutex);
    assert(pthread_join(timer->thread, NULL) == 0);
    assert(pthread_cond_destroy(&timer->changed) == 0);
    assert(pthread_mutex_destroy(&timer->mutex) == 0);
    free(timer);
    return ESP_OK;
}

static void *drip(void *argument)
{
    const int fd = *(const int *)argument;
    const char byte = 'x';
    for (int i = 0; i < 100; ++i) {
        if (send(fd, &byte, 1, 0) != 1) break;
        sleep_ms(20);
    }
    return NULL;
}

int main(void)
{
    (void)signal(SIGPIPE, SIG_IGN);
    int sockets[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0);
    pthread_t sender;
    assert(pthread_create(&sender, NULL, drip, &sockets[1]) == 0);
    eota_http_deadline_t deadline;
    assert(eota_http_deadline_init(&deadline));
    const int64_t start = esp_timer_get_time();
    assert(eota_http_deadline_arm(&deadline, start, start, 1000, 250));
    char byte;
    int received = 0;
    while (eota_http_deadline_alive(&deadline)) {
        if (recv(sockets[0], &byte, 1, MSG_DONTWAIT) == 1) received++;
        sleep_ms(1);
    }
    const int64_t elapsed_ms = (esp_timer_get_time() - start) / 1000;
    assert(received > 5 && elapsed_ms >= 200 && elapsed_ms < 800);
    assert(!eota_http_deadline_stop(&deadline));
    eota_http_deadline_destroy(&deadline);
    close(sockets[0]);
    close(sockets[1]);
    assert(pthread_join(sender, NULL) == 0);

    /* A stopped timer must not touch a later socket that reuses the FD. */
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0);
    assert(eota_http_deadline_init(&deadline));
    const int64_t next = esp_timer_get_time();
    assert(eota_http_deadline_arm(&deadline, next, next, 500, 500));
    assert(eota_http_deadline_stop(&deadline));
    eota_http_deadline_destroy(&deadline);
    close(sockets[0]);
    close(sockets[1]);
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0);
    sleep_ms(550);
    assert(send(sockets[1], "q", 1, 0) == 1);
    assert(recv(sockets[0], &byte, 1, 0) == 1 && byte == 'q');
    close(sockets[0]);
    close(sockets[1]);

    /* DNS can expire before any socket exists. A later socket remains owned
     * solely by its caller, and cannot be touched by the expired timer. */
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0);
    assert(eota_http_deadline_init(&deadline));
    const int64_t dns_start = esp_timer_get_time();
    assert(eota_http_deadline_arm(&deadline, dns_start, dns_start, 35, 35));
    sleep_ms(80);
    assert(!eota_http_deadline_stop(&deadline));
    assert(send(sockets[1], "r", 1, 0) == 1);
    assert(recv(sockets[0], &byte, 1, 0) == 1 && byte == 'r');
    eota_http_deadline_destroy(&deadline);
    close(sockets[0]);
    close(sockets[1]);
    /* Expiration must not enter lwIP's potentially unbounded shutdown path.
     * The owner observes the deadline and closes the socket after joining. */
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0);
    assert(eota_http_deadline_init(&deadline));
    const int64_t quiet_start = esp_timer_get_time();
    assert(eota_http_deadline_arm(&deadline, quiet_start, quiet_start, 35, 35));
    sleep_ms(80);
    assert(!eota_http_deadline_stop(&deadline));
    errno = 0;
    assert(recv(sockets[0], &byte, 1, MSG_DONTWAIT) == -1 &&
           (errno == EAGAIN || errno == EWOULDBLOCK));
    eota_http_deadline_destroy(&deadline);
    close(sockets[0]);
    close(sockets[1]);
    printf("  HTTP deadline passed (slow drip stopped at %lld ms, %d bytes; callback joined)\n",
           (long long)elapsed_ms, received);
}
