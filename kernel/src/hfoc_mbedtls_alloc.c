#include "hfoc_mbedtls_alloc.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define HFOC_MBEDTLS_FREE_END UINT32_MAX

struct hfoc_mbedtls_pool {
    uint32_t slot_size;
    uint32_t slot_count;
    uint32_t free_list;
    uint32_t free_count;
    uint32_t used_count;
    uint32_t high_water_count;
    uint8_t *base;
    uint8_t used[HFOC_MBEDTLS_POOL_SLOT_COUNT_MAX];
    uint32_t owner[HFOC_MBEDTLS_POOL_SLOT_COUNT_MAX];
    uint32_t next[HFOC_MBEDTLS_POOL_SLOT_COUNT_MAX];
};

struct hfoc_mbedtls_connection {
    bool active;
    uint32_t id;
    uint32_t allocation_count;
    uint32_t used_bytes;
    uint32_t class_used[HFOC_MBEDTLS_POOL_COUNT];
};

static _Alignas(64) uint8_t hfoc_mbedtls_storage[
    HFOC_MBEDTLS_ALLOC_TOTAL_BYTES];
static struct hfoc_mbedtls_pool hfoc_mbedtls_pools[
    HFOC_MBEDTLS_POOL_COUNT];
static struct hfoc_mbedtls_connection hfoc_mbedtls_connections[
    HFOC_MBEDTLS_CONNECTION_COUNT];
static bool hfoc_mbedtls_published;
/* ITEM-619: a bounded scope stack, not one process-wide TLS owner. */
static uint32_t hfoc_mbedtls_context_stack[HFOC_MBEDTLS_CONNECTION_COUNT];
static uint32_t hfoc_mbedtls_context_depth;
static uint32_t hfoc_mbedtls_current_connection;

static const uint32_t hfoc_mbedtls_slot_sizes[
    HFOC_MBEDTLS_POOL_COUNT] = {
    HFOC_MBEDTLS_POOL_SLOT_SIZE_TINY,
    HFOC_MBEDTLS_POOL_SLOT_SIZE_SMALL,
    HFOC_MBEDTLS_POOL_SLOT_SIZE_MEDIUM,
    HFOC_MBEDTLS_POOL_SLOT_SIZE_LARGE,
};

static const uint32_t hfoc_mbedtls_slot_counts[
    HFOC_MBEDTLS_POOL_COUNT] = {
    HFOC_MBEDTLS_POOL_SLOT_COUNT_TINY,
    HFOC_MBEDTLS_POOL_SLOT_COUNT_SMALL,
    HFOC_MBEDTLS_POOL_SLOT_COUNT_MEDIUM,
    HFOC_MBEDTLS_POOL_SLOT_COUNT_LARGE,
};

static const uint32_t hfoc_mbedtls_connection_maxes[
    HFOC_MBEDTLS_POOL_COUNT] = {
    HFOC_MBEDTLS_CONNECTION_MAX_TINY,
    HFOC_MBEDTLS_CONNECTION_MAX_SMALL,
    HFOC_MBEDTLS_CONNECTION_MAX_MEDIUM,
    HFOC_MBEDTLS_CONNECTION_MAX_LARGE,
};

static bool hfoc_mbedtls_pool_class_valid(unsigned int pool_class)
{
    return pool_class < HFOC_MBEDTLS_POOL_COUNT;
}

static void hfoc_mbedtls_zeroize_range(
    void *object,
    size_t size)
{
    volatile uint8_t *bytes = (volatile uint8_t *)object;
    size_t index;

    for (index = 0U; index < size; ++index) {
        bytes[index] = 0U;
    }
}

static struct hfoc_mbedtls_connection *
hfoc_mbedtls_connection_by_id(uint32_t id)
{
    struct hfoc_mbedtls_connection *connection;

    if (id == 0U || id > HFOC_MBEDTLS_CONNECTION_COUNT) {
        return NULL;
    }
    connection = &hfoc_mbedtls_connections[id - 1U];
    if (!connection->active || connection->id != id) {
        return NULL;
    }
    return connection;
}

bool hfoc_mbedtls_alloc_init(void)
{
    unsigned int pool_class;
    size_t offset = 0U;

    if (hfoc_mbedtls_published) {
        return false;
    }

    memset(hfoc_mbedtls_pools, 0, sizeof(hfoc_mbedtls_pools));
    memset(hfoc_mbedtls_connections, 0,
           sizeof(hfoc_mbedtls_connections));
    memset(hfoc_mbedtls_context_stack, 0,
           sizeof(hfoc_mbedtls_context_stack));
    hfoc_mbedtls_context_depth = 0U;
    hfoc_mbedtls_current_connection = 0U;

    for (pool_class = 0U; pool_class < HFOC_MBEDTLS_POOL_COUNT;
         ++pool_class) {
        struct hfoc_mbedtls_pool *pool =
            &hfoc_mbedtls_pools[pool_class];
        uint32_t slot;

        pool->slot_size = hfoc_mbedtls_slot_sizes[pool_class];
        pool->slot_count = hfoc_mbedtls_slot_counts[pool_class];
        pool->base = &hfoc_mbedtls_storage[offset];
        pool->free_list = 0U;
        pool->free_count = pool->slot_count;
        pool->used_count = 0U;
        pool->high_water_count = 0U;

        for (slot = 0U; slot < pool->slot_count; ++slot) {
            pool->next[slot] =
                (slot + 1U < pool->slot_count) ?
                    slot + 1U : HFOC_MBEDTLS_FREE_END;
            pool->used[slot] = 0U;
            pool->owner[slot] = 0U;
        }
        offset += (size_t)pool->slot_size * pool->slot_count;
    }
    hfoc_mbedtls_published = true;
    return true;
}

void hfoc_mbedtls_alloc_shutdown(void)
{
    hfoc_mbedtls_zeroize_range(
        hfoc_mbedtls_storage,
        sizeof(hfoc_mbedtls_storage));
    memset(hfoc_mbedtls_pools, 0, sizeof(hfoc_mbedtls_pools));
    memset(hfoc_mbedtls_connections, 0,
           sizeof(hfoc_mbedtls_connections));
    memset(hfoc_mbedtls_context_stack, 0,
           sizeof(hfoc_mbedtls_context_stack));
    hfoc_mbedtls_context_depth = 0U;
    hfoc_mbedtls_current_connection = 0U;
    hfoc_mbedtls_published = false;
}

bool hfoc_mbedtls_alloc_ready(void)
{
    return hfoc_mbedtls_published;
}

static unsigned int hfoc_mbedtls_pool_class_for_size(size_t size)
{
    if (size == 0U || size > HFOC_MBEDTLS_ALLOC_MAX_SIZE) {
        return HFOC_MBEDTLS_POOL_COUNT;
    }
    if (size <= HFOC_MBEDTLS_POOL_SLOT_SIZE_TINY) {
        return HFOC_MBEDTLS_POOL_CLASS_TINY;
    }
    if (size <= HFOC_MBEDTLS_POOL_SLOT_SIZE_SMALL) {
        return HFOC_MBEDTLS_POOL_CLASS_SMALL;
    }
    if (size <= HFOC_MBEDTLS_POOL_SLOT_SIZE_MEDIUM) {
        return HFOC_MBEDTLS_POOL_CLASS_MEDIUM;
    }
    return HFOC_MBEDTLS_POOL_CLASS_LARGE;
}

static bool hfoc_mbedtls_connection_budget_exhausted(
    unsigned int pool_class)
{
    struct hfoc_mbedtls_connection *connection;

    connection = hfoc_mbedtls_connection_by_id(
        hfoc_mbedtls_current_connection);
    if (connection == NULL) {
        return false;
    }
    return connection->class_used[pool_class] >=
           hfoc_mbedtls_connection_maxes[pool_class];
}

void *hfoc_mbedtls_alloc(size_t size)
{
    unsigned int pool_class;
    struct hfoc_mbedtls_pool *pool;
    struct hfoc_mbedtls_connection *connection;
    uint32_t slot;

    if (!hfoc_mbedtls_published) {
        return NULL;
    }
    pool_class = hfoc_mbedtls_pool_class_for_size(size);
    if (pool_class == HFOC_MBEDTLS_POOL_COUNT ||
        hfoc_mbedtls_connection_budget_exhausted(pool_class)) {
        return NULL;
    }

    pool = &hfoc_mbedtls_pools[pool_class];
    if (pool->free_list == HFOC_MBEDTLS_FREE_END) {
        return NULL;
    }

    slot = pool->free_list;
    pool->free_list = pool->next[slot];
    pool->next[slot] = HFOC_MBEDTLS_FREE_END;
    pool->used[slot] = 1U;
    pool->owner[slot] = hfoc_mbedtls_current_connection;
    --pool->free_count;
    ++pool->used_count;
    if (pool->used_count > pool->high_water_count) {
        pool->high_water_count = pool->used_count;
    }

    connection = hfoc_mbedtls_connection_by_id(
        hfoc_mbedtls_current_connection);
    if (connection != NULL) {
        ++connection->class_used[pool_class];
        ++connection->allocation_count;
        connection->used_bytes += pool->slot_size;
    }
    return pool->base + (size_t)slot * pool->slot_size;
}

void *hfoc_mbedtls_calloc(size_t count, size_t size)
{
    size_t total;
    void *result;

    if (count == 0U || size == 0U || count > SIZE_MAX / size) {
        return NULL;
    }
    total = count * size;
    result = hfoc_mbedtls_alloc(total);
    if (result != NULL) {
        memset(result, 0, total);
    }
    return result;
}

bool hfoc_mbedtls_alloc_free(void *ptr)
{
    uintptr_t address;
    unsigned int pool_class;

    if (!hfoc_mbedtls_published || ptr == NULL) {
        return false;
    }

    address = (uintptr_t)ptr;
    for (pool_class = 0U; pool_class < HFOC_MBEDTLS_POOL_COUNT;
         ++pool_class) {
        struct hfoc_mbedtls_pool *pool =
            &hfoc_mbedtls_pools[pool_class];
        struct hfoc_mbedtls_connection *connection;
        uintptr_t base = (uintptr_t)pool->base;
        uintptr_t offset;
        uint32_t slot;
        uint32_t owner;

        if (address < base) {
            continue;
        }
        offset = address - base;
        if (offset % pool->slot_size != 0U) {
            continue;
        }
        slot = (uint32_t)(offset / pool->slot_size);
        if (slot >= pool->slot_count || !pool->used[slot]) {
            continue;
        }

        owner = pool->owner[slot];
        connection = hfoc_mbedtls_connection_by_id(owner);
        if (owner != 0U && connection == NULL) {
            return false;
        }
        if (connection != NULL &&
            (connection->class_used[pool_class] == 0U ||
             connection->allocation_count == 0U ||
             connection->used_bytes < pool->slot_size)) {
            return false;
        }

        hfoc_mbedtls_zeroize_range(
            pool->base + (size_t)slot * pool->slot_size,
            pool->slot_size);
        pool->used[slot] = 0U;
        pool->owner[slot] = 0U;
        pool->next[slot] = pool->free_list;
        pool->free_list = slot;
        ++pool->free_count;
        --pool->used_count;

        if (connection != NULL) {
            --connection->class_used[pool_class];
            --connection->allocation_count;
            connection->used_bytes -= pool->slot_size;
        }
        return true;
    }
    return false;
}

void hfoc_mbedtls_free(void *ptr)
{
    (void)hfoc_mbedtls_alloc_free(ptr);
}

bool hfoc_mbedtls_alloc_connection_open(uint32_t *id)
{
    uint32_t index;

    if (!hfoc_mbedtls_published || id == NULL) {
        return false;
    }
    for (index = 0U; index < HFOC_MBEDTLS_CONNECTION_COUNT;
         ++index) {
        struct hfoc_mbedtls_connection *connection =
            &hfoc_mbedtls_connections[index];

        if (!connection->active) {
            memset(connection, 0, sizeof(*connection));
            connection->active = true;
            connection->id = index + 1U;
            *id = connection->id;
            return true;
        }
    }
    return false;
}

bool hfoc_mbedtls_alloc_connection_close(uint32_t id)
{
    struct hfoc_mbedtls_connection *connection;
    unsigned int pool_class;
    uint64_t expected_bytes = 0U;
    uint32_t expected_allocations = 0U;

    connection = hfoc_mbedtls_connection_by_id(id);
    if (connection == NULL) {
        return false;
    }
    for (pool_class = 0U; pool_class < HFOC_MBEDTLS_POOL_COUNT;
         ++pool_class) {
        expected_allocations += connection->class_used[pool_class];
        expected_bytes +=
            (uint64_t)hfoc_mbedtls_slot_sizes[pool_class] *
            connection->class_used[pool_class];
    }
    if (expected_allocations != connection->allocation_count ||
        expected_bytes != connection->used_bytes) {
        return false;
    }
    /*
     * A connection is normally closed after its scope was exited.  Removing
     * every remaining occurrence keeps a closed connection from becoming the
     * allocation owner again through a stale scope.
     */
    {
        uint32_t source;
        uint32_t destination = 0U;

        for (source = 0U; source < hfoc_mbedtls_context_depth; ++source) {
            if (hfoc_mbedtls_context_stack[source] != id) {
                hfoc_mbedtls_context_stack[destination++] =
                    hfoc_mbedtls_context_stack[source];
            }
        }
        hfoc_mbedtls_context_depth = destination;
        hfoc_mbedtls_current_connection = destination == 0U
            ? 0U
            : hfoc_mbedtls_context_stack[destination - 1U];
    }

    for (pool_class = 0U; pool_class < HFOC_MBEDTLS_POOL_COUNT;
         ++pool_class) {
        struct hfoc_mbedtls_pool *pool =
            &hfoc_mbedtls_pools[pool_class];
        uint32_t slot;

        for (slot = 0U; slot < pool->slot_count; ++slot) {
            if (!pool->used[slot] || pool->owner[slot] != id) {
                continue;
            }
            hfoc_mbedtls_zeroize_range(
                pool->base + (size_t)slot * pool->slot_size,
                pool->slot_size);
            pool->used[slot] = 0U;
            pool->owner[slot] = 0U;
            pool->next[slot] = pool->free_list;
            pool->free_list = slot;
            ++pool->free_count;
            --pool->used_count;

            if (connection->class_used[pool_class] == 0U ||
                connection->allocation_count == 0U ||
                connection->used_bytes < pool->slot_size) {
                return false;
            }
            --connection->class_used[pool_class];
            --connection->allocation_count;
            connection->used_bytes -= pool->slot_size;
        }
    }

    if (connection->allocation_count != 0U ||
        connection->used_bytes != 0U) {
        return false;
    }
    memset(connection, 0, sizeof(*connection));
    return true;
}

bool hfoc_mbedtls_alloc_connection_enter(uint32_t id)
{
    uint32_t index;

    if (!hfoc_mbedtls_published ||
        hfoc_mbedtls_context_depth >= HFOC_MBEDTLS_CONNECTION_COUNT ||
        hfoc_mbedtls_connection_by_id(id) == NULL) {
        return false;
    }
    /*
     * The same connection is never entered twice in one call chain: a nested
     * duplicate would make the matching exit pop somebody else's scope.
     */
    for (index = 0U; index < hfoc_mbedtls_context_depth; ++index) {
        if (hfoc_mbedtls_context_stack[index] == id) {
            return false;
        }
    }
    hfoc_mbedtls_context_stack[hfoc_mbedtls_context_depth++] = id;
    hfoc_mbedtls_current_connection = id;
    return true;
}

bool hfoc_mbedtls_alloc_connection_exit(void)
{
    if (!hfoc_mbedtls_published ||
        hfoc_mbedtls_context_depth == 0U) {
        return false;
    }
    --hfoc_mbedtls_context_depth;
    hfoc_mbedtls_context_stack[hfoc_mbedtls_context_depth] = 0U;
    hfoc_mbedtls_current_connection =
        hfoc_mbedtls_context_depth == 0U
            ? 0U
            : hfoc_mbedtls_context_stack[hfoc_mbedtls_context_depth - 1U];
    return true;
}

struct hfoc_mbedtls_alloc_snapshot hfoc_mbedtls_alloc_snapshot(void)
{
    struct hfoc_mbedtls_alloc_snapshot snapshot;
    unsigned int pool_class;
    uint32_t connection_index;
    uint64_t used_bytes = 0U;
    uint64_t free_bytes = 0U;
    uint64_t high_water_bytes = 0U;

    memset(&snapshot, 0, sizeof(snapshot));
    if (!hfoc_mbedtls_published) {
        return snapshot;
    }

    snapshot.version = HFOC_MBEDTLS_ALLOC_VERSION;
    for (pool_class = 0U; pool_class < HFOC_MBEDTLS_POOL_COUNT;
         ++pool_class) {
        const struct hfoc_mbedtls_pool *pool =
            &hfoc_mbedtls_pools[pool_class];
        struct hfoc_mbedtls_alloc_pool_snapshot *pool_snapshot =
            &snapshot.pools[pool_class];
        uint64_t slot_size = pool->slot_size;

        pool_snapshot->slot_size = pool->slot_size;
        pool_snapshot->slot_count = pool->slot_count;
        pool_snapshot->free_count = pool->free_count;
        pool_snapshot->used_count = pool->used_count;
        pool_snapshot->high_water_count = pool->high_water_count;

        used_bytes += (uint64_t)pool->used_count * slot_size;
        free_bytes += (uint64_t)pool->free_count * slot_size;
        high_water_bytes +=
            (uint64_t)pool->high_water_count * slot_size;
    }
    snapshot.total_used_bytes = (uint32_t)used_bytes;
    snapshot.total_free_bytes = (uint32_t)free_bytes;
    snapshot.total_high_water_bytes = (uint32_t)high_water_bytes;

    for (connection_index = 0U;
         connection_index < HFOC_MBEDTLS_CONNECTION_COUNT;
         ++connection_index) {
        const struct hfoc_mbedtls_connection *connection =
            &hfoc_mbedtls_connections[connection_index];
        struct hfoc_mbedtls_connection_snapshot *connection_snapshot =
            &snapshot.connections[connection_index];

        connection_snapshot->active = connection->active;
        connection_snapshot->id = connection->id;
        connection_snapshot->allocation_count =
            connection->allocation_count;
        connection_snapshot->used_bytes = connection->used_bytes;
        connection_snapshot->budget_bytes =
            HFOC_MBEDTLS_CONNECTION_BUDGET_BYTES;
    }
    return snapshot;
}

uint32_t hfoc_mbedtls_alloc_free_count(unsigned int pool_class)
{
    if (!hfoc_mbedtls_published ||
        !hfoc_mbedtls_pool_class_valid(pool_class)) {
        return 0U;
    }
    return hfoc_mbedtls_pools[pool_class].free_count;
}

uint32_t hfoc_mbedtls_alloc_used_count(unsigned int pool_class)
{
    if (!hfoc_mbedtls_published ||
        !hfoc_mbedtls_pool_class_valid(pool_class)) {
        return 0U;
    }
    return hfoc_mbedtls_pools[pool_class].used_count;
}

uint32_t hfoc_mbedtls_alloc_high_water_count(unsigned int pool_class)
{
    if (!hfoc_mbedtls_published ||
        !hfoc_mbedtls_pool_class_valid(pool_class)) {
        return 0U;
    }
    return hfoc_mbedtls_pools[pool_class].high_water_count;
}
