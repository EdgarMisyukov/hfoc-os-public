#ifndef HFOC_LWIP_TCP_TX_BACKPRESSURE_H
#define HFOC_LWIP_TCP_TX_BACKPRESSURE_H

#include "hfoc_lwip_tcp_pool.h"

#include "lwip/tcp.h"

#include <stdbool.h>
#include <stdint.h>

#define HFOC_LWIP_TCP_TX_BACKPRESSURE_VERSION 1U
#define HFOC_LWIP_TCP_TX_BACKPRESSURE_SLOT_COUNT \
    HFOC_LWIP_TCP_POOL_SLOT_COUNT
#define HFOC_LWIP_TCP_TX_BACKPRESSURE_RESPONSE_CAPACITY_MIN 1U
#define HFOC_LWIP_TCP_TX_BACKPRESSURE_RESPONSE_CAPACITY_MAX 4096U
#define HFOC_LWIP_TCP_TX_BACKPRESSURE_RESPONSE_CAPACITY_DEFAULT 2048U
#define HFOC_LWIP_TCP_TX_BACKPRESSURE_CHUNK_CAPACITY_MIN 1U
#define HFOC_LWIP_TCP_TX_BACKPRESSURE_CHUNK_CAPACITY_MAX 4096U
#define HFOC_LWIP_TCP_TX_BACKPRESSURE_CHUNK_CAPACITY_DEFAULT 536U

enum hfoc_lwip_tcp_tx_backpressure_state {
    HFOC_LWIP_TCP_TX_BACKPRESSURE_STATE_FREE = 0,
    HFOC_LWIP_TCP_TX_BACKPRESSURE_STATE_ACTIVE = 1,
    HFOC_LWIP_TCP_TX_BACKPRESSURE_STATE_BLOCKED = 2,
};

enum hfoc_lwip_tcp_tx_backpressure_decision {
    HFOC_LWIP_TCP_TX_BACKPRESSURE_DECISION_SUBMITTED = 0,
    HFOC_LWIP_TCP_TX_BACKPRESSURE_DECISION_RETAINED = 1,
    HFOC_LWIP_TCP_TX_BACKPRESSURE_DECISION_REJECTED = 2,
    HFOC_LWIP_TCP_TX_BACKPRESSURE_DECISION_COMPLETE = 3,
};

enum hfoc_lwip_tcp_tx_backpressure_status {
    HFOC_LWIP_TCP_TX_BACKPRESSURE_OK = 0,
    HFOC_LWIP_TCP_TX_BACKPRESSURE_NULL = 1,
    HFOC_LWIP_TCP_TX_BACKPRESSURE_BAD_STATE = 2,
    HFOC_LWIP_TCP_TX_BACKPRESSURE_BAD_TOKEN = 3,
    HFOC_LWIP_TCP_TX_BACKPRESSURE_STALE_TOKEN = 4,
    HFOC_LWIP_TCP_TX_BACKPRESSURE_NOT_ATTACHED = 5,
    HFOC_LWIP_TCP_TX_BACKPRESSURE_ALREADY_ATTACHED = 6,
    HFOC_LWIP_TCP_TX_BACKPRESSURE_DUPLICATE_PCB = 7,
    HFOC_LWIP_TCP_TX_BACKPRESSURE_INVALID_POLICY = 8,
    HFOC_LWIP_TCP_TX_BACKPRESSURE_INVALID_PCB = 9,
    HFOC_LWIP_TCP_TX_BACKPRESSURE_INVALID_LENGTH = 10,
    HFOC_LWIP_TCP_TX_BACKPRESSURE_RESPONSE_PENDING = 11,
    HFOC_LWIP_TCP_TX_BACKPRESSURE_NO_RESPONSE = 12,
    HFOC_LWIP_TCP_TX_BACKPRESSURE_INVALID_ACK = 13,
    HFOC_LWIP_TCP_TX_BACKPRESSURE_LIVE_OUTSTANDING = 14,
    HFOC_LWIP_TCP_TX_BACKPRESSURE_RETRY = 15,
    HFOC_LWIP_TCP_TX_BACKPRESSURE_REJECTED = 16,
    HFOC_LWIP_TCP_TX_BACKPRESSURE_EMPTY = 17,
};

struct hfoc_lwip_tcp_tx_backpressure_policy {
    uint32_t version;
    uint32_t response_capacity;
    uint32_t chunk_capacity;
};

struct hfoc_lwip_tcp_tx_backpressure_entry {
    bool active;
    bool blocked;
    bool response_pending;
    uint32_t generation;
    struct tcp_pcb *pcb;
    struct hfoc_lwip_tcp_tx_backpressure_policy policy;
    uint32_t response_length;
    uint32_t acked_offset;
    uint32_t published_offset;
    uint8_t response[
        HFOC_LWIP_TCP_TX_BACKPRESSURE_RESPONSE_CAPACITY_MAX];
    uint32_t submit_count;
    uint32_t publish_count;
    uint32_t sent_count;
    uint32_t retained_count;
    uint32_t rejected_count;
    uint32_t complete_count;
};

struct hfoc_lwip_tcp_tx_backpressure {
    bool published;
    uint32_t version;
    uint32_t active_count;
    struct hfoc_lwip_tcp_tx_backpressure_entry
        entries[HFOC_LWIP_TCP_TX_BACKPRESSURE_SLOT_COUNT];
};

enum hfoc_lwip_tcp_tx_backpressure_status
hfoc_lwip_tcp_tx_backpressure_init(
    struct hfoc_lwip_tcp_tx_backpressure *gate);
enum hfoc_lwip_tcp_tx_backpressure_status
hfoc_lwip_tcp_tx_backpressure_attach(
    struct hfoc_lwip_tcp_tx_backpressure *gate,
    const struct hfoc_lwip_tcp_pool_token *token,
    struct tcp_pcb *pcb,
    const struct hfoc_lwip_tcp_tx_backpressure_policy *policy);
enum hfoc_lwip_tcp_tx_backpressure_status
hfoc_lwip_tcp_tx_backpressure_submit(
    struct hfoc_lwip_tcp_tx_backpressure *gate,
    const struct hfoc_lwip_tcp_pool_token *token,
    struct tcp_pcb *pcb,
    const uint8_t *response,
    uint32_t length,
    enum hfoc_lwip_tcp_tx_backpressure_decision *decision);
enum hfoc_lwip_tcp_tx_backpressure_status
hfoc_lwip_tcp_tx_backpressure_publish(
    struct hfoc_lwip_tcp_tx_backpressure *gate,
    const struct hfoc_lwip_tcp_pool_token *token,
    struct tcp_pcb *pcb,
    uint32_t *published_length,
    enum hfoc_lwip_tcp_tx_backpressure_decision *decision);
enum hfoc_lwip_tcp_tx_backpressure_status
hfoc_lwip_tcp_tx_backpressure_sent(
    struct hfoc_lwip_tcp_tx_backpressure *gate,
    const struct hfoc_lwip_tcp_pool_token *token,
    struct tcp_pcb *pcb,
    uint32_t length);
enum hfoc_lwip_tcp_tx_backpressure_status
hfoc_lwip_tcp_tx_backpressure_detach(
    struct hfoc_lwip_tcp_tx_backpressure *gate,
    const struct hfoc_lwip_tcp_pool_token *token);
bool hfoc_lwip_tcp_tx_backpressure_published(
    const struct hfoc_lwip_tcp_tx_backpressure *gate);
uint32_t hfoc_lwip_tcp_tx_backpressure_active_count(
    const struct hfoc_lwip_tcp_tx_backpressure *gate);
enum hfoc_lwip_tcp_tx_backpressure_state
hfoc_lwip_tcp_tx_backpressure_state(
    const struct hfoc_lwip_tcp_tx_backpressure *gate,
    const struct hfoc_lwip_tcp_pool_token *token);
bool hfoc_lwip_tcp_tx_backpressure_response_pending(
    const struct hfoc_lwip_tcp_tx_backpressure *gate,
    const struct hfoc_lwip_tcp_pool_token *token);
bool hfoc_lwip_tcp_tx_backpressure_blocked(
    const struct hfoc_lwip_tcp_tx_backpressure *gate,
    const struct hfoc_lwip_tcp_pool_token *token);
uint32_t hfoc_lwip_tcp_tx_backpressure_response_length(
    const struct hfoc_lwip_tcp_tx_backpressure *gate,
    const struct hfoc_lwip_tcp_pool_token *token);
uint32_t hfoc_lwip_tcp_tx_backpressure_acked_offset(
    const struct hfoc_lwip_tcp_tx_backpressure *gate,
    const struct hfoc_lwip_tcp_pool_token *token);
uint32_t hfoc_lwip_tcp_tx_backpressure_published_offset(
    const struct hfoc_lwip_tcp_tx_backpressure *gate,
    const struct hfoc_lwip_tcp_pool_token *token);
uint32_t hfoc_lwip_tcp_tx_backpressure_outstanding_bytes(
    const struct hfoc_lwip_tcp_tx_backpressure *gate,
    const struct hfoc_lwip_tcp_pool_token *token);
uint32_t hfoc_lwip_tcp_tx_backpressure_submit_count(
    const struct hfoc_lwip_tcp_tx_backpressure *gate,
    const struct hfoc_lwip_tcp_pool_token *token);
uint32_t hfoc_lwip_tcp_tx_backpressure_publish_count(
    const struct hfoc_lwip_tcp_tx_backpressure *gate,
    const struct hfoc_lwip_tcp_pool_token *token);
uint32_t hfoc_lwip_tcp_tx_backpressure_sent_count(
    const struct hfoc_lwip_tcp_tx_backpressure *gate,
    const struct hfoc_lwip_tcp_pool_token *token);
uint32_t hfoc_lwip_tcp_tx_backpressure_retained_count(
    const struct hfoc_lwip_tcp_tx_backpressure *gate,
    const struct hfoc_lwip_tcp_pool_token *token);
uint32_t hfoc_lwip_tcp_tx_backpressure_rejected_count(
    const struct hfoc_lwip_tcp_tx_backpressure *gate,
    const struct hfoc_lwip_tcp_pool_token *token);
uint32_t hfoc_lwip_tcp_tx_backpressure_complete_count(
    const struct hfoc_lwip_tcp_tx_backpressure *gate,
    const struct hfoc_lwip_tcp_pool_token *token);
const char *hfoc_lwip_tcp_tx_backpressure_state_string(
    enum hfoc_lwip_tcp_tx_backpressure_state state);
const char *hfoc_lwip_tcp_tx_backpressure_decision_string(
    enum hfoc_lwip_tcp_tx_backpressure_decision decision);
const char *hfoc_lwip_tcp_tx_backpressure_status_string(
    enum hfoc_lwip_tcp_tx_backpressure_status status);

#endif
