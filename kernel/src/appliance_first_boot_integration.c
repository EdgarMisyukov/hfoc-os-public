#include "appliance_first_boot_integration.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

static void appliance_first_boot_integration_zero_bytes(
    void *destination,
    size_t length)
{
    unsigned char *bytes = destination;
    size_t index;

    for (index = 0U; index < length; ++index) {
        bytes[index] = 0U;
    }
}

static bool appliance_first_boot_integration_state_valid(
    enum appliance_first_boot_integration_state state)
{
    return state >= APPLIANCE_FIRST_BOOT_INTEGRATION_STATE_UNINITIALIZED &&
           state <= APPLIANCE_FIRST_BOOT_INTEGRATION_STATE_FAILED;
}

static bool appliance_first_boot_integration_status_valid(
    enum appliance_first_boot_integration_status status)
{
    return status >= APPLIANCE_FIRST_BOOT_INTEGRATION_OK &&
           status <= APPLIANCE_FIRST_BOOT_INTEGRATION_BUFFER_TOO_SMALL;
}

static bool appliance_first_boot_integration_failure_valid(
    enum appliance_first_boot_integration_failure failure)
{
    return failure >= APPLIANCE_FIRST_BOOT_INTEGRATION_FAILURE_NONE &&
           failure <= APPLIANCE_FIRST_BOOT_INTEGRATION_FAILURE_COMPLETE;
}

static bool appliance_first_boot_integration_initialized(
    const struct appliance_first_boot_integration *integration)
{
    return integration != NULL &&
           integration->initialized &&
           integration->version ==
               APPLIANCE_FIRST_BOOT_INTEGRATION_VERSION &&
           appliance_first_boot_integration_state_valid(
               integration->state) &&
           appliance_first_boot_integration_status_valid(
               integration->last_status) &&
           appliance_first_boot_integration_failure_valid(
               integration->failure);
}

static void appliance_first_boot_integration_clear_evidence(
    struct appliance_first_boot_integration *integration)
{
    integration->source_generation = 0U;
    integration->evidence_published = false;
    integration->first_boot_pending = false;
    integration->storage_ready = false;
    integration->wizard_complete = false;
    integration->management_active = false;
    integration->network_active = false;
    integration->https_management_ready = false;
}

static bool appliance_first_boot_integration_evidence_complete(
    const struct appliance_first_boot_integration_evidence *evidence)
{
    return evidence->storage_ready &&
           evidence->wizard_complete &&
           evidence->management_active &&
           evidence->network_active &&
           evidence->https_management_ready;
}

static enum appliance_first_boot_integration_status
appliance_first_boot_integration_set_failure(
    struct appliance_first_boot_integration *integration,
    enum appliance_first_boot_integration_failure failure)
{
    if (integration == NULL) {
        return APPLIANCE_FIRST_BOOT_INTEGRATION_NULL;
    }
    if (!appliance_first_boot_integration_failure_valid(failure) ||
        failure == APPLIANCE_FIRST_BOOT_INTEGRATION_FAILURE_NONE) {
        return APPLIANCE_FIRST_BOOT_INTEGRATION_BAD_STATE;
    }
    integration->state = APPLIANCE_FIRST_BOOT_INTEGRATION_STATE_FAILED;
    integration->failure = failure;
    integration->last_status = APPLIANCE_FIRST_BOOT_INTEGRATION_OK;
    appliance_first_boot_integration_clear_evidence(integration);
    ++integration->fail_count;
    return APPLIANCE_FIRST_BOOT_INTEGRATION_OK;
}

enum appliance_first_boot_integration_status
appliance_first_boot_integration_init(
    struct appliance_first_boot_integration *integration)
{
    if (integration == NULL) {
        return APPLIANCE_FIRST_BOOT_INTEGRATION_NULL;
    }
    if (integration->initialized) {
        return APPLIANCE_FIRST_BOOT_INTEGRATION_ALREADY_PUBLISHED;
    }
    appliance_first_boot_integration_zero_bytes(
        integration, sizeof(*integration));
    integration->initialized = true;
    integration->version = APPLIANCE_FIRST_BOOT_INTEGRATION_VERSION;
    integration->state = APPLIANCE_FIRST_BOOT_INTEGRATION_STATE_PREPARED;
    integration->last_status = APPLIANCE_FIRST_BOOT_INTEGRATION_OK;
    integration->failure = APPLIANCE_FIRST_BOOT_INTEGRATION_FAILURE_NONE;
    return APPLIANCE_FIRST_BOOT_INTEGRATION_OK;
}

enum appliance_first_boot_integration_status
appliance_first_boot_integration_publish(
    struct appliance_first_boot_integration *integration,
    const struct appliance_first_boot_integration_evidence *evidence)
{
    bool complete;

    if (integration == NULL || evidence == NULL) {
        return APPLIANCE_FIRST_BOOT_INTEGRATION_NULL;
    }
    if (!appliance_first_boot_integration_initialized(integration)) {
        return APPLIANCE_FIRST_BOOT_INTEGRATION_UNPUBLISHED;
    }
    if (integration->state ==
            APPLIANCE_FIRST_BOOT_INTEGRATION_STATE_COMPLETE ||
        integration->state ==
            APPLIANCE_FIRST_BOOT_INTEGRATION_STATE_FAILED) {
        return APPLIANCE_FIRST_BOOT_INTEGRATION_BAD_STATE;
    }
    if (evidence->version !=
        APPLIANCE_FIRST_BOOT_INTEGRATION_EVIDENCE_VERSION) {
        return APPLIANCE_FIRST_BOOT_INTEGRATION_BAD_VERSION;
    }
    if (!evidence->published ||
        evidence->source_generation == 0U) {
        return APPLIANCE_FIRST_BOOT_INTEGRATION_INVALID_EVIDENCE;
    }
    if (!evidence->first_boot_pending) {
        return APPLIANCE_FIRST_BOOT_INTEGRATION_INVALID_EVIDENCE;
    }
    if (evidence->source_generation <
        integration->source_generation) {
        return APPLIANCE_FIRST_BOOT_INTEGRATION_STALE_EVIDENCE;
    }

    complete = appliance_first_boot_integration_evidence_complete(
        evidence);
    if (integration->state ==
        APPLIANCE_FIRST_BOOT_INTEGRATION_STATE_READY) {
        if (!complete) {
            return appliance_first_boot_integration_set_failure(
                integration,
                APPLIANCE_FIRST_BOOT_INTEGRATION_FAILURE_EVIDENCE);
        }
    } else if (!complete) {
        if (!evidence->storage_ready ||
            !evidence->wizard_complete) {
            integration->state =
                APPLIANCE_FIRST_BOOT_INTEGRATION_STATE_AWAITING_WIZARD;
        } else {
            integration->state =
                APPLIANCE_FIRST_BOOT_INTEGRATION_STATE_AWAITING_MANAGEMENT;
        }
        integration->last_status =
            APPLIANCE_FIRST_BOOT_INTEGRATION_MISSING_EVIDENCE;
        return APPLIANCE_FIRST_BOOT_INTEGRATION_MISSING_EVIDENCE;
    }

    integration->source_generation = evidence->source_generation;
    integration->evidence_published = true;
    integration->first_boot_pending = true;
    integration->storage_ready = evidence->storage_ready;
    integration->wizard_complete = evidence->wizard_complete;
    integration->management_active = evidence->management_active;
    integration->network_active = evidence->network_active;
    integration->https_management_ready =
        evidence->https_management_ready;
    integration->state = APPLIANCE_FIRST_BOOT_INTEGRATION_STATE_READY;
    integration->last_status = APPLIANCE_FIRST_BOOT_INTEGRATION_OK;
    integration->failure = APPLIANCE_FIRST_BOOT_INTEGRATION_FAILURE_NONE;
    ++integration->update_count;
    return APPLIANCE_FIRST_BOOT_INTEGRATION_OK;
}

enum appliance_first_boot_integration_status
appliance_first_boot_integration_complete(
    struct appliance_first_boot_integration *integration)
{
    if (integration == NULL) {
        return APPLIANCE_FIRST_BOOT_INTEGRATION_NULL;
    }
    if (!appliance_first_boot_integration_initialized(integration)) {
        return APPLIANCE_FIRST_BOOT_INTEGRATION_UNPUBLISHED;
    }
    if (integration->state !=
        APPLIANCE_FIRST_BOOT_INTEGRATION_STATE_READY) {
        return APPLIANCE_FIRST_BOOT_INTEGRATION_BAD_STATE;
    }
    integration->state = APPLIANCE_FIRST_BOOT_INTEGRATION_STATE_COMPLETE;
    integration->last_status = APPLIANCE_FIRST_BOOT_INTEGRATION_OK;
    return APPLIANCE_FIRST_BOOT_INTEGRATION_OK;
}

enum appliance_first_boot_integration_status
appliance_first_boot_integration_fail_closed(
    struct appliance_first_boot_integration *integration,
    enum appliance_first_boot_integration_failure failure)
{
    if (integration == NULL) {
        return APPLIANCE_FIRST_BOOT_INTEGRATION_NULL;
    }
    if (!appliance_first_boot_integration_initialized(integration)) {
        return APPLIANCE_FIRST_BOOT_INTEGRATION_UNPUBLISHED;
    }
    if (integration->state ==
            APPLIANCE_FIRST_BOOT_INTEGRATION_STATE_COMPLETE ||
        integration->state ==
            APPLIANCE_FIRST_BOOT_INTEGRATION_STATE_FAILED) {
        return APPLIANCE_FIRST_BOOT_INTEGRATION_BAD_STATE;
    }
    return appliance_first_boot_integration_set_failure(
        integration, failure);
}

enum appliance_first_boot_integration_status
appliance_first_boot_integration_reset(
    struct appliance_first_boot_integration *integration)
{
    if (integration == NULL) {
        return APPLIANCE_FIRST_BOOT_INTEGRATION_NULL;
    }
    if (!appliance_first_boot_integration_initialized(integration)) {
        return APPLIANCE_FIRST_BOOT_INTEGRATION_UNPUBLISHED;
    }
    appliance_first_boot_integration_clear_evidence(integration);
    integration->state = APPLIANCE_FIRST_BOOT_INTEGRATION_STATE_PREPARED;
    integration->last_status = APPLIANCE_FIRST_BOOT_INTEGRATION_OK;
    integration->failure = APPLIANCE_FIRST_BOOT_INTEGRATION_FAILURE_NONE;
    ++integration->reset_count;
    return APPLIANCE_FIRST_BOOT_INTEGRATION_OK;
}

enum appliance_first_boot_integration_status
appliance_first_boot_integration_snapshot(
    const struct appliance_first_boot_integration *integration,
    struct appliance_first_boot_integration_snapshot *snapshot_out)
{
    enum appliance_first_boot_integration_health health;

    if (integration == NULL || snapshot_out == NULL) {
        return APPLIANCE_FIRST_BOOT_INTEGRATION_NULL;
    }
    if (!appliance_first_boot_integration_initialized(integration)) {
        return APPLIANCE_FIRST_BOOT_INTEGRATION_UNPUBLISHED;
    }
    if (integration->state ==
            APPLIANCE_FIRST_BOOT_INTEGRATION_STATE_READY ||
        integration->state ==
            APPLIANCE_FIRST_BOOT_INTEGRATION_STATE_COMPLETE) {
        health = APPLIANCE_FIRST_BOOT_INTEGRATION_HEALTH_PASS;
    } else if (integration->state ==
               APPLIANCE_FIRST_BOOT_INTEGRATION_STATE_FAILED) {
        health = APPLIANCE_FIRST_BOOT_INTEGRATION_HEALTH_FAIL;
    } else {
        health = APPLIANCE_FIRST_BOOT_INTEGRATION_HEALTH_UNKNOWN;
    }
    appliance_first_boot_integration_zero_bytes(
        snapshot_out, sizeof(*snapshot_out));
    snapshot_out->published = true;
    snapshot_out->version = APPLIANCE_FIRST_BOOT_INTEGRATION_VERSION;
    snapshot_out->state = integration->state;
    snapshot_out->last_status = integration->last_status;
    snapshot_out->failure = integration->failure;
    snapshot_out->health = health;
    snapshot_out->first_boot_pending = integration->first_boot_pending;
    snapshot_out->storage_ready = integration->storage_ready;
    snapshot_out->wizard_complete = integration->wizard_complete;
    snapshot_out->management_active = integration->management_active;
    snapshot_out->network_active = integration->network_active;
    snapshot_out->https_management_ready =
        integration->https_management_ready;
    snapshot_out->source_generation = integration->source_generation;
    snapshot_out->update_count = integration->update_count;
    snapshot_out->reset_count = integration->reset_count;
    snapshot_out->fail_count = integration->fail_count;
    return APPLIANCE_FIRST_BOOT_INTEGRATION_OK;
}

static bool appliance_first_boot_integration_append(
    char *output,
    size_t *offset,
    const char *text,
    size_t length)
{
    size_t index;

    if (*offset > APPLIANCE_FIRST_BOOT_INTEGRATION_OUTPUT_MAX ||
        length >
            APPLIANCE_FIRST_BOOT_INTEGRATION_OUTPUT_MAX - *offset) {
        return false;
    }
    for (index = 0U; index < length; ++index) {
        output[*offset + index] = text[index];
    }
    *offset += length;
    return true;
}

static bool appliance_first_boot_integration_append_text(
    char *output,
    size_t *offset,
    const char *text)
{
    size_t length = 0U;

    if (text == NULL) {
        return false;
    }
    while (text[length] != '\0') {
        ++length;
    }
    return appliance_first_boot_integration_append(
        output, offset, text, length);
}

enum appliance_first_boot_integration_status
appliance_first_boot_integration_render(
    const struct appliance_first_boot_integration_snapshot *snapshot,
    char *output,
    size_t capacity,
    size_t *output_length)
{
    char rendered[
        APPLIANCE_FIRST_BOOT_INTEGRATION_OUTPUT_MAX + 1U];
    size_t offset = 0U;
    const char *state;
    const char *first_boot;
    const char *storage;
    const char *wizard;
    const char *management;
    const char *network;
    const char *https;
    const char *health;

    if (snapshot == NULL || output == NULL || output_length == NULL) {
        return APPLIANCE_FIRST_BOOT_INTEGRATION_NULL;
    }
    if (!snapshot->published ||
        snapshot->version != APPLIANCE_FIRST_BOOT_INTEGRATION_VERSION ||
        !appliance_first_boot_integration_state_valid(snapshot->state) ||
        !appliance_first_boot_integration_status_valid(
            snapshot->last_status) ||
        !appliance_first_boot_integration_failure_valid(
            snapshot->failure)) {
        return APPLIANCE_FIRST_BOOT_INTEGRATION_BAD_STATE;
    }
    appliance_first_boot_integration_zero_bytes(
        rendered, sizeof(rendered));
    state = appliance_first_boot_integration_state_string(snapshot->state);
    first_boot = snapshot->first_boot_pending ? "PENDING" : "UNKNOWN";
    storage = snapshot->storage_ready ? "READY" : "UNKNOWN";
    wizard = snapshot->wizard_complete ? "COMPLETE" : "UNKNOWN";
    management = snapshot->management_active ? "ACTIVE" : "UNKNOWN";
    network = snapshot->network_active ? "ACTIVE" : "UNKNOWN";
    https = snapshot->https_management_ready ? "READY" : "UNKNOWN";
    switch (snapshot->health) {
    case APPLIANCE_FIRST_BOOT_INTEGRATION_HEALTH_PASS:
        health = "PASS";
        break;
    case APPLIANCE_FIRST_BOOT_INTEGRATION_HEALTH_FAIL:
        health = "FAIL";
        break;
    default:
        health = "UNKNOWN";
        break;
    }
    if (state == NULL ||
        !appliance_first_boot_integration_append_text(
            rendered, &offset,
            "first_boot_integration v=1 state=") ||
        !appliance_first_boot_integration_append_text(
            rendered, &offset, state) ||
        !appliance_first_boot_integration_append_text(
            rendered, &offset, " first_boot=") ||
        !appliance_first_boot_integration_append_text(
            rendered, &offset, first_boot) ||
        !appliance_first_boot_integration_append_text(
            rendered, &offset, " storage=") ||
        !appliance_first_boot_integration_append_text(
            rendered, &offset, storage) ||
        !appliance_first_boot_integration_append_text(
            rendered, &offset, " wizard=") ||
        !appliance_first_boot_integration_append_text(
            rendered, &offset, wizard) ||
        !appliance_first_boot_integration_append_text(
            rendered, &offset, " management=") ||
        !appliance_first_boot_integration_append_text(
            rendered, &offset, management) ||
        !appliance_first_boot_integration_append_text(
            rendered, &offset, " network=") ||
        !appliance_first_boot_integration_append_text(
            rendered, &offset, network) ||
        !appliance_first_boot_integration_append_text(
            rendered, &offset, " https=") ||
        !appliance_first_boot_integration_append_text(
            rendered, &offset, https) ||
        !appliance_first_boot_integration_append_text(
            rendered, &offset, " health=") ||
        !appliance_first_boot_integration_append_text(
            rendered, &offset, health) ||
        !appliance_first_boot_integration_append(
            rendered, &offset, "\n", 1U)) {
        return APPLIANCE_FIRST_BOOT_INTEGRATION_BUFFER_TOO_SMALL;
    }
    if (offset + 1U > capacity) {
        return APPLIANCE_FIRST_BOOT_INTEGRATION_BUFFER_TOO_SMALL;
    }
    appliance_first_boot_integration_zero_bytes(output, capacity);
    {
        size_t index;

        for (index = 0U; index < offset; ++index) {
            output[index] = rendered[index];
        }
    }
    output[offset] = '\0';
    *output_length = offset;
    return APPLIANCE_FIRST_BOOT_INTEGRATION_OK;
}

enum appliance_first_boot_integration_state
appliance_first_boot_integration_state(
    const struct appliance_first_boot_integration *integration)
{
    if (!appliance_first_boot_integration_initialized(integration)) {
        return APPLIANCE_FIRST_BOOT_INTEGRATION_STATE_NONE;
    }
    return integration->state;
}

enum appliance_first_boot_integration_failure
appliance_first_boot_integration_failure(
    const struct appliance_first_boot_integration *integration)
{
    if (!appliance_first_boot_integration_initialized(integration)) {
        return APPLIANCE_FIRST_BOOT_INTEGRATION_FAILURE_NONE;
    }
    return integration->failure;
}

bool appliance_first_boot_integration_published(
    const struct appliance_first_boot_integration *integration)
{
    return appliance_first_boot_integration_initialized(integration) &&
           integration->evidence_published;
}

const char *appliance_first_boot_integration_state_string(
    enum appliance_first_boot_integration_state state)
{
    switch (state) {
    case APPLIANCE_FIRST_BOOT_INTEGRATION_STATE_UNINITIALIZED:
        return "UNINITIALIZED";
    case APPLIANCE_FIRST_BOOT_INTEGRATION_STATE_PREPARED:
        return "PREPARED";
    case APPLIANCE_FIRST_BOOT_INTEGRATION_STATE_AWAITING_WIZARD:
        return "AWAITING_WIZARD";
    case APPLIANCE_FIRST_BOOT_INTEGRATION_STATE_AWAITING_MANAGEMENT:
        return "AWAITING_MANAGEMENT";
    case APPLIANCE_FIRST_BOOT_INTEGRATION_STATE_READY:
        return "READY";
    case APPLIANCE_FIRST_BOOT_INTEGRATION_STATE_COMPLETE:
        return "COMPLETE";
    case APPLIANCE_FIRST_BOOT_INTEGRATION_STATE_FAILED:
        return "FAILED";
    default:
        return NULL;
    }
}

const char *appliance_first_boot_integration_status_string(
    enum appliance_first_boot_integration_status status)
{
    switch (status) {
    case APPLIANCE_FIRST_BOOT_INTEGRATION_OK:
        return "OK";
    case APPLIANCE_FIRST_BOOT_INTEGRATION_NULL:
        return "NULL";
    case APPLIANCE_FIRST_BOOT_INTEGRATION_BAD_STATE:
        return "BAD_STATE";
    case APPLIANCE_FIRST_BOOT_INTEGRATION_BAD_VERSION:
        return "BAD_VERSION";
    case APPLIANCE_FIRST_BOOT_INTEGRATION_ALREADY_PUBLISHED:
        return "ALREADY_PUBLISHED";
    case APPLIANCE_FIRST_BOOT_INTEGRATION_UNPUBLISHED:
        return "UNPUBLISHED";
    case APPLIANCE_FIRST_BOOT_INTEGRATION_INVALID_EVIDENCE:
        return "INVALID_EVIDENCE";
    case APPLIANCE_FIRST_BOOT_INTEGRATION_STALE_EVIDENCE:
        return "STALE_EVIDENCE";
    case APPLIANCE_FIRST_BOOT_INTEGRATION_MISSING_EVIDENCE:
        return "MISSING_EVIDENCE";
    case APPLIANCE_FIRST_BOOT_INTEGRATION_BUFFER_TOO_SMALL:
        return "BUFFER_TOO_SMALL";
    default:
        return NULL;
    }
}

const char *appliance_first_boot_integration_failure_string(
    enum appliance_first_boot_integration_failure failure)
{
    switch (failure) {
    case APPLIANCE_FIRST_BOOT_INTEGRATION_FAILURE_NONE:
        return "NONE";
    case APPLIANCE_FIRST_BOOT_INTEGRATION_FAILURE_EVIDENCE:
        return "EVIDENCE";
    case APPLIANCE_FIRST_BOOT_INTEGRATION_FAILURE_STATE:
        return "STATE";
    case APPLIANCE_FIRST_BOOT_INTEGRATION_FAILURE_WIZARD:
        return "WIZARD";
    case APPLIANCE_FIRST_BOOT_INTEGRATION_FAILURE_MANAGEMENT:
        return "MANAGEMENT";
    case APPLIANCE_FIRST_BOOT_INTEGRATION_FAILURE_NETWORK:
        return "NETWORK";
    case APPLIANCE_FIRST_BOOT_INTEGRATION_FAILURE_HTTPS:
        return "HTTPS";
    case APPLIANCE_FIRST_BOOT_INTEGRATION_FAILURE_COMPLETE:
        return "COMPLETE";
    default:
        return NULL;
    }
}
