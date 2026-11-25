#ifndef HFOC_APPLIANCE_PRODUCTION_MANAGEMENT_H
#define HFOC_APPLIANCE_PRODUCTION_MANAGEMENT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define APPLIANCE_PRODUCTION_MANAGEMENT_VERSION 1U
#define APPLIANCE_PRODUCTION_MANAGEMENT_EVIDENCE_VERSION 1U
#define APPLIANCE_PRODUCTION_MANAGEMENT_OUTPUT_MAX 640U

/* The attestation vocabulary the management plane publishes.  It mirrors
 * the boot-attestation verdicts without linking the verification modules
 * into the management composition. */
enum appliance_production_management_attestation {
    APPLIANCE_PRODUCTION_MANAGEMENT_ATTESTATION_UNKNOWN = 0,
    APPLIANCE_PRODUCTION_MANAGEMENT_ATTESTATION_ATTESTED = 1,
    APPLIANCE_PRODUCTION_MANAGEMENT_ATTESTATION_UNATTESTED = 2,
    APPLIANCE_PRODUCTION_MANAGEMENT_ATTESTATION_UNAVAILABLE = 3,
};

/* ITEM-377 audit integrity view.  UNAVAILABLE is the honest default when no
 * chain was published; TAMPERED is published only by a real verification. */
enum appliance_production_management_audit {
    APPLIANCE_PRODUCTION_MANAGEMENT_AUDIT_UNKNOWN = 0,
    APPLIANCE_PRODUCTION_MANAGEMENT_AUDIT_OK = 1,
    APPLIANCE_PRODUCTION_MANAGEMENT_AUDIT_TAMPERED = 2,
    APPLIANCE_PRODUCTION_MANAGEMENT_AUDIT_UNAVAILABLE = 3,
};

/* ITEM-378 support plane view.  UNAVAILABLE is the honest default when no
 * bundle was published; UNSIGNED means the bundle exists without a bound seal
 * key and is never rendered as sealed. */
enum appliance_production_management_support {
    APPLIANCE_PRODUCTION_MANAGEMENT_SUPPORT_UNKNOWN = 0,
    APPLIANCE_PRODUCTION_MANAGEMENT_SUPPORT_SEALED = 1,
    APPLIANCE_PRODUCTION_MANAGEMENT_SUPPORT_UNSIGNED = 2,
    APPLIANCE_PRODUCTION_MANAGEMENT_SUPPORT_REDACTION_FAILED = 3,
    APPLIANCE_PRODUCTION_MANAGEMENT_SUPPORT_UNAVAILABLE = 4,
};

enum appliance_production_management_state {
    APPLIANCE_PRODUCTION_MANAGEMENT_STATE_NONE = 0,
    APPLIANCE_PRODUCTION_MANAGEMENT_STATE_UNINITIALIZED = 1,
    APPLIANCE_PRODUCTION_MANAGEMENT_STATE_PREPARED = 2,
    APPLIANCE_PRODUCTION_MANAGEMENT_STATE_AWAITING_EVIDENCE = 3,
    APPLIANCE_PRODUCTION_MANAGEMENT_STATE_ACTIVE = 4,
    APPLIANCE_PRODUCTION_MANAGEMENT_STATE_DEGRADED = 5,
    APPLIANCE_PRODUCTION_MANAGEMENT_STATE_STOPPED = 6,
    APPLIANCE_PRODUCTION_MANAGEMENT_STATE_FAILED = 7,
};

enum appliance_production_management_status {
    APPLIANCE_PRODUCTION_MANAGEMENT_OK = 0,
    APPLIANCE_PRODUCTION_MANAGEMENT_NULL = 1,
    APPLIANCE_PRODUCTION_MANAGEMENT_BAD_STATE = 2,
    APPLIANCE_PRODUCTION_MANAGEMENT_BAD_VERSION = 3,
    APPLIANCE_PRODUCTION_MANAGEMENT_ALREADY_PUBLISHED = 4,
    APPLIANCE_PRODUCTION_MANAGEMENT_UNPUBLISHED = 5,
    APPLIANCE_PRODUCTION_MANAGEMENT_INVALID_EVIDENCE = 6,
    APPLIANCE_PRODUCTION_MANAGEMENT_STALE_EVIDENCE = 7,
    APPLIANCE_PRODUCTION_MANAGEMENT_MISSING_EVIDENCE = 8,
    APPLIANCE_PRODUCTION_MANAGEMENT_STOP_FAILED = 9,
    APPLIANCE_PRODUCTION_MANAGEMENT_BUFFER_TOO_SMALL = 10,
};

enum appliance_production_management_failure {
    APPLIANCE_PRODUCTION_MANAGEMENT_FAILURE_NONE = 0,
    APPLIANCE_PRODUCTION_MANAGEMENT_FAILURE_EVIDENCE = 1,
    APPLIANCE_PRODUCTION_MANAGEMENT_FAILURE_STATE = 2,
    APPLIANCE_PRODUCTION_MANAGEMENT_FAILURE_NETWORK = 3,
    APPLIANCE_PRODUCTION_MANAGEMENT_FAILURE_SERVICES = 4,
    APPLIANCE_PRODUCTION_MANAGEMENT_FAILURE_HTTPS = 5,
    APPLIANCE_PRODUCTION_MANAGEMENT_FAILURE_STOP = 6,
};

enum appliance_production_management_health {
    APPLIANCE_PRODUCTION_MANAGEMENT_HEALTH_UNKNOWN = 0,
    APPLIANCE_PRODUCTION_MANAGEMENT_HEALTH_PASS = 1,
    APPLIANCE_PRODUCTION_MANAGEMENT_HEALTH_WARN = 2,
    APPLIANCE_PRODUCTION_MANAGEMENT_HEALTH_FAIL = 3,
};

struct appliance_production_management_evidence {
    uint32_t version;
    uint64_t source_generation;
    bool published;
    bool system_identity_present;
    bool boot_attestation_present;
    bool configuration_ready;
    bool network_required;
    bool network_active;
    bool network_health_ok;
    bool production_services_ready;
    bool https_management_ready;
    bool authorization_available;
    bool degraded_requested;
    enum appliance_production_management_attestation boot_attestation;
    bool audit_chain_present;
    enum appliance_production_management_audit audit_chain;
    uint64_t audit_records;
    uint64_t audit_head;
    bool support_present;
    enum appliance_production_management_support support_bundle;
    uint32_t support_withheld;
    bool support_session_active;
    bool support_break_glass_armed;
};

struct appliance_production_management {
    bool initialized;
    uint32_t version;
    enum appliance_production_management_state state;
    enum appliance_production_management_status last_status;
    enum appliance_production_management_failure failure;
    uint64_t source_generation;
    bool evidence_published;
    bool network_active;
    bool production_services_ready;
    bool https_management_ready;
    bool authorization_available;
    enum appliance_production_management_attestation boot_attestation;
    enum appliance_production_management_audit audit_chain;
    uint64_t audit_records;
    uint64_t audit_head;
    enum appliance_production_management_support support_bundle;
    uint32_t support_withheld;
    bool support_session_active;
    bool support_break_glass_armed;
    bool degraded;
    uint32_t update_count;
    uint32_t stop_count;
    uint32_t reset_count;
};

struct appliance_production_management_snapshot {
    bool published;
    uint32_t version;
    enum appliance_production_management_state state;
    enum appliance_production_management_status last_status;
    enum appliance_production_management_failure failure;
    enum appliance_production_management_health health;
    bool network_active;
    bool production_services_ready;
    bool https_management_ready;
    bool authorization_available;
    enum appliance_production_management_attestation boot_attestation;
    enum appliance_production_management_audit audit_chain;
    uint64_t audit_records;
    uint64_t audit_head;
    enum appliance_production_management_support support_bundle;
    uint32_t support_withheld;
    bool support_session_active;
    bool support_break_glass_armed;
    uint64_t source_generation;
    uint32_t update_count;
    uint32_t stop_count;
    uint32_t reset_count;
};

enum appliance_production_management_status
appliance_production_management_init(
    struct appliance_production_management *management);
enum appliance_production_management_status
appliance_production_management_publish(
    struct appliance_production_management *management,
    const struct appliance_production_management_evidence *evidence);
enum appliance_production_management_status
appliance_production_management_stop(
    struct appliance_production_management *management);
enum appliance_production_management_status
appliance_production_management_fail_closed(
    struct appliance_production_management *management,
    enum appliance_production_management_failure failure);
enum appliance_production_management_status
appliance_production_management_reset(
    struct appliance_production_management *management);
enum appliance_production_management_status
appliance_production_management_snapshot(
    const struct appliance_production_management *management,
    struct appliance_production_management_snapshot *snapshot_out);
const char *appliance_production_management_attestation_string(
    enum appliance_production_management_attestation attestation);
const char *appliance_production_management_audit_string(
    enum appliance_production_management_audit audit);
const char *appliance_production_management_support_string(
    enum appliance_production_management_support support);

enum appliance_production_management_status
appliance_production_management_render(
    const struct appliance_production_management_snapshot *snapshot,
    char *output,
    size_t capacity,
    size_t *output_length);
enum appliance_production_management_state
appliance_production_management_state(
    const struct appliance_production_management *management);
enum appliance_production_management_failure
appliance_production_management_failure(
    const struct appliance_production_management *management);
bool appliance_production_management_published(
    const struct appliance_production_management *management);
const char *appliance_production_management_state_string(
    enum appliance_production_management_state state);
const char *appliance_production_management_status_string(
    enum appliance_production_management_status status);
const char *appliance_production_management_failure_string(
    enum appliance_production_management_failure failure);

#endif
