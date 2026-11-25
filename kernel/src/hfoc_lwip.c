#include "hfoc_lwip.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define HFOC_LWIP_FREE_END UINT32_MAX

struct hfoc_lwip_pool {
    uint32_t slot_size;
    uint32_t slot_count;
    uint32_t free_list;
    uint32_t free_count;
    uint32_t used_count;
    uint32_t high_water_count;
    uint8_t *base;
    uint8_t used[HFOC_LWIP_POOL_SLOT_COUNT_MAX];
    uint32_t next[HFOC_LWIP_POOL_SLOT_COUNT_MAX];
};

static _Alignas(64) uint8_t hfoc_lwip_storage[
    HFOC_LWIP_ALLOC_TOTAL_BYTES];
static struct hfoc_lwip_pool hfoc_lwip_pools[HFOC_LWIP_POOL_COUNT];
static bool hfoc_lwip_published;

static const uint32_t hfoc_lwip_slot_sizes[HFOC_LWIP_POOL_COUNT] = {
    HFOC_LWIP_POOL_SLOT_SIZE_TINY,
    HFOC_LWIP_POOL_SLOT_SIZE_SMALL,
    HFOC_LWIP_POOL_SLOT_SIZE_MEDIUM,
    HFOC_LWIP_POOL_SLOT_SIZE_LARGE,
};

static const uint32_t hfoc_lwip_slot_counts[HFOC_LWIP_POOL_COUNT] = {
    HFOC_LWIP_POOL_SLOT_COUNT_TINY,
    HFOC_LWIP_POOL_SLOT_COUNT_SMALL,
    HFOC_LWIP_POOL_SLOT_COUNT_MEDIUM,
    HFOC_LWIP_POOL_SLOT_COUNT_LARGE,
};

static bool hfoc_lwip_pool_class_valid(unsigned int pool_class)
{
    return pool_class < HFOC_LWIP_POOL_COUNT;
}

bool hfoc_lwip_alloc_init(void)
{
    unsigned int pool_class;
    size_t offset = 0U;

    if (hfoc_lwip_published) {
        return false;
    }

    memset(hfoc_lwip_pools, 0, sizeof(hfoc_lwip_pools));
    for (pool_class = 0U; pool_class < HFOC_LWIP_POOL_COUNT;
         ++pool_class) {
        struct hfoc_lwip_pool *pool = &hfoc_lwip_pools[pool_class];
        uint32_t slot;

        pool->slot_size = hfoc_lwip_slot_sizes[pool_class];
        pool->slot_count = hfoc_lwip_slot_counts[pool_class];
        pool->base = &hfoc_lwip_storage[offset];
        pool->free_list = 0U;
        pool->free_count = pool->slot_count;
        pool->used_count = 0U;
        pool->high_water_count = 0U;

        for (slot = 0U; slot < pool->slot_count; ++slot) {
            pool->next[slot] =
                (slot + 1U < pool->slot_count) ?
                    slot + 1U : HFOC_LWIP_FREE_END;
            pool->used[slot] = 0U;
        }
        offset += (size_t)pool->slot_size * pool->slot_count;
    }
    hfoc_lwip_published = true;
    return true;
}

void hfoc_lwip_alloc_shutdown(void)
{
    memset(hfoc_lwip_pools, 0, sizeof(hfoc_lwip_pools));
    hfoc_lwip_published = false;
}

bool hfoc_lwip_alloc_ready(void)
{
    return hfoc_lwip_published;
}

static unsigned int hfoc_lwip_pool_class_for_size(size_t size)
{
    if (size == 0U || size > HFOC_LWIP_ALLOC_MAX_SIZE) {
        return HFOC_LWIP_POOL_COUNT;
    }
    if (size <= HFOC_LWIP_POOL_SLOT_SIZE_TINY) {
        return HFOC_LWIP_POOL_CLASS_TINY;
    }
    if (size <= HFOC_LWIP_POOL_SLOT_SIZE_SMALL) {
        return HFOC_LWIP_POOL_CLASS_SMALL;
    }
    if (size <= HFOC_LWIP_POOL_SLOT_SIZE_MEDIUM) {
        return HFOC_LWIP_POOL_CLASS_MEDIUM;
    }
    return HFOC_LWIP_POOL_CLASS_LARGE;
}

void *hfoc_lwip_alloc(size_t size)
{
    unsigned int pool_class;
    struct hfoc_lwip_pool *pool;
    uint32_t slot;

    if (!hfoc_lwip_published) {
        return NULL;
    }
    pool_class = hfoc_lwip_pool_class_for_size(size);
    if (pool_class == HFOC_LWIP_POOL_COUNT) {
        return NULL;
    }

    pool = &hfoc_lwip_pools[pool_class];
    if (pool->free_list == HFOC_LWIP_FREE_END) {
        return NULL;
    }

    slot = pool->free_list;
    pool->free_list = pool->next[slot];
    pool->next[slot] = HFOC_LWIP_FREE_END;
    pool->used[slot] = 1U;
    --pool->free_count;
    ++pool->used_count;
    if (pool->used_count > pool->high_water_count) {
        pool->high_water_count = pool->used_count;
    }
    return pool->base + (size_t)slot * pool->slot_size;
}

void *hfoc_lwip_alloc_calloc(size_t count, size_t size)
{
    size_t total;
    void *result;

    if (count == 0U || size == 0U || count > SIZE_MAX / size) {
        return NULL;
    }
    total = count * size;
    result = hfoc_lwip_alloc(total);
    if (result != NULL) {
        memset(result, 0, total);
    }
    return result;
}

bool hfoc_lwip_alloc_free(void *ptr)
{
    uintptr_t address;
    unsigned int pool_class;

    if (!hfoc_lwip_published || ptr == NULL) {
        return false;
    }

    address = (uintptr_t)ptr;
    for (pool_class = 0U; pool_class < HFOC_LWIP_POOL_COUNT;
         ++pool_class) {
        struct hfoc_lwip_pool *pool = &hfoc_lwip_pools[pool_class];
        uintptr_t base = (uintptr_t)pool->base;
        uintptr_t offset;
        uint32_t slot;

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

        pool->used[slot] = 0U;
        pool->next[slot] = pool->free_list;
        pool->free_list = slot;
        ++pool->free_count;
        --pool->used_count;
        return true;
    }
    return false;
}

struct hfoc_lwip_alloc_snapshot hfoc_lwip_alloc_snapshot(void)
{
    struct hfoc_lwip_alloc_snapshot snapshot;
    unsigned int pool_class;
    uint64_t used_bytes = 0U;
    uint64_t free_bytes = 0U;
    uint64_t high_water_bytes = 0U;

    memset(&snapshot, 0, sizeof(snapshot));
    if (!hfoc_lwip_published) {
        return snapshot;
    }

    snapshot.version = HFOC_LWIP_ALLOC_VERSION;
    for (pool_class = 0U; pool_class < HFOC_LWIP_POOL_COUNT;
         ++pool_class) {
        const struct hfoc_lwip_pool *pool =
            &hfoc_lwip_pools[pool_class];
        struct hfoc_lwip_alloc_pool_snapshot *pool_snapshot =
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
    return snapshot;
}

uint32_t hfoc_lwip_alloc_free_count(unsigned int pool_class)
{
    if (!hfoc_lwip_published ||
        !hfoc_lwip_pool_class_valid(pool_class)) {
        return 0U;
    }
    return hfoc_lwip_pools[pool_class].free_count;
}

uint32_t hfoc_lwip_alloc_used_count(unsigned int pool_class)
{
    if (!hfoc_lwip_published ||
        !hfoc_lwip_pool_class_valid(pool_class)) {
        return 0U;
    }
    return hfoc_lwip_pools[pool_class].used_count;
}

uint32_t hfoc_lwip_alloc_high_water_count(unsigned int pool_class)
{
    if (!hfoc_lwip_published ||
        !hfoc_lwip_pool_class_valid(pool_class)) {
        return 0U;
    }
    return hfoc_lwip_pools[pool_class].high_water_count;
}

void *hfoc_lwip_mem_malloc(size_t size)
{
    return hfoc_lwip_alloc(size);
}

void *hfoc_lwip_mem_calloc(size_t count, size_t size)
{
    return hfoc_lwip_alloc_calloc(count, size);
}

void hfoc_lwip_mem_free(void *ptr)
{
    (void)hfoc_lwip_alloc_free(ptr);
}
