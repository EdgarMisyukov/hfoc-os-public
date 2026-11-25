#include "https_health.h"

#include "audit.h"
#include "event_ring.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#if defined(HTTPS_HEALTH_HOST_TEST)

static bool https_health_listener_state_valid(
    enum https_health_listener_state state)
{
    return state >= HTTPS_HEALTH_LISTENER_CLOSED &&
           state <= HTTPS_HEALTH_LISTENER_FAILED;
}

static bool https_health_failure_category_valid(
    enum https_health_failure_category category)
{
    return category >= HTTPS_HEALTH_FAILURE_TLS_HANDSHAKE &&
           category <= HTTPS_HEALTH_FAILURE_RESOURCE_EXHAUSTION;
}

static bool https_health_metrics_valid(
    const struct https_health_metrics *metrics)
{
    uint32_t accepted_rejected;
    uint32_t index;

    if (metrics->version != HTTPS_HEALTH_VERSION ||
        !https_health_listener_state_valid(metrics->listener_state) ||
        metrics->connection_count > HTTPS_HEALTH_CONNECTION_CAPACITY) {
        return false;
    }
    if (metrics->accepted_connections >
            UINT32_MAX - metrics->rejected_connections) {
        return false;
    }
    accepted_rejected = metrics->accepted_connections +
                        metrics->rejected_connections;
    if (metrics->failed_connections >
        UINT32_MAX - accepted_rejected) {
        return false;
    }
    for (index = 0U;
         index < HTTPS_HEALTH_FAILURE_CATEGORY_COUNT;
         ++index) {
        if (!https_health_failure_category_valid(
                (enum https_health_failure_category)index)) {
            return false;
        }
    }
    return true;
}

static uint32_t https_health_audit_result(
    enum https_health_listener_state state)
{
    return state == HTTPS_HEALTH_LISTENER_FAILED ?
        HTTPS_HEALTH_AUDIT_RESULT_FAILED :
        HTTPS_HEALTH_AUDIT_RESULT_OK;
}

enum https_health_status https_health_init(
    struct https_health_store *store)
{
    if (store == NULL) {
        return HTTPS_HEALTH_NULL;
    }
    if (store->initialized) {
        return HTTPS_HEALTH_BAD_STATE;
    }

    memset(store, 0, sizeof(*store));
    store->version = HTTPS_HEALTH_VERSION;
    store->initialized = true;
    return HTTPS_HEALTH_OK;
}

enum https_health_status https_health_publish(
    struct https_health_store *store,
    const struct https_health_metrics *metrics,
    struct event_ring *audit_ring,
    uint64_t *sequence_out)
{
    struct https_health_metrics candidate;
    struct audit_record record;
    uint32_t correlation_id;
    uint32_t audit_result;
    enum audit_status audit_status;

    if (store == NULL || metrics == NULL) {
        return HTTPS_HEALTH_NULL;
    }
    if (!store->initialized || store->version != HTTPS_HEALTH_VERSION) {
        return HTTPS_HEALTH_BAD_STATE;
    }
    if (metrics->version != HTTPS_HEALTH_VERSION) {
        return HTTPS_HEALTH_BAD_VERSION;
    }
    if (!https_health_listener_state_valid(metrics->listener_state)) {
        return HTTPS_HEALTH_BAD_LISTENER_STATE;
    }
    if (metrics->connection_count > HTTPS_HEALTH_CONNECTION_CAPACITY) {
        return HTTPS_HEALTH_BAD_CONNECTION_COUNT;
    }
    if (!https_health_metrics_valid(metrics)) {
        return HTTPS_HEALTH_BAD_COUNTER;
    }
    if (store->correlation_id == UINT32_MAX) {
        return HTTPS_HEALTH_CORRELATION_OVERFLOW;
    }
    if (store->generation >= HTTPS_HEALTH_GENERATION_MAX) {
        return HTTPS_HEALTH_GENERATION_OVERFLOW;
    }

    correlation_id = store->correlation_id + 1U;
    audit_result = https_health_audit_result(metrics->listener_state);
    if (audit_ring != NULL) {
        if (!event_ring_is_published(audit_ring) ||
            event_ring_count(audit_ring) >= KERNEL_EVENT_RING_CAPACITY) {
            return HTTPS_HEALTH_AUDIT_FAILED;
        }
        audit_status = audit_record_init(
            &record,
            AUDIT_RECORD_NETWORK,
            HTTPS_HEALTH_AUDIT_OPERATION,
            audit_result,
            0U,
            0U,
            store->generation + 1U,
            correlation_id);
        if (audit_status != AUDIT_OK) {
            return HTTPS_HEALTH_AUDIT_FAILED;
        }
        audit_status = audit_record_append(
            audit_ring, &record, sequence_out);
        if (audit_status != AUDIT_OK) {
            return HTTPS_HEALTH_AUDIT_FAILED;
        }
    }

    candidate = *metrics;
    ++store->generation;
    store->correlation_id = correlation_id;
    if (metrics->connection_count > store->high_water) {
        store->high_water = metrics->connection_count;
    }
    store->published_metrics = candidate;
    store->published = true;
    return HTTPS_HEALTH_OK;
}

enum https_health_status https_health_snapshot(
    const struct https_health_store *store,
    struct https_health_snapshot *snapshot_out)
{
    if (store == NULL || snapshot_out == NULL) {
        return HTTPS_HEALTH_NULL;
    }
    if (!store->initialized || store->version != HTTPS_HEALTH_VERSION) {
        return HTTPS_HEALTH_BAD_STATE;
    }
    if (!store->published) {
        return HTTPS_HEALTH_NOT_PUBLISHED;
    }

    memset(snapshot_out, 0, sizeof(*snapshot_out));
    snapshot_out->version = HTTPS_HEALTH_VERSION;
    snapshot_out->generation = store->generation;
    snapshot_out->high_water = store->high_water;
    snapshot_out->correlation_id = store->correlation_id;
    snapshot_out->metrics = store->published_metrics;
    return HTTPS_HEALTH_OK;
}

const char *https_health_status_string(
    enum https_health_status status)
{
    switch (status) {
    case HTTPS_HEALTH_OK:
        return "HTTPS_HEALTH_OK";
    case HTTPS_HEALTH_NULL:
        return "HTTPS_HEALTH_NULL";
    case HTTPS_HEALTH_BAD_STATE:
        return "HTTPS_HEALTH_BAD_STATE";
    case HTTPS_HEALTH_BAD_VERSION:
        return "HTTPS_HEALTH_BAD_VERSION";
    case HTTPS_HEALTH_NOT_PUBLISHED:
        return "HTTPS_HEALTH_NOT_PUBLISHED";
    case HTTPS_HEALTH_BAD_LISTENER_STATE:
        return "HTTPS_HEALTH_BAD_LISTENER_STATE";
    case HTTPS_HEALTH_BAD_CONNECTION_COUNT:
        return "HTTPS_HEALTH_BAD_CONNECTION_COUNT";
    case HTTPS_HEALTH_BAD_COUNTER:
        return "HTTPS_HEALTH_BAD_COUNTER";
    case HTTPS_HEALTH_CORRELATION_OVERFLOW:
        return "HTTPS_HEALTH_CORRELATION_OVERFLOW";
    case HTTPS_HEALTH_GENERATION_OVERFLOW:
        return "HTTPS_HEALTH_GENERATION_OVERFLOW";
    case HTTPS_HEALTH_AUDIT_FAILED:
        return "HTTPS_HEALTH_AUDIT_FAILED";
    default:
        return NULL;
    }
}

const char *https_health_listener_state_string(
    enum https_health_listener_state state)
{
    switch (state) {
    case HTTPS_HEALTH_LISTENER_CLOSED:
        return "HTTPS_HEALTH_LISTENER_CLOSED";
    case HTTPS_HEALTH_LISTENER_STARTING:
        return "HTTPS_HEALTH_LISTENER_STARTING";
    case HTTPS_HEALTH_LISTENER_LISTENING:
        return "HTTPS_HEALTH_LISTENER_LISTENING";
    case HTTPS_HEALTH_LISTENER_DRAINING:
        return "HTTPS_HEALTH_LISTENER_DRAINING";
    case HTTPS_HEALTH_LISTENER_STOPPED:
        return "HTTPS_HEALTH_LISTENER_STOPPED";
    case HTTPS_HEALTH_LISTENER_FAILED:
        return "HTTPS_HEALTH_LISTENER_FAILED";
    default:
        return NULL;
    }
}

const char *https_health_failure_category_string(
    enum https_health_failure_category category)
{
    switch (category) {
    case HTTPS_HEALTH_FAILURE_TLS_HANDSHAKE:
        return "HTTPS_HEALTH_FAILURE_TLS_HANDSHAKE";
    case HTTPS_HEALTH_FAILURE_HTTP_PROTOCOL:
        return "HTTPS_HEALTH_FAILURE_HTTP_PROTOCOL";
    case HTTPS_HEALTH_FAILURE_PEER_ABUSE:
        return "HTTPS_HEALTH_FAILURE_PEER_ABUSE";
    case HTTPS_HEALTH_FAILURE_RESOURCE_EXHAUSTION:
        return "HTTPS_HEALTH_FAILURE_RESOURCE_EXHAUSTION";
    default:
        return NULL;
    }
}

#else

typedef int https_health_translation_unit_guard;

#endif
