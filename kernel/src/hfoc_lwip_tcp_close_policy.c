#include "hfoc_lwip_tcp_close_policy.h"

#include "hfoc_lwip_tcp_conn.h"
#include "hfoc_lwip_tcp_pool.h"
#include "hfoc_lwip_tcp_quarantine.h"

#include "lwip/tcp.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#if defined(HFOC_LWIP_TCP_CLOSE_POLICY_HOST_TEST) || \
    defined(HFOC_NETWORK_RESOURCE_QEMU_TEST)

static bool hfoc_lwip_tcp_close_policy_generation_valid(
    uint32_t generation)
{
    return generation >= HFOC_LWIP_TCP_POOL_GENERATION_MIN &&
           generation <= HFOC_LWIP_TCP_POOL_GENERATION_MAX;
}

static bool hfoc_lwip_tcp_close_policy_reason_valid(
    enum hfoc_lwip_tcp_close_policy_reason reason)
{
    return reason >= HFOC_LWIP_TCP_CLOSE_POLICY_REASON_NONE &&
           reason <=
               HFOC_LWIP_TCP_CLOSE_POLICY_REASON_SERVICE_SHUTDOWN;
}

static enum hfoc_lwip_tcp_close_policy_action
hfoc_lwip_tcp_close_policy_reason_action(
    enum hfoc_lwip_tcp_close_policy_reason reason)
{
    switch (reason) {
    case HFOC_LWIP_TCP_CLOSE_POLICY_REASON_GRACEFUL_FIN:
    case HFOC_LWIP_TCP_CLOSE_POLICY_REASON_PROTOCOL_CLOSE:
        return HFOC_LWIP_TCP_CLOSE_POLICY_ACTION_CLOSE;
    case HFOC_LWIP_TCP_CLOSE_POLICY_REASON_TIMEOUT_ABORT:
    case HFOC_LWIP_TCP_CLOSE_POLICY_REASON_LINK_RESET:
    case HFOC_LWIP_TCP_CLOSE_POLICY_REASON_SERVICE_SHUTDOWN:
        return HFOC_LWIP_TCP_CLOSE_POLICY_ACTION_ABORT;
    case HFOC_LWIP_TCP_CLOSE_POLICY_REASON_NONE:
    default:
        return HFOC_LWIP_TCP_CLOSE_POLICY_ACTION_NONE;
    }
}

static enum hfoc_lwip_tcp_quarantine_kind
hfoc_lwip_tcp_close_policy_quarantine_kind(
    enum hfoc_lwip_tcp_close_policy_action action)
{
    if (action == HFOC_LWIP_TCP_CLOSE_POLICY_ACTION_CLOSE) {
        return HFOC_LWIP_TCP_QUARANTINE_KIND_CLOSE;
    }
    return HFOC_LWIP_TCP_QUARANTINE_KIND_ABORT;
}

static bool hfoc_lwip_tcp_close_policy_entry_valid(
    const struct hfoc_lwip_tcp_close_policy_entry *entry)
{
    if (entry == NULL) {
        return false;
    }
    if (!entry->active) {
        return entry->generation == 0U &&
               entry->pool == NULL &&
               entry->quarantine == NULL &&
               entry->connection == NULL &&
               entry->pcb == NULL &&
               entry->handled_count == 0U &&
               entry->close_count == 0U &&
               entry->abort_count == 0U &&
               entry->rejected_count == 0U &&
               entry->last_reason ==
                   HFOC_LWIP_TCP_CLOSE_POLICY_REASON_NONE &&
               entry->last_action ==
                   HFOC_LWIP_TCP_CLOSE_POLICY_ACTION_NONE &&
               entry->last_status ==
                   HFOC_LWIP_TCP_CLOSE_POLICY_OK &&
               entry->last_index == 0U &&
               entry->last_generation == 0U;
    }
    return hfoc_lwip_tcp_close_policy_generation_valid(
               entry->generation) &&
           entry->pool != NULL &&
           entry->quarantine != NULL &&
           entry->connection != NULL &&
           entry->pcb != NULL;
}

static bool hfoc_lwip_tcp_close_policy_valid(
    const struct hfoc_lwip_tcp_close_policy *policy)
{
    uint32_t active_count;
    uint32_t index;

    if (policy == NULL ||
        !policy->published ||
        policy->version != HFOC_LWIP_TCP_CLOSE_POLICY_VERSION ||
        policy->active_count >
            HFOC_LWIP_TCP_CLOSE_POLICY_SLOT_COUNT ||
        policy->handled_count !=
            policy->close_count + policy->abort_count) {
        return false;
    }

    active_count = 0U;
    for (index = 0U;
         index < HFOC_LWIP_TCP_CLOSE_POLICY_SLOT_COUNT;
         ++index) {
        if (!hfoc_lwip_tcp_close_policy_entry_valid(
                &policy->entries[index])) {
            return false;
        }
        if (policy->entries[index].active) {
            ++active_count;
        }
    }
    return active_count == policy->active_count;
}

static bool hfoc_lwip_tcp_close_policy_lookup(
    const struct hfoc_lwip_tcp_close_policy *policy,
    const struct hfoc_lwip_tcp_pool_token *token,
    const struct hfoc_lwip_tcp_close_policy_entry **entry_out)
{
    const struct hfoc_lwip_tcp_close_policy_entry *entry;

    if (!hfoc_lwip_tcp_close_policy_valid(policy) ||
        token == NULL ||
        token->index >= HFOC_LWIP_TCP_CLOSE_POLICY_SLOT_COUNT ||
        !hfoc_lwip_tcp_close_policy_generation_valid(
            token->generation)) {
        return false;
    }
    entry = &policy->entries[token->index];
    if (!entry->active ||
        entry->generation != token->generation) {
        return false;
    }
    *entry_out = entry;
    return true;
}

static void hfoc_lwip_tcp_close_policy_record(
    struct hfoc_lwip_tcp_close_policy *policy,
    struct hfoc_lwip_tcp_close_policy_entry *entry,
    enum hfoc_lwip_tcp_close_policy_reason reason,
    enum hfoc_lwip_tcp_close_policy_action action,
    enum hfoc_lwip_tcp_close_policy_status status,
    uint32_t index,
    uint32_t generation)
{
    policy->last_reason = reason;
    policy->last_action = action;
    policy->last_status = status;
    policy->last_index = index;
    policy->last_generation = generation;

    if (entry == NULL) {
        return;
    }
    entry->last_reason = reason;
    entry->last_action = action;
    entry->last_status = status;
    entry->last_index = index;
    entry->last_generation = generation;
}

enum hfoc_lwip_tcp_close_policy_status
hfoc_lwip_tcp_close_policy_init(
    struct hfoc_lwip_tcp_close_policy *policy)
{
    if (policy == NULL) {
        return HFOC_LWIP_TCP_CLOSE_POLICY_NULL;
    }
    if (policy->published) {
        return HFOC_LWIP_TCP_CLOSE_POLICY_BAD_STATE;
    }

    memset(policy, 0, sizeof(*policy));
    policy->version = HFOC_LWIP_TCP_CLOSE_POLICY_VERSION;
    policy->published = true;
    return HFOC_LWIP_TCP_CLOSE_POLICY_OK;
}

enum hfoc_lwip_tcp_close_policy_status
hfoc_lwip_tcp_close_policy_attach(
    struct hfoc_lwip_tcp_close_policy *policy,
    struct hfoc_lwip_tcp_pool *pool,
    struct hfoc_lwip_tcp_quarantine *quarantine,
    const struct hfoc_lwip_tcp_pool_token *token)
{
    struct hfoc_lwip_tcp_close_policy_entry *entry;
    struct hfoc_lwip_tcp_connection *connection;
    uint32_t index;

    if (policy == NULL || pool == NULL || quarantine == NULL ||
        token == NULL) {
        return HFOC_LWIP_TCP_CLOSE_POLICY_NULL;
    }
    if (!hfoc_lwip_tcp_close_policy_valid(policy)) {
        return HFOC_LWIP_TCP_CLOSE_POLICY_BAD_STATE;
    }
    if (token->index >= HFOC_LWIP_TCP_CLOSE_POLICY_SLOT_COUNT ||
        !hfoc_lwip_tcp_close_policy_generation_valid(
            token->generation)) {
        return HFOC_LWIP_TCP_CLOSE_POLICY_BAD_TOKEN;
    }
    if (!hfoc_lwip_tcp_pool_published(pool)) {
        return HFOC_LWIP_TCP_CLOSE_POLICY_INVALID_POOL;
    }
    if (pool->slots[token->index].state !=
            HFOC_LWIP_TCP_POOL_STATE_PUBLISHED ||
        pool->slots[token->index].generation != token->generation) {
        return HFOC_LWIP_TCP_CLOSE_POLICY_INVALID_CONNECTION;
    }
    if (!hfoc_lwip_tcp_quarantine_published(quarantine)) {
        return HFOC_LWIP_TCP_CLOSE_POLICY_INVALID_QUARANTINE;
    }
    if (!quarantine->entries[token->index].active ||
        quarantine->entries[token->index].generation !=
            token->generation) {
        return HFOC_LWIP_TCP_CLOSE_POLICY_INVALID_QUARANTINE;
    }

    connection = &pool->slots[token->index].connection;
    if (!connection->published ||
        connection->version != HFOC_LWIP_TCP_CONN_VERSION ||
        !hfoc_lwip_tcp_close_policy_generation_valid(
            connection->generation) ||
        connection->generation != token->generation) {
        return HFOC_LWIP_TCP_CLOSE_POLICY_INVALID_CONNECTION;
    }
    if (connection->state != HFOC_LWIP_TCP_CONN_STATE_ESTABLISHED &&
        connection->state != HFOC_LWIP_TCP_CONN_STATE_CLOSING) {
        return HFOC_LWIP_TCP_CLOSE_POLICY_INVALID_CONNECTION;
    }
    if (connection->pcb == NULL) {
        return HFOC_LWIP_TCP_CLOSE_POLICY_INVALID_PCB;
    }

    entry = &policy->entries[token->index];
    if (entry->active) {
        return entry->generation == token->generation
            ? HFOC_LWIP_TCP_CLOSE_POLICY_ALREADY_ATTACHED
            : HFOC_LWIP_TCP_CLOSE_POLICY_STALE_TOKEN;
    }
    for (index = 0U;
         index < HFOC_LWIP_TCP_CLOSE_POLICY_SLOT_COUNT;
         ++index) {
        if (policy->entries[index].active &&
            policy->entries[index].connection == connection) {
            return HFOC_LWIP_TCP_CLOSE_POLICY_INVALID_CONNECTION;
        }
    }

    memset(entry, 0, sizeof(*entry));
    entry->active = true;
    entry->generation = token->generation;
    entry->pool = pool;
    entry->quarantine = quarantine;
    entry->connection = connection;
    entry->pcb = connection->pcb;
    entry->last_status = HFOC_LWIP_TCP_CLOSE_POLICY_OK;
    ++policy->active_count;
    return HFOC_LWIP_TCP_CLOSE_POLICY_OK;
}

enum hfoc_lwip_tcp_close_policy_status
hfoc_lwip_tcp_close_policy_handle(
    struct hfoc_lwip_tcp_close_policy *policy,
    struct hfoc_lwip_tcp_pool *pool,
    struct hfoc_lwip_tcp_quarantine *quarantine,
    const struct hfoc_lwip_tcp_pool_token *token,
    enum hfoc_lwip_tcp_close_policy_reason reason)
{
    struct hfoc_lwip_tcp_close_policy_entry *entry;
    struct hfoc_lwip_tcp_connection *connection;
    struct tcp_pcb *pcb;
    enum hfoc_lwip_tcp_close_policy_action action;
    enum hfoc_lwip_tcp_quarantine_kind kind;
    enum hfoc_lwip_tcp_quarantine_status quarantine_status;
    enum hfoc_lwip_tcp_pool_status pool_status;

    if (policy == NULL || pool == NULL || quarantine == NULL ||
        token == NULL) {
        return HFOC_LWIP_TCP_CLOSE_POLICY_NULL;
    }
    if (!hfoc_lwip_tcp_close_policy_valid(policy)) {
        return HFOC_LWIP_TCP_CLOSE_POLICY_BAD_STATE;
    }
    if (token->index >= HFOC_LWIP_TCP_CLOSE_POLICY_SLOT_COUNT ||
        !hfoc_lwip_tcp_close_policy_generation_valid(
            token->generation)) {
        return HFOC_LWIP_TCP_CLOSE_POLICY_BAD_TOKEN;
    }
    if (!hfoc_lwip_tcp_close_policy_reason_valid(reason) ||
        reason == HFOC_LWIP_TCP_CLOSE_POLICY_REASON_NONE) {
        return HFOC_LWIP_TCP_CLOSE_POLICY_INVALID_REASON;
    }

    entry = &policy->entries[token->index];
    if (!entry->active) {
        return HFOC_LWIP_TCP_CLOSE_POLICY_NOT_ATTACHED;
    }
    if (entry->generation != token->generation) {
        return HFOC_LWIP_TCP_CLOSE_POLICY_STALE_TOKEN;
    }
    if (pool != entry->pool) {
        return HFOC_LWIP_TCP_CLOSE_POLICY_INVALID_POOL;
    }
    if (quarantine != entry->quarantine) {
        return HFOC_LWIP_TCP_CLOSE_POLICY_INVALID_QUARANTINE;
    }

    connection = &pool->slots[token->index].connection;
    if (!connection->published ||
        connection->version != HFOC_LWIP_TCP_CONN_VERSION ||
        connection->generation != token->generation ||
        connection != entry->connection) {
        hfoc_lwip_tcp_close_policy_record(
            policy, entry, reason,
            hfoc_lwip_tcp_close_policy_reason_action(reason),
            HFOC_LWIP_TCP_CLOSE_POLICY_INVALID_CONNECTION,
            token->index, token->generation);
        ++policy->rejected_count;
        ++entry->rejected_count;
        return HFOC_LWIP_TCP_CLOSE_POLICY_INVALID_CONNECTION;
    }
    if (connection->state == HFOC_LWIP_TCP_CONN_STATE_CLOSED ||
        connection->state == HFOC_LWIP_TCP_CONN_STATE_ABORTED) {
        hfoc_lwip_tcp_close_policy_record(
            policy, entry, reason,
            hfoc_lwip_tcp_close_policy_reason_action(reason),
            HFOC_LWIP_TCP_CLOSE_POLICY_ALREADY_CLOSED,
            token->index, token->generation);
        return HFOC_LWIP_TCP_CLOSE_POLICY_ALREADY_CLOSED;
    }
    if (connection->state != HFOC_LWIP_TCP_CONN_STATE_ESTABLISHED &&
        connection->state != HFOC_LWIP_TCP_CONN_STATE_CLOSING) {
        hfoc_lwip_tcp_close_policy_record(
            policy, entry, reason,
            hfoc_lwip_tcp_close_policy_reason_action(reason),
            HFOC_LWIP_TCP_CLOSE_POLICY_INVALID_CONNECTION,
            token->index, token->generation);
        ++policy->rejected_count;
        ++entry->rejected_count;
        return HFOC_LWIP_TCP_CLOSE_POLICY_INVALID_CONNECTION;
    }

    pcb = connection->pcb;
    if (pcb == NULL || pcb != entry->pcb) {
        hfoc_lwip_tcp_close_policy_record(
            policy, entry, reason,
            hfoc_lwip_tcp_close_policy_reason_action(reason),
            HFOC_LWIP_TCP_CLOSE_POLICY_INVALID_PCB,
            token->index, token->generation);
        ++policy->rejected_count;
        ++entry->rejected_count;
        return HFOC_LWIP_TCP_CLOSE_POLICY_INVALID_PCB;
    }

    action = hfoc_lwip_tcp_close_policy_reason_action(reason);
    kind = hfoc_lwip_tcp_close_policy_quarantine_kind(action);
    quarantine_status = hfoc_lwip_tcp_quarantine_check(
        entry->quarantine, token, connection,
        kind == HFOC_LWIP_TCP_QUARANTINE_KIND_ABORT ? NULL : pcb,
        connection->state, kind);
    if (quarantine_status != HFOC_LWIP_TCP_QUARANTINE_OK) {
        hfoc_lwip_tcp_close_policy_record(
            policy, entry, reason, action,
            HFOC_LWIP_TCP_CLOSE_POLICY_INVALID_QUARANTINE,
            token->index, token->generation);
        ++policy->rejected_count;
        ++entry->rejected_count;
        return HFOC_LWIP_TCP_CLOSE_POLICY_INVALID_QUARANTINE;
    }

    if (action == HFOC_LWIP_TCP_CLOSE_POLICY_ACTION_CLOSE) {
        pool_status = hfoc_lwip_tcp_pool_close(pool, token);
        if (pool_status != HFOC_LWIP_TCP_POOL_OK) {
            hfoc_lwip_tcp_close_policy_record(
                policy, entry, reason, action,
                HFOC_LWIP_TCP_CLOSE_POLICY_CLOSE_FAILED,
                token->index, token->generation);
            ++policy->rejected_count;
            ++entry->rejected_count;
            return HFOC_LWIP_TCP_CLOSE_POLICY_CLOSE_FAILED;
        }
        ++entry->handled_count;
        ++entry->close_count;
        ++entry->reason_counts[(unsigned int)reason];
        ++policy->handled_count;
        ++policy->close_count;
        hfoc_lwip_tcp_close_policy_record(
            policy, entry, reason, action,
            HFOC_LWIP_TCP_CLOSE_POLICY_CLOSED,
            token->index, token->generation);
        return HFOC_LWIP_TCP_CLOSE_POLICY_CLOSED;
    }

    pool_status = hfoc_lwip_tcp_pool_abort(pool, token);
    if (pool_status != HFOC_LWIP_TCP_POOL_OK) {
        hfoc_lwip_tcp_close_policy_record(
            policy, entry, reason, action,
            HFOC_LWIP_TCP_CLOSE_POLICY_ABORT_FAILED,
            token->index, token->generation);
        ++policy->rejected_count;
        ++entry->rejected_count;
        return HFOC_LWIP_TCP_CLOSE_POLICY_ABORT_FAILED;
    }
    ++entry->handled_count;
    ++entry->abort_count;
    ++entry->reason_counts[(unsigned int)reason];
    ++policy->handled_count;
    ++policy->abort_count;
    hfoc_lwip_tcp_close_policy_record(
        policy, entry, reason, action,
        HFOC_LWIP_TCP_CLOSE_POLICY_ABORTED,
        token->index, token->generation);
    return HFOC_LWIP_TCP_CLOSE_POLICY_ABORTED;
}

enum hfoc_lwip_tcp_close_policy_status
hfoc_lwip_tcp_close_policy_detach(
    struct hfoc_lwip_tcp_close_policy *policy,
    struct hfoc_lwip_tcp_pool *pool,
    struct hfoc_lwip_tcp_quarantine *quarantine,
    const struct hfoc_lwip_tcp_pool_token *token)
{
    struct hfoc_lwip_tcp_close_policy_entry *entry;
    struct hfoc_lwip_tcp_connection *connection;

    if (policy == NULL || pool == NULL || quarantine == NULL ||
        token == NULL) {
        return HFOC_LWIP_TCP_CLOSE_POLICY_NULL;
    }
    if (!hfoc_lwip_tcp_close_policy_valid(policy)) {
        return HFOC_LWIP_TCP_CLOSE_POLICY_BAD_STATE;
    }
    if (token->index >= HFOC_LWIP_TCP_CLOSE_POLICY_SLOT_COUNT ||
        !hfoc_lwip_tcp_close_policy_generation_valid(
            token->generation)) {
        return HFOC_LWIP_TCP_CLOSE_POLICY_BAD_TOKEN;
    }
    entry = &policy->entries[token->index];
    if (!entry->active) {
        return HFOC_LWIP_TCP_CLOSE_POLICY_NOT_ATTACHED;
    }
    if (entry->generation != token->generation) {
        return HFOC_LWIP_TCP_CLOSE_POLICY_STALE_TOKEN;
    }
    if (pool != entry->pool) {
        return HFOC_LWIP_TCP_CLOSE_POLICY_INVALID_POOL;
    }
    if (quarantine != entry->quarantine) {
        return HFOC_LWIP_TCP_CLOSE_POLICY_INVALID_QUARANTINE;
    }

    connection = &pool->slots[token->index].connection;
    if (!connection->published ||
        connection->version != HFOC_LWIP_TCP_CONN_VERSION ||
        connection->generation != token->generation ||
        connection != entry->connection) {
        return HFOC_LWIP_TCP_CLOSE_POLICY_INVALID_CONNECTION;
    }
    if (connection->state != HFOC_LWIP_TCP_CONN_STATE_CLOSED &&
        connection->state != HFOC_LWIP_TCP_CONN_STATE_ABORTED) {
        return HFOC_LWIP_TCP_CLOSE_POLICY_LIVE_CONNECTION;
    }

    memset(entry, 0, sizeof(*entry));
    entry->last_status = HFOC_LWIP_TCP_CLOSE_POLICY_OK;
    --policy->active_count;
    return HFOC_LWIP_TCP_CLOSE_POLICY_OK;
}

bool hfoc_lwip_tcp_close_policy_published(
    const struct hfoc_lwip_tcp_close_policy *policy)
{
    return hfoc_lwip_tcp_close_policy_valid(policy);
}

uint32_t hfoc_lwip_tcp_close_policy_active_count(
    const struct hfoc_lwip_tcp_close_policy *policy)
{
    return hfoc_lwip_tcp_close_policy_valid(policy)
        ? policy->active_count
        : 0U;
}

uint32_t hfoc_lwip_tcp_close_policy_handled_count(
    const struct hfoc_lwip_tcp_close_policy *policy)
{
    return hfoc_lwip_tcp_close_policy_valid(policy)
        ? policy->handled_count
        : 0U;
}

uint32_t hfoc_lwip_tcp_close_policy_close_count(
    const struct hfoc_lwip_tcp_close_policy *policy)
{
    return hfoc_lwip_tcp_close_policy_valid(policy)
        ? policy->close_count
        : 0U;
}

uint32_t hfoc_lwip_tcp_close_policy_abort_count(
    const struct hfoc_lwip_tcp_close_policy *policy)
{
    return hfoc_lwip_tcp_close_policy_valid(policy)
        ? policy->abort_count
        : 0U;
}

uint32_t hfoc_lwip_tcp_close_policy_rejected_count(
    const struct hfoc_lwip_tcp_close_policy *policy)
{
    return hfoc_lwip_tcp_close_policy_valid(policy)
        ? policy->rejected_count
        : 0U;
}

uint32_t hfoc_lwip_tcp_close_policy_entry_handled_count(
    const struct hfoc_lwip_tcp_close_policy *policy,
    const struct hfoc_lwip_tcp_pool_token *token)
{
    const struct hfoc_lwip_tcp_close_policy_entry *entry;

    return hfoc_lwip_tcp_close_policy_lookup(policy, token, &entry)
        ? entry->handled_count
        : 0U;
}

uint32_t hfoc_lwip_tcp_close_policy_entry_close_count(
    const struct hfoc_lwip_tcp_close_policy *policy,
    const struct hfoc_lwip_tcp_pool_token *token)
{
    const struct hfoc_lwip_tcp_close_policy_entry *entry;

    return hfoc_lwip_tcp_close_policy_lookup(policy, token, &entry)
        ? entry->close_count
        : 0U;
}

uint32_t hfoc_lwip_tcp_close_policy_entry_abort_count(
    const struct hfoc_lwip_tcp_close_policy *policy,
    const struct hfoc_lwip_tcp_pool_token *token)
{
    const struct hfoc_lwip_tcp_close_policy_entry *entry;

    return hfoc_lwip_tcp_close_policy_lookup(policy, token, &entry)
        ? entry->abort_count
        : 0U;
}

uint32_t hfoc_lwip_tcp_close_policy_entry_rejected_count(
    const struct hfoc_lwip_tcp_close_policy *policy,
    const struct hfoc_lwip_tcp_pool_token *token)
{
    const struct hfoc_lwip_tcp_close_policy_entry *entry;

    return hfoc_lwip_tcp_close_policy_lookup(policy, token, &entry)
        ? entry->rejected_count
        : 0U;
}

uint32_t hfoc_lwip_tcp_close_policy_reason_count(
    const struct hfoc_lwip_tcp_close_policy *policy,
    const struct hfoc_lwip_tcp_pool_token *token,
    enum hfoc_lwip_tcp_close_policy_reason reason)
{
    const struct hfoc_lwip_tcp_close_policy_entry *entry;

    if (!hfoc_lwip_tcp_close_policy_reason_valid(reason)) {
        return 0U;
    }
    return hfoc_lwip_tcp_close_policy_lookup(policy, token, &entry)
        ? entry->reason_counts[(unsigned int)reason]
        : 0U;
}

enum hfoc_lwip_tcp_close_policy_reason
hfoc_lwip_tcp_close_policy_last_reason(
    const struct hfoc_lwip_tcp_close_policy *policy)
{
    return hfoc_lwip_tcp_close_policy_valid(policy)
        ? policy->last_reason
        : HFOC_LWIP_TCP_CLOSE_POLICY_REASON_NONE;
}

enum hfoc_lwip_tcp_close_policy_action
hfoc_lwip_tcp_close_policy_last_action(
    const struct hfoc_lwip_tcp_close_policy *policy)
{
    return hfoc_lwip_tcp_close_policy_valid(policy)
        ? policy->last_action
        : HFOC_LWIP_TCP_CLOSE_POLICY_ACTION_NONE;
}

enum hfoc_lwip_tcp_close_policy_status
hfoc_lwip_tcp_close_policy_last_status(
    const struct hfoc_lwip_tcp_close_policy *policy)
{
    return hfoc_lwip_tcp_close_policy_valid(policy)
        ? policy->last_status
        : HFOC_LWIP_TCP_CLOSE_POLICY_OK;
}

uint32_t hfoc_lwip_tcp_close_policy_last_index(
    const struct hfoc_lwip_tcp_close_policy *policy)
{
    return hfoc_lwip_tcp_close_policy_valid(policy)
        ? policy->last_index
        : 0U;
}

uint32_t hfoc_lwip_tcp_close_policy_last_generation(
    const struct hfoc_lwip_tcp_close_policy *policy)
{
    return hfoc_lwip_tcp_close_policy_valid(policy)
        ? policy->last_generation
        : 0U;
}

enum hfoc_lwip_tcp_close_policy_action
hfoc_lwip_tcp_close_policy_action_for_reason(
    enum hfoc_lwip_tcp_close_policy_reason reason)
{
    return hfoc_lwip_tcp_close_policy_reason_action(reason);
}

const char *hfoc_lwip_tcp_close_policy_reason_string(
    enum hfoc_lwip_tcp_close_policy_reason reason)
{
    switch (reason) {
    case HFOC_LWIP_TCP_CLOSE_POLICY_REASON_NONE:
        return "HFOC_LWIP_TCP_CLOSE_POLICY_REASON_NONE";
    case HFOC_LWIP_TCP_CLOSE_POLICY_REASON_GRACEFUL_FIN:
        return "HFOC_LWIP_TCP_CLOSE_POLICY_REASON_GRACEFUL_FIN";
    case HFOC_LWIP_TCP_CLOSE_POLICY_REASON_PROTOCOL_CLOSE:
        return "HFOC_LWIP_TCP_CLOSE_POLICY_REASON_PROTOCOL_CLOSE";
    case HFOC_LWIP_TCP_CLOSE_POLICY_REASON_TIMEOUT_ABORT:
        return "HFOC_LWIP_TCP_CLOSE_POLICY_REASON_TIMEOUT_ABORT";
    case HFOC_LWIP_TCP_CLOSE_POLICY_REASON_LINK_RESET:
        return "HFOC_LWIP_TCP_CLOSE_POLICY_REASON_LINK_RESET";
    case HFOC_LWIP_TCP_CLOSE_POLICY_REASON_SERVICE_SHUTDOWN:
        return "HFOC_LWIP_TCP_CLOSE_POLICY_REASON_SERVICE_SHUTDOWN";
    default:
        return NULL;
    }
}

const char *hfoc_lwip_tcp_close_policy_action_string(
    enum hfoc_lwip_tcp_close_policy_action action)
{
    switch (action) {
    case HFOC_LWIP_TCP_CLOSE_POLICY_ACTION_NONE:
        return "HFOC_LWIP_TCP_CLOSE_POLICY_ACTION_NONE";
    case HFOC_LWIP_TCP_CLOSE_POLICY_ACTION_CLOSE:
        return "HFOC_LWIP_TCP_CLOSE_POLICY_ACTION_CLOSE";
    case HFOC_LWIP_TCP_CLOSE_POLICY_ACTION_ABORT:
        return "HFOC_LWIP_TCP_CLOSE_POLICY_ACTION_ABORT";
    default:
        return NULL;
    }
}

const char *hfoc_lwip_tcp_close_policy_status_string(
    enum hfoc_lwip_tcp_close_policy_status status)
{
    switch (status) {
    case HFOC_LWIP_TCP_CLOSE_POLICY_OK:
        return "HFOC_LWIP_TCP_CLOSE_POLICY_OK";
    case HFOC_LWIP_TCP_CLOSE_POLICY_NULL:
        return "HFOC_LWIP_TCP_CLOSE_POLICY_NULL";
    case HFOC_LWIP_TCP_CLOSE_POLICY_BAD_STATE:
        return "HFOC_LWIP_TCP_CLOSE_POLICY_BAD_STATE";
    case HFOC_LWIP_TCP_CLOSE_POLICY_BAD_TOKEN:
        return "HFOC_LWIP_TCP_CLOSE_POLICY_BAD_TOKEN";
    case HFOC_LWIP_TCP_CLOSE_POLICY_NOT_ATTACHED:
        return "HFOC_LWIP_TCP_CLOSE_POLICY_NOT_ATTACHED";
    case HFOC_LWIP_TCP_CLOSE_POLICY_STALE_TOKEN:
        return "HFOC_LWIP_TCP_CLOSE_POLICY_STALE_TOKEN";
    case HFOC_LWIP_TCP_CLOSE_POLICY_INVALID_POOL:
        return "HFOC_LWIP_TCP_CLOSE_POLICY_INVALID_POOL";
    case HFOC_LWIP_TCP_CLOSE_POLICY_INVALID_QUARANTINE:
        return "HFOC_LWIP_TCP_CLOSE_POLICY_INVALID_QUARANTINE";
    case HFOC_LWIP_TCP_CLOSE_POLICY_INVALID_CONNECTION:
        return "HFOC_LWIP_TCP_CLOSE_POLICY_INVALID_CONNECTION";
    case HFOC_LWIP_TCP_CLOSE_POLICY_INVALID_PCB:
        return "HFOC_LWIP_TCP_CLOSE_POLICY_INVALID_PCB";
    case HFOC_LWIP_TCP_CLOSE_POLICY_INVALID_REASON:
        return "HFOC_LWIP_TCP_CLOSE_POLICY_INVALID_REASON";
    case HFOC_LWIP_TCP_CLOSE_POLICY_ALREADY_ATTACHED:
        return "HFOC_LWIP_TCP_CLOSE_POLICY_ALREADY_ATTACHED";
    case HFOC_LWIP_TCP_CLOSE_POLICY_LIVE_CONNECTION:
        return "HFOC_LWIP_TCP_CLOSE_POLICY_LIVE_CONNECTION";
    case HFOC_LWIP_TCP_CLOSE_POLICY_CLOSED:
        return "HFOC_LWIP_TCP_CLOSE_POLICY_CLOSED";
    case HFOC_LWIP_TCP_CLOSE_POLICY_ABORTED:
        return "HFOC_LWIP_TCP_CLOSE_POLICY_ABORTED";
    case HFOC_LWIP_TCP_CLOSE_POLICY_ALREADY_CLOSED:
        return "HFOC_LWIP_TCP_CLOSE_POLICY_ALREADY_CLOSED";
    case HFOC_LWIP_TCP_CLOSE_POLICY_CLOSE_FAILED:
        return "HFOC_LWIP_TCP_CLOSE_POLICY_CLOSE_FAILED";
    case HFOC_LWIP_TCP_CLOSE_POLICY_ABORT_FAILED:
        return "HFOC_LWIP_TCP_CLOSE_POLICY_ABORT_FAILED";
    default:
        return NULL;
    }
}

#endif
