#include "http_deadline.h"

#include <assert.h>
#include <pthread.h>
#include <signal.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdatomic.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

static atomic_bool virtual_clock_active;
static atomic_int_fast64_t virtual_clock_us;

int64_t esp_timer_get_time(void)
{
    if (atomic_load(&virtual_clock_active)) return atomic_load(&virtual_clock_us);
    struct timespec now;
    assert(clock_gettime(CLOCK_MONOTONIC, &now) == 0);
    return (int64_t)now.tv_sec * INT64_C(1000000) + now.tv_nsec / 1000;
}

static void sleep_ms(long ms)
{
    const struct timespec pause = {.tv_sec = ms / 1000, .tv_nsec = ms % 1000 * 1000000};
    (void)nanosleep(&pause, NULL);
}

#include "http_transport.h"
#include <stdint.h>
#include "esp_transport.h"
#include "freertos/semphr.h"
#include "lwip/dns.h"
#include "lwip/tcpip.h"
#include "mbedtls/ssl.h"
#include "mbedtls/net_sockets.h"

#include <arpa/inet.h>
#include <errno.h>
#include <string.h>

struct semaphore_fake { pthread_mutex_t mutex; pthread_cond_t condition; bool signaled; };
SemaphoreHandle_t xSemaphoreCreateBinary(void) {
    SemaphoreHandle_t sem = calloc(1,sizeof *sem); assert(sem);
    assert(pthread_mutex_init(&sem->mutex,NULL)==0);
    assert(pthread_cond_init(&sem->condition,NULL)==0); return sem;
}
int xSemaphoreGive(SemaphoreHandle_t sem) { pthread_mutex_lock(&sem->mutex); sem->signaled=true; pthread_cond_signal(&sem->condition); pthread_mutex_unlock(&sem->mutex); return 1; }
int xSemaphoreTake(SemaphoreHandle_t sem,uint32_t ticks) {
    pthread_mutex_lock(&sem->mutex); int result=sem->signaled;
    if (!result) { struct timespec until; clock_gettime(CLOCK_REALTIME,&until); until.tv_nsec += (long)ticks*1000000; until.tv_sec += until.tv_nsec/1000000000; until.tv_nsec %= 1000000000; (void)pthread_cond_timedwait(&sem->condition,&sem->mutex,&until); result=sem->signaled; }
    sem->signaled=false; pthread_mutex_unlock(&sem->mutex); return result;
}
void vSemaphoreDelete(SemaphoreHandle_t sem) { pthread_cond_destroy(&sem->condition); pthread_mutex_destroy(&sem->mutex); free(sem); }

struct esp_transport_fake { void *context; transport_connect_fn connect; transport_read_fn read; transport_write_fn write; transport_close_fn close; transport_poll_fn poll_read,poll_write; transport_destroy_fn destroy; };
esp_transport_handle_t esp_transport_init(void) { return calloc(1,sizeof(struct esp_transport_fake)); }
esp_err_t esp_transport_destroy(esp_transport_handle_t handle) { if (handle->destroy) handle->destroy(handle); free(handle); return ESP_OK; }
void *esp_transport_get_context_data(esp_transport_handle_t handle) { return handle->context; }
esp_err_t esp_transport_set_context_data(esp_transport_handle_t handle,void *data) { handle->context=data; return ESP_OK; }
esp_err_t esp_transport_set_func(esp_transport_handle_t h,transport_connect_fn c,transport_read_fn r,transport_write_fn w,transport_close_fn cl,transport_poll_fn pr,transport_poll_fn pw,transport_destroy_fn d) { h->connect=c;h->read=r;h->write=w;h->close=cl;h->poll_read=pr;h->poll_write=pw;h->destroy=d;return ESP_OK; }
static atomic_int bundle_attach_count;
static atomic_int verify_required_count;
static atomic_int hostname_set_count;
static atomic_int verification_checks;
static char observed_hostname[256];
int esp_crt_bundle_attach(void *config) { assert(config);atomic_fetch_add(&bundle_attach_count,1);return ESP_OK; }

static atomic_int dns_delay_ms;
static atomic_int dns_pending;
static atomic_int dns_lookup_count;
static atomic_int tcpip_delay_ms;
static atomic_int tcpip_pending;
static atomic_int verification_flags;
static atomic_int tls_mode;
enum { TLS_NORMAL, TLS_SLOW_HANDSHAKE, TLS_SLOW_WRITE, TLS_VERIFY_FAIL, TLS_FATAL_READ,
       TLS_SLOW_RECORD, TLS_REPEATED_TICKETS };
typedef struct { dns_found_callback callback; void *argument; char name[256]; } dns_job_t;
static void *dns_worker(void *argument) {
    dns_job_t *job=argument; sleep_ms(atomic_load(&dns_delay_ms));
    ip_addr_t address={.ipv4.s_addr=htonl(INADDR_LOOPBACK)};
    job->callback(job->name,&address,job->argument);
    atomic_fetch_sub(&dns_pending,1); free(job); return NULL;
}
err_t dns_gethostbyname(const char *name,ip_addr_t *out,dns_found_callback callback,void *argument) {
    atomic_fetch_add(&dns_lookup_count,1);
    if (strcmp(name,"dns-hang")==0 || strcasecmp(name,"localhost")==0) {
        if (atomic_load(&dns_delay_ms)>0) {
            dns_job_t *job=calloc(1,sizeof *job); assert(job); job->callback=callback;job->argument=argument;strncpy(job->name,name,sizeof job->name-1);
            atomic_fetch_add(&dns_pending,1); pthread_t thread; assert(pthread_create(&thread,NULL,dns_worker,job)==0); pthread_detach(thread); return ERR_INPROGRESS;
        }
        out->ipv4.s_addr=htonl(INADDR_LOOPBACK);out->v6=0;return ERR_OK;
    }
    return ERR_VAL;
}
typedef struct { void (*callback)(void *);void *argument;int delay_ms; } tcpip_job_t;
static void *tcpip_worker(void *argument) { tcpip_job_t *job=argument;sleep_ms(job->delay_ms);job->callback(job->argument);atomic_fetch_sub(&tcpip_pending,1);free(job);return NULL; }
err_t tcpip_try_callback(void (*callback)(void *),void *argument) {
    int delay=atomic_load(&tcpip_delay_ms);if(delay==0) { callback(argument);return ERR_OK; }
    tcpip_job_t *job=calloc(1,sizeof *job);assert(job);job->callback=callback;job->argument=argument;job->delay_ms=delay;
    atomic_fetch_add(&tcpip_pending,1);pthread_t thread;assert(pthread_create(&thread,NULL,tcpip_worker,job)==0);pthread_detach(thread);return ERR_OK;
}
void mbedtls_ssl_init(mbedtls_ssl_context *s) { memset(s,0,sizeof *s); }
void mbedtls_ssl_config_init(mbedtls_ssl_config *c) { memset(c,0,sizeof *c); }
int mbedtls_ssl_set_hostname(mbedtls_ssl_context *s,const char *hostname) { (void)s;assert(strlen(hostname)<sizeof observed_hostname);strcpy(observed_hostname,hostname);atomic_fetch_add(&hostname_set_count,1);return hostname[0]?0:-1; }
int mbedtls_ssl_config_defaults(mbedtls_ssl_config *c,int a,int b,int d) { (void)c;(void)a;(void)b;(void)d;return 0; }
void mbedtls_ssl_conf_authmode(mbedtls_ssl_config *c,int mode) { (void)c;assert(mode==MBEDTLS_SSL_VERIFY_REQUIRED);atomic_fetch_add(&verify_required_count,1); }
int mbedtls_ssl_setup(mbedtls_ssl_context *s,const mbedtls_ssl_config *c) { (void)s;(void)c;return 0; }
void mbedtls_ssl_set_bio(mbedtls_ssl_context *s,void *ctx,int (*send_fn)(void *,const unsigned char *,size_t),int (*recv_fn)(void *,unsigned char *,size_t),void *unused) { (void)unused;s->context=ctx;s->send=send_fn;s->recv=recv_fn; }
int mbedtls_ssl_is_handshake_over(const mbedtls_ssl_context *s) { return s->handshake_count>=1; }
int mbedtls_ssl_handshake_step(mbedtls_ssl_context *s) { unsigned char byte; int n=s->recv(s->context,&byte,1); if(n==1 && byte=='H') { s->handshake_count=1;return 0; } return n==MBEDTLS_ERR_NET_RECV_FAILED?n:MBEDTLS_ERR_SSL_WANT_READ; }
int mbedtls_ssl_read(mbedtls_ssl_context *s,unsigned char *buffer,size_t length) {
    if(atomic_load(&tls_mode)==TLS_FATAL_READ) return -0x7777;
    if(atomic_load(&tls_mode)==TLS_REPEATED_TICKETS) {
        /* TLS 1.3 post-handshake tickets can be returned without app data. */
        if(atomic_load(&virtual_clock_active)) atomic_fetch_add(&virtual_clock_us,5000);
        return MBEDTLS_ERR_SSL_RECEIVED_NEW_SESSION_TICKET;
    }
    if(atomic_load(&tls_mode)==TLS_SLOW_RECORD) {
        if(s->record_bytes<12) {
            unsigned char byte;
            int count=s->recv(s->context,&byte,1);
            if(count<=0) return count;
            s->record_bytes++;
            if(atomic_load(&virtual_clock_active)) atomic_fetch_add(&virtual_clock_us,18000);
            if(s->record_bytes<12) return MBEDTLS_ERR_SSL_WANT_READ;
        }
        s->record_bytes=0;
        if(length==0) return -0x7777;
        buffer[0]='D';
        return 1;
    }
    int count=s->recv(s->context,buffer,length);
    if(count>0 && atomic_load(&virtual_clock_active)) atomic_fetch_add(&virtual_clock_us,15000);
    return count;
}
int mbedtls_ssl_write(mbedtls_ssl_context *s,const unsigned char *buffer,size_t length) { if(atomic_load(&tls_mode)==TLS_SLOW_WRITE) { int result=s->send(s->context,buffer,1); atomic_fetch_add(&virtual_clock_us,15000); return result; } return s->send(s->context,buffer,length); }
size_t mbedtls_ssl_get_bytes_avail(const mbedtls_ssl_context *s) { (void)s;return 0; }
uint32_t mbedtls_ssl_get_verify_result(const mbedtls_ssl_context *s) { (void)s;atomic_fetch_add(&verification_checks,1);return (uint32_t)atomic_load(&verification_flags); }
void mbedtls_ssl_free(mbedtls_ssl_context *s) { (void)s; }
void mbedtls_ssl_config_free(mbedtls_ssl_config *c) { (void)c; }

typedef struct { int listener; int delay_ms; int drip_ms; int reply_delay_ms; bool handshake; bool read_request; bool single_reply; bool bulk_reply; } server_t;
static int listen_loopback(int *port) {
    int fd=socket(AF_INET,SOCK_STREAM,0); assert(fd>=0);
    struct sockaddr_in address={.sin_family=AF_INET,.sin_port=0,.sin_addr.s_addr=htonl(INADDR_LOOPBACK)};
    assert(bind(fd,(struct sockaddr *)&address,sizeof address)==0); assert(listen(fd,1)==0);
    socklen_t size=sizeof address;assert(getsockname(fd,(struct sockaddr *)&address,&size)==0);*port=ntohs(address.sin_port);return fd;
}
static void *serve(void *argument) {
    server_t *server=argument;int fd=accept(server->listener,NULL,NULL);assert(fd>=0);
    if(server->delay_ms) sleep_ms(server->delay_ms);
    if(server->bulk_reply) {
        char reply[65];reply[0]='H';memset(reply+1,'x',sizeof reply-1);
        assert(send(fd,reply,sizeof reply,MSG_NOSIGNAL)==sizeof reply);
        char byte;while(recv(fd,&byte,1,0)>0) {}
        close(fd);return NULL;
    }
    if(server->handshake) (void)send(fd,"H",1,0);
    if(server->single_reply) {
        sleep_ms(server->reply_delay_ms);
        (void)send(fd,"x",1,MSG_NOSIGNAL);
        close(fd);return NULL;
    }
    if(server->read_request) { char byte; for(int i=0;i<40;i++) { sleep_ms(15); if(recv(fd,&byte,1,MSG_DONTWAIT)==0) break; } }
    else for(int i=0;i<30 && server->drip_ms;i++) { sleep_ms(server->drip_ms); if(send(fd,"x",1,MSG_NOSIGNAL)!=1) break; }
    close(fd);return NULL;
}
static void wait_tcpip(void) { for(int i=0;i<500 && atomic_load(&tcpip_pending);i++) sleep_ms(1);assert(atomic_load(&tcpip_pending)==0); }
static void wait_dns(void) { for(int i=0;i<500 && atomic_load(&dns_pending);i++) sleep_ms(1);assert(atomic_load(&dns_pending)==0); }
static void run_case(const char *name,int port,int total_ms,int connect_ms,int expected_connect,int mode,int server_delay_ms) {
    (void)server_delay_ms;
    atomic_store(&tls_mode,mode);
    eota_http_deadline_t deadline; assert(eota_http_deadline_init(&deadline,total_ms,total_ms));
    int64_t start=deadline.started_us;
    esp_transport_handle_t transport=eota_http_transport_create(&deadline,connect_ms);assert(transport);
    int connected=transport->connect(transport,name,port,1000);
    assert((connected==0)==expected_connect);
    if(connected<0 && mode==TLS_SLOW_HANDSHAKE) { int64_t elapsed=(esp_timer_get_time()-start)/1000;assert(elapsed>=65); }
    if(connected==0) {
        if(mode==TLS_SLOW_WRITE) { int64_t before=esp_timer_get_time();char request[64];memset(request,'Q',sizeof request);int count=0;
            /* The 15 ms TLS work cost is a controlled clock step. Host
             * nanosleep can overshoot the whole 170 ms deadline under load. */
            atomic_store(&virtual_clock_us,before);atomic_store(&virtual_clock_active,true);
            while(transport->write(transport,request,sizeof request,1000)>0) count++;
            int64_t elapsed=(esp_timer_get_time()-before)/1000;
            atomic_store(&virtual_clock_active,false);
            assert(count>2 && elapsed>=90 && elapsed<total_ms+100); }
        else if(mode==TLS_NORMAL) {
            char byte;int reads=0;
            /* Bulk bytes plus controlled work steps test repeated reads and
             * the shared absolute deadline without depending on pthread sleep. */
            atomic_store(&virtual_clock_us,esp_timer_get_time());atomic_store(&virtual_clock_active,true);
            while(transport->read(transport,&byte,1,1000)>0) reads++;
            int64_t elapsed=(esp_timer_get_time()-start)/1000;
            atomic_store(&virtual_clock_active,false);
            assert(reads>1 && elapsed>=130 && elapsed<total_ms+100);
        }
        transport->close(transport);
    }
    assert(esp_transport_destroy(transport)==ESP_OK);
}
static void test_read_result_contract(void) {
    int port;
    server_t server={.handshake=true,.single_reply=true,.reply_delay_ms=80};
    server.listener=listen_loopback(&port);
    pthread_t thread;assert(pthread_create(&thread,NULL,serve,&server)==0);
    atomic_store(&tls_mode,TLS_NORMAL);
    eota_http_deadline_t deadline;assert(eota_http_deadline_init(&deadline,500,300));
    esp_transport_handle_t transport=eota_http_transport_create(&deadline,150);
    assert(transport && transport->connect(transport,"localhost",port,150)==0);
    char byte;
    /* A per-read timeout must remain retryable until the idle deadline. */
    assert(transport->read(transport,&byte,1,20)==ERR_TCP_TRANSPORT_CONNECTION_TIMEOUT);
    assert(transport->read(transport,&byte,1,200)==1 && byte=='x');
    assert(transport->read(transport,&byte,1,200)==ERR_TCP_TRANSPORT_CONNECTION_CLOSED_BY_FIN);
    atomic_store(&tls_mode,TLS_FATAL_READ);
    assert(transport->read(transport,&byte,1,20)==ERR_TCP_TRANSPORT_CONNECTION_FAILED);
    assert(esp_transport_destroy(transport)==ESP_OK);
    pthread_join(thread,NULL);close(server.listener);
    atomic_store(&tls_mode,TLS_NORMAL);
}
static void test_absolute_read_deadline(int total_ms,int idle_ms) {
    int port;
    server_t server={.handshake=true,.single_reply=true,.reply_delay_ms=600};
    server.listener=listen_loopback(&port);
    pthread_t thread;assert(pthread_create(&thread,NULL,serve,&server)==0);
    eota_http_deadline_t deadline;assert(eota_http_deadline_init(&deadline,total_ms,idle_ms));
    int64_t start=deadline.started_us;
    esp_transport_handle_t transport=eota_http_transport_create(&deadline,100);
    assert(transport && transport->connect(transport,"localhost",port,100)==0);
    char byte;int result;
    do {
        result=transport->read(transport,&byte,1,20);
    } while(result==ERR_TCP_TRANSPORT_CONNECTION_TIMEOUT);
    /* A busy host may consume the absolute budget during one read. The
     * preceding result-contract test covers retryability while time remains. */
    assert(result==ERR_TCP_TRANSPORT_CONNECTION_FAILED &&
           eota_http_deadline_remaining_us(&deadline)==0);
    int64_t elapsed_ms=(esp_timer_get_time()-start)/1000;
    assert(elapsed_ms>=160);
    assert(esp_transport_destroy(transport)==ESP_OK);
    pthread_join(thread,NULL);close(server.listener);
}
static void test_single_read_deadline_across_slow_tls_record(void) {
    int port;
    server_t server={.bulk_reply=true};
    server.listener=listen_loopback(&port);
    pthread_t thread;assert(pthread_create(&thread,NULL,serve,&server)==0);
    atomic_store(&tls_mode,TLS_SLOW_RECORD);
    eota_http_deadline_t deadline;assert(eota_http_deadline_init(&deadline,800,400));
    esp_transport_handle_t transport=eota_http_transport_create(&deadline,100);
    assert(transport && transport->connect(transport,"localhost",port,100)==0);
    char byte=0;
    int64_t started_us=esp_timer_get_time();
    atomic_store(&virtual_clock_us,started_us);atomic_store(&virtual_clock_active,true);
    int result=transport->read(transport,&byte,1,65);
    int64_t elapsed_ms=(esp_timer_get_time()-started_us)/1000;
    assert(result==ERR_TCP_TRANSPORT_CONNECTION_TIMEOUT);
    assert(elapsed_ms>=50 && elapsed_ms<120);
    int retries=1;
    while((result=transport->read(transport,&byte,1,65))==ERR_TCP_TRANSPORT_CONNECTION_TIMEOUT) {
        retries++;
        assert(retries<12);
    }
    assert(result==1 && byte=='D' && retries>=2);
    atomic_store(&virtual_clock_active,false);
    assert(esp_transport_destroy(transport)==ESP_OK);
    pthread_join(thread,NULL);close(server.listener);
    atomic_store(&tls_mode,TLS_NORMAL);
}
static void test_single_read_deadline_across_tickets(void) {
    int port;
    server_t server={.bulk_reply=true};
    server.listener=listen_loopback(&port);
    pthread_t thread;assert(pthread_create(&thread,NULL,serve,&server)==0);
    atomic_store(&tls_mode,TLS_NORMAL);
    eota_http_deadline_t deadline;assert(eota_http_deadline_init(&deadline,800,800));
    esp_transport_handle_t transport=eota_http_transport_create(&deadline,100);
    assert(transport && transport->connect(transport,"localhost",port,100)==0);
    atomic_store(&tls_mode,TLS_REPEATED_TICKETS);
    char byte=0;
    int64_t started_us=esp_timer_get_time();
    atomic_store(&virtual_clock_us,started_us);atomic_store(&virtual_clock_active,true);
    int result=transport->read(transport,&byte,1,25);
    int64_t elapsed_ms=(esp_timer_get_time()-started_us)/1000;
    assert(result==ERR_TCP_TRANSPORT_CONNECTION_TIMEOUT);
    assert(elapsed_ms>=20 && elapsed_ms<500);
    assert(eota_http_deadline_remaining_us(&deadline)>0);
    atomic_store(&tls_mode,TLS_NORMAL);
    assert(transport->read(transport,&byte,1,100)==1 && byte=='x');
    atomic_store(&virtual_clock_active,false);
    assert(esp_transport_destroy(transport)==ESP_OK);
    pthread_join(thread,NULL);close(server.listener);
}
int main(void) {
    signal(SIGPIPE,SIG_IGN);
    int port;atomic_store(&dns_delay_ms,180);
    run_case("dns-hang",443,500,60,0,TLS_NORMAL,0);
    /* The abandoned lookup still owns its callback allocation; a second
     * request must fail without queuing another unbounded DNS job. */
    int64_t retry_start=esp_timer_get_time();
    run_case("dns-hang",443,500,60,0,TLS_NORMAL,0);
    assert(atomic_load(&dns_pending)==1);
    assert((esp_timer_get_time()-retry_start)/1000 < 100);
    wait_dns(); atomic_store(&dns_delay_ms,0);
    /* The TCP/IP callback itself can remain queued past the caller deadline.
     * The late callback must release its reference without starting DNS. */
    int lookups=atomic_load(&dns_lookup_count);
    atomic_store(&tcpip_delay_ms,180);
    run_case("localhost",443,500,60,0,TLS_NORMAL,0);
    assert(atomic_load(&tcpip_pending)==1);
    wait_tcpip();assert(atomic_load(&dns_lookup_count)==lookups);
    atomic_store(&tcpip_delay_ms,0);
    run_case("invalid",443,300,100,0,TLS_NORMAL,0);
    run_case("localhost",1,300,100,0,TLS_NORMAL,0);
    server_t slow={.delay_ms=170,.handshake=true};slow.listener=listen_loopback(&port);pthread_t thread;assert(pthread_create(&thread,NULL,serve,&slow)==0);
    run_case("localhost",port,400,90,0,TLS_SLOW_HANDSHAKE,0);
    pthread_join(thread,NULL);close(slow.listener);
    server_t writer={.handshake=true,.read_request=true};writer.listener=listen_loopback(&port);assert(pthread_create(&thread,NULL,serve,&writer)==0);
    int hosts_before=atomic_load(&hostname_set_count),auth_before=atomic_load(&verify_required_count),bundle_before=atomic_load(&bundle_attach_count);
    run_case("LoCaLhOsT",port,170,100,1,TLS_SLOW_WRITE,0);
    assert(strcmp(observed_hostname,"LoCaLhOsT")==0);
    assert(atomic_load(&hostname_set_count)==hosts_before+1);
    assert(atomic_load(&verify_required_count)==auth_before+1);
    assert(atomic_load(&bundle_attach_count)==bundle_before+1);
    pthread_join(thread,NULL);close(writer.listener);
    server_t reader={.handshake=true,.bulk_reply=true};reader.listener=listen_loopback(&port);assert(pthread_create(&thread,NULL,serve,&reader)==0);
    run_case("localhost",port,170,100,1,TLS_NORMAL,0);
    pthread_join(thread,NULL);close(reader.listener);
    server_t bad_cert={.handshake=true};bad_cert.listener=listen_loopback(&port);assert(pthread_create(&thread,NULL,serve,&bad_cert)==0);
    atomic_store(&verification_flags,1);int checks_before=atomic_load(&verification_checks);
    run_case("localhost",port,400,200,0,TLS_VERIFY_FAIL,0);
    assert(atomic_load(&verification_checks)==checks_before+1);
    atomic_store(&verification_flags,0);pthread_join(thread,NULL);close(bad_cert.listener);
    test_read_result_contract();
    test_absolute_read_deadline(240,500);
    test_absolute_read_deadline(500,240);
    test_single_read_deadline_across_slow_tls_record();
    test_single_read_deadline_across_tickets();
    puts("  transport DNS, TCP, TLS and write/read deadlines passed");return 0;
}
