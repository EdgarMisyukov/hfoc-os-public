#include "audit_ring_health.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

static bool audit_ring_health_thresholds_valid(
    const struct audit_ring_health_input *input)
{
    return input->warn_threshold >= 1U &&
           input->fail_threshold >= 1U &&
           input->warn_threshold <= KERNEL_EVENT_RING_CAPACITY &&
           input->fail_threshold <= KERNEL_EVENT_RING_CAPACITY &&
           input->warn_threshold <= input->fail_threshold;
}

static bool audit_ring_health_ring_valid(
    const struct event_ring *ring)
{
    if (ring == NULL ||
        !ring->published ||
        ring->count > KERNEL_EVENT_RING_CAPACITY ||
        ring->write_cursor < ring->read_cursor) {
        return false;
    }
    return ring->write_cursor - ring->read_cursor == ring->count;
}

enum audit_ring_health_status audit_ring_health_evaluate(
    const struct audit_ring_health_input *input,
    struct audit_ring_health_verdict *verdict_out)
{
    const bool ring_full =
        input != NULL && input->ring != NULL &&
        input->ring->count == KERNEL_EVENT_RING_CAPACITY;
    uint32_t count;

    if (input == NULL || verdict_out == NULL) {
        return AUDIT_RING_HEALTH_NULL;
    }
    if (input->version != AUDIT_RING_HEALTH_VERSION) {
        return AUDIT_RING_HEALTH_BAD_VERSION;
    }
    if (input->ring == NULL) {
        return AUDIT_RING_HEALTH_NULL;
    }
    if (!input->ring->published) {
        return AUDIT_RING_HEALTH_BAD_STATE;
    }
    if (!audit_ring_health_thresholds_valid(input)) {
        return AUDIT_RING_HEALTH_BAD_THRESHOLD;
    }
    if (!audit_ring_health_ring_valid(input->ring)) {
        return AUDIT_RING_HEALTH_BAD_STATE;
    }

    count = input->ring->count;
    memset(verdict_out, 0, sizeof(*verdict_out));
    verdict_out->version = AUDIT_RING_HEALTH_VERSION;
    verdict_out->reject_on_full = true;
    verdict_out->overwrite_allowed = false;
    verdict_out->silent_loss_possible = false;
    verdict_out->capacity = KERNEL_EVENT_RING_CAPACITY;
    verdict_out->count = count;
    verdict_out->available = KERNEL_EVENT_RING_CAPACITY - count;
    verdict_out->utilization_percent =
        (count * 100U) / KERNEL_EVENT_RING_CAPACITY;
    verdict_out->write_cursor = input->ring->write_cursor;
    verdict_out->read_cursor = input->ring->read_cursor;
    verdict_out->ring_full = ring_full;

    if (ring_full) {
        verdict_out->result = HEALTH_CHECK_RESULT_FAIL;
        verdict_out->severity = HEALTH_CHECK_SEVERITY_CRITICAL;
        verdict_out->fault = AUDIT_RING_HEALTH_FAULT_FULL;
    } else if (count >= input->fail_threshold) {
        verdict_out->result = HEALTH_CHECK_RESULT_FAIL;
        verdict_out->severity = HEALTH_CHECK_SEVERITY_CRITICAL;
        verdict_out->fault = AUDIT_RING_HEALTH_FAULT_PRESSURE;
    } else if (count >= input->warn_threshold) {
        verdict_out->result = HEALTH_CHECK_RESULT_WARN;
        verdict_out->severity = HEALTH_CHECK_SEVERITY_WARNING;
        verdict_out->fault = AUDIT_RING_HEALTH_FAULT_PRESSURE;
    } else {
        verdict_out->result = HEALTH_CHECK_RESULT_PASS;
        verdict_out->severity = HEALTH_CHECK_SEVERITY_INFO;
        verdict_out->fault = AUDIT_RING_HEALTH_FAULT_NONE;
    }
    verdict_out->healthy = verdict_out->result == HEALTH_CHECK_RESULT_PASS;
    return AUDIT_RING_HEALTH_OK;
}

const char *audit_ring_health_status_string(
    enum audit_ring_health_status status)
{
    switch (status) {
    case AUDIT_RING_HEALTH_OK:
        return "AUDIT_RING_HEALTH_OK";
    case AUDIT_RING_HEALTH_NULL:
        return "AUDIT_RING_HEALTH_NULL";
    case AUDIT_RING_HEALTH_BAD_VERSION:
        return "AUDIT_RING_HEALTH_BAD_VERSION";
    case AUDIT_RING_HEALTH_BAD_THRESHOLD:
        return "AUDIT_RING_HEALTH_BAD_THRESHOLD";
    case AUDIT_RING_HEALTH_BAD_STATE:
        return "AUDIT_RING_HEALTH_BAD_STATE";
    default:
        return NULL;
    }
}

const char *audit_ring_health_fault_string(
    enum audit_ring_health_fault fault)
{
    switch (fault) {
    case AUDIT_RING_HEALTH_FAULT_NONE:
        return "AUDIT_RING_HEALTH_FAULT_NONE";
    case AUDIT_RING_HEALTH_FAULT_PRESSURE:
        return "AUDIT_RING_HEALTH_FAULT_PRESSURE";
    case AUDIT_RING_HEALTH_FAULT_FULL:
        return "AUDIT_RING_HEALTH_FAULT_FULL";
    default:
        return NULL;
    }
}
