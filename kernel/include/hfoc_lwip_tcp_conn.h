#ifndef HFOC_LWIP_TCP_CONN_H
#define HFOC_LWIP_TCP_CONN_H

#include "lwip/err.h"
#include "lwip/pbuf.h"
#include "lwip/tcp.h"

#include <stdbool.h>
#include <stdint.h>

#define HFOC_LWIP_TCP_CONN_VERSION 1U
#define HFOC_LWIP_TCP_CONN_GENERATION_MIN 1U
#define HFOC_LWIP_TCP_CONN_GENERATION_MAX UINT32_C(0xFFFFFFFE)
#define HFOC_LWIP_TCP_CONN_POLL_INTERVAL 1U

enum hfoc_lwip_tcp_conn_state {
    HFOC_LWIP_TCP_CONN_STATE_INACTIVE = 0,
    HFOC_LWIP_TCP_CONN_STATE_ESTABLISHED = 1,
    HFOC_LWIP_TCP_CONN_STATE_CLOSING = 2,
    HFOC_LWIP_TCP_CONN_STATE_CLOSED = 3,
    HFOC_LWIP_TCP_CONN_STATE_ABORTED = 4,
};

enum hfoc_lwip_tcp_conn_event {
    HFOC_LWIP_TCP_CONN_EVENT_ACCEPT = 0,
    HFOC_LWIP_TCP_CONN_EVENT_RECEIVE = 1,
    HFOC_LWIP_TCP_CONN_EVENT_SENT = 2,
    HFOC_LWIP_TCP_CONN_EVENT_ERROR = 3,
    HFOC_LWIP_TCP_CONN_EVENT_POLL = 4,
    HFOC_LWIP_TCP_CONN_EVENT_CLOSE = 5,
    HFOC_LWIP_TCP_CONN_EVENT_ABORT = 6,
};

struct hfoc_lwip_tcp_connection;

typedef err_t (*hfoc_lwip_tcp_conn_event_fn)(
    void *context,
    struct hfoc_lwip_tcp_connection *connection,
    enum hfoc_lwip_tcp_conn_event event,
    struct pbuf *p,
    uint16_t len,
    err_t err);

struct hfoc_lwip_tcp_conn_callbacks {
    hfoc_lwip_tcp_conn_event_fn event;
    void *context;
};

struct hfoc_lwip_tcp_connection {
    bool published;
    enum hfoc_lwip_tcp_conn_state state;
    uint32_t version;
    uint32_t generation;
    struct tcp_pcb *pcb;
    struct hfoc_lwip_tcp_conn_callbacks callbacks;
    uint32_t accept_count;
    uint32_t receive_count;
    uint32_t sent_count;
    uint32_t poll_count;
    uint32_t error_count;
    uint32_t close_count;
    uint32_t abort_count;
    uint32_t stale_count;
    uint32_t rejected_count;
    uint32_t received_bytes;
    uint32_t sent_bytes;
};

enum hfoc_lwip_tcp_conn_status {
    HFOC_LWIP_TCP_CONN_OK = 0,
    HFOC_LWIP_TCP_CONN_NULL = 1,
    HFOC_LWIP_TCP_CONN_BAD_STATE = 2,
    HFOC_LWIP_TCP_CONN_INVALID_VERSION = 3,
    HFOC_LWIP_TCP_CONN_ALREADY_PUBLISHED = 4,
    HFOC_LWIP_TCP_CONN_UNPUBLISHED = 5,
    HFOC_LWIP_TCP_CONN_INVALID_GENERATION = 6,
    HFOC_LWIP_TCP_CONN_INVALID_CALLBACK = 7,
    HFOC_LWIP_TCP_CONN_INVALID_PCB = 8,
    HFOC_LWIP_TCP_CONN_STALE_CALLBACK = 9,
    HFOC_LWIP_TCP_CONN_ABORTED = 10,
    HFOC_LWIP_TCP_CONN_CLOSE_FAILED = 11,
    HFOC_LWIP_TCP_CONN_NOT_ESTABLISHED = 12,
    HFOC_LWIP_TCP_CONN_ALREADY_CLOSED = 13,
};

enum hfoc_lwip_tcp_conn_status hfoc_lwip_tcp_conn_accept(
    struct hfoc_lwip_tcp_connection *connection,
    struct tcp_pcb *pcb,
    uint32_t generation,
    const struct hfoc_lwip_tcp_conn_callbacks *callbacks);
enum hfoc_lwip_tcp_conn_status hfoc_lwip_tcp_conn_recv(
    struct hfoc_lwip_tcp_connection *connection,
    struct tcp_pcb *pcb,
    struct pbuf *p,
    err_t err);
enum hfoc_lwip_tcp_conn_status hfoc_lwip_tcp_conn_sent(
    struct hfoc_lwip_tcp_connection *connection,
    struct tcp_pcb *pcb,
    uint16_t len);
enum hfoc_lwip_tcp_conn_status hfoc_lwip_tcp_conn_poll(
    struct hfoc_lwip_tcp_connection *connection,
    struct tcp_pcb *pcb);
enum hfoc_lwip_tcp_conn_status hfoc_lwip_tcp_conn_error(
    struct hfoc_lwip_tcp_connection *connection,
    err_t err);
enum hfoc_lwip_tcp_conn_status hfoc_lwip_tcp_conn_close(
    struct hfoc_lwip_tcp_connection *connection);
enum hfoc_lwip_tcp_conn_status hfoc_lwip_tcp_conn_abort(
    struct hfoc_lwip_tcp_connection *connection);
bool hfoc_lwip_tcp_conn_published(
    const struct hfoc_lwip_tcp_connection *connection);
uint32_t hfoc_lwip_tcp_conn_generation(
    const struct hfoc_lwip_tcp_connection *connection);
const char *hfoc_lwip_tcp_conn_state_string(
    enum hfoc_lwip_tcp_conn_state state);
const char *hfoc_lwip_tcp_conn_event_string(
    enum hfoc_lwip_tcp_conn_event event);
const char *hfoc_lwip_tcp_conn_status_string(
    enum hfoc_lwip_tcp_conn_status status);

#endif
