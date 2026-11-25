#include "hfoc_lwip_tcp_pool.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#if defined(HFOC_LWIP_TCP_POOL_HOST_TEST) || \
    defined(HFOC_TCP_LISTENER_QEMU_TEST)

static bool hfoc_lwip_tcp_pool_valid(
    const struct hfoc_lwip_tcp_pool *pool)
{
    uint32_t free_state_count;
    uint32_t reserved_state_count;
    uint32_t published_state_count;
    uint32_t index;

    if (pool == NULL ||
        !pool->published ||
        pool->version != HFOC_LWIP_TCP_POOL_VERSION ||
        pool->free_count > HFOC_LWIP_TCP_POOL_SLOT_COUNT ||
        pool->reserved_count > HFOC_LWIP_TCP_POOL_SLOT_COUNT ||
        pool->published_count > HFOC_LWIP_TCP_POOL_SLOT_COUNT ||
        pool->free_count + pool->reserved_count +
            pool->published_count !=
            HFOC_LWIP_TCP_POOL_SLOT_COUNT) {
        return false;
    }

    free_state_count = 0U;
    reserved_state_count = 0U;
    published_state_count = 0U;
    for (index = 0U; index < HFOC_LWIP_TCP_POOL_SLOT_COUNT;
         ++index) {
        const struct hfoc_lwip_tcp_pool_slot *slot =
            &pool->slots[index];

        if (slot->generation < HFOC_LWIP_TCP_POOL_GENERATION_MIN ||
            slot->generation > HFOC_LWIP_TCP_POOL_GENERATION_MAX) {
            return false;
        }
        switch (slot->state) {
        case HFOC_LWIP_TCP_POOL_STATE_FREE:
            ++free_state_count;
            if (slot->connection.version != 0U) {
                return false;
            }
            break;
        case HFOC_LWIP_TCP_POOL_STATE_RESERVED:
            ++reserved_state_count;
            if (slot->connection.version != 0U) {
                return false;
            }
            break;
        case HFOC_LWIP_TCP_POOL_STATE_PUBLISHED:
            ++published_state_count;
            if (slot->connection.version !=
                    HFOC_LWIP_TCP_CONN_VERSION ||
                slot->connection.generation != slot->generation ||
                !slot->connection.published) {
                return false;
            }
            break;
        default:
            return false;
        }
    }

    return free_state_count == pool->free_count &&
           reserved_state_count == pool->reserved_count &&
           published_state_count == pool->published_count;
}

static enum hfoc_lwip_tcp_pool_status
hfoc_lwip_tcp_pool_validate_token(
    const struct hfoc_lwip_tcp_pool *pool,
    const struct hfoc_lwip_tcp_pool_token *token,
    const struct hfoc_lwip_tcp_pool_slot **slot_out,
    enum hfoc_lwip_tcp_pool_state expected_state)
{
    const struct hfoc_lwip_tcp_pool_slot *slot;

    if (pool == NULL || token == NULL || slot_out == NULL) {
        return HFOC_LWIP_TCP_POOL_NULL;
    }
    if (!hfoc_lwip_tcp_pool_valid(pool)) {
        return HFOC_LWIP_TCP_POOL_BAD_STATE;
    }
    if (token->index >= HFOC_LWIP_TCP_POOL_SLOT_COUNT) {
        return HFOC_LWIP_TCP_POOL_BAD_INDEX;
    }
    slot = &pool->slots[token->index];
    if (slot->state == HFOC_LWIP_TCP_POOL_STATE_FREE) {
        return HFOC_LWIP_TCP_POOL_WRONG_STATE;
    }
    if (slot->generation != token->generation) {
        return HFOC_LWIP_TCP_POOL_STALE_TOKEN;
    }
    if (slot->state != expected_state) {
        return HFOC_LWIP_TCP_POOL_WRONG_STATE;
    }

    *slot_out = slot;
    return HFOC_LWIP_TCP_POOL_OK;
}

static uint32_t hfoc_lwip_tcp_pool_next_generation(uint32_t generation)
{
    return generation == HFOC_LWIP_TCP_POOL_GENERATION_MAX
        ? 0U
        : generation + 1U;
}

enum hfoc_lwip_tcp_pool_status hfoc_lwip_tcp_pool_init(
    struct hfoc_lwip_tcp_pool *pool)
{
    uint32_t index;

    if (pool == NULL) {
        return HFOC_LWIP_TCP_POOL_NULL;
    }
    if (pool->published) {
        return HFOC_LWIP_TCP_POOL_BAD_STATE;
    }

    memset(pool, 0, sizeof(*pool));
    for (index = 0U; index < HFOC_LWIP_TCP_POOL_SLOT_COUNT;
         ++index) {
        pool->slots[index].state = HFOC_LWIP_TCP_POOL_STATE_FREE;
        pool->slots[index].generation =
            HFOC_LWIP_TCP_POOL_GENERATION_MIN;
    }
    pool->version = HFOC_LWIP_TCP_POOL_VERSION;
    pool->free_count = HFOC_LWIP_TCP_POOL_SLOT_COUNT;
    pool->published = true;
    return HFOC_LWIP_TCP_POOL_OK;
}

enum hfoc_lwip_tcp_pool_status hfoc_lwip_tcp_pool_reserve(
    struct hfoc_lwip_tcp_pool *pool,
    struct hfoc_lwip_tcp_pool_token *token)
{
    uint32_t index;

    if (pool == NULL || token == NULL) {
        return HFOC_LWIP_TCP_POOL_NULL;
    }
    if (!hfoc_lwip_tcp_pool_valid(pool)) {
        return HFOC_LWIP_TCP_POOL_BAD_STATE;
    }
    if (pool->free_count == 0U) {
        return HFOC_LWIP_TCP_POOL_EXHAUSTED;
    }

    for (index = 0U; index < HFOC_LWIP_TCP_POOL_SLOT_COUNT;
         ++index) {
        if (pool->slots[index].state ==
            HFOC_LWIP_TCP_POOL_STATE_FREE) {
            token->index = index;
            token->generation = pool->slots[index].generation;
            pool->slots[index].state =
                HFOC_LWIP_TCP_POOL_STATE_RESERVED;
            --pool->free_count;
            ++pool->reserved_count;
            return HFOC_LWIP_TCP_POOL_OK;
        }
    }

    return HFOC_LWIP_TCP_POOL_BAD_STATE;
}

enum hfoc_lwip_tcp_pool_status hfoc_lwip_tcp_pool_publish(
    struct hfoc_lwip_tcp_pool *pool,
    const struct hfoc_lwip_tcp_pool_token *token,
    struct tcp_pcb *pcb,
    const struct hfoc_lwip_tcp_conn_callbacks *callbacks)
{
    const struct hfoc_lwip_tcp_pool_slot *slot;
    enum hfoc_lwip_tcp_pool_status token_status;
    enum hfoc_lwip_tcp_conn_status conn_status;

    token_status = hfoc_lwip_tcp_pool_validate_token(
        pool, token, &slot,
        HFOC_LWIP_TCP_POOL_STATE_RESERVED);
    if (token_status != HFOC_LWIP_TCP_POOL_OK) {
        return token_status;
    }
    if (pcb == NULL || callbacks == NULL) {
        return HFOC_LWIP_TCP_POOL_NULL;
    }

    conn_status = hfoc_lwip_tcp_conn_accept(
        &pool->slots[token->index].connection,
        pcb,
        token->generation,
        callbacks);
    if (conn_status != HFOC_LWIP_TCP_CONN_OK) {
        memset(&pool->slots[token->index].connection, 0,
               sizeof(pool->slots[token->index].connection));
        return HFOC_LWIP_TCP_POOL_CONN_FAILED;
    }

    pool->slots[token->index].state =
        HFOC_LWIP_TCP_POOL_STATE_PUBLISHED;
    --pool->reserved_count;
    ++pool->published_count;
    return HFOC_LWIP_TCP_POOL_OK;
}

enum hfoc_lwip_tcp_pool_status hfoc_lwip_tcp_pool_close(
    struct hfoc_lwip_tcp_pool *pool,
    const struct hfoc_lwip_tcp_pool_token *token)
{
    const struct hfoc_lwip_tcp_pool_slot *slot;
    enum hfoc_lwip_tcp_pool_status token_status;
    enum hfoc_lwip_tcp_conn_status conn_status;

    token_status = hfoc_lwip_tcp_pool_validate_token(
        pool, token, &slot,
        HFOC_LWIP_TCP_POOL_STATE_PUBLISHED);
    if (token_status != HFOC_LWIP_TCP_POOL_OK) {
        return token_status;
    }

    conn_status = hfoc_lwip_tcp_conn_close(
        &pool->slots[token->index].connection);
    if (conn_status == HFOC_LWIP_TCP_CONN_ALREADY_CLOSED) {
        return HFOC_LWIP_TCP_POOL_ALREADY_CLOSED;
    }
    if (conn_status != HFOC_LWIP_TCP_CONN_OK) {
        return conn_status == HFOC_LWIP_TCP_CONN_CLOSE_FAILED
            ? HFOC_LWIP_TCP_POOL_CLOSE_FAILED
            : HFOC_LWIP_TCP_POOL_CONN_FAILED;
    }
    return HFOC_LWIP_TCP_POOL_OK;
}

enum hfoc_lwip_tcp_pool_status hfoc_lwip_tcp_pool_abort(
    struct hfoc_lwip_tcp_pool *pool,
    const struct hfoc_lwip_tcp_pool_token *token)
{
    const struct hfoc_lwip_tcp_pool_slot *slot;
    enum hfoc_lwip_tcp_pool_status token_status;
    enum hfoc_lwip_tcp_conn_status conn_status;

    token_status = hfoc_lwip_tcp_pool_validate_token(
        pool, token, &slot,
        HFOC_LWIP_TCP_POOL_STATE_PUBLISHED);
    if (token_status != HFOC_LWIP_TCP_POOL_OK) {
        return token_status;
    }

    conn_status = hfoc_lwip_tcp_conn_abort(
        &pool->slots[token->index].connection);
    if (conn_status != HFOC_LWIP_TCP_CONN_OK) {
        return HFOC_LWIP_TCP_POOL_CONN_FAILED;
    }
    return HFOC_LWIP_TCP_POOL_OK;
}

enum hfoc_lwip_tcp_pool_status hfoc_lwip_tcp_pool_release(
    struct hfoc_lwip_tcp_pool *pool,
    const struct hfoc_lwip_tcp_pool_token *token)
{
    struct hfoc_lwip_tcp_pool_slot *slot;
    enum hfoc_lwip_tcp_pool_state old_state;
    uint32_t next_generation;

    if (pool == NULL || token == NULL) {
        return HFOC_LWIP_TCP_POOL_NULL;
    }
    if (!hfoc_lwip_tcp_pool_valid(pool)) {
        return HFOC_LWIP_TCP_POOL_BAD_STATE;
    }
    if (token->index >= HFOC_LWIP_TCP_POOL_SLOT_COUNT) {
        return HFOC_LWIP_TCP_POOL_BAD_INDEX;
    }
    slot = &pool->slots[token->index];
    if (slot->state == HFOC_LWIP_TCP_POOL_STATE_FREE) {
        return HFOC_LWIP_TCP_POOL_DOUBLE_FREE;
    }
    if (slot->generation != token->generation) {
        return HFOC_LWIP_TCP_POOL_STALE_TOKEN;
    }
    if (slot->state == HFOC_LWIP_TCP_POOL_STATE_PUBLISHED &&
        slot->connection.state != HFOC_LWIP_TCP_CONN_STATE_CLOSED &&
        slot->connection.state != HFOC_LWIP_TCP_CONN_STATE_ABORTED) {
        return HFOC_LWIP_TCP_POOL_LIVE_CONNECTION;
    }

    next_generation =
        hfoc_lwip_tcp_pool_next_generation(slot->generation);
    if (next_generation == 0U) {
        return HFOC_LWIP_TCP_POOL_BAD_STATE;
    }

    old_state = slot->state;
    memset(&slot->connection, 0, sizeof(slot->connection));
    slot->state = HFOC_LWIP_TCP_POOL_STATE_FREE;
    slot->generation = next_generation;
    ++pool->free_count;
    if (old_state == HFOC_LWIP_TCP_POOL_STATE_RESERVED) {
        --pool->reserved_count;
    } else {
        --pool->published_count;
    }
    return HFOC_LWIP_TCP_POOL_OK;
}

bool hfoc_lwip_tcp_pool_published(
    const struct hfoc_lwip_tcp_pool *pool)
{
    return hfoc_lwip_tcp_pool_valid(pool);
}

uint32_t hfoc_lwip_tcp_pool_free_count(
    const struct hfoc_lwip_tcp_pool *pool)
{
    return hfoc_lwip_tcp_pool_valid(pool) ? pool->free_count : 0U;
}

uint32_t hfoc_lwip_tcp_pool_reserved_count(
    const struct hfoc_lwip_tcp_pool *pool)
{
    return hfoc_lwip_tcp_pool_valid(pool)
        ? pool->reserved_count
        : 0U;
}

uint32_t hfoc_lwip_tcp_pool_published_count(
    const struct hfoc_lwip_tcp_pool *pool)
{
    return hfoc_lwip_tcp_pool_valid(pool)
        ? pool->published_count
        : 0U;
}

uint32_t hfoc_lwip_tcp_pool_generation(
    const struct hfoc_lwip_tcp_pool *pool,
    uint32_t index)
{
    if (!hfoc_lwip_tcp_pool_valid(pool) ||
        index >= HFOC_LWIP_TCP_POOL_SLOT_COUNT) {
        return 0U;
    }
    return pool->slots[index].generation;
}

enum hfoc_lwip_tcp_pool_state hfoc_lwip_tcp_pool_state(
    const struct hfoc_lwip_tcp_pool *pool,
    uint32_t index)
{
    if (!hfoc_lwip_tcp_pool_valid(pool) ||
        index >= HFOC_LWIP_TCP_POOL_SLOT_COUNT) {
        return HFOC_LWIP_TCP_POOL_STATE_FREE;
    }
    return pool->slots[index].state;
}

const struct hfoc_lwip_tcp_connection *hfoc_lwip_tcp_pool_connection(
    const struct hfoc_lwip_tcp_pool *pool,
    uint32_t index)
{
    if (!hfoc_lwip_tcp_pool_valid(pool) ||
        index >= HFOC_LWIP_TCP_POOL_SLOT_COUNT) {
        return NULL;
    }
    return &pool->slots[index].connection;
}

const char *hfoc_lwip_tcp_pool_state_string(
    enum hfoc_lwip_tcp_pool_state state)
{
    switch (state) {
    case HFOC_LWIP_TCP_POOL_STATE_FREE:
        return "HFOC_LWIP_TCP_POOL_STATE_FREE";
    case HFOC_LWIP_TCP_POOL_STATE_RESERVED:
        return "HFOC_LWIP_TCP_POOL_STATE_RESERVED";
    case HFOC_LWIP_TCP_POOL_STATE_PUBLISHED:
        return "HFOC_LWIP_TCP_POOL_STATE_PUBLISHED";
    default:
        return NULL;
    }
}

const char *hfoc_lwip_tcp_pool_status_string(
    enum hfoc_lwip_tcp_pool_status status)
{
    switch (status) {
    case HFOC_LWIP_TCP_POOL_OK:
        return "HFOC_LWIP_TCP_POOL_OK";
    case HFOC_LWIP_TCP_POOL_NULL:
        return "HFOC_LWIP_TCP_POOL_NULL";
    case HFOC_LWIP_TCP_POOL_BAD_STATE:
        return "HFOC_LWIP_TCP_POOL_BAD_STATE";
    case HFOC_LWIP_TCP_POOL_EXHAUSTED:
        return "HFOC_LWIP_TCP_POOL_EXHAUSTED";
    case HFOC_LWIP_TCP_POOL_BAD_INDEX:
        return "HFOC_LWIP_TCP_POOL_BAD_INDEX";
    case HFOC_LWIP_TCP_POOL_STALE_TOKEN:
        return "HFOC_LWIP_TCP_POOL_STALE_TOKEN";
    case HFOC_LWIP_TCP_POOL_WRONG_STATE:
        return "HFOC_LWIP_TCP_POOL_WRONG_STATE";
    case HFOC_LWIP_TCP_POOL_DOUBLE_FREE:
        return "HFOC_LWIP_TCP_POOL_DOUBLE_FREE";
    case HFOC_LWIP_TCP_POOL_CONN_FAILED:
        return "HFOC_LWIP_TCP_POOL_CONN_FAILED";
    case HFOC_LWIP_TCP_POOL_CLOSE_FAILED:
        return "HFOC_LWIP_TCP_POOL_CLOSE_FAILED";
    case HFOC_LWIP_TCP_POOL_ALREADY_CLOSED:
        return "HFOC_LWIP_TCP_POOL_ALREADY_CLOSED";
    case HFOC_LWIP_TCP_POOL_LIVE_CONNECTION:
        return "HFOC_LWIP_TCP_POOL_LIVE_CONNECTION";
    default:
        return NULL;
    }
}

#else

typedef int hfoc_lwip_tcp_pool_not_compiled;

#endif
