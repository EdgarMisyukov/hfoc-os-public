#include "packet_pressure.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

static bool packet_pressure_pool_counts_valid(
    const struct packet_pool_descriptor *pool)
{
    uint32_t count;
    uint32_t free_count;
    uint32_t reserved_count;
    uint32_t active_count;

    count = packet_pool_count(pool);
    free_count = packet_pool_free_count(pool);
    reserved_count = packet_pool_reserved_count(pool);
    active_count = packet_pool_active_count(pool);

    return free_count <= count &&
           reserved_count <= count &&
           active_count <= count &&
           free_count + reserved_count + active_count == count;
}

static bool packet_pressure_pool_valid(
    const struct packet_pool_descriptor *pool)
{
    return packet_pool_is_published(pool) &&
           packet_pressure_pool_counts_valid(pool);
}

static bool packet_pressure_snapshot_valid(
    const struct packet_pressure_snapshot *snapshot,
    const struct packet_pool_descriptor *pool)
{
    uint32_t pool_count;

    if (snapshot == NULL ||
        snapshot->version != PACKET_PRESSURE_VERSION ||
        snapshot->generation == 0U) {
        return false;
    }

    pool_count = packet_pool_count(pool);
    if (snapshot->pool_count != pool_count ||
        snapshot->pool_free_count > pool_count ||
        snapshot->pool_in_use_count > pool_count ||
        snapshot->pool_free_count + snapshot->pool_in_use_count !=
            pool_count) {
        return false;
    }
    return snapshot->pool_high_water_count >=
               snapshot->pool_in_use_count &&
           snapshot->pool_high_water_count <= pool_count;
}

static bool packet_pressure_valid(
    const struct packet_pressure *pressure)
{
    return pressure != NULL &&
           pressure->published &&
           pressure->version == PACKET_PRESSURE_VERSION &&
           packet_pressure_pool_valid(pressure->pool) &&
           packet_pressure_snapshot_valid(
               &pressure->published_snapshot, pressure->pool);
}

static enum packet_pressure_status packet_pressure_common_check(
    const struct packet_pressure *pressure)
{
    if (pressure == NULL) {
        return PACKET_PRESSURE_NULL;
    }
    if (!pressure->published) {
        return PACKET_PRESSURE_UNPUBLISHED;
    }
    if (pressure->version != PACKET_PRESSURE_VERSION) {
        return PACKET_PRESSURE_INVALID_VERSION;
    }
    if (!packet_pressure_pool_valid(pressure->pool)) {
        return PACKET_PRESSURE_BAD_POOL;
    }
    if (!packet_pressure_snapshot_valid(
            &pressure->published_snapshot, pressure->pool)) {
        return PACKET_PRESSURE_BAD_COUNTERS;
    }
    return PACKET_PRESSURE_OK;
}

enum packet_pressure_status packet_pressure_init(
    struct packet_pressure *pressure,
    const struct packet_pool_descriptor *pool)
{
    struct packet_pressure_snapshot snapshot;

    if (pressure == NULL) {
        return PACKET_PRESSURE_NULL;
    }
    if (pool == NULL) {
        return PACKET_PRESSURE_BAD_POOL;
    }
    if (pressure->published) {
        return PACKET_PRESSURE_ALREADY_PUBLISHED;
    }
    if (!packet_pressure_pool_valid(pool)) {
        return PACKET_PRESSURE_BAD_POOL;
    }

    memset(&snapshot, 0, sizeof(snapshot));
    snapshot.version = PACKET_PRESSURE_VERSION;
    snapshot.generation = 1U;
    snapshot.pool_count = packet_pool_count(pool);
    snapshot.pool_free_count = packet_pool_free_count(pool);
    snapshot.pool_in_use_count =
        snapshot.pool_count - snapshot.pool_free_count;
    snapshot.pool_high_water_count = snapshot.pool_in_use_count;

    memset(pressure, 0, sizeof(*pressure));
    pressure->version = PACKET_PRESSURE_VERSION;
    pressure->pool = pool;
    pressure->published_snapshot = snapshot;
    pressure->published = true;
    return PACKET_PRESSURE_OK;
}

enum packet_pressure_status packet_pressure_begin_update(
    struct packet_pressure *pressure)
{
    enum packet_pressure_status status;

    status = packet_pressure_common_check(pressure);
    if (status != PACKET_PRESSURE_OK) {
        return status;
    }
    if (pressure->transaction_open) {
        return PACKET_PRESSURE_TRANSACTION_OPEN;
    }

    pressure->pending_snapshot = pressure->published_snapshot;
    pressure->transaction_open = true;
    return PACKET_PRESSURE_OK;
}

enum packet_pressure_status packet_pressure_refresh_pool_counts(
    struct packet_pressure *pressure)
{
    enum packet_pressure_status status;
    uint32_t count;
    uint32_t free_count;
    uint32_t in_use_count;

    status = packet_pressure_common_check(pressure);
    if (status != PACKET_PRESSURE_OK) {
        return status;
    }
    if (!pressure->transaction_open) {
        return PACKET_PRESSURE_NO_TRANSACTION;
    }

    count = packet_pool_count(pressure->pool);
    free_count = packet_pool_free_count(pressure->pool);
    in_use_count = count - free_count;
    pressure->pending_snapshot.version = PACKET_PRESSURE_VERSION;
    pressure->pending_snapshot.pool_count = count;
    pressure->pending_snapshot.pool_free_count = free_count;
    pressure->pending_snapshot.pool_in_use_count = in_use_count;
    if (pressure->pending_snapshot.pool_high_water_count <
        in_use_count) {
        pressure->pending_snapshot.pool_high_water_count =
            in_use_count;
    }
    return PACKET_PRESSURE_OK;
}

static enum packet_pressure_status packet_pressure_record_count(
    struct packet_pressure *pressure,
    uint32_t count,
    uint32_t *counter)
{
    enum packet_pressure_status status;

    status = packet_pressure_common_check(pressure);
    if (status != PACKET_PRESSURE_OK) {
        return status;
    }
    if (!pressure->transaction_open) {
        return PACKET_PRESSURE_NO_TRANSACTION;
    }
    if (count == 0U) {
        return PACKET_PRESSURE_BAD_COUNTERS;
    }
    if (*counter > UINT32_MAX - count) {
        return PACKET_PRESSURE_OVERFLOW;
    }

    *counter += count;
    return PACKET_PRESSURE_OK;
}

enum packet_pressure_status packet_pressure_record_rx_drop(
    struct packet_pressure *pressure,
    uint32_t count)
{
    if (pressure == NULL) {
        return PACKET_PRESSURE_NULL;
    }
    return packet_pressure_record_count(
        pressure, count, &pressure->pending_snapshot.rx_drop_count);
}

enum packet_pressure_status packet_pressure_record_tx_backpressure(
    struct packet_pressure *pressure,
    uint32_t count)
{
    if (pressure == NULL) {
        return PACKET_PRESSURE_NULL;
    }
    return packet_pressure_record_count(
        pressure, count,
        &pressure->pending_snapshot.tx_backpressure_count);
}

enum packet_pressure_status packet_pressure_record_invalid_frame(
    struct packet_pressure *pressure,
    uint32_t count)
{
    if (pressure == NULL) {
        return PACKET_PRESSURE_NULL;
    }
    return packet_pressure_record_count(
        pressure, count,
        &pressure->pending_snapshot.invalid_frame_count);
}

enum packet_pressure_status packet_pressure_commit(
    struct packet_pressure *pressure)
{
    enum packet_pressure_status status;

    status = packet_pressure_common_check(pressure);
    if (status != PACKET_PRESSURE_OK) {
        return status;
    }
    if (!pressure->transaction_open) {
        return PACKET_PRESSURE_NO_TRANSACTION;
    }
    if (!packet_pressure_snapshot_valid(
            &pressure->pending_snapshot, pressure->pool)) {
        return PACKET_PRESSURE_BAD_COUNTERS;
    }
    if (pressure->published_snapshot.generation == UINT32_MAX) {
        return PACKET_PRESSURE_GENERATION_OVERFLOW;
    }

    pressure->pending_snapshot.generation =
        pressure->published_snapshot.generation + 1U;
    pressure->published_snapshot = pressure->pending_snapshot;
    pressure->transaction_open = false;
    return PACKET_PRESSURE_OK;
}

enum packet_pressure_status packet_pressure_rollback(
    struct packet_pressure *pressure)
{
    enum packet_pressure_status status;

    status = packet_pressure_common_check(pressure);
    if (status != PACKET_PRESSURE_OK) {
        return status;
    }
    if (!pressure->transaction_open) {
        return PACKET_PRESSURE_NO_TRANSACTION;
    }

    memset(&pressure->pending_snapshot, 0,
           sizeof(pressure->pending_snapshot));
    pressure->transaction_open = false;
    return PACKET_PRESSURE_OK;
}

enum packet_pressure_status packet_pressure_snapshot(
    const struct packet_pressure *pressure,
    struct packet_pressure_snapshot *snapshot)
{
    enum packet_pressure_status status;

    if (snapshot == NULL) {
        return PACKET_PRESSURE_NULL;
    }
    status = packet_pressure_common_check(pressure);
    if (status != PACKET_PRESSURE_OK) {
        return status;
    }

    *snapshot = pressure->published_snapshot;
    return PACKET_PRESSURE_OK;
}

bool packet_pressure_published(
    const struct packet_pressure *pressure)
{
    return packet_pressure_valid(pressure);
}

const char *packet_pressure_status_string(
    enum packet_pressure_status status)
{
    switch (status) {
    case PACKET_PRESSURE_OK:
        return "PACKET_PRESSURE_OK";
    case PACKET_PRESSURE_NULL:
        return "PACKET_PRESSURE_NULL";
    case PACKET_PRESSURE_BAD_STATE:
        return "PACKET_PRESSURE_BAD_STATE";
    case PACKET_PRESSURE_INVALID_VERSION:
        return "PACKET_PRESSURE_INVALID_VERSION";
    case PACKET_PRESSURE_ALREADY_PUBLISHED:
        return "PACKET_PRESSURE_ALREADY_PUBLISHED";
    case PACKET_PRESSURE_UNPUBLISHED:
        return "PACKET_PRESSURE_UNPUBLISHED";
    case PACKET_PRESSURE_BAD_POOL:
        return "PACKET_PRESSURE_BAD_POOL";
    case PACKET_PRESSURE_BAD_COUNTERS:
        return "PACKET_PRESSURE_BAD_COUNTERS";
    case PACKET_PRESSURE_TRANSACTION_OPEN:
        return "PACKET_PRESSURE_TRANSACTION_OPEN";
    case PACKET_PRESSURE_NO_TRANSACTION:
        return "PACKET_PRESSURE_NO_TRANSACTION";
    case PACKET_PRESSURE_OVERFLOW:
        return "PACKET_PRESSURE_OVERFLOW";
    case PACKET_PRESSURE_GENERATION_OVERFLOW:
        return "PACKET_PRESSURE_GENERATION_OVERFLOW";
    default:
        return NULL;
    }
}
