#include "appliance_production_service_activation.h"

#include "appliance_production_service_https.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

static void appliance_production_service_activation_zero_bytes(
    void *destination,
    size_t length)
{
    unsigned char *bytes = destination;
    size_t index;

    for (index = 0U; index < length; ++index) {
        bytes[index] = 0U;
    }
}

static bool appliance_production_service_activation_state_valid(
    enum appliance_production_service_activation_state state)
{
    return state >= APPLIANCE_PRODUCTION_SERVICE_ACTIVATION_STATE_NONE &&
           state <= APPLIANCE_PRODUCTION_SERVICE_ACTIVATION_STATE_FAILED;
}

static bool appliance_production_service_activation_status_valid(
    enum appliance_production_service_activation_status status)
{
    return status >= APPLIANCE_PRODUCTION_SERVICE_ACTIVATION_OK &&
           status <=
               APPLIANCE_PRODUCTION_SERVICE_ACTIVATION_BUFFER_TOO_SMALL;
}

static bool appliance_production_service_activation_published(
    const struct appliance_production_service_activation *activation)
{
    return activation != NULL &&
           activation->initialized &&
           activation->version ==
               APPLIANCE_PRODUCTION_SERVICE_ACTIVATION_VERSION &&
           appliance_production_service_activation_state_valid(
               activation->state) &&
           appliance_production_service_activation_status_valid(
               activation->last_status) &&
           activation->composition != NULL;
}

static enum appliance_production_service_activation_status
appliance_production_service_activation_fail(
    struct appliance_production_service_activation *activation,
    enum appliance_production_service_activation_status status)
{
    activation->state = APPLIANCE_PRODUCTION_SERVICE_ACTIVATION_STATE_FAILED;
    activation->last_status = status;
    ++activation->fail_count;
    return status;
}

enum appliance_production_service_activation_status
appliance_production_service_activation_init(
    struct appliance_production_service_activation *activation,
    struct appliance_production_service_https *composition)
{
    struct appliance_production_service_activation candidate;

    if (activation == NULL || composition == NULL) {
        return APPLIANCE_PRODUCTION_SERVICE_ACTIVATION_NULL;
    }
    if (activation->initialized) {
        return APPLIANCE_PRODUCTION_SERVICE_ACTIVATION_BAD_STATE;
    }
    if (!composition->initialized) {
        return APPLIANCE_PRODUCTION_SERVICE_ACTIVATION_UNPUBLISHED;
    }

    appliance_production_service_activation_zero_bytes(
        &candidate, sizeof(candidate));
    candidate.initialized = true;
    candidate.configured = false;
    candidate.version = APPLIANCE_PRODUCTION_SERVICE_ACTIVATION_VERSION;
    candidate.state = APPLIANCE_PRODUCTION_SERVICE_ACTIVATION_STATE_STOPPED;
    candidate.last_status = APPLIANCE_PRODUCTION_SERVICE_ACTIVATION_OK;
    candidate.composition = composition;
    candidate.secrets_logged = false;
    *activation = candidate;
    return APPLIANCE_PRODUCTION_SERVICE_ACTIVATION_OK;
}

enum appliance_production_service_activation_status
appliance_production_service_activation_configure(
    struct appliance_production_service_activation *activation,
    const struct appliance_production_service_https_evidence *evidence)
{
    enum appliance_production_service_https_status status;

    if (activation == NULL || evidence == NULL) {
        return APPLIANCE_PRODUCTION_SERVICE_ACTIVATION_NULL;
    }
    if (!appliance_production_service_activation_published(activation)) {
        return APPLIANCE_PRODUCTION_SERVICE_ACTIVATION_UNPUBLISHED;
    }
    if (activation->state !=
            APPLIANCE_PRODUCTION_SERVICE_ACTIVATION_STATE_STOPPED &&
        activation->state !=
            APPLIANCE_PRODUCTION_SERVICE_ACTIVATION_STATE_AWAITING_EVIDENCE) {
        return APPLIANCE_PRODUCTION_SERVICE_ACTIVATION_BAD_STATE;
    }
    status = appliance_production_service_https_configure(
        activation->composition, evidence);
    if (status != APPLIANCE_PRODUCTION_SERVICE_HTTPS_OK) {
        return appliance_production_service_activation_fail(
            activation,
            APPLIANCE_PRODUCTION_SERVICE_ACTIVATION_INVALID_EVIDENCE);
    }

    activation->configured = true;
    activation->evidence = *evidence;
    activation->https_enabled = evidence->https_enabled;
    activation->source_generation = evidence->source_generation;
    activation->state =
        APPLIANCE_PRODUCTION_SERVICE_ACTIVATION_STATE_AWAITING_NETWORK;
    activation->last_status = APPLIANCE_PRODUCTION_SERVICE_ACTIVATION_OK;
    return APPLIANCE_PRODUCTION_SERVICE_ACTIVATION_OK;
}

static enum appliance_production_service_activation_status
appliance_production_service_activation_start(
    struct appliance_production_service_activation *activation)
{
    struct appliance_production_service_https_readiness readiness;
    enum appliance_production_service_https_status composition_status;

    appliance_production_service_activation_zero_bytes(
        &readiness, sizeof(readiness));
    readiness.version =
        APPLIANCE_PRODUCTION_SERVICE_HTTPS_READINESS_VERSION;
    readiness.source_generation = activation->source_generation;
    readiness.network_active = activation->network_active;
    readiness.entropy_ready = activation->entropy_ready;
    readiness.certificate_ready = activation->certificate_ready;

    composition_status = appliance_production_service_https_readiness(
        activation->composition, &readiness);
    if (composition_status != APPLIANCE_PRODUCTION_SERVICE_HTTPS_OK) {
        return appliance_production_service_activation_fail(
            activation,
            APPLIANCE_PRODUCTION_SERVICE_ACTIVATION_START_FAILED);
    }
    composition_status = appliance_production_service_https_start(
        activation->composition);
    if (composition_status != APPLIANCE_PRODUCTION_SERVICE_HTTPS_OK) {
        return appliance_production_service_activation_fail(
            activation,
            APPLIANCE_PRODUCTION_SERVICE_ACTIVATION_START_FAILED);
    }
    composition_status = appliance_production_service_https_running(
        activation->composition);
    if (composition_status != APPLIANCE_PRODUCTION_SERVICE_HTTPS_OK) {
        return appliance_production_service_activation_fail(
            activation,
            APPLIANCE_PRODUCTION_SERVICE_ACTIVATION_START_FAILED);
    }

    activation->state =
        APPLIANCE_PRODUCTION_SERVICE_ACTIVATION_STATE_RUNNING;
    activation->last_status = APPLIANCE_PRODUCTION_SERVICE_ACTIVATION_OK;
    ++activation->start_count;
    return APPLIANCE_PRODUCTION_SERVICE_ACTIVATION_OK;
}

static enum appliance_production_service_activation_status
appliance_production_service_activation_evaluate_internal(
    struct appliance_production_service_activation *activation,
    bool network_active,
    bool entropy_ready,
    bool certificate_ready,
    bool clock_synchronized,
    uint32_t now_ms)
{
    (void)now_ms;

    if (activation == NULL) {
        return APPLIANCE_PRODUCTION_SERVICE_ACTIVATION_NULL;
    }
    if (!appliance_production_service_activation_published(activation)) {
        return APPLIANCE_PRODUCTION_SERVICE_ACTIVATION_UNPUBLISHED;
    }
    if (!activation->configured) {
        activation->state =
            APPLIANCE_PRODUCTION_SERVICE_ACTIVATION_STATE_AWAITING_EVIDENCE;
        activation->last_status =
            APPLIANCE_PRODUCTION_SERVICE_ACTIVATION_INVALID_EVIDENCE;
        return APPLIANCE_PRODUCTION_SERVICE_ACTIVATION_INVALID_EVIDENCE;
    }
    if (activation->state ==
        APPLIANCE_PRODUCTION_SERVICE_ACTIVATION_STATE_RUNNING) {
        return appliance_production_service_activation_poll(
            activation, network_active, now_ms);
    }
    if (activation->state !=
            APPLIANCE_PRODUCTION_SERVICE_ACTIVATION_STATE_AWAITING_NETWORK &&
        activation->state !=
            APPLIANCE_PRODUCTION_SERVICE_ACTIVATION_STATE_AWAITING_READINESS &&
        activation->state !=
            APPLIANCE_PRODUCTION_SERVICE_ACTIVATION_STATE_DEGRADED) {
        return APPLIANCE_PRODUCTION_SERVICE_ACTIVATION_BAD_STATE;
    }

    activation->network_active = network_active;
    activation->entropy_ready = entropy_ready;
    activation->certificate_ready = certificate_ready;
    activation->clock_synchronized = clock_synchronized;

    if (!network_active) {
        activation->state =
            APPLIANCE_PRODUCTION_SERVICE_ACTIVATION_STATE_AWAITING_NETWORK;
        activation->last_status =
            APPLIANCE_PRODUCTION_SERVICE_ACTIVATION_MISSING_NETWORK;
        return APPLIANCE_PRODUCTION_SERVICE_ACTIVATION_MISSING_NETWORK;
    }
    if (!entropy_ready || !certificate_ready || !clock_synchronized) {
        activation->state =
            APPLIANCE_PRODUCTION_SERVICE_ACTIVATION_STATE_AWAITING_READINESS;
        activation->last_status =
            APPLIANCE_PRODUCTION_SERVICE_ACTIVATION_MISSING_READINESS;
        return APPLIANCE_PRODUCTION_SERVICE_ACTIVATION_MISSING_READINESS;
    }
    return appliance_production_service_activation_start(activation);
}

enum appliance_production_service_activation_status
appliance_production_service_activation_evaluate(
    struct appliance_production_service_activation *activation,
    bool network_active,
    bool entropy_ready,
    bool certificate_ready,
    uint32_t now_ms)
{
    return appliance_production_service_activation_evaluate_internal(
        activation, network_active, entropy_ready, certificate_ready, true,
        now_ms);
}

enum appliance_production_service_activation_status
appliance_production_service_activation_evaluate_clock(
    struct appliance_production_service_activation *activation,
    bool network_active,
    bool entropy_ready,
    bool certificate_ready,
    bool clock_synchronized,
    uint32_t now_ms)
{
    return appliance_production_service_activation_evaluate_internal(
        activation, network_active, entropy_ready, certificate_ready,
        clock_synchronized, now_ms);
}

enum appliance_production_service_activation_status
appliance_production_service_activation_poll(
    struct appliance_production_service_activation *activation,
    bool network_active,
    uint32_t now_ms)
{
    enum appliance_production_service_https_status composition_status;

    (void)now_ms;
    if (activation == NULL) {
        return APPLIANCE_PRODUCTION_SERVICE_ACTIVATION_NULL;
    }
    if (!appliance_production_service_activation_published(activation)) {
        return APPLIANCE_PRODUCTION_SERVICE_ACTIVATION_UNPUBLISHED;
    }
    if (network_active) {
        activation->network_active = true;
        return APPLIANCE_PRODUCTION_SERVICE_ACTIVATION_OK;
    }
    if (activation->state !=
        APPLIANCE_PRODUCTION_SERVICE_ACTIVATION_STATE_RUNNING) {
        activation->network_active = false;
        return APPLIANCE_PRODUCTION_SERVICE_ACTIVATION_OK;
    }

    activation->network_active = false;
    composition_status = appliance_production_service_https_fail_closed(
        activation->composition,
        APPLIANCE_PRODUCTION_SERVICE_HTTPS_FAILURE_NETWORK);
    if (composition_status != APPLIANCE_PRODUCTION_SERVICE_HTTPS_OK) {
        return appliance_production_service_activation_fail(
            activation,
            APPLIANCE_PRODUCTION_SERVICE_ACTIVATION_STOP_FAILED);
    }

    activation->state =
        APPLIANCE_PRODUCTION_SERVICE_ACTIVATION_STATE_DEGRADED;
    activation->last_status =
        APPLIANCE_PRODUCTION_SERVICE_ACTIVATION_NETWORK_LOST;
    ++activation->degraded_count;
    return APPLIANCE_PRODUCTION_SERVICE_ACTIVATION_NETWORK_LOST;
}

enum appliance_production_service_activation_status
appliance_production_service_activation_restart(
    struct appliance_production_service_activation *activation)
{
    enum appliance_production_service_https_status composition_status;
    enum appliance_production_service_activation_status start_status;

    if (activation == NULL) {
        return APPLIANCE_PRODUCTION_SERVICE_ACTIVATION_NULL;
    }
    if (!appliance_production_service_activation_published(activation)) {
        return APPLIANCE_PRODUCTION_SERVICE_ACTIVATION_UNPUBLISHED;
    }
    if (activation->state !=
            APPLIANCE_PRODUCTION_SERVICE_ACTIVATION_STATE_RUNNING ||
        !activation->network_active) {
        return APPLIANCE_PRODUCTION_SERVICE_ACTIVATION_BAD_STATE;
    }

    composition_status = appliance_production_service_https_stop(
        activation->composition);
    if (composition_status != APPLIANCE_PRODUCTION_SERVICE_HTTPS_OK) {
        return appliance_production_service_activation_fail(
            activation,
            APPLIANCE_PRODUCTION_SERVICE_ACTIVATION_RESTART_FAILED);
    }
    composition_status = appliance_production_service_https_stopped(
        activation->composition);
    if (composition_status != APPLIANCE_PRODUCTION_SERVICE_HTTPS_OK) {
        return appliance_production_service_activation_fail(
            activation,
            APPLIANCE_PRODUCTION_SERVICE_ACTIVATION_RESTART_FAILED);
    }
    composition_status = appliance_production_service_https_reset(
        activation->composition);
    if (composition_status != APPLIANCE_PRODUCTION_SERVICE_HTTPS_OK) {
        return appliance_production_service_activation_fail(
            activation,
            APPLIANCE_PRODUCTION_SERVICE_ACTIVATION_RESTART_FAILED);
    }
    composition_status = appliance_production_service_https_configure(
        activation->composition, &activation->evidence);
    if (composition_status != APPLIANCE_PRODUCTION_SERVICE_HTTPS_OK) {
        return appliance_production_service_activation_fail(
            activation,
            APPLIANCE_PRODUCTION_SERVICE_ACTIVATION_RESTART_FAILED);
    }

    activation->configured = true;
    start_status = appliance_production_service_activation_start(activation);
    if (start_status != APPLIANCE_PRODUCTION_SERVICE_ACTIVATION_OK) {
        return appliance_production_service_activation_fail(
            activation,
            APPLIANCE_PRODUCTION_SERVICE_ACTIVATION_RESTART_FAILED);
    }
    ++activation->restart_count;
    activation->last_status = APPLIANCE_PRODUCTION_SERVICE_ACTIVATION_OK;
    return APPLIANCE_PRODUCTION_SERVICE_ACTIVATION_OK;
}

enum appliance_production_service_activation_status
appliance_production_service_activation_stop(
    struct appliance_production_service_activation *activation)
{
    enum appliance_production_service_https_status composition_status;

    if (activation == NULL) {
        return APPLIANCE_PRODUCTION_SERVICE_ACTIVATION_NULL;
    }
    if (!appliance_production_service_activation_published(activation)) {
        return APPLIANCE_PRODUCTION_SERVICE_ACTIVATION_UNPUBLISHED;
    }
    if (activation->state ==
        APPLIANCE_PRODUCTION_SERVICE_ACTIVATION_STATE_STOPPED) {
        return APPLIANCE_PRODUCTION_SERVICE_ACTIVATION_OK;
    }
    if (activation->state ==
        APPLIANCE_PRODUCTION_SERVICE_ACTIVATION_STATE_RUNNING) {
        composition_status = appliance_production_service_https_stop(
            activation->composition);
        if (composition_status != APPLIANCE_PRODUCTION_SERVICE_HTTPS_OK) {
            return appliance_production_service_activation_fail(
                activation,
                APPLIANCE_PRODUCTION_SERVICE_ACTIVATION_STOP_FAILED);
        }
        composition_status = appliance_production_service_https_stopped(
            activation->composition);
        if (composition_status != APPLIANCE_PRODUCTION_SERVICE_HTTPS_OK) {
            return appliance_production_service_activation_fail(
                activation,
                APPLIANCE_PRODUCTION_SERVICE_ACTIVATION_STOP_FAILED);
        }
        ++activation->stop_count;
    }

    activation->state =
        APPLIANCE_PRODUCTION_SERVICE_ACTIVATION_STATE_STOPPED;
    activation->last_status = APPLIANCE_PRODUCTION_SERVICE_ACTIVATION_OK;
    return APPLIANCE_PRODUCTION_SERVICE_ACTIVATION_OK;
}

enum appliance_production_service_activation_status
appliance_production_service_activation_snapshot(
    const struct appliance_production_service_activation *activation,
    struct appliance_production_service_activation_snapshot *snapshot_out)
{
    if (activation == NULL || snapshot_out == NULL) {
        return APPLIANCE_PRODUCTION_SERVICE_ACTIVATION_NULL;
    }
    if (!appliance_production_service_activation_published(activation)) {
        return APPLIANCE_PRODUCTION_SERVICE_ACTIVATION_UNPUBLISHED;
    }

    appliance_production_service_activation_zero_bytes(
        snapshot_out, sizeof(*snapshot_out));
    snapshot_out->published = true;
    snapshot_out->version = activation->version;
    snapshot_out->state = activation->state;
    snapshot_out->last_status = activation->last_status;
    snapshot_out->source_generation = activation->source_generation;
    snapshot_out->network_active = activation->network_active;
    snapshot_out->entropy_ready = activation->entropy_ready;
    snapshot_out->certificate_ready = activation->certificate_ready;
    snapshot_out->https_enabled = activation->https_enabled;
    snapshot_out->start_count = activation->start_count;
    snapshot_out->restart_count = activation->restart_count;
    snapshot_out->stop_count = activation->stop_count;
    snapshot_out->degraded_count = activation->degraded_count;
    snapshot_out->fail_count = activation->fail_count;
    snapshot_out->secrets_logged = false;
    return APPLIANCE_PRODUCTION_SERVICE_ACTIVATION_OK;
}

static bool appliance_production_service_activation_append_bytes(
    char *output,
    size_t *offset,
    const char *text,
    size_t length)
{
    size_t index;

    if (*offset > APPLIANCE_PRODUCTION_SERVICE_ACTIVATION_OUTPUT_MAX ||
        length >
            APPLIANCE_PRODUCTION_SERVICE_ACTIVATION_OUTPUT_MAX - *offset) {
        return false;
    }
    for (index = 0U; index < length; ++index) {
        output[*offset + index] = text[index];
    }
    *offset += length;
    return true;
}

static bool appliance_production_service_activation_append_text(
    char *output,
    size_t *offset,
    const char *text)
{
    size_t length;

    if (text == NULL) {
        return false;
    }
    for (length = 0U; text[length] != '\0'; ++length) {
    }
    return appliance_production_service_activation_append_bytes(
        output, offset, text, length);
}

static bool appliance_production_service_activation_append_char(
    char *output,
    size_t *offset,
    char value)
{
    return appliance_production_service_activation_append_bytes(
        output, offset, &value, 1U);
}

static bool appliance_production_service_activation_append_u64(
    char *output,
    size_t *offset,
    uint64_t value)
{
    char digits[20];
    size_t count = 0U;

    if (value == 0U) {
        digits[count++] = '0';
    }
    while (value != 0U) {
        digits[count++] = (char)('0' + (value % 10U));
        value /= 10U;
    }
    while (count > 0U) {
        --count;
        if (!appliance_production_service_activation_append_char(
                output, offset, digits[count])) {
            return false;
        }
    }
    return true;
}

enum appliance_production_service_activation_status
appliance_production_service_activation_render(
    const struct appliance_production_service_activation_snapshot *snapshot,
    char *output,
    size_t capacity,
    size_t *output_length)
{
    char rendered[APPLIANCE_PRODUCTION_SERVICE_ACTIVATION_OUTPUT_MAX + 1U];
    size_t offset = 0U;
    size_t index;

    if (snapshot == NULL || output == NULL || output_length == NULL) {
        return APPLIANCE_PRODUCTION_SERVICE_ACTIVATION_NULL;
    }
    if (!snapshot->published ||
        snapshot->version != APPLIANCE_PRODUCTION_SERVICE_ACTIVATION_VERSION ||
        !appliance_production_service_activation_state_valid(
            snapshot->state) ||
        !appliance_production_service_activation_status_valid(
            snapshot->last_status)) {
        return APPLIANCE_PRODUCTION_SERVICE_ACTIVATION_BAD_STATE;
    }

    if (!appliance_production_service_activation_append_text(
            rendered, &offset, "production_service_activation v=") ||
        !appliance_production_service_activation_append_u64(
            rendered, &offset, snapshot->version) ||
        !appliance_production_service_activation_append_text(
            rendered, &offset, " state=") ||
        !appliance_production_service_activation_append_text(
            rendered, &offset,
            appliance_production_service_activation_state_string(
                snapshot->state)) ||
        !appliance_production_service_activation_append_text(
            rendered, &offset, " https=") ||
        !appliance_production_service_activation_append_text(
            rendered, &offset,
            snapshot->https_enabled ? "ENABLED" : "DISABLED") ||
        !appliance_production_service_activation_append_text(
            rendered, &offset, " network=") ||
        !appliance_production_service_activation_append_text(
            rendered, &offset,
            snapshot->network_active ? "ACTIVE" : "INACTIVE") ||
        !appliance_production_service_activation_append_text(
            rendered, &offset, " entropy=") ||
        !appliance_production_service_activation_append_text(
            rendered, &offset,
            snapshot->entropy_ready ? "READY" : "UNAVAILABLE") ||
        !appliance_production_service_activation_append_text(
            rendered, &offset, " certificate=") ||
        !appliance_production_service_activation_append_text(
            rendered, &offset,
            snapshot->certificate_ready ? "READY" : "UNAVAILABLE") ||
        !appliance_production_service_activation_append_text(
            rendered, &offset, " secrets=NONE start=") ||
        !appliance_production_service_activation_append_u64(
            rendered, &offset, snapshot->start_count) ||
        !appliance_production_service_activation_append_text(
            rendered, &offset, " restart=") ||
        !appliance_production_service_activation_append_u64(
            rendered, &offset, snapshot->restart_count) ||
        !appliance_production_service_activation_append_text(
            rendered, &offset, " degraded=") ||
        !appliance_production_service_activation_append_u64(
            rendered, &offset, snapshot->degraded_count) ||
        !appliance_production_service_activation_append_text(
            rendered, &offset, " fail=") ||
        !appliance_production_service_activation_append_u64(
            rendered, &offset, snapshot->fail_count) ||
        !appliance_production_service_activation_append_char(
            rendered, &offset, '\n')) {
        return APPLIANCE_PRODUCTION_SERVICE_ACTIVATION_BUFFER_TOO_SMALL;
    }
    if (capacity <= offset) {
        return APPLIANCE_PRODUCTION_SERVICE_ACTIVATION_BUFFER_TOO_SMALL;
    }
    for (index = 0U; index < offset; ++index) {
        output[index] = rendered[index];
    }
    output[offset] = '\0';
    *output_length = offset;
    return APPLIANCE_PRODUCTION_SERVICE_ACTIVATION_OK;
}

enum appliance_production_service_activation_state
appliance_production_service_activation_state(
    const struct appliance_production_service_activation *activation)
{
    return appliance_production_service_activation_published(activation)
        ? activation->state
        : APPLIANCE_PRODUCTION_SERVICE_ACTIVATION_STATE_NONE;
}

bool appliance_production_service_activation_running(
    const struct appliance_production_service_activation *activation)
{
    return appliance_production_service_activation_published(activation) &&
           activation->state ==
               APPLIANCE_PRODUCTION_SERVICE_ACTIVATION_STATE_RUNNING;
}

bool appliance_production_service_activation_secrets_logged(
    const struct appliance_production_service_activation *activation)
{
    (void)activation;
    return false;
}

const char *appliance_production_service_activation_state_string(
    enum appliance_production_service_activation_state state)
{
    switch (state) {
    case APPLIANCE_PRODUCTION_SERVICE_ACTIVATION_STATE_NONE:
        return "NONE";
    case APPLIANCE_PRODUCTION_SERVICE_ACTIVATION_STATE_STOPPED:
        return "STOPPED";
    case APPLIANCE_PRODUCTION_SERVICE_ACTIVATION_STATE_AWAITING_EVIDENCE:
        return "AWAITING_EVIDENCE";
    case APPLIANCE_PRODUCTION_SERVICE_ACTIVATION_STATE_AWAITING_NETWORK:
        return "AWAITING_NETWORK";
    case APPLIANCE_PRODUCTION_SERVICE_ACTIVATION_STATE_AWAITING_READINESS:
        return "AWAITING_READINESS";
    case APPLIANCE_PRODUCTION_SERVICE_ACTIVATION_STATE_RUNNING:
        return "RUNNING";
    case APPLIANCE_PRODUCTION_SERVICE_ACTIVATION_STATE_DEGRADED:
        return "DEGRADED";
    case APPLIANCE_PRODUCTION_SERVICE_ACTIVATION_STATE_FAILED:
        return "FAILED";
    default:
        return NULL;
    }
}

const char *appliance_production_service_activation_status_string(
    enum appliance_production_service_activation_status status)
{
    switch (status) {
    case APPLIANCE_PRODUCTION_SERVICE_ACTIVATION_OK:
        return "APPLIANCE_PRODUCTION_SERVICE_ACTIVATION_OK";
    case APPLIANCE_PRODUCTION_SERVICE_ACTIVATION_NULL:
        return "APPLIANCE_PRODUCTION_SERVICE_ACTIVATION_NULL";
    case APPLIANCE_PRODUCTION_SERVICE_ACTIVATION_BAD_STATE:
        return "APPLIANCE_PRODUCTION_SERVICE_ACTIVATION_BAD_STATE";
    case APPLIANCE_PRODUCTION_SERVICE_ACTIVATION_BAD_VERSION:
        return "APPLIANCE_PRODUCTION_SERVICE_ACTIVATION_BAD_VERSION";
    case APPLIANCE_PRODUCTION_SERVICE_ACTIVATION_UNPUBLISHED:
        return "APPLIANCE_PRODUCTION_SERVICE_ACTIVATION_UNPUBLISHED";
    case APPLIANCE_PRODUCTION_SERVICE_ACTIVATION_INVALID_EVIDENCE:
        return "APPLIANCE_PRODUCTION_SERVICE_ACTIVATION_INVALID_EVIDENCE";
    case APPLIANCE_PRODUCTION_SERVICE_ACTIVATION_MISSING_NETWORK:
        return "APPLIANCE_PRODUCTION_SERVICE_ACTIVATION_MISSING_NETWORK";
    case APPLIANCE_PRODUCTION_SERVICE_ACTIVATION_MISSING_READINESS:
        return "APPLIANCE_PRODUCTION_SERVICE_ACTIVATION_MISSING_READINESS";
    case APPLIANCE_PRODUCTION_SERVICE_ACTIVATION_START_FAILED:
        return "APPLIANCE_PRODUCTION_SERVICE_ACTIVATION_START_FAILED";
    case APPLIANCE_PRODUCTION_SERVICE_ACTIVATION_STOP_FAILED:
        return "APPLIANCE_PRODUCTION_SERVICE_ACTIVATION_STOP_FAILED";
    case APPLIANCE_PRODUCTION_SERVICE_ACTIVATION_RESTART_FAILED:
        return "APPLIANCE_PRODUCTION_SERVICE_ACTIVATION_RESTART_FAILED";
    case APPLIANCE_PRODUCTION_SERVICE_ACTIVATION_NETWORK_LOST:
        return "APPLIANCE_PRODUCTION_SERVICE_ACTIVATION_NETWORK_LOST";
    case APPLIANCE_PRODUCTION_SERVICE_ACTIVATION_BUFFER_TOO_SMALL:
        return "APPLIANCE_PRODUCTION_SERVICE_ACTIVATION_BUFFER_TOO_SMALL";
    default:
        return NULL;
    }
}
