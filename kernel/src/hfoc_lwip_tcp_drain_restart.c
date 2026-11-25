#include "hfoc_lwip_tcp_drain_restart.h"

#include "hfoc_lwip_tcp_backlog.h"
#include "hfoc_lwip_tcp_close_policy.h"
#include "hfoc_lwip_tcp_conn.h"
#include "hfoc_lwip_tcp_deadline.h"
#include "hfoc_lwip_tcp_pool.h"
#include "hfoc_lwip_tcp_quarantine.h"

#include "lwip/tcp.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#if defined(HFOC_LWIP_TCP_DRAIN_RESTART_HOST_TEST) || \
    defined(HFOC_NETWORK_RESOURCE_QEMU_TEST)

#define HFOC_LWIP_TCP_DRAIN_RESTART_HALF_RANGE UINT32_C(0x7FFFFFFF)

static bool hfoc_lwip_tcp_drain_restart_phase_valid(
    enum hfoc_lwip_tcp_drain_restart_phase phase)
{
    return phase >= HFOC_LWIP_TCP_DRAIN_RESTART_PHASE_IDLE &&
           phase <= HFOC_LWIP_TCP_DRAIN_RESTART_PHASE_RESTARTED;
}

static bool hfoc_lwip_tcp_drain_restart_timeout_valid(
    uint32_t timeout_ms)
{
    return timeout_ms >=
               HFOC_LWIP_TCP_DRAIN_RESTART_DRAIN_TIMEOUT_MIN &&
           timeout_ms <=
               HFOC_LWIP_TCP_DRAIN_RESTART_DRAIN_TIMEOUT_MAX;
}

static bool hfoc_lwip_tcp_drain_restart_config_valid(
    const struct hfoc_lwip_tcp_drain_restart_config *config)
{
    return config != NULL &&
           config->version ==
               HFOC_LWIP_TCP_DRAIN_RESTART_VERSION &&
           hfoc_lwip_tcp_drain_restart_timeout_valid(
               config->drain_timeout_ms);
}

static bool hfoc_lwip_tcp_drain_restart_valid(
    const struct hfoc_lwip_tcp_drain_restart *drain)
{
    if (drain == NULL ||
        !drain->published ||
        drain->version != HFOC_LWIP_TCP_DRAIN_RESTART_VERSION ||
        !hfoc_lwip_tcp_drain_restart_phase_valid(drain->phase) ||
        !hfoc_lwip_tcp_drain_restart_timeout_valid(
            drain->drain_timeout_ms)) {
        return false;
    }
    if (drain->phase == HFOC_LWIP_TCP_DRAIN_RESTART_PHASE_IDLE) {
        return drain->admission_generation == 0U &&
               drain->drain_deadline_ms == 0U &&
               drain->stopped_count == 0U &&
               drain->drained_count == 0U &&
               drain->aborted_count == 0U &&
               drain->backlog_aborted_count == 0U &&
               drain->restarted_count == 0U &&
               drain->listener == NULL &&
               drain->netif == NULL &&
               drain->pool == NULL &&
               drain->deadline == NULL &&
               drain->backlog == NULL &&
               drain->close_policy == NULL &&
               drain->quarantine == NULL;
    }
    return drain->listener != NULL &&
           drain->netif != NULL &&
           drain->pool != NULL &&
           drain->deadline != NULL &&
           drain->backlog != NULL &&
           drain->close_policy != NULL &&
           drain->quarantine != NULL;
}

static bool hfoc_lwip_tcp_drain_restart_reached(
    uint32_t now_ms,
    uint32_t due_ms)
{
    return (uint32_t)(now_ms - due_ms) <=
        HFOC_LWIP_TCP_DRAIN_RESTART_HALF_RANGE;
}

enum hfoc_lwip_tcp_drain_restart_status
hfoc_lwip_tcp_drain_restart_init(
    struct hfoc_lwip_tcp_drain_restart *drain,
    const struct hfoc_lwip_tcp_drain_restart_config *config)
{
    if (drain == NULL || config == NULL) {
        return HFOC_LWIP_TCP_DRAIN_RESTART_NULL;
    }
    if (!hfoc_lwip_tcp_drain_restart_config_valid(config)) {
        return HFOC_LWIP_TCP_DRAIN_RESTART_INVALID_CONFIG;
    }
    if (drain->published) {
        return HFOC_LWIP_TCP_DRAIN_RESTART_BAD_STATE;
    }

    memset(drain, 0, sizeof(*drain));
    drain->version = HFOC_LWIP_TCP_DRAIN_RESTART_VERSION;
    drain->drain_timeout_ms = config->drain_timeout_ms;
    drain->phase = HFOC_LWIP_TCP_DRAIN_RESTART_PHASE_IDLE;
    drain->published = true;
    return HFOC_LWIP_TCP_DRAIN_RESTART_OK;
}

static enum hfoc_lwip_tcp_drain_restart_status
hfoc_lwip_tcp_drain_restart_validate_start(
    const struct hfoc_lwip_tcp_drain_restart *drain,
    const struct hfoc_lwip_tcp_listener *listener,
    const struct hfoc_lwip_netif *netif,
    const struct hfoc_lwip_tcp_pool *pool,
    const struct hfoc_lwip_tcp_deadline_scheduler *deadline,
    const struct hfoc_lwip_tcp_backlog *backlog,
    const struct hfoc_lwip_tcp_close_policy *close_policy,
    const struct hfoc_lwip_tcp_quarantine *quarantine)
{
    uint32_t index;

    if (drain == NULL || listener == NULL || netif == NULL ||
        pool == NULL || deadline == NULL || backlog == NULL ||
        close_policy == NULL || quarantine == NULL) {
        return HFOC_LWIP_TCP_DRAIN_RESTART_NULL;
    }
    if (!hfoc_lwip_tcp_drain_restart_valid(drain)) {
        return HFOC_LWIP_TCP_DRAIN_RESTART_BAD_STATE;
    }
    if (drain->phase != HFOC_LWIP_TCP_DRAIN_RESTART_PHASE_IDLE) {
        return HFOC_LWIP_TCP_DRAIN_RESTART_INVALID_PHASE;
    }
    if (!hfoc_lwip_tcp_listening(listener)) {
        return HFOC_LWIP_TCP_DRAIN_RESTART_LISTENER_NOT_LIVE;
    }
    if (!hfoc_lwip_netif_live(netif)) {
        return HFOC_LWIP_TCP_DRAIN_RESTART_INVALID_NETIF;
    }
    if (netif->generation != listener->generation) {
        return HFOC_LWIP_TCP_DRAIN_RESTART_INVALID_NETIF;
    }
    if (!hfoc_lwip_tcp_pool_published(pool)) {
        return HFOC_LWIP_TCP_DRAIN_RESTART_INVALID_POOL;
    }
    if (pool->reserved_count != 0U ||
        pool->free_count + pool->published_count !=
            HFOC_LWIP_TCP_POOL_SLOT_COUNT) {
        return HFOC_LWIP_TCP_DRAIN_RESTART_INVALID_POOL;
    }
    if (!hfoc_lwip_tcp_deadline_published(deadline)) {
        return HFOC_LWIP_TCP_DRAIN_RESTART_INVALID_DEADLINE;
    }
    if (deadline->active_count != pool->published_count) {
        return HFOC_LWIP_TCP_DRAIN_RESTART_INVALID_DEADLINE;
    }
    if (!hfoc_lwip_tcp_backlog_published(backlog)) {
        return HFOC_LWIP_TCP_DRAIN_RESTART_INVALID_BACKLOG;
    }
    if (backlog->pending_count != 0U) {
        return HFOC_LWIP_TCP_DRAIN_RESTART_INVALID_BACKLOG;
    }
    if (!hfoc_lwip_tcp_close_policy_published(close_policy)) {
        return HFOC_LWIP_TCP_DRAIN_RESTART_INVALID_CLOSE_POLICY;
    }
    if (close_policy->active_count != pool->published_count) {
        return HFOC_LWIP_TCP_DRAIN_RESTART_INVALID_CLOSE_POLICY;
    }
    if (!hfoc_lwip_tcp_quarantine_published(quarantine)) {
        return HFOC_LWIP_TCP_DRAIN_RESTART_INVALID_QUARANTINE;
    }
    if (quarantine->active_count != pool->published_count) {
        return HFOC_LWIP_TCP_DRAIN_RESTART_INVALID_QUARANTINE;
    }

    for (index = 0U; index < HFOC_LWIP_TCP_POOL_SLOT_COUNT;
         ++index) {
        const struct hfoc_lwip_tcp_pool_slot *slot =
            &pool->slots[index];
        const struct hfoc_lwip_tcp_connection *connection;

        if (slot->state != HFOC_LWIP_TCP_POOL_STATE_PUBLISHED) {
            continue;
        }
        connection = &slot->connection;
        if (!deadline->entries[index].active ||
            deadline->entries[index].generation !=
                slot->generation) {
            return HFOC_LWIP_TCP_DRAIN_RESTART_INVALID_DEADLINE;
        }
        if (!quarantine->entries[index].active ||
            quarantine->entries[index].generation !=
                slot->generation ||
            quarantine->entries[index].connection != connection ||
            quarantine->entries[index].pcb != connection->pcb) {
            return HFOC_LWIP_TCP_DRAIN_RESTART_INVALID_QUARANTINE;
        }
        if (!close_policy->entries[index].active ||
            close_policy->entries[index].generation !=
                slot->generation ||
            close_policy->entries[index].pool != pool ||
            close_policy->entries[index].quarantine !=
                quarantine ||
            close_policy->entries[index].connection !=
                connection) {
            return HFOC_LWIP_TCP_DRAIN_RESTART_INVALID_CLOSE_POLICY;
        }
    }
    return HFOC_LWIP_TCP_DRAIN_RESTART_OK;
}

enum hfoc_lwip_tcp_drain_restart_status
hfoc_lwip_tcp_drain_restart_start(
    struct hfoc_lwip_tcp_drain_restart *drain,
    struct hfoc_lwip_tcp_listener *listener,
    struct hfoc_lwip_netif *netif,
    struct hfoc_lwip_tcp_pool *pool,
    struct hfoc_lwip_tcp_deadline_scheduler *deadline,
    struct hfoc_lwip_tcp_backlog *backlog,
    struct hfoc_lwip_tcp_close_policy *close_policy,
    struct hfoc_lwip_tcp_quarantine *quarantine)
{
    enum hfoc_lwip_tcp_drain_restart_status status;

    status = hfoc_lwip_tcp_drain_restart_validate_start(
        drain, listener, netif, pool, deadline, backlog,
        close_policy, quarantine);
    if (status != HFOC_LWIP_TCP_DRAIN_RESTART_OK) {
        return status;
    }

    drain->listener = listener;
    drain->netif = netif;
    drain->pool = pool;
    drain->deadline = deadline;
    drain->backlog = backlog;
    drain->close_policy = close_policy;
    drain->quarantine = quarantine;
    drain->netif_snapshot = listener->netif;
    drain->config_snapshot = listener->config;
    drain->port_policy_snapshot = listener->port_policy;
    drain->callback_snapshot = listener->callback;
    drain->admission_generation = listener->generation;
    drain->phase = HFOC_LWIP_TCP_DRAIN_RESTART_PHASE_ADMITTING;
    return HFOC_LWIP_TCP_DRAIN_RESTART_OK;
}

enum hfoc_lwip_tcp_drain_restart_status
hfoc_lwip_tcp_drain_restart_stop_admission(
    struct hfoc_lwip_tcp_drain_restart *drain,
    uint32_t now_ms)
{
    enum hfoc_lwip_tcp_status listener_status;

    if (drain == NULL) {
        return HFOC_LWIP_TCP_DRAIN_RESTART_NULL;
    }
    if (!hfoc_lwip_tcp_drain_restart_valid(drain)) {
        return HFOC_LWIP_TCP_DRAIN_RESTART_BAD_STATE;
    }
    if (drain->phase !=
            HFOC_LWIP_TCP_DRAIN_RESTART_PHASE_ADMITTING &&
        drain->phase !=
            HFOC_LWIP_TCP_DRAIN_RESTART_PHASE_RESTARTED) {
        return HFOC_LWIP_TCP_DRAIN_RESTART_INVALID_PHASE;
    }
    if (!hfoc_lwip_tcp_listening(drain->listener)) {
        return HFOC_LWIP_TCP_DRAIN_RESTART_LISTENER_NOT_LIVE;
    }

    listener_status = hfoc_lwip_tcp_shutdown(drain->listener);
    if (listener_status != HFOC_LWIP_TCP_OK) {
        return HFOC_LWIP_TCP_DRAIN_RESTART_STOP_FAILED;
    }
    drain->drain_deadline_ms =
        (uint32_t)(now_ms + drain->drain_timeout_ms);
    drain->phase = HFOC_LWIP_TCP_DRAIN_RESTART_PHASE_DRAINING;
    ++drain->stopped_count;
    return HFOC_LWIP_TCP_DRAIN_RESTART_OK;
}

static enum hfoc_lwip_tcp_drain_restart_status
hfoc_lwip_tcp_drain_restart_release_slot(
    struct hfoc_lwip_tcp_drain_restart *drain,
    const struct hfoc_lwip_tcp_pool_token *token)
{
    enum hfoc_lwip_tcp_deadline_status deadline_status;
    enum hfoc_lwip_tcp_close_policy_status close_status;
    enum hfoc_lwip_tcp_quarantine_status quarantine_status;
    enum hfoc_lwip_tcp_pool_status pool_status;

    deadline_status = hfoc_lwip_tcp_deadline_cancel(
        drain->deadline, token);
    if (deadline_status != HFOC_LWIP_TCP_DEADLINE_OK) {
        return HFOC_LWIP_TCP_DRAIN_RESTART_BAD_STATE;
    }
    close_status = hfoc_lwip_tcp_close_policy_detach(
        drain->close_policy, drain->pool, drain->quarantine,
        token);
    if (close_status != HFOC_LWIP_TCP_CLOSE_POLICY_OK) {
        return HFOC_LWIP_TCP_DRAIN_RESTART_BAD_STATE;
    }
    quarantine_status = hfoc_lwip_tcp_quarantine_detach(
        drain->quarantine, token);
    if (quarantine_status != HFOC_LWIP_TCP_QUARANTINE_OK) {
        return HFOC_LWIP_TCP_DRAIN_RESTART_BAD_STATE;
    }
    pool_status = hfoc_lwip_tcp_pool_release(
        drain->pool, token);
    if (pool_status != HFOC_LWIP_TCP_POOL_OK) {
        return HFOC_LWIP_TCP_DRAIN_RESTART_BAD_STATE;
    }
    return HFOC_LWIP_TCP_DRAIN_RESTART_OK;
}

enum hfoc_lwip_tcp_drain_restart_status
hfoc_lwip_tcp_drain_restart_step(
    struct hfoc_lwip_tcp_drain_restart *drain,
    uint32_t now_ms)
{
    enum hfoc_lwip_tcp_drain_restart_status status;
    bool drain_expired;
    uint32_t index;

    if (drain == NULL) {
        return HFOC_LWIP_TCP_DRAIN_RESTART_NULL;
    }
    if (!hfoc_lwip_tcp_drain_restart_valid(drain)) {
        return HFOC_LWIP_TCP_DRAIN_RESTART_BAD_STATE;
    }
    if (drain->phase != HFOC_LWIP_TCP_DRAIN_RESTART_PHASE_DRAINING) {
        return HFOC_LWIP_TCP_DRAIN_RESTART_INVALID_PHASE;
    }

    while (drain->backlog->pending_count != 0U) {
        struct hfoc_lwip_tcp_backlog_token backlog_token;
        struct tcp_pcb *pcb = NULL;
        enum hfoc_lwip_tcp_backlog_status backlog_status;

        backlog_status = hfoc_lwip_tcp_backlog_drain(
            drain->backlog, &backlog_token, &pcb);
        if (backlog_status != HFOC_LWIP_TCP_BACKLOG_OK ||
            pcb == NULL) {
            return HFOC_LWIP_TCP_DRAIN_RESTART_BAD_STATE;
        }
        tcp_abort(pcb);
        ++drain->backlog_aborted_count;
    }

    drain_expired = hfoc_lwip_tcp_drain_restart_reached(
        now_ms, drain->drain_deadline_ms);
    for (index = 0U; index < HFOC_LWIP_TCP_POOL_SLOT_COUNT;
         ++index) {
        struct hfoc_lwip_tcp_pool_slot *slot =
            &drain->pool->slots[index];
        struct hfoc_lwip_tcp_connection *connection;
        struct hfoc_lwip_tcp_pool_token token;
        enum hfoc_lwip_tcp_deadline_kind expired_kind;
        enum hfoc_lwip_tcp_deadline_status deadline_status;

        if (slot->state != HFOC_LWIP_TCP_POOL_STATE_PUBLISHED) {
            if (slot->state == HFOC_LWIP_TCP_POOL_STATE_RESERVED) {
                return HFOC_LWIP_TCP_DRAIN_RESTART_BAD_STATE;
            }
            continue;
        }
        connection = &slot->connection;
        token.index = index;
        token.generation = slot->generation;

        if (connection->state == HFOC_LWIP_TCP_CONN_STATE_CLOSED ||
            connection->state == HFOC_LWIP_TCP_CONN_STATE_ABORTED) {
            status = hfoc_lwip_tcp_drain_restart_release_slot(
                drain, &token);
            if (status != HFOC_LWIP_TCP_DRAIN_RESTART_OK) {
                return status;
            }
            ++drain->drained_count;
            continue;
        }

        deadline_status = hfoc_lwip_tcp_deadline_check(
            drain->deadline, &token, now_ms, &expired_kind);
        if (deadline_status != HFOC_LWIP_TCP_DEADLINE_OK &&
            deadline_status != HFOC_LWIP_TCP_DEADLINE_EXPIRED) {
            return HFOC_LWIP_TCP_DRAIN_RESTART_BAD_STATE;
        }
        if (deadline_status == HFOC_LWIP_TCP_DEADLINE_EXPIRED ||
            drain_expired) {
            enum hfoc_lwip_tcp_close_policy_reason reason =
                drain_expired
                    ? HFOC_LWIP_TCP_CLOSE_POLICY_REASON_SERVICE_SHUTDOWN
                    : HFOC_LWIP_TCP_CLOSE_POLICY_REASON_TIMEOUT_ABORT;
            enum hfoc_lwip_tcp_close_policy_status close_status =
                hfoc_lwip_tcp_close_policy_handle(
                    drain->close_policy, drain->pool,
                    drain->quarantine, &token, reason);

            if (close_status !=
                    HFOC_LWIP_TCP_CLOSE_POLICY_ABORTED &&
                close_status !=
                    HFOC_LWIP_TCP_CLOSE_POLICY_CLOSED) {
                return HFOC_LWIP_TCP_DRAIN_RESTART_BAD_STATE;
            }
            status = hfoc_lwip_tcp_drain_restart_release_slot(
                drain, &token);
            if (status != HFOC_LWIP_TCP_DRAIN_RESTART_OK) {
                return status;
            }
            ++drain->aborted_count;
        }
    }
    return HFOC_LWIP_TCP_DRAIN_RESTART_OK;
}

enum hfoc_lwip_tcp_drain_restart_status
hfoc_lwip_tcp_drain_restart_finish_drain(
    struct hfoc_lwip_tcp_drain_restart *drain)
{
    enum hfoc_lwip_netif_status netif_status;

    if (drain == NULL) {
        return HFOC_LWIP_TCP_DRAIN_RESTART_NULL;
    }
    if (!hfoc_lwip_tcp_drain_restart_valid(drain)) {
        return HFOC_LWIP_TCP_DRAIN_RESTART_BAD_STATE;
    }
    if (drain->phase != HFOC_LWIP_TCP_DRAIN_RESTART_PHASE_DRAINING) {
        return HFOC_LWIP_TCP_DRAIN_RESTART_INVALID_PHASE;
    }
    if (drain->pool->published_count != 0U ||
        drain->pool->reserved_count != 0U ||
        drain->pool->free_count != HFOC_LWIP_TCP_POOL_SLOT_COUNT) {
        return HFOC_LWIP_TCP_DRAIN_RESTART_POOL_NOT_EMPTY;
    }
    if (drain->backlog->pending_count != 0U) {
        return HFOC_LWIP_TCP_DRAIN_RESTART_BACKLOG_NOT_EMPTY;
    }
    if (drain->deadline->active_count != 0U ||
        drain->close_policy->active_count != 0U ||
        drain->quarantine->active_count != 0U) {
        return HFOC_LWIP_TCP_DRAIN_RESTART_NOT_DRAINED;
    }
    if (!hfoc_lwip_netif_live(drain->netif)) {
        return HFOC_LWIP_TCP_DRAIN_RESTART_INVALID_NETIF;
    }
    if (hfoc_lwip_tcp_published(drain->listener)) {
        return HFOC_LWIP_TCP_DRAIN_RESTART_LISTENER_NOT_LIVE;
    }

    netif_status = hfoc_lwip_netif_remove(drain->netif);
    if (netif_status != HFOC_LWIP_NETIF_OK) {
        return HFOC_LWIP_TCP_DRAIN_RESTART_REMOVE_FAILED;
    }
    drain->phase = HFOC_LWIP_TCP_DRAIN_RESTART_PHASE_DRAINED;
    ++drain->drained_count;
    return HFOC_LWIP_TCP_DRAIN_RESTART_OK;
}

enum hfoc_lwip_tcp_drain_restart_status
hfoc_lwip_tcp_drain_restart_restart(
    struct hfoc_lwip_tcp_drain_restart *drain)
{
    enum hfoc_lwip_tcp_status tcp_status;
    uint32_t next_generation;

    if (drain == NULL) {
        return HFOC_LWIP_TCP_DRAIN_RESTART_NULL;
    }
    if (!hfoc_lwip_tcp_drain_restart_valid(drain)) {
        return HFOC_LWIP_TCP_DRAIN_RESTART_BAD_STATE;
    }
    if (drain->phase != HFOC_LWIP_TCP_DRAIN_RESTART_PHASE_DRAINED) {
        return HFOC_LWIP_TCP_DRAIN_RESTART_INVALID_PHASE;
    }
    if (drain->pool->published_count != 0U ||
        drain->pool->reserved_count != 0U ||
        drain->pool->free_count != HFOC_LWIP_TCP_POOL_SLOT_COUNT) {
        return HFOC_LWIP_TCP_DRAIN_RESTART_POOL_NOT_EMPTY;
    }
    if (drain->backlog->pending_count != 0U) {
        return HFOC_LWIP_TCP_DRAIN_RESTART_BACKLOG_NOT_EMPTY;
    }
    if (drain->deadline->active_count != 0U ||
        drain->close_policy->active_count != 0U ||
        drain->quarantine->active_count != 0U) {
        return HFOC_LWIP_TCP_DRAIN_RESTART_NOT_DRAINED;
    }

    next_generation =
        drain->admission_generation ==
            HFOC_LWIP_TCP_GENERATION_MAX
        ? 0U
        : drain->admission_generation + 1U;
    if (next_generation < HFOC_LWIP_TCP_GENERATION_MIN) {
        return HFOC_LWIP_TCP_DRAIN_RESTART_GENERATION_EXHAUSTED;
    }

    *drain->netif = drain->netif_snapshot;
    drain->netif->published = true;
    drain->netif->live = true;
    tcp_status = hfoc_lwip_tcp_init(
        drain->listener, &drain->netif_snapshot,
        &drain->config_snapshot, &drain->port_policy_snapshot,
        &drain->callback_snapshot, next_generation);
    if (tcp_status != HFOC_LWIP_TCP_OK) {
        return HFOC_LWIP_TCP_DRAIN_RESTART_RESTART_FAILED;
    }
    tcp_status = hfoc_lwip_tcp_bind(
        drain->listener, &drain->port_policy_snapshot);
    if (tcp_status != HFOC_LWIP_TCP_OK) {
        (void)hfoc_lwip_tcp_shutdown(drain->listener);
        return HFOC_LWIP_TCP_DRAIN_RESTART_RESTART_FAILED;
    }
    tcp_status = hfoc_lwip_tcp_listen(drain->listener);
    if (tcp_status != HFOC_LWIP_TCP_OK) {
        (void)hfoc_lwip_tcp_shutdown(drain->listener);
        return HFOC_LWIP_TCP_DRAIN_RESTART_RESTART_FAILED;
    }

    drain->admission_generation = next_generation;
    drain->phase = HFOC_LWIP_TCP_DRAIN_RESTART_PHASE_RESTARTED;
    ++drain->restarted_count;
    return HFOC_LWIP_TCP_DRAIN_RESTART_OK;
}

struct hfoc_lwip_tcp_drain_restart_snapshot
hfoc_lwip_tcp_drain_restart_snapshot(
    const struct hfoc_lwip_tcp_drain_restart *drain)
{
    struct hfoc_lwip_tcp_drain_restart_snapshot snapshot;

    memset(&snapshot, 0, sizeof(snapshot));
    if (!hfoc_lwip_tcp_drain_restart_valid(drain)) {
        return snapshot;
    }
    snapshot.version = HFOC_LWIP_TCP_DRAIN_RESTART_VERSION;
    snapshot.phase = drain->phase;
    snapshot.admission_generation = drain->admission_generation;
    if (drain->pool != NULL) {
        snapshot.pool_free_count =
            hfoc_lwip_tcp_pool_free_count(drain->pool);
        snapshot.pool_reserved_count =
            hfoc_lwip_tcp_pool_reserved_count(drain->pool);
        snapshot.pool_published_count =
            hfoc_lwip_tcp_pool_published_count(drain->pool);
    }
    if (drain->backlog != NULL) {
        snapshot.backlog_pending_count =
            hfoc_lwip_tcp_backlog_pending_count(drain->backlog);
    }
    if (drain->deadline != NULL) {
        snapshot.deadline_active_count =
            hfoc_lwip_tcp_deadline_active_count(drain->deadline);
    }
    if (drain->close_policy != NULL) {
        snapshot.close_policy_active_count =
            hfoc_lwip_tcp_close_policy_active_count(
                drain->close_policy);
    }
    if (drain->quarantine != NULL) {
        snapshot.quarantine_active_count =
            hfoc_lwip_tcp_quarantine_active_count(
                drain->quarantine);
    }
    snapshot.stopped_count = drain->stopped_count;
    snapshot.drained_count = drain->drained_count;
    snapshot.aborted_count = drain->aborted_count;
    snapshot.backlog_aborted_count =
        drain->backlog_aborted_count;
    snapshot.restarted_count = drain->restarted_count;
    return snapshot;
}

const char *hfoc_lwip_tcp_drain_restart_phase_string(
    enum hfoc_lwip_tcp_drain_restart_phase phase)
{
    switch (phase) {
    case HFOC_LWIP_TCP_DRAIN_RESTART_PHASE_IDLE:
        return "HFOC_LWIP_TCP_DRAIN_RESTART_PHASE_IDLE";
    case HFOC_LWIP_TCP_DRAIN_RESTART_PHASE_ADMITTING:
        return "HFOC_LWIP_TCP_DRAIN_RESTART_PHASE_ADMITTING";
    case HFOC_LWIP_TCP_DRAIN_RESTART_PHASE_DRAINING:
        return "HFOC_LWIP_TCP_DRAIN_RESTART_PHASE_DRAINING";
    case HFOC_LWIP_TCP_DRAIN_RESTART_PHASE_DRAINED:
        return "HFOC_LWIP_TCP_DRAIN_RESTART_PHASE_DRAINED";
    case HFOC_LWIP_TCP_DRAIN_RESTART_PHASE_RESTARTED:
        return "HFOC_LWIP_TCP_DRAIN_RESTART_PHASE_RESTARTED";
    default:
        return NULL;
    }
}

const char *hfoc_lwip_tcp_drain_restart_status_string(
    enum hfoc_lwip_tcp_drain_restart_status status)
{
    switch (status) {
    case HFOC_LWIP_TCP_DRAIN_RESTART_OK:
        return "HFOC_LWIP_TCP_DRAIN_RESTART_OK";
    case HFOC_LWIP_TCP_DRAIN_RESTART_NULL:
        return "HFOC_LWIP_TCP_DRAIN_RESTART_NULL";
    case HFOC_LWIP_TCP_DRAIN_RESTART_BAD_STATE:
        return "HFOC_LWIP_TCP_DRAIN_RESTART_BAD_STATE";
    case HFOC_LWIP_TCP_DRAIN_RESTART_INVALID_VERSION:
        return "HFOC_LWIP_TCP_DRAIN_RESTART_INVALID_VERSION";
    case HFOC_LWIP_TCP_DRAIN_RESTART_ALREADY_PUBLISHED:
        return "HFOC_LWIP_TCP_DRAIN_RESTART_ALREADY_PUBLISHED";
    case HFOC_LWIP_TCP_DRAIN_RESTART_UNPUBLISHED:
        return "HFOC_LWIP_TCP_DRAIN_RESTART_UNPUBLISHED";
    case HFOC_LWIP_TCP_DRAIN_RESTART_INVALID_CONFIG:
        return "HFOC_LWIP_TCP_DRAIN_RESTART_INVALID_CONFIG";
    case HFOC_LWIP_TCP_DRAIN_RESTART_INVALID_LISTENER:
        return "HFOC_LWIP_TCP_DRAIN_RESTART_INVALID_LISTENER";
    case HFOC_LWIP_TCP_DRAIN_RESTART_INVALID_NETIF:
        return "HFOC_LWIP_TCP_DRAIN_RESTART_INVALID_NETIF";
    case HFOC_LWIP_TCP_DRAIN_RESTART_INVALID_POOL:
        return "HFOC_LWIP_TCP_DRAIN_RESTART_INVALID_POOL";
    case HFOC_LWIP_TCP_DRAIN_RESTART_INVALID_DEADLINE:
        return "HFOC_LWIP_TCP_DRAIN_RESTART_INVALID_DEADLINE";
    case HFOC_LWIP_TCP_DRAIN_RESTART_INVALID_BACKLOG:
        return "HFOC_LWIP_TCP_DRAIN_RESTART_INVALID_BACKLOG";
    case HFOC_LWIP_TCP_DRAIN_RESTART_INVALID_CLOSE_POLICY:
        return "HFOC_LWIP_TCP_DRAIN_RESTART_INVALID_CLOSE_POLICY";
    case HFOC_LWIP_TCP_DRAIN_RESTART_INVALID_QUARANTINE:
        return "HFOC_LWIP_TCP_DRAIN_RESTART_INVALID_QUARANTINE";
    case HFOC_LWIP_TCP_DRAIN_RESTART_INVALID_PHASE:
        return "HFOC_LWIP_TCP_DRAIN_RESTART_INVALID_PHASE";
    case HFOC_LWIP_TCP_DRAIN_RESTART_LISTENER_NOT_LIVE:
        return "HFOC_LWIP_TCP_DRAIN_RESTART_LISTENER_NOT_LIVE";
    case HFOC_LWIP_TCP_DRAIN_RESTART_POOL_NOT_EMPTY:
        return "HFOC_LWIP_TCP_DRAIN_RESTART_POOL_NOT_EMPTY";
    case HFOC_LWIP_TCP_DRAIN_RESTART_BACKLOG_NOT_EMPTY:
        return "HFOC_LWIP_TCP_DRAIN_RESTART_BACKLOG_NOT_EMPTY";
    case HFOC_LWIP_TCP_DRAIN_RESTART_NOT_DRAINED:
        return "HFOC_LWIP_TCP_DRAIN_RESTART_NOT_DRAINED";
    case HFOC_LWIP_TCP_DRAIN_RESTART_STOP_FAILED:
        return "HFOC_LWIP_TCP_DRAIN_RESTART_STOP_FAILED";
    case HFOC_LWIP_TCP_DRAIN_RESTART_REMOVE_FAILED:
        return "HFOC_LWIP_TCP_DRAIN_RESTART_REMOVE_FAILED";
    case HFOC_LWIP_TCP_DRAIN_RESTART_RESTART_FAILED:
        return "HFOC_LWIP_TCP_DRAIN_RESTART_RESTART_FAILED";
    case HFOC_LWIP_TCP_DRAIN_RESTART_GENERATION_EXHAUSTED:
        return "HFOC_LWIP_TCP_DRAIN_RESTART_GENERATION_EXHAUSTED";
    default:
        return NULL;
    }
}

#else

typedef int hfoc_lwip_tcp_drain_restart_not_compiled;

#endif
