#include "https_service_lifecycle.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#if defined(HTTPS_SERVICE_LIFECYCLE_HOST_TEST)

#define HTTPS_SERVICE_LIFECYCLE_HALF_RANGE UINT32_C(0x7FFFFFFF)
#define HTTPS_SERVICE_LIFECYCLE_ALL_SLOTS UINT32_C(0x0F)

static bool https_service_lifecycle_generation_valid(uint32_t generation)
{
    return generation >= HTTPS_SERVICE_LIFECYCLE_GENERATION_MIN &&
           generation <= HTTPS_SERVICE_LIFECYCLE_GENERATION_MAX;
}

static bool https_service_lifecycle_timeout_valid(uint32_t timeout_ms)
{
    return timeout_ms >= HTTPS_SERVICE_LIFECYCLE_DRAIN_TIMEOUT_MIN &&
           timeout_ms <= HTTPS_SERVICE_LIFECYCLE_DRAIN_TIMEOUT_MAX;
}

static bool https_service_lifecycle_config_valid(
    const struct https_service_lifecycle_config *config)
{
    return config != NULL &&
           config->version == HTTPS_SERVICE_LIFECYCLE_VERSION &&
           https_service_lifecycle_timeout_valid(
               config->drain_timeout_ms) &&
           config->connection_capacity ==
               HTTPS_SERVICE_LIFECYCLE_CONNECTION_CAPACITY &&
           config->tls_context_capacity ==
               HTTPS_SERVICE_LIFECYCLE_TLS_CONTEXT_CAPACITY &&
           config->http_parser_capacity ==
               HTTPS_SERVICE_LIFECYCLE_HTTP_PARSER_CAPACITY &&
           config->packet_pool_capacity >=
               HTTPS_SERVICE_LIFECYCLE_PACKET_POOL_CAPACITY_MIN &&
           config->packet_pool_capacity <=
               HTTPS_SERVICE_LIFECYCLE_PACKET_POOL_CAPACITY_MAX;
}

static bool https_service_lifecycle_ops_valid(
    const struct https_service_lifecycle_ops *ops)
{
    return ops != NULL &&
           ops->stop_admission != NULL &&
           ops->drain_connection != NULL &&
           ops->abort_connection != NULL &&
           ops->zeroize_tls != NULL &&
           ops->release_http != NULL &&
           ops->release_packet != NULL &&
           ops->restart_admission != NULL &&
           ops->context != NULL;
}

static bool https_service_lifecycle_phase_valid(
    enum https_service_lifecycle_phase phase)
{
    return phase >= HTTPS_SERVICE_LIFECYCLE_PHASE_IDLE &&
           phase <= HTTPS_SERVICE_LIFECYCLE_PHASE_RESTARTED;
}

static bool https_service_lifecycle_valid(
    const struct https_service_lifecycle *lifecycle)
{
    if (lifecycle == NULL ||
        !lifecycle->published ||
        lifecycle->version != HTTPS_SERVICE_LIFECYCLE_VERSION ||
        !https_service_lifecycle_phase_valid(lifecycle->phase) ||
        !https_service_lifecycle_timeout_valid(
            lifecycle->drain_timeout_ms) ||
        lifecycle->connection_capacity !=
            HTTPS_SERVICE_LIFECYCLE_CONNECTION_CAPACITY ||
        lifecycle->tls_context_capacity !=
            HTTPS_SERVICE_LIFECYCLE_TLS_CONTEXT_CAPACITY ||
        lifecycle->http_parser_capacity !=
            HTTPS_SERVICE_LIFECYCLE_HTTP_PARSER_CAPACITY ||
        lifecycle->packet_pool_capacity <
            HTTPS_SERVICE_LIFECYCLE_PACKET_POOL_CAPACITY_MIN ||
        lifecycle->packet_pool_capacity >
            HTTPS_SERVICE_LIFECYCLE_PACKET_POOL_CAPACITY_MAX) {
        return false;
    }
    if (lifecycle->phase == HTTPS_SERVICE_LIFECYCLE_PHASE_IDLE) {
        return lifecycle->generation == 0U &&
               lifecycle->drain_deadline_ms == 0U &&
               lifecycle->active_slots == 0U &&
               lifecycle->stopped_count == 0U &&
               lifecycle->drained_count == 0U &&
               lifecycle->aborted_count == 0U &&
               lifecycle->zeroized_count == 0U &&
               lifecycle->http_released_count == 0U &&
               lifecycle->packet_released_count == 0U &&
               lifecycle->restarted_count == 0U &&
               https_service_lifecycle_ops_valid(&lifecycle->ops);
    }
    return https_service_lifecycle_generation_valid(lifecycle->generation) &&
           https_service_lifecycle_ops_valid(&lifecycle->ops);
}

static uint32_t https_service_lifecycle_active_count(uint32_t slots)
{
    uint32_t count = 0U;
    uint32_t index;

    for (index = 0U;
         index < HTTPS_SERVICE_LIFECYCLE_CONNECTION_CAPACITY;
         ++index) {
        if ((slots & (UINT32_C(1) << index)) != 0U) {
            ++count;
        }
    }
    return count;
}

static bool https_service_lifecycle_deadline_reached(
    uint32_t now_ms,
    uint32_t due_ms)
{
    return (uint32_t)(now_ms - due_ms) <=
        HTTPS_SERVICE_LIFECYCLE_HALF_RANGE;
}

enum https_service_lifecycle_status https_service_lifecycle_init(
    struct https_service_lifecycle *lifecycle,
    const struct https_service_lifecycle_config *config,
    const struct https_service_lifecycle_ops *ops)
{
    if (lifecycle == NULL || config == NULL || ops == NULL) {
        return HTTPS_SERVICE_LIFECYCLE_NULL;
    }
    if (lifecycle->published) {
        return HTTPS_SERVICE_LIFECYCLE_ALREADY_PUBLISHED;
    }
    if (lifecycle->version != 0U) {
        return HTTPS_SERVICE_LIFECYCLE_INVALID_VERSION;
    }
    if (config->version != HTTPS_SERVICE_LIFECYCLE_VERSION) {
        return HTTPS_SERVICE_LIFECYCLE_INVALID_VERSION;
    }
    if (!https_service_lifecycle_config_valid(config)) {
        return HTTPS_SERVICE_LIFECYCLE_INVALID_CONFIG;
    }
    if (!https_service_lifecycle_ops_valid(ops)) {
        return HTTPS_SERVICE_LIFECYCLE_INVALID_OPS;
    }

    memset(lifecycle, 0, sizeof(*lifecycle));
    lifecycle->published = true;
    lifecycle->version = HTTPS_SERVICE_LIFECYCLE_VERSION;
    lifecycle->phase = HTTPS_SERVICE_LIFECYCLE_PHASE_IDLE;
    lifecycle->drain_timeout_ms = config->drain_timeout_ms;
    lifecycle->connection_capacity = config->connection_capacity;
    lifecycle->tls_context_capacity = config->tls_context_capacity;
    lifecycle->http_parser_capacity = config->http_parser_capacity;
    lifecycle->packet_pool_capacity = config->packet_pool_capacity;
    lifecycle->ops = *ops;
    return HTTPS_SERVICE_LIFECYCLE_OK;
}

enum https_service_lifecycle_status https_service_lifecycle_start(
    struct https_service_lifecycle *lifecycle,
    uint32_t generation)
{
    if (lifecycle == NULL) {
        return HTTPS_SERVICE_LIFECYCLE_NULL;
    }
    if (!https_service_lifecycle_valid(lifecycle)) {
        return HTTPS_SERVICE_LIFECYCLE_BAD_STATE;
    }
    if (lifecycle->phase != HTTPS_SERVICE_LIFECYCLE_PHASE_IDLE) {
        return HTTPS_SERVICE_LIFECYCLE_INVALID_PHASE;
    }
    if (!https_service_lifecycle_generation_valid(generation)) {
        return HTTPS_SERVICE_LIFECYCLE_INVALID_GENERATION;
    }

    lifecycle->generation = generation;
    lifecycle->active_slots = HTTPS_SERVICE_LIFECYCLE_ALL_SLOTS;
    lifecycle->phase = HTTPS_SERVICE_LIFECYCLE_PHASE_LISTENING;
    return HTTPS_SERVICE_LIFECYCLE_OK;
}

enum https_service_lifecycle_status https_service_lifecycle_stop_admission(
    struct https_service_lifecycle *lifecycle,
    uint32_t now_ms)
{
    if (lifecycle == NULL) {
        return HTTPS_SERVICE_LIFECYCLE_NULL;
    }
    if (!https_service_lifecycle_valid(lifecycle)) {
        return HTTPS_SERVICE_LIFECYCLE_BAD_STATE;
    }
    if (lifecycle->phase != HTTPS_SERVICE_LIFECYCLE_PHASE_LISTENING &&
        lifecycle->phase != HTTPS_SERVICE_LIFECYCLE_PHASE_RESTARTED) {
        return HTTPS_SERVICE_LIFECYCLE_INVALID_PHASE;
    }
    if (!lifecycle->ops.stop_admission(
            lifecycle->ops.context, lifecycle->generation)) {
        return HTTPS_SERVICE_LIFECYCLE_ADMISSION_STOP_FAILED;
    }

    lifecycle->drain_deadline_ms =
        (uint32_t)(now_ms + lifecycle->drain_timeout_ms);
    lifecycle->phase = HTTPS_SERVICE_LIFECYCLE_PHASE_DRAINING;
    ++lifecycle->stopped_count;
    return HTTPS_SERVICE_LIFECYCLE_OK;
}

enum https_service_lifecycle_status https_service_lifecycle_step(
    struct https_service_lifecycle *lifecycle,
    uint32_t now_ms)
{
    struct https_service_lifecycle candidate;
    bool deadline_reached;
    uint32_t index;

    if (lifecycle == NULL) {
        return HTTPS_SERVICE_LIFECYCLE_NULL;
    }
    if (!https_service_lifecycle_valid(lifecycle)) {
        return HTTPS_SERVICE_LIFECYCLE_BAD_STATE;
    }
    if (lifecycle->phase != HTTPS_SERVICE_LIFECYCLE_PHASE_DRAINING) {
        return HTTPS_SERVICE_LIFECYCLE_INVALID_PHASE;
    }

    candidate = *lifecycle;
    deadline_reached = https_service_lifecycle_deadline_reached(
        now_ms, lifecycle->drain_deadline_ms);
    for (index = 0U;
         index < HTTPS_SERVICE_LIFECYCLE_CONNECTION_CAPACITY;
         ++index) {
        uint32_t bit = UINT32_C(1) << index;

        if ((candidate.active_slots & bit) == 0U) {
            continue;
        }
        if (!deadline_reached) {
            if (!candidate.ops.drain_connection(
                    candidate.ops.context,
                    candidate.generation,
                    index)) {
                continue;
            }
            candidate.active_slots &= ~bit;
            ++candidate.drained_count;
        } else {
            if (!candidate.ops.abort_connection(
                    candidate.ops.context,
                    candidate.generation,
                    index)) {
                return HTTPS_SERVICE_LIFECYCLE_DRAIN_FAILED;
            }
            candidate.active_slots &= ~bit;
            ++candidate.aborted_count;
        }
    }
    if (candidate.active_slots == 0U) {
        candidate.phase = HTTPS_SERVICE_LIFECYCLE_PHASE_DRAINED;
    }
    *lifecycle = candidate;
    return HTTPS_SERVICE_LIFECYCLE_OK;
}

enum https_service_lifecycle_status https_service_lifecycle_finish_drain(
    struct https_service_lifecycle *lifecycle)
{
    struct https_service_lifecycle candidate;

    if (lifecycle == NULL) {
        return HTTPS_SERVICE_LIFECYCLE_NULL;
    }
    if (!https_service_lifecycle_valid(lifecycle)) {
        return HTTPS_SERVICE_LIFECYCLE_BAD_STATE;
    }
    if (lifecycle->phase != HTTPS_SERVICE_LIFECYCLE_PHASE_DRAINING &&
        lifecycle->phase != HTTPS_SERVICE_LIFECYCLE_PHASE_DRAINED) {
        return HTTPS_SERVICE_LIFECYCLE_INVALID_PHASE;
    }
    if (lifecycle->active_slots != 0U) {
        return HTTPS_SERVICE_LIFECYCLE_NOT_DRAINED;
    }

    candidate = *lifecycle;
    if (!candidate.ops.zeroize_tls(
            candidate.ops.context,
            candidate.generation,
            candidate.tls_context_capacity)) {
        return HTTPS_SERVICE_LIFECYCLE_ZEROIZE_FAILED;
    }
    if (!candidate.ops.release_http(
            candidate.ops.context,
            candidate.generation,
            candidate.http_parser_capacity)) {
        return HTTPS_SERVICE_LIFECYCLE_HTTP_RELEASE_FAILED;
    }
    if (!candidate.ops.release_packet(
            candidate.ops.context,
            candidate.generation,
            candidate.packet_pool_capacity)) {
        return HTTPS_SERVICE_LIFECYCLE_PACKET_RELEASE_FAILED;
    }

    candidate.phase = HTTPS_SERVICE_LIFECYCLE_PHASE_STOPPED;
    candidate.drain_deadline_ms = 0U;
    candidate.zeroized_count += candidate.tls_context_capacity;
    candidate.http_released_count += candidate.http_parser_capacity;
    candidate.packet_released_count += candidate.packet_pool_capacity;
    *lifecycle = candidate;
    return HTTPS_SERVICE_LIFECYCLE_OK;
}

enum https_service_lifecycle_status https_service_lifecycle_restart(
    struct https_service_lifecycle *lifecycle)
{
    struct https_service_lifecycle candidate;
    uint32_t next_generation;

    if (lifecycle == NULL) {
        return HTTPS_SERVICE_LIFECYCLE_NULL;
    }
    if (!https_service_lifecycle_valid(lifecycle)) {
        return HTTPS_SERVICE_LIFECYCLE_BAD_STATE;
    }
    if (lifecycle->phase != HTTPS_SERVICE_LIFECYCLE_PHASE_STOPPED) {
        return HTTPS_SERVICE_LIFECYCLE_INVALID_PHASE;
    }

    next_generation =
        lifecycle->generation == HTTPS_SERVICE_LIFECYCLE_GENERATION_MAX
        ? 0U
        : lifecycle->generation + 1U;
    if (!https_service_lifecycle_generation_valid(next_generation)) {
        return HTTPS_SERVICE_LIFECYCLE_GENERATION_EXHAUSTED;
    }
    if (!lifecycle->ops.restart_admission(
            lifecycle->ops.context, next_generation)) {
        return HTTPS_SERVICE_LIFECYCLE_RESTART_FAILED;
    }

    candidate = *lifecycle;
    candidate.generation = next_generation;
    candidate.phase = HTTPS_SERVICE_LIFECYCLE_PHASE_RESTARTED;
    candidate.drain_deadline_ms = 0U;
    candidate.active_slots = HTTPS_SERVICE_LIFECYCLE_ALL_SLOTS;
    candidate.drained_count = 0U;
    candidate.aborted_count = 0U;
    candidate.zeroized_count = 0U;
    candidate.http_released_count = 0U;
    candidate.packet_released_count = 0U;
    ++candidate.restarted_count;
    *lifecycle = candidate;
    return HTTPS_SERVICE_LIFECYCLE_OK;
}

enum https_service_lifecycle_status https_service_lifecycle_snapshot(
    const struct https_service_lifecycle *lifecycle,
    struct https_service_lifecycle_snapshot *snapshot_out)
{
    if (lifecycle == NULL || snapshot_out == NULL) {
        return HTTPS_SERVICE_LIFECYCLE_NULL;
    }
    if (!https_service_lifecycle_valid(lifecycle)) {
        return HTTPS_SERVICE_LIFECYCLE_UNPUBLISHED;
    }

    memset(snapshot_out, 0, sizeof(*snapshot_out));
    snapshot_out->version = HTTPS_SERVICE_LIFECYCLE_VERSION;
    snapshot_out->phase = lifecycle->phase;
    snapshot_out->generation = lifecycle->generation;
    snapshot_out->drain_deadline_ms = lifecycle->drain_deadline_ms;
    snapshot_out->active_connection_count =
        https_service_lifecycle_active_count(lifecycle->active_slots);
    snapshot_out->stopped_count = lifecycle->stopped_count;
    snapshot_out->drained_count = lifecycle->drained_count;
    snapshot_out->aborted_count = lifecycle->aborted_count;
    snapshot_out->zeroized_count = lifecycle->zeroized_count;
    snapshot_out->http_released_count = lifecycle->http_released_count;
    snapshot_out->packet_released_count = lifecycle->packet_released_count;
    snapshot_out->restarted_count = lifecycle->restarted_count;
    return HTTPS_SERVICE_LIFECYCLE_OK;
}

bool https_service_lifecycle_published(
    const struct https_service_lifecycle *lifecycle)
{
    return https_service_lifecycle_valid(lifecycle);
}

enum https_service_lifecycle_phase https_service_lifecycle_phase(
    const struct https_service_lifecycle *lifecycle)
{
    return https_service_lifecycle_valid(lifecycle)
        ? lifecycle->phase
        : HTTPS_SERVICE_LIFECYCLE_PHASE_IDLE;
}

uint32_t https_service_lifecycle_generation(
    const struct https_service_lifecycle *lifecycle)
{
    return https_service_lifecycle_valid(lifecycle)
        ? lifecycle->generation
        : 0U;
}

uint32_t https_service_lifecycle_active_connection_count(
    const struct https_service_lifecycle *lifecycle)
{
    return https_service_lifecycle_valid(lifecycle)
        ? https_service_lifecycle_active_count(lifecycle->active_slots)
        : 0U;
}

const char *https_service_lifecycle_phase_string(
    enum https_service_lifecycle_phase phase)
{
    switch (phase) {
    case HTTPS_SERVICE_LIFECYCLE_PHASE_IDLE:
        return "HTTPS_SERVICE_LIFECYCLE_PHASE_IDLE";
    case HTTPS_SERVICE_LIFECYCLE_PHASE_LISTENING:
        return "HTTPS_SERVICE_LIFECYCLE_PHASE_LISTENING";
    case HTTPS_SERVICE_LIFECYCLE_PHASE_DRAINING:
        return "HTTPS_SERVICE_LIFECYCLE_PHASE_DRAINING";
    case HTTPS_SERVICE_LIFECYCLE_PHASE_DRAINED:
        return "HTTPS_SERVICE_LIFECYCLE_PHASE_DRAINED";
    case HTTPS_SERVICE_LIFECYCLE_PHASE_STOPPED:
        return "HTTPS_SERVICE_LIFECYCLE_PHASE_STOPPED";
    case HTTPS_SERVICE_LIFECYCLE_PHASE_RESTARTED:
        return "HTTPS_SERVICE_LIFECYCLE_PHASE_RESTARTED";
    default:
        return NULL;
    }
}

const char *https_service_lifecycle_status_string(
    enum https_service_lifecycle_status status)
{
    switch (status) {
    case HTTPS_SERVICE_LIFECYCLE_OK:
        return "HTTPS_SERVICE_LIFECYCLE_OK";
    case HTTPS_SERVICE_LIFECYCLE_NULL:
        return "HTTPS_SERVICE_LIFECYCLE_NULL";
    case HTTPS_SERVICE_LIFECYCLE_BAD_STATE:
        return "HTTPS_SERVICE_LIFECYCLE_BAD_STATE";
    case HTTPS_SERVICE_LIFECYCLE_INVALID_VERSION:
        return "HTTPS_SERVICE_LIFECYCLE_INVALID_VERSION";
    case HTTPS_SERVICE_LIFECYCLE_INVALID_CONFIG:
        return "HTTPS_SERVICE_LIFECYCLE_INVALID_CONFIG";
    case HTTPS_SERVICE_LIFECYCLE_ALREADY_PUBLISHED:
        return "HTTPS_SERVICE_LIFECYCLE_ALREADY_PUBLISHED";
    case HTTPS_SERVICE_LIFECYCLE_UNPUBLISHED:
        return "HTTPS_SERVICE_LIFECYCLE_UNPUBLISHED";
    case HTTPS_SERVICE_LIFECYCLE_INVALID_PHASE:
        return "HTTPS_SERVICE_LIFECYCLE_INVALID_PHASE";
    case HTTPS_SERVICE_LIFECYCLE_INVALID_GENERATION:
        return "HTTPS_SERVICE_LIFECYCLE_INVALID_GENERATION";
    case HTTPS_SERVICE_LIFECYCLE_INVALID_OPS:
        return "HTTPS_SERVICE_LIFECYCLE_INVALID_OPS";
    case HTTPS_SERVICE_LIFECYCLE_ADMISSION_STOP_FAILED:
        return "HTTPS_SERVICE_LIFECYCLE_ADMISSION_STOP_FAILED";
    case HTTPS_SERVICE_LIFECYCLE_DRAIN_FAILED:
        return "HTTPS_SERVICE_LIFECYCLE_DRAIN_FAILED";
    case HTTPS_SERVICE_LIFECYCLE_NOT_DRAINED:
        return "HTTPS_SERVICE_LIFECYCLE_NOT_DRAINED";
    case HTTPS_SERVICE_LIFECYCLE_ZEROIZE_FAILED:
        return "HTTPS_SERVICE_LIFECYCLE_ZEROIZE_FAILED";
    case HTTPS_SERVICE_LIFECYCLE_HTTP_RELEASE_FAILED:
        return "HTTPS_SERVICE_LIFECYCLE_HTTP_RELEASE_FAILED";
    case HTTPS_SERVICE_LIFECYCLE_PACKET_RELEASE_FAILED:
        return "HTTPS_SERVICE_LIFECYCLE_PACKET_RELEASE_FAILED";
    case HTTPS_SERVICE_LIFECYCLE_RESTART_FAILED:
        return "HTTPS_SERVICE_LIFECYCLE_RESTART_FAILED";
    case HTTPS_SERVICE_LIFECYCLE_GENERATION_EXHAUSTED:
        return "HTTPS_SERVICE_LIFECYCLE_GENERATION_EXHAUSTED";
    default:
        return NULL;
    }
}

#else

typedef int https_service_lifecycle_translation_unit_guard;

#endif
