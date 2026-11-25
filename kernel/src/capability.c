#include "capability.h"

#include <stddef.h>
#include <string.h>

static bool capability_equal(const struct capability *a,
                             const struct capability *b)
{
    return a->type == b->type &&
           a->flags == b->flags &&
           a->object == b->object;
}

static struct capability_entry *capability_find_entry(
    struct capability_table *table,
    uint64_t handle)
{
    uint32_t index;

    if (handle == PROCESS_CAPABILITY_HANDLE_INVALID) {
        return NULL;
    }
    for (index = 0U; index < PROCESS_CAPABILITY_CAPACITY; ++index) {
        if (table->entries[index].handle == handle) {
            return &table->entries[index];
        }
    }
    return NULL;
}

static const struct capability_entry *capability_find_entry_const(
    const struct capability_table *table,
    uint64_t handle)
{
    uint32_t index;

    if (handle == PROCESS_CAPABILITY_HANDLE_INVALID) {
        return NULL;
    }
    for (index = 0U; index < PROCESS_CAPABILITY_CAPACITY; ++index) {
        if (table->entries[index].handle == handle) {
            return &table->entries[index];
        }
    }
    return NULL;
}

static void capability_entry_clear(struct capability_entry *entry) {
    memset(entry, 0, sizeof(*entry));
}

enum capability_status capability_table_init(struct capability_table *table) {
    if (table == NULL) {
        return CAPABILITY_NULL;
    }
    if (table->published) {
        return CAPABILITY_BAD_STATE;
    }
    memset(table, 0, sizeof(*table));
    table->published = true;
    return CAPABILITY_OK;
}

enum capability_status capability_table_destroy(
    struct capability_table *table)
{
    if (table == NULL) {
        return CAPABILITY_NULL;
    }
    if (!table->published) {
        return CAPABILITY_BAD_STATE;
    }
    memset(table, 0, sizeof(*table));
    return CAPABILITY_OK;
}

enum capability_status capability_table_reserve(
    struct capability_table *table,
    uint64_t *handle_out)
{
    uint32_t index;

    if (table == NULL || handle_out == NULL) {
        return CAPABILITY_NULL;
    }
    if (!table->published) {
        return CAPABILITY_BAD_STATE;
    }
    for (index = 0U; index < PROCESS_CAPABILITY_CAPACITY; ++index) {
        struct capability_entry *const entry = &table->entries[index];

        if (!entry->pending && !entry->published) {
            ++table->next_handle;
            if (table->next_handle == PROCESS_CAPABILITY_HANDLE_INVALID) {
                table->next_handle = 1U;
            }
            entry->pending = true;
            entry->handle = table->next_handle;
            *handle_out = entry->handle;
            return CAPABILITY_OK;
        }
    }
    return CAPABILITY_FULL;
}

enum capability_status capability_table_commit(
    struct capability_table *table,
    uint64_t handle,
    const struct capability *capability)
{
    struct capability_entry *entry;

    if (table == NULL || capability == NULL) {
        return CAPABILITY_NULL;
    }
    if (!table->published) {
        return CAPABILITY_BAD_STATE;
    }
    entry = capability_find_entry(table, handle);
    if (entry == NULL) {
        return CAPABILITY_BAD_HANDLE;
    }
    if (!entry->pending || entry->published) {
        return CAPABILITY_BAD_STATE;
    }
    entry->capability = *capability;
    entry->pending = false;
    entry->published = true;
    ++table->open_count;
    return CAPABILITY_OK;
}

enum capability_status capability_table_rollback(
    struct capability_table *table,
    uint64_t handle)
{
    struct capability_entry *entry;

    if (table == NULL) {
        return CAPABILITY_NULL;
    }
    if (!table->published) {
        return CAPABILITY_BAD_STATE;
    }
    entry = capability_find_entry(table, handle);
    if (entry == NULL) {
        return CAPABILITY_BAD_HANDLE;
    }
    if (!entry->pending || entry->published) {
        return CAPABILITY_BAD_STATE;
    }
    capability_entry_clear(entry);
    return CAPABILITY_OK;
}

enum capability_status capability_table_add(
    struct capability_table *table,
    const struct capability *capability,
    uint64_t *handle_out)
{
    enum capability_status status;
    uint32_t index;

    if (table == NULL || capability == NULL || handle_out == NULL) {
        return CAPABILITY_NULL;
    }
    if (!table->published) {
        return CAPABILITY_BAD_STATE;
    }
    for (index = 0U; index < PROCESS_CAPABILITY_CAPACITY; ++index) {
        const struct capability_entry *const entry = &table->entries[index];

        if (entry->published &&
            capability_equal(&entry->capability, capability)) {
            return CAPABILITY_DUPLICATE;
        }
    }

    status = capability_table_reserve(table, handle_out);
    if (status != CAPABILITY_OK) {
        return status;
    }
    status = capability_table_commit(table, *handle_out, capability);
    if (status != CAPABILITY_OK) {
        (void)capability_table_rollback(table, *handle_out);
        *handle_out = PROCESS_CAPABILITY_HANDLE_INVALID;
        return status;
    }
    return CAPABILITY_OK;
}

static enum capability_status capability_remove(
    struct capability_table *table,
    uint64_t handle)
{
    struct capability_entry *entry;

    if (table == NULL) {
        return CAPABILITY_NULL;
    }
    if (!table->published) {
        return CAPABILITY_BAD_STATE;
    }
    entry = capability_find_entry(table, handle);
    if (entry == NULL) {
        return CAPABILITY_BAD_HANDLE;
    }
    if (!entry->published) {
        return CAPABILITY_NOT_PUBLISHED;
    }
    capability_entry_clear(entry);
    if (table->open_count != 0U) {
        --table->open_count;
    }
    return CAPABILITY_OK;
}

enum capability_status capability_table_close(
    struct capability_table *table,
    uint64_t handle)
{
    return capability_remove(table, handle);
}

enum capability_status capability_table_revoke(
    struct capability_table *table,
    uint64_t handle)
{
    return capability_remove(table, handle);
}

enum capability_status capability_table_get(
    const struct capability_table *table,
    uint64_t handle,
    struct capability *capability_out)
{
    const struct capability_entry *entry;

    if (table == NULL || capability_out == NULL) {
        return CAPABILITY_NULL;
    }
    if (!table->published) {
        return CAPABILITY_BAD_STATE;
    }
    entry = capability_find_entry_const(table, handle);
    if (entry == NULL) {
        return CAPABILITY_BAD_HANDLE;
    }
    if (!entry->published) {
        return CAPABILITY_NOT_PUBLISHED;
    }
    *capability_out = entry->capability;
    return CAPABILITY_OK;
}

bool capability_table_has(const struct capability_table *table,
                          uint32_t type)
{
    uint32_t index;

    if (table == NULL || !table->published) {
        return false;
    }
    for (index = 0U; index < PROCESS_CAPABILITY_CAPACITY; ++index) {
        const struct capability_entry *const entry = &table->entries[index];

        if (entry->published && entry->capability.type == type) {
            return true;
        }
    }
    return false;
}

uint32_t capability_table_open_count(const struct capability_table *table) {
    return table != NULL ? table->open_count : 0U;
}

bool capability_table_is_published(const struct capability_table *table) {
    return table != NULL && table->published;
}

const char *capability_status_string(enum capability_status status) {
    switch (status) {
    case CAPABILITY_OK:
        return "CAPABILITY_OK";
    case CAPABILITY_NULL:
        return "CAPABILITY_NULL";
    case CAPABILITY_BAD_STATE:
        return "CAPABILITY_BAD_STATE";
    case CAPABILITY_DUPLICATE:
        return "CAPABILITY_DUPLICATE";
    case CAPABILITY_FULL:
        return "CAPABILITY_FULL";
    case CAPABILITY_BAD_HANDLE:
        return "CAPABILITY_BAD_HANDLE";
    case CAPABILITY_NOT_PUBLISHED:
        return "CAPABILITY_NOT_PUBLISHED";
    default:
        return "CAPABILITY_UNKNOWN";
    }
}
