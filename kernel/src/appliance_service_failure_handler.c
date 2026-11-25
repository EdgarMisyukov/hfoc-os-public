#include "appliance_service_failure_handler.h"

#include "appliance_service_manager.h"
#include "service.h"
#include "service_supervisor.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

static void appliance_service_failure_handler_zero_bytes(
    void *destination,
    size_t count)
{
    unsigned char *bytes = destination;
    size_t index;

    for (index = 0U; index < count; ++index) {
        bytes[index] = 0U;
    }
}

static bool appliance_service_failure_handler_status_valid(
    enum appliance_service_failure_handler_status status)
{
    return status >= APPLIANCE_SERVICE_FAILURE_HANDLER_OK &&
           status <= APPLIANCE_SERVICE_FAILURE_HANDLER_OVERFLOW;
}

static bool appliance_service_failure_kind_valid(
    enum appliance_service_failure_kind kind)
{
    return kind >= APPLIANCE_SERVICE_FAILURE_KIND_NONE &&
           kind <= APPLIANCE_SERVICE_FAILURE_KIND_LIVENESS_FAILED;
}

static bool appliance_service_failure_action_valid(
    enum appliance_service_failure_action action)
{
    return action >= APPLIANCE_SERVICE_FAILURE_ACTION_NONE &&
           action <= APPLIANCE_SERVICE_FAILURE_ACTION_ISOLATE;
}

static bool appliance_service_failure_result_valid(
    enum appliance_service_failure_result result)
{
    return result >= APPLIANCE_SERVICE_FAILURE_RESULT_NONE &&
           result <= APPLIANCE_SERVICE_FAILURE_RESULT_NOT_RESTARTED;
}

static bool appliance_service_failure_supervisor_state_valid(
    enum service_supervisor_state state)
{
    return state >= SERVICE_SUPERVISOR_STATE_RUNNING &&
           state <= SERVICE_SUPERVISOR_STATE_EXHAUSTED;
}

static bool appliance_service_failure_health_state_valid(
    enum service_health_state state)
{
    return state >= SERVICE_HEALTH_PENDING &&
           state <= SERVICE_HEALTH_READINESS_TIMEOUT;
}

static bool appliance_service_failure_snapshot_valid(
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
            !appliance_service_failure_supervisor_state_valid(entry->state) ||
            entry->state == SERVICE_SUPERVISOR_STATE_STOPPED ||
            !appliance_service_failure_health_state_valid(
                entry->health_state)) {
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
appliance_service_failure_find_entry(
    const struct service_supervisor_snapshot *snapshot,
    uint32_t service_id)
{
    uint32_t index;

    if (snapshot == NULL) {
        return NULL;
    }
    for (index = 0U; index < snapshot->entry_count; ++index) {
        if (snapshot->entries[index].descriptor.id == service_id) {
            return &snapshot->entries[index];
        }
    }
    return NULL;
}

static const struct service_supervisor_snapshot_entry *
appliance_service_failure_first_exited(
    const struct service_supervisor_snapshot *snapshot)
{
    uint32_t index;

    if (snapshot == NULL) {
        return NULL;
    }
    for (index = 0U; index < snapshot->entry_count; ++index) {
        if (snapshot->entries[index].state ==
            SERVICE_SUPERVISOR_STATE_EXITED) {
            return &snapshot->entries[index];
        }
    }
    return NULL;
}

static void appliance_service_failure_increment(uint32_t *value)
{
    if (*value < UINT32_MAX) {
        ++*value;
    }
}

static void appliance_service_failure_event_reset(
    struct appliance_service_failure_event *event)
{
    if (event == NULL) {
        return;
    }
    appliance_service_failure_handler_zero_bytes(
        event, sizeof(*event));
    event->version = APPLIANCE_SERVICE_FAILURE_HANDLER_VERSION;
}

static bool appliance_service_failure_has_restart_budget(
    const struct service_supervisor_snapshot_entry *entry)
{
    return entry != NULL &&
           entry->descriptor.restart_limit > 0U &&
           entry->restart_count < entry->descriptor.restart_limit;
}

static enum appliance_service_failure_handler_status
appliance_service_failure_snapshot_after_restart(
    struct appliance_service_manager *manager,
    uint32_t service_id,
    const struct service_supervisor_snapshot_entry **entry_out,
    struct service_supervisor_snapshot *snapshot_out)
{
    enum service_supervisor_status status;

    if (manager == NULL ||
        manager->ops.snapshot == NULL ||
        entry_out == NULL ||
        snapshot_out == NULL) {
        return APPLIANCE_SERVICE_FAILURE_HANDLER_BAD_STATE;
    }
    appliance_service_failure_handler_zero_bytes(
        snapshot_out, sizeof(*snapshot_out));
    status = manager->ops.snapshot(
        manager->ops.context, snapshot_out);
    if (status != SERVICE_SUPERVISOR_OK ||
        !appliance_service_failure_snapshot_valid(snapshot_out)) {
        return APPLIANCE_SERVICE_FAILURE_HANDLER_SNAPSHOT_FAILED;
    }
    *entry_out = appliance_service_failure_find_entry(
        snapshot_out, service_id);
    return APPLIANCE_SERVICE_FAILURE_HANDLER_OK;
}

enum appliance_service_failure_handler_status
appliance_service_failure_handler_init(
    struct appliance_service_failure_handler *handler,
    struct appliance_service_manager *manager)
{
    if (handler == NULL || manager == NULL) {
        return APPLIANCE_SERVICE_FAILURE_HANDLER_NULL;
    }
    if (handler->initialized) {
        return APPLIANCE_SERVICE_FAILURE_HANDLER_BAD_STATE;
    }
    if (!manager->initialized ||
        !manager->graph.published ||
        manager->state != APPLIANCE_SERVICE_MANAGER_STATE_RUNNING ||
        manager->ops.snapshot == NULL ||
        manager->ops.restart_contained == NULL) {
        return APPLIANCE_SERVICE_FAILURE_HANDLER_BAD_STATE;
    }

    appliance_service_failure_handler_zero_bytes(
        handler, sizeof(*handler));
    handler->initialized = true;
    handler->version = APPLIANCE_SERVICE_FAILURE_HANDLER_VERSION;
    handler->manager = manager;
    appliance_service_failure_event_reset(&handler->last_event);
    return APPLIANCE_SERVICE_FAILURE_HANDLER_OK;
}

enum appliance_service_failure_handler_status
appliance_service_failure_handler_poll(
    struct appliance_service_failure_handler *handler,
    uint64_t now_ticks,
    struct appliance_service_failure_event *event_out)
{
    struct service_supervisor_snapshot snapshot;
    const struct service_supervisor_snapshot_entry *entry;
    enum service_supervisor_status supervisor_status;
    enum appliance_service_manager_status manager_status;
    struct appliance_service_failure_event event;
    uint32_t exit_status = 0U;
    bool restarted = false;

    if (handler == NULL || event_out == NULL) {
        return APPLIANCE_SERVICE_FAILURE_HANDLER_NULL;
    }
    if (!handler->initialized ||
        handler->version != APPLIANCE_SERVICE_FAILURE_HANDLER_VERSION ||
        handler->manager == NULL) {
        return APPLIANCE_SERVICE_FAILURE_HANDLER_BAD_STATE;
    }
    if (handler->manager->state !=
            APPLIANCE_SERVICE_MANAGER_STATE_RUNNING ||
        !handler->manager->graph.published) {
        return APPLIANCE_SERVICE_FAILURE_HANDLER_BAD_STATE;
    }

    appliance_service_failure_event_reset(event_out);
    appliance_service_failure_handler_zero_bytes(
        &snapshot, sizeof(snapshot));
    supervisor_status = handler->manager->ops.snapshot(
        handler->manager->ops.context, &snapshot);
    if (supervisor_status != SERVICE_SUPERVISOR_OK ||
        !appliance_service_failure_snapshot_valid(&snapshot)) {
        return APPLIANCE_SERVICE_FAILURE_HANDLER_SNAPSHOT_FAILED;
    }

    entry = appliance_service_failure_first_exited(&snapshot);
    if (entry == NULL) {
        return APPLIANCE_SERVICE_FAILURE_HANDLER_NO_FAILURE;
    }

    appliance_service_failure_event_reset(&event);
    event.published = true;
    event.service_id = entry->descriptor.id;
    event.kind = APPLIANCE_SERVICE_FAILURE_KIND_EXIT;
    event.action = APPLIANCE_SERVICE_FAILURE_ACTION_RESTART;
    event.result = APPLIANCE_SERVICE_FAILURE_RESULT_RESTARTED;
    event.exit_status = entry->exit_status;
    event.restart_count = entry->restart_count;
    event.now_ticks = now_ticks;
    appliance_service_failure_increment(&handler->observed_count);

    manager_status = appliance_service_manager_restart(
        handler->manager, entry->descriptor.id, now_ticks,
        &exit_status, &restarted);
    if (manager_status == APPLIANCE_SERVICE_MANAGER_OK &&
        restarted) {
        event.action = APPLIANCE_SERVICE_FAILURE_ACTION_RESTART;
        event.result = APPLIANCE_SERVICE_FAILURE_RESULT_RESTARTED;
        appliance_service_failure_increment(&handler->recovered_count);
    } else if (manager_status ==
               APPLIANCE_SERVICE_MANAGER_RESTART_DISABLED) {
        event.action = APPLIANCE_SERVICE_FAILURE_ACTION_ISOLATE;
        event.result = APPLIANCE_SERVICE_FAILURE_RESULT_EXHAUSTED;
        appliance_service_failure_increment(&handler->exhausted_count);
    } else {
        struct service_supervisor_snapshot fresh_snapshot;
        const struct service_supervisor_snapshot_entry *fresh_entry = NULL;
        enum appliance_service_failure_handler_status fresh_status;

        fresh_status = appliance_service_failure_snapshot_after_restart(
            handler->manager, entry->descriptor.id,
            &fresh_entry, &fresh_snapshot);
        if (fresh_status ==
                APPLIANCE_SERVICE_FAILURE_HANDLER_SNAPSHOT_FAILED) {
            event.action = APPLIANCE_SERVICE_FAILURE_ACTION_ISOLATE;
            event.result = APPLIANCE_SERVICE_FAILURE_RESULT_EXHAUSTED;
            appliance_service_failure_increment(
                &handler->exhausted_count);
        } else if (fresh_entry == NULL ||
                   !appliance_service_failure_has_restart_budget(
                       fresh_entry)) {
            event.action = APPLIANCE_SERVICE_FAILURE_ACTION_ISOLATE;
            event.result = APPLIANCE_SERVICE_FAILURE_RESULT_EXHAUSTED;
            appliance_service_failure_increment(
                &handler->exhausted_count);
        } else {
            event.action = APPLIANCE_SERVICE_FAILURE_ACTION_DEFER;
            event.result = APPLIANCE_SERVICE_FAILURE_RESULT_DEFERRED;
            appliance_service_failure_increment(&handler->deferred_count);
        }
    }

    handler->last_event = event;
    *event_out = event;
    return APPLIANCE_SERVICE_FAILURE_HANDLER_OK;
}

enum appliance_service_failure_handler_status
appliance_service_failure_handler_snapshot(
    const struct appliance_service_failure_handler *handler,
    struct appliance_service_failure_handler_snapshot *snapshot_out)
{
    struct appliance_service_manager_snapshot manager_snapshot;
    enum appliance_service_manager_status manager_status;

    if (handler == NULL || snapshot_out == NULL) {
        return APPLIANCE_SERVICE_FAILURE_HANDLER_NULL;
    }
    if (!handler->initialized ||
        handler->version != APPLIANCE_SERVICE_FAILURE_HANDLER_VERSION ||
        handler->manager == NULL) {
        return APPLIANCE_SERVICE_FAILURE_HANDLER_BAD_STATE;
    }

    appliance_service_failure_handler_zero_bytes(
        &manager_snapshot, sizeof(manager_snapshot));
    manager_status = appliance_service_manager_snapshot(
        handler->manager, &manager_snapshot);
    if (manager_status != APPLIANCE_SERVICE_MANAGER_OK) {
        return APPLIANCE_SERVICE_FAILURE_HANDLER_SNAPSHOT_FAILED;
    }

    appliance_service_failure_handler_zero_bytes(
        snapshot_out, sizeof(*snapshot_out));
    snapshot_out->published = true;
    snapshot_out->version = APPLIANCE_SERVICE_FAILURE_HANDLER_VERSION;
    snapshot_out->observed_count = handler->observed_count;
    snapshot_out->recovered_count = handler->recovered_count;
    snapshot_out->deferred_count = handler->deferred_count;
    snapshot_out->exhausted_count = handler->exhausted_count;
    snapshot_out->last_event = handler->last_event;
    snapshot_out->manager_state = manager_snapshot.state;
    snapshot_out->manager_health = manager_snapshot.health_result;
    return APPLIANCE_SERVICE_FAILURE_HANDLER_OK;
}

static bool appliance_service_failure_append_bytes(
    char *output,
    size_t *offset,
    const char *text,
    size_t length)
{
    size_t index;

    if (*offset > APPLIANCE_SERVICE_FAILURE_HANDLER_OUTPUT_MAX ||
        length > APPLIANCE_SERVICE_FAILURE_HANDLER_OUTPUT_MAX - *offset) {
        return false;
    }
    for (index = 0U; index < length; ++index) {
        output[*offset + index] = text[index];
    }
    *offset += length;
    return true;
}

static bool appliance_service_failure_append_text(
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
    return appliance_service_failure_append_bytes(
        output, offset, text, length);
}

static bool appliance_service_failure_append_char(
    char *output,
    size_t *offset,
    char value)
{
    return appliance_service_failure_append_bytes(
        output, offset, &value, 1U);
}

static bool appliance_service_failure_append_u32(
    char *output,
    size_t *offset,
    uint32_t value)
{
    char digits[10];
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
        if (!appliance_service_failure_append_char(
                output, offset, digits[count])) {
            return false;
        }
    }
    return true;
}

static const char *appliance_service_failure_health_string(
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

static const char *appliance_service_failure_render_action(
    enum appliance_service_failure_action action)
{
    switch (action) {
    case APPLIANCE_SERVICE_FAILURE_ACTION_NONE:
        return "NONE";
    case APPLIANCE_SERVICE_FAILURE_ACTION_RESTART:
        return "RESTART";
    case APPLIANCE_SERVICE_FAILURE_ACTION_DEFER:
        return "DEFER";
    case APPLIANCE_SERVICE_FAILURE_ACTION_ISOLATE:
        return "ISOLATE";
    default:
        return NULL;
    }
}

static const char *appliance_service_failure_render_result(
    enum appliance_service_failure_result result)
{
    switch (result) {
    case APPLIANCE_SERVICE_FAILURE_RESULT_NONE:
        return "NONE";
    case APPLIANCE_SERVICE_FAILURE_RESULT_RESTARTED:
        return "RECOVERED";
    case APPLIANCE_SERVICE_FAILURE_RESULT_DEFERRED:
        return "DEFERRED";
    case APPLIANCE_SERVICE_FAILURE_RESULT_EXHAUSTED:
        return "EXHAUSTED";
    case APPLIANCE_SERVICE_FAILURE_RESULT_NOT_RESTARTED:
        return "NOT_RESTARTED";
    default:
        return NULL;
    }
}

enum appliance_service_failure_handler_status
appliance_service_failure_handler_render(
    const struct appliance_service_failure_handler_snapshot *snapshot,
    char *output,
    size_t capacity,
    size_t *output_length)
{
    char rendered[APPLIANCE_SERVICE_FAILURE_HANDLER_OUTPUT_MAX + 1U];
    size_t offset = 0U;
    size_t index;
    const char *kind;
    const char *action;
    const char *result;
    const char *health;

    if (snapshot == NULL || output == NULL || output_length == NULL) {
        return APPLIANCE_SERVICE_FAILURE_HANDLER_NULL;
    }
    if (!snapshot->published ||
        snapshot->version != APPLIANCE_SERVICE_FAILURE_HANDLER_VERSION ||
        !snapshot->last_event.published ||
        !appliance_service_failure_kind_valid(snapshot->last_event.kind) ||
        !appliance_service_failure_action_valid(
            snapshot->last_event.action) ||
        !appliance_service_failure_result_valid(
            snapshot->last_event.result)) {
        return APPLIANCE_SERVICE_FAILURE_HANDLER_BAD_STATE;
    }
    kind = appliance_service_failure_kind_string(
        snapshot->last_event.kind);
    action = appliance_service_failure_render_action(
        snapshot->last_event.action);
    result = appliance_service_failure_render_result(
        snapshot->last_event.result);
    health = appliance_service_failure_health_string(
        snapshot->manager_health);
    if (kind == NULL || action == NULL || result == NULL ||
        health == NULL ||
        appliance_service_failure_handler_state_string(
            snapshot->manager_state) == NULL) {
        return APPLIANCE_SERVICE_FAILURE_HANDLER_BAD_STATE;
    }

    if (!appliance_service_failure_append_text(
            rendered, &offset, "appliance_failures v=") ||
        !appliance_service_failure_append_u32(
            rendered, &offset, snapshot->version) ||
        !appliance_service_failure_append_text(
            rendered, &offset, " observed=") ||
        !appliance_service_failure_append_u32(
            rendered, &offset, snapshot->observed_count) ||
        !appliance_service_failure_append_text(
            rendered, &offset, " recovered=") ||
        !appliance_service_failure_append_u32(
            rendered, &offset, snapshot->recovered_count) ||
        !appliance_service_failure_append_text(
            rendered, &offset, " deferred=") ||
        !appliance_service_failure_append_u32(
            rendered, &offset, snapshot->deferred_count) ||
        !appliance_service_failure_append_text(
            rendered, &offset, " exhausted=") ||
        !appliance_service_failure_append_u32(
            rendered, &offset, snapshot->exhausted_count) ||
        !appliance_service_failure_append_text(
            rendered, &offset, " last=") ||
        !appliance_service_failure_append_text(
            rendered, &offset, kind) ||
        !appliance_service_failure_append_char(
            rendered, &offset, ':') ||
        !appliance_service_failure_append_u32(
            rendered, &offset, snapshot->last_event.service_id) ||
        !appliance_service_failure_append_char(
            rendered, &offset, ':') ||
        !appliance_service_failure_append_text(
            rendered, &offset, action) ||
        !appliance_service_failure_append_char(
            rendered, &offset, ':') ||
        !appliance_service_failure_append_text(
            rendered, &offset, result) ||
        !appliance_service_failure_append_text(
            rendered, &offset, " manager=") ||
        !appliance_service_failure_append_text(
            rendered, &offset,
            appliance_service_failure_handler_state_string(
                snapshot->manager_state)) ||
        !appliance_service_failure_append_text(
            rendered, &offset, " health=") ||
        !appliance_service_failure_append_text(
            rendered, &offset, health) ||
        !appliance_service_failure_append_char(
            rendered, &offset, '\n')) {
        return APPLIANCE_SERVICE_FAILURE_HANDLER_OVERFLOW;
    }
    if (capacity <= offset) {
        return APPLIANCE_SERVICE_FAILURE_HANDLER_OVERFLOW;
    }
    for (index = 0U; index < offset; ++index) {
        output[index] = rendered[index];
    }
    output[offset] = '\0';
    *output_length = offset;
    return APPLIANCE_SERVICE_FAILURE_HANDLER_OK;
}

const char *appliance_service_failure_handler_status_string(
    enum appliance_service_failure_handler_status status)
{
    if (!appliance_service_failure_handler_status_valid(status)) {
        return NULL;
    }
    switch (status) {
    case APPLIANCE_SERVICE_FAILURE_HANDLER_OK:
        return "OK";
    case APPLIANCE_SERVICE_FAILURE_HANDLER_NULL:
        return "NULL";
    case APPLIANCE_SERVICE_FAILURE_HANDLER_BAD_STATE:
        return "BAD_STATE";
    case APPLIANCE_SERVICE_FAILURE_HANDLER_NOT_CONFIGURED:
        return "NOT_CONFIGURED";
    case APPLIANCE_SERVICE_FAILURE_HANDLER_NO_FAILURE:
        return "NO_FAILURE";
    case APPLIANCE_SERVICE_FAILURE_HANDLER_SNAPSHOT_FAILED:
        return "SNAPSHOT_FAILED";
    case APPLIANCE_SERVICE_FAILURE_HANDLER_RESTART_FAILED:
        return "RESTART_FAILED";
    case APPLIANCE_SERVICE_FAILURE_HANDLER_OVERFLOW:
        return "OVERFLOW";
    default:
        return NULL;
    }
}

const char *appliance_service_failure_kind_string(
    enum appliance_service_failure_kind kind)
{
    if (!appliance_service_failure_kind_valid(kind)) {
        return NULL;
    }
    switch (kind) {
    case APPLIANCE_SERVICE_FAILURE_KIND_NONE:
        return "NONE";
    case APPLIANCE_SERVICE_FAILURE_KIND_EXIT:
        return "EXIT";
    case APPLIANCE_SERVICE_FAILURE_KIND_READINESS_TIMEOUT:
        return "READINESS_TIMEOUT";
    case APPLIANCE_SERVICE_FAILURE_KIND_LIVENESS_FAILED:
        return "LIVENESS_FAILED";
    default:
        return NULL;
    }
}

const char *appliance_service_failure_action_string(
    enum appliance_service_failure_action action)
{
    if (!appliance_service_failure_action_valid(action)) {
        return NULL;
    }
    switch (action) {
    case APPLIANCE_SERVICE_FAILURE_ACTION_NONE:
        return "NONE";
    case APPLIANCE_SERVICE_FAILURE_ACTION_RESTART:
        return "RESTART";
    case APPLIANCE_SERVICE_FAILURE_ACTION_DEFER:
        return "DEFER";
    case APPLIANCE_SERVICE_FAILURE_ACTION_ISOLATE:
        return "ISOLATE";
    default:
        return NULL;
    }
}

const char *appliance_service_failure_result_string(
    enum appliance_service_failure_result result)
{
    if (!appliance_service_failure_result_valid(result)) {
        return NULL;
    }
    switch (result) {
    case APPLIANCE_SERVICE_FAILURE_RESULT_NONE:
        return "NONE";
    case APPLIANCE_SERVICE_FAILURE_RESULT_RESTARTED:
        return "RESTARTED";
    case APPLIANCE_SERVICE_FAILURE_RESULT_DEFERRED:
        return "DEFERRED";
    case APPLIANCE_SERVICE_FAILURE_RESULT_EXHAUSTED:
        return "EXHAUSTED";
    case APPLIANCE_SERVICE_FAILURE_RESULT_NOT_RESTARTED:
        return "NOT_RESTARTED";
    default:
        return NULL;
    }
}

const char *appliance_service_failure_handler_state_string(
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
