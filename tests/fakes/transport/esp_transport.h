#pragma once
#include "esp_err.h"
/* Keep the IDF v6.1 transport read result contract visible to host tests. */
enum esp_tcp_transport_err_t {
    ERR_TCP_TRANSPORT_NO_MEM = -3,
    ERR_TCP_TRANSPORT_CONNECTION_FAILED = -2,
    ERR_TCP_TRANSPORT_CONNECTION_CLOSED_BY_FIN = -1,
    ERR_TCP_TRANSPORT_CONNECTION_TIMEOUT = 0,
};
typedef struct esp_transport_fake *esp_transport_handle_t;
typedef int (*transport_connect_fn)(esp_transport_handle_t,const char *,int,int);
typedef int (*transport_read_fn)(esp_transport_handle_t,char *,int,int);
typedef int (*transport_write_fn)(esp_transport_handle_t,const char *,int,int);
typedef int (*transport_close_fn)(esp_transport_handle_t);
typedef int (*transport_poll_fn)(esp_transport_handle_t,int);
typedef int (*transport_destroy_fn)(esp_transport_handle_t);
esp_transport_handle_t esp_transport_init(void);
esp_err_t esp_transport_destroy(esp_transport_handle_t);
void *esp_transport_get_context_data(esp_transport_handle_t);
esp_err_t esp_transport_set_context_data(esp_transport_handle_t,void *);
esp_err_t esp_transport_set_func(esp_transport_handle_t,transport_connect_fn,transport_read_fn,transport_write_fn,transport_close_fn,transport_poll_fn,transport_poll_fn,transport_destroy_fn);
