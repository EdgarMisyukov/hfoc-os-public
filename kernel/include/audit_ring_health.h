#ifndef HFOC_AUDIT_RING_HEALTH_H
#define HFOC_AUDIT_RING_HEALTH_H

#include "event_ring.h"
#include "health_check.h"

#include <stdbool.h>
#include <stdint.h>

#define AUDIT_RING_HEALTH_VERSION 1U

enum audit_ring_health_status {
    AUDIT_RING_HEALTH_OK = 0,
    AUDIT_RING_HEALTH_NULL = 1,
    AUDIT_RING_HEALTH_BAD_VERSION = 2,
    AUDIT_RING_HEALTH_BAD_THRESHOLD = 3,
    AUDIT_RING_HEALTH_BAD_STATE = 4,
};

enum audit_ring_health_fault {
    AUDIT_RING_HEALTH_FAULT_NONE = 0,
    AUDIT_RING_HEALTH_FAULT_PRESSURE = 1,
    AUDIT_RING_HEALTH_FAULT_FULL = 2,
};

struct audit_ring_health_input {
    uint32_t version;
    const struct event_ring *ring;
    uint32_t warn_threshold;
    uint32_t fail_threshold;
};

struct audit_ring_health_verdict {
    uint32_t version;
    bool healthy;
    bool ring_full;
    bool reject_on_full;
    bool overwrite_allowed;
    bool silent_loss_possible;
    enum health_check_result_state result;
    enum health_check_severity severity;
    enum audit_ring_health_fault fault;
    uint32_t capacity;
    uint32_t count;
    uint32_t available;
    uint32_t utilization_percent;
    uint64_t write_cursor;
    uint64_t read_cursor;
};

enum audit_ring_health_status audit_ring_health_evaluate(
    const struct audit_ring_health_input *input,
    struct audit_ring_health_verdict *verdict_out);
const char *audit_ring_health_status_string(
    enum audit_ring_health_status status);
const char *audit_ring_health_fault_string(
    enum audit_ring_health_fault fault);

#endif
