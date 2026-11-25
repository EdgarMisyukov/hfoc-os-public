#ifndef HFOC_INTEGRATION_FAILURE_ISOLATION_H
#define HFOC_INTEGRATION_FAILURE_ISOLATION_H

#include <stdint.h>

#define INTEGRATION_FAILURE_ISOLATION_VERSION 1U
#define INTEGRATION_FAILURE_ISOLATION_PEER_ID_MIN 1U
#define INTEGRATION_FAILURE_ISOLATION_PEER_ID_MAX 65535U
#define INTEGRATION_FAILURE_ISOLATION_MAX_PEERS 64U
#define INTEGRATION_FAILURE_ISOLATION_THRESHOLD 3U
#define INTEGRATION_FAILURE_ISOLATION_COOLDOWN_MALFORMED 10U
#define INTEGRATION_FAILURE_ISOLATION_COOLDOWN_UNAVAILABLE 20U
#define INTEGRATION_FAILURE_ISOLATION_COOLDOWN_SLOW 30U
#define INTEGRATION_FAILURE_ISOLATION_COOLDOWN_MAX 30U

enum integration_failure_kind {
    INTEGRATION_FAILURE_MALFORMED = 1,
    INTEGRATION_FAILURE_UNAVAILABLE = 2,
    INTEGRATION_FAILURE_SLOW = 3,
};

enum integration_peer_state {
    INTEGRATION_PEER_STATE_HEALTHY = 0,
    INTEGRATION_PEER_STATE_DEGRADED = 1,
    INTEGRATION_PEER_STATE_ISOLATED = 2,
};

enum integration_local_operation {
    INTEGRATION_LOCAL_MANAGEMENT = 1,
    INTEGRATION_LOCAL_SERVICE_SUPERVISION = 2,
    INTEGRATION_LOCAL_STATE_RECOVERY = 3,
};

enum integration_failure_isolation_status {
    INTEGRATION_FAILURE_ISOLATION_OK = 0,
    INTEGRATION_FAILURE_ISOLATION_NULL = 1,
    INTEGRATION_FAILURE_ISOLATION_BAD_CAPACITY = 2,
    INTEGRATION_FAILURE_ISOLATION_BAD_STATE = 3,
    INTEGRATION_FAILURE_ISOLATION_INVALID_PEER_ID = 4,
    INTEGRATION_FAILURE_ISOLATION_INVALID_FAULT_KIND = 5,
    INTEGRATION_FAILURE_ISOLATION_INVALID_LOCAL_OPERATION = 6,
    INTEGRATION_FAILURE_ISOLATION_TABLE_FULL = 7,
    INTEGRATION_FAILURE_ISOLATION_PEER_ISOLATED = 8,
    INTEGRATION_FAILURE_ISOLATION_PEER_NOT_FOUND = 9,
};

struct integration_failure_peer {
    uint32_t peer_id;
    uint32_t consecutive_failures;
    uint32_t state;
    uint64_t last_failure_tick;
    uint64_t isolated_until_tick;
};

struct integration_failure_isolation_table {
    uint32_t version;
    uint32_t capacity;
    uint32_t count;
    struct integration_failure_peer
        peers[INTEGRATION_FAILURE_ISOLATION_MAX_PEERS];
};

enum integration_failure_isolation_status integration_failure_isolation_init(
    struct integration_failure_isolation_table *table,
    uint32_t capacity);
enum integration_failure_isolation_status integration_failure_isolation_record(
    struct integration_failure_isolation_table *table,
    uint32_t peer_id,
    enum integration_failure_kind kind,
    uint64_t current_tick);
enum integration_failure_isolation_status integration_failure_isolation_check(
    const struct integration_failure_isolation_table *table,
    uint32_t peer_id,
    uint64_t current_tick);
enum integration_failure_isolation_status integration_failure_isolation_recover(
    struct integration_failure_isolation_table *table,
    uint32_t peer_id);
enum integration_failure_isolation_status integration_failure_isolation_local_allowed(
    enum integration_local_operation operation);
const char *integration_failure_isolation_status_string(
    enum integration_failure_isolation_status status);

#endif
