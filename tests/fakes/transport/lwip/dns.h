#pragma once
#include <netinet/in.h>
#include <string.h>
typedef int err_t;
#define ERR_OK 0
#define ERR_INPROGRESS -5
#define ERR_VAL -6
typedef struct { struct in_addr ipv4; int v6; } ip_addr_t;
#define ip_addr_copy(dst,src) ((dst)=(src))
#define IP_IS_V6(addr) ((addr)->v6)
#define ip_2_ip4(addr) (&(addr)->ipv4)
#define ip_2_ip6(addr) (addr)
#define ip4_addr_get_u32(addr) ((addr)->s_addr)
typedef void (*dns_found_callback)(const char *,const ip_addr_t *,void *);
err_t dns_gethostbyname(const char *,ip_addr_t *,dns_found_callback,void *);
