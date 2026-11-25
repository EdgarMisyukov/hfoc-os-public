#include "audit_correlation.h"

#include <stddef.h>
#include <stdint.h>

enum audit_correlation_status audit_correlation_validate(
    const struct audit_correlation *correlation)
{
    if (correlation == NULL) {
        return AUDIT_CORRELATION_NULL;
    }
    if (correlation->version != AUDIT_CORRELATION_VERSION) {
        return AUDIT_CORRELATION_BAD_VERSION;
    }
    if (correlation->session_id < AUDIT_CORRELATION_ID_MIN ||
        correlation->session_id > AUDIT_CORRELATION_ID_MAX ||
        correlation->principal_id < AUDIT_CORRELATION_ID_MIN ||
        correlation->principal_id > AUDIT_CORRELATION_ID_MAX ||
        correlation->command_id < AUDIT_CORRELATION_ID_MIN ||
        correlation->command_id > AUDIT_CORRELATION_ID_MAX) {
        return AUDIT_CORRELATION_INVALID_ID;
    }
    if (correlation->result != AUDIT_CORRELATION_RESULT_OK &&
        correlation->result != AUDIT_CORRELATION_RESULT_DENIED) {
        return AUDIT_CORRELATION_INVALID_RESULT;
    }
    return AUDIT_CORRELATION_OK;
}

enum audit_correlation_status audit_correlation_append(
    struct event_ring *ring,
    uint32_t operation,
    const struct audit_correlation *correlation,
    uint64_t detail,
    uint64_t *sequence_out)
{
    struct audit_record record;
    enum audit_correlation_status validation_status;
    enum audit_status audit_status;

    if (ring == NULL || correlation == NULL) {
        return AUDIT_CORRELATION_NULL;
    }
    validation_status = audit_correlation_validate(correlation);
    if (validation_status != AUDIT_CORRELATION_OK) {
        return validation_status;
    }
    if (operation < AUDIT_CORRELATION_OPERATION_MIN ||
        operation > AUDIT_CORRELATION_OPERATION_MAX) {
        return AUDIT_CORRELATION_INVALID_OPERATION;
    }
    if (!event_ring_is_published(ring)) {
        return AUDIT_CORRELATION_BAD_STATE;
    }
    if (event_ring_count(ring) >= KERNEL_EVENT_RING_CAPACITY) {
        return AUDIT_CORRELATION_AUDIT_FAILED;
    }

    audit_status = audit_record_init_management(
        &record, operation, correlation->result,
        correlation->session_id, correlation->principal_id,
        correlation->command_id, detail);
    if (audit_status != AUDIT_OK) {
        return AUDIT_CORRELATION_AUDIT_FAILED;
    }
    audit_status = audit_record_append(ring, &record, sequence_out);
    if (audit_status != AUDIT_OK) {
        if (audit_status == AUDIT_BAD_STATE) {
            return AUDIT_CORRELATION_BAD_STATE;
        }
        return AUDIT_CORRELATION_AUDIT_FAILED;
    }
    return AUDIT_CORRELATION_OK;
}

const char *audit_correlation_status_string(
    enum audit_correlation_status status)
{
    switch (status) {
    case AUDIT_CORRELATION_OK:
        return "AUDIT_CORRELATION_OK";
    case AUDIT_CORRELATION_NULL:
        return "AUDIT_CORRELATION_NULL";
    case AUDIT_CORRELATION_BAD_VERSION:
        return "AUDIT_CORRELATION_BAD_VERSION";
    case AUDIT_CORRELATION_INVALID_ID:
        return "AUDIT_CORRELATION_INVALID_ID";
    case AUDIT_CORRELATION_INVALID_OPERATION:
        return "AUDIT_CORRELATION_INVALID_OPERATION";
    case AUDIT_CORRELATION_INVALID_RESULT:
        return "AUDIT_CORRELATION_INVALID_RESULT";
    case AUDIT_CORRELATION_BAD_STATE:
        return "AUDIT_CORRELATION_BAD_STATE";
    case AUDIT_CORRELATION_AUDIT_FAILED:
        return "AUDIT_CORRELATION_AUDIT_FAILED";
    default:
        return NULL;
    }
}
