// SPDX-License-Identifier: Apache-2.0
#include "http_transport.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include "esp_crt_bundle.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "lwip/dns.h"
#include "lwip/inet.h"
#include "lwip/tcpip.h"
#include "mbedtls/net_sockets.h"
#include "mbedtls/ssl.h"

typedef struct {
    atomic_int references;
    atomic_bool complete;
    atomic_bool cancelled;
    SemaphoreHandle_t finished;
    ip_addr_t address;
    bool found;
    char hostname[];
} eota_dns_request_t;

typedef struct {
    eota_http_deadline_t *deadline;
    int64_t connect_started_us;
    int64_t operation_deadline_us;
    uint32_t connect_timeout_ms;
    int socket_fd;
    bool tls_initialized;
    mbedtls_ssl_context ssl;
    mbedtls_ssl_config config;
} eota_http_transport_t;

/* lwIP offers no cancellation handle for an in-flight raw DNS lookup. Keep at
 * most one deferred callback context alive after a caller has timed out. */
static atomic_bool dns_request_active = ATOMIC_VAR_INIT(false);

static void dns_release(eota_dns_request_t *request)
{
    if (atomic_fetch_sub_explicit(&request->references, 1, memory_order_acq_rel) == 1) {
        vSemaphoreDelete(request->finished);
        free(request);
        atomic_store_explicit(&dns_request_active, false, memory_order_release);
    }
}

static void dns_complete(eota_dns_request_t *request, const ip_addr_t *address)
{
    if (address != NULL) {
        ip_addr_copy(request->address, *address);
        request->found = true;
    }
    atomic_store_explicit(&request->complete, true, memory_order_release);
    (void)xSemaphoreGive(request->finished);
    dns_release(request);
}

static void dns_found(const char *hostname, const ip_addr_t *address, void *argument)
{
    (void)hostname;
    dns_complete(argument, address);
}

static void dns_start(void *argument)
{
    eota_dns_request_t *request = argument;
    if (atomic_load_explicit(&request->cancelled, memory_order_acquire)) {
        dns_complete(request, NULL);
        return;
    }
    ip_addr_t address;
    const err_t result = dns_gethostbyname(request->hostname, &address, dns_found, request);
    if (result == ERR_OK) dns_complete(request, &address);
    else if (result != ERR_INPROGRESS) dns_complete(request, NULL);
    /* ERR_INPROGRESS transfers this reference to dns_found. */
}

static int64_t remaining_us(const eota_http_transport_t *transport, bool connecting)
{
    const int64_t now = esp_timer_get_time();
    int64_t left = eota_http_deadline_remaining_us_at(transport->deadline, now);
    if (connecting) {
        if (now < transport->connect_started_us) return 0;
        const int64_t connect = (int64_t)transport->connect_timeout_ms * 1000 -
                                (now - transport->connect_started_us);
        if (connect < left) left = connect;
    }
    return left > 0 ? left : 0;
}

static bool note_progress(eota_http_transport_t *transport)
{
    return eota_http_deadline_progress(transport->deadline);
}

/* A TLS record may arrive one ciphertext byte at a time. Bound the entire
 * transport call, not each select within mbedtls_ssl_read/write. */
static bool begin_operation(eota_http_transport_t *transport, int timeout_ms)
{
    transport->operation_deadline_us = -1;
    if (timeout_ms < 0) return true;
    const int64_t now = esp_timer_get_time();
    const int64_t duration_us = (int64_t)timeout_ms * 1000;
    if (now < 0 || now > INT64_MAX - duration_us) return false;
    transport->operation_deadline_us = now + duration_us;
    return true;
}

static int64_t operation_remaining_us(const eota_http_transport_t *transport)
{
    return transport->operation_deadline_us < 0 ? INT64_MAX :
           transport->operation_deadline_us - esp_timer_get_time();
}

static bool resolve_host(eota_http_transport_t *transport, const char *hostname,
                         ip_addr_t *address)
{
    const size_t length = strlen(hostname);
    if (length == 0 || length >= 256 || remaining_us(transport, true) <= 0) return false;
    bool expected = false;
    if (!atomic_compare_exchange_strong_explicit(&dns_request_active, &expected, true,
                                                  memory_order_acq_rel,
                                                  memory_order_acquire)) return false;
    eota_dns_request_t *request = calloc(1, sizeof *request + length + 1);
    if (request == NULL) {
        atomic_store_explicit(&dns_request_active, false, memory_order_release);
        return false;
    }
    memcpy(request->hostname, hostname, length + 1);
    atomic_init(&request->references, 2); /* caller and TCP/IP callback */
    atomic_init(&request->complete, false);
    atomic_init(&request->cancelled, false);
    request->finished = xSemaphoreCreateBinary();
    if (request->finished == NULL) {
        free(request);
        atomic_store_explicit(&dns_request_active, false, memory_order_release);
        return false;
    }
    if (tcpip_try_callback(dns_start, request) != ERR_OK) {
        dns_release(request);
        dns_release(request);
        return false;
    }
    /* A queued lwIP lookup cannot be cancelled. Its reference owns all memory
     * until the callback fires, even when this caller reaches its deadline. */
    while (!atomic_load_explicit(&request->complete, memory_order_acquire) &&
           remaining_us(transport, true) > 0) {
        (void)xSemaphoreTake(request->finished, 1);
    }
    const bool found = atomic_load_explicit(&request->complete, memory_order_acquire) &&
                       request->found && remaining_us(transport, true) > 0;
    if (!found) atomic_store_explicit(&request->cancelled, true, memory_order_release);
    if (found) ip_addr_copy(*address, request->address);
    dns_release(request);
    return found && note_progress(transport);
}

static int wait_socket(eota_http_transport_t *transport, bool write_ready,
                       int timeout_ms, bool connecting)
{
    const int64_t left_us = remaining_us(transport, connecting);
    if (left_us <= 0 || transport->socket_fd < 0) return -1;
    int64_t wait_us = left_us;
    const int64_t operation_us = operation_remaining_us(transport);
    if (operation_us <= 0) return 0;
    if (operation_us < wait_us) wait_us = operation_us;
    if (timeout_ms >= 0 && (int64_t)timeout_ms * 1000 < wait_us) {
        wait_us = (int64_t)timeout_ms * 1000;
    }
    struct timeval timeout = {.tv_sec = (time_t)(wait_us / 1000000),
                              .tv_usec = (suseconds_t)(wait_us % 1000000)};
    fd_set read_set;
    fd_set write_set;
    FD_ZERO(&read_set);
    FD_ZERO(&write_set);
    if (write_ready) FD_SET(transport->socket_fd, &write_set);
    else FD_SET(transport->socket_fd, &read_set);
    const int ready = select(transport->socket_fd + 1, &read_set, &write_set, NULL, &timeout);
    if (remaining_us(transport, connecting) <= 0) return -1;
    if (operation_remaining_us(transport) <= 0) return 0;
    return ready > 0 ? 1 : ready == 0 ? 0 : -1;
}

static bool connect_socket(eota_http_transport_t *transport, const ip_addr_t *address, int port)
{
    if (port < 1 || port > 65535 || remaining_us(transport, true) <= 0) return false;
    struct sockaddr_storage storage = {0};
    socklen_t length;
    if (IP_IS_V6(address)) {
        struct sockaddr_in6 *ipv6 = (struct sockaddr_in6 *)&storage;
        ipv6->sin6_family = AF_INET6;
        ipv6->sin6_port = htons((uint16_t)port);
        inet6_addr_from_ip6addr(&ipv6->sin6_addr, ip_2_ip6(address));
        length = sizeof *ipv6;
    } else {
        struct sockaddr_in *ipv4 = (struct sockaddr_in *)&storage;
        ipv4->sin_family = AF_INET;
        ipv4->sin_port = htons((uint16_t)port);
        ipv4->sin_addr.s_addr = ip4_addr_get_u32(ip_2_ip4(address));
        length = sizeof *ipv4;
    }
    const int fd = socket(storage.ss_family, SOCK_STREAM, IPPROTO_TCP);
    if (fd < 0) return false;
    transport->socket_fd = fd;
    const int flags = fcntl(fd, F_GETFL, 0);
    if (fd >= FD_SETSIZE || flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0 ||
        remaining_us(transport, true) <= 0) return false;
    if (connect(fd, (const struct sockaddr *)&storage, length) < 0) {
        if (errno != EINPROGRESS || wait_socket(transport, true, -1, true) != 1) return false;
        int socket_error = 0;
        socklen_t error_size = sizeof socket_error;
        if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &socket_error, &error_size) < 0 ||
            socket_error != 0) return false;
    }
    return remaining_us(transport, true) > 0 && note_progress(transport);
}

static int tls_send(void *argument, const unsigned char *buffer, size_t length)
{
    eota_http_transport_t *transport = argument;
    if (remaining_us(transport, transport->connect_started_us != 0) <= 0) {
        return MBEDTLS_ERR_NET_SEND_FAILED;
    }
    if (operation_remaining_us(transport) <= 0) return MBEDTLS_ERR_SSL_WANT_WRITE;
    const ssize_t sent = send(transport->socket_fd, buffer, length, 0);
    if (sent > 0) return note_progress(transport) ? (int)sent : MBEDTLS_ERR_NET_SEND_FAILED;
    if (sent < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) {
        return MBEDTLS_ERR_SSL_WANT_WRITE;
    }
    return MBEDTLS_ERR_NET_SEND_FAILED;
}

static int tls_recv(void *argument, unsigned char *buffer, size_t length)
{
    eota_http_transport_t *transport = argument;
    if (remaining_us(transport, transport->connect_started_us != 0) <= 0) {
        return MBEDTLS_ERR_NET_RECV_FAILED;
    }
    if (operation_remaining_us(transport) <= 0) return MBEDTLS_ERR_SSL_WANT_READ;
    const ssize_t received = recv(transport->socket_fd, buffer, length, 0);
    if (received > 0) return note_progress(transport) ? (int)received : MBEDTLS_ERR_NET_RECV_FAILED;
    if (received < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) {
        return MBEDTLS_ERR_SSL_WANT_READ;
    }
    return received == 0 ? 0 : MBEDTLS_ERR_NET_RECV_FAILED;
}

static bool handshake(eota_http_transport_t *transport, const char *hostname)
{
    mbedtls_ssl_init(&transport->ssl);
    mbedtls_ssl_config_init(&transport->config);
    transport->tls_initialized = true;
    if (mbedtls_ssl_set_hostname(&transport->ssl, hostname) != 0 ||
        mbedtls_ssl_config_defaults(&transport->config, MBEDTLS_SSL_IS_CLIENT,
                                    MBEDTLS_SSL_TRANSPORT_STREAM,
                                    MBEDTLS_SSL_PRESET_DEFAULT) != 0) return false;
    mbedtls_ssl_conf_authmode(&transport->config, MBEDTLS_SSL_VERIFY_REQUIRED);
    if (esp_crt_bundle_attach(&transport->config) != ESP_OK ||
        mbedtls_ssl_setup(&transport->ssl, &transport->config) != 0) return false;
    mbedtls_ssl_set_bio(&transport->ssl, transport, tls_send, tls_recv, NULL);
    while (!mbedtls_ssl_is_handshake_over(&transport->ssl)) {
        if (remaining_us(transport, true) <= 0) return false;
        const int result = mbedtls_ssl_handshake_step(&transport->ssl);
        if (remaining_us(transport, true) <= 0) return false;
        if (result == MBEDTLS_ERR_SSL_WANT_READ || result == MBEDTLS_ERR_SSL_WANT_WRITE) {
            if (wait_socket(transport, result == MBEDTLS_ERR_SSL_WANT_WRITE,
                            -1, true) != 1) return false;
        } else if (result != 0) return false;
    }
    transport->connect_started_us = 0;
    return mbedtls_ssl_get_verify_result(&transport->ssl) == 0 &&
           remaining_us(transport, false) > 0;
}

static int transport_connect(esp_transport_handle_t handle, const char *hostname,
                             int port, int timeout_ms)
{
    (void)timeout_ms;
    eota_http_transport_t *transport = esp_transport_get_context_data(handle);
    if (transport == NULL || transport->socket_fd >= 0 || hostname == NULL) return -1;
    transport->connect_started_us = esp_timer_get_time();
    ip_addr_t address;
    if (!resolve_host(transport, hostname, &address) ||
        !connect_socket(transport, &address, port) ||
        !handshake(transport, hostname)) return -1;
    return 0;
}

static int transport_read(esp_transport_handle_t handle, char *buffer, int length,
                          int timeout_ms)
{
    eota_http_transport_t *transport = esp_transport_get_context_data(handle);
    if (transport == NULL || buffer == NULL || length <= 0) {
        return ERR_TCP_TRANSPORT_CONNECTION_FAILED;
    }
    if (length > 1024) length = 1024;
    if (!begin_operation(transport, timeout_ms)) return ERR_TCP_TRANSPORT_CONNECTION_FAILED;
    int result = ERR_TCP_TRANSPORT_CONNECTION_FAILED;
    while (remaining_us(transport, false) > 0) {
        /* TLS 1.3 may return repeated post-handshake tickets without WANT_READ.
         * They cannot extend this HTTP client's single read deadline. */
        if (operation_remaining_us(transport) <= 0) {
            result = ERR_TCP_TRANSPORT_CONNECTION_TIMEOUT;
            break;
        }
        const int received = mbedtls_ssl_read(&transport->ssl, (unsigned char *)buffer,
                                               (size_t)length);
        if (remaining_us(transport, false) <= 0) break;
        /* Preserve data already consumed by TLS, even if a single crypto
         * step ended just after the per-call timeout. */
        if (received > 0) { result = received; break; }
        if (received == 0) { result = ERR_TCP_TRANSPORT_CONNECTION_CLOSED_BY_FIN; break; }
        /* TLS 1.3 may deliver a post-handshake ticket before application data. */
        if (received == MBEDTLS_ERR_SSL_RECEIVED_NEW_SESSION_TICKET) continue;
        if (received != MBEDTLS_ERR_SSL_WANT_READ &&
            received != MBEDTLS_ERR_SSL_WANT_WRITE) break;
        if (operation_remaining_us(transport) <= 0) {
            result = ERR_TCP_TRANSPORT_CONNECTION_TIMEOUT;
            break;
        }
        if (received == MBEDTLS_ERR_SSL_WANT_READ &&
            mbedtls_ssl_get_bytes_avail(&transport->ssl) != 0) continue;
        const int ready = wait_socket(transport, received == MBEDTLS_ERR_SSL_WANT_WRITE,
                                      timeout_ms, false);
        if (ready == 0) { result = ERR_TCP_TRANSPORT_CONNECTION_TIMEOUT; break; }
        if (ready < 0) break;
    }
    transport->operation_deadline_us = -1;
    return result;
}

static int transport_write(esp_transport_handle_t handle, const char *buffer, int length,
                           int timeout_ms)
{
    eota_http_transport_t *transport = esp_transport_get_context_data(handle);
    if (transport == NULL || buffer == NULL || length <= 0) return -1;
    if (length > 1024) length = 1024;
    if (!begin_operation(transport, timeout_ms)) return -1;
    int result = -1;
    while (remaining_us(transport, false) > 0) {
        /* WANT_* must retry this exact buffer and length. */
        const int sent = mbedtls_ssl_write(&transport->ssl, (const unsigned char *)buffer,
                                           (size_t)length);
        if (remaining_us(transport, false) <= 0) break;
        if (sent > 0) { result = sent; break; }
        if (sent != MBEDTLS_ERR_SSL_WANT_READ &&
            sent != MBEDTLS_ERR_SSL_WANT_WRITE) break;
        if (operation_remaining_us(transport) <= 0) break;
        if (wait_socket(transport, sent == MBEDTLS_ERR_SSL_WANT_WRITE,
                        timeout_ms, false) != 1) break;
    }
    transport->operation_deadline_us = -1;
    return result;
}

static int transport_poll_read(esp_transport_handle_t handle, int timeout_ms)
{
    eota_http_transport_t *transport = esp_transport_get_context_data(handle);
    if (transport == NULL || remaining_us(transport, false) <= 0) return -1;
    if (mbedtls_ssl_get_bytes_avail(&transport->ssl) != 0) return 1;
    return wait_socket(transport, false, timeout_ms, false);
}

static int transport_poll_write(esp_transport_handle_t handle, int timeout_ms)
{
    eota_http_transport_t *transport = esp_transport_get_context_data(handle);
    return transport == NULL ? -1 : wait_socket(transport, true, timeout_ms, false);
}

static int transport_close(esp_transport_handle_t handle)
{
    (void)handle;
    /* This OTA transport is single-use. HTTP may call close on an error while
     * unwinding; its owner releases the socket in transport_destroy. */
    return 0;
}

static int transport_destroy(esp_transport_handle_t handle)
{
    eota_http_transport_t *transport = esp_transport_get_context_data(handle);
    if (transport != NULL) {
        if (transport->tls_initialized) {
            mbedtls_ssl_free(&transport->ssl);
            mbedtls_ssl_config_free(&transport->config);
        }
        if (transport->socket_fd >= 0) (void)close(transport->socket_fd);
        free(transport);
    }
    return 0;
}

esp_transport_handle_t eota_http_transport_create(eota_http_deadline_t *deadline,
                                                   uint32_t connect_timeout_ms)
{
    if (deadline == NULL || connect_timeout_ms == 0 ||
        eota_http_deadline_remaining_us(deadline) <= 0) return NULL;
    esp_transport_handle_t handle = esp_transport_init();
    if (handle == NULL) return NULL;
    eota_http_transport_t *transport = calloc(1, sizeof *transport);
    if (transport == NULL) {
        (void)esp_transport_destroy(handle);
        return NULL;
    }
    transport->deadline = deadline;
    transport->connect_timeout_ms = connect_timeout_ms;
    transport->operation_deadline_us = -1;
    transport->socket_fd = -1;
    if (esp_transport_set_context_data(handle, transport) != ESP_OK) {
        free(transport);
        (void)esp_transport_destroy(handle);
        return NULL;
    }
    if (esp_transport_set_func(handle, transport_connect, transport_read, transport_write,
                               transport_close, transport_poll_read, transport_poll_write,
                               transport_destroy) != ESP_OK) {
        (void)esp_transport_set_context_data(handle, NULL);
        free(transport);
        (void)esp_transport_destroy(handle);
        return NULL;
    }
    return handle;
}
