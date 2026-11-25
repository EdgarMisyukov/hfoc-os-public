#include "appliance_service_manager.h"

#include "service.h"
#include "service_health.h"
#include "service_supervisor.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

static bool appliance_service_manager_state_valid(
    enum appliance_service_manager_state state)
{
    return state == APPLIANCE_SERVICE_MANAGER_STATE_STOPPED ||
           state == APPLIANCE_SERVICE_MANAGER_STATE_RUNNING ||
           state == APPLIANCE_SERVICE_MANAGER_STATE_START_FAILED ||
           state == APPLIANCE_SERVICE_MANAGER_STATE_STOP_FAILED;
}

static bool appliance_service_manager_policy_valid(
    enum appliance_service_manager_startup_policy policy)
{
    return policy == APPLIANCE_SERVICE_MANAGER_STARTUP_ROLLBACK ||
           policy == APPLIANCE_SERVICE_MANAGER_STARTUP_PRESERVE;
}

static bool appliance_service_manager_entries_valid(
    const struct service_descriptor *descriptors,
    uint32_t descriptor_count,
    const struct service_supervisor_entry *entries,
    uint32_t entry_count)
{
    uint32_t enabled_count = 0U;
    uint32_t index;
    uint32_t previous;

    if (descriptor_count > APPLIANCE_SERVICE_MANAGER_CAPACITY ||
        entry_count > APPLIANCE_SERVICE_MANAGER_CAPACITY) {
        return false;
    }
    if (entry_count > 0U && entries == NULL) {
        return false;
    }
    for (index = 0U; index < descriptor_count; ++index) {
        if (descriptors[index].enabled) {
            ++enabled_count;
        }
    }
    if (enabled_count != entry_count) {
        return false;
    }
    for (index = 0U; index < entry_count; ++index) {
        bool matched = false;
        uint32_t descriptor_index;

        if (entries[index].service_id < SERVICE_ID_MIN ||
            entries[index].service_id > SERVICE_ID_MAX ||
            entries[index].entry == NULL) {
            return false;
        }
        for (previous = 0U; previous < index; ++previous) {
            if (entries[index].service_id ==
                entries[previous].service_id) {
                return false;
            }
        }
        for (descriptor_index = 0U;
             descriptor_index < descriptor_count;
             ++descriptor_index) {
            if (descriptors[descriptor_index].id ==
                    entries[index].service_id &&
                descriptors[descriptor_index].enabled) {
                matched = true;
                break;
            }
        }
        if (!matched) {
            return false;
        }
    }
    for (index = 0U; index < descriptor_count; ++index) {
        bool matched = false;
        uint32_t entry_index;

        if (!descriptors[index].enabled) {
            continue;
        }
        for (entry_index = 0U;
             entry_index < entry_count;
             ++entry_index) {
            if (entries[entry_index].service_id ==
                descriptors[index].id) {
                matched = true;
                break;
            }
        }
        if (!matched) {
            return false;
        }
    }
    return true;
}

enum appliance_service_manager_status appliance_service_manager_init(
    struct appliance_service_manager *manager,
    const struct appliance_service_manager_ops *ops,
    enum appliance_service_manager_startup_policy startup_policy)
{
    if (manager == NULL || ops == NULL) {
        return APPLIANCE_SERVICE_MANAGER_NULL;
    }
    if (manager->initialized) {
        return APPLIANCE_SERVICE_MANAGER_BAD_STATE;
    }
    if (ops->start_ordered == NULL ||
        ops->stop_ordered == NULL ||
        ops->restart_contained == NULL ||
        ops->snapshot == NULL ||
        !appliance_service_manager_policy_valid(startup_policy)) {
        return APPLIANCE_SERVICE_MANAGER_BAD_STATE;
    }

    memset(manager, 0, sizeof(*manager));
    manager->initialized = true;
    manager->version = APPLIANCE_SERVICE_MANAGER_VERSION;
    manager->state = APPLIANCE_SERVICE_MANAGER_STATE_STOPPED;
    manager->startup_policy = startup_policy;
    manager->ops = *ops;
    return APPLIANCE_SERVICE_MANAGER_OK;
}

enum appliance_service_manager_status appliance_service_manager_configure(
    struct appliance_service_manager *manager,
    const struct service_descriptor *descriptors,
    uint32_t descriptor_count,
    const struct service_supervisor_entry *entries,
    uint32_t entry_count)
{
    struct service_graph_snapshot candidate_graph;
    enum service_dependency_status dependency_status;
    enum service_graph_status graph_status;

    if (manager == NULL ||
        (descriptor_count > 0U && descriptors == NULL) ||
        (entry_count > 0U && entries == NULL)) {
        return APPLIANCE_SERVICE_MANAGER_NULL;
    }
    if (!manager->initialized) {
        return APPLIANCE_SERVICE_MANAGER_BAD_STATE;
    }
    if (manager->state != APPLIANCE_SERVICE_MANAGER_STATE_STOPPED) {
        return APPLIANCE_SERVICE_MANAGER_BAD_STATE;
    }
    dependency_status = service_dependency_set_validate(
        descriptors, descriptor_count);
    if (dependency_status != SERVICE_DEPENDENCY_OK) {
        return APPLIANCE_SERVICE_MANAGER_INVALID_DESCRIPTOR_SET;
    }
    if (!appliance_service_manager_entries_valid(
            descriptors, descriptor_count, entries, entry_count)) {
        return APPLIANCE_SERVICE_MANAGER_INVALID_ENTRIES;
    }

    memset(&candidate_graph, 0, sizeof(candidate_graph));
    graph_status = service_graph_snapshot_build(
        &candidate_graph, descriptors, descriptor_count);
    if (graph_status != SERVICE_GRAPH_OK) {
        return APPLIANCE_SERVICE_MANAGER_INVALID_DESCRIPTOR_SET;
    }

    manager->graph = candidate_graph;
    manager->entry_count = entry_count;
    if (entry_count > 0U) {
        memcpy(manager->entries, entries,
               entry_count * sizeof(manager->entries[0]));
    }
    return APPLIANCE_SERVICE_MANAGER_OK;
}

enum appliance_service_manager_status appliance_service_manager_start(
    struct appliance_service_manager *manager)
{
    enum service_supervisor_status supervisor_status;

    if (manager == NULL) {
        return APPLIANCE_SERVICE_MANAGER_NULL;
    }
    if (!manager->initialized) {
        return APPLIANCE_SERVICE_MANAGER_BAD_STATE;
    }
    if (!manager->graph.published) {
        return APPLIANCE_SERVICE_MANAGER_NOT_CONFIGURED;
    }
    if (manager->state != APPLIANCE_SERVICE_MANAGER_STATE_STOPPED) {
        return APPLIANCE_SERVICE_MANAGER_BAD_STATE;
    }

    supervisor_status = manager->ops.start_ordered(
        manager->ops.context, &manager->graph,
        manager->entries, manager->entry_count);
    if (supervisor_status == SERVICE_SUPERVISOR_OK) {
        if (manager->generation < UINT64_MAX) {
            ++manager->generation;
        }
        manager->state = APPLIANCE_SERVICE_MANAGER_STATE_RUNNING;
        return APPLIANCE_SERVICE_MANAGER_OK;
    }

    if (manager->startup_policy ==
        APPLIANCE_SERVICE_MANAGER_STARTUP_ROLLBACK) {
        supervisor_status = manager->ops.stop_ordered(
            manager->ops.context, &manager->graph);
        if (supervisor_status != SERVICE_SUPERVISOR_OK) {
            manager->state = APPLIANCE_SERVICE_MANAGER_STATE_STOP_FAILED;
            return APPLIANCE_SERVICE_MANAGER_STOP_FAILED;
        }
        manager->state = APPLIANCE_SERVICE_MANAGER_STATE_STOPPED;
        return APPLIANCE_SERVICE_MANAGER_START_FAILED;
    }

    manager->state = APPLIANCE_SERVICE_MANAGER_STATE_START_FAILED;
    return APPLIANCE_SERVICE_MANAGER_START_FAILED;
}

enum appliance_service_manager_status appliance_service_manager_stop(
    struct appliance_service_manager *manager)
{
    enum service_supervisor_status supervisor_status;

    if (manager == NULL) {
        return APPLIANCE_SERVICE_MANAGER_NULL;
    }
    if (!manager->initialized) {
        return APPLIANCE_SERVICE_MANAGER_BAD_STATE;
    }
    if (manager->state == APPLIANCE_SERVICE_MANAGER_STATE_STOPPED) {
        return APPLIANCE_SERVICE_MANAGER_OK;
    }
    if (manager->state != APPLIANCE_SERVICE_MANAGER_STATE_RUNNING &&
        manager->state != APPLIANCE_SERVICE_MANAGER_STATE_START_FAILED &&
        manager->state != APPLIANCE_SERVICE_MANAGER_STATE_STOP_FAILED) {
        return APPLIANCE_SERVICE_MANAGER_BAD_STATE;
    }

    supervisor_status = manager->ops.stop_ordered(
        manager->ops.context, &manager->graph);
    if (supervisor_status != SERVICE_SUPERVISOR_OK) {
        manager->state = APPLIANCE_SERVICE_MANAGER_STATE_STOP_FAILED;
        return APPLIANCE_SERVICE_MANAGER_STOP_FAILED;
    }
    manager->state = APPLIANCE_SERVICE_MANAGER_STATE_STOPPED;
    return APPLIANCE_SERVICE_MANAGER_OK;
}

enum appliance_service_manager_status appliance_service_manager_restart(
    struct appliance_service_manager *manager,
    uint32_t service_id,
    uint64_t now_ticks,
    uint32_t *exit_status_out,
    bool *restarted_out)
{
    enum service_supervisor_status supervisor_status;

    if (manager == NULL || exit_status_out == NULL ||
        restarted_out == NULL) {
        return APPLIANCE_SERVICE_MANAGER_NULL;
    }
    if (!manager->initialized) {
        return APPLIANCE_SERVICE_MANAGER_BAD_STATE;
    }
    if (!manager->graph.published) {
        return APPLIANCE_SERVICE_MANAGER_NOT_CONFIGURED;
    }
    if (manager->state != APPLIANCE_SERVICE_MANAGER_STATE_RUNNING &&
        manager->state != APPLIANCE_SERVICE_MANAGER_STATE_START_FAILED) {
        return APPLIANCE_SERVICE_MANAGER_BAD_STATE;
    }

    *restarted_out = false;
    supervisor_status = manager->ops.restart_contained(
        manager->ops.context, service_id, now_ticks, &manager->graph,
        exit_status_out, restarted_out);
    if (supervisor_status != SERVICE_SUPERVISOR_OK) {
        return APPLIANCE_SERVICE_MANAGER_RESTART_FAILED;
    }
    if (!*restarted_out) {
        return APPLIANCE_SERVICE_MANAGER_RESTART_DISABLED;
    }
    return APPLIANCE_SERVICE_MANAGER_OK;
}

enum appliance_service_manager_status appliance_service_manager_snapshot(
    const struct appliance_service_manager *manager,
    struct appliance_service_manager_snapshot *snapshot_out)
{
    struct service_supervisor_snapshot supervisor_snapshot;
    struct service_health_input health_input;
    struct service_health_verdict health_verdict;
    enum service_supervisor_status supervisor_status;
    enum service_health_status health_status;

    if (manager == NULL || snapshot_out == NULL) {
        return APPLIANCE_SERVICE_MANAGER_NULL;
    }
    if (!manager->initialized) {
        return APPLIANCE_SERVICE_MANAGER_BAD_STATE;
    }
    if (!manager->graph.published) {
        return APPLIANCE_SERVICE_MANAGER_NOT_CONFIGURED;
    }

    memset(&supervisor_snapshot, 0, sizeof(supervisor_snapshot));
    supervisor_status = manager->ops.snapshot(
        manager->ops.context, &supervisor_snapshot);
    if (supervisor_status != SERVICE_SUPERVISOR_OK) {
        return APPLIANCE_SERVICE_MANAGER_SNAPSHOT_FAILED;
    }

    memset(&health_input, 0, sizeof(health_input));
    health_input.version = SERVICE_HEALTH_VERSION;
    health_input.snapshot = &supervisor_snapshot;
    memset(&health_verdict, 0, sizeof(health_verdict));
    health_status = service_health_evaluate(
        &health_input, &health_verdict);
    if (health_status != SERVICE_HEALTH_OK) {
        return APPLIANCE_SERVICE_MANAGER_HEALTH_FAILED;
    }

    memset(snapshot_out, 0, sizeof(*snapshot_out));
    snapshot_out->version = APPLIANCE_SERVICE_MANAGER_VERSION;
    snapshot_out->published = true;
    snapshot_out->state = manager->state;
    snapshot_out->startup_policy = manager->startup_policy;
    snapshot_out->generation = manager->generation;
    snapshot_out->configured_count =
        service_graph_snapshot_count(&manager->graph);
    snapshot_out->active_count = health_verdict.service_count;
    snapshot_out->ready_count = health_verdict.ready_count;
    snapshot_out->pending_count = health_verdict.pending_count;
    snapshot_out->failed_count = health_verdict.failed_count;
    snapshot_out->exhausted_count = health_verdict.exhausted_count;
    snapshot_out->health_result = health_verdict.result;
    snapshot_out->health_fault = health_verdict.fault;
    return APPLIANCE_SERVICE_MANAGER_OK;
}

static bool appliance_service_manager_append_bytes(
    char *output,
    size_t *offset,
    const char *text,
    size_t length)
{
    size_t index;

    if (*offset > APPLIANCE_SERVICE_MANAGER_OUTPUT_MAX ||
        length > APPLIANCE_SERVICE_MANAGER_OUTPUT_MAX - *offset) {
        return false;
    }
    for (index = 0U; index < length; ++index) {
        output[*offset + index] = text[index];
    }
    *offset += length;
    return true;
}

static bool appliance_service_manager_append_text(
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
    return appliance_service_manager_append_bytes(
        output, offset, text, length);
}

static bool appliance_service_manager_append_char(
    char *output,
    size_t *offset,
    char value)
{
    return appliance_service_manager_append_bytes(
        output, offset, &value, 1U);
}

static bool appliance_service_manager_append_u64(
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
        if (!appliance_service_manager_append_char(
                output, offset, digits[count])) {
            return false;
        }
    }
    return true;
}

static const char *appliance_service_manager_health_string(
    enum health_check_result_state result)
{
    switch (result) {
    case HEALTH_CHECK_RESULT_UNKNOWN:
        return "UNKNOWN";
    case HEALTH_CHECK_RESULT_PASS:
        return "PASS";
    case HEALTH_CHECK_RESULT_WARN:
        return "WARN";
    case HEALTH_CHECK_RESULT_FAIL:
        return "FAIL";
    default:
        return NULL;
    }
}

enum appliance_service_manager_status appliance_service_manager_render(
    const struct appliance_service_manager_snapshot *snapshot,
    char *output,
    size_t capacity,
    size_t *output_length)
{
    char rendered[APPLIANCE_SERVICE_MANAGER_OUTPUT_MAX + 1U];
    size_t offset = 0U;
    size_t index;
    const char *health;

    if (snapshot == NULL || output == NULL || output_length == NULL) {
        return APPLIANCE_SERVICE_MANAGER_NULL;
    }
    if (!snapshot->published ||
        snapshot->version != APPLIANCE_SERVICE_MANAGER_VERSION ||
        !appliance_service_manager_state_valid(snapshot->state) ||
        !appliance_service_manager_policy_valid(
            snapshot->startup_policy)) {
        return APPLIANCE_SERVICE_MANAGER_BAD_STATE;
    }
    health = appliance_service_manager_health_string(
        snapshot->health_result);
    if (health == NULL) {
        return APPLIANCE_SERVICE_MANAGER_BAD_STATE;
    }

    if (!appliance_service_manager_append_text(
            rendered, &offset, "appliance_services v=") ||
        !appliance_service_manager_append_u64(
            rendered, &offset, snapshot->version) ||
        !appliance_service_manager_append_text(
            rendered, &offset, " state=") ||
        !appliance_service_manager_append_text(
            rendered, &offset,
            appliance_service_manager_state_string(snapshot->state)) ||
        !appliance_service_manager_append_text(
            rendered, &offset, " policy=") ||
        !appliance_service_manager_append_text(
            rendered, &offset,
            appliance_service_manager_startup_policy_string(
                snapshot->startup_policy)) ||
        !appliance_service_manager_append_text(
            rendered, &offset, " generation=") ||
        !appliance_service_manager_append_u64(
            rendered, &offset, snapshot->generation) ||
        !appliance_service_manager_append_text(
            rendered, &offset, " configured=") ||
        !appliance_service_manager_append_u64(
            rendered, &offset, snapshot->configured_count) ||
        !appliance_service_manager_append_text(
            rendered, &offset, " active=") ||
        !appliance_service_manager_append_u64(
            rendered, &offset, snapshot->active_count) ||
        !appliance_service_manager_append_text(
            rendered, &offset, " ready=") ||
        !appliance_service_manager_append_u64(
            rendered, &offset, snapshot->ready_count) ||
        !appliance_service_manager_append_text(
            rendered, &offset, " pending=") ||
        !appliance_service_manager_append_u64(
            rendered, &offset, snapshot->pending_count) ||
        !appliance_service_manager_append_text(
            rendered, &offset, " failed=") ||
        !appliance_service_manager_append_u64(
            rendered, &offset, snapshot->failed_count) ||
        !appliance_service_manager_append_text(
            rendered, &offset, " exhausted=") ||
        !appliance_service_manager_append_u64(
            rendered, &offset, snapshot->exhausted_count) ||
        !appliance_service_manager_append_text(
            rendered, &offset, " health=") ||
        !appliance_service_manager_append_text(
            rendered, &offset, health) ||
        !appliance_service_manager_append_char(
            rendered, &offset, '\n')) {
        return APPLIANCE_SERVICE_MANAGER_OVERFLOW;
    }
    if (capacity <= offset) {
        return APPLIANCE_SERVICE_MANAGER_OVERFLOW;
    }
    for (index = 0U; index < offset; ++index) {
        output[index] = rendered[index];
    }
    output[offset] = '\0';
    *output_length = offset;
    return APPLIANCE_SERVICE_MANAGER_OK;
}

const char *appliance_service_manager_status_string(
    enum appliance_service_manager_status status)
{
    switch (status) {
    case APPLIANCE_SERVICE_MANAGER_OK:
        return "APPLIANCE_SERVICE_MANAGER_OK";
    case APPLIANCE_SERVICE_MANAGER_NULL:
        return "APPLIANCE_SERVICE_MANAGER_NULL";
    case APPLIANCE_SERVICE_MANAGER_BAD_STATE:
        return "APPLIANCE_SERVICE_MANAGER_BAD_STATE";
    case APPLIANCE_SERVICE_MANAGER_NOT_CONFIGURED:
        return "APPLIANCE_SERVICE_MANAGER_NOT_CONFIGURED";
    case APPLIANCE_SERVICE_MANAGER_INVALID_DESCRIPTOR_SET:
        return "APPLIANCE_SERVICE_MANAGER_INVALID_DESCRIPTOR_SET";
    case APPLIANCE_SERVICE_MANAGER_INVALID_ENTRIES:
        return "APPLIANCE_SERVICE_MANAGER_INVALID_ENTRIES";
    case APPLIANCE_SERVICE_MANAGER_START_FAILED:
        return "APPLIANCE_SERVICE_MANAGER_START_FAILED";
    case APPLIANCE_SERVICE_MANAGER_STOP_FAILED:
        return "APPLIANCE_SERVICE_MANAGER_STOP_FAILED";
    case APPLIANCE_SERVICE_MANAGER_RESTART_FAILED:
        return "APPLIANCE_SERVICE_MANAGER_RESTART_FAILED";
    case APPLIANCE_SERVICE_MANAGER_RESTART_DISABLED:
        return "APPLIANCE_SERVICE_MANAGER_RESTART_DISABLED";
    case APPLIANCE_SERVICE_MANAGER_SNAPSHOT_FAILED:
        return "APPLIANCE_SERVICE_MANAGER_SNAPSHOT_FAILED";
    case APPLIANCE_SERVICE_MANAGER_HEALTH_FAILED:
        return "APPLIANCE_SERVICE_MANAGER_HEALTH_FAILED";
    case APPLIANCE_SERVICE_MANAGER_OVERFLOW:
        return "APPLIANCE_SERVICE_MANAGER_OVERFLOW";
    default:
        return NULL;
    }
}

const char *appliance_service_manager_state_string(
    enum appliance_service_manager_state state)
{
    switch (state) {
    case APPLIANCE_SERVICE_MANAGER_STATE_STOPPED:
        return "STOPPED";
    case APPLIANCE_SERVICE_MANAGER_STATE_RUNNING:
        return "RUNNING";
    case APPLIANCE_SERVICE_MANAGER_STATE_START_FAILED:
        return "START_FAILED";
    case APPLIANCE_SERVICE_MANAGER_STATE_STOP_FAILED:
        return "STOP_FAILED";
    default:
        return NULL;
    }
}

const char *appliance_service_manager_startup_policy_string(
    enum appliance_service_manager_startup_policy policy)
{
    switch (policy) {
    case APPLIANCE_SERVICE_MANAGER_STARTUP_ROLLBACK:
        return "ROLLBACK";
    case APPLIANCE_SERVICE_MANAGER_STARTUP_PRESERVE:
        return "PRESERVE";
    default:
        return NULL;
    }
}
