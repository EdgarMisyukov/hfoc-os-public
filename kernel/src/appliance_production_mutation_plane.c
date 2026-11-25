#include "appliance_production_mutation_plane.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

static bool appliance_production_mutation_plane_state_valid(
    enum appliance_production_mutation_plane_state state)
{
    return state >= APPLIANCE_PRODUCTION_MUTATION_PLANE_STATE_NONE &&
           state <= APPLIANCE_PRODUCTION_MUTATION_PLANE_STATE_FAILED;
}

static bool appliance_production_mutation_plane_status_valid(
    enum appliance_production_mutation_plane_status status)
{
    return status >= APPLIANCE_PRODUCTION_MUTATION_PLANE_OK &&
           status <= APPLIANCE_PRODUCTION_MUTATION_PLANE_BUFFER_TOO_SMALL;
}

static bool appliance_production_mutation_plane_initialized(
    const struct appliance_production_mutation_plane *plane)
{
    return plane != NULL &&
           plane->initialized &&
           plane->version == APPLIANCE_PRODUCTION_MUTATION_PLANE_VERSION &&
           appliance_production_mutation_plane_state_valid(plane->state) &&
           appliance_production_mutation_plane_status_valid(
               plane->last_status) &&
           plane->auth != NULL;
}

/*
 * ITEM-503: the management plane and the network transaction are optional
 * bindings.  A production composition that publishes no read-only management
 * plane still has an available mutation plane when the administrator gate is
 * available; a mutation whose executor is not bound is refused as an
 * unsupported target instead of being reported as applied.
 */
static bool appliance_production_mutation_plane_network_ready(
    const struct appliance_production_mutation_plane *plane)
{
    return plane->network != NULL;
}

static bool appliance_production_mutation_plane_available(
    const struct appliance_production_mutation_plane *plane)
{
    return appliance_production_auth_available(plane->auth) &&
           (plane->management == NULL ||
            appliance_production_management_plane_ready(plane->management));
}

static enum appliance_production_mutation_plane_state
appliance_production_mutation_plane_derive_state(
    const struct appliance_production_mutation_plane *plane)
{
    if (!appliance_production_mutation_plane_available(plane)) {
        return APPLIANCE_PRODUCTION_MUTATION_PLANE_STATE_UNAVAILABLE;
    }
    if (plane->pending_mutation !=
        APPLIANCE_MANAGEMENT_AUTHORIZATION_MUTATION_NONE) {
        return APPLIANCE_PRODUCTION_MUTATION_PLANE_STATE_PENDING_CONFIRMATION;
    }
    if (plane->management != NULL &&
        appliance_production_management_plane_state(plane->management) ==
            APPLIANCE_PRODUCTION_MANAGEMENT_PLANE_STATE_DEGRADED) {
        return APPLIANCE_PRODUCTION_MUTATION_PLANE_STATE_DEGRADED;
    }
    return APPLIANCE_PRODUCTION_MUTATION_PLANE_STATE_READY;
}

static void appliance_production_mutation_plane_audit(
    struct appliance_production_mutation_plane *plane,
    enum appliance_production_mutation_plane_audit_outcome outcome,
    enum appliance_management_authorization_mutation mutation,
    uint64_t token,
    uint64_t tick,
    const struct appliance_admin_auth_decision *decision)
{
    struct appliance_production_mutation_plane_audit_entry *entry;

    entry = &plane->audit[plane->audit_head];
    memset(entry, 0, sizeof(*entry));
    entry->sequence = plane->audit_next_sequence;
    entry->tick = tick;
    entry->token = token;
    entry->mutation = (uint32_t)mutation;
    entry->outcome = (uint32_t)outcome;
    if (decision != NULL) {
        entry->session_id = decision->session_id;
        entry->principal_id = decision->principal_id;
    }
    plane->audit_next_sequence++;
    plane->audit_head =
        (plane->audit_head + 1U) %
        APPLIANCE_PRODUCTION_MUTATION_PLANE_AUDIT_CAPACITY;
    plane->audit_count++;
    plane->audit_published = true;
}

static void appliance_production_mutation_plane_clear_pending(
    struct appliance_production_mutation_plane *plane)
{
    plane->pending_token = 0U;
    plane->pending_tick = 0U;
    plane->pending_mutation =
        APPLIANCE_MANAGEMENT_AUTHORIZATION_MUTATION_NONE;
}

/*
 * ITEM-503: roll back whichever executor owns the pending candidate.  A
 * pending mutation whose executor is not bound has nothing to roll back; the
 * caller refuses it before staging.
 *
 * ITEM-505: the network candidate belongs to the caller's executor when one
 * is bound; the borrowed RAM-only transaction is the fallback for the
 * compositions that still stage through it.
 */
static void appliance_production_mutation_plane_rollback_pending(
    struct appliance_production_mutation_plane *plane)
{
    if (plane->pending_mutation ==
        APPLIANCE_MANAGEMENT_AUTHORIZATION_MUTATION_NETWORK_CONFIG) {
        if (plane->network_executor_bound &&
            plane->network_executor.rollback != NULL) {
            plane->network_executor.rollback(
                plane->network_executor.context);
        } else if (plane->network != NULL) {
            (void)network_config_transaction_rollback(plane->network);
        }
    } else if (plane->pending_mutation ==
                   APPLIANCE_MANAGEMENT_AUTHORIZATION_MUTATION_CERTIFICATE_IMPORT &&
               plane->certificate_bound &&
               plane->certificate.rollback != NULL) {
        plane->certificate.rollback(plane->certificate.context);
    } else if (plane->pending_mutation ==
                   APPLIANCE_MANAGEMENT_AUTHORIZATION_MUTATION_ROUTING_CONFIG &&
               plane->routing_bound &&
               plane->routing.rollback != NULL) {
        plane->routing.rollback(plane->routing.context);
    } else if (plane->pending_mutation ==
                   APPLIANCE_MANAGEMENT_AUTHORIZATION_MUTATION_SYSTEM_HOSTNAME &&
               plane->system_bound &&
               plane->system.rollback != NULL) {
        plane->system.rollback(plane->system.context);
    } else if (plane->pending_mutation ==
                   APPLIANCE_MANAGEMENT_AUTHORIZATION_MUTATION_ACCOUNT_CREDENTIAL &&
               plane->account_bound &&
               plane->account.rollback != NULL) {
        plane->account.rollback(plane->account.context);
    } else if (plane->pending_mutation ==
                   APPLIANCE_MANAGEMENT_AUTHORIZATION_MUTATION_CERTIFICATE_NAME &&
               plane->certificate_name_bound &&
               plane->certificate_name.rollback != NULL) {
        plane->certificate_name.rollback(plane->certificate_name.context);
    } else if (plane->pending_mutation ==
                   APPLIANCE_MANAGEMENT_AUTHORIZATION_MUTATION_ACME_CONTROL &&
               plane->acme_bound &&
               plane->acme.rollback != NULL) {
        plane->acme.rollback(plane->acme.context);
    } else if (plane->pending_mutation ==
                   APPLIANCE_MANAGEMENT_AUTHORIZATION_MUTATION_PANEL_GATE &&
               plane->panel_gate_bound &&
               plane->panel_gate.rollback != NULL) {
        plane->panel_gate.rollback(plane->panel_gate.context);
    }
}

static bool appliance_production_mutation_plane_certificate_ready(
    const struct appliance_production_mutation_plane *plane)
{
    return plane->certificate_bound &&
           plane->certificate.stage != NULL &&
           plane->certificate.commit != NULL &&
           plane->certificate.rollback != NULL;
}

static bool appliance_production_mutation_plane_routing_ready(
    const struct appliance_production_mutation_plane *plane)
{
    return plane->routing_bound &&
           plane->routing.stage != NULL &&
           plane->routing.commit != NULL &&
           plane->routing.rollback != NULL;
}

static bool appliance_production_mutation_plane_network_executor_ready(
    const struct appliance_production_mutation_plane *plane)
{
    return plane->network_executor_bound &&
           plane->network_executor.stage != NULL &&
           plane->network_executor.commit != NULL &&
           plane->network_executor.rollback != NULL;
}

static bool appliance_production_mutation_plane_system_ready(
    const struct appliance_production_mutation_plane *plane)
{
    return plane->system_bound &&
           plane->system.stage != NULL &&
           plane->system.commit != NULL &&
           plane->system.rollback != NULL;
}

static bool appliance_production_mutation_plane_account_ready(
    const struct appliance_production_mutation_plane *plane)
{
    return plane->account_bound &&
           plane->account.stage != NULL &&
           plane->account.commit != NULL &&
           plane->account.rollback != NULL;
}

static bool appliance_production_mutation_plane_certificate_name_ready(
    const struct appliance_production_mutation_plane *plane)
{
    return plane->certificate_name_bound &&
           plane->certificate_name.stage != NULL &&
           plane->certificate_name.commit != NULL &&
           plane->certificate_name.rollback != NULL;
}

static bool appliance_production_mutation_plane_acme_ready(
    const struct appliance_production_mutation_plane *plane)
{
    return plane->acme_bound &&
           plane->acme.stage != NULL &&
           plane->acme.commit != NULL &&
           plane->acme.rollback != NULL;
}

static bool appliance_production_mutation_plane_panel_gate_ready(
    const struct appliance_production_mutation_plane *plane)
{
    return plane->panel_gate_bound &&
           plane->panel_gate.stage != NULL &&
           plane->panel_gate.commit != NULL &&
           plane->panel_gate.rollback != NULL;
}

static uint64_t appliance_production_mutation_plane_issue_token(
    struct appliance_production_mutation_plane *plane,
    uint64_t now_tick)
{
    plane->confirmation_sequence++;
    return ((now_tick + 1U) << 16) ^ (plane->confirmation_sequence + 1U);
}

static bool appliance_production_mutation_plane_append(
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

static enum appliance_production_mutation_plane_status
appliance_production_mutation_plane_finish(
    struct appliance_production_mutation_plane *plane,
    enum appliance_production_mutation_plane_status status,
    enum appliance_management_authorization_mutation mutation,
    char *output,
    size_t capacity,
    size_t *output_length)
{
    size_t offset = 0U;

    plane->last_status = status;
    plane->state = appliance_production_mutation_plane_derive_state(plane);
    *output_length = 0U;
    if (output == NULL || capacity == 0U) {
        return APPLIANCE_PRODUCTION_MUTATION_PLANE_BUFFER_TOO_SMALL;
    }
    if (!appliance_production_mutation_plane_append(
            output, &offset, "mutation=", capacity) ||
        !appliance_production_mutation_plane_append(
            output,
            &offset,
            appliance_management_authorization_mutation_string(mutation),
            capacity) ||
        !appliance_production_mutation_plane_append(
            output, &offset, " state=", capacity) ||
        !appliance_production_mutation_plane_append(
            output,
            &offset,
            appliance_production_mutation_plane_state_string(plane->state),
            capacity) ||
        !appliance_production_mutation_plane_append(
            output, &offset, " result=", capacity) ||
        !appliance_production_mutation_plane_append(
            output,
            &offset,
            appliance_production_mutation_plane_status_string(status),
            capacity)) {
        *output_length = 0U;
        return APPLIANCE_PRODUCTION_MUTATION_PLANE_BUFFER_TOO_SMALL;
    }
    /*
     * ITEM-649: a stage that refused for a named cause publishes it next to
     * the generic result, and only on a refusal; the applied record and every
     * refusal without a more specific cause keep their existing bytes.
     */
    if (status == APPLIANCE_PRODUCTION_MUTATION_PLANE_APPLY_FAILED &&
        plane->certificate_name_refusal != NULL &&
        (!appliance_production_mutation_plane_append(
             output, &offset, " reason=", capacity) ||
         !appliance_production_mutation_plane_append(
             output, &offset, plane->certificate_name_refusal, capacity))) {
        *output_length = 0U;
        return APPLIANCE_PRODUCTION_MUTATION_PLANE_BUFFER_TOO_SMALL;
    }
    *output_length = offset;
    return status;
}

enum appliance_production_mutation_plane_status
appliance_production_mutation_plane_init(
    struct appliance_production_mutation_plane *plane,
    struct appliance_production_auth *auth,
    struct appliance_production_management_plane *management,
    struct network_config_transaction *network)
{
    /*
     * ITEM-503: the read-only management plane and the network transaction
     * are optional bindings.  The administrator gate is not: without it no
     * mutation can ever be authorized, so a composition that binds none is
     * refused at init and one that binds only some refuses exactly the
     * mutations whose executor is missing.
     */
    if (plane == NULL || auth == NULL) {
        return APPLIANCE_PRODUCTION_MUTATION_PLANE_NULL;
    }
    memset(plane, 0, sizeof(*plane));
    plane->initialized = true;
    plane->version = APPLIANCE_PRODUCTION_MUTATION_PLANE_VERSION;
    plane->state = APPLIANCE_PRODUCTION_MUTATION_PLANE_STATE_PREPARED;
    plane->last_status = APPLIANCE_PRODUCTION_MUTATION_PLANE_OK;
    plane->auth = auth;
    plane->management = management;
    plane->network = network;
    plane->confirmation_window =
        APPLIANCE_PRODUCTION_MUTATION_PLANE_CONFIRMATION_WINDOW_MS;
    return APPLIANCE_PRODUCTION_MUTATION_PLANE_OK;
}

enum appliance_production_mutation_plane_status
appliance_production_mutation_plane_bind_certificate(
    struct appliance_production_mutation_plane *plane,
    const struct appliance_production_mutation_plane_certificate_executor
        *executor)
{
    if (!appliance_production_mutation_plane_initialized(plane)) {
        return APPLIANCE_PRODUCTION_MUTATION_PLANE_NULL;
    }
    if (executor == NULL || executor->stage == NULL ||
        executor->commit == NULL || executor->rollback == NULL) {
        memset(&plane->certificate, 0, sizeof(plane->certificate));
        plane->certificate_bound = false;
        return APPLIANCE_PRODUCTION_MUTATION_PLANE_OK;
    }
    plane->certificate = *executor;
    plane->certificate_bound = true;
    return APPLIANCE_PRODUCTION_MUTATION_PLANE_OK;
}

enum appliance_production_mutation_plane_status
appliance_production_mutation_plane_bind_routing(
    struct appliance_production_mutation_plane *plane,
    const struct appliance_production_mutation_plane_routing_executor
        *executor)
{
    if (!appliance_production_mutation_plane_initialized(plane)) {
        return APPLIANCE_PRODUCTION_MUTATION_PLANE_NULL;
    }
    if (executor == NULL || executor->stage == NULL ||
        executor->commit == NULL || executor->rollback == NULL) {
        memset(&plane->routing, 0, sizeof(plane->routing));
        plane->routing_bound = false;
        return APPLIANCE_PRODUCTION_MUTATION_PLANE_OK;
    }
    plane->routing = *executor;
    plane->routing_bound = true;
    return APPLIANCE_PRODUCTION_MUTATION_PLANE_OK;
}

enum appliance_production_mutation_plane_status
appliance_production_mutation_plane_bind_network_executor(
    struct appliance_production_mutation_plane *plane,
    const struct appliance_production_mutation_plane_network_executor
        *executor)
{
    if (!appliance_production_mutation_plane_initialized(plane)) {
        return APPLIANCE_PRODUCTION_MUTATION_PLANE_NULL;
    }
    if (executor == NULL || executor->stage == NULL ||
        executor->commit == NULL || executor->rollback == NULL) {
        memset(&plane->network_executor, 0,
               sizeof(plane->network_executor));
        plane->network_executor_bound = false;
        return APPLIANCE_PRODUCTION_MUTATION_PLANE_OK;
    }
    plane->network_executor = *executor;
    plane->network_executor_bound = true;
    return APPLIANCE_PRODUCTION_MUTATION_PLANE_OK;
}

enum appliance_production_mutation_plane_status
appliance_production_mutation_plane_bind_system(
    struct appliance_production_mutation_plane *plane,
    const struct appliance_production_mutation_plane_system_executor
        *executor)
{
    if (!appliance_production_mutation_plane_initialized(plane)) {
        return APPLIANCE_PRODUCTION_MUTATION_PLANE_NULL;
    }
    if (executor == NULL || executor->stage == NULL ||
        executor->commit == NULL || executor->rollback == NULL) {
        memset(&plane->system, 0, sizeof(plane->system));
        plane->system_bound = false;
        return APPLIANCE_PRODUCTION_MUTATION_PLANE_OK;
    }
    plane->system = *executor;
    plane->system_bound = true;
    return APPLIANCE_PRODUCTION_MUTATION_PLANE_OK;
}

enum appliance_production_mutation_plane_status
appliance_production_mutation_plane_bind_account(
    struct appliance_production_mutation_plane *plane,
    const struct appliance_production_mutation_plane_account_executor
        *executor)
{
    if (!appliance_production_mutation_plane_initialized(plane)) {
        return APPLIANCE_PRODUCTION_MUTATION_PLANE_NULL;
    }
    if (executor == NULL || executor->stage == NULL ||
        executor->commit == NULL || executor->rollback == NULL) {
        memset(&plane->account, 0, sizeof(plane->account));
        plane->account_bound = false;
        return APPLIANCE_PRODUCTION_MUTATION_PLANE_OK;
    }
    plane->account = *executor;
    plane->account_bound = true;
    return APPLIANCE_PRODUCTION_MUTATION_PLANE_OK;
}

enum appliance_production_mutation_plane_status
appliance_production_mutation_plane_bind_certificate_name(
    struct appliance_production_mutation_plane *plane,
    const struct
        appliance_production_mutation_plane_certificate_name_executor
            *executor)
{
    if (!appliance_production_mutation_plane_initialized(plane)) {
        return APPLIANCE_PRODUCTION_MUTATION_PLANE_NULL;
    }
    if (executor == NULL || executor->stage == NULL ||
        executor->commit == NULL || executor->rollback == NULL) {
        memset(&plane->certificate_name, 0,
               sizeof(plane->certificate_name));
        plane->certificate_name_bound = false;
        return APPLIANCE_PRODUCTION_MUTATION_PLANE_OK;
    }
    plane->certificate_name = *executor;
    plane->certificate_name_bound = true;
    return APPLIANCE_PRODUCTION_MUTATION_PLANE_OK;
}

enum appliance_production_mutation_plane_status
appliance_production_mutation_plane_bind_acme(
    struct appliance_production_mutation_plane *plane,
    const struct appliance_production_mutation_plane_acme_executor *executor)
{
    if (!appliance_production_mutation_plane_initialized(plane)) {
        return APPLIANCE_PRODUCTION_MUTATION_PLANE_NULL;
    }
    if (executor == NULL || executor->stage == NULL ||
        executor->commit == NULL || executor->rollback == NULL) {
        memset(&plane->acme, 0, sizeof(plane->acme));
        plane->acme_bound = false;
        return APPLIANCE_PRODUCTION_MUTATION_PLANE_OK;
    }
    plane->acme = *executor;
    plane->acme_bound = true;
    return APPLIANCE_PRODUCTION_MUTATION_PLANE_OK;
}

enum appliance_production_mutation_plane_status
appliance_production_mutation_plane_bind_panel_gate(
    struct appliance_production_mutation_plane *plane,
    const struct appliance_production_mutation_plane_panel_gate_executor
        *executor)
{
    if (!appliance_production_mutation_plane_initialized(plane)) {
        return APPLIANCE_PRODUCTION_MUTATION_PLANE_NULL;
    }
    if (executor == NULL || executor->stage == NULL ||
        executor->commit == NULL || executor->rollback == NULL) {
        memset(&plane->panel_gate, 0, sizeof(plane->panel_gate));
        plane->panel_gate_bound = false;
        return APPLIANCE_PRODUCTION_MUTATION_PLANE_OK;
    }
    plane->panel_gate = *executor;
    plane->panel_gate_bound = true;
    return APPLIANCE_PRODUCTION_MUTATION_PLANE_OK;
}

enum appliance_production_mutation_plane_status
appliance_production_mutation_plane_refresh(
    struct appliance_production_mutation_plane *plane)
{
    if (!appliance_production_mutation_plane_initialized(plane)) {
        return APPLIANCE_PRODUCTION_MUTATION_PLANE_NULL;
    }
    if (plane->state == APPLIANCE_PRODUCTION_MUTATION_PLANE_STATE_STOPPED ||
        plane->state == APPLIANCE_PRODUCTION_MUTATION_PLANE_STATE_FAILED) {
        return APPLIANCE_PRODUCTION_MUTATION_PLANE_BAD_STATE;
    }
    plane->state = appliance_production_mutation_plane_derive_state(plane);
    if (plane->state == APPLIANCE_PRODUCTION_MUTATION_PLANE_STATE_READY ||
        plane->state ==
            APPLIANCE_PRODUCTION_MUTATION_PLANE_STATE_DEGRADED ||
        plane->state ==
            APPLIANCE_PRODUCTION_MUTATION_PLANE_STATE_PENDING_CONFIRMATION) {
        plane->last_status = APPLIANCE_PRODUCTION_MUTATION_PLANE_OK;
        return APPLIANCE_PRODUCTION_MUTATION_PLANE_OK;
    }
    plane->last_status = APPLIANCE_PRODUCTION_MUTATION_PLANE_UNAVAILABLE;
    return APPLIANCE_PRODUCTION_MUTATION_PLANE_UNAVAILABLE;
}

bool appliance_production_mutation_plane_ready(
    const struct appliance_production_mutation_plane *plane)
{
    if (!appliance_production_mutation_plane_initialized(plane)) {
        return false;
    }
    if (plane->state == APPLIANCE_PRODUCTION_MUTATION_PLANE_STATE_STOPPED ||
        plane->state == APPLIANCE_PRODUCTION_MUTATION_PLANE_STATE_FAILED) {
        return false;
    }
    return appliance_production_mutation_plane_available(plane);
}

enum appliance_production_mutation_plane_status
appliance_production_mutation_plane_request(
    struct appliance_production_mutation_plane *plane,
    const uint8_t *authorization_header,
    uint32_t authorization_header_length,
    enum appliance_management_authorization_method method,
    const uint8_t *target,
    uint32_t target_length,
    const char *body,
    size_t body_length,
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
    enum appliance_production_mutation_plane_certificate_name_status
        certificate_name_status;
    enum appliance_production_auth_status auth_status;

    if (!appliance_production_mutation_plane_initialized(plane) ||
        output == NULL || output_length == NULL || target == NULL) {
        return APPLIANCE_PRODUCTION_MUTATION_PLANE_NULL;
    }
    *output_length = 0U;
    plane->certificate_name_refusal = NULL;
    if (plane->state == APPLIANCE_PRODUCTION_MUTATION_PLANE_STATE_STOPPED ||
        plane->state == APPLIANCE_PRODUCTION_MUTATION_PLANE_STATE_FAILED) {
        plane->last_status = APPLIANCE_PRODUCTION_MUTATION_PLANE_BAD_STATE;
        return plane->last_status;
    }

    if (plane->pending_mutation !=
            APPLIANCE_MANAGEMENT_AUTHORIZATION_MUTATION_NONE &&
        now_tick > plane->pending_tick + plane->confirmation_window) {
        appliance_production_mutation_plane_rollback_pending(plane);
        appliance_production_mutation_plane_audit(
            plane,
            APPLIANCE_PRODUCTION_MUTATION_PLANE_AUDIT_EXPIRED,
            plane->pending_mutation,
            plane->pending_token,
            now_tick,
            NULL);
        appliance_production_mutation_plane_clear_pending(plane);
        ++plane->expired_count;
        return appliance_production_mutation_plane_finish(
            plane,
            APPLIANCE_PRODUCTION_MUTATION_PLANE_EXPIRED,
            APPLIANCE_MANAGEMENT_AUTHORIZATION_MUTATION_NONE,
            output,
            capacity,
            output_length);
    }

    if (!appliance_production_mutation_plane_available(plane)) {
        ++plane->unavailable_count;
        appliance_production_mutation_plane_audit(
            plane,
            APPLIANCE_PRODUCTION_MUTATION_PLANE_AUDIT_UNAVAILABLE,
            APPLIANCE_MANAGEMENT_AUTHORIZATION_MUTATION_NONE,
            0U,
            now_tick,
            NULL);
        return appliance_production_mutation_plane_finish(
            plane,
            APPLIANCE_PRODUCTION_MUTATION_PLANE_UNAVAILABLE,
            APPLIANCE_MANAGEMENT_AUTHORIZATION_MUTATION_NONE,
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
            appliance_production_mutation_plane_audit(
                plane,
                APPLIANCE_PRODUCTION_MUTATION_PLANE_AUDIT_UNAVAILABLE,
                APPLIANCE_MANAGEMENT_AUTHORIZATION_MUTATION_NONE,
                0U,
                now_tick,
                &decision);
            return appliance_production_mutation_plane_finish(
                plane,
                APPLIANCE_PRODUCTION_MUTATION_PLANE_UNAVAILABLE,
                APPLIANCE_MANAGEMENT_AUTHORIZATION_MUTATION_NONE,
                output,
                capacity,
                output_length);
        }
        ++plane->denied_count;
        appliance_production_mutation_plane_audit(
            plane,
            APPLIANCE_PRODUCTION_MUTATION_PLANE_AUDIT_DENIED,
            mutation,
            confirmation_token,
            now_tick,
            &decision);
        return appliance_production_mutation_plane_finish(
            plane,
            APPLIANCE_PRODUCTION_MUTATION_PLANE_DENIED,
            mutation,
            output,
            capacity,
            output_length);
    }

    if (cancel) {
        if (plane->pending_mutation ==
            APPLIANCE_MANAGEMENT_AUTHORIZATION_MUTATION_NONE) {
            return appliance_production_mutation_plane_finish(
                plane,
                APPLIANCE_PRODUCTION_MUTATION_PLANE_NO_PENDING,
                mutation,
                output,
                capacity,
                output_length);
        }
        if (confirmation_token != plane->pending_token) {
            /*
             * A mis-typed confirmation is an audited refusal, not an
             * authorization denial: `denied_count` counts requests the
             * authorization gate rejected.
             */
            appliance_production_mutation_plane_audit(
                plane,
                APPLIANCE_PRODUCTION_MUTATION_PLANE_AUDIT_DENIED,
                plane->pending_mutation,
                confirmation_token,
                now_tick,
                &decision);
            return appliance_production_mutation_plane_finish(
                plane,
                APPLIANCE_PRODUCTION_MUTATION_PLANE_BAD_CONFIRMATION,
                plane->pending_mutation,
                output,
                capacity,
                output_length);
        }
        appliance_production_mutation_plane_rollback_pending(plane);
        appliance_production_mutation_plane_audit(
            plane,
            APPLIANCE_PRODUCTION_MUTATION_PLANE_AUDIT_CANCELLED,
            plane->pending_mutation,
            plane->pending_token,
            now_tick,
            &decision);
        appliance_production_mutation_plane_clear_pending(plane);
        ++plane->cancelled_count;
        return appliance_production_mutation_plane_finish(
            plane,
            APPLIANCE_PRODUCTION_MUTATION_PLANE_CANCELLED,
            mutation,
            output,
            capacity,
            output_length);
    }

    if (confirmation_token != 0U) {
        if (plane->pending_mutation ==
                APPLIANCE_MANAGEMENT_AUTHORIZATION_MUTATION_NONE ||
            confirmation_token != plane->pending_token) {
            /* See the cancel path: only the authorization gate denies. */
            appliance_production_mutation_plane_audit(
                plane,
                APPLIANCE_PRODUCTION_MUTATION_PLANE_AUDIT_DENIED,
                plane->pending_mutation,
                confirmation_token,
                now_tick,
                &decision);
            return appliance_production_mutation_plane_finish(
                plane,
                APPLIANCE_PRODUCTION_MUTATION_PLANE_BAD_CONFIRMATION,
                plane->pending_mutation,
                output,
                capacity,
                output_length);
        }
        if (mutation != plane->pending_mutation) {
            /*
             * ITEM-506 increment 6: the token authorizes the candidate the
             * plane actually holds and nothing else.  The target of the
             * confirmation request must resolve to the same mutation as the
             * pending candidate; otherwise a token shown for one page could
             * commit another page's candidate (the credential change is the
             * one where that would be a silent account takeover).  The
             * candidate is kept - it still has its own window and its own
             * valid target - and the misdirected confirmation is refused.
             */
            appliance_production_mutation_plane_audit(
                plane,
                APPLIANCE_PRODUCTION_MUTATION_PLANE_AUDIT_DENIED,
                plane->pending_mutation,
                confirmation_token,
                now_tick,
                &decision);
            return appliance_production_mutation_plane_finish(
                plane,
                APPLIANCE_PRODUCTION_MUTATION_PLANE_BAD_CONFIRMATION,
                mutation,
                output,
                capacity,
                output_length);
        }
        if (plane->pending_mutation ==
                APPLIANCE_MANAGEMENT_AUTHORIZATION_MUTATION_NETWORK_CONFIG &&
            (appliance_production_mutation_plane_network_executor_ready(
                 plane) ||
             appliance_production_mutation_plane_network_ready(plane))) {
            /*
             * ITEM-505: the bound executor owns the durable record.  Its
             * commit writes the candidate to non-volatile state and proves
             * the write before it answers OK; the RAM-only transaction is the
             * fallback for the compositions that still bind one and no
             * executor.
             */
            const bool committed =
                appliance_production_mutation_plane_network_executor_ready(
                    plane)
                    ? plane->network_executor.commit(
                          plane->network_executor.context) ==
                          APPLIANCE_PRODUCTION_MUTATION_PLANE_NETWORK_OK
                    : network_config_transaction_commit(plane->network) ==
                          NETWORK_CONFIG_TRANSACTION_OK;

            if (!committed) {
                appliance_production_mutation_plane_rollback_pending(plane);
                appliance_production_mutation_plane_audit(
                    plane,
                    APPLIANCE_PRODUCTION_MUTATION_PLANE_AUDIT_APPLY_FAILED,
                    plane->pending_mutation,
                    confirmation_token,
                    now_tick,
                    &decision);
                appliance_production_mutation_plane_clear_pending(plane);
                return appliance_production_mutation_plane_finish(
                    plane,
                    APPLIANCE_PRODUCTION_MUTATION_PLANE_APPLY_FAILED,
                    mutation,
                    output,
                    capacity,
                    output_length);
            }
        } else if (plane->pending_mutation ==
                       APPLIANCE_MANAGEMENT_AUTHORIZATION_MUTATION_CERTIFICATE_IMPORT &&
                   appliance_production_mutation_plane_certificate_ready(
                       plane)) {
            /*
             * ITEM-503: the executor re-validates the candidate against the
             * current clock and writes record v2; a refusal applies nothing
             * and keeps the previously stored certificate.
             */
            if (plane->certificate.commit(
                    plane->certificate.context) !=
                APPLIANCE_PRODUCTION_MUTATION_PLANE_CERTIFICATE_OK) {
                appliance_production_mutation_plane_rollback_pending(plane);
                appliance_production_mutation_plane_audit(
                    plane,
                    APPLIANCE_PRODUCTION_MUTATION_PLANE_AUDIT_APPLY_FAILED,
                    plane->pending_mutation,
                    confirmation_token,
                    now_tick,
                    &decision);
                appliance_production_mutation_plane_clear_pending(plane);
                return appliance_production_mutation_plane_finish(
                    plane,
                    APPLIANCE_PRODUCTION_MUTATION_PLANE_APPLY_FAILED,
                    mutation,
                    output,
                    capacity,
                    output_length);
            }
        } else if (plane->pending_mutation ==
                       APPLIANCE_MANAGEMENT_AUTHORIZATION_MUTATION_ROUTING_CONFIG &&
                   appliance_production_mutation_plane_routing_ready(plane)) {
            /*
             * ITEM-504: the route configuration replaces the applied table
             * with the staged candidate whole; a refusal applies nothing and
             * keeps the table the machine already routes.
             */
            if (plane->routing.commit(plane->routing.context) !=
                APPLIANCE_PRODUCTION_MUTATION_PLANE_ROUTING_OK) {
                appliance_production_mutation_plane_rollback_pending(plane);
                appliance_production_mutation_plane_audit(
                    plane,
                    APPLIANCE_PRODUCTION_MUTATION_PLANE_AUDIT_APPLY_FAILED,
                    plane->pending_mutation,
                    confirmation_token,
                    now_tick,
                    &decision);
                appliance_production_mutation_plane_clear_pending(plane);
                return appliance_production_mutation_plane_finish(
                    plane,
                    APPLIANCE_PRODUCTION_MUTATION_PLANE_APPLY_FAILED,
                    mutation,
                    output,
                    capacity,
                    output_length);
            }
        } else if (plane->pending_mutation ==
                       APPLIANCE_MANAGEMENT_AUTHORIZATION_MUTATION_SYSTEM_HOSTNAME &&
                   appliance_production_mutation_plane_system_ready(plane)) {
            /*
             * ITEM-505 increment 3: the executor writes the hostname into the
             * durable system record and proves the write; a refusal applies
             * nothing and the running identity keeps the name it booted with.
             */
            if (plane->system.commit(plane->system.context) !=
                APPLIANCE_PRODUCTION_MUTATION_PLANE_SYSTEM_OK) {
                appliance_production_mutation_plane_rollback_pending(plane);
                appliance_production_mutation_plane_audit(
                    plane,
                    APPLIANCE_PRODUCTION_MUTATION_PLANE_AUDIT_APPLY_FAILED,
                    plane->pending_mutation,
                    confirmation_token,
                    now_tick,
                    &decision);
                appliance_production_mutation_plane_clear_pending(plane);
                return appliance_production_mutation_plane_finish(
                    plane,
                    APPLIANCE_PRODUCTION_MUTATION_PLANE_APPLY_FAILED,
                    mutation,
                    output,
                    capacity,
                    output_length);
            }
        } else if (plane->pending_mutation ==
                       APPLIANCE_MANAGEMENT_AUTHORIZATION_MUTATION_ACCOUNT_CREDENTIAL &&
                   appliance_production_mutation_plane_account_ready(plane)) {
            /*
             * ITEM-506 increment 6: the executor writes the new credential
             * through the enrollment owner - the single setter that also
             * republishes the verifier and closes every issued session - and
             * proves the write; a refusal applies nothing and the account
             * keeps the login and password it had.
             */
            if (plane->account.commit(plane->account.context) !=
                APPLIANCE_PRODUCTION_MUTATION_PLANE_ACCOUNT_OK) {
                appliance_production_mutation_plane_rollback_pending(plane);
                appliance_production_mutation_plane_audit(
                    plane,
                    APPLIANCE_PRODUCTION_MUTATION_PLANE_AUDIT_APPLY_FAILED,
                    plane->pending_mutation,
                    confirmation_token,
                    now_tick,
                    &decision);
                appliance_production_mutation_plane_clear_pending(plane);
                return appliance_production_mutation_plane_finish(
                    plane,
                    APPLIANCE_PRODUCTION_MUTATION_PLANE_APPLY_FAILED,
                    mutation,
                    output,
                    capacity,
                    output_length);
            }
        } else if (plane->pending_mutation ==
                       APPLIANCE_MANAGEMENT_AUTHORIZATION_MUTATION_CERTIFICATE_NAME &&
                   appliance_production_mutation_plane_certificate_name_ready(
                       plane)) {
            /*
             * ITEM-507 increment 4: the executor rewrites the whole durable
             * name record and proves the write; a failure applies nothing, and
             * the table the listener was configured with stays in force.
             */
            if (plane->certificate_name.commit(
                    plane->certificate_name.context) !=
                APPLIANCE_PRODUCTION_MUTATION_PLANE_CERTIFICATE_NAME_OK) {
                appliance_production_mutation_plane_rollback_pending(plane);
                appliance_production_mutation_plane_audit(
                    plane,
                    APPLIANCE_PRODUCTION_MUTATION_PLANE_AUDIT_APPLY_FAILED,
                    plane->pending_mutation,
                    confirmation_token,
                    now_tick,
                    &decision);
                appliance_production_mutation_plane_clear_pending(plane);
                return appliance_production_mutation_plane_finish(
                    plane,
                    APPLIANCE_PRODUCTION_MUTATION_PLANE_APPLY_FAILED,
                    mutation,
                    output,
                    capacity,
                    output_length);
            }
        } else if (plane->pending_mutation ==
                       APPLIANCE_MANAGEMENT_AUTHORIZATION_MUTATION_ACME_CONTROL &&
                   appliance_production_mutation_plane_acme_ready(plane)) {
            /*
             * ITEM-508 increment 34: the executor writes the durable control
             * record and proves the write by reading it back; a failure applies
             * nothing, and the setting the running service reads stays as it
             * was.
             */
            if (plane->acme.commit(plane->acme.context) !=
                APPLIANCE_PRODUCTION_MUTATION_PLANE_ACME_OK) {
                appliance_production_mutation_plane_rollback_pending(plane);
                appliance_production_mutation_plane_audit(
                    plane,
                    APPLIANCE_PRODUCTION_MUTATION_PLANE_AUDIT_APPLY_FAILED,
                    plane->pending_mutation,
                    confirmation_token,
                    now_tick,
                    &decision);
                appliance_production_mutation_plane_clear_pending(plane);
                return appliance_production_mutation_plane_finish(
                    plane,
                    APPLIANCE_PRODUCTION_MUTATION_PLANE_APPLY_FAILED,
                    mutation,
                    output,
                    capacity,
                    output_length);
            }
        } else if (plane->pending_mutation ==
                       APPLIANCE_MANAGEMENT_AUTHORIZATION_MUTATION_PANEL_GATE &&
                   appliance_production_mutation_plane_panel_gate_ready(
                       plane)) {
            /*
             * ITEM-516: the executor writes the default closed policy into the
             * system record and proves the write; a refusal applies nothing and
             * the record keeps the setting it had.
             */
            if (plane->panel_gate.commit(plane->panel_gate.context) !=
                APPLIANCE_PRODUCTION_MUTATION_PLANE_PANEL_GATE_OK) {
                appliance_production_mutation_plane_rollback_pending(plane);
                appliance_production_mutation_plane_audit(
                    plane,
                    APPLIANCE_PRODUCTION_MUTATION_PLANE_AUDIT_APPLY_FAILED,
                    plane->pending_mutation,
                    confirmation_token,
                    now_tick,
                    &decision);
                appliance_production_mutation_plane_clear_pending(plane);
                return appliance_production_mutation_plane_finish(
                    plane,
                    APPLIANCE_PRODUCTION_MUTATION_PLANE_APPLY_FAILED,
                    mutation,
                    output,
                    capacity,
                    output_length);
            }
        } else {
            appliance_production_mutation_plane_rollback_pending(plane);
            appliance_production_mutation_plane_audit(
                plane,
                APPLIANCE_PRODUCTION_MUTATION_PLANE_AUDIT_DENIED,
                plane->pending_mutation,
                confirmation_token,
                now_tick,
                &decision);
            appliance_production_mutation_plane_clear_pending(plane);
            return appliance_production_mutation_plane_finish(
                plane,
                APPLIANCE_PRODUCTION_MUTATION_PLANE_UNSUPPORTED_TARGET,
                mutation,
                output,
                capacity,
                output_length);
        }
        appliance_production_mutation_plane_audit(
            plane,
            APPLIANCE_PRODUCTION_MUTATION_PLANE_AUDIT_ALLOWED,
            plane->pending_mutation,
            confirmation_token,
            now_tick,
            &decision);
        appliance_production_mutation_plane_clear_pending(plane);
        ++plane->applied_count;
        return appliance_production_mutation_plane_finish(
            plane,
            APPLIANCE_PRODUCTION_MUTATION_PLANE_APPLIED,
            mutation,
            output,
            capacity,
            output_length);
    }

    /*
     * ITEM-506 increment 6: the plane holds exactly one candidate.  A new
     * stage replaces it whole, so the previous executor's rollback runs
     * first.  Without this a staged credential candidate would stay in
     * memory with no token and no later path that ever zeroes it, which
     * contradicts the documented lifetime of the new password.
     */
    if (plane->pending_mutation !=
        APPLIANCE_MANAGEMENT_AUTHORIZATION_MUTATION_NONE) {
        appliance_production_mutation_plane_rollback_pending(plane);
        appliance_production_mutation_plane_clear_pending(plane);
    }

    if (mutation ==
            APPLIANCE_MANAGEMENT_AUTHORIZATION_MUTATION_NETWORK_CONFIG &&
        (appliance_production_mutation_plane_network_executor_ready(plane) ||
         appliance_production_mutation_plane_network_ready(plane))) {
        /*
         * ITEM-505: the executor decodes the one form body and holds the
         * candidate until the exact token comes back.  The borrowed
         * transaction is the RAM-only fallback, exactly as it is on commit.
         */
        const bool staged =
            body != NULL &&
            (appliance_production_mutation_plane_network_executor_ready(
                 plane)
                 ? plane->network_executor.stage(
                       plane->network_executor.context,
                       (const uint8_t *)body,
                       body_length) ==
                       APPLIANCE_PRODUCTION_MUTATION_PLANE_NETWORK_OK
                 : network_config_transaction_stage(
                       plane->network, body, body_length) ==
                       NETWORK_CONFIG_TRANSACTION_OK);

        if (!staged) {
            appliance_production_mutation_plane_clear_pending(plane);
            appliance_production_mutation_plane_audit(
                plane,
                APPLIANCE_PRODUCTION_MUTATION_PLANE_AUDIT_APPLY_FAILED,
                mutation,
                0U,
                now_tick,
                &decision);
            return appliance_production_mutation_plane_finish(
                plane,
                APPLIANCE_PRODUCTION_MUTATION_PLANE_APPLY_FAILED,
                mutation,
                output,
                capacity,
                output_length);
        }
    } else if (mutation ==
                   APPLIANCE_MANAGEMENT_AUTHORIZATION_MUTATION_CERTIFICATE_IMPORT &&
               appliance_production_mutation_plane_certificate_ready(plane)) {
        /*
         * ITEM-503: the upload executor decodes the form, validates the pair
         * and the chain, and holds the candidate.  Nothing durable is written
         * until the exact token comes back; a refusal applies nothing and the
         * previously stored certificate stays active.
         */
        if (body == NULL ||
            plane->certificate.stage(
                plane->certificate.context,
                (const uint8_t *)body,
                body_length) !=
                APPLIANCE_PRODUCTION_MUTATION_PLANE_CERTIFICATE_OK) {
            appliance_production_mutation_plane_rollback_pending(plane);
            appliance_production_mutation_plane_clear_pending(plane);
            appliance_production_mutation_plane_audit(
                plane,
                APPLIANCE_PRODUCTION_MUTATION_PLANE_AUDIT_APPLY_FAILED,
                mutation,
                0U,
                now_tick,
                &decision);
            return appliance_production_mutation_plane_finish(
                plane,
                APPLIANCE_PRODUCTION_MUTATION_PLANE_APPLY_FAILED,
                mutation,
                output,
                capacity,
                output_length);
        }
    } else if (mutation ==
                   APPLIANCE_MANAGEMENT_AUTHORIZATION_MUTATION_ROUTING_CONFIG &&
               appliance_production_mutation_plane_routing_ready(plane)) {
        /*
         * ITEM-504: the route configuration decodes one form body and holds
         * the candidate table.  Nothing changes until the exact token comes
         * back; a refusal applies nothing and the applied table stays as it
         * was.
         */
        if (body == NULL ||
            plane->routing.stage(
                plane->routing.context,
                (const uint8_t *)body,
                body_length) !=
                APPLIANCE_PRODUCTION_MUTATION_PLANE_ROUTING_OK) {
            appliance_production_mutation_plane_rollback_pending(plane);
            appliance_production_mutation_plane_clear_pending(plane);
            appliance_production_mutation_plane_audit(
                plane,
                APPLIANCE_PRODUCTION_MUTATION_PLANE_AUDIT_APPLY_FAILED,
                mutation,
                0U,
                now_tick,
                &decision);
            return appliance_production_mutation_plane_finish(
                plane,
                APPLIANCE_PRODUCTION_MUTATION_PLANE_APPLY_FAILED,
                mutation,
                output,
                capacity,
                output_length);
        }
    } else if (mutation ==
                   APPLIANCE_MANAGEMENT_AUTHORIZATION_MUTATION_SYSTEM_HOSTNAME &&
               appliance_production_mutation_plane_system_ready(plane)) {
        /*
         * ITEM-505 increment 3: the system executor decodes the one hostname
         * field, validates it with the configuration model's own rule and
         * holds the candidate.  Nothing durable is written until the exact
         * token comes back; a refused name leaves the record untouched.
         */
        if (body == NULL ||
            plane->system.stage(
                plane->system.context,
                (const uint8_t *)body,
                body_length) !=
                APPLIANCE_PRODUCTION_MUTATION_PLANE_SYSTEM_OK) {
            appliance_production_mutation_plane_rollback_pending(plane);
            appliance_production_mutation_plane_clear_pending(plane);
            appliance_production_mutation_plane_audit(
                plane,
                APPLIANCE_PRODUCTION_MUTATION_PLANE_AUDIT_APPLY_FAILED,
                mutation,
                0U,
                now_tick,
                &decision);
            return appliance_production_mutation_plane_finish(
                plane,
                APPLIANCE_PRODUCTION_MUTATION_PLANE_APPLY_FAILED,
                mutation,
                output,
                capacity,
                output_length);
        }
    } else if (mutation ==
                   APPLIANCE_MANAGEMENT_AUTHORIZATION_MUTATION_ACCOUNT_CREDENTIAL &&
               appliance_production_mutation_plane_account_ready(plane)) {
        /*
         * ITEM-506 increment 6: the account executor decodes the four-field
         * form, proves the current password through the enrollment owner's
         * verify-only entry, validates the new login and password with the
         * credential model's own policy and holds the candidate.  Nothing
         * durable is written until the exact token comes back; a refused
         * change leaves the account exactly as it was.
         */
        if (body == NULL ||
            plane->account.stage(
                plane->account.context,
                (const uint8_t *)body,
                body_length) !=
                APPLIANCE_PRODUCTION_MUTATION_PLANE_ACCOUNT_OK) {
            appliance_production_mutation_plane_rollback_pending(plane);
            appliance_production_mutation_plane_clear_pending(plane);
            appliance_production_mutation_plane_audit(
                plane,
                APPLIANCE_PRODUCTION_MUTATION_PLANE_AUDIT_APPLY_FAILED,
                mutation,
                0U,
                now_tick,
                &decision);
            return appliance_production_mutation_plane_finish(
                plane,
                APPLIANCE_PRODUCTION_MUTATION_PLANE_APPLY_FAILED,
                mutation,
                output,
                capacity,
                output_length);
        }
    } else if (mutation ==
                   APPLIANCE_MANAGEMENT_AUTHORIZATION_MUTATION_CERTIFICATE_NAME &&
               appliance_production_mutation_plane_certificate_name_ready(
                   plane)) {
        /*
         * ITEM-507 increment 4: the executor decodes the action, the exact
         * name and the PEM pair, validates the pair against the declared name
         * through the ITEM-503 import gate, and holds the candidate - private
         * key included - until the exact token comes back.  A refusal writes
         * nothing.
         */
        certificate_name_status =
            APPLIANCE_PRODUCTION_MUTATION_PLANE_CERTIFICATE_NAME_REFUSED;
        if (body != NULL) {
            certificate_name_status = plane->certificate_name.stage(
                plane->certificate_name.context,
                (const uint8_t *)body,
                body_length);
        }
        if (certificate_name_status !=
            APPLIANCE_PRODUCTION_MUTATION_PLANE_CERTIFICATE_NAME_OK) {
            if (certificate_name_status ==
                APPLIANCE_PRODUCTION_MUTATION_PLANE_CERTIFICATE_NAME_BAD_CLOCK) {
                plane->certificate_name_refusal = "BAD_CLOCK";
            }
            appliance_production_mutation_plane_rollback_pending(plane);
            appliance_production_mutation_plane_clear_pending(plane);
            appliance_production_mutation_plane_audit(
                plane,
                APPLIANCE_PRODUCTION_MUTATION_PLANE_AUDIT_APPLY_FAILED,
                mutation,
                0U,
                now_tick,
                &decision);
            return appliance_production_mutation_plane_finish(
                plane,
                APPLIANCE_PRODUCTION_MUTATION_PLANE_APPLY_FAILED,
                mutation,
                output,
                capacity,
                output_length);
        }
    } else if (mutation ==
                   APPLIANCE_MANAGEMENT_AUTHORIZATION_MUTATION_ACME_CONTROL &&
               appliance_production_mutation_plane_acme_ready(plane)) {
        /*
         * ITEM-508 increment 34: the executor decodes the one action field
         * (enable, disable or request) and holds the candidate; the refusal of
         * an action the current record cannot mean is named and writes nothing.
         * Nothing durable is written until the exact token comes back.
         */
        if (body == NULL ||
            plane->acme.stage(
                plane->acme.context,
                (const uint8_t *)body,
                body_length) != APPLIANCE_PRODUCTION_MUTATION_PLANE_ACME_OK) {
            appliance_production_mutation_plane_rollback_pending(plane);
            appliance_production_mutation_plane_clear_pending(plane);
            appliance_production_mutation_plane_audit(
                plane,
                APPLIANCE_PRODUCTION_MUTATION_PLANE_AUDIT_APPLY_FAILED,
                mutation,
                0U,
                now_tick,
                &decision);
            return appliance_production_mutation_plane_finish(
                plane,
                APPLIANCE_PRODUCTION_MUTATION_PLANE_APPLY_FAILED,
                mutation,
                output,
                capacity,
                output_length);
        }
    } else if (mutation ==
                   APPLIANCE_MANAGEMENT_AUTHORIZATION_MUTATION_PANEL_GATE &&
               appliance_production_mutation_plane_panel_gate_ready(plane)) {
        /*
         * ITEM-516: the executor decodes the one action field and holds the
         * candidate; anything that is not the safe direction is refused by
         * name and writes nothing.  Nothing durable is written until the exact
         * token comes back.
         */
        if (body == NULL ||
            plane->panel_gate.stage(
                plane->panel_gate.context,
                (const uint8_t *)body,
                body_length) !=
                APPLIANCE_PRODUCTION_MUTATION_PLANE_PANEL_GATE_OK) {
            appliance_production_mutation_plane_rollback_pending(plane);
            appliance_production_mutation_plane_clear_pending(plane);
            appliance_production_mutation_plane_audit(
                plane,
                APPLIANCE_PRODUCTION_MUTATION_PLANE_AUDIT_APPLY_FAILED,
                mutation,
                0U,
                now_tick,
                &decision);
            return appliance_production_mutation_plane_finish(
                plane,
                APPLIANCE_PRODUCTION_MUTATION_PLANE_APPLY_FAILED,
                mutation,
                output,
                capacity,
                output_length);
        }
    } else {
        appliance_production_mutation_plane_audit(
            plane,
            APPLIANCE_PRODUCTION_MUTATION_PLANE_AUDIT_DENIED,
            mutation,
            0U,
            now_tick,
            &decision);
        return appliance_production_mutation_plane_finish(
            plane,
            APPLIANCE_PRODUCTION_MUTATION_PLANE_UNSUPPORTED_TARGET,
            mutation,
            output,
            capacity,
            output_length);
    }

    plane->pending_mutation = mutation;
    plane->pending_token =
        appliance_production_mutation_plane_issue_token(plane, now_tick);
    plane->pending_tick = now_tick;
    return appliance_production_mutation_plane_finish(
        plane,
        APPLIANCE_PRODUCTION_MUTATION_PLANE_PENDING_CONFIRMATION,
        mutation,
        output,
        capacity,
        output_length);
}

enum appliance_production_mutation_plane_status
appliance_production_mutation_plane_stop(
    struct appliance_production_mutation_plane *plane)
{
    if (!appliance_production_mutation_plane_initialized(plane)) {
        return APPLIANCE_PRODUCTION_MUTATION_PLANE_NULL;
    }
    if (plane->pending_mutation !=
        APPLIANCE_MANAGEMENT_AUTHORIZATION_MUTATION_NONE) {
        appliance_production_mutation_plane_rollback_pending(plane);
        appliance_production_mutation_plane_clear_pending(plane);
    }
    plane->state = APPLIANCE_PRODUCTION_MUTATION_PLANE_STATE_STOPPED;
    plane->last_status = APPLIANCE_PRODUCTION_MUTATION_PLANE_OK;
    return APPLIANCE_PRODUCTION_MUTATION_PLANE_OK;
}

enum appliance_production_mutation_plane_status
appliance_production_mutation_plane_fail_closed(
    struct appliance_production_mutation_plane *plane)
{
    if (!appliance_production_mutation_plane_initialized(plane)) {
        return APPLIANCE_PRODUCTION_MUTATION_PLANE_NULL;
    }
    if (plane->pending_mutation !=
        APPLIANCE_MANAGEMENT_AUTHORIZATION_MUTATION_NONE) {
        appliance_production_mutation_plane_rollback_pending(plane);
        appliance_production_mutation_plane_clear_pending(plane);
    }
    plane->state = APPLIANCE_PRODUCTION_MUTATION_PLANE_STATE_FAILED;
    plane->last_status = APPLIANCE_PRODUCTION_MUTATION_PLANE_UNAVAILABLE;
    return APPLIANCE_PRODUCTION_MUTATION_PLANE_UNAVAILABLE;
}

enum appliance_production_mutation_plane_state
appliance_production_mutation_plane_state(
    const struct appliance_production_mutation_plane *plane)
{
    if (!appliance_production_mutation_plane_initialized(plane)) {
        return APPLIANCE_PRODUCTION_MUTATION_PLANE_STATE_UNINITIALIZED;
    }
    if (plane->state == APPLIANCE_PRODUCTION_MUTATION_PLANE_STATE_STOPPED ||
        plane->state == APPLIANCE_PRODUCTION_MUTATION_PLANE_STATE_FAILED) {
        return plane->state;
    }
    if (plane->state ==
        APPLIANCE_PRODUCTION_MUTATION_PLANE_STATE_PENDING_CONFIRMATION) {
        return plane->state;
    }
    return appliance_production_mutation_plane_derive_state(plane);
}

bool appliance_production_mutation_plane_has_pending(
    const struct appliance_production_mutation_plane *plane)
{
    if (!appliance_production_mutation_plane_initialized(plane)) {
        return false;
    }
    return plane->pending_mutation !=
           APPLIANCE_MANAGEMENT_AUTHORIZATION_MUTATION_NONE;
}

uint64_t appliance_production_mutation_plane_pending_token(
    const struct appliance_production_mutation_plane *plane)
{
    if (!appliance_production_mutation_plane_has_pending(plane)) {
        return 0U;
    }
    return plane->pending_token;
}

const char *appliance_production_mutation_plane_certificate_name_refusal(
    const struct appliance_production_mutation_plane *plane)
{
    return plane == NULL ? NULL : plane->certificate_name_refusal;
}

enum appliance_management_authorization_mutation
appliance_production_mutation_plane_pending_mutation(
    const struct appliance_production_mutation_plane *plane)
{
    if (!appliance_production_mutation_plane_has_pending(plane)) {
        return APPLIANCE_MANAGEMENT_AUTHORIZATION_MUTATION_NONE;
    }
    return plane->pending_mutation;
}

uint32_t appliance_production_mutation_plane_applied_count(
    const struct appliance_production_mutation_plane *plane)
{
    if (!appliance_production_mutation_plane_initialized(plane)) {
        return 0U;
    }
    return plane->applied_count;
}

uint32_t appliance_production_mutation_plane_denied_count(
    const struct appliance_production_mutation_plane *plane)
{
    if (!appliance_production_mutation_plane_initialized(plane)) {
        return 0U;
    }
    return plane->denied_count;
}

uint32_t appliance_production_mutation_plane_cancelled_count(
    const struct appliance_production_mutation_plane *plane)
{
    if (!appliance_production_mutation_plane_initialized(plane)) {
        return 0U;
    }
    return plane->cancelled_count;
}

uint32_t appliance_production_mutation_plane_expired_count(
    const struct appliance_production_mutation_plane *plane)
{
    if (!appliance_production_mutation_plane_initialized(plane)) {
        return 0U;
    }
    return plane->expired_count;
}

uint32_t appliance_production_mutation_plane_unavailable_count(
    const struct appliance_production_mutation_plane *plane)
{
    if (!appliance_production_mutation_plane_initialized(plane)) {
        return 0U;
    }
    return plane->unavailable_count;
}

uint32_t appliance_production_mutation_plane_audit_count(
    const struct appliance_production_mutation_plane *plane)
{
    if (!appliance_production_mutation_plane_initialized(plane)) {
        return 0U;
    }
    return plane->audit_count;
}

bool appliance_production_mutation_plane_audit_entry(
    const struct appliance_production_mutation_plane *plane,
    uint32_t index,
    struct appliance_production_mutation_plane_audit_entry *entry_out)
{
    uint32_t retained;
    uint32_t start;
    uint32_t slot;

    if (!appliance_production_mutation_plane_initialized(plane) ||
        entry_out == NULL) {
        return false;
    }
    retained = plane->audit_count <
            APPLIANCE_PRODUCTION_MUTATION_PLANE_AUDIT_CAPACITY
        ? plane->audit_count
        : APPLIANCE_PRODUCTION_MUTATION_PLANE_AUDIT_CAPACITY;
    if (index >= retained) {
        return false;
    }
    start = (plane->audit_head +
             APPLIANCE_PRODUCTION_MUTATION_PLANE_AUDIT_CAPACITY - retained) %
            APPLIANCE_PRODUCTION_MUTATION_PLANE_AUDIT_CAPACITY;
    slot = (start + index) %
           APPLIANCE_PRODUCTION_MUTATION_PLANE_AUDIT_CAPACITY;
    *entry_out = plane->audit[slot];
    return true;
}

const char *appliance_production_mutation_plane_state_string(
    enum appliance_production_mutation_plane_state state)
{
    switch (state) {
    case APPLIANCE_PRODUCTION_MUTATION_PLANE_STATE_NONE:
        return "NONE";
    case APPLIANCE_PRODUCTION_MUTATION_PLANE_STATE_UNINITIALIZED:
        return "UNINITIALIZED";
    case APPLIANCE_PRODUCTION_MUTATION_PLANE_STATE_PREPARED:
        return "PREPARED";
    case APPLIANCE_PRODUCTION_MUTATION_PLANE_STATE_UNAVAILABLE:
        return "UNAVAILABLE";
    case APPLIANCE_PRODUCTION_MUTATION_PLANE_STATE_READY:
        return "READY";
    case APPLIANCE_PRODUCTION_MUTATION_PLANE_STATE_DEGRADED:
        return "DEGRADED";
    case APPLIANCE_PRODUCTION_MUTATION_PLANE_STATE_PENDING_CONFIRMATION:
        return "PENDING_CONFIRMATION";
    case APPLIANCE_PRODUCTION_MUTATION_PLANE_STATE_STOPPED:
        return "STOPPED";
    case APPLIANCE_PRODUCTION_MUTATION_PLANE_STATE_FAILED:
        return "FAILED";
    default:
        return "UNKNOWN";
    }
}

const char *appliance_production_mutation_plane_status_string(
    enum appliance_production_mutation_plane_status status)
{
    switch (status) {
    case APPLIANCE_PRODUCTION_MUTATION_PLANE_OK:
        return "OK";
    case APPLIANCE_PRODUCTION_MUTATION_PLANE_NULL:
        return "NULL";
    case APPLIANCE_PRODUCTION_MUTATION_PLANE_BAD_STATE:
        return "BAD_STATE";
    case APPLIANCE_PRODUCTION_MUTATION_PLANE_BAD_VERSION:
        return "BAD_VERSION";
    case APPLIANCE_PRODUCTION_MUTATION_PLANE_UNAVAILABLE:
        return "UNAVAILABLE";
    case APPLIANCE_PRODUCTION_MUTATION_PLANE_DENIED:
        return "DENIED";
    case APPLIANCE_PRODUCTION_MUTATION_PLANE_PENDING_CONFIRMATION:
        return "PENDING_CONFIRMATION";
    case APPLIANCE_PRODUCTION_MUTATION_PLANE_APPLIED:
        return "APPLIED";
    case APPLIANCE_PRODUCTION_MUTATION_PLANE_CANCELLED:
        return "CANCELLED";
    case APPLIANCE_PRODUCTION_MUTATION_PLANE_EXPIRED:
        return "EXPIRED";
    case APPLIANCE_PRODUCTION_MUTATION_PLANE_NO_PENDING:
        return "NO_PENDING";
    case APPLIANCE_PRODUCTION_MUTATION_PLANE_BAD_CONFIRMATION:
        return "BAD_CONFIRMATION";
    case APPLIANCE_PRODUCTION_MUTATION_PLANE_UNSUPPORTED_TARGET:
        return "UNSUPPORTED_TARGET";
    case APPLIANCE_PRODUCTION_MUTATION_PLANE_APPLY_FAILED:
        return "APPLY_FAILED";
    case APPLIANCE_PRODUCTION_MUTATION_PLANE_BUFFER_TOO_SMALL:
        return "BUFFER_TOO_SMALL";
    default:
        return "UNKNOWN";
    }
}

const char *appliance_production_mutation_plane_audit_outcome_string(
    enum appliance_production_mutation_plane_audit_outcome outcome)
{
    switch (outcome) {
    case APPLIANCE_PRODUCTION_MUTATION_PLANE_AUDIT_NONE:
        return "NONE";
    case APPLIANCE_PRODUCTION_MUTATION_PLANE_AUDIT_ALLOWED:
        return "ALLOWED";
    case APPLIANCE_PRODUCTION_MUTATION_PLANE_AUDIT_DENIED:
        return "DENIED";
    case APPLIANCE_PRODUCTION_MUTATION_PLANE_AUDIT_UNAVAILABLE:
        return "UNAVAILABLE";
    case APPLIANCE_PRODUCTION_MUTATION_PLANE_AUDIT_CANCELLED:
        return "CANCELLED";
    case APPLIANCE_PRODUCTION_MUTATION_PLANE_AUDIT_EXPIRED:
        return "EXPIRED";
    case APPLIANCE_PRODUCTION_MUTATION_PLANE_AUDIT_APPLY_FAILED:
        return "APPLY_FAILED";
    default:
        return "UNKNOWN";
    }
}
