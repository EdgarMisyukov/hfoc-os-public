#ifndef HFOC_CONFIG_SCHEMA_H
#define HFOC_CONFIG_SCHEMA_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define CONFIG_SCHEMA_VERSION 1U
#define CONFIG_INPUT_MAX 4096U
#define CONFIG_LINE_MAX 64U
#define CONFIG_KEY_MAX 32U
#define CONFIG_VALUE_MAX 32U

enum config_status {
    CONFIG_OK = 0,
    CONFIG_NULL = 1,
    CONFIG_TOO_LONG = 2,
    CONFIG_MISSING_FIELD = 3,
    CONFIG_DUPLICATE_FIELD = 4,
    CONFIG_UNKNOWN_FIELD = 5,
    CONFIG_MALFORMED_FIELD = 6,
    CONFIG_BAD_VERSION = 7,
    CONFIG_OUT_OF_RANGE = 8,
};

struct appliance_config {
    uint32_t schema_version;
    uint32_t recovery_mode;
    uint32_t max_processes;
    uint32_t max_user_memory_pages;
    uint32_t audit_ring_capacity;
    uint32_t boot_timeout_ms;
    uint32_t restart_limit;
    uint32_t service_health_timeout_ms;
};

enum config_snapshot_status {
    CONFIG_SNAPSHOT_OK = 0,
    CONFIG_SNAPSHOT_NULL = 1,
    CONFIG_SNAPSHOT_BAD_STATE = 2,
    CONFIG_SNAPSHOT_NO_PENDING = 3,
    CONFIG_SNAPSHOT_PARSE_FAILED = 4,
    CONFIG_SNAPSHOT_COMMIT_FAILED = 5,
    CONFIG_SNAPSHOT_INVALID_CONFIG = 6,
};

struct config_snapshot {
    struct appliance_config active;
    struct appliance_config pending;
    bool has_active;
    bool has_pending;
};

typedef bool (*config_commit_validator)(
    const struct appliance_config *candidate,
    void *context);

enum config_status config_schema_parse(
    const char *input,
    size_t input_length,
    struct appliance_config *config_out);
const char *config_status_string(enum config_status status);
enum config_snapshot_status config_snapshot_init(
    struct config_snapshot *snapshot,
    const struct appliance_config *initial);
enum config_snapshot_status config_snapshot_prepare(
    struct config_snapshot *snapshot,
    const char *input,
    size_t input_length);
enum config_snapshot_status config_snapshot_commit(
    struct config_snapshot *snapshot,
    config_commit_validator validator,
    void *context);
enum config_snapshot_status config_snapshot_rollback(
    struct config_snapshot *snapshot);
enum config_snapshot_status config_snapshot_destroy(
    struct config_snapshot *snapshot);
const struct appliance_config *config_snapshot_active(
    const struct config_snapshot *snapshot);
bool config_snapshot_has_pending(const struct config_snapshot *snapshot);
const char *config_snapshot_status_string(enum config_snapshot_status status);

#endif
