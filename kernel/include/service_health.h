#ifndef HFOC_SERVICE_HEALTH_H
#define HFOC_SERVICE_HEALTH_H

#include "health_check.h"
#include "service_supervisor.h"

#include <stdbool.h>
#include <stdint.h>

#define SERVICE_HEALTH_VERSION 1U

enum service_health_status {
    SERVICE_HEALTH_OK = 0,
    SERVICE_HEALTH_NULL = 1,
    SERVICE_HEALTH_BAD_VERSION = 2,
    SERVICE_HEALTH_BAD_STATE = 3,
};

enum service_health_fault {
    SERVICE_HEALTH_FAULT_NONE = 0,
    SERVICE_HEALTH_FAULT_READINESS = 1,
    SERVICE_HEALTH_FAULT_RESTART = 2,
    SERVICE_HEALTH_FAULT_DEPENDENCY = 3,
};

struct service_health_input {
    uint32_t version;
    const struct service_supervisor_snapshot *snapshot;
};

struct service_health_verdict {
    uint32_t version;
    bool healthy;
    enum health_check_result_state result;
    enum health_check_severity severity;
    enum service_health_fault fault;
    uint32_t service_count;
    uint32_t ready_count;
    uint32_t pending_count;
    uint32_t failed_count;
    uint32_t exhausted_count;
    uint32_t dependency_blocked_count;
    uint32_t worst_service_id;
};

enum service_health_status service_health_evaluate(
    const struct service_health_input *input,
    struct service_health_verdict *verdict_out);
const char *service_health_status_string(
    enum service_health_status status);
const char *service_health_fault_string(
    enum service_health_fault fault);

#endif
