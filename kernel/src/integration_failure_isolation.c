#include "integration_failure_isolation.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

static enum integration_failure_isolation_status
integration_failure_isolation_validate(
    const struct integration_failure_isolation_table *table)
{
    uint32_t index;

    if (table->capacity == 0U ||
        table->capacity > INTEGRATION_FAILURE_ISOLATION_MAX_PEERS) {
        return INTEGRATION_FAILURE_ISOLATION_BAD_CAPACITY;
    }
    if (table->version != INTEGRATION_FAILURE_ISOLATION_VERSION ||
        table->count > table->capacity) {
        return INTEGRATION_FAILURE_ISOLATION_BAD_STATE;
    }

    for (index = 0U; index < table->count; ++index) {
        const struct integration_failure_peer *const peer =
            &table->peers[index];

        if (peer->peer_id < INTEGRATION_FAILURE_ISOLATION_PEER_ID_MIN ||
            peer->peer_id > INTEGRATION_FAILURE_ISOLATION_PEER_ID_MAX ||
            peer->consecutive_failures >
                INTEGRATION_FAILURE_ISOLATION_THRESHOLD ||
            peer->state > INTEGRATION_PEER_STATE_ISOLATED ||
            (peer->state == INTEGRATION_PEER_STATE_HEALTHY &&
             peer->consecutive_failures != 0U) ||
            (peer->state != INTEGRATION_PEER_STATE_HEALTHY &&
             peer->consecutive_failures == 0U) ||
            (peer->state != INTEGRATION_PEER_STATE_ISOLATED &&
             peer->isolated_until_tick != 0U) ||
            (peer->state == INTEGRATION_PEER_STATE_ISOLATED &&
             peer->isolated_until_tick == 0U)) {
            return INTEGRATION_FAILURE_ISOLATION_BAD_STATE;
        }
    }

    for (index = table->count;
         index < INTEGRATION_FAILURE_ISOLATION_MAX_PEERS; ++index) {
        const struct integration_failure_peer *const peer =
            &table->peers[index];

        if (peer->peer_id != 0U ||
            peer->consecutive_failures != 0U ||
            peer->state != INTEGRATION_PEER_STATE_HEALTHY ||
            peer->last_failure_tick != 0U ||
            peer->isolated_until_tick != 0U) {
            return INTEGRATION_FAILURE_ISOLATION_BAD_STATE;
        }
    }
    return INTEGRATION_FAILURE_ISOLATION_OK;
}

static struct integration_failure_peer *
integration_failure_isolation_find(
    struct integration_failure_isolation_table *table,
    uint32_t peer_id)
{
    uint32_t index;

    for (index = 0U; index < table->count; ++index) {
        if (table->peers[index].peer_id == peer_id) {
            return &table->peers[index];
        }
    }
    return NULL;
}

static const struct integration_failure_peer *
integration_failure_isolation_find_const(
    const struct integration_failure_isolation_table *table,
    uint32_t peer_id)
{
    uint32_t index;

    for (index = 0U; index < table->count; ++index) {
        if (table->peers[index].peer_id == peer_id) {
            return &table->peers[index];
        }
    }
    return NULL;
}

static uint32_t integration_failure_isolation_cooldown(
    enum integration_failure_kind kind)
{
    switch (kind) {
    case INTEGRATION_FAILURE_MALFORMED:
        return INTEGRATION_FAILURE_ISOLATION_COOLDOWN_MALFORMED;
    case INTEGRATION_FAILURE_UNAVAILABLE:
        return INTEGRATION_FAILURE_ISOLATION_COOLDOWN_UNAVAILABLE;
    case INTEGRATION_FAILURE_SLOW:
        return INTEGRATION_FAILURE_ISOLATION_COOLDOWN_SLOW;
    default:
        return 0U;
    }
}

enum integration_failure_isolation_status integration_failure_isolation_init(
    struct integration_failure_isolation_table *table,
    uint32_t capacity)
{
    if (table == NULL) {
        return INTEGRATION_FAILURE_ISOLATION_NULL;
    }
    if (capacity == 0U ||
        capacity > INTEGRATION_FAILURE_ISOLATION_MAX_PEERS) {
        return INTEGRATION_FAILURE_ISOLATION_BAD_CAPACITY;
    }

    memset(table, 0, sizeof(*table));
    table->version = INTEGRATION_FAILURE_ISOLATION_VERSION;
    table->capacity = capacity;
    return INTEGRATION_FAILURE_ISOLATION_OK;
}

enum integration_failure_isolation_status integration_failure_isolation_record(
    struct integration_failure_isolation_table *table,
    uint32_t peer_id,
    enum integration_failure_kind kind,
    uint64_t current_tick)
{
    enum integration_failure_isolation_status status;
    struct integration_failure_peer *peer;
    uint32_t cooldown;

    if (table == NULL) {
        return INTEGRATION_FAILURE_ISOLATION_NULL;
    }
    status = integration_failure_isolation_validate(table);
    if (status != INTEGRATION_FAILURE_ISOLATION_OK) {
        return status;
    }
    if (peer_id < INTEGRATION_FAILURE_ISOLATION_PEER_ID_MIN ||
        peer_id > INTEGRATION_FAILURE_ISOLATION_PEER_ID_MAX) {
        return INTEGRATION_FAILURE_ISOLATION_INVALID_PEER_ID;
    }
    cooldown = integration_failure_isolation_cooldown(kind);
    if (cooldown == 0U) {
        return INTEGRATION_FAILURE_ISOLATION_INVALID_FAULT_KIND;
    }
    if (current_tick > UINT64_MAX - cooldown) {
        return INTEGRATION_FAILURE_ISOLATION_BAD_STATE;
    }

    peer = integration_failure_isolation_find(table, peer_id);
    if (peer == NULL) {
        if (table->count >= table->capacity) {
            return INTEGRATION_FAILURE_ISOLATION_TABLE_FULL;
        }
        peer = &table->peers[table->count];
        memset(peer, 0, sizeof(*peer));
        peer->peer_id = peer_id;
        peer->consecutive_failures = 1U;
        peer->state = INTEGRATION_PEER_STATE_DEGRADED;
        peer->last_failure_tick = current_tick;
        ++table->count;
        return INTEGRATION_FAILURE_ISOLATION_OK;
    }

    if (peer->state == INTEGRATION_PEER_STATE_ISOLATED &&
        current_tick < peer->isolated_until_tick) {
        return INTEGRATION_FAILURE_ISOLATION_PEER_ISOLATED;
    }
    if (peer->consecutive_failures <
        INTEGRATION_FAILURE_ISOLATION_THRESHOLD) {
        ++peer->consecutive_failures;
    }
    peer->last_failure_tick = current_tick;
    if (peer->consecutive_failures >=
        INTEGRATION_FAILURE_ISOLATION_THRESHOLD) {
        peer->state = INTEGRATION_PEER_STATE_ISOLATED;
        peer->isolated_until_tick = current_tick + cooldown;
        return INTEGRATION_FAILURE_ISOLATION_PEER_ISOLATED;
    }
    peer->state = INTEGRATION_PEER_STATE_DEGRADED;
    return INTEGRATION_FAILURE_ISOLATION_OK;
}

enum integration_failure_isolation_status integration_failure_isolation_check(
    const struct integration_failure_isolation_table *table,
    uint32_t peer_id,
    uint64_t current_tick)
{
    const struct integration_failure_peer *peer;
    enum integration_failure_isolation_status status;

    if (table == NULL) {
        return INTEGRATION_FAILURE_ISOLATION_NULL;
    }
    status = integration_failure_isolation_validate(table);
    if (status != INTEGRATION_FAILURE_ISOLATION_OK) {
        return status;
    }
    if (peer_id < INTEGRATION_FAILURE_ISOLATION_PEER_ID_MIN ||
        peer_id > INTEGRATION_FAILURE_ISOLATION_PEER_ID_MAX) {
        return INTEGRATION_FAILURE_ISOLATION_INVALID_PEER_ID;
    }

    peer = integration_failure_isolation_find_const(table, peer_id);
    if (peer == NULL) {
        return INTEGRATION_FAILURE_ISOLATION_OK;
    }
    if (peer->state == INTEGRATION_PEER_STATE_ISOLATED &&
        current_tick < peer->isolated_until_tick) {
        return INTEGRATION_FAILURE_ISOLATION_PEER_ISOLATED;
    }
    return INTEGRATION_FAILURE_ISOLATION_OK;
}

enum integration_failure_isolation_status integration_failure_isolation_recover(
    struct integration_failure_isolation_table *table,
    uint32_t peer_id)
{
    enum integration_failure_isolation_status status;
    uint32_t index;

    if (table == NULL) {
        return INTEGRATION_FAILURE_ISOLATION_NULL;
    }
    status = integration_failure_isolation_validate(table);
    if (status != INTEGRATION_FAILURE_ISOLATION_OK) {
        return status;
    }
    if (peer_id < INTEGRATION_FAILURE_ISOLATION_PEER_ID_MIN ||
        peer_id > INTEGRATION_FAILURE_ISOLATION_PEER_ID_MAX) {
        return INTEGRATION_FAILURE_ISOLATION_INVALID_PEER_ID;
    }

    for (index = 0U; index < table->count; ++index) {
        if (table->peers[index].peer_id == peer_id) {
            if (index + 1U < table->count) {
                memmove(&table->peers[index],
                        &table->peers[index + 1U],
                        sizeof(table->peers[0]) *
                            (table->count - index - 1U));
            }
            memset(&table->peers[table->count - 1U],
                   0,
                   sizeof(table->peers[0]));
            --table->count;
            return INTEGRATION_FAILURE_ISOLATION_OK;
        }
    }
    return INTEGRATION_FAILURE_ISOLATION_PEER_NOT_FOUND;
}

enum integration_failure_isolation_status
integration_failure_isolation_local_allowed(
    enum integration_local_operation operation)
{
    switch (operation) {
    case INTEGRATION_LOCAL_MANAGEMENT:
    case INTEGRATION_LOCAL_SERVICE_SUPERVISION:
    case INTEGRATION_LOCAL_STATE_RECOVERY:
        return INTEGRATION_FAILURE_ISOLATION_OK;
    default:
        return INTEGRATION_FAILURE_ISOLATION_INVALID_LOCAL_OPERATION;
    }
}

const char *integration_failure_isolation_status_string(
    enum integration_failure_isolation_status status)
{
    switch (status) {
    case INTEGRATION_FAILURE_ISOLATION_OK:
        return "INTEGRATION_FAILURE_ISOLATION_OK";
    case INTEGRATION_FAILURE_ISOLATION_NULL:
        return "INTEGRATION_FAILURE_ISOLATION_NULL";
    case INTEGRATION_FAILURE_ISOLATION_BAD_CAPACITY:
        return "INTEGRATION_FAILURE_ISOLATION_BAD_CAPACITY";
    case INTEGRATION_FAILURE_ISOLATION_BAD_STATE:
        return "INTEGRATION_FAILURE_ISOLATION_BAD_STATE";
    case INTEGRATION_FAILURE_ISOLATION_INVALID_PEER_ID:
        return "INTEGRATION_FAILURE_ISOLATION_INVALID_PEER_ID";
    case INTEGRATION_FAILURE_ISOLATION_INVALID_FAULT_KIND:
        return "INTEGRATION_FAILURE_ISOLATION_INVALID_FAULT_KIND";
    case INTEGRATION_FAILURE_ISOLATION_INVALID_LOCAL_OPERATION:
        return "INTEGRATION_FAILURE_ISOLATION_INVALID_LOCAL_OPERATION";
    case INTEGRATION_FAILURE_ISOLATION_TABLE_FULL:
        return "INTEGRATION_FAILURE_ISOLATION_TABLE_FULL";
    case INTEGRATION_FAILURE_ISOLATION_PEER_ISOLATED:
        return "INTEGRATION_FAILURE_ISOLATION_PEER_ISOLATED";
    case INTEGRATION_FAILURE_ISOLATION_PEER_NOT_FOUND:
        return "INTEGRATION_FAILURE_ISOLATION_PEER_NOT_FOUND";
    default:
        return NULL;
    }
}
