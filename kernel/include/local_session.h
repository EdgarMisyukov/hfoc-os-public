#ifndef HFOC_LOCAL_SESSION_H
#define HFOC_LOCAL_SESSION_H

#include <stdint.h>

#define LOCAL_SESSION_VERSION 1U
#define LOCAL_SESSION_ID_MIN 1U
#define LOCAL_SESSION_ID_MAX 255U
#define LOCAL_SESSION_PRINCIPAL_ID_MIN 1U
#define LOCAL_SESSION_PRINCIPAL_ID_MAX 255U
#define LOCAL_SESSION_GENERATION_MIN 1U
#define LOCAL_SESSION_LIFETIME_MAX 3600U
#define LOCAL_SESSION_TABLE_MAX 16U

enum local_session_state {
    LOCAL_SESSION_STATE_CLOSED = 0,
    LOCAL_SESSION_STATE_OPEN = 1,
};

enum local_session_status {
    LOCAL_SESSION_OK = 0,
    LOCAL_SESSION_NULL = 1,
    LOCAL_SESSION_BAD_CAPACITY = 2,
    LOCAL_SESSION_INVALID_VERSION = 3,
    LOCAL_SESSION_INVALID_SESSION_ID = 4,
    LOCAL_SESSION_INVALID_PRINCIPAL_ID = 5,
    LOCAL_SESSION_INVALID_GENERATION = 6,
    LOCAL_SESSION_INVALID_LIFETIME = 7,
    LOCAL_SESSION_INVALID_TICK = 8,
    LOCAL_SESSION_TABLE_FULL = 9,
    LOCAL_SESSION_ID_EXHAUSTED = 10,
    LOCAL_SESSION_STALE_GENERATION = 11,
    LOCAL_SESSION_CLOSED = 12,
    LOCAL_SESSION_EXPIRED = 13,
    LOCAL_SESSION_NOT_OWNER = 14,
    LOCAL_SESSION_INVALID_STATE = 15,
};

struct local_session {
    uint32_t version;
    uint32_t id;
    uint32_t principal_id;
    uint32_t generation;
    uint64_t opened_tick;
    uint64_t expires_tick;
    enum local_session_state state;
};

struct local_session_table {
    uint32_t capacity;
    uint32_t next_id;
    struct local_session sessions[LOCAL_SESSION_TABLE_MAX];
};

enum local_session_status local_session_table_init(
    struct local_session_table *table, uint32_t capacity);
enum local_session_status local_session_open(
    struct local_session_table *table,
    uint32_t principal_id,
    uint32_t generation,
    uint64_t now_tick,
    uint32_t lifetime_ticks,
    uint32_t *out_session_id);
enum local_session_status local_session_close(
    struct local_session_table *table, uint32_t session_id);
enum local_session_status local_session_validate(
    const struct local_session_table *table,
    uint32_t session_id,
    uint32_t principal_id,
    uint32_t generation,
    uint64_t now_tick);
enum local_session_status local_session_invalidate_generation(
    struct local_session_table *table,
    uint32_t principal_id,
    uint32_t generation);
const char *local_session_status_string(
    enum local_session_status status);

#endif
