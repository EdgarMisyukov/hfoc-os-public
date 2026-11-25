#include "virtio_descriptor_ownership.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

static bool virtio_descriptor_ownership_pool_valid(
    const struct virtio_descriptor_ownership *pool)
{
    uint32_t free_state_count;
    uint32_t reserved_state_count;
    uint32_t published_state_count;
    uint32_t index;

    if (pool == NULL ||
        !pool->published ||
        pool->version != VIRTIO_DESCRIPTOR_OWNERSHIP_VERSION ||
        pool->capacity < VIRTIO_DESCRIPTOR_OWNERSHIP_CAPACITY_MIN ||
        pool->capacity > VIRTIO_DESCRIPTOR_OWNERSHIP_CAPACITY_MAX ||
        pool->free_count > pool->capacity ||
        pool->reserved_count > pool->capacity ||
        pool->published_count > pool->capacity ||
        pool->free_count + pool->reserved_count + pool->published_count !=
            pool->capacity ||
        pool->freelist_top != pool->free_count) {
        return false;
    }

    free_state_count = 0U;
    reserved_state_count = 0U;
    published_state_count = 0U;
    for (index = 0U; index < pool->capacity; ++index) {
        if (pool->generations[index] == 0U) {
            return false;
        }
        switch ((enum virtio_descriptor_state)pool->states[index]) {
        case VIRTIO_DESCRIPTOR_STATE_FREE:
            ++free_state_count;
            break;
        case VIRTIO_DESCRIPTOR_STATE_RESERVED:
            ++reserved_state_count;
            break;
        case VIRTIO_DESCRIPTOR_STATE_PUBLISHED:
            ++published_state_count;
            break;
        default:
            return false;
        }
    }

    return free_state_count == pool->free_count &&
           reserved_state_count == pool->reserved_count &&
           published_state_count == pool->published_count;
}

static uint32_t virtio_descriptor_ownership_next_generation(
    uint32_t generation)
{
    return generation == UINT32_MAX ? 1U : generation + 1U;
}

enum virtio_descriptor_ownership_status
virtio_descriptor_ownership_init(
    struct virtio_descriptor_ownership *pool,
    uint32_t capacity)
{
    uint32_t index;

    if (pool == NULL) {
        return VIRTIO_DESCRIPTOR_OWNERSHIP_NULL;
    }
    if (pool->published) {
        return VIRTIO_DESCRIPTOR_OWNERSHIP_BAD_STATE;
    }
    if (capacity < VIRTIO_DESCRIPTOR_OWNERSHIP_CAPACITY_MIN ||
        capacity > VIRTIO_DESCRIPTOR_OWNERSHIP_CAPACITY_MAX) {
        return VIRTIO_DESCRIPTOR_OWNERSHIP_BAD_CAPACITY;
    }

    memset(pool, 0, sizeof(*pool));
    for (index = 0U; index < capacity; ++index) {
        pool->freelist[index] = index;
        pool->states[index] = VIRTIO_DESCRIPTOR_STATE_FREE;
        pool->generations[index] = 1U;
    }
    pool->version = VIRTIO_DESCRIPTOR_OWNERSHIP_VERSION;
    pool->capacity = capacity;
    pool->free_count = capacity;
    pool->freelist_top = capacity;
    pool->published = true;
    return VIRTIO_DESCRIPTOR_OWNERSHIP_OK;
}

enum virtio_descriptor_ownership_status
virtio_descriptor_ownership_reserve(
    struct virtio_descriptor_ownership *pool,
    struct virtio_descriptor_token *token)
{
    uint32_t index;

    if (pool == NULL || token == NULL) {
        return VIRTIO_DESCRIPTOR_OWNERSHIP_NULL;
    }
    if (!virtio_descriptor_ownership_pool_valid(pool)) {
        return VIRTIO_DESCRIPTOR_OWNERSHIP_BAD_STATE;
    }
    if (pool->freelist_top == 0U) {
        return VIRTIO_DESCRIPTOR_OWNERSHIP_EXHAUSTED;
    }

    --pool->freelist_top;
    index = pool->freelist[pool->freelist_top];
    if (index >= pool->capacity ||
        pool->states[index] != VIRTIO_DESCRIPTOR_STATE_FREE ||
        pool->generations[index] == 0U) {
        return VIRTIO_DESCRIPTOR_OWNERSHIP_BAD_STATE;
    }

    token->index = index;
    token->generation = pool->generations[index];
    pool->states[index] = VIRTIO_DESCRIPTOR_STATE_RESERVED;
    --pool->free_count;
    ++pool->reserved_count;
    return VIRTIO_DESCRIPTOR_OWNERSHIP_OK;
}

enum virtio_descriptor_ownership_status
virtio_descriptor_ownership_publish(
    struct virtio_descriptor_ownership *pool,
    const struct virtio_descriptor_token *token)
{
    if (pool == NULL || token == NULL) {
        return VIRTIO_DESCRIPTOR_OWNERSHIP_NULL;
    }
    if (!virtio_descriptor_ownership_pool_valid(pool)) {
        return VIRTIO_DESCRIPTOR_OWNERSHIP_BAD_STATE;
    }
    if (token->index >= pool->capacity) {
        return VIRTIO_DESCRIPTOR_OWNERSHIP_BAD_DESCRIPTOR;
    }
    if (pool->states[token->index] == VIRTIO_DESCRIPTOR_STATE_FREE) {
        return VIRTIO_DESCRIPTOR_OWNERSHIP_WRONG_STATE;
    }
    if (pool->generations[token->index] != token->generation) {
        return VIRTIO_DESCRIPTOR_OWNERSHIP_STALE_DESCRIPTOR;
    }
    if (pool->states[token->index] != VIRTIO_DESCRIPTOR_STATE_RESERVED) {
        return VIRTIO_DESCRIPTOR_OWNERSHIP_WRONG_STATE;
    }

    pool->states[token->index] = VIRTIO_DESCRIPTOR_STATE_PUBLISHED;
    --pool->reserved_count;
    ++pool->published_count;
    return VIRTIO_DESCRIPTOR_OWNERSHIP_OK;
}

static enum virtio_descriptor_ownership_status
virtio_descriptor_ownership_reclaim(
    struct virtio_descriptor_ownership *pool,
    const struct virtio_descriptor_token *token,
    enum virtio_descriptor_state expected_state)
{
    uint32_t index;

    if (pool == NULL || token == NULL) {
        return VIRTIO_DESCRIPTOR_OWNERSHIP_NULL;
    }
    if (!virtio_descriptor_ownership_pool_valid(pool)) {
        return VIRTIO_DESCRIPTOR_OWNERSHIP_BAD_STATE;
    }
    index = token->index;
    if (index >= pool->capacity) {
        return VIRTIO_DESCRIPTOR_OWNERSHIP_BAD_DESCRIPTOR;
    }
    if (pool->states[index] == VIRTIO_DESCRIPTOR_STATE_FREE) {
        return VIRTIO_DESCRIPTOR_OWNERSHIP_DOUBLE_FREE;
    }
    if (pool->generations[index] != token->generation) {
        return VIRTIO_DESCRIPTOR_OWNERSHIP_STALE_DESCRIPTOR;
    }
    if (pool->states[index] != expected_state) {
        return VIRTIO_DESCRIPTOR_OWNERSHIP_WRONG_STATE;
    }
    if (pool->freelist_top >= pool->capacity) {
        return VIRTIO_DESCRIPTOR_OWNERSHIP_BAD_STATE;
    }

    pool->states[index] = VIRTIO_DESCRIPTOR_STATE_FREE;
    pool->generations[index] =
        virtio_descriptor_ownership_next_generation(
            pool->generations[index]);
    pool->freelist[pool->freelist_top] = index;
    ++pool->freelist_top;
    ++pool->free_count;
    if (expected_state == VIRTIO_DESCRIPTOR_STATE_RESERVED) {
        --pool->reserved_count;
    } else {
        --pool->published_count;
    }
    return VIRTIO_DESCRIPTOR_OWNERSHIP_OK;
}

enum virtio_descriptor_ownership_status
virtio_descriptor_ownership_rollback(
    struct virtio_descriptor_ownership *pool,
    const struct virtio_descriptor_token *token)
{
    return virtio_descriptor_ownership_reclaim(
        pool, token, VIRTIO_DESCRIPTOR_STATE_RESERVED);
}

enum virtio_descriptor_ownership_status
virtio_descriptor_ownership_consume(
    struct virtio_descriptor_ownership *pool,
    const struct virtio_descriptor_token *token)
{
    return virtio_descriptor_ownership_reclaim(
        pool, token, VIRTIO_DESCRIPTOR_STATE_PUBLISHED);
}

bool virtio_descriptor_ownership_published(
    const struct virtio_descriptor_ownership *pool)
{
    return virtio_descriptor_ownership_pool_valid(pool);
}

uint32_t virtio_descriptor_ownership_free_count(
    const struct virtio_descriptor_ownership *pool)
{
    return virtio_descriptor_ownership_pool_valid(pool) ?
               pool->free_count : 0U;
}

uint32_t virtio_descriptor_ownership_reserved_count(
    const struct virtio_descriptor_ownership *pool)
{
    return virtio_descriptor_ownership_pool_valid(pool) ?
               pool->reserved_count : 0U;
}

uint32_t virtio_descriptor_ownership_published_count(
    const struct virtio_descriptor_ownership *pool)
{
    return virtio_descriptor_ownership_pool_valid(pool) ?
               pool->published_count : 0U;
}

const char *virtio_descriptor_ownership_status_string(
    enum virtio_descriptor_ownership_status status)
{
    switch (status) {
    case VIRTIO_DESCRIPTOR_OWNERSHIP_OK:
        return "VIRTIO_DESCRIPTOR_OWNERSHIP_OK";
    case VIRTIO_DESCRIPTOR_OWNERSHIP_NULL:
        return "VIRTIO_DESCRIPTOR_OWNERSHIP_NULL";
    case VIRTIO_DESCRIPTOR_OWNERSHIP_BAD_STATE:
        return "VIRTIO_DESCRIPTOR_OWNERSHIP_BAD_STATE";
    case VIRTIO_DESCRIPTOR_OWNERSHIP_BAD_CAPACITY:
        return "VIRTIO_DESCRIPTOR_OWNERSHIP_BAD_CAPACITY";
    case VIRTIO_DESCRIPTOR_OWNERSHIP_EXHAUSTED:
        return "VIRTIO_DESCRIPTOR_OWNERSHIP_EXHAUSTED";
    case VIRTIO_DESCRIPTOR_OWNERSHIP_BAD_DESCRIPTOR:
        return "VIRTIO_DESCRIPTOR_OWNERSHIP_BAD_DESCRIPTOR";
    case VIRTIO_DESCRIPTOR_OWNERSHIP_STALE_DESCRIPTOR:
        return "VIRTIO_DESCRIPTOR_OWNERSHIP_STALE_DESCRIPTOR";
    case VIRTIO_DESCRIPTOR_OWNERSHIP_WRONG_STATE:
        return "VIRTIO_DESCRIPTOR_OWNERSHIP_WRONG_STATE";
    case VIRTIO_DESCRIPTOR_OWNERSHIP_DOUBLE_FREE:
        return "VIRTIO_DESCRIPTOR_OWNERSHIP_DOUBLE_FREE";
    default:
        return NULL;
    }
}
