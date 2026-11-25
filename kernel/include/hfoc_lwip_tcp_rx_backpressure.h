#ifndef HFOC_LWIP_TCP_RX_BACKPRESSURE_H
#define HFOC_LWIP_TCP_RX_BACKPRESSURE_H

#include "hfoc_lwip_tcp_pool.h"

#include "lwip/tcp.h"

#include <stdbool.h>
#include <stdint.h>

#define HFOC_LWIP_TCP_RX_BACKPRESSURE_VERSION 1U
#define HFOC_LWIP_TCP_RX_BACKPRESSURE_SLOT_COUNT \
    HFOC_LWIP_TCP_POOL_SLOT_COUNT
#define HFOC_LWIP_TCP_RX_BACKPRESSURE_PACKET_CAPACITY_MIN 1U
#define HFOC_LWIP_TCP_RX_BACKPRESSURE_PACKET_CAPACITY_MAX 64U
#define HFOC_LWIP_TCP_RX_BACKPRESSURE_PACKET_CAPACITY_DEFAULT 4U
#define HFOC_LWIP_TCP_RX_BACKPRESSURE_APPLICATION_CAPACITY_MIN 1U
#define HFOC_LWIP_TCP_RX_BACKPRESSURE_APPLICATION_CAPACITY_MAX 2048U
#define HFOC_LWIP_TCP_RX_BACKPRESSURE_APPLICATION_CAPACITY_DEFAULT 2048U

enum hfoc_lwip_tcp_rx_backpressure_mode {
    HFOC_LWIP_TCP_RX_BACKPRESSURE_MODE_PAUSE = 0,
    HFOC_LWIP_TCP_RX_BACKPRESSURE_MODE_REJECT = 1,
};

enum hfoc_lwip_tcp_rx_backpressure_state {
    HFOC_LWIP_TCP_RX_BACKPRESSURE_STATE_FREE = 0,
    HFOC_LWIP_TCP_RX_BACKPRESSURE_STATE_ACTIVE = 1,
    HFOC_LWIP_TCP_RX_BACKPRESSURE_STATE_PAUSED = 2,
};

enum hfoc_lwip_tcp_rx_backpressure_decision {
    HFOC_LWIP_TCP_RX_BACKPRESSURE_DECISION_ACCEPT = 0,
    HFOC_LWIP_TCP_RX_BACKPRESSURE_DECISION_PAUSE = 1,
    HFOC_LWIP_TCP_RX_BACKPRESSURE_DECISION_REJECT = 2,
};

enum hfoc_lwip_tcp_rx_backpressure_status {
    HFOC_LWIP_TCP_RX_BACKPRESSURE_OK = 0,
    HFOC_LWIP_TCP_RX_BACKPRESSURE_NULL = 1,
    HFOC_LWIP_TCP_RX_BACKPRESSURE_BAD_STATE = 2,
    HFOC_LWIP_TCP_RX_BACKPRESSURE_BAD_TOKEN = 3,
    HFOC_LWIP_TCP_RX_BACKPRESSURE_STALE_TOKEN = 4,
    HFOC_LWIP_TCP_RX_BACKPRESSURE_NOT_ATTACHED = 5,
    HFOC_LWIP_TCP_RX_BACKPRESSURE_ALREADY_ATTACHED = 6,
    HFOC_LWIP_TCP_RX_BACKPRESSURE_DUPLICATE_PCB = 7,
    HFOC_LWIP_TCP_RX_BACKPRESSURE_INVALID_POLICY = 8,
    HFOC_LWIP_TCP_RX_BACKPRESSURE_INVALID_PCB = 9,
    HFOC_LWIP_TCP_RX_BACKPRESSURE_INVALID_LENGTH = 10,
    HFOC_LWIP_TCP_RX_BACKPRESSURE_PAUSED = 11,
    HFOC_LWIP_TCP_RX_BACKPRESSURE_REJECTED = 12,
    HFOC_LWIP_TCP_RX_BACKPRESSURE_BAD_RELEASE = 13,
    HFOC_LWIP_TCP_RX_BACKPRESSURE_LIVE_OUTSTANDING = 14,
};

struct hfoc_lwip_tcp_rx_backpressure_policy {
    uint32_t version;
    enum hfoc_lwip_tcp_rx_backpressure_mode mode;
    uint32_t packet_capacity;
    uint32_t application_capacity;
};

struct hfoc_lwip_tcp_rx_backpressure_entry {
    bool active;
    bool paused;
    uint32_t generation;
    struct tcp_pcb *pcb;
    struct hfoc_lwip_tcp_rx_backpressure_policy policy;
    uint32_t packet_head;
    uint32_t packet_tail;
    uint32_t outstanding_packets;
    uint32_t outstanding_bytes;
    uint32_t accepted_packets[
        HFOC_LWIP_TCP_RX_BACKPRESSURE_PACKET_CAPACITY_MAX];
    uint32_t accepted_count;
    uint32_t paused_count;
    uint32_t rejected_count;
    uint32_t released_count;
};

struct hfoc_lwip_tcp_rx_backpressure {
    bool published;
    uint32_t version;
    uint32_t active_count;
    struct hfoc_lwip_tcp_rx_backpressure_entry
        entries[HFOC_LWIP_TCP_RX_BACKPRESSURE_SLOT_COUNT];
};

enum hfoc_lwip_tcp_rx_backpressure_status
hfoc_lwip_tcp_rx_backpressure_init(
    struct hfoc_lwip_tcp_rx_backpressure *gate);
enum hfoc_lwip_tcp_rx_backpressure_status
hfoc_lwip_tcp_rx_backpressure_attach(
    struct hfoc_lwip_tcp_rx_backpressure *gate,
    const struct hfoc_lwip_tcp_pool_token *token,
    struct tcp_pcb *pcb,
    const struct hfoc_lwip_tcp_rx_backpressure_policy *policy);
enum hfoc_lwip_tcp_rx_backpressure_status
hfoc_lwip_tcp_rx_backpressure_receive(
    struct hfoc_lwip_tcp_rx_backpressure *gate,
    const struct hfoc_lwip_tcp_pool_token *token,
    struct tcp_pcb *pcb,
    uint32_t length,
    enum hfoc_lwip_tcp_rx_backpressure_decision *decision);
enum hfoc_lwip_tcp_rx_backpressure_status
hfoc_lwip_tcp_rx_backpressure_release(
    struct hfoc_lwip_tcp_rx_backpressure *gate,
    const struct hfoc_lwip_tcp_pool_token *token,
    struct tcp_pcb *pcb,
    uint32_t length);
enum hfoc_lwip_tcp_rx_backpressure_status
hfoc_lwip_tcp_rx_backpressure_detach(
    struct hfoc_lwip_tcp_rx_backpressure *gate,
    const struct hfoc_lwip_tcp_pool_token *token);
bool hfoc_lwip_tcp_rx_backpressure_published(
    const struct hfoc_lwip_tcp_rx_backpressure *gate);
uint32_t hfoc_lwip_tcp_rx_backpressure_active_count(
    const struct hfoc_lwip_tcp_rx_backpressure *gate);
enum hfoc_lwip_tcp_rx_backpressure_state
hfoc_lwip_tcp_rx_backpressure_state(
    const struct hfoc_lwip_tcp_rx_backpressure *gate,
    const struct hfoc_lwip_tcp_pool_token *token);
bool hfoc_lwip_tcp_rx_backpressure_paused(
    const struct hfoc_lwip_tcp_rx_backpressure *gate,
    const struct hfoc_lwip_tcp_pool_token *token);
uint32_t hfoc_lwip_tcp_rx_backpressure_outstanding_packets(
    const struct hfoc_lwip_tcp_rx_backpressure *gate,
    const struct hfoc_lwip_tcp_pool_token *token);
uint32_t hfoc_lwip_tcp_rx_backpressure_outstanding_bytes(
    const struct hfoc_lwip_tcp_rx_backpressure *gate,
    const struct hfoc_lwip_tcp_pool_token *token);
uint32_t hfoc_lwip_tcp_rx_backpressure_accepted_count(
    const struct hfoc_lwip_tcp_rx_backpressure *gate,
    const struct hfoc_lwip_tcp_pool_token *token);
uint32_t hfoc_lwip_tcp_rx_backpressure_paused_count(
    const struct hfoc_lwip_tcp_rx_backpressure *gate,
    const struct hfoc_lwip_tcp_pool_token *token);
uint32_t hfoc_lwip_tcp_rx_backpressure_rejected_count(
    const struct hfoc_lwip_tcp_rx_backpressure *gate,
    const struct hfoc_lwip_tcp_pool_token *token);
uint32_t hfoc_lwip_tcp_rx_backpressure_released_count(
    const struct hfoc_lwip_tcp_rx_backpressure *gate,
    const struct hfoc_lwip_tcp_pool_token *token);
const char *hfoc_lwip_tcp_rx_backpressure_mode_string(
    enum hfoc_lwip_tcp_rx_backpressure_mode mode);
const char *hfoc_lwip_tcp_rx_backpressure_state_string(
    enum hfoc_lwip_tcp_rx_backpressure_state state);
const char *hfoc_lwip_tcp_rx_backpressure_decision_string(
    enum hfoc_lwip_tcp_rx_backpressure_decision decision);
const char *hfoc_lwip_tcp_rx_backpressure_status_string(
    enum hfoc_lwip_tcp_rx_backpressure_status status);

#endif
