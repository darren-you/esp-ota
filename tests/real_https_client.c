#include "http_transport.h"

#include <assert.h>
#include <arpa/inet.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "esp_crt_bundle.h"
#include "esp_transport.h"
#include "freertos/semphr.h"
#include "lwip/dns.h"
#include "lwip/tcpip.h"
#include "mbedtls/ssl.h"
#include "mbedtls/x509_crt.h"
#include "psa/crypto.h"

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

static void sleep_ms(long milliseconds)
{
    const struct timespec pause = {.tv_sec = milliseconds / 1000,
                                   .tv_nsec = milliseconds % 1000 * 1000000};
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

esp_err_t esp_timer_stop_blocking(esp_timer_handle_t timer, uint32_t ticks)
{
    assert(timer && ticks == UINT32_MAX);
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

struct semaphore_fake { bool signaled; };
SemaphoreHandle_t xSemaphoreCreateBinary(void) { return calloc(1, sizeof(struct semaphore_fake)); }
int xSemaphoreGive(SemaphoreHandle_t semaphore) { semaphore->signaled = true; return 1; }
int xSemaphoreTake(SemaphoreHandle_t semaphore, uint32_t ticks)
{
    (void)ticks;
    const int result = semaphore->signaled;
    semaphore->signaled = false;
    return result;
}
void vSemaphoreDelete(SemaphoreHandle_t semaphore) { free(semaphore); }

struct esp_transport_fake {
    void *context;
    transport_connect_fn connect;
    transport_read_fn read;
    transport_write_fn write;
    transport_close_fn close;
    transport_poll_fn poll_read;
    transport_poll_fn poll_write;
    transport_destroy_fn destroy;
};
esp_transport_handle_t esp_transport_init(void) { return calloc(1, sizeof(struct esp_transport_fake)); }
esp_err_t esp_transport_destroy(esp_transport_handle_t handle)
{
    if (handle->destroy) handle->destroy(handle);
    free(handle);
    return ESP_OK;
}
void *esp_transport_get_context_data(esp_transport_handle_t handle) { return handle->context; }
esp_err_t esp_transport_set_context_data(esp_transport_handle_t handle, void *data)
{ handle->context = data; return ESP_OK; }
esp_err_t esp_transport_set_func(esp_transport_handle_t handle, transport_connect_fn connect,
                                 transport_read_fn read, transport_write_fn write,
                                 transport_close_fn close, transport_poll_fn poll_read,
                                 transport_poll_fn poll_write, transport_destroy_fn destroy)
{
    handle->connect = connect;
    handle->read = read;
    handle->write = write;
    handle->close = close;
    handle->poll_read = poll_read;
    handle->poll_write = poll_write;
    handle->destroy = destroy;
    return ESP_OK;
}

err_t tcpip_try_callback(void (*callback)(void *), void *argument)
{ callback(argument); return ERR_OK; }
err_t dns_gethostbyname(const char *name, ip_addr_t *address,
                        dns_found_callback callback, void *argument)
{
    (void)callback;
    (void)argument;
    if (strcmp(name, "localhost") != 0 && strcmp(name, "wrong.local") != 0) return ERR_VAL;
    address->ipv4.s_addr = htonl(INADDR_LOOPBACK);
    address->v6 = 0;
    return ERR_OK;
}

static mbedtls_x509_crt test_ca;
int esp_crt_bundle_attach(void *config)
{
    mbedtls_ssl_conf_ca_chain(config, &test_ca, NULL);
    return ESP_OK;
}

int main(int argc, char **argv)
{
    assert(argc == 6);
    const char *hostname = argv[1];
    const int port = atoi(argv[2]);
    const char *ca_path = argv[3];
    const uint32_t total_ms = (uint32_t)atoi(argv[4]);
    const uint32_t idle_ms = (uint32_t)atoi(argv[5]);
    assert(port > 0 && total_ms > 0 && idle_ms > 0);
    assert(psa_crypto_init() == PSA_SUCCESS);
    mbedtls_x509_crt_init(&test_ca);
    assert(mbedtls_x509_crt_parse_file(&test_ca, ca_path) == 0);

    const int64_t started_us = esp_timer_get_time();
    int64_t last_progress_us = started_us;
    eota_http_deadline_t deadline;
    assert(eota_http_deadline_init(&deadline, -1));
    assert(eota_http_deadline_arm(&deadline, started_us, last_progress_us, total_ms, idle_ms));
    esp_transport_handle_t transport = eota_http_transport_create(&deadline, started_us,
        &last_progress_us, total_ms, idle_ms, total_ms);
    assert(transport);

    const char *stage = "connect";
    bool success = transport->connect(transport, hostname, port, -1) == 0;
    if (success) {
        stage = "write";
        const char request[] = "GET / HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n";
        size_t sent = 0;
        while (sent < sizeof request - 1) {
            const int count = transport->write(transport, request + sent,
                                                (int)(sizeof request - 1 - sent), -1);
            if (count <= 0) { success = false; break; }
            sent += (size_t)count;
        }
    }
    char received[256] = {0};
    size_t used = 0;
    if (success) {
        stage = "read";
        while (used < sizeof received - 1) {
            const int count = transport->read(transport, received + used,
                                              (int)(sizeof received - 1 - used), -1);
            if (count <= 0) {
                success = count == 0 && strstr(received, "hello") != NULL;
                break;
            }
            used += (size_t)count;
            received[used] = '\0';
            if (strstr(received, "hello") != NULL) break;
        }
        if (strstr(received, "hello") == NULL) success = false;
    }
    const int64_t elapsed_ms = (esp_timer_get_time() - started_us) / 1000;
    if (!eota_http_deadline_stop(&deadline)) success = false;
    eota_http_deadline_destroy(&deadline);
    transport->close(transport);
    esp_transport_destroy(transport);
    mbedtls_x509_crt_free(&test_ca);
    printf("%s %s %lld bytes=%zu\n", success ? "success" : "failure", stage,
           (long long)elapsed_ms, used);
    return success ? 0 : 2;
}
