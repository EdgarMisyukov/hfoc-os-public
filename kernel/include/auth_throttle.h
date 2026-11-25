#ifndef HFOC_AUTH_THROTTLE_H
#define HFOC_AUTH_THROTTLE_H

#include <stdint.h>

#define AUTH_THROTTLE_VERSION 1U
#define AUTH_THROTTLE_PRINCIPAL_ID_MIN 1U
#define AUTH_THROTTLE_PRINCIPAL_ID_MAX 255U
#define AUTH_THROTTLE_TABLE_MAX 16U
#define AUTH_THROTTLE_FAILURE_LIMIT_MIN 1U
#define AUTH_THROTTLE_FAILURE_LIMIT_MAX 8U
#define AUTH_THROTTLE_COOLDOWN_MAX 3600U

enum auth_throttle_entry_state {
    AUTH_THROTTLE_ENTRY_FREE = 0,
    AUTH_THROTTLE_ENTRY_ACTIVE = 1,
};

enum auth_throttle_status {
    AUTH_THROTTLE_OK = 0,
    AUTH_THROTTLE_NULL = 1,
    AUTH_THROTTLE_BAD_CAPACITY = 2,
    AUTH_THROTTLE_INVALID_VERSION = 3,
    AUTH_THROTTLE_INVALID_PRINCIPAL_ID = 4,
    AUTH_THROTTLE_INVALID_FAILURE_LIMIT = 5,
    AUTH_THROTTLE_INVALID_COOLDOWN = 6,
    AUTH_THROTTLE_INVALID_TICK = 7,
    AUTH_THROTTLE_TABLE_FULL = 8,
    AUTH_THROTTLE_LOCKED = 9,
    AUTH_THROTTLE_INVALID_STATE = 10,
};

struct auth_throttle_entry {
    uint32_t version;
    uint32_t principal_id;
    uint32_t failure_count;
    uint64_t locked_until_tick;
    enum auth_throttle_entry_state state;
};

struct auth_throttle_table {
    uint32_t version;
    uint32_t capacity;
    uint32_t failure_limit;
    uint32_t cooldown_ticks;
    struct auth_throttle_entry entries[AUTH_THROTTLE_TABLE_MAX];
};

enum auth_throttle_status auth_throttle_table_init(
    struct auth_throttle_table *table,
    uint32_t capacity,
    uint32_t failure_limit,
    uint32_t cooldown_ticks);
enum auth_throttle_status auth_throttle_record_failure(
    struct auth_throttle_table *table,
    uint32_t principal_id,
    uint64_t now_tick,
    uint64_t *out_locked_until_tick);
enum auth_throttle_status auth_throttle_reset(
    struct auth_throttle_table *table,
    uint32_t principal_id);
enum auth_throttle_status auth_throttle_check(
    const struct auth_throttle_table *table,
    uint32_t principal_id,
    uint64_t now_tick);
const char *auth_throttle_status_string(
    enum auth_throttle_status status);

#endif
