#include "https_peer_throttle.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#if defined(HTTPS_PEER_THROTTLE_HOST_TEST)

static bool https_peer_throttle_config_valid(
    const struct https_peer_throttle_table *table)
{
    return table->capacity > 0U &&
           table->capacity <= HTTPS_PEER_THROTTLE_MAX_PEERS &&
           table->failure_limit >= HTTPS_PEER_THROTTLE_FAILURE_LIMIT_MIN &&
           table->failure_limit <= HTTPS_PEER_THROTTLE_FAILURE_LIMIT_MAX &&
           table->window_ticks > 0U &&
           table->window_ticks <= HTTPS_PEER_THROTTLE_WINDOW_MAX &&
           table->cooldown_ticks > 0U &&
           table->cooldown_ticks <= HTTPS_PEER_THROTTLE_COOLDOWN_MAX;
}

static enum https_peer_throttle_status https_peer_throttle_table_valid(
    const struct https_peer_throttle_table *table)
{
    uint32_t index;

    if (table == NULL) {
        return HTTPS_PEER_THROTTLE_NULL;
    }
    if (table->version != HTTPS_PEER_THROTTLE_VERSION ||
        !https_peer_throttle_config_valid(table) ||
        table->count > table->capacity) {
        return table->version != HTTPS_PEER_THROTTLE_VERSION
            ? HTTPS_PEER_THROTTLE_INVALID_VERSION
            : !https_peer_throttle_config_valid(table)
                ? (table->capacity == 0U ||
                   table->capacity > HTTPS_PEER_THROTTLE_MAX_PEERS
                       ? HTTPS_PEER_THROTTLE_BAD_CAPACITY
                       : table->failure_limit == 0U ||
                             table->failure_limit >
                                 HTTPS_PEER_THROTTLE_FAILURE_LIMIT_MAX
                             ? HTTPS_PEER_THROTTLE_INVALID_FAILURE_LIMIT
                             : table->window_ticks == 0U ||
                                   table->window_ticks >
                                       HTTPS_PEER_THROTTLE_WINDOW_MAX
                                   ? HTTPS_PEER_THROTTLE_INVALID_WINDOW
                                   : HTTPS_PEER_THROTTLE_INVALID_COOLDOWN)
                : HTTPS_PEER_THROTTLE_INVALID_STATE;
    }

    for (index = 0U; index < table->count; ++index) {
        const struct https_peer_throttle_bucket *bucket =
            &table->buckets[index];

        if (bucket->version != HTTPS_PEER_THROTTLE_VERSION ||
            bucket->peer_id < HTTPS_PEER_THROTTLE_PEER_ID_MIN ||
            bucket->peer_id > HTTPS_PEER_THROTTLE_PEER_ID_MAX ||
            bucket->first_failure_tick > bucket->last_failure_tick ||
            bucket->first_failure_tick >
                UINT64_MAX - (uint64_t)table->window_ticks ||
            bucket->last_failure_tick >
                UINT64_MAX - (uint64_t)table->cooldown_ticks) {
            return HTTPS_PEER_THROTTLE_INVALID_STATE;
        }
        if (bucket->state == HTTPS_PEER_THROTTLE_BUCKET_ACTIVE) {
            if (bucket->failure_count == 0U ||
                bucket->failure_count > table->failure_limit ||
                bucket->blocked_until_tick != 0U) {
                return HTTPS_PEER_THROTTLE_INVALID_STATE;
            }
        } else if (bucket->state == HTTPS_PEER_THROTTLE_BUCKET_BLOCKED) {
            if (bucket->failure_count != table->failure_limit ||
                bucket->blocked_until_tick == 0U ||
                bucket->blocked_until_tick <= bucket->last_failure_tick) {
                return HTTPS_PEER_THROTTLE_INVALID_STATE;
            }
        } else {
            return HTTPS_PEER_THROTTLE_INVALID_STATE;
        }
    }

    for (index = table->count;
         index < HTTPS_PEER_THROTTLE_MAX_PEERS;
         ++index) {
        const struct https_peer_throttle_bucket *bucket =
            &table->buckets[index];

        if (bucket->version != HTTPS_PEER_THROTTLE_VERSION ||
            bucket->peer_id != 0U ||
            bucket->failure_count != 0U ||
            bucket->first_failure_tick != 0U ||
            bucket->last_failure_tick != 0U ||
            bucket->blocked_until_tick != 0U ||
            bucket->state != HTTPS_PEER_THROTTLE_BUCKET_FREE) {
            return HTTPS_PEER_THROTTLE_INVALID_STATE;
        }
    }
    return HTTPS_PEER_THROTTLE_OK;
}

static int https_peer_throttle_find(
    const struct https_peer_throttle_table *table,
    uint32_t peer_id)
{
    uint32_t index;

    for (index = 0U; index < table->count; ++index) {
        if (table->buckets[index].peer_id == peer_id) {
            return (int)index;
        }
    }
    return -1;
}

static uint32_t https_peer_throttle_oldest_index(
    const struct https_peer_throttle_table *table)
{
    uint32_t oldest = 0U;
    uint32_t index;

    for (index = 1U; index < table->count; ++index) {
        if (table->buckets[index].last_failure_tick <
            table->buckets[oldest].last_failure_tick) {
            oldest = index;
        }
    }
    return oldest;
}

static void https_peer_throttle_clear_bucket(
    struct https_peer_throttle_bucket *bucket)
{
    memset(bucket, 0, sizeof(*bucket));
    bucket->version = HTTPS_PEER_THROTTLE_VERSION;
    bucket->state = HTTPS_PEER_THROTTLE_BUCKET_FREE;
}

static void https_peer_throttle_start_bucket(
    struct https_peer_throttle_bucket *bucket,
    uint32_t peer_id,
    uint64_t current_tick)
{
    https_peer_throttle_clear_bucket(bucket);
    bucket->peer_id = peer_id;
    bucket->failure_count = 1U;
    bucket->first_failure_tick = current_tick;
    bucket->last_failure_tick = current_tick;
    bucket->state = HTTPS_PEER_THROTTLE_BUCKET_ACTIVE;
}

static void https_peer_throttle_remove_index(
    struct https_peer_throttle_table *table,
    uint32_t index)
{
    uint32_t trailing = table->count - index - 1U;

    if (trailing != 0U) {
        memmove(&table->buckets[index],
                &table->buckets[index + 1U],
                sizeof(table->buckets[0]) * trailing);
    }
    https_peer_throttle_clear_bucket(
        &table->buckets[table->count - 1U]);
    --table->count;
}

static bool https_peer_throttle_window_expired(
    const struct https_peer_throttle_table *table,
    const struct https_peer_throttle_bucket *bucket,
    uint64_t current_tick)
{
    uint64_t window_end = bucket->first_failure_tick +
                          (uint64_t)table->window_ticks;

    return current_tick > window_end;
}

enum https_peer_throttle_status https_peer_throttle_init(
    struct https_peer_throttle_table *table,
    uint32_t capacity,
    uint32_t failure_limit,
    uint32_t window_ticks,
    uint32_t cooldown_ticks)
{
    uint32_t index;

    if (table == NULL) {
        return HTTPS_PEER_THROTTLE_NULL;
    }
    if (capacity == 0U ||
        capacity > HTTPS_PEER_THROTTLE_MAX_PEERS) {
        return HTTPS_PEER_THROTTLE_BAD_CAPACITY;
    }
    if (failure_limit < HTTPS_PEER_THROTTLE_FAILURE_LIMIT_MIN ||
        failure_limit > HTTPS_PEER_THROTTLE_FAILURE_LIMIT_MAX) {
        return HTTPS_PEER_THROTTLE_INVALID_FAILURE_LIMIT;
    }
    if (window_ticks == 0U ||
        window_ticks > HTTPS_PEER_THROTTLE_WINDOW_MAX) {
        return HTTPS_PEER_THROTTLE_INVALID_WINDOW;
    }
    if (cooldown_ticks == 0U ||
        cooldown_ticks > HTTPS_PEER_THROTTLE_COOLDOWN_MAX) {
        return HTTPS_PEER_THROTTLE_INVALID_COOLDOWN;
    }

    memset(table, 0, sizeof(*table));
    table->version = HTTPS_PEER_THROTTLE_VERSION;
    table->capacity = capacity;
    table->failure_limit = failure_limit;
    table->window_ticks = window_ticks;
    table->cooldown_ticks = cooldown_ticks;
    for (index = 0U;
         index < HTTPS_PEER_THROTTLE_MAX_PEERS;
         ++index) {
        https_peer_throttle_clear_bucket(&table->buckets[index]);
    }
    return HTTPS_PEER_THROTTLE_OK;
}

enum https_peer_throttle_status https_peer_throttle_record_failure(
    struct https_peer_throttle_table *table,
    uint32_t peer_id,
    uint64_t current_tick,
    uint64_t *out_blocked_until_tick)
{
    enum https_peer_throttle_status status;
    struct https_peer_throttle_bucket *bucket;
    int index;

    if (table == NULL || out_blocked_until_tick == NULL) {
        return HTTPS_PEER_THROTTLE_NULL;
    }
    status = https_peer_throttle_table_valid(table);
    if (status != HTTPS_PEER_THROTTLE_OK) {
        return status;
    }
    if (peer_id < HTTPS_PEER_THROTTLE_PEER_ID_MIN ||
        peer_id > HTTPS_PEER_THROTTLE_PEER_ID_MAX) {
        return HTTPS_PEER_THROTTLE_INVALID_PEER_ID;
    }
    if (current_tick > UINT64_MAX - (uint64_t)table->window_ticks ||
        current_tick > UINT64_MAX - (uint64_t)table->cooldown_ticks) {
        return HTTPS_PEER_THROTTLE_INVALID_TICK;
    }

    index = https_peer_throttle_find(table, peer_id);
    if (index < 0) {
        if (table->count >= table->capacity) {
            https_peer_throttle_remove_index(
                table, https_peer_throttle_oldest_index(table));
        }
        bucket = &table->buckets[table->count];
        https_peer_throttle_start_bucket(
            bucket, peer_id, current_tick);
        ++table->count;
        *out_blocked_until_tick = 0U;
        return HTTPS_PEER_THROTTLE_OK;
    }

    bucket = &table->buckets[index];
    if (bucket->state == HTTPS_PEER_THROTTLE_BUCKET_BLOCKED) {
        if (current_tick < bucket->blocked_until_tick) {
            *out_blocked_until_tick = bucket->blocked_until_tick;
            return HTTPS_PEER_THROTTLE_PEER_BLOCKED;
        }
        https_peer_throttle_start_bucket(
            bucket, peer_id, current_tick);
        *out_blocked_until_tick = 0U;
        return HTTPS_PEER_THROTTLE_OK;
    }

    if (https_peer_throttle_window_expired(
            table, bucket, current_tick)) {
        https_peer_throttle_start_bucket(
            bucket, peer_id, current_tick);
        *out_blocked_until_tick = 0U;
        return HTTPS_PEER_THROTTLE_OK;
    }

    ++bucket->failure_count;
    bucket->last_failure_tick = current_tick;
    if (bucket->failure_count >= table->failure_limit) {
        bucket->state = HTTPS_PEER_THROTTLE_BUCKET_BLOCKED;
        bucket->blocked_until_tick =
            current_tick + (uint64_t)table->cooldown_ticks;
        *out_blocked_until_tick = bucket->blocked_until_tick;
        return HTTPS_PEER_THROTTLE_PEER_BLOCKED;
    }
    *out_blocked_until_tick = 0U;
    return HTTPS_PEER_THROTTLE_OK;
}

enum https_peer_throttle_status https_peer_throttle_check(
    const struct https_peer_throttle_table *table,
    uint32_t peer_id,
    uint64_t current_tick)
{
    const struct https_peer_throttle_bucket *bucket;
    enum https_peer_throttle_status status;
    int index;

    if (table == NULL) {
        return HTTPS_PEER_THROTTLE_NULL;
    }
    status = https_peer_throttle_table_valid(table);
    if (status != HTTPS_PEER_THROTTLE_OK) {
        return status;
    }
    if (peer_id < HTTPS_PEER_THROTTLE_PEER_ID_MIN ||
        peer_id > HTTPS_PEER_THROTTLE_PEER_ID_MAX) {
        return HTTPS_PEER_THROTTLE_INVALID_PEER_ID;
    }

    index = https_peer_throttle_find(table, peer_id);
    if (index < 0) {
        return HTTPS_PEER_THROTTLE_OK;
    }
    bucket = &table->buckets[index];
    if (bucket->state == HTTPS_PEER_THROTTLE_BUCKET_BLOCKED &&
        current_tick < bucket->blocked_until_tick) {
        return HTTPS_PEER_THROTTLE_PEER_BLOCKED;
    }
    return HTTPS_PEER_THROTTLE_OK;
}

enum https_peer_throttle_status https_peer_throttle_reset(
    struct https_peer_throttle_table *table,
    uint32_t peer_id)
{
    enum https_peer_throttle_status status;
    int index;

    if (table == NULL) {
        return HTTPS_PEER_THROTTLE_NULL;
    }
    status = https_peer_throttle_table_valid(table);
    if (status != HTTPS_PEER_THROTTLE_OK) {
        return status;
    }
    if (peer_id < HTTPS_PEER_THROTTLE_PEER_ID_MIN ||
        peer_id > HTTPS_PEER_THROTTLE_PEER_ID_MAX) {
        return HTTPS_PEER_THROTTLE_INVALID_PEER_ID;
    }

    index = https_peer_throttle_find(table, peer_id);
    if (index < 0) {
        return HTTPS_PEER_THROTTLE_PEER_NOT_FOUND;
    }
    https_peer_throttle_remove_index(table, (uint32_t)index);
    return HTTPS_PEER_THROTTLE_OK;
}

enum https_peer_throttle_status https_peer_throttle_local_allowed(
    enum https_peer_throttle_local_operation operation)
{
    switch (operation) {
    case HTTPS_PEER_THROTTLE_LOCAL_MANAGEMENT:
    case HTTPS_PEER_THROTTLE_LOCAL_HEALTH:
    case HTTPS_PEER_THROTTLE_LOCAL_SUPERVISION:
        return HTTPS_PEER_THROTTLE_OK;
    default:
        return HTTPS_PEER_THROTTLE_INVALID_LOCAL_OPERATION;
    }
}

const char *https_peer_throttle_status_string(
    enum https_peer_throttle_status status)
{
    switch (status) {
    case HTTPS_PEER_THROTTLE_OK:
        return "HTTPS_PEER_THROTTLE_OK";
    case HTTPS_PEER_THROTTLE_NULL:
        return "HTTPS_PEER_THROTTLE_NULL";
    case HTTPS_PEER_THROTTLE_BAD_CAPACITY:
        return "HTTPS_PEER_THROTTLE_BAD_CAPACITY";
    case HTTPS_PEER_THROTTLE_INVALID_VERSION:
        return "HTTPS_PEER_THROTTLE_INVALID_VERSION";
    case HTTPS_PEER_THROTTLE_INVALID_PEER_ID:
        return "HTTPS_PEER_THROTTLE_INVALID_PEER_ID";
    case HTTPS_PEER_THROTTLE_INVALID_FAILURE_LIMIT:
        return "HTTPS_PEER_THROTTLE_INVALID_FAILURE_LIMIT";
    case HTTPS_PEER_THROTTLE_INVALID_WINDOW:
        return "HTTPS_PEER_THROTTLE_INVALID_WINDOW";
    case HTTPS_PEER_THROTTLE_INVALID_COOLDOWN:
        return "HTTPS_PEER_THROTTLE_INVALID_COOLDOWN";
    case HTTPS_PEER_THROTTLE_INVALID_TICK:
        return "HTTPS_PEER_THROTTLE_INVALID_TICK";
    case HTTPS_PEER_THROTTLE_PEER_BLOCKED:
        return "HTTPS_PEER_THROTTLE_PEER_BLOCKED";
    case HTTPS_PEER_THROTTLE_PEER_NOT_FOUND:
        return "HTTPS_PEER_THROTTLE_PEER_NOT_FOUND";
    case HTTPS_PEER_THROTTLE_INVALID_STATE:
        return "HTTPS_PEER_THROTTLE_INVALID_STATE";
    case HTTPS_PEER_THROTTLE_INVALID_LOCAL_OPERATION:
        return "HTTPS_PEER_THROTTLE_INVALID_LOCAL_OPERATION";
    default:
        return NULL;
    }
}

#else

typedef int https_peer_throttle_translation_unit_guard;

#endif
