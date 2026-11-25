#include "block_cache.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

static enum block_cache_status block_cache_check(
    const struct block_cache *cache)
{
    if (cache == NULL) {
        return BLOCK_CACHE_NULL;
    }
    if (!cache->initialized || cache->device == NULL ||
        block_device_validate(cache->device) != BLOCK_DEVICE_OK) {
        return BLOCK_CACHE_BAD_DEVICE;
    }
    return BLOCK_CACHE_OK;
}

static unsigned char *block_cache_entry_storage(
    const struct block_cache *cache,
    size_t slot)
{
    return cache->storage + slot * cache->device->block_size;
}

static size_t block_cache_find(
    const struct block_cache *cache,
    uint64_t block_index)
{
    size_t slot;

    for (slot = 0U; slot < cache->capacity; ++slot) {
        const struct block_cache_entry *entry = &cache->entries[slot];

        if (entry->valid && entry->block_index == block_index) {
            return slot;
        }
    }
    return SIZE_MAX;
}

static size_t block_cache_find_invalid(const struct block_cache *cache) {
    size_t slot;

    for (slot = 0U; slot < cache->capacity; ++slot) {
        if (!cache->entries[slot].valid) {
            return slot;
        }
    }
    return SIZE_MAX;
}

static size_t block_cache_find_oldest(const struct block_cache *cache) {
    size_t slot;
    size_t oldest = SIZE_MAX;
    uint64_t oldest_sequence = UINT64_MAX;

    for (slot = 0U; slot < cache->capacity; ++slot) {
        const struct block_cache_entry *entry = &cache->entries[slot];

        if (entry->valid && entry->sequence < oldest_sequence) {
            oldest = slot;
            oldest_sequence = entry->sequence;
        }
    }
    return oldest;
}

static uint64_t block_cache_offset(
    const struct block_cache *cache,
    uint64_t block_index)
{
    return block_index * cache->device->block_size;
}

enum block_cache_status block_cache_init(
    struct block_cache *cache,
    struct block_device *device,
    struct block_cache_entry *entries,
    size_t capacity,
    void *storage,
    size_t storage_size)
{
    uint64_t required_size;
    size_t slot;

    if (cache == NULL || device == NULL) {
        return BLOCK_CACHE_NULL;
    }
    if (block_device_validate(device) != BLOCK_DEVICE_OK) {
        return BLOCK_CACHE_BAD_DEVICE;
    }
    if (capacity < BLOCK_CACHE_CAPACITY_MIN ||
        capacity > BLOCK_CACHE_CAPACITY_MAX) {
        return BLOCK_CACHE_BAD_CAPACITY;
    }
    if (entries == NULL || storage == NULL) {
        return BLOCK_CACHE_BAD_STORAGE;
    }

    required_size = (uint64_t)capacity * device->block_size;
    if (storage_size != (size_t)required_size) {
        return BLOCK_CACHE_BAD_STORAGE;
    }

    for (slot = 0U; slot < capacity; ++slot) {
        entries[slot].block_index = 0U;
        entries[slot].sequence = 0U;
        entries[slot].valid = false;
        entries[slot].dirty = false;
    }

    cache->device = device;
    cache->entries = entries;
    cache->storage = (unsigned char *)storage;
    cache->capacity = capacity;
    cache->next_sequence = 0U;
    cache->initialized = true;
    return BLOCK_CACHE_OK;
}

enum block_cache_status block_cache_read(
    struct block_cache *cache,
    uint64_t block_index,
    void *buffer)
{
    enum block_cache_status cache_status;
    enum block_device_status io_status;
    size_t slot;
    size_t invalid_slot;
    struct block_cache_entry *entry;
    uint64_t offset;

    cache_status = block_cache_check(cache);
    if (cache_status != BLOCK_CACHE_OK) {
        return cache_status;
    }
    if (buffer == NULL) {
        return BLOCK_CACHE_NULL;
    }
    if (block_index >= cache->device->block_count) {
        return BLOCK_CACHE_BAD_BLOCK_INDEX;
    }

    slot = block_cache_find(cache, block_index);
    if (slot != SIZE_MAX) {
        memcpy(buffer, block_cache_entry_storage(cache, slot),
               cache->device->block_size);
        return BLOCK_CACHE_OK;
    }

    invalid_slot = block_cache_find_invalid(cache);
    if (invalid_slot == SIZE_MAX) {
        slot = block_cache_find_oldest(cache);
        if (slot == SIZE_MAX) {
            return BLOCK_CACHE_IO_ERROR;
        }
        entry = &cache->entries[slot];
        if (entry->dirty) {
            offset = block_cache_offset(cache, entry->block_index);
            io_status = block_device_write(
                cache->device, offset,
                block_cache_entry_storage(cache, slot),
                cache->device->block_size);
            if (io_status != BLOCK_DEVICE_OK) {
                return BLOCK_CACHE_IO_ERROR;
            }
        }
        entry->valid = false;
        entry->dirty = false;
    } else {
        slot = invalid_slot;
    }

    offset = block_cache_offset(cache, block_index);
    io_status = block_device_read(
        cache->device, offset,
        block_cache_entry_storage(cache, slot),
        cache->device->block_size);
    if (io_status != BLOCK_DEVICE_OK) {
        return BLOCK_CACHE_IO_ERROR;
    }

    entry = &cache->entries[slot];
    entry->block_index = block_index;
    entry->sequence = cache->next_sequence++;
    entry->valid = true;
    entry->dirty = false;
    memcpy(buffer, block_cache_entry_storage(cache, slot),
           cache->device->block_size);
    return BLOCK_CACHE_OK;
}

enum block_cache_status block_cache_write(
    struct block_cache *cache,
    uint64_t block_index,
    const void *buffer)
{
    enum block_cache_status cache_status;
    enum block_device_status io_status;
    size_t slot;
    size_t invalid_slot;
    struct block_cache_entry *entry;
    uint64_t offset;

    cache_status = block_cache_check(cache);
    if (cache_status != BLOCK_CACHE_OK) {
        return cache_status;
    }
    if (buffer == NULL) {
        return BLOCK_CACHE_NULL;
    }
    if (block_index >= cache->device->block_count) {
        return BLOCK_CACHE_BAD_BLOCK_INDEX;
    }

    slot = block_cache_find(cache, block_index);
    if (slot != SIZE_MAX) {
        memcpy(block_cache_entry_storage(cache, slot), buffer,
               cache->device->block_size);
        cache->entries[slot].valid = true;
        cache->entries[slot].dirty = true;
        return BLOCK_CACHE_OK;
    }

    invalid_slot = block_cache_find_invalid(cache);
    if (invalid_slot != SIZE_MAX) {
        slot = invalid_slot;
    } else {
        slot = block_cache_find_oldest(cache);
        if (slot == SIZE_MAX) {
            return BLOCK_CACHE_IO_ERROR;
        }
        entry = &cache->entries[slot];
        if (entry->dirty) {
            offset = block_cache_offset(cache, entry->block_index);
            io_status = block_device_write(
                cache->device, offset,
                block_cache_entry_storage(cache, slot),
                cache->device->block_size);
            if (io_status != BLOCK_DEVICE_OK) {
                return BLOCK_CACHE_IO_ERROR;
            }
        }
        entry->valid = false;
        entry->dirty = false;
    }

    memcpy(block_cache_entry_storage(cache, slot), buffer,
           cache->device->block_size);
    entry = &cache->entries[slot];
    entry->block_index = block_index;
    entry->sequence = cache->next_sequence++;
    entry->valid = true;
    entry->dirty = true;
    return BLOCK_CACHE_OK;
}

enum block_cache_status block_cache_flush(
    struct block_cache *cache,
    uint64_t block_index)
{
    enum block_cache_status cache_status;
    enum block_device_status io_status;
    size_t slot;
    struct block_cache_entry *entry;

    cache_status = block_cache_check(cache);
    if (cache_status != BLOCK_CACHE_OK) {
        return cache_status;
    }
    if (block_index >= cache->device->block_count) {
        return BLOCK_CACHE_BAD_BLOCK_INDEX;
    }

    slot = block_cache_find(cache, block_index);
    if (slot == SIZE_MAX) {
        return BLOCK_CACHE_OK;
    }
    entry = &cache->entries[slot];
    if (!entry->dirty) {
        return BLOCK_CACHE_OK;
    }

    io_status = block_device_write(
        cache->device, block_cache_offset(cache, block_index),
        block_cache_entry_storage(cache, slot),
        cache->device->block_size);
    if (io_status != BLOCK_DEVICE_OK) {
        return BLOCK_CACHE_IO_ERROR;
    }
    entry->dirty = false;
    return BLOCK_CACHE_OK;
}

enum block_cache_status block_cache_flush_all(
    struct block_cache *cache)
{
    enum block_cache_status cache_status;
    size_t slot;

    cache_status = block_cache_check(cache);
    if (cache_status != BLOCK_CACHE_OK) {
        return cache_status;
    }

    for (slot = 0U; slot < cache->capacity; ++slot) {
        const struct block_cache_entry *entry = &cache->entries[slot];

        if (entry->valid && entry->dirty) {
            cache_status = block_cache_flush(cache, entry->block_index);
            if (cache_status != BLOCK_CACHE_OK) {
                return cache_status;
            }
        }
    }
    return BLOCK_CACHE_OK;
}

const char *block_cache_status_string(enum block_cache_status status) {
    switch (status) {
    case BLOCK_CACHE_OK:
        return "BLOCK_CACHE_OK";
    case BLOCK_CACHE_NULL:
        return "BLOCK_CACHE_NULL";
    case BLOCK_CACHE_BAD_DEVICE:
        return "BLOCK_CACHE_BAD_DEVICE";
    case BLOCK_CACHE_BAD_CAPACITY:
        return "BLOCK_CACHE_BAD_CAPACITY";
    case BLOCK_CACHE_BAD_STORAGE:
        return "BLOCK_CACHE_BAD_STORAGE";
    case BLOCK_CACHE_BAD_BLOCK_INDEX:
        return "BLOCK_CACHE_BAD_BLOCK_INDEX";
    case BLOCK_CACHE_IO_ERROR:
        return "BLOCK_CACHE_IO_ERROR";
    default:
        return "BLOCK_CACHE_UNKNOWN";
    }
}
