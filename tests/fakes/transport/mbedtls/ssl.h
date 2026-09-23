#pragma once
#include <stddef.h>
#define MBEDTLS_SSL_IS_CLIENT 0
#define MBEDTLS_SSL_TRANSPORT_STREAM 0
#define MBEDTLS_SSL_PRESET_DEFAULT 0
#define MBEDTLS_SSL_VERIFY_REQUIRED 2
#define MBEDTLS_ERR_SSL_WANT_READ -0x6900
#define MBEDTLS_ERR_SSL_WANT_WRITE -0x6880
typedef struct { int unused; } mbedtls_ssl_config;
typedef struct { void *context; int (*send)(void *,const unsigned char *,size_t); int (*recv)(void *,unsigned char *,size_t); int handshake_count; } mbedtls_ssl_context;
void mbedtls_ssl_init(mbedtls_ssl_context *);
void mbedtls_ssl_config_init(mbedtls_ssl_config *);
int mbedtls_ssl_set_hostname(mbedtls_ssl_context *,const char *);
int mbedtls_ssl_config_defaults(mbedtls_ssl_config *,int,int,int);
void mbedtls_ssl_conf_authmode(mbedtls_ssl_config *,int);
int mbedtls_ssl_setup(mbedtls_ssl_context *,const mbedtls_ssl_config *);
void mbedtls_ssl_set_bio(mbedtls_ssl_context *,void *,int (*)(void *,const unsigned char *,size_t),int (*)(void *,unsigned char *,size_t),void *);
int mbedtls_ssl_is_handshake_over(const mbedtls_ssl_context *);
int mbedtls_ssl_handshake_step(mbedtls_ssl_context *);
int mbedtls_ssl_read(mbedtls_ssl_context *,unsigned char *,size_t);
int mbedtls_ssl_write(mbedtls_ssl_context *,const unsigned char *,size_t);
size_t mbedtls_ssl_get_bytes_avail(const mbedtls_ssl_context *);
void mbedtls_ssl_free(mbedtls_ssl_context *);
void mbedtls_ssl_config_free(mbedtls_ssl_config *);
uint32_t mbedtls_ssl_get_verify_result(const mbedtls_ssl_context *);
