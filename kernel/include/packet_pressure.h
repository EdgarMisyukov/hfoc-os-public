#ifndef HFOC_PACKET_PRESSURE_H
#define HFOC_PACKET_PRESSURE_H

#include "packet_pool.h"

#include <stdbool.h>
#include <stdint.h>

#define PACKET_PRESSURE_VERSION 1U

enum packet_pressure_status {
    PACKET_PRESSURE_OK = 0,
    PACKET_PRESSURE_NULL = 1,
    PACKET_PRESSURE_BAD_STATE = 2,
    PACKET_PRESSURE_INVALID_VERSION = 3,
    PACKET_PRESSURE_ALREADY_PUBLISHED = 4,
    PACKET_PRESSURE_UNPUBLISHED = 5,
    PACKET_PRESSURE_BAD_POOL = 6,
    PACKET_PRESSURE_BAD_COUNTERS = 7,
    PACKET_PRESSURE_TRANSACTION_OPEN = 8,
    PACKET_PRESSURE_NO_TRANSACTION = 9,
    PACKET_PRESSURE_OVERFLOW = 10,
    PACKET_PRESSURE_GENERATION_OVERFLOW = 11,
};

struct packet_pressure_snapshot {
    uint32_t version;
    uint32_t generation;
    uint32_t pool_count;
    uint32_t pool_free_count;
    uint32_t pool_in_use_count;
    uint32_t pool_high_water_count;
    uint32_t rx_drop_count;
    uint32_t tx_backpressure_count;
    uint32_t invalid_frame_count;
};

struct packet_pressure {
    bool published;
    bool transaction_open;
    uint32_t version;
    const struct packet_pool_descriptor *pool;
    struct packet_pressure_snapshot published_snapshot;
    struct packet_pressure_snapshot pending_snapshot;
};

enum packet_pressure_status packet_pressure_init(
    struct packet_pressure *pressure,
    const struct packet_pool_descriptor *pool);
enum packet_pressure_status packet_pressure_begin_update(
    struct packet_pressure *pressure);
enum packet_pressure_status packet_pressure_refresh_pool_counts(
    struct packet_pressure *pressure);
enum packet_pressure_status packet_pressure_record_rx_drop(
    struct packet_pressure *pressure,
    uint32_t count);
enum packet_pressure_status packet_pressure_record_tx_backpressure(
    struct packet_pressure *pressure,
    uint32_t count);
enum packet_pressure_status packet_pressure_record_invalid_frame(
    struct packet_pressure *pressure,
    uint32_t count);
enum packet_pressure_status packet_pressure_commit(
    struct packet_pressure *pressure);
enum packet_pressure_status packet_pressure_rollback(
    struct packet_pressure *pressure);
enum packet_pressure_status packet_pressure_snapshot(
    const struct packet_pressure *pressure,
    struct packet_pressure_snapshot *snapshot);
bool packet_pressure_published(
    const struct packet_pressure *pressure);
const char *packet_pressure_status_string(
    enum packet_pressure_status status);

#endif
