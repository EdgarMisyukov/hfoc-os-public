#include "service_health.h"

#include "service.h"
#include "service_supervisor.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

static void service_health_zero_bytes(unsigned char *bytes,
                                      size_t count)
{
    size_t index;

    for (index = 0U; index < count; ++index) {
        bytes[index] = 0U;
    }
}

static unsigned int service_health_order(
    enum health_check_result_state result)
{
    switch (result) {
    case HEALTH_CHECK_RESULT_FAIL:
        return 4U;
    case HEALTH_CHECK_RESULT_WARN:
        return 3U;
    case HEALTH_CHECK_RESULT_UNKNOWN:
        return 2U;
    case HEALTH_CHECK_RESULT_PASS:
        return 1U;
    default:
        return 0U;
    }
}

static bool service_health_state_valid(
    enum service_supervisor_state state)
{
    return state >= SERVICE_SUPERVISOR_STATE_RUNNING &&
           state <= SERVICE_SUPERVISOR_STATE_EXHAUSTED;
}

static bool service_health_health_state_valid(
    enum service_health_state health_state)
{
    return health_state >= SERVICE_HEALTH_PENDING &&
           health_state <= SERVICE_HEALTH_READINESS_TIMEOUT;
}

static bool service_health_snapshot_valid(
    const struct service_supervisor_snapshot *snapshot)
{
    uint32_t index;

    if (snapshot == NULL ||
        !snapshot->published ||
        snapshot->version != SERVICE_SUPERVISOR_SNAPSHOT_VERSION ||
        snapshot->entry_count > SERVICE_SUPERVISOR_CAPACITY ||
        snapshot->active_count != snapshot->entry_count) {
        return false;
    }

    for (index = 0U; index < snapshot->entry_count; ++index) {
        const struct service_supervisor_snapshot_entry *entry =
            &snapshot->entries[index];

        if (service_descriptor_validate(&entry->descriptor) !=
                SERVICE_DESCRIPTOR_OK ||
            !entry->descriptor.enabled ||
            !service_health_state_valid(entry->state) ||
            entry->state == SERVICE_SUPERVISOR_STATE_STOPPED ||
            !service_health_health_state_valid(entry->health_state)) {
            return false;
        }
        if (index > 0U &&
            entry->descriptor.id <=
                snapshot->entries[index - 1U].descriptor.id) {
            return false;
        }
    }
    return true;
}

static const struct service_supervisor_snapshot_entry *
service_health_find(
    const struct service_supervisor_snapshot *snapshot,
    uint32_t service_id)
{
    uint32_t index;

    for (index = 0U; index < snapshot->entry_count; ++index) {
        if (snapshot->entries[index].descriptor.id == service_id) {
            return &snapshot->entries[index];
        }
    }
    return NULL;
}

static bool service_health_dependency_blocked(
    const struct service_supervisor_snapshot *snapshot,
    const struct service_supervisor_snapshot_entry *entry,
    const enum health_check_result_state *own_results,
    bool *blocked)
{
    uint32_t dependency_position;

    *blocked = false;
    if (entry->descriptor.dependency_count == 0U) {
        return true;
    }
    for (dependency_position = 0U;
         dependency_position < entry->descriptor.dependency_count;
         ++dependency_position) {
        const uint32_t dependency_id =
            entry->descriptor.dependencies[dependency_position];
        const struct service_supervisor_snapshot_entry *dependency =
            service_health_find(snapshot, dependency_id);

        if (dependency == NULL ||
            !dependency->descriptor.enabled ||
            own_results[dependency->descriptor.id] !=
                HEALTH_CHECK_RESULT_PASS) {
            *blocked = true;
        }
    }
    return true;
}

static bool service_health_entry_result(
    const struct service_supervisor_snapshot_entry *entry,
    enum health_check_result_state *result,
    enum service_health_fault *fault,
    bool *restart_exhausted)
{
    *fault = SERVICE_HEALTH_FAULT_NONE;
    *restart_exhausted = false;

    switch (entry->state) {
    case SERVICE_SUPERVISOR_STATE_RUNNING:
        switch (entry->health_state) {
        case SERVICE_HEALTH_READY:
            *result = HEALTH_CHECK_RESULT_PASS;
            break;
        case SERVICE_HEALTH_PENDING:
            *result = HEALTH_CHECK_RESULT_UNKNOWN;
            break;
        case SERVICE_HEALTH_LIVENESS_FAILED:
        case SERVICE_HEALTH_READINESS_TIMEOUT:
            *result = HEALTH_CHECK_RESULT_FAIL;
            *fault = SERVICE_HEALTH_FAULT_READINESS;
            break;
        default:
            return false;
        }
        break;
    case SERVICE_SUPERVISOR_STATE_EXITED:
        *result = HEALTH_CHECK_RESULT_FAIL;
        *fault = SERVICE_HEALTH_FAULT_READINESS;
        break;
    case SERVICE_SUPERVISOR_STATE_EXHAUSTED:
        *result = HEALTH_CHECK_RESULT_FAIL;
        *fault = SERVICE_HEALTH_FAULT_RESTART;
        *restart_exhausted = true;
        break;
    default:
        return false;
    }

    if (entry->descriptor.restart_limit > 0U &&
        entry->restart_count >= entry->descriptor.restart_limit) {
        *result = HEALTH_CHECK_RESULT_FAIL;
        *fault = SERVICE_HEALTH_FAULT_RESTART;
        *restart_exhausted = true;
    }
    return true;
}

enum service_health_status service_health_evaluate(
    const struct service_health_input *input,
    struct service_health_verdict *verdict_out)
{
    enum health_check_result_state own_results[
        SERVICE_ID_MAX + 1U];
    bool readiness_fault = false;
    enum health_check_result_state worst_result =
        HEALTH_CHECK_RESULT_PASS;
    uint32_t index;

    if (input == NULL || verdict_out == NULL) {
        return SERVICE_HEALTH_NULL;
    }
    if (input->version != SERVICE_HEALTH_VERSION) {
        return SERVICE_HEALTH_BAD_VERSION;
    }
    if (!service_health_snapshot_valid(input->snapshot)) {
        return SERVICE_HEALTH_BAD_STATE;
    }

    service_health_zero_bytes(
        (unsigned char *)verdict_out, sizeof(*verdict_out));
    verdict_out->version = SERVICE_HEALTH_VERSION;
    verdict_out->result = HEALTH_CHECK_RESULT_PASS;
    verdict_out->severity = HEALTH_CHECK_SEVERITY_INFO;
    verdict_out->fault = SERVICE_HEALTH_FAULT_NONE;

    for (index = 0U; index <= SERVICE_ID_MAX; ++index) {
        own_results[index] = HEALTH_CHECK_RESULT_PASS;
    }
    for (index = 0U; index < input->snapshot->entry_count; ++index) {
        const struct service_supervisor_snapshot_entry *entry =
            &input->snapshot->entries[index];
        enum health_check_result_state result;
        enum service_health_fault fault;
        bool restart_exhausted;

        if (!service_health_entry_result(
                entry, &result, &fault, &restart_exhausted)) {
            return SERVICE_HEALTH_BAD_STATE;
        }
        own_results[entry->descriptor.id] = result;
        ++verdict_out->service_count;
        if (result == HEALTH_CHECK_RESULT_PASS) {
            ++verdict_out->ready_count;
        } else if (result == HEALTH_CHECK_RESULT_UNKNOWN) {
            ++verdict_out->pending_count;
        } else {
            ++verdict_out->failed_count;
        }
        if (restart_exhausted ||
            entry->state == SERVICE_SUPERVISOR_STATE_EXHAUSTED) {
            ++verdict_out->exhausted_count;
        }
        if (fault == SERVICE_HEALTH_FAULT_READINESS) {
            readiness_fault = true;
        }
        if (service_health_order(result) >
                service_health_order(worst_result) ||
            (result == HEALTH_CHECK_RESULT_PASS &&
             verdict_out->worst_service_id == 0U)) {
            worst_result = result;
            verdict_out->worst_service_id = entry->descriptor.id;
        }
    }

    for (index = 0U;
         index < input->snapshot->entry_count;
         ++index) {
        uint32_t pass;

        for (pass = 0U;
             pass < input->snapshot->entry_count;
             ++pass) {
            const struct service_supervisor_snapshot_entry *entry =
                &input->snapshot->entries[pass];
            bool blocked;

            (void)service_health_dependency_blocked(
                input->snapshot, entry, own_results, &blocked);
            if (blocked &&
                own_results[entry->descriptor.id] ==
                    HEALTH_CHECK_RESULT_PASS) {
                own_results[entry->descriptor.id] =
                    HEALTH_CHECK_RESULT_WARN;
                if (verdict_out->ready_count > 0U) {
                    --verdict_out->ready_count;
                }
            }
        }
    }

    for (index = 0U; index < input->snapshot->entry_count; ++index) {
        const struct service_supervisor_snapshot_entry *entry =
            &input->snapshot->entries[index];

        if (own_results[entry->descriptor.id] ==
            HEALTH_CHECK_RESULT_WARN) {
            ++verdict_out->dependency_blocked_count;
            if (service_health_order(HEALTH_CHECK_RESULT_WARN) >
                service_health_order(worst_result)) {
                worst_result = HEALTH_CHECK_RESULT_WARN;
                verdict_out->worst_service_id = entry->descriptor.id;
            }
        }
    }

    verdict_out->result = worst_result;
    if (worst_result == HEALTH_CHECK_RESULT_FAIL) {
        verdict_out->healthy = false;
        verdict_out->severity = HEALTH_CHECK_SEVERITY_CRITICAL;
        verdict_out->fault = readiness_fault ?
            SERVICE_HEALTH_FAULT_READINESS :
            SERVICE_HEALTH_FAULT_RESTART;
    } else if (worst_result == HEALTH_CHECK_RESULT_WARN) {
        verdict_out->healthy = false;
        verdict_out->severity = HEALTH_CHECK_SEVERITY_WARNING;
        verdict_out->fault = SERVICE_HEALTH_FAULT_DEPENDENCY;
    } else {
        verdict_out->healthy = worst_result == HEALTH_CHECK_RESULT_PASS;
        verdict_out->severity = HEALTH_CHECK_SEVERITY_INFO;
        verdict_out->fault = SERVICE_HEALTH_FAULT_NONE;
    }
    return SERVICE_HEALTH_OK;
}

const char *service_health_status_string(
    enum service_health_status status)
{
    switch (status) {
    case SERVICE_HEALTH_OK:
        return "SERVICE_HEALTH_OK";
    case SERVICE_HEALTH_NULL:
        return "SERVICE_HEALTH_NULL";
    case SERVICE_HEALTH_BAD_VERSION:
        return "SERVICE_HEALTH_BAD_VERSION";
    case SERVICE_HEALTH_BAD_STATE:
        return "SERVICE_HEALTH_BAD_STATE";
    default:
        return NULL;
    }
}

const char *service_health_fault_string(
    enum service_health_fault fault)
{
    switch (fault) {
    case SERVICE_HEALTH_FAULT_NONE:
        return "SERVICE_HEALTH_FAULT_NONE";
    case SERVICE_HEALTH_FAULT_READINESS:
        return "SERVICE_HEALTH_FAULT_READINESS";
    case SERVICE_HEALTH_FAULT_RESTART:
        return "SERVICE_HEALTH_FAULT_RESTART";
    case SERVICE_HEALTH_FAULT_DEPENDENCY:
        return "SERVICE_HEALTH_FAULT_DEPENDENCY";
    default:
        return NULL;
    }
}
