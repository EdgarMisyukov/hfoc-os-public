#include "appliance_production_management.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

static void appliance_production_management_zero_bytes(
    void *destination,
    size_t length)
{
    unsigned char *bytes = destination;
    size_t index;

    for (index = 0U; index < length; ++index) {
        bytes[index] = 0U;
    }
}

static bool appliance_production_management_state_valid(
    enum appliance_production_management_state state)
{
    return state >= APPLIANCE_PRODUCTION_MANAGEMENT_STATE_UNINITIALIZED &&
           state <= APPLIANCE_PRODUCTION_MANAGEMENT_STATE_FAILED;
}

static bool appliance_production_management_status_valid(
    enum appliance_production_management_status status)
{
    return status >= APPLIANCE_PRODUCTION_MANAGEMENT_OK &&
           status <= APPLIANCE_PRODUCTION_MANAGEMENT_BUFFER_TOO_SMALL;
}

static bool appliance_production_management_failure_valid(
    enum appliance_production_management_failure failure)
{
    return failure >= APPLIANCE_PRODUCTION_MANAGEMENT_FAILURE_NONE &&
           failure <= APPLIANCE_PRODUCTION_MANAGEMENT_FAILURE_STOP;
}

static bool appliance_production_management_audit_valid(
    enum appliance_production_management_audit audit)
{
    return audit >= APPLIANCE_PRODUCTION_MANAGEMENT_AUDIT_UNKNOWN &&
           audit <= APPLIANCE_PRODUCTION_MANAGEMENT_AUDIT_UNAVAILABLE;
}

static bool appliance_production_management_support_valid(
    enum appliance_production_management_support support)
{
    return support >= APPLIANCE_PRODUCTION_MANAGEMENT_SUPPORT_UNKNOWN &&
           support <= APPLIANCE_PRODUCTION_MANAGEMENT_SUPPORT_UNAVAILABLE;
}

static bool appliance_production_management_initialized(
    const struct appliance_production_management *management)
{
    return management != NULL &&
           management->initialized &&
           management->version == APPLIANCE_PRODUCTION_MANAGEMENT_VERSION &&
           appliance_production_management_state_valid(management->state) &&
           appliance_production_management_status_valid(
               management->last_status) &&
           appliance_production_management_failure_valid(
               management->failure);
}

static void appliance_production_management_clear_evidence(
    struct appliance_production_management *management)
{
    management->evidence_published = false;
    management->network_active = false;
    management->production_services_ready = false;
    management->https_management_ready = false;
    management->authorization_available = false;
    management->audit_chain =
        APPLIANCE_PRODUCTION_MANAGEMENT_AUDIT_UNKNOWN;
    management->audit_records = 0U;
    management->audit_head = 0U;
    management->support_bundle =
        APPLIANCE_PRODUCTION_MANAGEMENT_SUPPORT_UNKNOWN;
    management->support_withheld = 0U;
    management->support_session_active = false;
    management->support_break_glass_armed = false;
    management->degraded = false;
    management->source_generation = 0U;
}

static enum appliance_production_management_status
appliance_production_management_set_failure(
    struct appliance_production_management *management,
    enum appliance_production_management_failure failure)
{
    if (management == NULL) {
        return APPLIANCE_PRODUCTION_MANAGEMENT_NULL;
    }
    if (!appliance_production_management_failure_valid(failure) ||
        failure == APPLIANCE_PRODUCTION_MANAGEMENT_FAILURE_NONE) {
        return APPLIANCE_PRODUCTION_MANAGEMENT_BAD_STATE;
    }
    management->state = APPLIANCE_PRODUCTION_MANAGEMENT_STATE_FAILED;
    management->failure = failure;
    management->last_status = APPLIANCE_PRODUCTION_MANAGEMENT_OK;
    appliance_production_management_clear_evidence(management);
    return APPLIANCE_PRODUCTION_MANAGEMENT_OK;
}

enum appliance_production_management_status
appliance_production_management_init(
    struct appliance_production_management *management)
{
    if (management == NULL) {
        return APPLIANCE_PRODUCTION_MANAGEMENT_NULL;
    }
    if (management->initialized) {
        return APPLIANCE_PRODUCTION_MANAGEMENT_ALREADY_PUBLISHED;
    }
    appliance_production_management_zero_bytes(
        management, sizeof(*management));
    management->initialized = true;
    management->version = APPLIANCE_PRODUCTION_MANAGEMENT_VERSION;
    management->state = APPLIANCE_PRODUCTION_MANAGEMENT_STATE_PREPARED;
    management->last_status = APPLIANCE_PRODUCTION_MANAGEMENT_OK;
    management->failure = APPLIANCE_PRODUCTION_MANAGEMENT_FAILURE_NONE;
    return APPLIANCE_PRODUCTION_MANAGEMENT_OK;
}

static bool appliance_production_management_required_evidence_ready(
    const struct appliance_production_management_evidence *evidence)
{
    if (!evidence->system_identity_present ||
        !evidence->configuration_ready ||
        !evidence->production_services_ready ||
        !evidence->https_management_ready) {
        return false;
    }
    if (evidence->network_required &&
        !evidence->network_active) {
        return false;
    }
    return true;
}

enum appliance_production_management_status
appliance_production_management_publish(
    struct appliance_production_management *management,
    const struct appliance_production_management_evidence *evidence)
{
    bool ready;

    if (management == NULL || evidence == NULL) {
        return APPLIANCE_PRODUCTION_MANAGEMENT_NULL;
    }
    if (!appliance_production_management_initialized(management)) {
        return APPLIANCE_PRODUCTION_MANAGEMENT_UNPUBLISHED;
    }
    if (management->state == APPLIANCE_PRODUCTION_MANAGEMENT_STATE_STOPPED ||
        management->state == APPLIANCE_PRODUCTION_MANAGEMENT_STATE_FAILED) {
        return APPLIANCE_PRODUCTION_MANAGEMENT_BAD_STATE;
    }
    if (evidence->version !=
        APPLIANCE_PRODUCTION_MANAGEMENT_EVIDENCE_VERSION) {
        return APPLIANCE_PRODUCTION_MANAGEMENT_BAD_VERSION;
    }
    if (!evidence->published ||
        evidence->source_generation == 0U) {
        return APPLIANCE_PRODUCTION_MANAGEMENT_INVALID_EVIDENCE;
    }
    if (!appliance_production_management_audit_valid(
            evidence->audit_chain) ||
        (evidence->audit_chain_present &&
         evidence->audit_chain ==
             APPLIANCE_PRODUCTION_MANAGEMENT_AUDIT_UNKNOWN)) {
        return APPLIANCE_PRODUCTION_MANAGEMENT_INVALID_EVIDENCE;
    }
    if (!appliance_production_management_support_valid(
            evidence->support_bundle) ||
        (evidence->support_present &&
         evidence->support_bundle ==
             APPLIANCE_PRODUCTION_MANAGEMENT_SUPPORT_UNKNOWN)) {
        return APPLIANCE_PRODUCTION_MANAGEMENT_INVALID_EVIDENCE;
    }
    if (evidence->source_generation <
        management->source_generation) {
        return APPLIANCE_PRODUCTION_MANAGEMENT_STALE_EVIDENCE;
    }
    ready = appliance_production_management_required_evidence_ready(
        evidence);
    if (!ready) {
        if (management->state ==
                APPLIANCE_PRODUCTION_MANAGEMENT_STATE_ACTIVE ||
            management->state ==
                APPLIANCE_PRODUCTION_MANAGEMENT_STATE_DEGRADED) {
            return appliance_production_management_set_failure(
                management,
                APPLIANCE_PRODUCTION_MANAGEMENT_FAILURE_EVIDENCE);
        }
        management->state =
            APPLIANCE_PRODUCTION_MANAGEMENT_STATE_AWAITING_EVIDENCE;
        management->last_status =
            APPLIANCE_PRODUCTION_MANAGEMENT_MISSING_EVIDENCE;
        return APPLIANCE_PRODUCTION_MANAGEMENT_MISSING_EVIDENCE;
    }
    management->source_generation = evidence->source_generation;
    management->evidence_published = true;
    management->network_active = evidence->network_active;
    management->production_services_ready =
        evidence->production_services_ready;
    management->https_management_ready =
        evidence->https_management_ready;
    management->authorization_available =
        evidence->authorization_available;
    management->boot_attestation = evidence->boot_attestation_present
        ? evidence->boot_attestation
        : APPLIANCE_PRODUCTION_MANAGEMENT_ATTESTATION_UNAVAILABLE;
    management->audit_chain = evidence->audit_chain_present
        ? evidence->audit_chain
        : APPLIANCE_PRODUCTION_MANAGEMENT_AUDIT_UNAVAILABLE;
    management->audit_records = evidence->audit_chain_present
        ? evidence->audit_records
        : 0U;
    management->audit_head = evidence->audit_chain_present
        ? evidence->audit_head
        : 0U;
    management->support_bundle = evidence->support_present
        ? evidence->support_bundle
        : APPLIANCE_PRODUCTION_MANAGEMENT_SUPPORT_UNAVAILABLE;
    management->support_withheld = evidence->support_present
        ? evidence->support_withheld
        : 0U;
    management->support_session_active = evidence->support_present &&
        evidence->support_session_active;
    management->support_break_glass_armed = evidence->support_present &&
        evidence->support_break_glass_armed;
    management->degraded = evidence->degraded_requested ||
        (evidence->network_required &&
         evidence->network_active &&
         !evidence->network_health_ok);
    management->state = management->degraded
        ? APPLIANCE_PRODUCTION_MANAGEMENT_STATE_DEGRADED
        : APPLIANCE_PRODUCTION_MANAGEMENT_STATE_ACTIVE;
    management->last_status = APPLIANCE_PRODUCTION_MANAGEMENT_OK;
    management->failure = APPLIANCE_PRODUCTION_MANAGEMENT_FAILURE_NONE;
    ++management->update_count;
    return APPLIANCE_PRODUCTION_MANAGEMENT_OK;
}

enum appliance_production_management_status
appliance_production_management_stop(
    struct appliance_production_management *management)
{
    if (management == NULL) {
        return APPLIANCE_PRODUCTION_MANAGEMENT_NULL;
    }
    if (!appliance_production_management_initialized(management)) {
        return APPLIANCE_PRODUCTION_MANAGEMENT_UNPUBLISHED;
    }
    if (management->state !=
            APPLIANCE_PRODUCTION_MANAGEMENT_STATE_PREPARED &&
        management->state !=
            APPLIANCE_PRODUCTION_MANAGEMENT_STATE_AWAITING_EVIDENCE &&
        management->state !=
            APPLIANCE_PRODUCTION_MANAGEMENT_STATE_ACTIVE &&
        management->state !=
            APPLIANCE_PRODUCTION_MANAGEMENT_STATE_DEGRADED) {
        return APPLIANCE_PRODUCTION_MANAGEMENT_BAD_STATE;
    }
    management->state = APPLIANCE_PRODUCTION_MANAGEMENT_STATE_STOPPED;
    management->last_status = APPLIANCE_PRODUCTION_MANAGEMENT_OK;
    appliance_production_management_clear_evidence(management);
    ++management->stop_count;
    return APPLIANCE_PRODUCTION_MANAGEMENT_OK;
}

enum appliance_production_management_status
appliance_production_management_fail_closed(
    struct appliance_production_management *management,
    enum appliance_production_management_failure failure)
{
    if (management == NULL) {
        return APPLIANCE_PRODUCTION_MANAGEMENT_NULL;
    }
    if (!appliance_production_management_initialized(management)) {
        return APPLIANCE_PRODUCTION_MANAGEMENT_UNPUBLISHED;
    }
    if (management->state !=
            APPLIANCE_PRODUCTION_MANAGEMENT_STATE_AWAITING_EVIDENCE &&
        management->state !=
            APPLIANCE_PRODUCTION_MANAGEMENT_STATE_ACTIVE &&
        management->state !=
            APPLIANCE_PRODUCTION_MANAGEMENT_STATE_DEGRADED) {
        return APPLIANCE_PRODUCTION_MANAGEMENT_BAD_STATE;
    }
    return appliance_production_management_set_failure(
        management, failure);
}

enum appliance_production_management_status
appliance_production_management_reset(
    struct appliance_production_management *management)
{
    if (management == NULL) {
        return APPLIANCE_PRODUCTION_MANAGEMENT_NULL;
    }
    if (!appliance_production_management_initialized(management)) {
        return APPLIANCE_PRODUCTION_MANAGEMENT_UNPUBLISHED;
    }
    if (management->state !=
            APPLIANCE_PRODUCTION_MANAGEMENT_STATE_STOPPED &&
        management->state !=
            APPLIANCE_PRODUCTION_MANAGEMENT_STATE_FAILED &&
        management->state !=
            APPLIANCE_PRODUCTION_MANAGEMENT_STATE_UNINITIALIZED) {
        return APPLIANCE_PRODUCTION_MANAGEMENT_BAD_STATE;
    }
    appliance_production_management_clear_evidence(management);
    management->state = APPLIANCE_PRODUCTION_MANAGEMENT_STATE_PREPARED;
    management->last_status = APPLIANCE_PRODUCTION_MANAGEMENT_OK;
    management->failure = APPLIANCE_PRODUCTION_MANAGEMENT_FAILURE_NONE;
    ++management->reset_count;
    return APPLIANCE_PRODUCTION_MANAGEMENT_OK;
}

enum appliance_production_management_status
appliance_production_management_snapshot(
    const struct appliance_production_management *management,
    struct appliance_production_management_snapshot *snapshot_out)
{
    enum appliance_production_management_health health;

    if (management == NULL || snapshot_out == NULL) {
        return APPLIANCE_PRODUCTION_MANAGEMENT_NULL;
    }
    if (!appliance_production_management_initialized(management)) {
        return APPLIANCE_PRODUCTION_MANAGEMENT_UNPUBLISHED;
    }
    if (management->state ==
        APPLIANCE_PRODUCTION_MANAGEMENT_STATE_ACTIVE) {
        health = APPLIANCE_PRODUCTION_MANAGEMENT_HEALTH_PASS;
    } else if (management->state ==
               APPLIANCE_PRODUCTION_MANAGEMENT_STATE_DEGRADED) {
        health = APPLIANCE_PRODUCTION_MANAGEMENT_HEALTH_WARN;
    } else if (management->state ==
               APPLIANCE_PRODUCTION_MANAGEMENT_STATE_FAILED) {
        health = APPLIANCE_PRODUCTION_MANAGEMENT_HEALTH_FAIL;
    } else {
        health = APPLIANCE_PRODUCTION_MANAGEMENT_HEALTH_UNKNOWN;
    }
    appliance_production_management_zero_bytes(
        snapshot_out, sizeof(*snapshot_out));
    snapshot_out->published = true;
    snapshot_out->version = APPLIANCE_PRODUCTION_MANAGEMENT_VERSION;
    snapshot_out->state = management->state;
    snapshot_out->last_status = management->last_status;
    snapshot_out->failure = management->failure;
    snapshot_out->health = health;
    snapshot_out->network_active = management->network_active;
    snapshot_out->production_services_ready =
        management->production_services_ready;
    snapshot_out->https_management_ready =
        management->https_management_ready;
    snapshot_out->authorization_available =
        management->authorization_available;
    snapshot_out->boot_attestation = management->boot_attestation;
    snapshot_out->audit_chain = management->audit_chain;
    snapshot_out->audit_records = management->audit_records;
    snapshot_out->audit_head = management->audit_head;
    snapshot_out->support_bundle = management->support_bundle;
    snapshot_out->support_withheld = management->support_withheld;
    snapshot_out->support_session_active =
        management->support_session_active;
    snapshot_out->support_break_glass_armed =
        management->support_break_glass_armed;
    snapshot_out->source_generation = management->source_generation;
    snapshot_out->update_count = management->update_count;
    snapshot_out->stop_count = management->stop_count;
    snapshot_out->reset_count = management->reset_count;
    return APPLIANCE_PRODUCTION_MANAGEMENT_OK;
}

static bool appliance_production_management_append(
    char *output,
    size_t *offset,
    const char *text,
    size_t length)
{
    size_t index;

    if (*offset > APPLIANCE_PRODUCTION_MANAGEMENT_OUTPUT_MAX ||
        length > APPLIANCE_PRODUCTION_MANAGEMENT_OUTPUT_MAX - *offset) {
        return false;
    }
    for (index = 0U; index < length; ++index) {
        output[*offset + index] = text[index];
    }
    *offset += length;
    return true;
}

static bool appliance_production_management_append_text(
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
    return appliance_production_management_append(
        output, offset, text, length);
}

const char *appliance_production_management_attestation_string(
    enum appliance_production_management_attestation attestation)
{
    switch (attestation) {
    case APPLIANCE_PRODUCTION_MANAGEMENT_ATTESTATION_ATTESTED:
        return "ATTESTED";
    case APPLIANCE_PRODUCTION_MANAGEMENT_ATTESTATION_UNATTESTED:
        return "UNATTESTED";
    case APPLIANCE_PRODUCTION_MANAGEMENT_ATTESTATION_UNAVAILABLE:
        return "UNAVAILABLE";
    default:
        return "UNKNOWN";
    }
}

const char *appliance_production_management_audit_string(
    enum appliance_production_management_audit audit)
{
    switch (audit) {
    case APPLIANCE_PRODUCTION_MANAGEMENT_AUDIT_OK:
        return "OK";
    case APPLIANCE_PRODUCTION_MANAGEMENT_AUDIT_TAMPERED:
        return "TAMPERED";
    case APPLIANCE_PRODUCTION_MANAGEMENT_AUDIT_UNAVAILABLE:
        return "UNAVAILABLE";
    default:
        return "UNKNOWN";
    }
}

const char *appliance_production_management_support_string(
    enum appliance_production_management_support support)
{
    switch (support) {
    case APPLIANCE_PRODUCTION_MANAGEMENT_SUPPORT_SEALED:
        return "SEALED";
    case APPLIANCE_PRODUCTION_MANAGEMENT_SUPPORT_UNSIGNED:
        return "UNSIGNED";
    case APPLIANCE_PRODUCTION_MANAGEMENT_SUPPORT_REDACTION_FAILED:
        return "REDACTION_FAILED";
    case APPLIANCE_PRODUCTION_MANAGEMENT_SUPPORT_UNAVAILABLE:
        return "UNAVAILABLE";
    default:
        return "UNKNOWN";
    }
}

static bool appliance_production_management_append_u64(
    char *output,
    size_t *offset,
    uint64_t value)
{
    char digits[21];
    uint32_t count = 0U;

    if (value == 0U) {
        return appliance_production_management_append_text(
            output, offset, "0");
    }
    while (value != 0U && count < 20U) {
        digits[count] = (char)('0' + (value % 10U));
        value /= 10U;
        ++count;
    }
    while (count != 0U) {
        char single[2];

        --count;
        single[0] = digits[count];
        single[1] = '\0';
        if (!appliance_production_management_append_text(
                output, offset, single)) {
            return false;
        }
    }
    return true;
}

static bool appliance_production_management_append_hex64(
    char *output,
    size_t *offset,
    uint64_t value)
{
    static const char hex[] = "0123456789abcdef";
    char digits[17];
    uint32_t index;

    for (index = 0U; index < 16U; ++index) {
        digits[index] =
            hex[(value >> ((15U - index) * 4U)) & UINT64_C(0xF)];
    }
    digits[16] = '\0';
    return appliance_production_management_append_text(
        output, offset, digits);
}

enum appliance_production_management_status
appliance_production_management_render(
    const struct appliance_production_management_snapshot *snapshot,
    char *output,
    size_t capacity,
    size_t *output_length)
{
    char rendered[APPLIANCE_PRODUCTION_MANAGEMENT_OUTPUT_MAX + 1U];
    size_t offset = 0U;
    const char *state;
    const char *network;
    const char *services;
    const char *https;
    const char *authorization;
    const char *health;
    const char *identity;
    const char *configuration;

    if (snapshot == NULL || output == NULL || output_length == NULL) {
        return APPLIANCE_PRODUCTION_MANAGEMENT_NULL;
    }
    if (!snapshot->published ||
        snapshot->version != APPLIANCE_PRODUCTION_MANAGEMENT_VERSION ||
        !appliance_production_management_state_valid(snapshot->state) ||
        !appliance_production_management_status_valid(
            snapshot->last_status) ||
        !appliance_production_management_failure_valid(
            snapshot->failure) ||
        !appliance_production_management_audit_valid(
            snapshot->audit_chain) ||
        !appliance_production_management_support_valid(
            snapshot->support_bundle)) {
        return APPLIANCE_PRODUCTION_MANAGEMENT_BAD_STATE;
    }
    appliance_production_management_zero_bytes(
        rendered, sizeof(rendered));
    state = appliance_production_management_state_string(snapshot->state);
    network = snapshot->network_active ? "ACTIVE" : "STOPPED";
    services = snapshot->production_services_ready
        ? "ACTIVE"
        : "STOPPED";
    https = snapshot->https_management_ready
        ? "READY"
        : "UNKNOWN";
    authorization = snapshot->authorization_available
        ? "AVAILABLE"
        : "UNAVAILABLE";
    identity = snapshot->state ==
            APPLIANCE_PRODUCTION_MANAGEMENT_STATE_ACTIVE ||
        snapshot->state ==
            APPLIANCE_PRODUCTION_MANAGEMENT_STATE_DEGRADED
        ? "PRESENT"
        : "UNKNOWN";
    configuration = snapshot->state ==
            APPLIANCE_PRODUCTION_MANAGEMENT_STATE_ACTIVE ||
        snapshot->state ==
            APPLIANCE_PRODUCTION_MANAGEMENT_STATE_DEGRADED
        ? "COMMITTED"
        : "UNKNOWN";
    switch (snapshot->health) {
    case APPLIANCE_PRODUCTION_MANAGEMENT_HEALTH_PASS:
        health = "PASS";
        break;
    case APPLIANCE_PRODUCTION_MANAGEMENT_HEALTH_WARN:
        health = "WARN";
        break;
    case APPLIANCE_PRODUCTION_MANAGEMENT_HEALTH_FAIL:
        health = "FAIL";
        break;
    default:
        health = "UNKNOWN";
        break;
    }
    if (state == NULL ||
        !appliance_production_management_append_text(
            rendered, &offset,
            "production_management v=1 state=") ||
        !appliance_production_management_append_text(
            rendered, &offset, state) ||
        !appliance_production_management_append_text(
            rendered, &offset, " identity=") ||
        !appliance_production_management_append_text(
            rendered, &offset, identity) ||
        !appliance_production_management_append_text(
            rendered, &offset, " config=") ||
        !appliance_production_management_append_text(
            rendered, &offset, configuration) ||
        !appliance_production_management_append_text(
            rendered, &offset, " network=") ||
        !appliance_production_management_append_text(
            rendered, &offset, network) ||
        !appliance_production_management_append_text(
            rendered, &offset, " services=") ||
        !appliance_production_management_append_text(
            rendered, &offset, services) ||
        !appliance_production_management_append_text(
            rendered, &offset, " https=") ||
        !appliance_production_management_append_text(
            rendered, &offset, https) ||
        !appliance_production_management_append_text(
            rendered, &offset, " authorization=") ||
        !appliance_production_management_append_text(
            rendered, &offset, authorization) ||
        !appliance_production_management_append_text(
            rendered, &offset, " attestation=") ||
        !appliance_production_management_append_text(
            rendered, &offset,
            appliance_production_management_attestation_string(
                snapshot->boot_attestation)) ||
        !appliance_production_management_append_text(
            rendered, &offset, " health=") ||
        !appliance_production_management_append_text(
            rendered, &offset, health) ||
        !appliance_production_management_append_text(
            rendered, &offset, " audit_chain=") ||
        !appliance_production_management_append_text(
            rendered, &offset,
            appliance_production_management_audit_string(
                snapshot->audit_chain)) ||
        !appliance_production_management_append_text(
            rendered, &offset, " audit_records=") ||
        !appliance_production_management_append_u64(
            rendered, &offset, snapshot->audit_records) ||
        !appliance_production_management_append_text(
            rendered, &offset, " audit_head=") ||
        !appliance_production_management_append_hex64(
            rendered, &offset, snapshot->audit_head) ||
        !appliance_production_management_append_text(
            rendered, &offset, " support=") ||
        !appliance_production_management_append_text(
            rendered, &offset,
            appliance_production_management_support_string(
                snapshot->support_bundle)) ||
        !appliance_production_management_append_text(
            rendered, &offset, " support_withheld=") ||
        !appliance_production_management_append_u64(
            rendered, &offset, snapshot->support_withheld) ||
        !appliance_production_management_append_text(
            rendered, &offset, " session=") ||
        !appliance_production_management_append_text(
            rendered, &offset,
            snapshot->support_session_active ? "ACTIVE" : "NONE") ||
        !appliance_production_management_append_text(
            rendered, &offset, " break_glass=") ||
        !appliance_production_management_append_text(
            rendered, &offset,
            snapshot->support_break_glass_armed ? "ARMED" : "STANDBY") ||
        !appliance_production_management_append(
            rendered, &offset, "\n", 1U)) {
        return APPLIANCE_PRODUCTION_MANAGEMENT_BUFFER_TOO_SMALL;
    }
    if (offset + 1U > capacity) {
        return APPLIANCE_PRODUCTION_MANAGEMENT_BUFFER_TOO_SMALL;
    }
    appliance_production_management_zero_bytes(output, capacity);
    {
        size_t index;

        for (index = 0U; index < offset; ++index) {
            output[index] = rendered[index];
        }
    }
    output[offset] = '\0';
    *output_length = offset;
    return APPLIANCE_PRODUCTION_MANAGEMENT_OK;
}

enum appliance_production_management_state
appliance_production_management_state(
    const struct appliance_production_management *management)
{
    if (!appliance_production_management_initialized(management)) {
        return APPLIANCE_PRODUCTION_MANAGEMENT_STATE_NONE;
    }
    return management->state;
}

enum appliance_production_management_failure
appliance_production_management_failure(
    const struct appliance_production_management *management)
{
    if (!appliance_production_management_initialized(management)) {
        return APPLIANCE_PRODUCTION_MANAGEMENT_FAILURE_NONE;
    }
    return management->failure;
}

bool appliance_production_management_published(
    const struct appliance_production_management *management)
{
    return appliance_production_management_initialized(management) &&
           management->evidence_published;
}

const char *appliance_production_management_state_string(
    enum appliance_production_management_state state)
{
    switch (state) {
    case APPLIANCE_PRODUCTION_MANAGEMENT_STATE_UNINITIALIZED:
        return "UNINITIALIZED";
    case APPLIANCE_PRODUCTION_MANAGEMENT_STATE_PREPARED:
        return "PREPARED";
    case APPLIANCE_PRODUCTION_MANAGEMENT_STATE_AWAITING_EVIDENCE:
        return "AWAITING_EVIDENCE";
    case APPLIANCE_PRODUCTION_MANAGEMENT_STATE_ACTIVE:
        return "ACTIVE";
    case APPLIANCE_PRODUCTION_MANAGEMENT_STATE_DEGRADED:
        return "DEGRADED";
    case APPLIANCE_PRODUCTION_MANAGEMENT_STATE_STOPPED:
        return "STOPPED";
    case APPLIANCE_PRODUCTION_MANAGEMENT_STATE_FAILED:
        return "FAILED";
    default:
        return NULL;
    }
}

const char *appliance_production_management_status_string(
    enum appliance_production_management_status status)
{
    switch (status) {
    case APPLIANCE_PRODUCTION_MANAGEMENT_OK:
        return "OK";
    case APPLIANCE_PRODUCTION_MANAGEMENT_NULL:
        return "NULL";
    case APPLIANCE_PRODUCTION_MANAGEMENT_BAD_STATE:
        return "BAD_STATE";
    case APPLIANCE_PRODUCTION_MANAGEMENT_BAD_VERSION:
        return "BAD_VERSION";
    case APPLIANCE_PRODUCTION_MANAGEMENT_ALREADY_PUBLISHED:
        return "ALREADY_PUBLISHED";
    case APPLIANCE_PRODUCTION_MANAGEMENT_UNPUBLISHED:
        return "UNPUBLISHED";
    case APPLIANCE_PRODUCTION_MANAGEMENT_INVALID_EVIDENCE:
        return "INVALID_EVIDENCE";
    case APPLIANCE_PRODUCTION_MANAGEMENT_STALE_EVIDENCE:
        return "STALE_EVIDENCE";
    case APPLIANCE_PRODUCTION_MANAGEMENT_MISSING_EVIDENCE:
        return "MISSING_EVIDENCE";
    case APPLIANCE_PRODUCTION_MANAGEMENT_STOP_FAILED:
        return "STOP_FAILED";
    case APPLIANCE_PRODUCTION_MANAGEMENT_BUFFER_TOO_SMALL:
        return "BUFFER_TOO_SMALL";
    default:
        return NULL;
    }
}

const char *appliance_production_management_failure_string(
    enum appliance_production_management_failure failure)
{
    switch (failure) {
    case APPLIANCE_PRODUCTION_MANAGEMENT_FAILURE_NONE:
        return "NONE";
    case APPLIANCE_PRODUCTION_MANAGEMENT_FAILURE_EVIDENCE:
        return "EVIDENCE";
    case APPLIANCE_PRODUCTION_MANAGEMENT_FAILURE_STATE:
        return "STATE";
    case APPLIANCE_PRODUCTION_MANAGEMENT_FAILURE_NETWORK:
        return "NETWORK";
    case APPLIANCE_PRODUCTION_MANAGEMENT_FAILURE_SERVICES:
        return "SERVICES";
    case APPLIANCE_PRODUCTION_MANAGEMENT_FAILURE_HTTPS:
        return "HTTPS";
    case APPLIANCE_PRODUCTION_MANAGEMENT_FAILURE_STOP:
        return "STOP";
    default:
        return NULL;
    }
}
