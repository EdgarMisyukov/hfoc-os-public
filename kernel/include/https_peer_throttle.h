#ifndef HFOC_HTTPS_PEER_THROTTLE_H
#define HFOC_HTTPS_PEER_THROTTLE_H

#include <stdint.h>

#define HTTPS_PEER_THROTTLE_VERSION 1U
#define HTTPS_PEER_THROTTLE_PEER_ID_MIN 1U
#define HTTPS_PEER_THROTTLE_PEER_ID_MAX 65535U
#define HTTPS_PEER_THROTTLE_MAX_PEERS 16U
#define HTTPS_PEER_THROTTLE_FAILURE_LIMIT_MIN 1U
#define HTTPS_PEER_THROTTLE_FAILURE_LIMIT_MAX 8U
#define HTTPS_PEER_THROTTLE_WINDOW_MAX 3600U
#define HTTPS_PEER_THROTTLE_COOLDOWN_MAX 3600U

enum https_peer_throttle_bucket_state {
    HTTPS_PEER_THROTTLE_BUCKET_FREE = 0,
    HTTPS_PEER_THROTTLE_BUCKET_ACTIVE = 1,
    HTTPS_PEER_THROTTLE_BUCKET_BLOCKED = 2,
};

enum https_peer_throttle_local_operation {
    HTTPS_PEER_THROTTLE_LOCAL_MANAGEMENT = 1,
    HTTPS_PEER_THROTTLE_LOCAL_HEALTH = 2,
    HTTPS_PEER_THROTTLE_LOCAL_SUPERVISION = 3,
};

enum https_peer_throttle_status {
    HTTPS_PEER_THROTTLE_OK = 0,
    HTTPS_PEER_THROTTLE_NULL = 1,
    HTTPS_PEER_THROTTLE_BAD_CAPACITY = 2,
    HTTPS_PEER_THROTTLE_INVALID_VERSION = 3,
    HTTPS_PEER_THROTTLE_INVALID_PEER_ID = 4,
    HTTPS_PEER_THROTTLE_INVALID_FAILURE_LIMIT = 5,
    HTTPS_PEER_THROTTLE_INVALID_WINDOW = 6,
    HTTPS_PEER_THROTTLE_INVALID_COOLDOWN = 7,
    HTTPS_PEER_THROTTLE_INVALID_TICK = 8,
    HTTPS_PEER_THROTTLE_PEER_BLOCKED = 9,
    HTTPS_PEER_THROTTLE_PEER_NOT_FOUND = 10,
    HTTPS_PEER_THROTTLE_INVALID_STATE = 11,
    HTTPS_PEER_THROTTLE_INVALID_LOCAL_OPERATION = 12,
};

struct https_peer_throttle_bucket {
    uint32_t version;
    uint32_t peer_id;
    uint32_t failure_count;
    uint64_t first_failure_tick;
    uint64_t last_failure_tick;
    uint64_t blocked_until_tick;
    enum https_peer_throttle_bucket_state state;
};

struct https_peer_throttle_table {
    uint32_t version;
    uint32_t capacity;
    uint32_t failure_limit;
    uint32_t window_ticks;
    uint32_t cooldown_ticks;
    uint32_t count;
    struct https_peer_throttle_bucket
        buckets[HTTPS_PEER_THROTTLE_MAX_PEERS];
};

enum https_peer_throttle_status https_peer_throttle_init(
    struct https_peer_throttle_table *table,
    uint32_t capacity,
    uint32_t failure_limit,
    uint32_t window_ticks,
    uint32_t cooldown_ticks);
enum https_peer_throttle_status https_peer_throttle_record_failure(
    struct https_peer_throttle_table *table,
    uint32_t peer_id,
    uint64_t current_tick,
    uint64_t *out_blocked_until_tick);
enum https_peer_throttle_status https_peer_throttle_check(
    const struct https_peer_throttle_table *table,
    uint32_t peer_id,
    uint64_t current_tick);
enum https_peer_throttle_status https_peer_throttle_reset(
    struct https_peer_throttle_table *table,
    uint32_t peer_id);
enum https_peer_throttle_status https_peer_throttle_local_allowed(
    enum https_peer_throttle_local_operation operation);
const char *https_peer_throttle_status_string(
    enum https_peer_throttle_status status);

#endif
