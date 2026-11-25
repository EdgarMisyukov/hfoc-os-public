#include "packet_buffer.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

static bool packet_buffer_pool_valid(
    const struct packet_buffer_pool *pool)
{
    return pool != NULL &&
           pool->published &&
           pool->version == PACKET_BUFFER_POOL_VERSION &&
           pool->capacity >= PACKET_BUFFER_CAPACITY_MIN &&
           pool->capacity <= PACKET_BUFFER_CAPACITY_MAX &&
           pool->payload_size >= PACKET_BUFFER_PAYLOAD_MIN &&
           pool->payload_size <= PACKET_BUFFER_PAYLOAD_MAX &&
           pool->storage != NULL &&
           pool->free_count <= pool->capacity;
}

static uint32_t packet_buffer_find_free(
    const struct packet_buffer_pool *pool)
{
    uint32_t slot;

    for (slot = 0U; slot < pool->capacity; ++slot) {
        if (pool->owners[slot] == PACKET_BUFFER_OWNER_FREE) {
            return slot;
        }
    }
    return pool->capacity;
}

static uint32_t packet_buffer_next_generation(uint32_t generation)
{
    return generation == UINT32_MAX ? 1U : generation + 1U;
}

enum packet_buffer_status packet_buffer_pool_init(
    struct packet_buffer_pool *pool,
    void *storage,
    size_t storage_size,
    uint32_t capacity,
    uint32_t payload_size)
{
    uint64_t required_size;
    uint32_t slot;

    if (pool == NULL || storage == NULL) {
        return PACKET_BUFFER_NULL;
    }
    if (pool->published) {
        return PACKET_BUFFER_BAD_STATE;
    }
    if (capacity < PACKET_BUFFER_CAPACITY_MIN ||
        capacity > PACKET_BUFFER_CAPACITY_MAX) {
        return PACKET_BUFFER_BAD_CAPACITY;
    }
    if (payload_size < PACKET_BUFFER_PAYLOAD_MIN ||
        payload_size > PACKET_BUFFER_PAYLOAD_MAX) {
        return PACKET_BUFFER_BAD_PAYLOAD;
    }

    required_size = (uint64_t)capacity * payload_size;
    if (storage_size != (size_t)required_size) {
        return PACKET_BUFFER_BAD_STORAGE;
    }

    memset(pool, 0, sizeof(*pool));
    for (slot = 0U; slot < capacity; ++slot) {
        pool->generations[slot] = 1U;
        pool->owners[slot] = PACKET_BUFFER_OWNER_FREE;
    }
    pool->version = PACKET_BUFFER_POOL_VERSION;
    pool->capacity = capacity;
    pool->payload_size = payload_size;
    pool->storage = (uint8_t *)storage;
    pool->free_count = capacity;
    pool->published = true;
    return PACKET_BUFFER_OK;
}

static enum packet_buffer_status packet_buffer_acquire(
    struct packet_buffer_pool *pool,
    struct packet_buffer_descriptor *descriptor,
    uint8_t owner)
{
    uint32_t slot;

    if (pool == NULL || descriptor == NULL) {
        return PACKET_BUFFER_NULL;
    }
    if (!packet_buffer_pool_valid(pool)) {
        return PACKET_BUFFER_BAD_STATE;
    }

    slot = packet_buffer_find_free(pool);
    if (slot == pool->capacity) {
        return PACKET_BUFFER_EXHAUSTED;
    }

    descriptor->slot = slot;
    descriptor->generation = pool->generations[slot];
    pool->owners[slot] = owner;
    --pool->free_count;
    return PACKET_BUFFER_OK;
}

enum packet_buffer_status packet_buffer_acquire_rx(
    struct packet_buffer_pool *pool,
    struct packet_buffer_descriptor *descriptor)
{
    return packet_buffer_acquire(
        pool, descriptor, PACKET_BUFFER_OWNER_RX);
}

enum packet_buffer_status packet_buffer_acquire_tx(
    struct packet_buffer_pool *pool,
    struct packet_buffer_descriptor *descriptor)
{
    return packet_buffer_acquire(
        pool, descriptor, PACKET_BUFFER_OWNER_TX);
}

enum packet_buffer_status packet_buffer_handoff_tx(
    struct packet_buffer_pool *pool,
    const struct packet_buffer_descriptor *descriptor)
{
    if (pool == NULL || descriptor == NULL) {
        return PACKET_BUFFER_NULL;
    }
    if (!packet_buffer_pool_valid(pool)) {
        return PACKET_BUFFER_BAD_STATE;
    }
    if (descriptor->slot >= pool->capacity) {
        return PACKET_BUFFER_BAD_DESCRIPTOR;
    }
    if (pool->owners[descriptor->slot] == PACKET_BUFFER_OWNER_FREE) {
        return PACKET_BUFFER_WRONG_OWNER;
    }
    if (pool->generations[descriptor->slot] !=
        descriptor->generation) {
        return PACKET_BUFFER_STALE_DESCRIPTOR;
    }
    if (pool->owners[descriptor->slot] != PACKET_BUFFER_OWNER_RX) {
        return PACKET_BUFFER_WRONG_OWNER;
    }

    pool->owners[descriptor->slot] = PACKET_BUFFER_OWNER_TX;
    return PACKET_BUFFER_OK;
}

enum packet_buffer_status packet_buffer_release(
    struct packet_buffer_pool *pool,
    const struct packet_buffer_descriptor *descriptor)
{
    if (pool == NULL || descriptor == NULL) {
        return PACKET_BUFFER_NULL;
    }
    if (!packet_buffer_pool_valid(pool)) {
        return PACKET_BUFFER_BAD_STATE;
    }
    if (descriptor->slot >= pool->capacity) {
        return PACKET_BUFFER_BAD_DESCRIPTOR;
    }
    if (pool->owners[descriptor->slot] == PACKET_BUFFER_OWNER_FREE) {
        return PACKET_BUFFER_DOUBLE_FREE;
    }
    if (pool->generations[descriptor->slot] !=
        descriptor->generation) {
        return PACKET_BUFFER_STALE_DESCRIPTOR;
    }

    pool->owners[descriptor->slot] = PACKET_BUFFER_OWNER_FREE;
    pool->generations[descriptor->slot] =
        packet_buffer_next_generation(
            pool->generations[descriptor->slot]);
    ++pool->free_count;
    return PACKET_BUFFER_OK;
}

void *packet_buffer_data(
    struct packet_buffer_pool *pool,
    const struct packet_buffer_descriptor *descriptor)
{
    if (pool == NULL || descriptor == NULL) {
        return NULL;
    }
    if (!packet_buffer_pool_valid(pool)) {
        return NULL;
    }
    if (descriptor->slot >= pool->capacity) {
        return NULL;
    }
    if (pool->owners[descriptor->slot] == PACKET_BUFFER_OWNER_FREE ||
        pool->generations[descriptor->slot] !=
            descriptor->generation) {
        return NULL;
    }
    return pool->storage +
           (size_t)descriptor->slot * pool->payload_size;
}

uint32_t packet_buffer_free_count(
    const struct packet_buffer_pool *pool)
{
    return packet_buffer_pool_valid(pool) ? pool->free_count : 0U;
}

uint32_t packet_buffer_capacity(
    const struct packet_buffer_pool *pool)
{
    return packet_buffer_pool_valid(pool) ? pool->capacity : 0U;
}

const char *packet_buffer_status_string(
    enum packet_buffer_status status)
{
    switch (status) {
    case PACKET_BUFFER_OK:
        return "PACKET_BUFFER_OK";
    case PACKET_BUFFER_NULL:
        return "PACKET_BUFFER_NULL";
    case PACKET_BUFFER_BAD_STATE:
        return "PACKET_BUFFER_BAD_STATE";
    case PACKET_BUFFER_BAD_CAPACITY:
        return "PACKET_BUFFER_BAD_CAPACITY";
    case PACKET_BUFFER_BAD_PAYLOAD:
        return "PACKET_BUFFER_BAD_PAYLOAD";
    case PACKET_BUFFER_BAD_STORAGE:
        return "PACKET_BUFFER_BAD_STORAGE";
    case PACKET_BUFFER_EXHAUSTED:
        return "PACKET_BUFFER_EXHAUSTED";
    case PACKET_BUFFER_BAD_DESCRIPTOR:
        return "PACKET_BUFFER_BAD_DESCRIPTOR";
    case PACKET_BUFFER_STALE_DESCRIPTOR:
        return "PACKET_BUFFER_STALE_DESCRIPTOR";
    case PACKET_BUFFER_WRONG_OWNER:
        return "PACKET_BUFFER_WRONG_OWNER";
    case PACKET_BUFFER_DOUBLE_FREE:
        return "PACKET_BUFFER_DOUBLE_FREE";
    default:
        return NULL;
    }
}
