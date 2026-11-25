#include "appliance_https_service.h"

#include "appliance_service_manager.h"
#include "https_service.h"
#include "https_service_lifecycle.h"
#include "service.h"
#include "service_supervisor.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#if defined(HFOC_APPLIANCE_HTTPS_SERVICE_HOST_TEST) || \
    defined(HFOC_APPLIANCE_HTTPS_SERVICE_QEMU_TEST)

static void appliance_https_service_zero_bytes(
    void *destination,
    size_t length)
{
    unsigned char *bytes = destination;
    size_t index;

    for (index = 0U; index < length; ++index) {
        bytes[index] = 0U;
    }
}

static bool appliance_https_service_valid_state(
    enum appliance_https_service_state state)
{
    return state >= APPLIANCE_HTTPS_SERVICE_STATE_STOPPED &&
           state <= APPLIANCE_HTTPS_SERVICE_STATE_FAILED;
}

static bool appliance_https_service_valid_failure(
    enum appliance_https_service_failure failure)
{
    return failure >= APPLIANCE_HTTPS_SERVICE_FAILURE_NONE &&
           failure <= APPLIANCE_HTTPS_SERVICE_FAILURE_SNAPSHOT;
}

static bool appliance_https_service_initialized(
    const struct appliance_https_service *service)
{
    return service != NULL &&
           service->initialized &&
           service->version == APPLIANCE_HTTPS_SERVICE_VERSION &&
           appliance_https_service_valid_state(service->state) &&
           appliance_https_service_valid_failure(service->failure) &&
           https_service_descriptor_published(&service->descriptor) &&
           https_service_lifecycle_published(&service->lifecycle);
}

static bool appliance_https_service_name_equal(
    const char *left,
    const char *right)
{
    uint32_t index;

    for (index = 0U; index <= SERVICE_NAME_MAX; ++index) {
        if (left[index] != right[index]) {
            return false;
        }
        if (left[index] == '\0') {
            return true;
        }
    }
    return true;
}

static bool appliance_https_service_manager_matches(
    const struct appliance_service_manager *manager,
    const struct https_service_descriptor *descriptor)
{
    uint32_t index;
    bool found = false;

    if (manager == NULL ||
        !manager->initialized ||
        !manager->graph.published) {
        return false;
    }
    for (index = 0U;
         index < service_graph_snapshot_count(&manager->graph);
         ++index) {
        const struct service_descriptor *service =
            service_graph_snapshot_descriptor(&manager->graph, index);

        if (service == NULL || service->id != HTTPS_SERVICE_ID) {
            continue;
        }
        if (service_descriptor_validate(service) !=
                SERVICE_DESCRIPTOR_OK ||
            !service->enabled ||
            !appliance_https_service_name_equal(
                service->name, HTTPS_SERVICE_NAME) ||
            service->restart_limit != descriptor->restart_budget ||
            service->readiness_timeout_ms !=
                descriptor->readiness_deadline_ms) {
            return false;
        }
        found = true;
    }
    return found;
}

static bool appliance_https_service_lifecycle_matches_descriptor(
    const struct https_service_lifecycle_config *config,
    const struct https_service_descriptor *descriptor)
{
    return config != NULL &&
           descriptor != NULL &&
           config->connection_capacity ==
               descriptor->envelope.connection_capacity &&
           config->tls_context_capacity ==
               descriptor->envelope.tls_context_capacity &&
           config->http_parser_capacity ==
               descriptor->envelope.http_parser_capacity &&
           config->packet_pool_capacity ==
               descriptor->envelope.packet_pool_capacity;
}

static void appliance_https_service_fail(
    struct appliance_https_service *service,
    enum appliance_https_service_failure failure)
{
    service->state = APPLIANCE_HTTPS_SERVICE_STATE_FAILED;
    service->failure = failure;
}

static enum appliance_https_service_status
appliance_https_service_check_readiness(
    const struct appliance_https_service *service,
    const struct https_service_readiness *readiness,
    bool *ready_out)
{
    enum https_service_status status;
    bool ready = false;

    if (readiness == NULL || ready_out == NULL) {
        return APPLIANCE_HTTPS_SERVICE_NULL;
    }
    status = https_service_start_check(
        &service->descriptor, readiness, &ready);
    if (status == HTTPS_SERVICE_NULL) {
        return APPLIANCE_HTTPS_SERVICE_NULL;
    }
    if (status == HTTPS_SERVICE_BAD_VERSION ||
        status == HTTPS_SERVICE_BAD_DEPENDENCY) {
        return APPLIANCE_HTTPS_SERVICE_INVALID_READINESS;
    }
    if (status == HTTPS_SERVICE_DEPENDENCY_UNAVAILABLE) {
        *ready_out = false;
        return APPLIANCE_HTTPS_SERVICE_DEPENDENCY_UNAVAILABLE;
    }
    if (status != HTTPS_SERVICE_OK ||
        !ready) {
        return APPLIANCE_HTTPS_SERVICE_BAD_STATE;
    }
    *ready_out = true;
    return APPLIANCE_HTTPS_SERVICE_OK;
}

enum appliance_https_service_status appliance_https_service_init(
    struct appliance_https_service *service,
    struct appliance_service_manager *manager,
    const struct https_service_descriptor *descriptor,
    const struct https_service_lifecycle_config *lifecycle_config,
    const struct https_service_lifecycle_ops *lifecycle_ops)
{
    struct appliance_https_service candidate;
    struct https_service_descriptor published_descriptor;
    enum https_service_status descriptor_status;
    enum https_service_lifecycle_status lifecycle_status;

    if (service == NULL || manager == NULL || descriptor == NULL ||
        lifecycle_config == NULL || lifecycle_ops == NULL) {
        return APPLIANCE_HTTPS_SERVICE_NULL;
    }
    if (service->initialized) {
        return APPLIANCE_HTTPS_SERVICE_ALREADY_PUBLISHED;
    }
    if (manager->state != APPLIANCE_SERVICE_MANAGER_STATE_STOPPED ||
        !appliance_https_service_manager_matches(manager, descriptor)) {
        return APPLIANCE_HTTPS_SERVICE_INVALID_MANAGER;
    }
    if (!appliance_https_service_lifecycle_matches_descriptor(
            lifecycle_config, descriptor)) {
        return APPLIANCE_HTTPS_SERVICE_INVALID_CONFIG;
    }

    appliance_https_service_zero_bytes(
        &published_descriptor, sizeof(published_descriptor));
    descriptor_status = https_service_descriptor_init(
        &published_descriptor, descriptor);
    if (descriptor_status != HTTPS_SERVICE_OK) {
        return APPLIANCE_HTTPS_SERVICE_INVALID_DESCRIPTOR;
    }

    candidate = *service;
    appliance_https_service_zero_bytes(
        &candidate.lifecycle, sizeof(candidate.lifecycle));
    lifecycle_status = https_service_lifecycle_init(
        &candidate.lifecycle,
        lifecycle_config,
        lifecycle_ops);
    if (lifecycle_status != HTTPS_SERVICE_LIFECYCLE_OK) {
        return APPLIANCE_HTTPS_SERVICE_INVALID_CONFIG;
    }

    candidate.manager = manager;
    candidate.descriptor = published_descriptor;
    candidate.initialized = true;
    candidate.version = APPLIANCE_HTTPS_SERVICE_VERSION;
    candidate.state = APPLIANCE_HTTPS_SERVICE_STATE_STOPPED;
    candidate.failure = APPLIANCE_HTTPS_SERVICE_FAILURE_NONE;
    candidate.start_count = 0U;
    candidate.stop_count = 0U;
    candidate.restart_count = 0U;
    *service = candidate;
    return APPLIANCE_HTTPS_SERVICE_OK;
}

enum appliance_https_service_status appliance_https_service_start(
    struct appliance_https_service *service,
    const struct https_service_readiness *readiness)
{
    enum appliance_https_service_status readiness_status;
    enum appliance_service_manager_status manager_status;
    bool ready = false;

    if (service == NULL || readiness == NULL) {
        return APPLIANCE_HTTPS_SERVICE_NULL;
    }
    if (!appliance_https_service_initialized(service)) {
        return APPLIANCE_HTTPS_SERVICE_UNPUBLISHED;
    }
    if (service->state != APPLIANCE_HTTPS_SERVICE_STATE_STOPPED ||
        service->manager->state != APPLIANCE_SERVICE_MANAGER_STATE_STOPPED) {
        return APPLIANCE_HTTPS_SERVICE_BAD_STATE;
    }
    readiness_status = appliance_https_service_check_readiness(
        service, readiness, &ready);
    if (readiness_status != APPLIANCE_HTTPS_SERVICE_OK) {
        return readiness_status;
    }

    manager_status = appliance_service_manager_start(service->manager);
    if (manager_status != APPLIANCE_SERVICE_MANAGER_OK) {
        appliance_https_service_fail(
            service, APPLIANCE_HTTPS_SERVICE_FAILURE_START);
        return APPLIANCE_HTTPS_SERVICE_START_FAILED;
    }
    service->state = APPLIANCE_HTTPS_SERVICE_STATE_STARTING;
    ++service->start_count;
    return APPLIANCE_HTTPS_SERVICE_OK;
}

enum appliance_https_service_status appliance_https_service_ready(
    struct appliance_https_service *service,
    const struct https_service_readiness *readiness)
{
    enum appliance_https_service_status readiness_status;
    enum https_service_lifecycle_status lifecycle_status;
    uint64_t manager_generation;
    bool ready = false;

    if (service == NULL || readiness == NULL) {
        return APPLIANCE_HTTPS_SERVICE_NULL;
    }
    if (!appliance_https_service_initialized(service)) {
        return APPLIANCE_HTTPS_SERVICE_UNPUBLISHED;
    }
    if (service->state != APPLIANCE_HTTPS_SERVICE_STATE_STARTING ||
        service->manager->state != APPLIANCE_SERVICE_MANAGER_STATE_RUNNING) {
        return APPLIANCE_HTTPS_SERVICE_BAD_STATE;
    }
    readiness_status = appliance_https_service_check_readiness(
        service, readiness, &ready);
    if (readiness_status != APPLIANCE_HTTPS_SERVICE_OK) {
        return readiness_status;
    }

    manager_generation = service->manager->generation;
    if (manager_generation < HTTPS_SERVICE_LIFECYCLE_GENERATION_MIN ||
        manager_generation > HTTPS_SERVICE_LIFECYCLE_GENERATION_MAX) {
        appliance_https_service_fail(
            service, APPLIANCE_HTTPS_SERVICE_FAILURE_READY);
        return APPLIANCE_HTTPS_SERVICE_READY_FAILED;
    }
    if (service->lifecycle.phase == HTTPS_SERVICE_LIFECYCLE_PHASE_IDLE) {
        lifecycle_status = https_service_lifecycle_start(
            &service->lifecycle, (uint32_t)manager_generation);
    } else if (service->lifecycle.phase ==
               HTTPS_SERVICE_LIFECYCLE_PHASE_STOPPED) {
        lifecycle_status = https_service_lifecycle_restart(
            &service->lifecycle);
    } else {
        appliance_https_service_fail(
            service, APPLIANCE_HTTPS_SERVICE_FAILURE_READY);
        return APPLIANCE_HTTPS_SERVICE_READY_FAILED;
    }
    if (lifecycle_status != HTTPS_SERVICE_LIFECYCLE_OK) {
        appliance_https_service_fail(
            service, APPLIANCE_HTTPS_SERVICE_FAILURE_READY);
        return APPLIANCE_HTTPS_SERVICE_READY_FAILED;
    }
    service->state = APPLIANCE_HTTPS_SERVICE_STATE_RUNNING;
    return APPLIANCE_HTTPS_SERVICE_OK;
}

enum appliance_https_service_status appliance_https_service_stop_admission(
    struct appliance_https_service *service,
    uint32_t now_ms)
{
    enum appliance_service_manager_status manager_status;
    enum https_service_lifecycle_status lifecycle_status;

    if (service == NULL) {
        return APPLIANCE_HTTPS_SERVICE_NULL;
    }
    if (!appliance_https_service_initialized(service)) {
        return APPLIANCE_HTTPS_SERVICE_UNPUBLISHED;
    }
    if (service->state == APPLIANCE_HTTPS_SERVICE_STATE_STOPPED) {
        return APPLIANCE_HTTPS_SERVICE_OK;
    }
    if (service->state == APPLIANCE_HTTPS_SERVICE_STATE_STARTING) {
        manager_status = appliance_service_manager_stop(service->manager);
        if (manager_status != APPLIANCE_SERVICE_MANAGER_OK) {
            appliance_https_service_fail(
                service, APPLIANCE_HTTPS_SERVICE_FAILURE_STOP);
            return APPLIANCE_HTTPS_SERVICE_STOP_FAILED;
        }
        service->state = APPLIANCE_HTTPS_SERVICE_STATE_STOPPED;
        ++service->stop_count;
        return APPLIANCE_HTTPS_SERVICE_OK;
    }
    if (service->state == APPLIANCE_HTTPS_SERVICE_STATE_STOPPING) {
        return APPLIANCE_HTTPS_SERVICE_OK;
    }
    if (service->state != APPLIANCE_HTTPS_SERVICE_STATE_RUNNING ||
        service->manager->state != APPLIANCE_SERVICE_MANAGER_STATE_RUNNING) {
        return APPLIANCE_HTTPS_SERVICE_BAD_STATE;
    }
    lifecycle_status = https_service_lifecycle_stop_admission(
        &service->lifecycle, now_ms);
    if (lifecycle_status != HTTPS_SERVICE_LIFECYCLE_OK) {
        appliance_https_service_fail(
            service, APPLIANCE_HTTPS_SERVICE_FAILURE_STOP);
        return APPLIANCE_HTTPS_SERVICE_STOP_FAILED;
    }
    service->state = APPLIANCE_HTTPS_SERVICE_STATE_STOPPING;
    ++service->stop_count;
    return APPLIANCE_HTTPS_SERVICE_OK;
}

enum appliance_https_service_status appliance_https_service_stop_step(
    struct appliance_https_service *service,
    uint32_t now_ms)
{
    enum appliance_service_manager_status manager_status;
    enum https_service_lifecycle_status lifecycle_status;

    if (service == NULL) {
        return APPLIANCE_HTTPS_SERVICE_NULL;
    }
    if (!appliance_https_service_initialized(service)) {
        return APPLIANCE_HTTPS_SERVICE_UNPUBLISHED;
    }
    if (service->state != APPLIANCE_HTTPS_SERVICE_STATE_STOPPING) {
        return APPLIANCE_HTTPS_SERVICE_BAD_STATE;
    }

    lifecycle_status = https_service_lifecycle_step(
        &service->lifecycle, now_ms);
    if (lifecycle_status != HTTPS_SERVICE_LIFECYCLE_OK) {
        appliance_https_service_fail(
            service, APPLIANCE_HTTPS_SERVICE_FAILURE_STOP);
        return APPLIANCE_HTTPS_SERVICE_STOP_FAILED;
    }
    if (service->lifecycle.phase != HTTPS_SERVICE_LIFECYCLE_PHASE_DRAINED) {
        return APPLIANCE_HTTPS_SERVICE_STOP_PENDING;
    }

    lifecycle_status = https_service_lifecycle_finish_drain(
        &service->lifecycle);
    if (lifecycle_status != HTTPS_SERVICE_LIFECYCLE_OK) {
        appliance_https_service_fail(
            service, APPLIANCE_HTTPS_SERVICE_FAILURE_STOP);
        return APPLIANCE_HTTPS_SERVICE_STOP_FAILED;
    }
    manager_status = appliance_service_manager_stop(service->manager);
    if (manager_status != APPLIANCE_SERVICE_MANAGER_OK) {
        appliance_https_service_fail(
            service, APPLIANCE_HTTPS_SERVICE_FAILURE_STOP);
        return APPLIANCE_HTTPS_SERVICE_STOP_FAILED;
    }
    service->state = APPLIANCE_HTTPS_SERVICE_STATE_STOPPED;
    return APPLIANCE_HTTPS_SERVICE_OK;
}

enum appliance_https_service_status appliance_https_service_restart(
    struct appliance_https_service *service,
    const struct https_service_readiness *readiness,
    uint32_t now_ms,
    uint64_t now_ticks,
    uint32_t *exit_status_out,
    bool *restarted_out)
{
    enum appliance_https_service_status readiness_status;
    enum appliance_service_manager_status manager_status;
    enum https_service_lifecycle_status lifecycle_status;
    uint32_t drain_deadline_ms;
    bool ready = false;

    if (service == NULL || readiness == NULL ||
        exit_status_out == NULL || restarted_out == NULL) {
        return APPLIANCE_HTTPS_SERVICE_NULL;
    }
    if (!appliance_https_service_initialized(service)) {
        return APPLIANCE_HTTPS_SERVICE_UNPUBLISHED;
    }
    if (service->state != APPLIANCE_HTTPS_SERVICE_STATE_RUNNING ||
        service->manager->state != APPLIANCE_SERVICE_MANAGER_STATE_RUNNING ||
        (service->lifecycle.phase != HTTPS_SERVICE_LIFECYCLE_PHASE_LISTENING &&
         service->lifecycle.phase !=
             HTTPS_SERVICE_LIFECYCLE_PHASE_RESTARTED)) {
        return APPLIANCE_HTTPS_SERVICE_BAD_STATE;
    }
    readiness_status = appliance_https_service_check_readiness(
        service, readiness, &ready);
    if (readiness_status != APPLIANCE_HTTPS_SERVICE_OK) {
        return readiness_status;
    }

    lifecycle_status = https_service_lifecycle_stop_admission(
        &service->lifecycle, now_ms);
    if (lifecycle_status != HTTPS_SERVICE_LIFECYCLE_OK) {
        appliance_https_service_fail(
            service, APPLIANCE_HTTPS_SERVICE_FAILURE_RESTART);
        return APPLIANCE_HTTPS_SERVICE_RESTART_FAILED;
    }
    drain_deadline_ms =
        now_ms + service->lifecycle.drain_timeout_ms + 1U;
    lifecycle_status = https_service_lifecycle_step(
        &service->lifecycle, drain_deadline_ms);
    if (lifecycle_status != HTTPS_SERVICE_LIFECYCLE_OK) {
        appliance_https_service_fail(
            service, APPLIANCE_HTTPS_SERVICE_FAILURE_RESTART);
        return APPLIANCE_HTTPS_SERVICE_RESTART_FAILED;
    }
    lifecycle_status = https_service_lifecycle_finish_drain(
        &service->lifecycle);
    if (lifecycle_status != HTTPS_SERVICE_LIFECYCLE_OK) {
        appliance_https_service_fail(
            service, APPLIANCE_HTTPS_SERVICE_FAILURE_RESTART);
        return APPLIANCE_HTTPS_SERVICE_RESTART_FAILED;
    }

    *restarted_out = false;
    manager_status = appliance_service_manager_restart(
        service->manager,
        HTTPS_SERVICE_ID,
        now_ticks,
        exit_status_out,
        restarted_out);
    if (manager_status != APPLIANCE_SERVICE_MANAGER_OK) {
        appliance_https_service_fail(
            service, APPLIANCE_HTTPS_SERVICE_FAILURE_RESTART);
        return APPLIANCE_HTTPS_SERVICE_RESTART_FAILED;
    }
    if (!*restarted_out) {
        appliance_https_service_fail(
            service, APPLIANCE_HTTPS_SERVICE_FAILURE_RESTART);
        return APPLIANCE_HTTPS_SERVICE_RESTART_FAILED;
    }
    lifecycle_status = https_service_lifecycle_restart(
        &service->lifecycle);
    if (lifecycle_status != HTTPS_SERVICE_LIFECYCLE_OK) {
        appliance_https_service_fail(
            service, APPLIANCE_HTTPS_SERVICE_FAILURE_RESTART);
        return APPLIANCE_HTTPS_SERVICE_RESTART_FAILED;
    }
    service->state = APPLIANCE_HTTPS_SERVICE_STATE_RUNNING;
    ++service->restart_count;
    return APPLIANCE_HTTPS_SERVICE_OK;
}

enum appliance_https_service_status appliance_https_service_snapshot(
    const struct appliance_https_service *service,
    struct appliance_https_service_snapshot *snapshot_out)
{
    struct appliance_service_manager_snapshot manager_snapshot;
    struct service_supervisor_snapshot supervisor_snapshot;
    struct https_service_lifecycle_snapshot lifecycle_snapshot;
    enum appliance_service_manager_status manager_status;
    enum service_supervisor_status supervisor_status;
    enum https_service_lifecycle_status lifecycle_status;
    uint32_t index;

    if (service == NULL || snapshot_out == NULL) {
        return APPLIANCE_HTTPS_SERVICE_NULL;
    }
    if (!appliance_https_service_initialized(service)) {
        return APPLIANCE_HTTPS_SERVICE_UNPUBLISHED;
    }

    appliance_https_service_zero_bytes(
        &manager_snapshot, sizeof(manager_snapshot));
    manager_status = appliance_service_manager_snapshot(
        service->manager, &manager_snapshot);
    if (manager_status != APPLIANCE_SERVICE_MANAGER_OK) {
        return APPLIANCE_HTTPS_SERVICE_SNAPSHOT_FAILED;
    }
    appliance_https_service_zero_bytes(
        &supervisor_snapshot, sizeof(supervisor_snapshot));
    supervisor_status = service->manager->ops.snapshot(
        service->manager->ops.context, &supervisor_snapshot);
    if (supervisor_status != SERVICE_SUPERVISOR_OK) {
        return APPLIANCE_HTTPS_SERVICE_SNAPSHOT_FAILED;
    }
    appliance_https_service_zero_bytes(
        &lifecycle_snapshot, sizeof(lifecycle_snapshot));
    lifecycle_status = https_service_lifecycle_snapshot(
        &service->lifecycle, &lifecycle_snapshot);
    if (lifecycle_status != HTTPS_SERVICE_LIFECYCLE_OK) {
        return APPLIANCE_HTTPS_SERVICE_SNAPSHOT_FAILED;
    }

    appliance_https_service_zero_bytes(
        snapshot_out, sizeof(*snapshot_out));
    snapshot_out->published = true;
    snapshot_out->version = APPLIANCE_HTTPS_SERVICE_VERSION;
    snapshot_out->state = service->state;
    snapshot_out->failure = service->failure;
    snapshot_out->manager_state = manager_snapshot.state;
    snapshot_out->manager_generation = manager_snapshot.generation;
    snapshot_out->manager_health = manager_snapshot.health_result;
    snapshot_out->service_health = SERVICE_HEALTH_PENDING;
    for (index = 0U;
         index < supervisor_snapshot.entry_count &&
         index < SERVICE_SUPERVISOR_CAPACITY;
         ++index) {
        if (supervisor_snapshot.entries[index].descriptor.id ==
            HTTPS_SERVICE_ID) {
            snapshot_out->service_health =
                supervisor_snapshot.entries[index].health_state;
            break;
        }
    }
    snapshot_out->lifecycle_phase = lifecycle_snapshot.phase;
    snapshot_out->lifecycle_generation =
        lifecycle_snapshot.generation;
    snapshot_out->active_connection_count =
        lifecycle_snapshot.active_connection_count;
    snapshot_out->start_count = service->start_count;
    snapshot_out->stop_count = service->stop_count;
    snapshot_out->restart_count = service->restart_count;
    snapshot_out->lifecycle_stopped_count =
        lifecycle_snapshot.stopped_count;
    snapshot_out->drained_count = lifecycle_snapshot.drained_count;
    snapshot_out->aborted_count = lifecycle_snapshot.aborted_count;
    snapshot_out->zeroized_count = lifecycle_snapshot.zeroized_count;
    snapshot_out->http_released_count =
        lifecycle_snapshot.http_released_count;
    snapshot_out->packet_released_count =
        lifecycle_snapshot.packet_released_count;
    return APPLIANCE_HTTPS_SERVICE_OK;
}

static bool appliance_https_service_append_bytes(
    char *output,
    size_t *offset,
    const char *text,
    size_t length)
{
    size_t index;

    if (*offset > APPLIANCE_HTTPS_SERVICE_OUTPUT_MAX ||
        length > APPLIANCE_HTTPS_SERVICE_OUTPUT_MAX - *offset) {
        return false;
    }
    for (index = 0U; index < length; ++index) {
        output[*offset + index] = text[index];
    }
    *offset += length;
    return true;
}

static bool appliance_https_service_append_text(
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
    return appliance_https_service_append_bytes(
        output, offset, text, length);
}

static bool appliance_https_service_append_char(
    char *output,
    size_t *offset,
    char value)
{
    return appliance_https_service_append_bytes(
        output, offset, &value, 1U);
}

static bool appliance_https_service_append_u64(
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
        if (!appliance_https_service_append_char(
                output, offset, digits[count])) {
            return false;
        }
    }
    return true;
}

static const char *appliance_https_service_health_string(
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

static const char *appliance_https_service_service_health_string(
    enum service_health_state state)
{
    switch (state) {
    case SERVICE_HEALTH_PENDING:
        return "PENDING";
    case SERVICE_HEALTH_READY:
        return "READY";
    case SERVICE_HEALTH_LIVENESS_FAILED:
        return "LIVENESS_FAILED";
    case SERVICE_HEALTH_READINESS_TIMEOUT:
        return "READINESS_TIMEOUT";
    default:
        return NULL;
    }
}

static const char *appliance_https_service_phase_string(
    enum https_service_lifecycle_phase phase)
{
    switch (phase) {
    case HTTPS_SERVICE_LIFECYCLE_PHASE_IDLE:
        return "IDLE";
    case HTTPS_SERVICE_LIFECYCLE_PHASE_LISTENING:
        return "LISTENING";
    case HTTPS_SERVICE_LIFECYCLE_PHASE_DRAINING:
        return "DRAINING";
    case HTTPS_SERVICE_LIFECYCLE_PHASE_DRAINED:
        return "DRAINED";
    case HTTPS_SERVICE_LIFECYCLE_PHASE_STOPPED:
        return "STOPPED";
    case HTTPS_SERVICE_LIFECYCLE_PHASE_RESTARTED:
        return "RESTARTED";
    default:
        return NULL;
    }
}

enum appliance_https_service_status appliance_https_service_render(
    const struct appliance_https_service_snapshot *snapshot,
    char *output,
    size_t capacity,
    size_t *output_length)
{
    char rendered[APPLIANCE_HTTPS_SERVICE_OUTPUT_MAX + 1U];
    size_t offset = 0U;
    size_t index;
    const char *health;
    const char *service_health;
    const char *phase;

    if (snapshot == NULL || output == NULL || output_length == NULL) {
        return APPLIANCE_HTTPS_SERVICE_NULL;
    }
    if (!snapshot->published ||
        snapshot->version != APPLIANCE_HTTPS_SERVICE_VERSION ||
        !appliance_https_service_valid_state(snapshot->state) ||
        !appliance_https_service_valid_failure(snapshot->failure)) {
        return APPLIANCE_HTTPS_SERVICE_BAD_STATE;
    }
    health = appliance_https_service_health_string(
        snapshot->manager_health);
    service_health = appliance_https_service_service_health_string(
        snapshot->service_health);
    phase = appliance_https_service_phase_string(
        snapshot->lifecycle_phase);
    if (health == NULL || service_health == NULL || phase == NULL) {
        return APPLIANCE_HTTPS_SERVICE_BAD_STATE;
    }

    if (!appliance_https_service_append_text(
            rendered, &offset, "appliance_https v=") ||
        !appliance_https_service_append_u64(
            rendered, &offset, snapshot->version) ||
        !appliance_https_service_append_text(
            rendered, &offset, " state=") ||
        !appliance_https_service_append_text(
            rendered, &offset,
            appliance_https_service_state_string(snapshot->state)) ||
        !appliance_https_service_append_text(
            rendered, &offset, " service=https") ||
        !appliance_https_service_append_text(
            rendered, &offset, " manager=") ||
        !appliance_https_service_append_text(
            rendered, &offset,
            appliance_service_manager_state_string(
                snapshot->manager_state)) ||
        !appliance_https_service_append_text(
            rendered, &offset, " health=") ||
        !appliance_https_service_append_text(
            rendered, &offset, health) ||
        !appliance_https_service_append_text(
            rendered, &offset, " service_health=") ||
        !appliance_https_service_append_text(
            rendered, &offset, service_health) ||
        !appliance_https_service_append_text(
            rendered, &offset, " lifecycle=") ||
        !appliance_https_service_append_text(
            rendered, &offset, phase) ||
        !appliance_https_service_append_text(
            rendered, &offset, " generation=") ||
        !appliance_https_service_append_u64(
            rendered, &offset, snapshot->lifecycle_generation) ||
        !appliance_https_service_append_text(
            rendered, &offset, " connections=") ||
        !appliance_https_service_append_u64(
            rendered, &offset, snapshot->active_connection_count) ||
        !appliance_https_service_append_text(
            rendered, &offset, " start=") ||
        !appliance_https_service_append_u64(
            rendered, &offset, snapshot->start_count) ||
        !appliance_https_service_append_text(
            rendered, &offset, " stop=") ||
        !appliance_https_service_append_u64(
            rendered, &offset, snapshot->stop_count) ||
        !appliance_https_service_append_text(
            rendered, &offset, " restart=") ||
        !appliance_https_service_append_u64(
            rendered, &offset, snapshot->restart_count) ||
        !appliance_https_service_append_text(
            rendered, &offset, " failure=") ||
        !appliance_https_service_append_text(
            rendered, &offset,
            appliance_https_service_failure_string(
                snapshot->failure)) ||
        !appliance_https_service_append_char(
            rendered, &offset, '\n')) {
        return APPLIANCE_HTTPS_SERVICE_OVERFLOW;
    }
    if (capacity <= offset) {
        return APPLIANCE_HTTPS_SERVICE_OVERFLOW;
    }
    for (index = 0U; index < offset; ++index) {
        output[index] = rendered[index];
    }
    output[offset] = '\0';
    *output_length = offset;
    return APPLIANCE_HTTPS_SERVICE_OK;
}

bool appliance_https_service_published(
    const struct appliance_https_service *service)
{
    return appliance_https_service_initialized(service);
}

enum appliance_https_service_state appliance_https_service_state(
    const struct appliance_https_service *service)
{
    return appliance_https_service_initialized(service)
        ? service->state
        : APPLIANCE_HTTPS_SERVICE_STATE_STOPPED;
}

enum appliance_https_service_failure appliance_https_service_failure(
    const struct appliance_https_service *service)
{
    return appliance_https_service_initialized(service)
        ? service->failure
        : APPLIANCE_HTTPS_SERVICE_FAILURE_NONE;
}

const char *appliance_https_service_state_string(
    enum appliance_https_service_state state)
{
    switch (state) {
    case APPLIANCE_HTTPS_SERVICE_STATE_STOPPED:
        return "STOPPED";
    case APPLIANCE_HTTPS_SERVICE_STATE_STARTING:
        return "STARTING";
    case APPLIANCE_HTTPS_SERVICE_STATE_RUNNING:
        return "RUNNING";
    case APPLIANCE_HTTPS_SERVICE_STATE_STOPPING:
        return "STOPPING";
    case APPLIANCE_HTTPS_SERVICE_STATE_FAILED:
        return "FAILED";
    default:
        return NULL;
    }
}

const char *appliance_https_service_failure_string(
    enum appliance_https_service_failure failure)
{
    switch (failure) {
    case APPLIANCE_HTTPS_SERVICE_FAILURE_NONE:
        return "NONE";
    case APPLIANCE_HTTPS_SERVICE_FAILURE_START:
        return "START";
    case APPLIANCE_HTTPS_SERVICE_FAILURE_READY:
        return "READY";
    case APPLIANCE_HTTPS_SERVICE_FAILURE_STOP:
        return "STOP";
    case APPLIANCE_HTTPS_SERVICE_FAILURE_RESTART:
        return "RESTART";
    case APPLIANCE_HTTPS_SERVICE_FAILURE_SNAPSHOT:
        return "SNAPSHOT";
    default:
        return NULL;
    }
}

const char *appliance_https_service_status_string(
    enum appliance_https_service_status status)
{
    switch (status) {
    case APPLIANCE_HTTPS_SERVICE_OK:
        return "APPLIANCE_HTTPS_SERVICE_OK";
    case APPLIANCE_HTTPS_SERVICE_NULL:
        return "APPLIANCE_HTTPS_SERVICE_NULL";
    case APPLIANCE_HTTPS_SERVICE_BAD_STATE:
        return "APPLIANCE_HTTPS_SERVICE_BAD_STATE";
    case APPLIANCE_HTTPS_SERVICE_ALREADY_PUBLISHED:
        return "APPLIANCE_HTTPS_SERVICE_ALREADY_PUBLISHED";
    case APPLIANCE_HTTPS_SERVICE_UNPUBLISHED:
        return "APPLIANCE_HTTPS_SERVICE_UNPUBLISHED";
    case APPLIANCE_HTTPS_SERVICE_INVALID_MANAGER:
        return "APPLIANCE_HTTPS_SERVICE_INVALID_MANAGER";
    case APPLIANCE_HTTPS_SERVICE_INVALID_DESCRIPTOR:
        return "APPLIANCE_HTTPS_SERVICE_INVALID_DESCRIPTOR";
    case APPLIANCE_HTTPS_SERVICE_INVALID_CONFIG:
        return "APPLIANCE_HTTPS_SERVICE_INVALID_CONFIG";
    case APPLIANCE_HTTPS_SERVICE_INVALID_READINESS:
        return "APPLIANCE_HTTPS_SERVICE_INVALID_READINESS";
    case APPLIANCE_HTTPS_SERVICE_DEPENDENCY_UNAVAILABLE:
        return "APPLIANCE_HTTPS_SERVICE_DEPENDENCY_UNAVAILABLE";
    case APPLIANCE_HTTPS_SERVICE_START_FAILED:
        return "APPLIANCE_HTTPS_SERVICE_START_FAILED";
    case APPLIANCE_HTTPS_SERVICE_READY_FAILED:
        return "APPLIANCE_HTTPS_SERVICE_READY_FAILED";
    case APPLIANCE_HTTPS_SERVICE_STOP_PENDING:
        return "APPLIANCE_HTTPS_SERVICE_STOP_PENDING";
    case APPLIANCE_HTTPS_SERVICE_STOP_FAILED:
        return "APPLIANCE_HTTPS_SERVICE_STOP_FAILED";
    case APPLIANCE_HTTPS_SERVICE_RESTART_FAILED:
        return "APPLIANCE_HTTPS_SERVICE_RESTART_FAILED";
    case APPLIANCE_HTTPS_SERVICE_SNAPSHOT_FAILED:
        return "APPLIANCE_HTTPS_SERVICE_SNAPSHOT_FAILED";
    case APPLIANCE_HTTPS_SERVICE_OVERFLOW:
        return "APPLIANCE_HTTPS_SERVICE_OVERFLOW";
    default:
        return NULL;
    }
}

#else

typedef int appliance_https_service_translation_unit_guard;

#endif
