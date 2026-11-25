#ifndef HFOC_LWIP_TCP_CLOSE_POLICY_H
#define HFOC_LWIP_TCP_CLOSE_POLICY_H

#include "hfoc_lwip_tcp_pool.h"
#include "hfoc_lwip_tcp_quarantine.h"

#include "lwip/tcp.h"

#include <stdbool.h>
#include <stdint.h>

#define HFOC_LWIP_TCP_CLOSE_POLICY_VERSION 1U
#define HFOC_LWIP_TCP_CLOSE_POLICY_SLOT_COUNT \
    HFOC_LWIP_TCP_POOL_SLOT_COUNT
#define HFOC_LWIP_TCP_CLOSE_POLICY_REASON_COUNT 6U

enum hfoc_lwip_tcp_close_policy_reason {
    HFOC_LWIP_TCP_CLOSE_POLICY_REASON_NONE = 0,
    HFOC_LWIP_TCP_CLOSE_POLICY_REASON_GRACEFUL_FIN = 1,
    HFOC_LWIP_TCP_CLOSE_POLICY_REASON_PROTOCOL_CLOSE = 2,
    HFOC_LWIP_TCP_CLOSE_POLICY_REASON_TIMEOUT_ABORT = 3,
    HFOC_LWIP_TCP_CLOSE_POLICY_REASON_LINK_RESET = 4,
    HFOC_LWIP_TCP_CLOSE_POLICY_REASON_SERVICE_SHUTDOWN = 5,
};

enum hfoc_lwip_tcp_close_policy_action {
    HFOC_LWIP_TCP_CLOSE_POLICY_ACTION_NONE = 0,
    HFOC_LWIP_TCP_CLOSE_POLICY_ACTION_CLOSE = 1,
    HFOC_LWIP_TCP_CLOSE_POLICY_ACTION_ABORT = 2,
};

enum hfoc_lwip_tcp_close_policy_status {
    HFOC_LWIP_TCP_CLOSE_POLICY_OK = 0,
    HFOC_LWIP_TCP_CLOSE_POLICY_NULL = 1,
    HFOC_LWIP_TCP_CLOSE_POLICY_BAD_STATE = 2,
    HFOC_LWIP_TCP_CLOSE_POLICY_BAD_TOKEN = 3,
    HFOC_LWIP_TCP_CLOSE_POLICY_NOT_ATTACHED = 4,
    HFOC_LWIP_TCP_CLOSE_POLICY_STALE_TOKEN = 5,
    HFOC_LWIP_TCP_CLOSE_POLICY_INVALID_POOL = 6,
    HFOC_LWIP_TCP_CLOSE_POLICY_INVALID_QUARANTINE = 7,
    HFOC_LWIP_TCP_CLOSE_POLICY_INVALID_CONNECTION = 8,
    HFOC_LWIP_TCP_CLOSE_POLICY_INVALID_PCB = 9,
    HFOC_LWIP_TCP_CLOSE_POLICY_INVALID_REASON = 10,
    HFOC_LWIP_TCP_CLOSE_POLICY_ALREADY_ATTACHED = 11,
    HFOC_LWIP_TCP_CLOSE_POLICY_LIVE_CONNECTION = 12,
    HFOC_LWIP_TCP_CLOSE_POLICY_CLOSED = 13,
    HFOC_LWIP_TCP_CLOSE_POLICY_ABORTED = 14,
    HFOC_LWIP_TCP_CLOSE_POLICY_ALREADY_CLOSED = 15,
    HFOC_LWIP_TCP_CLOSE_POLICY_CLOSE_FAILED = 16,
    HFOC_LWIP_TCP_CLOSE_POLICY_ABORT_FAILED = 17,
};

struct hfoc_lwip_tcp_close_policy_entry {
    bool active;
    uint32_t generation;
    struct hfoc_lwip_tcp_pool *pool;
    struct hfoc_lwip_tcp_quarantine *quarantine;
    struct hfoc_lwip_tcp_connection *connection;
    struct tcp_pcb *pcb;
    uint32_t handled_count;
    uint32_t close_count;
    uint32_t abort_count;
    uint32_t rejected_count;
    uint32_t reason_counts[
        HFOC_LWIP_TCP_CLOSE_POLICY_REASON_COUNT];
    enum hfoc_lwip_tcp_close_policy_reason last_reason;
    enum hfoc_lwip_tcp_close_policy_action last_action;
    enum hfoc_lwip_tcp_close_policy_status last_status;
    uint32_t last_index;
    uint32_t last_generation;
};

struct hfoc_lwip_tcp_close_policy {
    bool published;
    uint32_t version;
    uint32_t active_count;
    uint32_t handled_count;
    uint32_t close_count;
    uint32_t abort_count;
    uint32_t rejected_count;
    struct hfoc_lwip_tcp_close_policy_entry
        entries[HFOC_LWIP_TCP_CLOSE_POLICY_SLOT_COUNT];
    enum hfoc_lwip_tcp_close_policy_reason last_reason;
    enum hfoc_lwip_tcp_close_policy_action last_action;
    enum hfoc_lwip_tcp_close_policy_status last_status;
    uint32_t last_index;
    uint32_t last_generation;
};

enum hfoc_lwip_tcp_close_policy_status
hfoc_lwip_tcp_close_policy_init(
    struct hfoc_lwip_tcp_close_policy *policy);
enum hfoc_lwip_tcp_close_policy_status
hfoc_lwip_tcp_close_policy_attach(
    struct hfoc_lwip_tcp_close_policy *policy,
    struct hfoc_lwip_tcp_pool *pool,
    struct hfoc_lwip_tcp_quarantine *quarantine,
    const struct hfoc_lwip_tcp_pool_token *token);
enum hfoc_lwip_tcp_close_policy_status
hfoc_lwip_tcp_close_policy_handle(
    struct hfoc_lwip_tcp_close_policy *policy,
    struct hfoc_lwip_tcp_pool *pool,
    struct hfoc_lwip_tcp_quarantine *quarantine,
    const struct hfoc_lwip_tcp_pool_token *token,
    enum hfoc_lwip_tcp_close_policy_reason reason);
enum hfoc_lwip_tcp_close_policy_status
hfoc_lwip_tcp_close_policy_detach(
    struct hfoc_lwip_tcp_close_policy *policy,
    struct hfoc_lwip_tcp_pool *pool,
    struct hfoc_lwip_tcp_quarantine *quarantine,
    const struct hfoc_lwip_tcp_pool_token *token);
bool hfoc_lwip_tcp_close_policy_published(
    const struct hfoc_lwip_tcp_close_policy *policy);
uint32_t hfoc_lwip_tcp_close_policy_active_count(
    const struct hfoc_lwip_tcp_close_policy *policy);
uint32_t hfoc_lwip_tcp_close_policy_handled_count(
    const struct hfoc_lwip_tcp_close_policy *policy);
uint32_t hfoc_lwip_tcp_close_policy_close_count(
    const struct hfoc_lwip_tcp_close_policy *policy);
uint32_t hfoc_lwip_tcp_close_policy_abort_count(
    const struct hfoc_lwip_tcp_close_policy *policy);
uint32_t hfoc_lwip_tcp_close_policy_rejected_count(
    const struct hfoc_lwip_tcp_close_policy *policy);
uint32_t hfoc_lwip_tcp_close_policy_entry_handled_count(
    const struct hfoc_lwip_tcp_close_policy *policy,
    const struct hfoc_lwip_tcp_pool_token *token);
uint32_t hfoc_lwip_tcp_close_policy_entry_close_count(
    const struct hfoc_lwip_tcp_close_policy *policy,
    const struct hfoc_lwip_tcp_pool_token *token);
uint32_t hfoc_lwip_tcp_close_policy_entry_abort_count(
    const struct hfoc_lwip_tcp_close_policy *policy,
    const struct hfoc_lwip_tcp_pool_token *token);
uint32_t hfoc_lwip_tcp_close_policy_entry_rejected_count(
    const struct hfoc_lwip_tcp_close_policy *policy,
    const struct hfoc_lwip_tcp_pool_token *token);
uint32_t hfoc_lwip_tcp_close_policy_reason_count(
    const struct hfoc_lwip_tcp_close_policy *policy,
    const struct hfoc_lwip_tcp_pool_token *token,
    enum hfoc_lwip_tcp_close_policy_reason reason);
enum hfoc_lwip_tcp_close_policy_reason
hfoc_lwip_tcp_close_policy_last_reason(
    const struct hfoc_lwip_tcp_close_policy *policy);
enum hfoc_lwip_tcp_close_policy_action
hfoc_lwip_tcp_close_policy_last_action(
    const struct hfoc_lwip_tcp_close_policy *policy);
enum hfoc_lwip_tcp_close_policy_status
hfoc_lwip_tcp_close_policy_last_status(
    const struct hfoc_lwip_tcp_close_policy *policy);
uint32_t hfoc_lwip_tcp_close_policy_last_index(
    const struct hfoc_lwip_tcp_close_policy *policy);
uint32_t hfoc_lwip_tcp_close_policy_last_generation(
    const struct hfoc_lwip_tcp_close_policy *policy);
enum hfoc_lwip_tcp_close_policy_action
hfoc_lwip_tcp_close_policy_action_for_reason(
    enum hfoc_lwip_tcp_close_policy_reason reason);
const char *hfoc_lwip_tcp_close_policy_reason_string(
    enum hfoc_lwip_tcp_close_policy_reason reason);
const char *hfoc_lwip_tcp_close_policy_action_string(
    enum hfoc_lwip_tcp_close_policy_action action);
const char *hfoc_lwip_tcp_close_policy_status_string(
    enum hfoc_lwip_tcp_close_policy_status status);

#endif
