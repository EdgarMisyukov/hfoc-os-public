#ifndef HFOC_APPLIANCE_STATUS_H
#define HFOC_APPLIANCE_STATUS_H

#include "authorization.h"
#include "management_command.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define APPLIANCE_STATUS_VERSION 1U
#define APPLIANCE_STATUS_COMMAND_ID 1U
#define APPLIANCE_STATUS_REQUEST_LENGTH 0U
#define APPLIANCE_STATUS_RESPONSE_MIN 1U
#define APPLIANCE_STATUS_RESPONSE_MAX 255U
#define APPLIANCE_STATUS_OUTPUT_MAX APPLIANCE_STATUS_RESPONSE_MAX
#define APPLIANCE_STATUS_SERVICE_MAX 8U
#define APPLIANCE_STATUS_COUNT_MAX 255U
#define APPLIANCE_STATUS_RECOVERY_MODE_MAX 2U

enum appliance_status_status {
    APPLIANCE_STATUS_OK = 0,
    APPLIANCE_STATUS_NULL = 1,
    APPLIANCE_STATUS_BAD_VERSION = 2,
    APPLIANCE_STATUS_BAD_STATE = 3,
    APPLIANCE_STATUS_INVALID_SNAPSHOT = 4,
    APPLIANCE_STATUS_INVALID_SERVICE = 5,
    APPLIANCE_STATUS_INVALID_STORAGE = 6,
    APPLIANCE_STATUS_INVALID_HEALTH = 7,
    APPLIANCE_STATUS_OVERFLOW = 8,
};

enum appliance_status_health_state {
    APPLIANCE_HEALTH_OK = 0,
    APPLIANCE_HEALTH_DEGRADED = 1,
    APPLIANCE_HEALTH_FAILED = 2,
};

enum appliance_status_storage_state {
    APPLIANCE_STORAGE_OK = 0,
    APPLIANCE_STORAGE_DEGRADED = 1,
    APPLIANCE_STORAGE_FAILED = 2,
};

enum appliance_status_service_state {
    APPLIANCE_SERVICE_STOPPED = 0,
    APPLIANCE_SERVICE_RUNNING = 1,
    APPLIANCE_SERVICE_READY = 2,
    APPLIANCE_SERVICE_EXITED = 3,
    APPLIANCE_SERVICE_EXHAUSTED = 4,
};

enum appliance_status_service_health {
    APPLIANCE_SERVICE_HEALTH_PENDING = 0,
    APPLIANCE_SERVICE_HEALTH_READY = 1,
    APPLIANCE_SERVICE_HEALTH_LIVENESS_FAILED = 2,
    APPLIANCE_SERVICE_HEALTH_READINESS_TIMEOUT = 3,
};

struct appliance_status_state_snapshot {
    uint32_t version;
    uint64_t generation;
    uint64_t uptime_ticks;
    uint32_t recovery_mode;
    uint64_t config_revision;
    uint64_t audit_sequence;
    uint32_t active_process_count;
};

struct appliance_status_storage_snapshot {
    uint32_t version;
    uint64_t journal_generation;
    uint64_t backup_generation;
    uint32_t schema_version;
    enum appliance_status_storage_state state;
};

struct appliance_status_service_entry {
    uint32_t id;
    enum appliance_status_service_state state;
    enum appliance_status_service_health health;
    uint32_t restart_count;
};

struct appliance_status_health_snapshot {
    uint32_t version;
    enum appliance_status_health_state state;
    uint32_t ready_count;
    uint32_t pending_count;
    uint32_t failed_count;
};

struct appliance_status_snapshot {
    uint32_t version;
    bool published;
    struct appliance_status_state_snapshot state;
    struct appliance_status_storage_snapshot storage;
    struct appliance_status_health_snapshot health;
    uint32_t service_count;
    struct appliance_status_service_entry services[
        APPLIANCE_STATUS_SERVICE_MAX];
};

enum appliance_status_status appliance_status_snapshot_init(
    struct appliance_status_snapshot *snapshot,
    const struct appliance_status_snapshot *source);
enum appliance_status_status appliance_status_render(
    const struct appliance_status_snapshot *snapshot,
    char *output,
    size_t capacity,
    size_t *output_length);
enum appliance_status_status appliance_status_command_descriptor(
    struct management_command_descriptor *descriptor);
enum appliance_status_status appliance_status_authorization_entry(
    struct authorization_entry *entry);
const char *appliance_status_status_string(
    enum appliance_status_status status);
const char *appliance_status_health_string(
    enum appliance_status_health_state state);
const char *appliance_status_storage_string(
    enum appliance_status_storage_state state);
const char *appliance_status_service_state_string(
    enum appliance_status_service_state state);
const char *appliance_status_service_health_string(
    enum appliance_status_service_health state);

#endif
