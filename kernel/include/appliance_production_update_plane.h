#ifndef HFOC_APPLIANCE_PRODUCTION_UPDATE_PLANE_H
#define HFOC_APPLIANCE_PRODUCTION_UPDATE_PLANE_H

#include "appliance_production_auth.h"
#include "appliance_production_management_plane.h"
#include "appliance_update_integrity.h"
#include "appliance_update_manifest.h"
#include "verification_policy.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * ITEM-367B production update operations.
 *
 * This caller-owned, allocation-free composition binds the ITEM-365
 * production request gate (`appliance_production_auth`), the ITEM-366
 * read-only management plane (`appliance_production_management_plane`) and the
 * existing signed-artifact boundaries (`appliance_update_manifest`,
 * `appliance_update_integrity`) so an authorized administrator can accept or
 * reject a signed update without introducing a second device registry, route
 * table, trust store, or verification policy.
 *
 * An update is two-phase and never partially accepted:
 *   1. an authorized request stages exactly one signed candidate only after the
 *      manifest parses and the signed envelope verifies against the bound
 *      verification policy; a single-use confirmation token with a bounded
 *      lifetime is issued;
 *   2. only a second authorized request carrying that exact token accepts the
 *      candidate, while an explicit cancel or an expired window discards it.
 *
 * Reboot behaviour is published honestly: an accepted candidate sets
 * `reboot_required`, but the plane never claims that a reboot happened. It
 * exposes a reboot request to a caller-bound executor and reports
 * `UNSUPPORTED_TARGET` when no executor is bound, mirroring the ITEM-367A
 * refusal of an unbound service executor.
 */

#define APPLIANCE_PRODUCTION_UPDATE_PLANE_VERSION 1U
#define APPLIANCE_PRODUCTION_UPDATE_PLANE_OUTPUT_MAX 192U
#define APPLIANCE_PRODUCTION_UPDATE_PLANE_AUDIT_CAPACITY 16U
#define APPLIANCE_PRODUCTION_UPDATE_PLANE_CONFIRMATION_TICKS 300U

enum appliance_production_update_plane_state {
    APPLIANCE_PRODUCTION_UPDATE_PLANE_STATE_NONE = 0,
    APPLIANCE_PRODUCTION_UPDATE_PLANE_STATE_UNINITIALIZED = 1,
    APPLIANCE_PRODUCTION_UPDATE_PLANE_STATE_PREPARED = 2,
    APPLIANCE_PRODUCTION_UPDATE_PLANE_STATE_UNAVAILABLE = 3,
    APPLIANCE_PRODUCTION_UPDATE_PLANE_STATE_READY = 4,
    APPLIANCE_PRODUCTION_UPDATE_PLANE_STATE_DEGRADED = 5,
    APPLIANCE_PRODUCTION_UPDATE_PLANE_STATE_PENDING_CONFIRMATION = 6,
    APPLIANCE_PRODUCTION_UPDATE_PLANE_STATE_REBOOT_REQUIRED = 7,
    APPLIANCE_PRODUCTION_UPDATE_PLANE_STATE_STOPPED = 8,
    APPLIANCE_PRODUCTION_UPDATE_PLANE_STATE_FAILED = 9,
};

enum appliance_production_update_plane_status {
    APPLIANCE_PRODUCTION_UPDATE_PLANE_OK = 0,
    APPLIANCE_PRODUCTION_UPDATE_PLANE_NULL = 1,
    APPLIANCE_PRODUCTION_UPDATE_PLANE_BAD_STATE = 2,
    APPLIANCE_PRODUCTION_UPDATE_PLANE_BAD_VERSION = 3,
    APPLIANCE_PRODUCTION_UPDATE_PLANE_UNAVAILABLE = 4,
    APPLIANCE_PRODUCTION_UPDATE_PLANE_DENIED = 5,
    APPLIANCE_PRODUCTION_UPDATE_PLANE_PENDING_CONFIRMATION = 6,
    APPLIANCE_PRODUCTION_UPDATE_PLANE_ACCEPTED = 7,
    APPLIANCE_PRODUCTION_UPDATE_PLANE_REBOOT_REQUIRED = 8,
    APPLIANCE_PRODUCTION_UPDATE_PLANE_CANCELLED = 9,
    APPLIANCE_PRODUCTION_UPDATE_PLANE_EXPIRED = 10,
    APPLIANCE_PRODUCTION_UPDATE_PLANE_NO_PENDING = 11,
    APPLIANCE_PRODUCTION_UPDATE_PLANE_BAD_CONFIRMATION = 12,
    APPLIANCE_PRODUCTION_UPDATE_PLANE_UNSUPPORTED_TARGET = 13,
    APPLIANCE_PRODUCTION_UPDATE_PLANE_VERIFY_FAILED = 14,
    APPLIANCE_PRODUCTION_UPDATE_PLANE_APPLY_FAILED = 15,
    APPLIANCE_PRODUCTION_UPDATE_PLANE_BUFFER_TOO_SMALL = 16,
};

enum appliance_production_update_plane_audit_outcome {
    APPLIANCE_PRODUCTION_UPDATE_PLANE_AUDIT_NONE = 0,
    APPLIANCE_PRODUCTION_UPDATE_PLANE_AUDIT_ALLOWED = 1,
    APPLIANCE_PRODUCTION_UPDATE_PLANE_AUDIT_DENIED = 2,
    APPLIANCE_PRODUCTION_UPDATE_PLANE_AUDIT_UNAVAILABLE = 3,
    APPLIANCE_PRODUCTION_UPDATE_PLANE_AUDIT_CANCELLED = 4,
    APPLIANCE_PRODUCTION_UPDATE_PLANE_AUDIT_EXPIRED = 5,
    APPLIANCE_PRODUCTION_UPDATE_PLANE_AUDIT_VERIFY_FAILED = 6,
};

struct appliance_production_update_plane_audit_entry {
    uint64_t sequence;
    uint64_t tick;
    uint64_t token;
    uint32_t outcome;
    uint32_t signer_id;
    uint32_t policy_generation;
    uint32_t session_id;
    uint32_t principal_id;
};

/*
 * Caller-owned bundle of one signed update candidate. Every pointer is
 * borrowed for the duration of the request; the plane copies nothing and owns
 * no payload storage.
 */
struct appliance_production_update_artifact {
    const char *manifest_text;
    size_t manifest_length;
    const void *metadata;
    size_t metadata_length;
    const void *payload;
    size_t payload_length;
    const void *envelope;
    size_t envelope_length;
};

/*
 * Reboot executor boundary. `request` returns true when the reboot request was
 * accepted by the platform; the plane records the intent and never reports
 * that a reboot happened without this acknowledgement.
 */
typedef bool (*appliance_production_update_reboot_fn)(void *context);

struct appliance_production_update_reboot_io {
    void *context;
    appliance_production_update_reboot_fn request;
};

struct appliance_production_update_plane {
    bool initialized;
    uint32_t version;
    enum appliance_production_update_plane_state state;
    enum appliance_production_update_plane_status last_status;
    struct appliance_production_auth *auth;
    struct appliance_production_management_plane *management;
    const struct verification_policy *policy;
    struct appliance_update_integrity_gate gate;
    struct appliance_update_manifest pending_manifest;
    struct appliance_production_update_reboot_io reboot;
    bool pending;
    bool reboot_required;
    uint64_t confirmation_sequence;
    uint64_t pending_token;
    uint64_t pending_tick;
    uint64_t confirmation_window;
    uint32_t accepted_count;
    uint32_t denied_count;
    uint32_t cancelled_count;
    uint32_t expired_count;
    uint32_t unavailable_count;
    uint32_t rejected_count;
    bool audit_published;
    uint64_t audit_next_sequence;
    uint32_t audit_count;
    uint32_t audit_head;
    struct appliance_production_update_plane_audit_entry audit[
        APPLIANCE_PRODUCTION_UPDATE_PLANE_AUDIT_CAPACITY];
};

enum appliance_production_update_plane_status
appliance_production_update_plane_init(
    struct appliance_production_update_plane *plane,
    struct appliance_production_auth *auth,
    struct appliance_production_management_plane *management,
    const struct verification_policy *policy);
enum appliance_production_update_plane_status
appliance_production_update_plane_bind_reboot(
    struct appliance_production_update_plane *plane,
    const struct appliance_production_update_reboot_io *reboot);
enum appliance_production_update_plane_status
appliance_production_update_plane_refresh(
    struct appliance_production_update_plane *plane);
bool appliance_production_update_plane_ready(
    const struct appliance_production_update_plane *plane);
enum appliance_production_update_plane_status
appliance_production_update_plane_request(
    struct appliance_production_update_plane *plane,
    const uint8_t *authorization_header,
    uint32_t authorization_header_length,
    enum appliance_management_authorization_method method,
    const uint8_t *target,
    uint32_t target_length,
    const struct appliance_production_update_artifact *artifact,
    uint64_t confirmation_token,
    bool cancel,
    uint64_t now_tick,
    char *output,
    size_t capacity,
    size_t *output_length);
enum appliance_production_update_plane_status
appliance_production_update_plane_stop(
    struct appliance_production_update_plane *plane);
enum appliance_production_update_plane_status
appliance_production_update_plane_fail_closed(
    struct appliance_production_update_plane *plane);
enum appliance_production_update_plane_state
appliance_production_update_plane_state(
    const struct appliance_production_update_plane *plane);
bool appliance_production_update_plane_has_pending(
    const struct appliance_production_update_plane *plane);
bool appliance_production_update_plane_reboot_required(
    const struct appliance_production_update_plane *plane);
uint64_t appliance_production_update_plane_pending_token(
    const struct appliance_production_update_plane *plane);
uint32_t appliance_production_update_plane_accepted_count(
    const struct appliance_production_update_plane *plane);
uint32_t appliance_production_update_plane_denied_count(
    const struct appliance_production_update_plane *plane);
uint32_t appliance_production_update_plane_cancelled_count(
    const struct appliance_production_update_plane *plane);
uint32_t appliance_production_update_plane_expired_count(
    const struct appliance_production_update_plane *plane);
uint32_t appliance_production_update_plane_unavailable_count(
    const struct appliance_production_update_plane *plane);
uint32_t appliance_production_update_plane_rejected_count(
    const struct appliance_production_update_plane *plane);
uint32_t appliance_production_update_plane_audit_count(
    const struct appliance_production_update_plane *plane);
bool appliance_production_update_plane_audit_entry(
    const struct appliance_production_update_plane *plane,
    uint32_t index,
    struct appliance_production_update_plane_audit_entry *entry_out);
const char *appliance_production_update_plane_state_string(
    enum appliance_production_update_plane_state state);
const char *appliance_production_update_plane_status_string(
    enum appliance_production_update_plane_status status);
const char *appliance_production_update_plane_audit_outcome_string(
    enum appliance_production_update_plane_audit_outcome outcome);

#endif
