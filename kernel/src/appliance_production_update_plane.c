#include "appliance_production_update_plane.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

static bool appliance_production_update_plane_state_valid(
    enum appliance_production_update_plane_state state)
{
    return state >= APPLIANCE_PRODUCTION_UPDATE_PLANE_STATE_NONE &&
           state <= APPLIANCE_PRODUCTION_UPDATE_PLANE_STATE_FAILED;
}

static bool appliance_production_update_plane_status_valid(
    enum appliance_production_update_plane_status status)
{
    return status >= APPLIANCE_PRODUCTION_UPDATE_PLANE_OK &&
           status <= APPLIANCE_PRODUCTION_UPDATE_PLANE_BUFFER_TOO_SMALL;
}

static bool appliance_production_update_plane_initialized(
    const struct appliance_production_update_plane *plane)
{
    return plane != NULL &&
           plane->initialized &&
           plane->version == APPLIANCE_PRODUCTION_UPDATE_PLANE_VERSION &&
           appliance_production_update_plane_state_valid(plane->state) &&
           appliance_production_update_plane_status_valid(plane->last_status) &&
           plane->auth != NULL &&
           plane->management != NULL &&
           plane->policy != NULL;
}

static bool appliance_production_update_plane_available(
    const struct appliance_production_update_plane *plane)
{
    return appliance_production_auth_available(plane->auth) &&
           appliance_production_management_plane_ready(plane->management);
}

static enum appliance_production_update_plane_state
appliance_production_update_plane_derive_state(
    const struct appliance_production_update_plane *plane)
{
    if (!appliance_production_update_plane_available(plane)) {
        return APPLIANCE_PRODUCTION_UPDATE_PLANE_STATE_UNAVAILABLE;
    }
    if (plane->reboot_required) {
        return APPLIANCE_PRODUCTION_UPDATE_PLANE_STATE_REBOOT_REQUIRED;
    }
    if (plane->pending) {
        return APPLIANCE_PRODUCTION_UPDATE_PLANE_STATE_PENDING_CONFIRMATION;
    }
    if (appliance_production_management_plane_state(plane->management) ==
        APPLIANCE_PRODUCTION_MANAGEMENT_PLANE_STATE_DEGRADED) {
        return APPLIANCE_PRODUCTION_UPDATE_PLANE_STATE_DEGRADED;
    }
    return APPLIANCE_PRODUCTION_UPDATE_PLANE_STATE_READY;
}

static void appliance_production_update_plane_audit(
    struct appliance_production_update_plane *plane,
    enum appliance_production_update_plane_audit_outcome outcome,
    uint64_t token,
    uint64_t tick,
    const struct appliance_admin_auth_decision *decision)
{
    struct appliance_production_update_plane_audit_entry *entry;

    entry = &plane->audit[plane->audit_head];
    memset(entry, 0, sizeof(*entry));
    entry->sequence = plane->audit_next_sequence;
    entry->tick = tick;
    entry->token = token;
    entry->outcome = (uint32_t)outcome;
    entry->signer_id = plane->gate.signer_id;
    entry->policy_generation = plane->gate.policy_generation;
    if (decision != NULL) {
        entry->session_id = decision->session_id;
        entry->principal_id = decision->principal_id;
    }
    plane->audit_next_sequence++;
    plane->audit_head =
        (plane->audit_head + 1U) %
        APPLIANCE_PRODUCTION_UPDATE_PLANE_AUDIT_CAPACITY;
    plane->audit_count++;
    plane->audit_published = true;
}

static void appliance_production_update_plane_clear_pending(
    struct appliance_production_update_plane *plane)
{
    plane->pending = false;
    plane->pending_token = 0U;
    plane->pending_tick = 0U;
    memset(&plane->pending_manifest, 0, sizeof(plane->pending_manifest));
}

static uint64_t appliance_production_update_plane_issue_token(
    struct appliance_production_update_plane *plane,
    uint64_t now_tick)
{
    plane->confirmation_sequence++;
    return ((now_tick + 1U) << 16) ^ (plane->confirmation_sequence + 1U);
}

static bool appliance_production_update_plane_append(
    char *rendered,
    size_t *offset,
    const char *text,
    size_t capacity)
{
    size_t length = strlen(text);
    size_t index;

    if (*offset + length >= capacity) {
        return false;
    }
    for (index = 0U; index < length; ++index) {
        rendered[*offset + index] = text[index];
    }
    *offset += length;
    rendered[*offset] = '\0';
    return true;
}

static enum appliance_production_update_plane_status
appliance_production_update_plane_finish(
    struct appliance_production_update_plane *plane,
    enum appliance_production_update_plane_status status,
    char *output,
    size_t capacity,
    size_t *output_length)
{
    size_t offset = 0U;

    plane->last_status = status;
    plane->state = appliance_production_update_plane_derive_state(plane);
    *output_length = 0U;
    if (output == NULL || capacity == 0U) {
        return APPLIANCE_PRODUCTION_UPDATE_PLANE_BUFFER_TOO_SMALL;
    }
    if (!appliance_production_update_plane_append(
            output, &offset, "state=", capacity) ||
        !appliance_production_update_plane_append(
            output,
            &offset,
            appliance_production_update_plane_state_string(plane->state),
            capacity) ||
        !appliance_production_update_plane_append(
            output, &offset, " result=", capacity) ||
        !appliance_production_update_plane_append(
            output,
            &offset,
            appliance_production_update_plane_status_string(status),
            capacity)) {
        *output_length = 0U;
        return APPLIANCE_PRODUCTION_UPDATE_PLANE_BUFFER_TOO_SMALL;
    }
    *output_length = offset;
    return status;
}

static enum appliance_production_update_plane_status
appliance_production_update_plane_verify(
    struct appliance_production_update_plane *plane,
    const struct appliance_production_update_artifact *artifact,
    struct appliance_update_manifest *manifest_out)
{
    struct appliance_update_integrity_gate gate;

    if (artifact == NULL || artifact->manifest_text == NULL ||
        artifact->metadata == NULL || artifact->payload == NULL ||
        artifact->envelope == NULL) {
        return APPLIANCE_PRODUCTION_UPDATE_PLANE_VERIFY_FAILED;
    }
    if (appliance_update_manifest_parse(
            artifact->manifest_text,
            artifact->manifest_length,
            manifest_out) != APPLIANCE_UPDATE_MANIFEST_OK) {
        return APPLIANCE_PRODUCTION_UPDATE_PLANE_VERIFY_FAILED;
    }
    /*
     * The ITEM-331 integrity gate is single-use: one gate proves exactly one
     * candidate. Every request therefore verifies against a fresh gate and
     * only a verified result is retained as the plane's audit evidence.
     */
    memset(&gate, 0, sizeof(gate));
    if (appliance_update_integrity_init(&gate) !=
            APPLIANCE_UPDATE_INTEGRITY_OK ||
        appliance_update_integrity_verify(
            &gate,
            manifest_out,
            plane->policy,
            artifact->metadata,
            artifact->metadata_length,
            artifact->payload,
            artifact->payload_length,
            artifact->envelope,
            artifact->envelope_length) != APPLIANCE_UPDATE_INTEGRITY_OK ||
        !appliance_update_integrity_is_verified(&gate)) {
        return APPLIANCE_PRODUCTION_UPDATE_PLANE_VERIFY_FAILED;
    }
    plane->gate = gate;
    return APPLIANCE_PRODUCTION_UPDATE_PLANE_OK;
}

enum appliance_production_update_plane_status
appliance_production_update_plane_init(
    struct appliance_production_update_plane *plane,
    struct appliance_production_auth *auth,
    struct appliance_production_management_plane *management,
    const struct verification_policy *policy)
{
    if (plane == NULL || auth == NULL || management == NULL ||
        policy == NULL) {
        return APPLIANCE_PRODUCTION_UPDATE_PLANE_NULL;
    }
    memset(plane, 0, sizeof(*plane));
    plane->initialized = true;
    plane->version = APPLIANCE_PRODUCTION_UPDATE_PLANE_VERSION;
    plane->state = APPLIANCE_PRODUCTION_UPDATE_PLANE_STATE_PREPARED;
    plane->last_status = APPLIANCE_PRODUCTION_UPDATE_PLANE_OK;
    plane->auth = auth;
    plane->management = management;
    plane->policy = policy;
    (void)appliance_update_integrity_init(&plane->gate);
    plane->confirmation_window =
        APPLIANCE_PRODUCTION_UPDATE_PLANE_CONFIRMATION_TICKS;
    return APPLIANCE_PRODUCTION_UPDATE_PLANE_OK;
}

enum appliance_production_update_plane_status
appliance_production_update_plane_bind_reboot(
    struct appliance_production_update_plane *plane,
    const struct appliance_production_update_reboot_io *reboot)
{
    if (!appliance_production_update_plane_initialized(plane)) {
        return APPLIANCE_PRODUCTION_UPDATE_PLANE_NULL;
    }
    if (reboot == NULL || reboot->request == NULL) {
        return APPLIANCE_PRODUCTION_UPDATE_PLANE_NULL;
    }
    plane->reboot = *reboot;
    return APPLIANCE_PRODUCTION_UPDATE_PLANE_OK;
}

enum appliance_production_update_plane_status
appliance_production_update_plane_refresh(
    struct appliance_production_update_plane *plane)
{
    if (!appliance_production_update_plane_initialized(plane)) {
        return APPLIANCE_PRODUCTION_UPDATE_PLANE_NULL;
    }
    if (plane->state == APPLIANCE_PRODUCTION_UPDATE_PLANE_STATE_STOPPED ||
        plane->state == APPLIANCE_PRODUCTION_UPDATE_PLANE_STATE_FAILED) {
        return APPLIANCE_PRODUCTION_UPDATE_PLANE_BAD_STATE;
    }
    plane->state = appliance_production_update_plane_derive_state(plane);
    if (plane->state == APPLIANCE_PRODUCTION_UPDATE_PLANE_STATE_READY ||
        plane->state == APPLIANCE_PRODUCTION_UPDATE_PLANE_STATE_DEGRADED ||
        plane->state ==
            APPLIANCE_PRODUCTION_UPDATE_PLANE_STATE_PENDING_CONFIRMATION ||
        plane->state ==
            APPLIANCE_PRODUCTION_UPDATE_PLANE_STATE_REBOOT_REQUIRED) {
        plane->last_status = APPLIANCE_PRODUCTION_UPDATE_PLANE_OK;
        return APPLIANCE_PRODUCTION_UPDATE_PLANE_OK;
    }
    plane->last_status = APPLIANCE_PRODUCTION_UPDATE_PLANE_UNAVAILABLE;
    return APPLIANCE_PRODUCTION_UPDATE_PLANE_UNAVAILABLE;
}

bool appliance_production_update_plane_ready(
    const struct appliance_production_update_plane *plane)
{
    if (!appliance_production_update_plane_initialized(plane)) {
        return false;
    }
    if (plane->state == APPLIANCE_PRODUCTION_UPDATE_PLANE_STATE_STOPPED ||
        plane->state == APPLIANCE_PRODUCTION_UPDATE_PLANE_STATE_FAILED) {
        return false;
    }
    return appliance_production_update_plane_available(plane);
}

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
    size_t *output_length)
{
    struct appliance_admin_auth_decision decision;
    enum appliance_management_authorization_mutation mutation =
        APPLIANCE_MANAGEMENT_AUTHORIZATION_MUTATION_NONE;
    enum appliance_production_auth_status auth_status;
    struct appliance_update_manifest manifest;

    if (!appliance_production_update_plane_initialized(plane) ||
        output == NULL || output_length == NULL || target == NULL) {
        return APPLIANCE_PRODUCTION_UPDATE_PLANE_NULL;
    }
    *output_length = 0U;
    if (plane->state == APPLIANCE_PRODUCTION_UPDATE_PLANE_STATE_STOPPED ||
        plane->state == APPLIANCE_PRODUCTION_UPDATE_PLANE_STATE_FAILED ||
        plane->reboot_required) {
        plane->last_status = APPLIANCE_PRODUCTION_UPDATE_PLANE_BAD_STATE;
        return plane->last_status;
    }

    if (plane->pending &&
        now_tick > plane->pending_tick + plane->confirmation_window) {
        appliance_production_update_plane_clear_pending(plane);
        appliance_production_update_plane_audit(
            plane,
            APPLIANCE_PRODUCTION_UPDATE_PLANE_AUDIT_EXPIRED,
            plane->pending_token,
            now_tick,
            NULL);
        ++plane->expired_count;
        return appliance_production_update_plane_finish(
            plane,
            APPLIANCE_PRODUCTION_UPDATE_PLANE_EXPIRED,
            output,
            capacity,
            output_length);
    }

    if (!appliance_production_update_plane_available(plane)) {
        ++plane->unavailable_count;
        appliance_production_update_plane_audit(
            plane,
            APPLIANCE_PRODUCTION_UPDATE_PLANE_AUDIT_UNAVAILABLE,
            0U,
            now_tick,
            NULL);
        return appliance_production_update_plane_finish(
            plane,
            APPLIANCE_PRODUCTION_UPDATE_PLANE_UNAVAILABLE,
            output,
            capacity,
            output_length);
    }

    memset(&decision, 0, sizeof(decision));
    auth_status = appliance_production_auth_authorize_mutation(
        plane->auth,
        authorization_header,
        authorization_header_length,
        method,
        target,
        target_length,
        now_tick,
        &decision,
        &mutation);
    if (auth_status != APPLIANCE_PRODUCTION_AUTH_ALLOWED) {
        if (auth_status == APPLIANCE_PRODUCTION_AUTH_UNAVAILABLE) {
            ++plane->unavailable_count;
            appliance_production_update_plane_audit(
                plane,
                APPLIANCE_PRODUCTION_UPDATE_PLANE_AUDIT_UNAVAILABLE,
                0U,
                now_tick,
                &decision);
            return appliance_production_update_plane_finish(
                plane,
                APPLIANCE_PRODUCTION_UPDATE_PLANE_UNAVAILABLE,
                output,
                capacity,
                output_length);
        }
        ++plane->denied_count;
        appliance_production_update_plane_audit(
            plane,
            APPLIANCE_PRODUCTION_UPDATE_PLANE_AUDIT_DENIED,
            confirmation_token,
            now_tick,
            &decision);
        return appliance_production_update_plane_finish(
            plane,
            APPLIANCE_PRODUCTION_UPDATE_PLANE_DENIED,
            output,
            capacity,
            output_length);
    }

    if (mutation != APPLIANCE_MANAGEMENT_AUTHORIZATION_MUTATION_UPDATE_ACCEPT) {
        appliance_production_update_plane_audit(
            plane,
            APPLIANCE_PRODUCTION_UPDATE_PLANE_AUDIT_DENIED,
            0U,
            now_tick,
            &decision);
        return appliance_production_update_plane_finish(
            plane,
            APPLIANCE_PRODUCTION_UPDATE_PLANE_UNSUPPORTED_TARGET,
            output,
            capacity,
            output_length);
    }

    if (cancel) {
        if (!plane->pending) {
            return appliance_production_update_plane_finish(
                plane,
                APPLIANCE_PRODUCTION_UPDATE_PLANE_NO_PENDING,
                output,
                capacity,
                output_length);
        }
        if (confirmation_token != plane->pending_token) {
            appliance_production_update_plane_audit(
                plane,
                APPLIANCE_PRODUCTION_UPDATE_PLANE_AUDIT_DENIED,
                confirmation_token,
                now_tick,
                &decision);
            return appliance_production_update_plane_finish(
                plane,
                APPLIANCE_PRODUCTION_UPDATE_PLANE_BAD_CONFIRMATION,
                output,
                capacity,
                output_length);
        }
        appliance_production_update_plane_audit(
            plane,
            APPLIANCE_PRODUCTION_UPDATE_PLANE_AUDIT_CANCELLED,
            plane->pending_token,
            now_tick,
            &decision);
        appliance_production_update_plane_clear_pending(plane);
        ++plane->cancelled_count;
        return appliance_production_update_plane_finish(
            plane,
            APPLIANCE_PRODUCTION_UPDATE_PLANE_CANCELLED,
            output,
            capacity,
            output_length);
    }

    if (confirmation_token != 0U) {
        if (!plane->pending || confirmation_token != plane->pending_token) {
            appliance_production_update_plane_audit(
                plane,
                APPLIANCE_PRODUCTION_UPDATE_PLANE_AUDIT_DENIED,
                confirmation_token,
                now_tick,
                &decision);
            return appliance_production_update_plane_finish(
                plane,
                APPLIANCE_PRODUCTION_UPDATE_PLANE_BAD_CONFIRMATION,
                output,
                capacity,
                output_length);
        }
        if (plane->reboot.request == NULL) {
            appliance_production_update_plane_audit(
                plane,
                APPLIANCE_PRODUCTION_UPDATE_PLANE_AUDIT_DENIED,
                confirmation_token,
                now_tick,
                &decision);
            appliance_production_update_plane_clear_pending(plane);
            return appliance_production_update_plane_finish(
                plane,
                APPLIANCE_PRODUCTION_UPDATE_PLANE_UNSUPPORTED_TARGET,
                output,
                capacity,
                output_length);
        }
        if (appliance_production_update_plane_verify(
                plane, artifact, &manifest) !=
            APPLIANCE_PRODUCTION_UPDATE_PLANE_OK) {
            appliance_production_update_plane_audit(
                plane,
                APPLIANCE_PRODUCTION_UPDATE_PLANE_AUDIT_VERIFY_FAILED,
                confirmation_token,
                now_tick,
                &decision);
            appliance_production_update_plane_clear_pending(plane);
            ++plane->rejected_count;
            return appliance_production_update_plane_finish(
                plane,
                APPLIANCE_PRODUCTION_UPDATE_PLANE_VERIFY_FAILED,
                output,
                capacity,
                output_length);
        }
        if (!plane->reboot.request(plane->reboot.context)) {
            appliance_production_update_plane_audit(
                plane,
                APPLIANCE_PRODUCTION_UPDATE_PLANE_AUDIT_DENIED,
                confirmation_token,
                now_tick,
                &decision);
            appliance_production_update_plane_clear_pending(plane);
            return appliance_production_update_plane_finish(
                plane,
                APPLIANCE_PRODUCTION_UPDATE_PLANE_APPLY_FAILED,
                output,
                capacity,
                output_length);
        }
        appliance_production_update_plane_audit(
            plane,
            APPLIANCE_PRODUCTION_UPDATE_PLANE_AUDIT_ALLOWED,
            confirmation_token,
            now_tick,
            &decision);
        appliance_production_update_plane_clear_pending(plane);
        plane->reboot_required = true;
        ++plane->accepted_count;
        return appliance_production_update_plane_finish(
            plane,
            APPLIANCE_PRODUCTION_UPDATE_PLANE_ACCEPTED,
            output,
            capacity,
            output_length);
    }

    if (appliance_production_update_plane_verify(
            plane, artifact, &manifest) !=
        APPLIANCE_PRODUCTION_UPDATE_PLANE_OK) {
        appliance_production_update_plane_audit(
            plane,
            APPLIANCE_PRODUCTION_UPDATE_PLANE_AUDIT_VERIFY_FAILED,
            0U,
            now_tick,
            &decision);
        ++plane->rejected_count;
        return appliance_production_update_plane_finish(
            plane,
            APPLIANCE_PRODUCTION_UPDATE_PLANE_VERIFY_FAILED,
            output,
            capacity,
            output_length);
    }

    plane->pending_manifest = manifest;
    plane->pending = true;
    plane->pending_token =
        appliance_production_update_plane_issue_token(plane, now_tick);
    plane->pending_tick = now_tick;
    return appliance_production_update_plane_finish(
        plane,
        APPLIANCE_PRODUCTION_UPDATE_PLANE_PENDING_CONFIRMATION,
        output,
        capacity,
        output_length);
}

enum appliance_production_update_plane_status
appliance_production_update_plane_stop(
    struct appliance_production_update_plane *plane)
{
    if (!appliance_production_update_plane_initialized(plane)) {
        return APPLIANCE_PRODUCTION_UPDATE_PLANE_NULL;
    }
    appliance_production_update_plane_clear_pending(plane);
    plane->state = APPLIANCE_PRODUCTION_UPDATE_PLANE_STATE_STOPPED;
    plane->last_status = APPLIANCE_PRODUCTION_UPDATE_PLANE_OK;
    return APPLIANCE_PRODUCTION_UPDATE_PLANE_OK;
}

enum appliance_production_update_plane_status
appliance_production_update_plane_fail_closed(
    struct appliance_production_update_plane *plane)
{
    if (!appliance_production_update_plane_initialized(plane)) {
        return APPLIANCE_PRODUCTION_UPDATE_PLANE_NULL;
    }
    appliance_production_update_plane_clear_pending(plane);
    plane->state = APPLIANCE_PRODUCTION_UPDATE_PLANE_STATE_FAILED;
    plane->last_status = APPLIANCE_PRODUCTION_UPDATE_PLANE_UNAVAILABLE;
    return APPLIANCE_PRODUCTION_UPDATE_PLANE_UNAVAILABLE;
}

enum appliance_production_update_plane_state
appliance_production_update_plane_state(
    const struct appliance_production_update_plane *plane)
{
    if (!appliance_production_update_plane_initialized(plane)) {
        return APPLIANCE_PRODUCTION_UPDATE_PLANE_STATE_UNINITIALIZED;
    }
    if (plane->state == APPLIANCE_PRODUCTION_UPDATE_PLANE_STATE_STOPPED ||
        plane->state == APPLIANCE_PRODUCTION_UPDATE_PLANE_STATE_FAILED ||
        plane->state ==
            APPLIANCE_PRODUCTION_UPDATE_PLANE_STATE_PENDING_CONFIRMATION ||
        plane->state ==
            APPLIANCE_PRODUCTION_UPDATE_PLANE_STATE_REBOOT_REQUIRED) {
        return plane->state;
    }
    return appliance_production_update_plane_derive_state(plane);
}

bool appliance_production_update_plane_has_pending(
    const struct appliance_production_update_plane *plane)
{
    if (!appliance_production_update_plane_initialized(plane)) {
        return false;
    }
    return plane->pending;
}

bool appliance_production_update_plane_reboot_required(
    const struct appliance_production_update_plane *plane)
{
    if (!appliance_production_update_plane_initialized(plane)) {
        return false;
    }
    return plane->reboot_required;
}

uint64_t appliance_production_update_plane_pending_token(
    const struct appliance_production_update_plane *plane)
{
    if (!appliance_production_update_plane_has_pending(plane)) {
        return 0U;
    }
    return plane->pending_token;
}

uint32_t appliance_production_update_plane_accepted_count(
    const struct appliance_production_update_plane *plane)
{
    if (!appliance_production_update_plane_initialized(plane)) {
        return 0U;
    }
    return plane->accepted_count;
}

uint32_t appliance_production_update_plane_denied_count(
    const struct appliance_production_update_plane *plane)
{
    if (!appliance_production_update_plane_initialized(plane)) {
        return 0U;
    }
    return plane->denied_count;
}

uint32_t appliance_production_update_plane_cancelled_count(
    const struct appliance_production_update_plane *plane)
{
    if (!appliance_production_update_plane_initialized(plane)) {
        return 0U;
    }
    return plane->cancelled_count;
}

uint32_t appliance_production_update_plane_expired_count(
    const struct appliance_production_update_plane *plane)
{
    if (!appliance_production_update_plane_initialized(plane)) {
        return 0U;
    }
    return plane->expired_count;
}

uint32_t appliance_production_update_plane_unavailable_count(
    const struct appliance_production_update_plane *plane)
{
    if (!appliance_production_update_plane_initialized(plane)) {
        return 0U;
    }
    return plane->unavailable_count;
}

uint32_t appliance_production_update_plane_rejected_count(
    const struct appliance_production_update_plane *plane)
{
    if (!appliance_production_update_plane_initialized(plane)) {
        return 0U;
    }
    return plane->rejected_count;
}

uint32_t appliance_production_update_plane_audit_count(
    const struct appliance_production_update_plane *plane)
{
    if (!appliance_production_update_plane_initialized(plane)) {
        return 0U;
    }
    return plane->audit_count;
}

bool appliance_production_update_plane_audit_entry(
    const struct appliance_production_update_plane *plane,
    uint32_t index,
    struct appliance_production_update_plane_audit_entry *entry_out)
{
    uint32_t oldest;

    if (!appliance_production_update_plane_initialized(plane) ||
        entry_out == NULL || index >= plane->audit_count) {
        return false;
    }
    if (plane->audit_count > APPLIANCE_PRODUCTION_UPDATE_PLANE_AUDIT_CAPACITY) {
        oldest = plane->audit_count -
                 APPLIANCE_PRODUCTION_UPDATE_PLANE_AUDIT_CAPACITY;
        if (index < oldest) {
            return false;
        }
        index -= oldest;
    }
    *entry_out = plane->audit[(plane->audit_head +
                               APPLIANCE_PRODUCTION_UPDATE_PLANE_AUDIT_CAPACITY -
                               plane->audit_count + index) %
                              APPLIANCE_PRODUCTION_UPDATE_PLANE_AUDIT_CAPACITY];
    return true;
}

const char *appliance_production_update_plane_state_string(
    enum appliance_production_update_plane_state state)
{
    switch (state) {
    case APPLIANCE_PRODUCTION_UPDATE_PLANE_STATE_NONE:
        return "NONE";
    case APPLIANCE_PRODUCTION_UPDATE_PLANE_STATE_UNINITIALIZED:
        return "UNINITIALIZED";
    case APPLIANCE_PRODUCTION_UPDATE_PLANE_STATE_PREPARED:
        return "PREPARED";
    case APPLIANCE_PRODUCTION_UPDATE_PLANE_STATE_UNAVAILABLE:
        return "UNAVAILABLE";
    case APPLIANCE_PRODUCTION_UPDATE_PLANE_STATE_READY:
        return "READY";
    case APPLIANCE_PRODUCTION_UPDATE_PLANE_STATE_DEGRADED:
        return "DEGRADED";
    case APPLIANCE_PRODUCTION_UPDATE_PLANE_STATE_PENDING_CONFIRMATION:
        return "PENDING_CONFIRMATION";
    case APPLIANCE_PRODUCTION_UPDATE_PLANE_STATE_REBOOT_REQUIRED:
        return "REBOOT_REQUIRED";
    case APPLIANCE_PRODUCTION_UPDATE_PLANE_STATE_STOPPED:
        return "STOPPED";
    case APPLIANCE_PRODUCTION_UPDATE_PLANE_STATE_FAILED:
        return "FAILED";
    default:
        return "UNKNOWN";
    }
}

const char *appliance_production_update_plane_status_string(
    enum appliance_production_update_plane_status status)
{
    switch (status) {
    case APPLIANCE_PRODUCTION_UPDATE_PLANE_OK:
        return "OK";
    case APPLIANCE_PRODUCTION_UPDATE_PLANE_NULL:
        return "NULL";
    case APPLIANCE_PRODUCTION_UPDATE_PLANE_BAD_STATE:
        return "BAD_STATE";
    case APPLIANCE_PRODUCTION_UPDATE_PLANE_BAD_VERSION:
        return "BAD_VERSION";
    case APPLIANCE_PRODUCTION_UPDATE_PLANE_UNAVAILABLE:
        return "UNAVAILABLE";
    case APPLIANCE_PRODUCTION_UPDATE_PLANE_DENIED:
        return "DENIED";
    case APPLIANCE_PRODUCTION_UPDATE_PLANE_PENDING_CONFIRMATION:
        return "PENDING_CONFIRMATION";
    case APPLIANCE_PRODUCTION_UPDATE_PLANE_ACCEPTED:
        return "ACCEPTED";
    case APPLIANCE_PRODUCTION_UPDATE_PLANE_REBOOT_REQUIRED:
        return "REBOOT_REQUIRED";
    case APPLIANCE_PRODUCTION_UPDATE_PLANE_CANCELLED:
        return "CANCELLED";
    case APPLIANCE_PRODUCTION_UPDATE_PLANE_EXPIRED:
        return "EXPIRED";
    case APPLIANCE_PRODUCTION_UPDATE_PLANE_NO_PENDING:
        return "NO_PENDING";
    case APPLIANCE_PRODUCTION_UPDATE_PLANE_BAD_CONFIRMATION:
        return "BAD_CONFIRMATION";
    case APPLIANCE_PRODUCTION_UPDATE_PLANE_UNSUPPORTED_TARGET:
        return "UNSUPPORTED_TARGET";
    case APPLIANCE_PRODUCTION_UPDATE_PLANE_VERIFY_FAILED:
        return "VERIFY_FAILED";
    case APPLIANCE_PRODUCTION_UPDATE_PLANE_APPLY_FAILED:
        return "APPLY_FAILED";
    case APPLIANCE_PRODUCTION_UPDATE_PLANE_BUFFER_TOO_SMALL:
        return "BUFFER_TOO_SMALL";
    default:
        return "UNKNOWN";
    }
}

const char *appliance_production_update_plane_audit_outcome_string(
    enum appliance_production_update_plane_audit_outcome outcome)
{
    switch (outcome) {
    case APPLIANCE_PRODUCTION_UPDATE_PLANE_AUDIT_NONE:
        return "NONE";
    case APPLIANCE_PRODUCTION_UPDATE_PLANE_AUDIT_ALLOWED:
        return "ALLOWED";
    case APPLIANCE_PRODUCTION_UPDATE_PLANE_AUDIT_DENIED:
        return "DENIED";
    case APPLIANCE_PRODUCTION_UPDATE_PLANE_AUDIT_UNAVAILABLE:
        return "UNAVAILABLE";
    case APPLIANCE_PRODUCTION_UPDATE_PLANE_AUDIT_CANCELLED:
        return "CANCELLED";
    case APPLIANCE_PRODUCTION_UPDATE_PLANE_AUDIT_EXPIRED:
        return "EXPIRED";
    case APPLIANCE_PRODUCTION_UPDATE_PLANE_AUDIT_VERIFY_FAILED:
        return "VERIFY_FAILED";
    default:
        return "UNKNOWN";
    }
}
