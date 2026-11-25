#ifndef HFOC_LWIP_TCP_DRAIN_RESTART_H
#define HFOC_LWIP_TCP_DRAIN_RESTART_H

#include "hfoc_lwip_tcp.h"
#include "hfoc_lwip_tcp_backlog.h"
#include "hfoc_lwip_tcp_close_policy.h"
#include "hfoc_lwip_tcp_deadline.h"
#include "hfoc_lwip_tcp_pool.h"
#include "hfoc_lwip_tcp_quarantine.h"

#include <stdbool.h>
#include <stdint.h>

#define HFOC_LWIP_TCP_DRAIN_RESTART_VERSION 1U
#define HFOC_LWIP_TCP_DRAIN_RESTART_PHASE_COUNT 5U
#define HFOC_LWIP_TCP_DRAIN_RESTART_DRAIN_TIMEOUT_MIN 1U
#define HFOC_LWIP_TCP_DRAIN_RESTART_DRAIN_TIMEOUT_MAX \
    (UINT32_MAX / 4U)

enum hfoc_lwip_tcp_drain_restart_phase {
    HFOC_LWIP_TCP_DRAIN_RESTART_PHASE_IDLE = 0,
    HFOC_LWIP_TCP_DRAIN_RESTART_PHASE_ADMITTING = 1,
    HFOC_LWIP_TCP_DRAIN_RESTART_PHASE_DRAINING = 2,
    HFOC_LWIP_TCP_DRAIN_RESTART_PHASE_DRAINED = 3,
    HFOC_LWIP_TCP_DRAIN_RESTART_PHASE_RESTARTED = 4,
};

enum hfoc_lwip_tcp_drain_restart_status {
    HFOC_LWIP_TCP_DRAIN_RESTART_OK = 0,
    HFOC_LWIP_TCP_DRAIN_RESTART_NULL = 1,
    HFOC_LWIP_TCP_DRAIN_RESTART_BAD_STATE = 2,
    HFOC_LWIP_TCP_DRAIN_RESTART_INVALID_VERSION = 3,
    HFOC_LWIP_TCP_DRAIN_RESTART_ALREADY_PUBLISHED = 4,
    HFOC_LWIP_TCP_DRAIN_RESTART_UNPUBLISHED = 5,
    HFOC_LWIP_TCP_DRAIN_RESTART_INVALID_CONFIG = 6,
    HFOC_LWIP_TCP_DRAIN_RESTART_INVALID_LISTENER = 7,
    HFOC_LWIP_TCP_DRAIN_RESTART_INVALID_NETIF = 8,
    HFOC_LWIP_TCP_DRAIN_RESTART_INVALID_POOL = 9,
    HFOC_LWIP_TCP_DRAIN_RESTART_INVALID_DEADLINE = 10,
    HFOC_LWIP_TCP_DRAIN_RESTART_INVALID_BACKLOG = 11,
    HFOC_LWIP_TCP_DRAIN_RESTART_INVALID_CLOSE_POLICY = 12,
    HFOC_LWIP_TCP_DRAIN_RESTART_INVALID_QUARANTINE = 13,
    HFOC_LWIP_TCP_DRAIN_RESTART_INVALID_PHASE = 14,
    HFOC_LWIP_TCP_DRAIN_RESTART_LISTENER_NOT_LIVE = 15,
    HFOC_LWIP_TCP_DRAIN_RESTART_POOL_NOT_EMPTY = 16,
    HFOC_LWIP_TCP_DRAIN_RESTART_BACKLOG_NOT_EMPTY = 17,
    HFOC_LWIP_TCP_DRAIN_RESTART_NOT_DRAINED = 18,
    HFOC_LWIP_TCP_DRAIN_RESTART_STOP_FAILED = 19,
    HFOC_LWIP_TCP_DRAIN_RESTART_REMOVE_FAILED = 20,
    HFOC_LWIP_TCP_DRAIN_RESTART_RESTART_FAILED = 21,
    HFOC_LWIP_TCP_DRAIN_RESTART_GENERATION_EXHAUSTED = 22,
};

struct hfoc_lwip_tcp_drain_restart_config {
    uint32_t version;
    uint32_t drain_timeout_ms;
};

struct hfoc_lwip_tcp_drain_restart {
    bool published;
    uint32_t version;
    enum hfoc_lwip_tcp_drain_restart_phase phase;
    uint32_t drain_timeout_ms;
    uint32_t admission_generation;
    uint32_t drain_deadline_ms;
    uint32_t stopped_count;
    uint32_t drained_count;
    uint32_t aborted_count;
    uint32_t backlog_aborted_count;
    uint32_t restarted_count;
    struct hfoc_lwip_tcp_listener *listener;
    struct hfoc_lwip_netif *netif;
    struct hfoc_lwip_tcp_pool *pool;
    struct hfoc_lwip_tcp_deadline_scheduler *deadline;
    struct hfoc_lwip_tcp_backlog *backlog;
    struct hfoc_lwip_tcp_close_policy *close_policy;
    struct hfoc_lwip_tcp_quarantine *quarantine;
    struct hfoc_lwip_netif netif_snapshot;
    struct network_ipv4_config config_snapshot;
    struct network_ipv4_port_policy port_policy_snapshot;
    struct hfoc_lwip_tcp_callback callback_snapshot;
};

struct hfoc_lwip_tcp_drain_restart_snapshot {
    uint32_t version;
    enum hfoc_lwip_tcp_drain_restart_phase phase;
    uint32_t admission_generation;
    uint32_t pool_free_count;
    uint32_t pool_reserved_count;
    uint32_t pool_published_count;
    uint32_t backlog_pending_count;
    uint32_t deadline_active_count;
    uint32_t close_policy_active_count;
    uint32_t quarantine_active_count;
    uint32_t stopped_count;
    uint32_t drained_count;
    uint32_t aborted_count;
    uint32_t backlog_aborted_count;
    uint32_t restarted_count;
};

enum hfoc_lwip_tcp_drain_restart_status
hfoc_lwip_tcp_drain_restart_init(
    struct hfoc_lwip_tcp_drain_restart *drain,
    const struct hfoc_lwip_tcp_drain_restart_config *config);
enum hfoc_lwip_tcp_drain_restart_status
hfoc_lwip_tcp_drain_restart_start(
    struct hfoc_lwip_tcp_drain_restart *drain,
    struct hfoc_lwip_tcp_listener *listener,
    struct hfoc_lwip_netif *netif,
    struct hfoc_lwip_tcp_pool *pool,
    struct hfoc_lwip_tcp_deadline_scheduler *deadline,
    struct hfoc_lwip_tcp_backlog *backlog,
    struct hfoc_lwip_tcp_close_policy *close_policy,
    struct hfoc_lwip_tcp_quarantine *quarantine);
enum hfoc_lwip_tcp_drain_restart_status
hfoc_lwip_tcp_drain_restart_stop_admission(
    struct hfoc_lwip_tcp_drain_restart *drain,
    uint32_t now_ms);
enum hfoc_lwip_tcp_drain_restart_status
hfoc_lwip_tcp_drain_restart_step(
    struct hfoc_lwip_tcp_drain_restart *drain,
    uint32_t now_ms);
enum hfoc_lwip_tcp_drain_restart_status
hfoc_lwip_tcp_drain_restart_finish_drain(
    struct hfoc_lwip_tcp_drain_restart *drain);
enum hfoc_lwip_tcp_drain_restart_status
hfoc_lwip_tcp_drain_restart_restart(
    struct hfoc_lwip_tcp_drain_restart *drain);
struct hfoc_lwip_tcp_drain_restart_snapshot
hfoc_lwip_tcp_drain_restart_snapshot(
    const struct hfoc_lwip_tcp_drain_restart *drain);
const char *hfoc_lwip_tcp_drain_restart_phase_string(
    enum hfoc_lwip_tcp_drain_restart_phase phase);
const char *hfoc_lwip_tcp_drain_restart_status_string(
    enum hfoc_lwip_tcp_drain_restart_status status);

#endif
