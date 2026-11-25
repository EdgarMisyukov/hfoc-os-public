#ifndef HFOC_APPLIANCE_RECOVERY_FACTORY_RESET_INTEGRATION_H
#define HFOC_APPLIANCE_RECOVERY_FACTORY_RESET_INTEGRATION_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define APPLIANCE_RECOVERY_FACTORY_RESET_INTEGRATION_VERSION 1U
#define APPLIANCE_RECOVERY_FACTORY_RESET_INTEGRATION_EVIDENCE_VERSION 1U
#define APPLIANCE_RECOVERY_FACTORY_RESET_INTEGRATION_OUTPUT_MAX 640U

#define APPLIANCE_RECOVERY_FACTORY_RESET_OPERATION_NONE 0U
#define APPLIANCE_RECOVERY_FACTORY_RESET_OPERATION_RECOVERY 1U
#define APPLIANCE_RECOVERY_FACTORY_RESET_OPERATION_FACTORY_RESET 2U

enum appliance_recovery_factory_reset_integration_state {
    APPLIANCE_RECOVERY_FACTORY_RESET_INTEGRATION_STATE_NONE = 0,
    APPLIANCE_RECOVERY_FACTORY_RESET_INTEGRATION_STATE_UNINITIALIZED = 1,
    APPLIANCE_RECOVERY_FACTORY_RESET_INTEGRATION_STATE_PREPARED = 2,
    APPLIANCE_RECOVERY_FACTORY_RESET_INTEGRATION_STATE_AWAITING_RECOVERY = 3,
    APPLIANCE_RECOVERY_FACTORY_RESET_INTEGRATION_STATE_AWAITING_FOUNDATION = 4,
    APPLIANCE_RECOVERY_FACTORY_RESET_INTEGRATION_STATE_AWAITING_AUTHORIZATION = 5,
    APPLIANCE_RECOVERY_FACTORY_RESET_INTEGRATION_STATE_READY = 6,
    APPLIANCE_RECOVERY_FACTORY_RESET_INTEGRATION_STATE_COMPLETE = 7,
    APPLIANCE_RECOVERY_FACTORY_RESET_INTEGRATION_STATE_FAILED = 8,
};

enum appliance_recovery_factory_reset_integration_status {
    APPLIANCE_RECOVERY_FACTORY_RESET_INTEGRATION_OK = 0,
    APPLIANCE_RECOVERY_FACTORY_RESET_INTEGRATION_NULL = 1,
    APPLIANCE_RECOVERY_FACTORY_RESET_INTEGRATION_BAD_STATE = 2,
    APPLIANCE_RECOVERY_FACTORY_RESET_INTEGRATION_BAD_VERSION = 3,
    APPLIANCE_RECOVERY_FACTORY_RESET_INTEGRATION_ALREADY_PUBLISHED = 4,
    APPLIANCE_RECOVERY_FACTORY_RESET_INTEGRATION_UNPUBLISHED = 5,
    APPLIANCE_RECOVERY_FACTORY_RESET_INTEGRATION_INVALID_EVIDENCE = 6,
    APPLIANCE_RECOVERY_FACTORY_RESET_INTEGRATION_STALE_EVIDENCE = 7,
    APPLIANCE_RECOVERY_FACTORY_RESET_INTEGRATION_MISSING_EVIDENCE = 8,
    APPLIANCE_RECOVERY_FACTORY_RESET_INTEGRATION_BUFFER_TOO_SMALL = 9,
};

enum appliance_recovery_factory_reset_integration_failure {
    APPLIANCE_RECOVERY_FACTORY_RESET_INTEGRATION_FAILURE_NONE = 0,
    APPLIANCE_RECOVERY_FACTORY_RESET_INTEGRATION_FAILURE_EVIDENCE = 1,
    APPLIANCE_RECOVERY_FACTORY_RESET_INTEGRATION_FAILURE_STATE = 2,
    APPLIANCE_RECOVERY_FACTORY_RESET_INTEGRATION_FAILURE_RECOVERY = 3,
    APPLIANCE_RECOVERY_FACTORY_RESET_INTEGRATION_FAILURE_FACTORY_RESET = 4,
    APPLIANCE_RECOVERY_FACTORY_RESET_INTEGRATION_FAILURE_AUTHORIZATION = 5,
    APPLIANCE_RECOVERY_FACTORY_RESET_INTEGRATION_FAILURE_FOUNDATION = 6,
    APPLIANCE_RECOVERY_FACTORY_RESET_INTEGRATION_FAILURE_COMPLETE = 7,
};

enum appliance_recovery_factory_reset_integration_health {
    APPLIANCE_RECOVERY_FACTORY_RESET_INTEGRATION_HEALTH_UNKNOWN = 0,
    APPLIANCE_RECOVERY_FACTORY_RESET_INTEGRATION_HEALTH_PASS = 1,
    APPLIANCE_RECOVERY_FACTORY_RESET_INTEGRATION_HEALTH_FAIL = 2,
};

struct appliance_recovery_factory_reset_integration_evidence {
    uint32_t version;
    uint64_t source_generation;
    bool published;
    uint32_t operation_kind;
    bool recovery_decision_present;
    uint32_t recovery_action;
    bool recovery_local_only;
    bool recovery_requires_reboot;
    bool factory_reset_requested;
    bool factory_reset_plan_published;
    bool authorization_verified;
    bool boot_foundation_verified;
    bool storage_loader_ready;
    bool installer_recovery_ready;
    bool efi_environment_ready;
    bool management_profile_required;
    bool management_active;
    bool first_boot_complete;
    bool network_active;
    bool https_management_ready;
    bool recovery_trust_active;
};

struct appliance_recovery_factory_reset_integration {
    bool initialized;
    uint32_t version;
    enum appliance_recovery_factory_reset_integration_state state;
    enum appliance_recovery_factory_reset_integration_status last_status;
    enum appliance_recovery_factory_reset_integration_failure failure;
    uint64_t source_generation;
    bool evidence_published;
    uint32_t operation_kind;
    uint32_t recovery_action;
    bool recovery_requires_reboot;
    bool factory_reset_plan_published;
    bool authorization_verified;
    bool boot_foundation_verified;
    bool storage_loader_ready;
    bool installer_recovery_ready;
    bool efi_environment_ready;
    bool management_active;
    bool first_boot_complete;
    bool network_active;
    bool https_management_ready;
    uint32_t update_count;
    uint32_t reset_count;
    uint32_t fail_count;
};

struct appliance_recovery_factory_reset_integration_snapshot {
    bool published;
    uint32_t version;
    enum appliance_recovery_factory_reset_integration_state state;
    enum appliance_recovery_factory_reset_integration_status last_status;
    enum appliance_recovery_factory_reset_integration_failure failure;
    enum appliance_recovery_factory_reset_integration_health health;
    uint32_t operation_kind;
    uint32_t recovery_action;
    bool recovery_requires_reboot;
    bool factory_reset_plan_published;
    bool authorization_verified;
    bool boot_foundation_verified;
    bool storage_loader_ready;
    bool installer_recovery_ready;
    bool efi_environment_ready;
    bool management_active;
    bool first_boot_complete;
    bool network_active;
    bool https_management_ready;
    uint64_t source_generation;
    uint32_t update_count;
    uint32_t reset_count;
    uint32_t fail_count;
};

enum appliance_recovery_factory_reset_integration_status
appliance_recovery_factory_reset_integration_init(
    struct appliance_recovery_factory_reset_integration *integration);
enum appliance_recovery_factory_reset_integration_status
appliance_recovery_factory_reset_integration_publish(
    struct appliance_recovery_factory_reset_integration *integration,
    const struct appliance_recovery_factory_reset_integration_evidence *evidence);
enum appliance_recovery_factory_reset_integration_status
appliance_recovery_factory_reset_integration_complete(
    struct appliance_recovery_factory_reset_integration *integration);
enum appliance_recovery_factory_reset_integration_status
appliance_recovery_factory_reset_integration_fail_closed(
    struct appliance_recovery_factory_reset_integration *integration,
    enum appliance_recovery_factory_reset_integration_failure failure);
enum appliance_recovery_factory_reset_integration_status
appliance_recovery_factory_reset_integration_reset(
    struct appliance_recovery_factory_reset_integration *integration);
enum appliance_recovery_factory_reset_integration_status
appliance_recovery_factory_reset_integration_snapshot(
    const struct appliance_recovery_factory_reset_integration *integration,
    struct appliance_recovery_factory_reset_integration_snapshot *snapshot_out);
enum appliance_recovery_factory_reset_integration_status
appliance_recovery_factory_reset_integration_render(
    const struct appliance_recovery_factory_reset_integration_snapshot *snapshot,
    char *output,
    size_t capacity,
    size_t *output_length);
enum appliance_recovery_factory_reset_integration_state
appliance_recovery_factory_reset_integration_state(
    const struct appliance_recovery_factory_reset_integration *integration);
enum appliance_recovery_factory_reset_integration_failure
appliance_recovery_factory_reset_integration_failure(
    const struct appliance_recovery_factory_reset_integration *integration);
bool appliance_recovery_factory_reset_integration_published(
    const struct appliance_recovery_factory_reset_integration *integration);
const char *appliance_recovery_factory_reset_integration_state_string(
    enum appliance_recovery_factory_reset_integration_state state);
const char *appliance_recovery_factory_reset_integration_status_string(
    enum appliance_recovery_factory_reset_integration_status status);
const char *appliance_recovery_factory_reset_integration_failure_string(
    enum appliance_recovery_factory_reset_integration_failure failure);
const char *appliance_recovery_factory_reset_integration_operation_string(
    uint32_t operation);

#endif
