#include "local_session.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

static enum local_session_status local_session_table_valid(
    const struct local_session_table *table)
{
    if (table == NULL) {
        return LOCAL_SESSION_NULL;
    }
    if (table->capacity == 0U ||
        table->capacity > LOCAL_SESSION_TABLE_MAX) {
        return LOCAL_SESSION_BAD_CAPACITY;
    }
    return LOCAL_SESSION_OK;
}

static int local_session_find_index(
    const struct local_session_table *table,
    uint32_t session_id)
{
    uint32_t index;

    for (index = 0U; index < table->capacity; ++index) {
        if (table->sessions[index].id == session_id) {
            return (int)index;
        }
    }
    return -1;
}

enum local_session_status local_session_table_init(
    struct local_session_table *table,
    uint32_t capacity)
{
    uint32_t index;

    if (table == NULL) {
        return LOCAL_SESSION_NULL;
    }
    if (capacity == 0U || capacity > LOCAL_SESSION_TABLE_MAX) {
        return LOCAL_SESSION_BAD_CAPACITY;
    }

    memset(table, 0, sizeof(*table));
    table->capacity = capacity;
    table->next_id = LOCAL_SESSION_ID_MIN;
    for (index = 0U; index < LOCAL_SESSION_TABLE_MAX; ++index) {
        table->sessions[index].version = LOCAL_SESSION_VERSION;
        table->sessions[index].state = LOCAL_SESSION_STATE_CLOSED;
    }
    return LOCAL_SESSION_OK;
}

enum local_session_status local_session_open(
    struct local_session_table *table,
    uint32_t principal_id,
    uint32_t generation,
    uint64_t now_tick,
    uint32_t lifetime_ticks,
    uint32_t *out_session_id)
{
    enum local_session_status status;
    struct local_session *session = NULL;
    uint32_t index;

    if (table == NULL || out_session_id == NULL) {
        return LOCAL_SESSION_NULL;
    }
    status = local_session_table_valid(table);
    if (status != LOCAL_SESSION_OK) {
        return status;
    }
    if (principal_id < LOCAL_SESSION_PRINCIPAL_ID_MIN ||
        principal_id > LOCAL_SESSION_PRINCIPAL_ID_MAX) {
        return LOCAL_SESSION_INVALID_PRINCIPAL_ID;
    }
    if (generation < LOCAL_SESSION_GENERATION_MIN) {
        return LOCAL_SESSION_INVALID_GENERATION;
    }
    if (lifetime_ticks == 0U ||
        lifetime_ticks > LOCAL_SESSION_LIFETIME_MAX) {
        return LOCAL_SESSION_INVALID_LIFETIME;
    }
    if (now_tick > UINT64_MAX - (uint64_t)lifetime_ticks) {
        return LOCAL_SESSION_INVALID_TICK;
    }

    for (index = 0U; index < table->capacity; ++index) {
        if (table->sessions[index].state == LOCAL_SESSION_STATE_CLOSED) {
            session = &table->sessions[index];
            break;
        }
    }
    if (session == NULL) {
        return LOCAL_SESSION_TABLE_FULL;
    }
    if (table->next_id < LOCAL_SESSION_ID_MIN ||
        table->next_id > LOCAL_SESSION_ID_MAX) {
        return LOCAL_SESSION_ID_EXHAUSTED;
    }

    session->version = LOCAL_SESSION_VERSION;
    session->id = table->next_id;
    session->principal_id = principal_id;
    session->generation = generation;
    session->opened_tick = now_tick;
    session->expires_tick = now_tick + (uint64_t)lifetime_ticks;
    session->state = LOCAL_SESSION_STATE_OPEN;
    ++table->next_id;
    *out_session_id = session->id;
    return LOCAL_SESSION_OK;
}

enum local_session_status local_session_close(
    struct local_session_table *table,
    uint32_t session_id)
{
    enum local_session_status status;
    int index;

    if (table == NULL) {
        return LOCAL_SESSION_NULL;
    }
    status = local_session_table_valid(table);
    if (status != LOCAL_SESSION_OK) {
        return status;
    }
    if (session_id < LOCAL_SESSION_ID_MIN ||
        session_id > LOCAL_SESSION_ID_MAX) {
        return LOCAL_SESSION_INVALID_SESSION_ID;
    }
    index = local_session_find_index(table, session_id);
    if (index < 0) {
        return LOCAL_SESSION_INVALID_SESSION_ID;
    }
    if (table->sessions[index].state == LOCAL_SESSION_STATE_CLOSED) {
        return LOCAL_SESSION_OK;
    }
    if (table->sessions[index].state != LOCAL_SESSION_STATE_OPEN) {
        return LOCAL_SESSION_INVALID_STATE;
    }
    table->sessions[index].state = LOCAL_SESSION_STATE_CLOSED;
    return LOCAL_SESSION_OK;
}

enum local_session_status local_session_validate(
    const struct local_session_table *table,
    uint32_t session_id,
    uint32_t principal_id,
    uint32_t generation,
    uint64_t now_tick)
{
    enum local_session_status status;
    int index;

    if (table == NULL) {
        return LOCAL_SESSION_NULL;
    }
    status = local_session_table_valid(table);
    if (status != LOCAL_SESSION_OK) {
        return status;
    }
    if (session_id < LOCAL_SESSION_ID_MIN ||
        session_id > LOCAL_SESSION_ID_MAX) {
        return LOCAL_SESSION_INVALID_SESSION_ID;
    }
    if (principal_id < LOCAL_SESSION_PRINCIPAL_ID_MIN ||
        principal_id > LOCAL_SESSION_PRINCIPAL_ID_MAX) {
        return LOCAL_SESSION_INVALID_PRINCIPAL_ID;
    }
    if (generation < LOCAL_SESSION_GENERATION_MIN) {
        return LOCAL_SESSION_INVALID_GENERATION;
    }

    index = local_session_find_index(table, session_id);
    if (index < 0) {
        return LOCAL_SESSION_INVALID_SESSION_ID;
    }
    if (table->sessions[index].version != LOCAL_SESSION_VERSION) {
        return LOCAL_SESSION_INVALID_VERSION;
    }
    if (table->sessions[index].generation != generation) {
        return LOCAL_SESSION_STALE_GENERATION;
    }
    if (table->sessions[index].state == LOCAL_SESSION_STATE_CLOSED) {
        return LOCAL_SESSION_CLOSED;
    }
    if (table->sessions[index].state != LOCAL_SESSION_STATE_OPEN) {
        return LOCAL_SESSION_INVALID_STATE;
    }
    if (now_tick >= table->sessions[index].expires_tick) {
        return LOCAL_SESSION_EXPIRED;
    }
    if (table->sessions[index].principal_id != principal_id) {
        return LOCAL_SESSION_NOT_OWNER;
    }
    return LOCAL_SESSION_OK;
}

enum local_session_status local_session_invalidate_generation(
    struct local_session_table *table,
    uint32_t principal_id,
    uint32_t generation)
{
    enum local_session_status status;
    uint32_t index;

    if (table == NULL) {
        return LOCAL_SESSION_NULL;
    }
    status = local_session_table_valid(table);
    if (status != LOCAL_SESSION_OK) {
        return status;
    }
    if (principal_id < LOCAL_SESSION_PRINCIPAL_ID_MIN ||
        principal_id > LOCAL_SESSION_PRINCIPAL_ID_MAX) {
        return LOCAL_SESSION_INVALID_PRINCIPAL_ID;
    }
    if (generation < LOCAL_SESSION_GENERATION_MIN) {
        return LOCAL_SESSION_INVALID_GENERATION;
    }

    for (index = 0U; index < table->capacity; ++index) {
        if (table->sessions[index].state == LOCAL_SESSION_STATE_OPEN &&
            table->sessions[index].principal_id == principal_id &&
            table->sessions[index].generation < generation) {
            table->sessions[index].state = LOCAL_SESSION_STATE_CLOSED;
        }
    }
    return LOCAL_SESSION_OK;
}

const char *local_session_status_string(
    enum local_session_status status)
{
    switch (status) {
    case LOCAL_SESSION_OK:
        return "LOCAL_SESSION_OK";
    case LOCAL_SESSION_NULL:
        return "LOCAL_SESSION_NULL";
    case LOCAL_SESSION_BAD_CAPACITY:
        return "LOCAL_SESSION_BAD_CAPACITY";
    case LOCAL_SESSION_INVALID_VERSION:
        return "LOCAL_SESSION_INVALID_VERSION";
    case LOCAL_SESSION_INVALID_SESSION_ID:
        return "LOCAL_SESSION_INVALID_SESSION_ID";
    case LOCAL_SESSION_INVALID_PRINCIPAL_ID:
        return "LOCAL_SESSION_INVALID_PRINCIPAL_ID";
    case LOCAL_SESSION_INVALID_GENERATION:
        return "LOCAL_SESSION_INVALID_GENERATION";
    case LOCAL_SESSION_INVALID_LIFETIME:
        return "LOCAL_SESSION_INVALID_LIFETIME";
    case LOCAL_SESSION_INVALID_TICK:
        return "LOCAL_SESSION_INVALID_TICK";
    case LOCAL_SESSION_TABLE_FULL:
        return "LOCAL_SESSION_TABLE_FULL";
    case LOCAL_SESSION_ID_EXHAUSTED:
        return "LOCAL_SESSION_ID_EXHAUSTED";
    case LOCAL_SESSION_STALE_GENERATION:
        return "LOCAL_SESSION_STALE_GENERATION";
    case LOCAL_SESSION_CLOSED:
        return "LOCAL_SESSION_CLOSED";
    case LOCAL_SESSION_EXPIRED:
        return "LOCAL_SESSION_EXPIRED";
    case LOCAL_SESSION_NOT_OWNER:
        return "LOCAL_SESSION_NOT_OWNER";
    case LOCAL_SESSION_INVALID_STATE:
        return "LOCAL_SESSION_INVALID_STATE";
    default:
        return NULL;
    }
}
