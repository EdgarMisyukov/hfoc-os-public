#include "controlled_configuration.h"

#include <stddef.h>
#include <stdint.h>

static enum controlled_configuration_status
controlled_configuration_audit_result(
    struct event_ring *ring,
    const struct audit_correlation *correlation,
    uint32_t result,
    uint64_t *sequence_out)
{
    struct audit_record record;
    enum audit_status audit_status;
    enum audit_correlation_status correlation_status;
    uint64_t sequence = UINT64_MAX;

    if (ring == NULL) {
        return CONTROLLED_CONFIGURATION_NULL;
    }
    if (correlation != NULL) {
        struct audit_correlation effective = *correlation;

        effective.result = result;
        correlation_status = audit_correlation_append(
            ring, CONTROLLED_CONFIGURATION_AUDIT_OPERATION_ACTIVATE,
            &effective, CONTROLLED_CONFIGURATION_COMMAND_ID,
            sequence_out);
        if (correlation_status != AUDIT_CORRELATION_OK) {
            return CONTROLLED_CONFIGURATION_AUDIT_FAILED;
        }
        return CONTROLLED_CONFIGURATION_OK;
    }
    audit_status = audit_record_init(
        &record, AUDIT_RECORD_CAPABILITY,
        CONTROLLED_CONFIGURATION_AUDIT_OPERATION_ACTIVATE,
        result, 0U, 0U, 0U,
        CONTROLLED_CONFIGURATION_COMMAND_ID);
    if (audit_status != AUDIT_OK) {
        return CONTROLLED_CONFIGURATION_AUDIT_FAILED;
    }
    audit_status = audit_record_append(ring, &record, &sequence);
    if (audit_status != AUDIT_OK) {
        return CONTROLLED_CONFIGURATION_AUDIT_FAILED;
    }
    if (sequence_out != NULL) {
        *sequence_out = sequence;
    }
    return CONTROLLED_CONFIGURATION_OK;
}

enum controlled_configuration_status controlled_configuration_apply(
    struct config_snapshot *snapshot,
    const char *input,
    size_t input_length,
    config_commit_validator validator,
    void *context,
    struct event_ring *audit_ring,
    uint64_t *audit_sequence_out)
{
    return controlled_configuration_apply_correlated(
        snapshot, input, input_length, validator, context,
        NULL, audit_ring, audit_sequence_out);
}

enum controlled_configuration_status
controlled_configuration_apply_correlated(
    struct config_snapshot *snapshot,
    const char *input,
    size_t input_length,
    config_commit_validator validator,
    void *context,
    const struct audit_correlation *correlation,
    struct event_ring *audit_ring,
    uint64_t *audit_sequence_out)
{
    enum config_snapshot_status config_status;
    enum controlled_configuration_status control_status;

    if (snapshot == NULL || input == NULL || audit_ring == NULL) {
        return CONTROLLED_CONFIGURATION_NULL;
    }
    if (input_length < CONTROLLED_CONFIGURATION_REQUEST_MIN ||
        input_length > CONTROLLED_CONFIGURATION_REQUEST_MAX) {
        return CONTROLLED_CONFIGURATION_TOO_LONG;
    }
    if (correlation != NULL &&
        audit_correlation_validate(correlation) !=
            AUDIT_CORRELATION_OK) {
        return CONTROLLED_CONFIGURATION_BAD_STATE;
    }
    if (!snapshot->has_active) {
        return CONTROLLED_CONFIGURATION_BAD_STATE;
    }
    if (!event_ring_is_published(audit_ring)) {
        return CONTROLLED_CONFIGURATION_BAD_STATE;
    }
    if (event_ring_count(audit_ring) >= KERNEL_EVENT_RING_CAPACITY) {
        return CONTROLLED_CONFIGURATION_AUDIT_FAILED;
    }

    config_status = config_snapshot_prepare(
        snapshot, input, input_length);
    if (config_status != CONFIG_SNAPSHOT_OK) {
        if (config_status == CONFIG_SNAPSHOT_NULL) {
            return CONTROLLED_CONFIGURATION_NULL;
        }
        if (config_status == CONFIG_SNAPSHOT_BAD_STATE) {
            return CONTROLLED_CONFIGURATION_BAD_STATE;
        }
        control_status = controlled_configuration_audit_result(
            audit_ring, correlation,
            CONTROLLED_CONFIGURATION_AUDIT_RESULT_FAILED,
            audit_sequence_out);
        if (control_status != CONTROLLED_CONFIGURATION_OK) {
            return control_status;
        }
        return CONTROLLED_CONFIGURATION_PARSE_FAILED;
    }

    config_status = config_snapshot_commit(
        snapshot, validator, context);
    if (config_status != CONFIG_SNAPSHOT_OK) {
        if (config_status == CONFIG_SNAPSHOT_NULL) {
            return CONTROLLED_CONFIGURATION_NULL;
        }
        if (config_status == CONFIG_SNAPSHOT_BAD_STATE) {
            return CONTROLLED_CONFIGURATION_BAD_STATE;
        }
        control_status = controlled_configuration_audit_result(
            audit_ring, correlation,
            CONTROLLED_CONFIGURATION_AUDIT_RESULT_FAILED,
            audit_sequence_out);
        if (control_status != CONTROLLED_CONFIGURATION_OK) {
            return control_status;
        }
        return CONTROLLED_CONFIGURATION_COMMIT_FAILED;
    }

    control_status = controlled_configuration_audit_result(
        audit_ring, correlation,
        CONTROLLED_CONFIGURATION_AUDIT_RESULT_OK,
        audit_sequence_out);
    if (control_status != CONTROLLED_CONFIGURATION_OK) {
        return control_status;
    }
    return CONTROLLED_CONFIGURATION_OK;
}

enum controlled_configuration_status
controlled_configuration_command_descriptor(
    struct management_command_descriptor *descriptor)
{
    if (descriptor == NULL) {
        return CONTROLLED_CONFIGURATION_NULL;
    }
    descriptor->version = MANAGEMENT_COMMAND_VERSION;
    descriptor->command_id = CONTROLLED_CONFIGURATION_COMMAND_ID;
    descriptor->request_min_length =
        CONTROLLED_CONFIGURATION_REQUEST_MIN;
    descriptor->request_max_length =
        CONTROLLED_CONFIGURATION_REQUEST_MAX;
    descriptor->response_min_length =
        CONTROLLED_CONFIGURATION_RESPONSE_MIN;
    descriptor->response_max_length =
        CONTROLLED_CONFIGURATION_RESPONSE_MAX;
    return CONTROLLED_CONFIGURATION_OK;
}

enum controlled_configuration_status
controlled_configuration_authorization_entry(
    struct authorization_entry *entry)
{
    uint32_t index;

    if (entry == NULL) {
        return CONTROLLED_CONFIGURATION_NULL;
    }
    for (index = 0U;
         index < AUTHORIZATION_ROLE_SET_MAX; ++index) {
        entry->roles[index] = 0U;
    }
    for (index = 0U;
         index < AUTHORIZATION_CAPABILITY_SET_MAX; ++index) {
        entry->capabilities[index] = 0U;
    }
    entry->version = AUTHORIZATION_VERSION;
    entry->command_id = CONTROLLED_CONFIGURATION_COMMAND_ID;
    entry->role_count = 1U;
    entry->roles[0] = AUTHORIZATION_ROLE_ADMIN;
    entry->capability_count = 1U;
    entry->capabilities[0] =
        AUTHORIZATION_CAPABILITY_CONFIG_WRITE;
    return CONTROLLED_CONFIGURATION_OK;
}

const char *controlled_configuration_status_string(
    enum controlled_configuration_status status)
{
    switch (status) {
    case CONTROLLED_CONFIGURATION_OK:
        return "CONTROLLED_CONFIGURATION_OK";
    case CONTROLLED_CONFIGURATION_NULL:
        return "CONTROLLED_CONFIGURATION_NULL";
    case CONTROLLED_CONFIGURATION_BAD_STATE:
        return "CONTROLLED_CONFIGURATION_BAD_STATE";
    case CONTROLLED_CONFIGURATION_TOO_LONG:
        return "CONTROLLED_CONFIGURATION_TOO_LONG";
    case CONTROLLED_CONFIGURATION_PARSE_FAILED:
        return "CONTROLLED_CONFIGURATION_PARSE_FAILED";
    case CONTROLLED_CONFIGURATION_COMMIT_FAILED:
        return "CONTROLLED_CONFIGURATION_COMMIT_FAILED";
    case CONTROLLED_CONFIGURATION_AUDIT_FAILED:
        return "CONTROLLED_CONFIGURATION_AUDIT_FAILED";
    default:
        return NULL;
    }
}
