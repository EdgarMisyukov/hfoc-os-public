#include "appliance_recovery_factory_reset_integration.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

static void recovery_integration_zero_bytes(
    void *destination,
    size_t length)
{
    unsigned char *bytes = destination;
    size_t index;

    for (index = 0U; index < length; ++index) {
        bytes[index] = 0U;
    }
}

static bool recovery_integration_state_valid(
    enum appliance_recovery_factory_reset_integration_state state)
{
    return state >=
               APPLIANCE_RECOVERY_FACTORY_RESET_INTEGRATION_STATE_UNINITIALIZED &&
           state <=
               APPLIANCE_RECOVERY_FACTORY_RESET_INTEGRATION_STATE_FAILED;
}

static bool recovery_integration_status_valid(
    enum appliance_recovery_factory_reset_integration_status status)
{
    return status >= APPLIANCE_RECOVERY_FACTORY_RESET_INTEGRATION_OK &&
           status <=
               APPLIANCE_RECOVERY_FACTORY_RESET_INTEGRATION_BUFFER_TOO_SMALL;
}

static bool recovery_integration_failure_valid(
    enum appliance_recovery_factory_reset_integration_failure failure)
{
    return failure >=
               APPLIANCE_RECOVERY_FACTORY_RESET_INTEGRATION_FAILURE_NONE &&
           failure <=
               APPLIANCE_RECOVERY_FACTORY_RESET_INTEGRATION_FAILURE_COMPLETE;
}

static bool recovery_integration_operation_valid(uint32_t operation)
{
    return operation == APPLIANCE_RECOVERY_FACTORY_RESET_OPERATION_NONE ||
           operation == APPLIANCE_RECOVERY_FACTORY_RESET_OPERATION_RECOVERY ||
           operation ==
               APPLIANCE_RECOVERY_FACTORY_RESET_OPERATION_FACTORY_RESET;
}

static bool recovery_integration_action_valid(uint32_t action)
{
    return action <= 5U;
}

static bool recovery_integration_initialized(
    const struct appliance_recovery_factory_reset_integration *integration)
{
    return integration != NULL &&
           integration->initialized &&
           integration->version ==
               APPLIANCE_RECOVERY_FACTORY_RESET_INTEGRATION_VERSION &&
           recovery_integration_state_valid(integration->state) &&
           recovery_integration_status_valid(integration->last_status) &&
           recovery_integration_failure_valid(integration->failure);
}

static void recovery_integration_clear_evidence(
    struct appliance_recovery_factory_reset_integration *integration)
{
    integration->source_generation = 0U;
    integration->evidence_published = false;
    integration->operation_kind = APPLIANCE_RECOVERY_FACTORY_RESET_OPERATION_NONE;
    integration->recovery_action = 0U;
    integration->recovery_requires_reboot = false;
    integration->factory_reset_plan_published = false;
    integration->authorization_verified = false;
    integration->boot_foundation_verified = false;
    integration->storage_loader_ready = false;
    integration->installer_recovery_ready = false;
    integration->efi_environment_ready = false;
    integration->management_active = false;
    integration->first_boot_complete = false;
    integration->network_active = false;
    integration->https_management_ready = false;
}

static bool recovery_integration_operation_complete(
    const struct appliance_recovery_factory_reset_integration_evidence *evidence)
{
    if (evidence->operation_kind ==
        APPLIANCE_RECOVERY_FACTORY_RESET_OPERATION_RECOVERY) {
        if (!evidence->recovery_decision_present ||
            evidence->recovery_action == 0U ||
            !recovery_integration_action_valid(
                evidence->recovery_action) ||
            !evidence->storage_loader_ready ||
            !evidence->boot_foundation_verified ||
            !evidence->efi_environment_ready) {
            return false;
        }
        if (evidence->recovery_requires_reboot &&
            !evidence->installer_recovery_ready) {
            return false;
        }
    } else if (evidence->operation_kind ==
               APPLIANCE_RECOVERY_FACTORY_RESET_OPERATION_FACTORY_RESET) {
        if (!evidence->factory_reset_requested ||
            !evidence->factory_reset_plan_published ||
            !evidence->authorization_verified ||
            !evidence->boot_foundation_verified ||
            !evidence->storage_loader_ready ||
            !evidence->installer_recovery_ready ||
            !evidence->efi_environment_ready) {
            return false;
        }
    } else {
        return false;
    }
    if (evidence->management_profile_required &&
        (!evidence->management_active ||
         !evidence->network_active ||
         !evidence->https_management_ready)) {
        return false;
    }
    return true;
}

static enum appliance_recovery_factory_reset_integration_status
recovery_integration_set_failure(
    struct appliance_recovery_factory_reset_integration *integration,
    enum appliance_recovery_factory_reset_integration_failure failure)
{
    if (!recovery_integration_failure_valid(failure) ||
        failure == APPLIANCE_RECOVERY_FACTORY_RESET_INTEGRATION_FAILURE_NONE) {
        return APPLIANCE_RECOVERY_FACTORY_RESET_INTEGRATION_BAD_STATE;
    }
    integration->state =
        APPLIANCE_RECOVERY_FACTORY_RESET_INTEGRATION_STATE_FAILED;
    integration->failure = failure;
    integration->last_status = APPLIANCE_RECOVERY_FACTORY_RESET_INTEGRATION_OK;
    recovery_integration_clear_evidence(integration);
    ++integration->fail_count;
    return APPLIANCE_RECOVERY_FACTORY_RESET_INTEGRATION_OK;
}

enum appliance_recovery_factory_reset_integration_status
appliance_recovery_factory_reset_integration_init(
    struct appliance_recovery_factory_reset_integration *integration)
{
    if (integration == NULL) {
        return APPLIANCE_RECOVERY_FACTORY_RESET_INTEGRATION_NULL;
    }
    if (integration->initialized) {
        return APPLIANCE_RECOVERY_FACTORY_RESET_INTEGRATION_ALREADY_PUBLISHED;
    }
    recovery_integration_zero_bytes(
        integration, sizeof(*integration));
    integration->initialized = true;
    integration->version =
        APPLIANCE_RECOVERY_FACTORY_RESET_INTEGRATION_VERSION;
    integration->state =
        APPLIANCE_RECOVERY_FACTORY_RESET_INTEGRATION_STATE_PREPARED;
    integration->last_status = APPLIANCE_RECOVERY_FACTORY_RESET_INTEGRATION_OK;
    integration->failure =
        APPLIANCE_RECOVERY_FACTORY_RESET_INTEGRATION_FAILURE_NONE;
    return APPLIANCE_RECOVERY_FACTORY_RESET_INTEGRATION_OK;
}

enum appliance_recovery_factory_reset_integration_status
appliance_recovery_factory_reset_integration_publish(
    struct appliance_recovery_factory_reset_integration *integration,
    const struct appliance_recovery_factory_reset_integration_evidence *evidence)
{
    bool complete;

    if (integration == NULL || evidence == NULL) {
        return APPLIANCE_RECOVERY_FACTORY_RESET_INTEGRATION_NULL;
    }
    if (!recovery_integration_initialized(integration)) {
        return APPLIANCE_RECOVERY_FACTORY_RESET_INTEGRATION_UNPUBLISHED;
    }
    if (integration->state ==
            APPLIANCE_RECOVERY_FACTORY_RESET_INTEGRATION_STATE_COMPLETE ||
        integration->state ==
            APPLIANCE_RECOVERY_FACTORY_RESET_INTEGRATION_STATE_FAILED) {
        return APPLIANCE_RECOVERY_FACTORY_RESET_INTEGRATION_BAD_STATE;
    }
    if (evidence->version !=
        APPLIANCE_RECOVERY_FACTORY_RESET_INTEGRATION_EVIDENCE_VERSION) {
        return APPLIANCE_RECOVERY_FACTORY_RESET_INTEGRATION_BAD_VERSION;
    }
    if (!evidence->published ||
        evidence->source_generation == 0U) {
        return APPLIANCE_RECOVERY_FACTORY_RESET_INTEGRATION_INVALID_EVIDENCE;
    }
    if (evidence->recovery_trust_active ||
        !recovery_integration_operation_valid(evidence->operation_kind) ||
        (evidence->operation_kind ==
             APPLIANCE_RECOVERY_FACTORY_RESET_OPERATION_RECOVERY &&
         evidence->factory_reset_requested) ||
        (evidence->operation_kind ==
             APPLIANCE_RECOVERY_FACTORY_RESET_OPERATION_FACTORY_RESET &&
         evidence->recovery_decision_present)) {
        return APPLIANCE_RECOVERY_FACTORY_RESET_INTEGRATION_INVALID_EVIDENCE;
    }
    if (evidence->source_generation < integration->source_generation) {
        return APPLIANCE_RECOVERY_FACTORY_RESET_INTEGRATION_STALE_EVIDENCE;
    }

    complete = recovery_integration_operation_complete(evidence);
    if (integration->state ==
        APPLIANCE_RECOVERY_FACTORY_RESET_INTEGRATION_STATE_READY) {
        if (!complete) {
            return recovery_integration_set_failure(
                integration,
                APPLIANCE_RECOVERY_FACTORY_RESET_INTEGRATION_FAILURE_EVIDENCE);
        }
    } else if (!complete) {
        if (evidence->operation_kind ==
            APPLIANCE_RECOVERY_FACTORY_RESET_OPERATION_NONE) {
            integration->state =
                APPLIANCE_RECOVERY_FACTORY_RESET_INTEGRATION_STATE_AWAITING_RECOVERY;
        } else if (evidence->operation_kind ==
                       APPLIANCE_RECOVERY_FACTORY_RESET_OPERATION_FACTORY_RESET &&
                   !evidence->authorization_verified) {
            integration->state =
                APPLIANCE_RECOVERY_FACTORY_RESET_INTEGRATION_STATE_AWAITING_AUTHORIZATION;
        } else {
            integration->state =
                APPLIANCE_RECOVERY_FACTORY_RESET_INTEGRATION_STATE_AWAITING_FOUNDATION;
        }
        integration->last_status =
            APPLIANCE_RECOVERY_FACTORY_RESET_INTEGRATION_MISSING_EVIDENCE;
        return APPLIANCE_RECOVERY_FACTORY_RESET_INTEGRATION_MISSING_EVIDENCE;
    }

    integration->source_generation = evidence->source_generation;
    integration->evidence_published = true;
    integration->operation_kind = evidence->operation_kind;
    integration->recovery_action = evidence->recovery_action;
    integration->recovery_requires_reboot =
        evidence->recovery_requires_reboot;
    integration->factory_reset_plan_published =
        evidence->factory_reset_plan_published;
    integration->authorization_verified =
        evidence->authorization_verified;
    integration->boot_foundation_verified =
        evidence->boot_foundation_verified;
    integration->storage_loader_ready = evidence->storage_loader_ready;
    integration->installer_recovery_ready =
        evidence->installer_recovery_ready;
    integration->efi_environment_ready =
        evidence->efi_environment_ready;
    integration->management_active = evidence->management_active;
    integration->first_boot_complete = evidence->first_boot_complete;
    integration->network_active = evidence->network_active;
    integration->https_management_ready =
        evidence->https_management_ready;
    integration->state =
        APPLIANCE_RECOVERY_FACTORY_RESET_INTEGRATION_STATE_READY;
    integration->last_status = APPLIANCE_RECOVERY_FACTORY_RESET_INTEGRATION_OK;
    integration->failure =
        APPLIANCE_RECOVERY_FACTORY_RESET_INTEGRATION_FAILURE_NONE;
    ++integration->update_count;
    return APPLIANCE_RECOVERY_FACTORY_RESET_INTEGRATION_OK;
}

enum appliance_recovery_factory_reset_integration_status
appliance_recovery_factory_reset_integration_complete(
    struct appliance_recovery_factory_reset_integration *integration)
{
    if (integration == NULL) {
        return APPLIANCE_RECOVERY_FACTORY_RESET_INTEGRATION_NULL;
    }
    if (!recovery_integration_initialized(integration)) {
        return APPLIANCE_RECOVERY_FACTORY_RESET_INTEGRATION_UNPUBLISHED;
    }
    if (integration->state !=
        APPLIANCE_RECOVERY_FACTORY_RESET_INTEGRATION_STATE_READY) {
        return APPLIANCE_RECOVERY_FACTORY_RESET_INTEGRATION_BAD_STATE;
    }
    integration->state =
        APPLIANCE_RECOVERY_FACTORY_RESET_INTEGRATION_STATE_COMPLETE;
    integration->last_status = APPLIANCE_RECOVERY_FACTORY_RESET_INTEGRATION_OK;
    return APPLIANCE_RECOVERY_FACTORY_RESET_INTEGRATION_OK;
}

enum appliance_recovery_factory_reset_integration_status
appliance_recovery_factory_reset_integration_fail_closed(
    struct appliance_recovery_factory_reset_integration *integration,
    enum appliance_recovery_factory_reset_integration_failure failure)
{
    if (integration == NULL) {
        return APPLIANCE_RECOVERY_FACTORY_RESET_INTEGRATION_NULL;
    }
    if (!recovery_integration_initialized(integration)) {
        return APPLIANCE_RECOVERY_FACTORY_RESET_INTEGRATION_UNPUBLISHED;
    }
    if (integration->state ==
            APPLIANCE_RECOVERY_FACTORY_RESET_INTEGRATION_STATE_COMPLETE ||
        integration->state ==
            APPLIANCE_RECOVERY_FACTORY_RESET_INTEGRATION_STATE_FAILED) {
        return APPLIANCE_RECOVERY_FACTORY_RESET_INTEGRATION_BAD_STATE;
    }
    return recovery_integration_set_failure(integration, failure);
}

enum appliance_recovery_factory_reset_integration_status
appliance_recovery_factory_reset_integration_reset(
    struct appliance_recovery_factory_reset_integration *integration)
{
    if (integration == NULL) {
        return APPLIANCE_RECOVERY_FACTORY_RESET_INTEGRATION_NULL;
    }
    if (!recovery_integration_initialized(integration)) {
        return APPLIANCE_RECOVERY_FACTORY_RESET_INTEGRATION_UNPUBLISHED;
    }
    recovery_integration_clear_evidence(integration);
    integration->state =
        APPLIANCE_RECOVERY_FACTORY_RESET_INTEGRATION_STATE_PREPARED;
    integration->last_status = APPLIANCE_RECOVERY_FACTORY_RESET_INTEGRATION_OK;
    integration->failure =
        APPLIANCE_RECOVERY_FACTORY_RESET_INTEGRATION_FAILURE_NONE;
    ++integration->reset_count;
    return APPLIANCE_RECOVERY_FACTORY_RESET_INTEGRATION_OK;
}

enum appliance_recovery_factory_reset_integration_status
appliance_recovery_factory_reset_integration_snapshot(
    const struct appliance_recovery_factory_reset_integration *integration,
    struct appliance_recovery_factory_reset_integration_snapshot *snapshot_out)
{
    enum appliance_recovery_factory_reset_integration_health health;

    if (integration == NULL || snapshot_out == NULL) {
        return APPLIANCE_RECOVERY_FACTORY_RESET_INTEGRATION_NULL;
    }
    if (!recovery_integration_initialized(integration)) {
        return APPLIANCE_RECOVERY_FACTORY_RESET_INTEGRATION_UNPUBLISHED;
    }
    if (integration->state ==
            APPLIANCE_RECOVERY_FACTORY_RESET_INTEGRATION_STATE_READY ||
        integration->state ==
            APPLIANCE_RECOVERY_FACTORY_RESET_INTEGRATION_STATE_COMPLETE) {
        health = APPLIANCE_RECOVERY_FACTORY_RESET_INTEGRATION_HEALTH_PASS;
    } else if (integration->state ==
               APPLIANCE_RECOVERY_FACTORY_RESET_INTEGRATION_STATE_FAILED) {
        health = APPLIANCE_RECOVERY_FACTORY_RESET_INTEGRATION_HEALTH_FAIL;
    } else {
        health = APPLIANCE_RECOVERY_FACTORY_RESET_INTEGRATION_HEALTH_UNKNOWN;
    }
    recovery_integration_zero_bytes(
        snapshot_out, sizeof(*snapshot_out));
    snapshot_out->published = true;
    snapshot_out->version =
        APPLIANCE_RECOVERY_FACTORY_RESET_INTEGRATION_VERSION;
    snapshot_out->state = integration->state;
    snapshot_out->last_status = integration->last_status;
    snapshot_out->failure = integration->failure;
    snapshot_out->health = health;
    snapshot_out->operation_kind = integration->operation_kind;
    snapshot_out->recovery_action = integration->recovery_action;
    snapshot_out->recovery_requires_reboot =
        integration->recovery_requires_reboot;
    snapshot_out->factory_reset_plan_published =
        integration->factory_reset_plan_published;
    snapshot_out->authorization_verified =
        integration->authorization_verified;
    snapshot_out->boot_foundation_verified =
        integration->boot_foundation_verified;
    snapshot_out->storage_loader_ready =
        integration->storage_loader_ready;
    snapshot_out->installer_recovery_ready =
        integration->installer_recovery_ready;
    snapshot_out->efi_environment_ready =
        integration->efi_environment_ready;
    snapshot_out->management_active = integration->management_active;
    snapshot_out->first_boot_complete = integration->first_boot_complete;
    snapshot_out->network_active = integration->network_active;
    snapshot_out->https_management_ready =
        integration->https_management_ready;
    snapshot_out->source_generation = integration->source_generation;
    snapshot_out->update_count = integration->update_count;
    snapshot_out->reset_count = integration->reset_count;
    snapshot_out->fail_count = integration->fail_count;
    return APPLIANCE_RECOVERY_FACTORY_RESET_INTEGRATION_OK;
}

static bool recovery_integration_append_text(
    char *output,
    size_t *offset,
    const char *text)
{
    size_t length = 0U;
    size_t index;

    if (text == NULL ||
        *offset > APPLIANCE_RECOVERY_FACTORY_RESET_INTEGRATION_OUTPUT_MAX) {
        return false;
    }
    while (text[length] != '\0') {
        ++length;
    }
    if (length >
        APPLIANCE_RECOVERY_FACTORY_RESET_INTEGRATION_OUTPUT_MAX - *offset) {
        return false;
    }
    for (index = 0U; index < length; ++index) {
        output[*offset + index] = text[index];
    }
    *offset += length;
    return true;
}

static const char *recovery_integration_operation_name(uint32_t operation)
{
    if (operation == APPLIANCE_RECOVERY_FACTORY_RESET_OPERATION_RECOVERY) {
        return "RECOVERY";
    }
    if (operation == APPLIANCE_RECOVERY_FACTORY_RESET_OPERATION_FACTORY_RESET) {
        return "FACTORY_RESET";
    }
    return "NONE";
}

static const char *recovery_integration_recovery_action_name(uint32_t action)
{
    switch (action) {
    case 1U:
        return "KEEP_LAST_KNOWN_GOOD";
    case 2U:
        return "SAFE_DEFAULT";
    case 3U:
        return "AUTOMATIC_ROLLBACK";
    case 4U:
        return "SUPERVISED_RESTART";
    case 5U:
        return "READ_ONLY";
    default:
        return "NONE";
    }
}

enum appliance_recovery_factory_reset_integration_status
appliance_recovery_factory_reset_integration_render(
    const struct appliance_recovery_factory_reset_integration_snapshot *snapshot,
    char *output,
    size_t capacity,
    size_t *output_length)
{
    char rendered[
        APPLIANCE_RECOVERY_FACTORY_RESET_INTEGRATION_OUTPUT_MAX + 1U];
    size_t offset = 0U;
    const char *state;
    const char *operation;
    const char *recovery;
    const char *reset;
    const char *auth;
    const char *boot;
    const char *storage;
    const char *installer;
    const char *efi;
    const char *management;
    const char *health;

    if (snapshot == NULL || output == NULL || output_length == NULL) {
        return APPLIANCE_RECOVERY_FACTORY_RESET_INTEGRATION_NULL;
    }
    if (!snapshot->published ||
        snapshot->version !=
            APPLIANCE_RECOVERY_FACTORY_RESET_INTEGRATION_VERSION ||
        !recovery_integration_state_valid(snapshot->state) ||
        !recovery_integration_status_valid(snapshot->last_status) ||
        !recovery_integration_failure_valid(snapshot->failure)) {
        return APPLIANCE_RECOVERY_FACTORY_RESET_INTEGRATION_BAD_STATE;
    }
    recovery_integration_zero_bytes(rendered, sizeof(rendered));
    state = appliance_recovery_factory_reset_integration_state_string(
        snapshot->state);
    operation = recovery_integration_operation_name(
        snapshot->operation_kind);
    recovery = recovery_integration_recovery_action_name(
        snapshot->recovery_action);
    reset = snapshot->factory_reset_plan_published ? "APPROVED" : "NONE";
    auth = snapshot->authorization_verified ? "VERIFIED" : "UNKNOWN";
    boot = snapshot->boot_foundation_verified ? "VERIFIED" : "UNKNOWN";
    storage = snapshot->storage_loader_ready ? "READY" : "UNKNOWN";
    installer = snapshot->installer_recovery_ready ? "READY" : "UNKNOWN";
    efi = snapshot->efi_environment_ready ? "READY" : "UNKNOWN";
    management = snapshot->management_active ? "ACTIVE" : "UNKNOWN";
    health = snapshot->health ==
            APPLIANCE_RECOVERY_FACTORY_RESET_INTEGRATION_HEALTH_PASS
        ? "PASS"
        : (snapshot->health ==
                   APPLIANCE_RECOVERY_FACTORY_RESET_INTEGRATION_HEALTH_FAIL
               ? "FAIL"
               : "UNKNOWN");
    if (state == NULL ||
        !recovery_integration_append_text(
            rendered, &offset, "recovery_integration v=1 state=") ||
        !recovery_integration_append_text(rendered, &offset, state) ||
        !recovery_integration_append_text(
            rendered, &offset, " operation=") ||
        !recovery_integration_append_text(rendered, &offset, operation) ||
        !recovery_integration_append_text(
            rendered, &offset, " recovery=") ||
        !recovery_integration_append_text(rendered, &offset, recovery) ||
        !recovery_integration_append_text(
            rendered, &offset, " reset=") ||
        !recovery_integration_append_text(rendered, &offset, reset) ||
        !recovery_integration_append_text(
            rendered, &offset, " auth=") ||
        !recovery_integration_append_text(rendered, &offset, auth) ||
        !recovery_integration_append_text(
            rendered, &offset, " boot=") ||
        !recovery_integration_append_text(rendered, &offset, boot) ||
        !recovery_integration_append_text(
            rendered, &offset, " storage=") ||
        !recovery_integration_append_text(rendered, &offset, storage) ||
        !recovery_integration_append_text(
            rendered, &offset, " installer=") ||
        !recovery_integration_append_text(rendered, &offset, installer) ||
        !recovery_integration_append_text(
            rendered, &offset, " efi=") ||
        !recovery_integration_append_text(rendered, &offset, efi) ||
        !recovery_integration_append_text(
            rendered, &offset, " management=") ||
        !recovery_integration_append_text(rendered, &offset, management) ||
        !recovery_integration_append_text(
            rendered, &offset, " health=") ||
        !recovery_integration_append_text(rendered, &offset, health) ||
        !recovery_integration_append_text(rendered, &offset, "\n")) {
        return APPLIANCE_RECOVERY_FACTORY_RESET_INTEGRATION_BUFFER_TOO_SMALL;
    }
    if (offset + 1U > capacity) {
        return APPLIANCE_RECOVERY_FACTORY_RESET_INTEGRATION_BUFFER_TOO_SMALL;
    }
    recovery_integration_zero_bytes(output, capacity);
    {
        size_t index;

        for (index = 0U; index < offset; ++index) {
            output[index] = rendered[index];
        }
    }
    output[offset] = '\0';
    *output_length = offset;
    return APPLIANCE_RECOVERY_FACTORY_RESET_INTEGRATION_OK;
}

enum appliance_recovery_factory_reset_integration_state
appliance_recovery_factory_reset_integration_state(
    const struct appliance_recovery_factory_reset_integration *integration)
{
    if (!recovery_integration_initialized(integration)) {
        return APPLIANCE_RECOVERY_FACTORY_RESET_INTEGRATION_STATE_NONE;
    }
    return integration->state;
}

enum appliance_recovery_factory_reset_integration_failure
appliance_recovery_factory_reset_integration_failure(
    const struct appliance_recovery_factory_reset_integration *integration)
{
    if (!recovery_integration_initialized(integration)) {
        return APPLIANCE_RECOVERY_FACTORY_RESET_INTEGRATION_FAILURE_NONE;
    }
    return integration->failure;
}

bool appliance_recovery_factory_reset_integration_published(
    const struct appliance_recovery_factory_reset_integration *integration)
{
    return recovery_integration_initialized(integration) &&
           integration->evidence_published;
}

const char *appliance_recovery_factory_reset_integration_state_string(
    enum appliance_recovery_factory_reset_integration_state state)
{
    switch (state) {
    case APPLIANCE_RECOVERY_FACTORY_RESET_INTEGRATION_STATE_UNINITIALIZED:
        return "UNINITIALIZED";
    case APPLIANCE_RECOVERY_FACTORY_RESET_INTEGRATION_STATE_PREPARED:
        return "PREPARED";
    case APPLIANCE_RECOVERY_FACTORY_RESET_INTEGRATION_STATE_AWAITING_RECOVERY:
        return "AWAITING_RECOVERY";
    case APPLIANCE_RECOVERY_FACTORY_RESET_INTEGRATION_STATE_AWAITING_FOUNDATION:
        return "AWAITING_FOUNDATION";
    case APPLIANCE_RECOVERY_FACTORY_RESET_INTEGRATION_STATE_AWAITING_AUTHORIZATION:
        return "AWAITING_AUTHORIZATION";
    case APPLIANCE_RECOVERY_FACTORY_RESET_INTEGRATION_STATE_READY:
        return "READY";
    case APPLIANCE_RECOVERY_FACTORY_RESET_INTEGRATION_STATE_COMPLETE:
        return "COMPLETE";
    case APPLIANCE_RECOVERY_FACTORY_RESET_INTEGRATION_STATE_FAILED:
        return "FAILED";
    default:
        return NULL;
    }
}

const char *appliance_recovery_factory_reset_integration_status_string(
    enum appliance_recovery_factory_reset_integration_status status)
{
    switch (status) {
    case APPLIANCE_RECOVERY_FACTORY_RESET_INTEGRATION_OK:
        return "OK";
    case APPLIANCE_RECOVERY_FACTORY_RESET_INTEGRATION_NULL:
        return "NULL";
    case APPLIANCE_RECOVERY_FACTORY_RESET_INTEGRATION_BAD_STATE:
        return "BAD_STATE";
    case APPLIANCE_RECOVERY_FACTORY_RESET_INTEGRATION_BAD_VERSION:
        return "BAD_VERSION";
    case APPLIANCE_RECOVERY_FACTORY_RESET_INTEGRATION_ALREADY_PUBLISHED:
        return "ALREADY_PUBLISHED";
    case APPLIANCE_RECOVERY_FACTORY_RESET_INTEGRATION_UNPUBLISHED:
        return "UNPUBLISHED";
    case APPLIANCE_RECOVERY_FACTORY_RESET_INTEGRATION_INVALID_EVIDENCE:
        return "INVALID_EVIDENCE";
    case APPLIANCE_RECOVERY_FACTORY_RESET_INTEGRATION_STALE_EVIDENCE:
        return "STALE_EVIDENCE";
    case APPLIANCE_RECOVERY_FACTORY_RESET_INTEGRATION_MISSING_EVIDENCE:
        return "MISSING_EVIDENCE";
    case APPLIANCE_RECOVERY_FACTORY_RESET_INTEGRATION_BUFFER_TOO_SMALL:
        return "BUFFER_TOO_SMALL";
    default:
        return NULL;
    }
}

const char *appliance_recovery_factory_reset_integration_failure_string(
    enum appliance_recovery_factory_reset_integration_failure failure)
{
    switch (failure) {
    case APPLIANCE_RECOVERY_FACTORY_RESET_INTEGRATION_FAILURE_NONE:
        return "NONE";
    case APPLIANCE_RECOVERY_FACTORY_RESET_INTEGRATION_FAILURE_EVIDENCE:
        return "EVIDENCE";
    case APPLIANCE_RECOVERY_FACTORY_RESET_INTEGRATION_FAILURE_STATE:
        return "STATE";
    case APPLIANCE_RECOVERY_FACTORY_RESET_INTEGRATION_FAILURE_RECOVERY:
        return "RECOVERY";
    case APPLIANCE_RECOVERY_FACTORY_RESET_INTEGRATION_FAILURE_FACTORY_RESET:
        return "FACTORY_RESET";
    case APPLIANCE_RECOVERY_FACTORY_RESET_INTEGRATION_FAILURE_AUTHORIZATION:
        return "AUTHORIZATION";
    case APPLIANCE_RECOVERY_FACTORY_RESET_INTEGRATION_FAILURE_FOUNDATION:
        return "FOUNDATION";
    case APPLIANCE_RECOVERY_FACTORY_RESET_INTEGRATION_FAILURE_COMPLETE:
        return "COMPLETE";
    default:
        return NULL;
    }
}

const char *appliance_recovery_factory_reset_integration_operation_string(
    uint32_t operation)
{
    return recovery_integration_operation_name(operation);
}
