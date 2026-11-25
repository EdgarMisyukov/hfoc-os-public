#include "packet_pool.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

static bool packet_pool_valid(
    const struct packet_pool_descriptor *pool)
{
    return pool != NULL &&
           pool->published &&
           pool->version == PACKET_POOL_VERSION &&
           pool->count >= PACKET_POOL_COUNT_MIN &&
           pool->count <= PACKET_POOL_COUNT_MAX &&
           pool->capacity >= PACKET_POOL_CAPACITY_MIN &&
           pool->capacity <= PACKET_POOL_CAPACITY_MAX &&
           pool->headroom <= PACKET_POOL_HEADROOM_MAX &&
           pool->length >= PACKET_POOL_LENGTH_MIN &&
           pool->length <= pool->capacity &&
           pool->storage != NULL;
}

enum packet_pool_status packet_pool_init(
    struct packet_pool_descriptor *pool,
    void *storage,
    size_t storage_size,
    uint32_t count,
    uint32_t capacity,
    uint32_t headroom,
    uint32_t length)
{
    uint64_t unit_size;
    uint64_t required_size;
    uint32_t slot;

    if (pool == NULL || storage == NULL) {
        return PACKET_POOL_NULL;
    }
    if (pool->published) {
        return PACKET_POOL_BAD_STATE;
    }

    unit_size = (uint64_t)headroom + (uint64_t)capacity;
    if (unit_size != 0U && count > UINT64_MAX / unit_size) {
        return PACKET_POOL_OVERFLOW;
    }
    required_size = unit_size * (uint64_t)count;
    if (required_size > PACKET_POOL_STORAGE_SIZE_MAX) {
        return PACKET_POOL_OVERFLOW;
    }

    if (count < PACKET_POOL_COUNT_MIN ||
        count > PACKET_POOL_COUNT_MAX) {
        return PACKET_POOL_BAD_COUNT;
    }
    if (capacity < PACKET_POOL_CAPACITY_MIN ||
        capacity > PACKET_POOL_CAPACITY_MAX) {
        return PACKET_POOL_BAD_CAPACITY;
    }
    if (headroom > PACKET_POOL_HEADROOM_MAX) {
        return PACKET_POOL_BAD_HEADROOM;
    }
    if (length < PACKET_POOL_LENGTH_MIN ||
        length > capacity) {
        return PACKET_POOL_BAD_LENGTH;
    }
    if (storage_size != (size_t)required_size) {
        return PACKET_POOL_BAD_STORAGE;
    }

    memset(pool, 0, sizeof(*pool));
    for (slot = 0U; slot < count; ++slot) {
        pool->generations[slot] = 1U;
        pool->owners[slot] = PACKET_POOL_OWNER_FREE;
        pool->directions[slot] = PACKET_POOL_DIRECTION_NONE;
    }
    pool->version = PACKET_POOL_VERSION;
    pool->count = count;
    pool->capacity = capacity;
    pool->headroom = headroom;
    pool->length = length;
    pool->storage_size = (size_t)required_size;
    pool->storage = (uint8_t *)storage;
    pool->free_count = count;
    pool->reserved_count = 0U;
    pool->active_count = 0U;
    pool->published = true;
    return PACKET_POOL_OK;
}

bool packet_pool_is_published(
    const struct packet_pool_descriptor *pool)
{
    return packet_pool_valid(pool);
}

uint32_t packet_pool_count(
    const struct packet_pool_descriptor *pool)
{
    return packet_pool_valid(pool) ? pool->count : 0U;
}

uint32_t packet_pool_capacity(
    const struct packet_pool_descriptor *pool)
{
    return packet_pool_valid(pool) ? pool->capacity : 0U;
}

uint32_t packet_pool_headroom(
    const struct packet_pool_descriptor *pool)
{
    return packet_pool_valid(pool) ? pool->headroom : 0U;
}

uint32_t packet_pool_length(
    const struct packet_pool_descriptor *pool)
{
    return packet_pool_valid(pool) ? pool->length : 0U;
}

size_t packet_pool_storage_size(
    const struct packet_pool_descriptor *pool)
{
    return packet_pool_valid(pool) ? pool->storage_size : 0U;
}

enum packet_pool_status packet_pool_slot(
    const struct packet_pool_descriptor *pool,
    uint32_t slot,
    struct packet_pool_slot_descriptor *descriptor)
{
    if (pool == NULL || descriptor == NULL) {
        return PACKET_POOL_NULL;
    }
    if (!packet_pool_valid(pool)) {
        return PACKET_POOL_BAD_STATE;
    }
    if (slot >= pool->count) {
        return PACKET_POOL_BAD_SLOT;
    }

    descriptor->slot = slot;
    descriptor->generation = pool->generations[slot];
    descriptor->owner = (enum packet_pool_owner)pool->owners[slot];
    descriptor->direction =
        (enum packet_pool_direction)pool->directions[slot];
    return PACKET_POOL_OK;
}

static uint32_t packet_pool_find_free(
    const struct packet_pool_descriptor *pool)
{
    uint32_t slot;

    for (slot = 0U; slot < pool->count; ++slot) {
        if (pool->owners[slot] == PACKET_POOL_OWNER_FREE) {
            return slot;
        }
    }
    return pool->count;
}

static uint32_t packet_pool_next_generation(uint32_t generation)
{
    return generation == UINT32_MAX ? 1U : generation + 1U;
}

enum packet_pool_status packet_pool_reserve(
    struct packet_pool_descriptor *pool,
    enum packet_pool_direction direction,
    struct packet_pool_slot_descriptor *descriptor)
{
    uint32_t slot;

    if (pool == NULL || descriptor == NULL) {
        return PACKET_POOL_NULL;
    }
    if (!packet_pool_valid(pool)) {
        return PACKET_POOL_BAD_STATE;
    }
    if (direction != PACKET_POOL_DIRECTION_RX &&
        direction != PACKET_POOL_DIRECTION_TX) {
        return PACKET_POOL_BAD_DIRECTION;
    }

    slot = packet_pool_find_free(pool);
    if (slot == pool->count) {
        return PACKET_POOL_EXHAUSTED;
    }

    descriptor->slot = slot;
    descriptor->generation = pool->generations[slot];
    descriptor->owner = PACKET_POOL_OWNER_RESERVED;
    descriptor->direction = direction;
    pool->owners[slot] = PACKET_POOL_OWNER_RESERVED;
    pool->directions[slot] = direction;
    --pool->free_count;
    ++pool->reserved_count;
    return PACKET_POOL_OK;
}

enum packet_pool_status packet_pool_commit(
    struct packet_pool_descriptor *pool,
    struct packet_pool_slot_descriptor *descriptor)
{
    uint32_t slot;

    if (pool == NULL || descriptor == NULL) {
        return PACKET_POOL_NULL;
    }
    if (!packet_pool_valid(pool)) {
        return PACKET_POOL_BAD_STATE;
    }
    if (descriptor->slot >= pool->count) {
        return PACKET_POOL_BAD_SLOT;
    }
    if (pool->generations[descriptor->slot] !=
        descriptor->generation) {
        return PACKET_POOL_STALE_DESCRIPTOR;
    }
    if (pool->owners[descriptor->slot] !=
            PACKET_POOL_OWNER_RESERVED ||
        descriptor->owner != PACKET_POOL_OWNER_RESERVED ||
        pool->directions[descriptor->slot] !=
            descriptor->direction ||
        descriptor->direction == PACKET_POOL_DIRECTION_NONE) {
        return PACKET_POOL_WRONG_OWNER;
    }

    slot = descriptor->slot;
    descriptor->owner = PACKET_POOL_OWNER_ACTIVE;
    pool->owners[slot] = PACKET_POOL_OWNER_ACTIVE;
    --pool->reserved_count;
    ++pool->active_count;
    return PACKET_POOL_OK;
}

enum packet_pool_status packet_pool_transfer(
    struct packet_pool_descriptor *pool,
    struct packet_pool_slot_descriptor *descriptor,
    enum packet_pool_direction direction)
{
    uint32_t slot;

    if (pool == NULL || descriptor == NULL) {
        return PACKET_POOL_NULL;
    }
    if (!packet_pool_valid(pool)) {
        return PACKET_POOL_BAD_STATE;
    }
    if (descriptor->slot >= pool->count) {
        return PACKET_POOL_BAD_SLOT;
    }
    if (pool->generations[descriptor->slot] !=
        descriptor->generation) {
        return PACKET_POOL_STALE_DESCRIPTOR;
    }
    if (pool->owners[descriptor->slot] !=
            PACKET_POOL_OWNER_ACTIVE ||
        descriptor->owner != PACKET_POOL_OWNER_ACTIVE ||
        pool->directions[descriptor->slot] !=
            descriptor->direction) {
        return PACKET_POOL_WRONG_OWNER;
    }
    if (direction != PACKET_POOL_DIRECTION_RX &&
        direction != PACKET_POOL_DIRECTION_TX) {
        return PACKET_POOL_BAD_DIRECTION;
    }
    if (direction == descriptor->direction) {
        return PACKET_POOL_BAD_DIRECTION;
    }

    slot = descriptor->slot;
    descriptor->direction = direction;
    pool->directions[slot] = direction;
    return PACKET_POOL_OK;
}

enum packet_pool_status packet_pool_release(
    struct packet_pool_descriptor *pool,
    const struct packet_pool_slot_descriptor *descriptor)
{
    uint32_t slot;

    if (pool == NULL || descriptor == NULL) {
        return PACKET_POOL_NULL;
    }
    if (!packet_pool_valid(pool)) {
        return PACKET_POOL_BAD_STATE;
    }
    if (descriptor->slot >= pool->count) {
        return PACKET_POOL_BAD_SLOT;
    }
    if (pool->owners[descriptor->slot] == PACKET_POOL_OWNER_FREE) {
        return PACKET_POOL_DOUBLE_FREE;
    }
    if (pool->generations[descriptor->slot] !=
        descriptor->generation) {
        return PACKET_POOL_STALE_DESCRIPTOR;
    }
    if (pool->owners[descriptor->slot] != descriptor->owner ||
        pool->directions[descriptor->slot] !=
            descriptor->direction ||
        (descriptor->owner != PACKET_POOL_OWNER_RESERVED &&
         descriptor->owner != PACKET_POOL_OWNER_ACTIVE)) {
        return PACKET_POOL_WRONG_OWNER;
    }

    slot = descriptor->slot;
    if (descriptor->owner == PACKET_POOL_OWNER_RESERVED) {
        --pool->reserved_count;
    } else {
        --pool->active_count;
    }
    pool->owners[slot] = PACKET_POOL_OWNER_FREE;
    pool->directions[slot] = PACKET_POOL_DIRECTION_NONE;
    pool->generations[slot] =
        packet_pool_next_generation(pool->generations[slot]);
    ++pool->free_count;
    return PACKET_POOL_OK;
}

uint32_t packet_pool_free_count(
    const struct packet_pool_descriptor *pool)
{
    return packet_pool_valid(pool) ? pool->free_count : 0U;
}

uint32_t packet_pool_reserved_count(
    const struct packet_pool_descriptor *pool)
{
    return packet_pool_valid(pool) ? pool->reserved_count : 0U;
}

uint32_t packet_pool_active_count(
    const struct packet_pool_descriptor *pool)
{
    return packet_pool_valid(pool) ? pool->active_count : 0U;
}

const char *packet_pool_status_string(
    enum packet_pool_status status)
{
    switch (status) {
    case PACKET_POOL_OK:
        return "PACKET_POOL_OK";
    case PACKET_POOL_NULL:
        return "PACKET_POOL_NULL";
    case PACKET_POOL_BAD_STATE:
        return "PACKET_POOL_BAD_STATE";
    case PACKET_POOL_BAD_COUNT:
        return "PACKET_POOL_BAD_COUNT";
    case PACKET_POOL_BAD_CAPACITY:
        return "PACKET_POOL_BAD_CAPACITY";
    case PACKET_POOL_BAD_HEADROOM:
        return "PACKET_POOL_BAD_HEADROOM";
    case PACKET_POOL_BAD_LENGTH:
        return "PACKET_POOL_BAD_LENGTH";
    case PACKET_POOL_BAD_STORAGE:
        return "PACKET_POOL_BAD_STORAGE";
    case PACKET_POOL_OVERFLOW:
        return "PACKET_POOL_OVERFLOW";
    case PACKET_POOL_BAD_SLOT:
        return "PACKET_POOL_BAD_SLOT";
    case PACKET_POOL_EXHAUSTED:
        return "PACKET_POOL_EXHAUSTED";
    case PACKET_POOL_BAD_DIRECTION:
        return "PACKET_POOL_BAD_DIRECTION";
    case PACKET_POOL_STALE_DESCRIPTOR:
        return "PACKET_POOL_STALE_DESCRIPTOR";
    case PACKET_POOL_WRONG_OWNER:
        return "PACKET_POOL_WRONG_OWNER";
    case PACKET_POOL_DOUBLE_FREE:
        return "PACKET_POOL_DOUBLE_FREE";
    default:
        return NULL;
    }
}
