#ifndef HFOC_LWIP_TCP_ISOLATION_H
#define HFOC_LWIP_TCP_ISOLATION_H

#include "hfoc_lwip_tcp_pool.h"
#include "hfoc_lwip_tcp_quarantine.h"

#include "lwip/tcp.h"

#include <stdbool.h>
#include <stdint.h>

#define HFOC_LWIP_TCP_ISOLATION_VERSION 1U
#define HFOC_LWIP_TCP_ISOLATION_SLOT_COUNT \
    HFOC_LWIP_TCP_POOL_SLOT_COUNT
#define HFOC_LWIP_TCP_ISOLATION_EVENT_COUNT 6U

enum hfoc_lwip_tcp_isolation_event {
    HFOC_LWIP_TCP_ISOLATION_EVENT_NONE = 0,
    HFOC_LWIP_TCP_ISOLATION_EVENT_RESET = 1,
    HFOC_LWIP_TCP_ISOLATION_EVENT_DUPLICATE_COMPLETION = 2,
    HFOC_LWIP_TCP_ISOLATION_EVENT_INVALID_CALLBACK_ORDER = 3,
    HFOC_LWIP_TCP_ISOLATION_EVENT_SHORT_RECEIVE = 4,
    HFOC_LWIP_TCP_ISOLATION_EVENT_REMOTE_ABORT = 5,
};

enum hfoc_lwip_tcp_isolation_status {
    HFOC_LWIP_TCP_ISOLATION_OK = 0,
    HFOC_LWIP_TCP_ISOLATION_NULL = 1,
    HFOC_LWIP_TCP_ISOLATION_BAD_STATE = 2,
    HFOC_LWIP_TCP_ISOLATION_BAD_TOKEN = 3,
    HFOC_LWIP_TCP_ISOLATION_NOT_ATTACHED = 4,
    HFOC_LWIP_TCP_ISOLATION_STALE_TOKEN = 5,
    HFOC_LWIP_TCP_ISOLATION_INVALID_POOL = 6,
    HFOC_LWIP_TCP_ISOLATION_INVALID_QUARANTINE = 7,
    HFOC_LWIP_TCP_ISOLATION_INVALID_CONNECTION = 8,
    HFOC_LWIP_TCP_ISOLATION_INVALID_PCB = 9,
    HFOC_LWIP_TCP_ISOLATION_INVALID_EVENT = 10,
    HFOC_LWIP_TCP_ISOLATION_LIVE_CONNECTION = 11,
    HFOC_LWIP_TCP_ISOLATION_ABORTED = 12,
    HFOC_LWIP_TCP_ISOLATION_ALREADY_CLOSED = 13,
    HFOC_LWIP_TCP_ISOLATION_CLOSE_FAILED = 14,
    HFOC_LWIP_TCP_ISOLATION_ALREADY_ATTACHED = 15,
};

struct hfoc_lwip_tcp_isolation_entry {
    bool active;
    uint32_t generation;
    struct hfoc_lwip_tcp_pool *pool;
    struct hfoc_lwip_tcp_quarantine *quarantine;
    struct hfoc_lwip_tcp_connection *connection;
    struct tcp_pcb *pcb;
    uint32_t event_count;
    uint32_t close_count;
    uint32_t rejected_count;
    uint32_t event_counts[
        HFOC_LWIP_TCP_ISOLATION_EVENT_COUNT];
    enum hfoc_lwip_tcp_isolation_event last_event;
    enum hfoc_lwip_tcp_isolation_status last_status;
    uint32_t last_index;
    uint32_t last_generation;
};

struct hfoc_lwip_tcp_isolation {
    bool published;
    uint32_t version;
    uint32_t active_count;
    uint32_t closed_count;
    uint32_t rejected_count;
    struct hfoc_lwip_tcp_isolation_entry
        entries[HFOC_LWIP_TCP_ISOLATION_SLOT_COUNT];
    enum hfoc_lwip_tcp_isolation_event last_event;
    enum hfoc_lwip_tcp_isolation_status last_status;
    uint32_t last_index;
    uint32_t last_generation;
};

enum hfoc_lwip_tcp_isolation_status
hfoc_lwip_tcp_isolation_init(
    struct hfoc_lwip_tcp_isolation *matrix);
enum hfoc_lwip_tcp_isolation_status
hfoc_lwip_tcp_isolation_attach(
    struct hfoc_lwip_tcp_isolation *matrix,
    struct hfoc_lwip_tcp_pool *pool,
    struct hfoc_lwip_tcp_quarantine *quarantine,
    const struct hfoc_lwip_tcp_pool_token *token);
enum hfoc_lwip_tcp_isolation_status
hfoc_lwip_tcp_isolation_inject(
    struct hfoc_lwip_tcp_isolation *matrix,
    struct hfoc_lwip_tcp_pool *pool,
    struct hfoc_lwip_tcp_quarantine *quarantine,
    const struct hfoc_lwip_tcp_pool_token *token,
    enum hfoc_lwip_tcp_isolation_event event);
enum hfoc_lwip_tcp_isolation_status
hfoc_lwip_tcp_isolation_detach(
    struct hfoc_lwip_tcp_isolation *matrix,
    struct hfoc_lwip_tcp_pool *pool,
    struct hfoc_lwip_tcp_quarantine *quarantine,
    const struct hfoc_lwip_tcp_pool_token *token);
bool hfoc_lwip_tcp_isolation_published(
    const struct hfoc_lwip_tcp_isolation *matrix);
uint32_t hfoc_lwip_tcp_isolation_active_count(
    const struct hfoc_lwip_tcp_isolation *matrix);
uint32_t hfoc_lwip_tcp_isolation_closed_count(
    const struct hfoc_lwip_tcp_isolation *matrix);
uint32_t hfoc_lwip_tcp_isolation_rejected_count(
    const struct hfoc_lwip_tcp_isolation *matrix);
uint32_t hfoc_lwip_tcp_isolation_entry_event_count(
    const struct hfoc_lwip_tcp_isolation *matrix,
    const struct hfoc_lwip_tcp_pool_token *token);
uint32_t hfoc_lwip_tcp_isolation_entry_close_count(
    const struct hfoc_lwip_tcp_isolation *matrix,
    const struct hfoc_lwip_tcp_pool_token *token);
uint32_t hfoc_lwip_tcp_isolation_entry_rejected_count(
    const struct hfoc_lwip_tcp_isolation *matrix,
    const struct hfoc_lwip_tcp_pool_token *token);
uint32_t hfoc_lwip_tcp_isolation_event_count(
    const struct hfoc_lwip_tcp_isolation *matrix,
    const struct hfoc_lwip_tcp_pool_token *token,
    enum hfoc_lwip_tcp_isolation_event event);
enum hfoc_lwip_tcp_isolation_event
hfoc_lwip_tcp_isolation_last_event(
    const struct hfoc_lwip_tcp_isolation *matrix);
enum hfoc_lwip_tcp_isolation_status
hfoc_lwip_tcp_isolation_last_status(
    const struct hfoc_lwip_tcp_isolation *matrix);
uint32_t hfoc_lwip_tcp_isolation_last_index(
    const struct hfoc_lwip_tcp_isolation *matrix);
uint32_t hfoc_lwip_tcp_isolation_last_generation(
    const struct hfoc_lwip_tcp_isolation *matrix);
const char *hfoc_lwip_tcp_isolation_event_string(
    enum hfoc_lwip_tcp_isolation_event event);
const char *hfoc_lwip_tcp_isolation_status_string(
    enum hfoc_lwip_tcp_isolation_status status);

#endif
