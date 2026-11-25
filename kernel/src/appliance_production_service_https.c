#include "appliance_production_service_https.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

static void appliance_production_service_https_zero_bytes(
    void *destination,
    size_t length)
{
    unsigned char *bytes = destination;
    size_t index;

    for (index = 0U; index < length; ++index) {
        bytes[index] = 0U;
    }
}

static bool appliance_production_service_https_state_valid(
    enum appliance_production_service_https_state state)
{
    return state >= APPLIANCE_PRODUCTION_SERVICE_HTTPS_STATE_STOPPED &&
           state <= APPLIANCE_PRODUCTION_SERVICE_HTTPS_STATE_FAILED;
}

static bool appliance_production_service_https_status_valid(
    enum appliance_production_service_https_status status)
{
    return status >= APPLIANCE_PRODUCTION_SERVICE_HTTPS_OK &&
           status <= APPLIANCE_PRODUCTION_SERVICE_HTTPS_BUFFER_TOO_SMALL;
}

static bool appliance_production_service_https_failure_valid(
    enum appliance_production_service_https_failure failure)
{
    return failure >= APPLIANCE_PRODUCTION_SERVICE_HTTPS_FAILURE_NONE &&
           failure <= APPLIANCE_PRODUCTION_SERVICE_HTTPS_FAILURE_INVALID_EVIDENCE;
}

static bool appliance_production_service_https_initialized(
    const struct appliance_production_service_https *composition)
{
    return composition != NULL &&
           composition->initialized &&
           composition->version ==
               APPLIANCE_PRODUCTION_SERVICE_HTTPS_VERSION &&
           appliance_production_service_https_state_valid(
               composition->state) &&
           appliance_production_service_https_status_valid(
               composition->last_status) &&
           appliance_production_service_https_failure_valid(
               composition->failure);
}

static void appliance_production_service_https_clear_evidence(
    struct appliance_production_service_https *composition)
{
    composition->readiness_present = false;
    composition->evidence_published = false;
    composition->network_active = false;
    composition->entropy_ready = false;
    composition->certificate_ready = false;
    composition->https_enabled = false;
    composition->source_generation = 0U;
    composition->configured = false;
}

static enum appliance_production_service_https_status
appliance_production_service_https_set_failure(
    struct appliance_production_service_https *composition,
    enum appliance_production_service_https_failure failure)
{
    if (composition == NULL) {
        return APPLIANCE_PRODUCTION_SERVICE_HTTPS_NULL;
    }
    if (!appliance_production_service_https_failure_valid(failure) ||
        failure == APPLIANCE_PRODUCTION_SERVICE_HTTPS_FAILURE_NONE) {
        return APPLIANCE_PRODUCTION_SERVICE_HTTPS_BAD_STATE;
    }
    composition->state = APPLIANCE_PRODUCTION_SERVICE_HTTPS_STATE_FAILED;
    composition->failure = failure;
    composition->last_status = APPLIANCE_PRODUCTION_SERVICE_HTTPS_OK;
    appliance_production_service_https_clear_evidence(composition);
    return APPLIANCE_PRODUCTION_SERVICE_HTTPS_OK;
}

enum appliance_production_service_https_status
appliance_production_service_https_init(
    struct appliance_production_service_https *composition)
{
    if (composition == NULL) {
        return APPLIANCE_PRODUCTION_SERVICE_HTTPS_NULL;
    }
    if (composition->initialized) {
        return APPLIANCE_PRODUCTION_SERVICE_HTTPS_ALREADY_PUBLISHED;
    }
    appliance_production_service_https_zero_bytes(
        composition, sizeof(*composition));
    composition->initialized = true;
    composition->version = APPLIANCE_PRODUCTION_SERVICE_HTTPS_VERSION;
    composition->state = APPLIANCE_PRODUCTION_SERVICE_HTTPS_STATE_STOPPED;
    composition->last_status = APPLIANCE_PRODUCTION_SERVICE_HTTPS_OK;
    composition->failure = APPLIANCE_PRODUCTION_SERVICE_HTTPS_FAILURE_NONE;
    return APPLIANCE_PRODUCTION_SERVICE_HTTPS_OK;
}

enum appliance_production_service_https_status
appliance_production_service_https_configure(
    struct appliance_production_service_https *composition,
    const struct appliance_production_service_https_evidence *evidence)
{
    if (composition == NULL || evidence == NULL) {
        return APPLIANCE_PRODUCTION_SERVICE_HTTPS_NULL;
    }
    if (!appliance_production_service_https_initialized(composition)) {
        return APPLIANCE_PRODUCTION_SERVICE_HTTPS_UNPUBLISHED;
    }
    if (composition->state !=
            APPLIANCE_PRODUCTION_SERVICE_HTTPS_STATE_STOPPED ||
        composition->configured) {
        return APPLIANCE_PRODUCTION_SERVICE_HTTPS_BAD_STATE;
    }
    if (evidence->version !=
            APPLIANCE_PRODUCTION_SERVICE_HTTPS_EVIDENCE_VERSION ||
        !evidence->published ||
        evidence->source_generation == 0U) {
        return APPLIANCE_PRODUCTION_SERVICE_HTTPS_INVALID_EVIDENCE;
    }
    if (!evidence->loaded_state_committed ||
        !evidence->manager_configured ||
        !evidence->manager_stopped) {
        return APPLIANCE_PRODUCTION_SERVICE_HTTPS_INVALID_EVIDENCE;
    }
    if (evidence->https_enabled && !evidence->https_stopped) {
        return APPLIANCE_PRODUCTION_SERVICE_HTTPS_INVALID_EVIDENCE;
    }
    composition->configured = true;
    composition->https_enabled = evidence->https_enabled;
    composition->source_generation = evidence->source_generation;
    composition->state =
        APPLIANCE_PRODUCTION_SERVICE_HTTPS_STATE_CONFIGURED;
    composition->last_status = APPLIANCE_PRODUCTION_SERVICE_HTTPS_OK;
    composition->failure = APPLIANCE_PRODUCTION_SERVICE_HTTPS_FAILURE_NONE;
    return APPLIANCE_PRODUCTION_SERVICE_HTTPS_OK;
}

enum appliance_production_service_https_status
appliance_production_service_https_readiness(
    struct appliance_production_service_https *composition,
    const struct appliance_production_service_https_readiness *readiness)
{
    if (composition == NULL || readiness == NULL) {
        return APPLIANCE_PRODUCTION_SERVICE_HTTPS_NULL;
    }
    if (!appliance_production_service_https_initialized(composition)) {
        return APPLIANCE_PRODUCTION_SERVICE_HTTPS_UNPUBLISHED;
    }
    if (composition->state !=
            APPLIANCE_PRODUCTION_SERVICE_HTTPS_STATE_CONFIGURED &&
        composition->state !=
            APPLIANCE_PRODUCTION_SERVICE_HTTPS_STATE_AWAITING_READINESS) {
        return APPLIANCE_PRODUCTION_SERVICE_HTTPS_BAD_STATE;
    }
    if (readiness->version !=
            APPLIANCE_PRODUCTION_SERVICE_HTTPS_READINESS_VERSION) {
        return APPLIANCE_PRODUCTION_SERVICE_HTTPS_BAD_VERSION;
    }
    if (readiness->source_generation !=
        composition->source_generation) {
        return APPLIANCE_PRODUCTION_SERVICE_HTTPS_STALE_EVIDENCE;
    }
    if (composition->https_enabled &&
        (!readiness->network_active ||
         !readiness->entropy_ready ||
         !readiness->certificate_ready)) {
        composition->state =
            APPLIANCE_PRODUCTION_SERVICE_HTTPS_STATE_AWAITING_READINESS;
        composition->readiness_present = false;
        composition->last_status =
            APPLIANCE_PRODUCTION_SERVICE_HTTPS_MISSING_READINESS;
        return APPLIANCE_PRODUCTION_SERVICE_HTTPS_MISSING_READINESS;
    }
    composition->network_active = readiness->network_active;
    composition->entropy_ready = readiness->entropy_ready;
    composition->certificate_ready = readiness->certificate_ready;
    composition->readiness_present = true;
    composition->state =
        APPLIANCE_PRODUCTION_SERVICE_HTTPS_STATE_AWAITING_READINESS;
    composition->last_status = APPLIANCE_PRODUCTION_SERVICE_HTTPS_OK;
    return APPLIANCE_PRODUCTION_SERVICE_HTTPS_OK;
}

enum appliance_production_service_https_status
appliance_production_service_https_start(
    struct appliance_production_service_https *composition)
{
    if (composition == NULL) {
        return APPLIANCE_PRODUCTION_SERVICE_HTTPS_NULL;
    }
    if (!appliance_production_service_https_initialized(composition)) {
        return APPLIANCE_PRODUCTION_SERVICE_HTTPS_UNPUBLISHED;
    }
    if (composition->state !=
            APPLIANCE_PRODUCTION_SERVICE_HTTPS_STATE_AWAITING_READINESS ||
        !composition->readiness_present) {
        return APPLIANCE_PRODUCTION_SERVICE_HTTPS_MISSING_READINESS;
    }
    composition->state = APPLIANCE_PRODUCTION_SERVICE_HTTPS_STATE_STARTING;
    composition->last_status = APPLIANCE_PRODUCTION_SERVICE_HTTPS_OK;
    ++composition->start_count;
    return APPLIANCE_PRODUCTION_SERVICE_HTTPS_OK;
}

enum appliance_production_service_https_status
appliance_production_service_https_running(
    struct appliance_production_service_https *composition)
{
    if (composition == NULL) {
        return APPLIANCE_PRODUCTION_SERVICE_HTTPS_NULL;
    }
    if (!appliance_production_service_https_initialized(composition)) {
        return APPLIANCE_PRODUCTION_SERVICE_HTTPS_UNPUBLISHED;
    }
    if (composition->state !=
        APPLIANCE_PRODUCTION_SERVICE_HTTPS_STATE_STARTING) {
        return APPLIANCE_PRODUCTION_SERVICE_HTTPS_BAD_STATE;
    }
    composition->state = APPLIANCE_PRODUCTION_SERVICE_HTTPS_STATE_RUNNING;
    composition->evidence_published = true;
    composition->last_status = APPLIANCE_PRODUCTION_SERVICE_HTTPS_OK;
    composition->failure = APPLIANCE_PRODUCTION_SERVICE_HTTPS_FAILURE_NONE;
    return APPLIANCE_PRODUCTION_SERVICE_HTTPS_OK;
}

enum appliance_production_service_https_status
appliance_production_service_https_start_failed(
    struct appliance_production_service_https *composition,
    enum appliance_production_service_https_failure failure)
{
    if (composition == NULL) {
        return APPLIANCE_PRODUCTION_SERVICE_HTTPS_NULL;
    }
    if (!appliance_production_service_https_initialized(composition)) {
        return APPLIANCE_PRODUCTION_SERVICE_HTTPS_UNPUBLISHED;
    }
    if (composition->state !=
        APPLIANCE_PRODUCTION_SERVICE_HTTPS_STATE_STARTING) {
        return APPLIANCE_PRODUCTION_SERVICE_HTTPS_BAD_STATE;
    }
    return appliance_production_service_https_set_failure(
        composition, failure);
}

enum appliance_production_service_https_status
appliance_production_service_https_stop(
    struct appliance_production_service_https *composition)
{
    if (composition == NULL) {
        return APPLIANCE_PRODUCTION_SERVICE_HTTPS_NULL;
    }
    if (!appliance_production_service_https_initialized(composition)) {
        return APPLIANCE_PRODUCTION_SERVICE_HTTPS_UNPUBLISHED;
    }
    if (composition->state ==
        APPLIANCE_PRODUCTION_SERVICE_HTTPS_STATE_STOPPING) {
        composition->last_status =
            APPLIANCE_PRODUCTION_SERVICE_HTTPS_STOP_PENDING;
        return APPLIANCE_PRODUCTION_SERVICE_HTTPS_STOP_PENDING;
    }
    if (composition->state !=
            APPLIANCE_PRODUCTION_SERVICE_HTTPS_STATE_RUNNING &&
        composition->state !=
            APPLIANCE_PRODUCTION_SERVICE_HTTPS_STATE_STARTING) {
        return APPLIANCE_PRODUCTION_SERVICE_HTTPS_BAD_STATE;
    }
    composition->state = APPLIANCE_PRODUCTION_SERVICE_HTTPS_STATE_STOPPING;
    composition->last_status = APPLIANCE_PRODUCTION_SERVICE_HTTPS_OK;
    ++composition->stop_count;
    return APPLIANCE_PRODUCTION_SERVICE_HTTPS_OK;
}

enum appliance_production_service_https_status
appliance_production_service_https_stopped(
    struct appliance_production_service_https *composition)
{
    if (composition == NULL) {
        return APPLIANCE_PRODUCTION_SERVICE_HTTPS_NULL;
    }
    if (!appliance_production_service_https_initialized(composition)) {
        return APPLIANCE_PRODUCTION_SERVICE_HTTPS_UNPUBLISHED;
    }
    if (composition->state !=
        APPLIANCE_PRODUCTION_SERVICE_HTTPS_STATE_STOPPING) {
        return APPLIANCE_PRODUCTION_SERVICE_HTTPS_BAD_STATE;
    }
    composition->state = APPLIANCE_PRODUCTION_SERVICE_HTTPS_STATE_STOPPED;
    composition->evidence_published = false;
    composition->readiness_present = false;
    composition->last_status = APPLIANCE_PRODUCTION_SERVICE_HTTPS_OK;
    return APPLIANCE_PRODUCTION_SERVICE_HTTPS_OK;
}

enum appliance_production_service_https_status
appliance_production_service_https_stop_failed(
    struct appliance_production_service_https *composition)
{
    if (composition == NULL) {
        return APPLIANCE_PRODUCTION_SERVICE_HTTPS_NULL;
    }
    if (!appliance_production_service_https_initialized(composition)) {
        return APPLIANCE_PRODUCTION_SERVICE_HTTPS_UNPUBLISHED;
    }
    if (composition->state !=
        APPLIANCE_PRODUCTION_SERVICE_HTTPS_STATE_STOPPING) {
        return APPLIANCE_PRODUCTION_SERVICE_HTTPS_BAD_STATE;
    }
    return appliance_production_service_https_set_failure(
        composition, APPLIANCE_PRODUCTION_SERVICE_HTTPS_FAILURE_STOP);
}

enum appliance_production_service_https_status
appliance_production_service_https_fail_closed(
    struct appliance_production_service_https *composition,
    enum appliance_production_service_https_failure failure)
{
    if (composition == NULL) {
        return APPLIANCE_PRODUCTION_SERVICE_HTTPS_NULL;
    }
    if (!appliance_production_service_https_initialized(composition)) {
        return APPLIANCE_PRODUCTION_SERVICE_HTTPS_UNPUBLISHED;
    }
    if (composition->state !=
            APPLIANCE_PRODUCTION_SERVICE_HTTPS_STATE_STARTING &&
        composition->state !=
            APPLIANCE_PRODUCTION_SERVICE_HTTPS_STATE_RUNNING &&
        composition->state !=
            APPLIANCE_PRODUCTION_SERVICE_HTTPS_STATE_STOPPING) {
        return APPLIANCE_PRODUCTION_SERVICE_HTTPS_BAD_STATE;
    }
    return appliance_production_service_https_set_failure(
        composition, failure);
}

enum appliance_production_service_https_status
appliance_production_service_https_reset(
    struct appliance_production_service_https *composition)
{
    if (composition == NULL) {
        return APPLIANCE_PRODUCTION_SERVICE_HTTPS_NULL;
    }
    if (!appliance_production_service_https_initialized(composition)) {
        return APPLIANCE_PRODUCTION_SERVICE_HTTPS_UNPUBLISHED;
    }
    if (composition->state !=
            APPLIANCE_PRODUCTION_SERVICE_HTTPS_STATE_STOPPED &&
        composition->state !=
            APPLIANCE_PRODUCTION_SERVICE_HTTPS_STATE_FAILED) {
        return APPLIANCE_PRODUCTION_SERVICE_HTTPS_BAD_STATE;
    }
    appliance_production_service_https_clear_evidence(composition);
    composition->state = APPLIANCE_PRODUCTION_SERVICE_HTTPS_STATE_STOPPED;
    composition->last_status = APPLIANCE_PRODUCTION_SERVICE_HTTPS_OK;
    composition->failure = APPLIANCE_PRODUCTION_SERVICE_HTTPS_FAILURE_NONE;
    ++composition->reset_count;
    return APPLIANCE_PRODUCTION_SERVICE_HTTPS_OK;
}

enum appliance_production_service_https_status
appliance_production_service_https_snapshot(
    const struct appliance_production_service_https *composition,
    struct appliance_production_service_https_snapshot *snapshot_out)
{
    if (composition == NULL || snapshot_out == NULL) {
        return APPLIANCE_PRODUCTION_SERVICE_HTTPS_NULL;
    }
    if (!appliance_production_service_https_initialized(composition)) {
        return APPLIANCE_PRODUCTION_SERVICE_HTTPS_UNPUBLISHED;
    }
    appliance_production_service_https_zero_bytes(
        snapshot_out, sizeof(*snapshot_out));
    snapshot_out->published = true;
    snapshot_out->version = APPLIANCE_PRODUCTION_SERVICE_HTTPS_VERSION;
    snapshot_out->state = composition->state;
    snapshot_out->last_status = composition->last_status;
    snapshot_out->failure = composition->failure;
    snapshot_out->https_enabled = composition->https_enabled;
    snapshot_out->evidence_published =
        composition->evidence_published;
    snapshot_out->source_generation = composition->source_generation;
    snapshot_out->start_count = composition->start_count;
    snapshot_out->stop_count = composition->stop_count;
    snapshot_out->reset_count = composition->reset_count;
    return APPLIANCE_PRODUCTION_SERVICE_HTTPS_OK;
}

static bool appliance_production_service_https_append(
    char *output,
    size_t *offset,
    const char *text,
    size_t length)
{
    size_t index;

    if (*offset > APPLIANCE_PRODUCTION_SERVICE_HTTPS_OUTPUT_MAX ||
        length > APPLIANCE_PRODUCTION_SERVICE_HTTPS_OUTPUT_MAX - *offset) {
        return false;
    }
    for (index = 0U; index < length; ++index) {
        output[*offset + index] = text[index];
    }
    *offset += length;
    return true;
}

static bool appliance_production_service_https_append_text(
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
    return appliance_production_service_https_append(
        output, offset, text, length);
}

enum appliance_production_service_https_status
appliance_production_service_https_render(
    const struct appliance_production_service_https_snapshot *snapshot,
    char *output,
    size_t capacity,
    size_t *output_length)
{
    char rendered[APPLIANCE_PRODUCTION_SERVICE_HTTPS_OUTPUT_MAX + 1U];
    size_t offset = 0U;
    const char *state;
    const char *https;
    const char *network;
    const char *entropy;
    const char *certificate;
    const char *health;

    if (snapshot == NULL || output == NULL || output_length == NULL) {
        return APPLIANCE_PRODUCTION_SERVICE_HTTPS_NULL;
    }
    if (!snapshot->published ||
        snapshot->version != APPLIANCE_PRODUCTION_SERVICE_HTTPS_VERSION ||
        !appliance_production_service_https_state_valid(snapshot->state) ||
        !appliance_production_service_https_status_valid(
            snapshot->last_status) ||
        !appliance_production_service_https_failure_valid(
            snapshot->failure)) {
        return APPLIANCE_PRODUCTION_SERVICE_HTTPS_BAD_STATE;
    }
    appliance_production_service_https_zero_bytes(
        rendered, sizeof(rendered));
    state = appliance_production_service_https_state_string(
        snapshot->state);
    https = snapshot->https_enabled ? "ENABLED" : "DISABLED";
    network = snapshot->evidence_published ? "ACTIVE" : "STOPPED";
    entropy = snapshot->evidence_published ? "READY" : "UNKNOWN";
    certificate = snapshot->evidence_published ? "READY" : "UNKNOWN";
    health = snapshot->state ==
            APPLIANCE_PRODUCTION_SERVICE_HTTPS_STATE_RUNNING
                ? "PASS"
                : "UNKNOWN";
    if (state == NULL ||
        !appliance_production_service_https_append_text(
            rendered, &offset,
            "production_service_https v=1 state=") ||
        !appliance_production_service_https_append_text(
            rendered, &offset, state) ||
        !appliance_production_service_https_append_text(
            rendered, &offset, " https=") ||
        !appliance_production_service_https_append_text(
            rendered, &offset, https) ||
        !appliance_production_service_https_append_text(
            rendered, &offset, " network=") ||
        !appliance_production_service_https_append_text(
            rendered, &offset, network) ||
        !appliance_production_service_https_append_text(
            rendered, &offset, " entropy=") ||
        !appliance_production_service_https_append_text(
            rendered, &offset, entropy) ||
        !appliance_production_service_https_append_text(
            rendered, &offset, " certificate=") ||
        !appliance_production_service_https_append_text(
            rendered, &offset, certificate) ||
        !appliance_production_service_https_append_text(
            rendered, &offset, " health=") ||
        !appliance_production_service_https_append_text(
            rendered, &offset, health) ||
        !appliance_production_service_https_append(
            rendered, &offset, "\n", 1U)) {
        return APPLIANCE_PRODUCTION_SERVICE_HTTPS_BUFFER_TOO_SMALL;
    }
    if (offset + 1U > capacity) {
        return APPLIANCE_PRODUCTION_SERVICE_HTTPS_BUFFER_TOO_SMALL;
    }
    appliance_production_service_https_zero_bytes(
        output, capacity);
    {
        size_t index;

        for (index = 0U; index < offset; ++index) {
            output[index] = rendered[index];
        }
    }
    output[offset] = '\0';
    *output_length = offset;
    return APPLIANCE_PRODUCTION_SERVICE_HTTPS_OK;
}

enum appliance_production_service_https_state
appliance_production_service_https_state(
    const struct appliance_production_service_https *composition)
{
    if (!appliance_production_service_https_initialized(composition)) {
        return APPLIANCE_PRODUCTION_SERVICE_HTTPS_STATE_NONE;
    }
    return composition->state;
}

enum appliance_production_service_https_failure
appliance_production_service_https_failure(
    const struct appliance_production_service_https *composition)
{
    if (!appliance_production_service_https_initialized(composition)) {
        return APPLIANCE_PRODUCTION_SERVICE_HTTPS_FAILURE_NONE;
    }
    return composition->failure;
}

bool appliance_production_service_https_published(
    const struct appliance_production_service_https *composition)
{
    return appliance_production_service_https_initialized(composition) &&
           composition->evidence_published;
}

const char *appliance_production_service_https_state_string(
    enum appliance_production_service_https_state state)
{
    switch (state) {
    case APPLIANCE_PRODUCTION_SERVICE_HTTPS_STATE_STOPPED:
        return "STOPPED";
    case APPLIANCE_PRODUCTION_SERVICE_HTTPS_STATE_CONFIGURED:
        return "CONFIGURED";
    case APPLIANCE_PRODUCTION_SERVICE_HTTPS_STATE_AWAITING_READINESS:
        return "AWAITING_READINESS";
    case APPLIANCE_PRODUCTION_SERVICE_HTTPS_STATE_STARTING:
        return "STARTING";
    case APPLIANCE_PRODUCTION_SERVICE_HTTPS_STATE_RUNNING:
        return "RUNNING";
    case APPLIANCE_PRODUCTION_SERVICE_HTTPS_STATE_STOPPING:
        return "STOPPING";
    case APPLIANCE_PRODUCTION_SERVICE_HTTPS_STATE_FAILED:
        return "FAILED";
    default:
        return NULL;
    }
}

const char *appliance_production_service_https_status_string(
    enum appliance_production_service_https_status status)
{
    switch (status) {
    case APPLIANCE_PRODUCTION_SERVICE_HTTPS_OK:
        return "OK";
    case APPLIANCE_PRODUCTION_SERVICE_HTTPS_NULL:
        return "NULL";
    case APPLIANCE_PRODUCTION_SERVICE_HTTPS_BAD_STATE:
        return "BAD_STATE";
    case APPLIANCE_PRODUCTION_SERVICE_HTTPS_BAD_VERSION:
        return "BAD_VERSION";
    case APPLIANCE_PRODUCTION_SERVICE_HTTPS_ALREADY_PUBLISHED:
        return "ALREADY_PUBLISHED";
    case APPLIANCE_PRODUCTION_SERVICE_HTTPS_UNPUBLISHED:
        return "UNPUBLISHED";
    case APPLIANCE_PRODUCTION_SERVICE_HTTPS_INVALID_EVIDENCE:
        return "INVALID_EVIDENCE";
    case APPLIANCE_PRODUCTION_SERVICE_HTTPS_STALE_EVIDENCE:
        return "STALE_EVIDENCE";
    case APPLIANCE_PRODUCTION_SERVICE_HTTPS_MISSING_READINESS:
        return "MISSING_READINESS";
    case APPLIANCE_PRODUCTION_SERVICE_HTTPS_START_FAILED:
        return "START_FAILED";
    case APPLIANCE_PRODUCTION_SERVICE_HTTPS_STOP_PENDING:
        return "STOP_PENDING";
    case APPLIANCE_PRODUCTION_SERVICE_HTTPS_STOP_FAILED:
        return "STOP_FAILED";
    case APPLIANCE_PRODUCTION_SERVICE_HTTPS_BUFFER_TOO_SMALL:
        return "BUFFER_TOO_SMALL";
    default:
        return NULL;
    }
}

const char *appliance_production_service_https_failure_string(
    enum appliance_production_service_https_failure failure)
{
    switch (failure) {
    case APPLIANCE_PRODUCTION_SERVICE_HTTPS_FAILURE_NONE:
        return "NONE";
    case APPLIANCE_PRODUCTION_SERVICE_HTTPS_FAILURE_START:
        return "START";
    case APPLIANCE_PRODUCTION_SERVICE_HTTPS_FAILURE_READY:
        return "READY";
    case APPLIANCE_PRODUCTION_SERVICE_HTTPS_FAILURE_STOP:
        return "STOP";
    case APPLIANCE_PRODUCTION_SERVICE_HTTPS_FAILURE_SERVICE:
        return "SERVICE";
    case APPLIANCE_PRODUCTION_SERVICE_HTTPS_FAILURE_NETWORK:
        return "NETWORK";
    case APPLIANCE_PRODUCTION_SERVICE_HTTPS_FAILURE_RETRY_EXHAUSTED:
        return "RETRY_EXHAUSTED";
    case APPLIANCE_PRODUCTION_SERVICE_HTTPS_FAILURE_INVALID_EVIDENCE:
        return "INVALID_EVIDENCE";
    default:
        return NULL;
    }
}
