#ifndef HFOC_METRICS_H
#define HFOC_METRICS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define METRICS_VERSION 1U
#define METRICS_SCHEMA_ID "HFOC-METRICS-1"
#define METRICS_SCHEMA_ID_LENGTH 14U
#define METRICS_PATH_COUNT 4U
#define METRICS_HEALTH_CHECK_MAX 16U
#define METRICS_EVENT_RING_CAPACITY 64U
#define METRICS_SERVICE_CAPACITY 8U
#define METRICS_SLOT_CAPACITY 8U
#define METRICS_STATE_SCHEMA_VERSION_MIN 1U
#define METRICS_STATE_SCHEMA_VERSION_MAX 1024U

enum metrics_status {
    METRICS_OK = 0,
    METRICS_NULL = 1,
    METRICS_BAD_STATE = 2,
    METRICS_BAD_VERSION = 3,
    METRICS_NOT_PUBLISHED = 4,
    METRICS_NO_TRANSACTION = 5,
    METRICS_TRANSACTION_OPEN = 6,
    METRICS_BAD_VALUE = 7,
    METRICS_INCOMPLETE = 8,
    METRICS_GENERATION_OVERFLOW = 9,
};

struct metrics_health_values {
    uint32_t version;
    uint32_t check_count;
    uint64_t pass_count;
    uint64_t warn_count;
    uint64_t fail_count;
    uint64_t unknown_count;
    bool ring_published;
    uint32_t ring_count;
    uint32_t ring_available;
    uint32_t ring_utilization_percent;
};

struct metrics_state_values {
    uint32_t version;
    uint64_t generation;
    uint64_t journal_generation;
    uint64_t checkpoint_sequence;
    uint32_t schema_version;
    uint32_t active_generation;
    uint64_t migrations;
    uint64_t compactions;
    uint64_t restores;
    uint64_t backup_exports;
};

struct metrics_service_values {
    uint32_t version;
    uint64_t generation;
    uint32_t service_count;
    uint32_t ready_count;
    uint32_t pending_count;
    uint32_t failed_count;
    uint32_t exhausted_count;
    uint32_t dependency_blocked_count;
    uint64_t restarts;
    uint64_t exits;
};

struct metrics_update_values {
    uint32_t version;
    uint64_t generation;
    uint32_t inventory_generation;
    uint32_t slot_count;
    uint32_t known_good_slot;
    uint32_t pending_slot;
    uint32_t rollback_slot;
    bool candidate_pending;
    bool candidate_active;
    bool accepted;
    bool rollback_published;
    uint64_t acceptances;
    uint64_t rollbacks;
    uint64_t rejections;
};

struct metrics_paths {
    struct metrics_health_values health;
    struct metrics_state_values state;
    struct metrics_service_values service;
    struct metrics_update_values update;
};

struct metrics_snapshot {
    uint32_t version;
    uint64_t generation;
    struct metrics_paths paths;
};

struct metrics_store {
    uint32_t version;
    bool initialized;
    bool published;
    bool mutation_open;
    uint64_t generation;
    uint32_t pending_path_mask;
    struct metrics_paths published_paths;
    struct metrics_paths pending_paths;
};

enum metrics_status metrics_init(struct metrics_store *store);
enum metrics_status metrics_begin_update(struct metrics_store *store);
enum metrics_status metrics_set_health(
    struct metrics_store *store,
    const struct metrics_health_values *values);
enum metrics_status metrics_set_state(
    struct metrics_store *store,
    const struct metrics_state_values *values);
enum metrics_status metrics_set_service(
    struct metrics_store *store,
    const struct metrics_service_values *values);
enum metrics_status metrics_set_update(
    struct metrics_store *store,
    const struct metrics_update_values *values);
enum metrics_status metrics_commit(struct metrics_store *store);
enum metrics_status metrics_rollback(struct metrics_store *store);
enum metrics_status metrics_snapshot(
    const struct metrics_store *store,
    struct metrics_snapshot *snapshot_out);
const char *metrics_status_string(enum metrics_status status);
const char *metrics_schema_id(void);

#endif
