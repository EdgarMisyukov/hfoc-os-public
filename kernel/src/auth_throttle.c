#include "auth_throttle.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

static enum auth_throttle_status auth_throttle_table_valid(
    const struct auth_throttle_table *table)
{
    uint32_t index;

    if (table == NULL) {
        return AUTH_THROTTLE_NULL;
    }
    if (table->version != AUTH_THROTTLE_VERSION) {
        return AUTH_THROTTLE_INVALID_VERSION;
    }
    if (table->capacity == 0U ||
        table->capacity > AUTH_THROTTLE_TABLE_MAX) {
        return AUTH_THROTTLE_BAD_CAPACITY;
    }
    if (table->failure_limit < AUTH_THROTTLE_FAILURE_LIMIT_MIN ||
        table->failure_limit > AUTH_THROTTLE_FAILURE_LIMIT_MAX) {
        return AUTH_THROTTLE_INVALID_FAILURE_LIMIT;
    }
    if (table->cooldown_ticks == 0U ||
        table->cooldown_ticks > AUTH_THROTTLE_COOLDOWN_MAX) {
        return AUTH_THROTTLE_INVALID_COOLDOWN;
    }

    for (index = 0U; index < AUTH_THROTTLE_TABLE_MAX; ++index) {
        const struct auth_throttle_entry *entry = &table->entries[index];

        if (entry->version != AUTH_THROTTLE_VERSION) {
            return AUTH_THROTTLE_INVALID_STATE;
        }
        if (entry->state == AUTH_THROTTLE_ENTRY_FREE) {
            if (entry->principal_id != 0U ||
                entry->failure_count != 0U ||
                entry->locked_until_tick != 0U) {
                return AUTH_THROTTLE_INVALID_STATE;
            }
        } else if (entry->state == AUTH_THROTTLE_ENTRY_ACTIVE) {
            if (entry->principal_id < AUTH_THROTTLE_PRINCIPAL_ID_MIN ||
                entry->principal_id > AUTH_THROTTLE_PRINCIPAL_ID_MAX) {
                return AUTH_THROTTLE_INVALID_STATE;
            }
            if (entry->failure_count == 0U ||
                entry->failure_count > table->failure_limit) {
                return AUTH_THROTTLE_INVALID_STATE;
            }
            if (entry->failure_count < table->failure_limit &&
                entry->locked_until_tick != 0U) {
                return AUTH_THROTTLE_INVALID_STATE;
            }
        } else {
            return AUTH_THROTTLE_INVALID_STATE;
        }
    }
    return AUTH_THROTTLE_OK;
}

static int auth_throttle_find_active(
    const struct auth_throttle_table *table,
    uint32_t principal_id)
{
    uint32_t index;

    for (index = 0U; index < table->capacity; ++index) {
        if (table->entries[index].state == AUTH_THROTTLE_ENTRY_ACTIVE &&
            table->entries[index].principal_id == principal_id) {
            return (int)index;
        }
    }
    return -1;
}

static int auth_throttle_find_free(
    const struct auth_throttle_table *table)
{
    uint32_t index;

    for (index = 0U; index < table->capacity; ++index) {
        if (table->entries[index].state == AUTH_THROTTLE_ENTRY_FREE) {
            return (int)index;
        }
    }
    return -1;
}

static void auth_throttle_clear_entry(
    struct auth_throttle_entry *entry)
{
    memset(entry, 0, sizeof(*entry));
    entry->version = AUTH_THROTTLE_VERSION;
    entry->state = AUTH_THROTTLE_ENTRY_FREE;
}

enum auth_throttle_status auth_throttle_table_init(
    struct auth_throttle_table *table,
    uint32_t capacity,
    uint32_t failure_limit,
    uint32_t cooldown_ticks)
{
    uint32_t index;

    if (table == NULL) {
        return AUTH_THROTTLE_NULL;
    }
    if (capacity == 0U || capacity > AUTH_THROTTLE_TABLE_MAX) {
        return AUTH_THROTTLE_BAD_CAPACITY;
    }
    if (failure_limit < AUTH_THROTTLE_FAILURE_LIMIT_MIN ||
        failure_limit > AUTH_THROTTLE_FAILURE_LIMIT_MAX) {
        return AUTH_THROTTLE_INVALID_FAILURE_LIMIT;
    }
    if (cooldown_ticks == 0U ||
        cooldown_ticks > AUTH_THROTTLE_COOLDOWN_MAX) {
        return AUTH_THROTTLE_INVALID_COOLDOWN;
    }

    memset(table, 0, sizeof(*table));
    table->version = AUTH_THROTTLE_VERSION;
    table->capacity = capacity;
    table->failure_limit = failure_limit;
    table->cooldown_ticks = cooldown_ticks;
    for (index = 0U; index < AUTH_THROTTLE_TABLE_MAX; ++index) {
        auth_throttle_clear_entry(&table->entries[index]);
    }
    return AUTH_THROTTLE_OK;
}

enum auth_throttle_status auth_throttle_record_failure(
    struct auth_throttle_table *table,
    uint32_t principal_id,
    uint64_t now_tick,
    uint64_t *out_locked_until_tick)
{
    enum auth_throttle_status status;
    struct auth_throttle_entry *entry;
    uint64_t locked_until_tick;
    int index;

    if (table == NULL || out_locked_until_tick == NULL) {
        return AUTH_THROTTLE_NULL;
    }
    status = auth_throttle_table_valid(table);
    if (status != AUTH_THROTTLE_OK) {
        return status;
    }
    if (principal_id < AUTH_THROTTLE_PRINCIPAL_ID_MIN ||
        principal_id > AUTH_THROTTLE_PRINCIPAL_ID_MAX) {
        return AUTH_THROTTLE_INVALID_PRINCIPAL_ID;
    }
    if (now_tick > UINT64_MAX - (uint64_t)table->cooldown_ticks) {
        return AUTH_THROTTLE_INVALID_TICK;
    }

    index = auth_throttle_find_active(table, principal_id);
    if (index < 0) {
        index = auth_throttle_find_free(table);
        if (index < 0) {
            return AUTH_THROTTLE_TABLE_FULL;
        }
        entry = &table->entries[index];
        auth_throttle_clear_entry(entry);
        entry->principal_id = principal_id;
        entry->failure_count = 1U;
        if (table->failure_limit == 1U) {
            entry->locked_until_tick =
                now_tick + (uint64_t)table->cooldown_ticks;
        }
        entry->state = AUTH_THROTTLE_ENTRY_ACTIVE;
        *out_locked_until_tick = entry->locked_until_tick;
        return AUTH_THROTTLE_OK;
    }

    entry = &table->entries[index];
    if (entry->locked_until_tick > now_tick) {
        *out_locked_until_tick = entry->locked_until_tick;
        return AUTH_THROTTLE_LOCKED;
    }

    if (entry->failure_count == table->failure_limit) {
        entry->failure_count = 1U;
        if (table->failure_limit == 1U) {
            entry->locked_until_tick =
                now_tick + (uint64_t)table->cooldown_ticks;
        } else {
            entry->locked_until_tick = 0U;
        }
        *out_locked_until_tick = entry->locked_until_tick;
        return AUTH_THROTTLE_OK;
    }

    ++entry->failure_count;
    if (entry->failure_count == table->failure_limit) {
        locked_until_tick =
            now_tick + (uint64_t)table->cooldown_ticks;
        entry->locked_until_tick = locked_until_tick;
    } else {
        entry->locked_until_tick = 0U;
        locked_until_tick = 0U;
    }
    *out_locked_until_tick = locked_until_tick;
    return AUTH_THROTTLE_OK;
}

enum auth_throttle_status auth_throttle_reset(
    struct auth_throttle_table *table,
    uint32_t principal_id)
{
    enum auth_throttle_status status;
    uint32_t index;

    if (table == NULL) {
        return AUTH_THROTTLE_NULL;
    }
    status = auth_throttle_table_valid(table);
    if (status != AUTH_THROTTLE_OK) {
        return status;
    }
    if (principal_id < AUTH_THROTTLE_PRINCIPAL_ID_MIN ||
        principal_id > AUTH_THROTTLE_PRINCIPAL_ID_MAX) {
        return AUTH_THROTTLE_INVALID_PRINCIPAL_ID;
    }

    for (index = 0U; index < table->capacity; ++index) {
        if (table->entries[index].state == AUTH_THROTTLE_ENTRY_ACTIVE &&
            table->entries[index].principal_id == principal_id) {
            auth_throttle_clear_entry(&table->entries[index]);
        }
    }
    return AUTH_THROTTLE_OK;
}

enum auth_throttle_status auth_throttle_check(
    const struct auth_throttle_table *table,
    uint32_t principal_id,
    uint64_t now_tick)
{
    enum auth_throttle_status status;
    int index;

    if (table == NULL) {
        return AUTH_THROTTLE_NULL;
    }
    status = auth_throttle_table_valid(table);
    if (status != AUTH_THROTTLE_OK) {
        return status;
    }
    if (principal_id < AUTH_THROTTLE_PRINCIPAL_ID_MIN ||
        principal_id > AUTH_THROTTLE_PRINCIPAL_ID_MAX) {
        return AUTH_THROTTLE_INVALID_PRINCIPAL_ID;
    }

    index = auth_throttle_find_active(table, principal_id);
    if (index < 0) {
        return AUTH_THROTTLE_OK;
    }
    if (table->entries[index].locked_until_tick > now_tick) {
        return AUTH_THROTTLE_LOCKED;
    }
    return AUTH_THROTTLE_OK;
}

const char *auth_throttle_status_string(
    enum auth_throttle_status status)
{
    switch (status) {
    case AUTH_THROTTLE_OK:
        return "AUTH_THROTTLE_OK";
    case AUTH_THROTTLE_NULL:
        return "AUTH_THROTTLE_NULL";
    case AUTH_THROTTLE_BAD_CAPACITY:
        return "AUTH_THROTTLE_BAD_CAPACITY";
    case AUTH_THROTTLE_INVALID_VERSION:
        return "AUTH_THROTTLE_INVALID_VERSION";
    case AUTH_THROTTLE_INVALID_PRINCIPAL_ID:
        return "AUTH_THROTTLE_INVALID_PRINCIPAL_ID";
    case AUTH_THROTTLE_INVALID_FAILURE_LIMIT:
        return "AUTH_THROTTLE_INVALID_FAILURE_LIMIT";
    case AUTH_THROTTLE_INVALID_COOLDOWN:
        return "AUTH_THROTTLE_INVALID_COOLDOWN";
    case AUTH_THROTTLE_INVALID_TICK:
        return "AUTH_THROTTLE_INVALID_TICK";
    case AUTH_THROTTLE_TABLE_FULL:
        return "AUTH_THROTTLE_TABLE_FULL";
    case AUTH_THROTTLE_LOCKED:
        return "AUTH_THROTTLE_LOCKED";
    case AUTH_THROTTLE_INVALID_STATE:
        return "AUTH_THROTTLE_INVALID_STATE";
    default:
        return NULL;
    }
}
