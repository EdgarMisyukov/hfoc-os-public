#ifndef HFOC_LWIP_TCP_QUARANTINE_H
#define HFOC_LWIP_TCP_QUARANTINE_H

#include "hfoc_lwip_tcp_pool.h"

#include "lwip/tcp.h"

#include <stdbool.h>
#include <stdint.h>

#define HFOC_LWIP_TCP_QUARANTINE_VERSION 1U
#define HFOC_LWIP_TCP_QUARANTINE_SLOT_COUNT \
    HFOC_LWIP_TCP_POOL_SLOT_COUNT
#define HFOC_LWIP_TCP_QUARANTINE_KIND_COUNT 8U

enum hfoc_lwip_tcp_quarantine_kind {
    HFOC_LWIP_TCP_QUARANTINE_KIND_NONE = 0,
    HFOC_LWIP_TCP_QUARANTINE_KIND_ACCEPT = 1,
    HFOC_LWIP_TCP_QUARANTINE_KIND_RECEIVE = 2,
    HFOC_LWIP_TCP_QUARANTINE_KIND_SENT = 3,
    HFOC_LWIP_TCP_QUARANTINE_KIND_POLL = 4,
    HFOC_LWIP_TCP_QUARANTINE_KIND_ERROR = 5,
    HFOC_LWIP_TCP_QUARANTINE_KIND_CLOSE = 6,
    HFOC_LWIP_TCP_QUARANTINE_KIND_ABORT = 7,
};

enum hfoc_lwip_tcp_quarantine_reason {
    HFOC_LWIP_TCP_QUARANTINE_REASON_NONE = 0,
    HFOC_LWIP_TCP_QUARANTINE_REASON_BAD_TOKEN = 1,
    HFOC_LWIP_TCP_QUARANTINE_REASON_NOT_ATTACHED = 2,
    HFOC_LWIP_TCP_QUARANTINE_REASON_STALE_TOKEN = 3,
    HFOC_LWIP_TCP_QUARANTINE_REASON_INVALID_CONNECTION = 4,
    HFOC_LWIP_TCP_QUARANTINE_REASON_INVALID_PCB = 5,
    HFOC_LWIP_TCP_QUARANTINE_REASON_INVALID_STATE = 6,
    HFOC_LWIP_TCP_QUARANTINE_REASON_INVALID_KIND = 7,
};

enum hfoc_lwip_tcp_quarantine_status {
    HFOC_LWIP_TCP_QUARANTINE_OK = 0,
    HFOC_LWIP_TCP_QUARANTINE_NULL = 1,
    HFOC_LWIP_TCP_QUARANTINE_BAD_STATE = 2,
    HFOC_LWIP_TCP_QUARANTINE_BAD_TOKEN = 3,
    HFOC_LWIP_TCP_QUARANTINE_NOT_ATTACHED = 4,
    HFOC_LWIP_TCP_QUARANTINE_STALE_TOKEN = 5,
    HFOC_LWIP_TCP_QUARANTINE_ALREADY_ATTACHED = 6,
    HFOC_LWIP_TCP_QUARANTINE_INVALID_CONNECTION = 7,
    HFOC_LWIP_TCP_QUARANTINE_INVALID_PCB = 8,
    HFOC_LWIP_TCP_QUARANTINE_INVALID_STATE = 9,
    HFOC_LWIP_TCP_QUARANTINE_INVALID_KIND = 10,
    HFOC_LWIP_TCP_QUARANTINE_LIVE_CONNECTION = 11,
    HFOC_LWIP_TCP_QUARANTINE_DUPLICATE_PCB = 12,
    HFOC_LWIP_TCP_QUARANTINE_DUPLICATE_CONNECTION = 13,
};

struct hfoc_lwip_tcp_quarantine_entry {
    bool active;
    uint32_t generation;
    struct hfoc_lwip_tcp_connection *connection;
    struct tcp_pcb *pcb;
    uint32_t accepted_count;
    uint32_t quarantined_count;
    uint32_t stale_token_count;
    uint32_t invalid_connection_count;
    uint32_t invalid_pcb_count;
    uint32_t invalid_state_count;
    uint32_t invalid_kind_count;
    uint32_t kind_counts[
        HFOC_LWIP_TCP_QUARANTINE_KIND_COUNT];
    enum hfoc_lwip_tcp_quarantine_reason last_reason;
    enum hfoc_lwip_tcp_quarantine_kind last_kind;
    enum hfoc_lwip_tcp_conn_state last_expected_state;
    uint32_t last_index;
    uint32_t last_generation;
};

struct hfoc_lwip_tcp_quarantine {
    bool published;
    uint32_t version;
    uint32_t active_count;
    uint32_t quarantined_count;
    struct hfoc_lwip_tcp_quarantine_entry
        entries[HFOC_LWIP_TCP_QUARANTINE_SLOT_COUNT];
    enum hfoc_lwip_tcp_quarantine_reason last_reason;
    enum hfoc_lwip_tcp_quarantine_kind last_kind;
    enum hfoc_lwip_tcp_conn_state last_expected_state;
    uint32_t last_index;
    uint32_t last_generation;
};

enum hfoc_lwip_tcp_quarantine_status
hfoc_lwip_tcp_quarantine_init(
    struct hfoc_lwip_tcp_quarantine *gate);
enum hfoc_lwip_tcp_quarantine_status
hfoc_lwip_tcp_quarantine_attach(
    struct hfoc_lwip_tcp_quarantine *gate,
    const struct hfoc_lwip_tcp_pool_token *token,
    struct hfoc_lwip_tcp_connection *connection,
    struct tcp_pcb *pcb);
enum hfoc_lwip_tcp_quarantine_status
hfoc_lwip_tcp_quarantine_check(
    struct hfoc_lwip_tcp_quarantine *gate,
    const struct hfoc_lwip_tcp_pool_token *token,
    struct hfoc_lwip_tcp_connection *connection,
    struct tcp_pcb *pcb,
    enum hfoc_lwip_tcp_conn_state expected_state,
    enum hfoc_lwip_tcp_quarantine_kind kind);
enum hfoc_lwip_tcp_quarantine_status
hfoc_lwip_tcp_quarantine_detach(
    struct hfoc_lwip_tcp_quarantine *gate,
    const struct hfoc_lwip_tcp_pool_token *token);
bool hfoc_lwip_tcp_quarantine_published(
    const struct hfoc_lwip_tcp_quarantine *gate);
uint32_t hfoc_lwip_tcp_quarantine_active_count(
    const struct hfoc_lwip_tcp_quarantine *gate);
uint32_t hfoc_lwip_tcp_quarantine_quarantined_count(
    const struct hfoc_lwip_tcp_quarantine *gate);
uint32_t hfoc_lwip_tcp_quarantine_accepted_count(
    const struct hfoc_lwip_tcp_quarantine *gate,
    const struct hfoc_lwip_tcp_pool_token *token);
uint32_t hfoc_lwip_tcp_quarantine_entry_quarantined_count(
    const struct hfoc_lwip_tcp_quarantine *gate,
    const struct hfoc_lwip_tcp_pool_token *token);
uint32_t hfoc_lwip_tcp_quarantine_stale_token_count(
    const struct hfoc_lwip_tcp_quarantine *gate,
    const struct hfoc_lwip_tcp_pool_token *token);
uint32_t hfoc_lwip_tcp_quarantine_invalid_connection_count(
    const struct hfoc_lwip_tcp_quarantine *gate,
    const struct hfoc_lwip_tcp_pool_token *token);
uint32_t hfoc_lwip_tcp_quarantine_invalid_pcb_count(
    const struct hfoc_lwip_tcp_quarantine *gate,
    const struct hfoc_lwip_tcp_pool_token *token);
uint32_t hfoc_lwip_tcp_quarantine_invalid_state_count(
    const struct hfoc_lwip_tcp_quarantine *gate,
    const struct hfoc_lwip_tcp_pool_token *token);
uint32_t hfoc_lwip_tcp_quarantine_invalid_kind_count(
    const struct hfoc_lwip_tcp_quarantine *gate,
    const struct hfoc_lwip_tcp_pool_token *token);
uint32_t hfoc_lwip_tcp_quarantine_kind_count(
    const struct hfoc_lwip_tcp_quarantine *gate,
    const struct hfoc_lwip_tcp_pool_token *token,
    enum hfoc_lwip_tcp_quarantine_kind kind);
enum hfoc_lwip_tcp_quarantine_reason
hfoc_lwip_tcp_quarantine_last_reason(
    const struct hfoc_lwip_tcp_quarantine *gate);
enum hfoc_lwip_tcp_quarantine_kind
hfoc_lwip_tcp_quarantine_last_kind(
    const struct hfoc_lwip_tcp_quarantine *gate);
enum hfoc_lwip_tcp_conn_state
hfoc_lwip_tcp_quarantine_last_expected_state(
    const struct hfoc_lwip_tcp_quarantine *gate);
uint32_t hfoc_lwip_tcp_quarantine_last_index(
    const struct hfoc_lwip_tcp_quarantine *gate);
uint32_t hfoc_lwip_tcp_quarantine_last_generation(
    const struct hfoc_lwip_tcp_quarantine *gate);
const char *hfoc_lwip_tcp_quarantine_kind_string(
    enum hfoc_lwip_tcp_quarantine_kind kind);
const char *hfoc_lwip_tcp_quarantine_reason_string(
    enum hfoc_lwip_tcp_quarantine_reason reason);
const char *hfoc_lwip_tcp_quarantine_status_string(
    enum hfoc_lwip_tcp_quarantine_status status);

#endif
