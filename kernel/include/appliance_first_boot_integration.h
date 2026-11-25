#ifndef HFOC_APPLIANCE_FIRST_BOOT_INTEGRATION_H
#define HFOC_APPLIANCE_FIRST_BOOT_INTEGRATION_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define APPLIANCE_FIRST_BOOT_INTEGRATION_VERSION 1U
#define APPLIANCE_FIRST_BOOT_INTEGRATION_EVIDENCE_VERSION 1U
#define APPLIANCE_FIRST_BOOT_INTEGRATION_OUTPUT_MAX 640U

enum appliance_first_boot_integration_state {
    APPLIANCE_FIRST_BOOT_INTEGRATION_STATE_NONE = 0,
    APPLIANCE_FIRST_BOOT_INTEGRATION_STATE_UNINITIALIZED = 1,
    APPLIANCE_FIRST_BOOT_INTEGRATION_STATE_PREPARED = 2,
    APPLIANCE_FIRST_BOOT_INTEGRATION_STATE_AWAITING_WIZARD = 3,
    APPLIANCE_FIRST_BOOT_INTEGRATION_STATE_AWAITING_MANAGEMENT = 4,
    APPLIANCE_FIRST_BOOT_INTEGRATION_STATE_READY = 5,
    APPLIANCE_FIRST_BOOT_INTEGRATION_STATE_COMPLETE = 6,
    APPLIANCE_FIRST_BOOT_INTEGRATION_STATE_FAILED = 7,
};

enum appliance_first_boot_integration_status {
    APPLIANCE_FIRST_BOOT_INTEGRATION_OK = 0,
    APPLIANCE_FIRST_BOOT_INTEGRATION_NULL = 1,
    APPLIANCE_FIRST_BOOT_INTEGRATION_BAD_STATE = 2,
    APPLIANCE_FIRST_BOOT_INTEGRATION_BAD_VERSION = 3,
    APPLIANCE_FIRST_BOOT_INTEGRATION_ALREADY_PUBLISHED = 4,
    APPLIANCE_FIRST_BOOT_INTEGRATION_UNPUBLISHED = 5,
    APPLIANCE_FIRST_BOOT_INTEGRATION_INVALID_EVIDENCE = 6,
    APPLIANCE_FIRST_BOOT_INTEGRATION_STALE_EVIDENCE = 7,
    APPLIANCE_FIRST_BOOT_INTEGRATION_MISSING_EVIDENCE = 8,
    APPLIANCE_FIRST_BOOT_INTEGRATION_BUFFER_TOO_SMALL = 9,
};

enum appliance_first_boot_integration_failure {
    APPLIANCE_FIRST_BOOT_INTEGRATION_FAILURE_NONE = 0,
    APPLIANCE_FIRST_BOOT_INTEGRATION_FAILURE_EVIDENCE = 1,
    APPLIANCE_FIRST_BOOT_INTEGRATION_FAILURE_STATE = 2,
    APPLIANCE_FIRST_BOOT_INTEGRATION_FAILURE_WIZARD = 3,
    APPLIANCE_FIRST_BOOT_INTEGRATION_FAILURE_MANAGEMENT = 4,
    APPLIANCE_FIRST_BOOT_INTEGRATION_FAILURE_NETWORK = 5,
    APPLIANCE_FIRST_BOOT_INTEGRATION_FAILURE_HTTPS = 6,
    APPLIANCE_FIRST_BOOT_INTEGRATION_FAILURE_COMPLETE = 7,
};

enum appliance_first_boot_integration_health {
    APPLIANCE_FIRST_BOOT_INTEGRATION_HEALTH_UNKNOWN = 0,
    APPLIANCE_FIRST_BOOT_INTEGRATION_HEALTH_PASS = 1,
    APPLIANCE_FIRST_BOOT_INTEGRATION_HEALTH_FAIL = 2,
};

struct appliance_first_boot_integration_evidence {
    uint32_t version;
    uint64_t source_generation;
    bool published;
    bool first_boot_pending;
    bool storage_ready;
    bool wizard_complete;
    bool management_active;
    bool network_active;
    bool https_management_ready;
};

struct appliance_first_boot_integration {
    bool initialized;
    uint32_t version;
    enum appliance_first_boot_integration_state state;
    enum appliance_first_boot_integration_status last_status;
    enum appliance_first_boot_integration_failure failure;
    uint64_t source_generation;
    bool evidence_published;
    bool first_boot_pending;
    bool storage_ready;
    bool wizard_complete;
    bool management_active;
    bool network_active;
    bool https_management_ready;
    uint32_t update_count;
    uint32_t reset_count;
    uint32_t fail_count;
};

struct appliance_first_boot_integration_snapshot {
    bool published;
    uint32_t version;
    enum appliance_first_boot_integration_state state;
    enum appliance_first_boot_integration_status last_status;
    enum appliance_first_boot_integration_failure failure;
    enum appliance_first_boot_integration_health health;
    bool first_boot_pending;
    bool storage_ready;
    bool wizard_complete;
    bool management_active;
    bool network_active;
    bool https_management_ready;
    uint64_t source_generation;
    uint32_t update_count;
    uint32_t reset_count;
    uint32_t fail_count;
};

enum appliance_first_boot_integration_status
appliance_first_boot_integration_init(
    struct appliance_first_boot_integration *integration);
enum appliance_first_boot_integration_status
appliance_first_boot_integration_publish(
    struct appliance_first_boot_integration *integration,
    const struct appliance_first_boot_integration_evidence *evidence);
enum appliance_first_boot_integration_status
appliance_first_boot_integration_complete(
    struct appliance_first_boot_integration *integration);
enum appliance_first_boot_integration_status
appliance_first_boot_integration_fail_closed(
    struct appliance_first_boot_integration *integration,
    enum appliance_first_boot_integration_failure failure);
enum appliance_first_boot_integration_status
appliance_first_boot_integration_reset(
    struct appliance_first_boot_integration *integration);
enum appliance_first_boot_integration_status
appliance_first_boot_integration_snapshot(
    const struct appliance_first_boot_integration *integration,
    struct appliance_first_boot_integration_snapshot *snapshot_out);
enum appliance_first_boot_integration_status
appliance_first_boot_integration_render(
    const struct appliance_first_boot_integration_snapshot *snapshot,
    char *output,
    size_t capacity,
    size_t *output_length);
enum appliance_first_boot_integration_state
appliance_first_boot_integration_state(
    const struct appliance_first_boot_integration *integration);
enum appliance_first_boot_integration_failure
appliance_first_boot_integration_failure(
    const struct appliance_first_boot_integration *integration);
bool appliance_first_boot_integration_published(
    const struct appliance_first_boot_integration *integration);
const char *appliance_first_boot_integration_state_string(
    enum appliance_first_boot_integration_state state);
const char *appliance_first_boot_integration_status_string(
    enum appliance_first_boot_integration_status status);
const char *appliance_first_boot_integration_failure_string(
    enum appliance_first_boot_integration_failure failure);

#endif
