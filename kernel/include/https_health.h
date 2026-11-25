#ifndef HFOC_HTTPS_HEALTH_H
#define HFOC_HTTPS_HEALTH_H

#include "event_ring.h"

#include <stdbool.h>
#include <stdint.h>

#define HTTPS_HEALTH_VERSION 1U
#define HTTPS_HEALTH_CONNECTION_CAPACITY 4U
#define HTTPS_HEALTH_FAILURE_CATEGORY_COUNT 4U
#define HTTPS_HEALTH_GENERATION_MAX UINT32_C(0xFFFFFF)
#define HTTPS_HEALTH_AUDIT_OPERATION 1U
#define HTTPS_HEALTH_AUDIT_RESULT_OK 0U
#define HTTPS_HEALTH_AUDIT_RESULT_FAILED 1U

enum https_health_status {
    HTTPS_HEALTH_OK = 0,
    HTTPS_HEALTH_NULL = 1,
    HTTPS_HEALTH_BAD_STATE = 2,
    HTTPS_HEALTH_BAD_VERSION = 3,
    HTTPS_HEALTH_NOT_PUBLISHED = 4,
    HTTPS_HEALTH_BAD_LISTENER_STATE = 5,
    HTTPS_HEALTH_BAD_CONNECTION_COUNT = 6,
    HTTPS_HEALTH_BAD_COUNTER = 7,
    HTTPS_HEALTH_CORRELATION_OVERFLOW = 8,
    HTTPS_HEALTH_GENERATION_OVERFLOW = 9,
    HTTPS_HEALTH_AUDIT_FAILED = 10,
};

enum https_health_listener_state {
    HTTPS_HEALTH_LISTENER_CLOSED = 1,
    HTTPS_HEALTH_LISTENER_STARTING = 2,
    HTTPS_HEALTH_LISTENER_LISTENING = 3,
    HTTPS_HEALTH_LISTENER_DRAINING = 4,
    HTTPS_HEALTH_LISTENER_STOPPED = 5,
    HTTPS_HEALTH_LISTENER_FAILED = 6,
};

enum https_health_failure_category {
    HTTPS_HEALTH_FAILURE_TLS_HANDSHAKE = 0,
    HTTPS_HEALTH_FAILURE_HTTP_PROTOCOL = 1,
    HTTPS_HEALTH_FAILURE_PEER_ABUSE = 2,
    HTTPS_HEALTH_FAILURE_RESOURCE_EXHAUSTION = 3,
};

struct https_health_metrics {
    uint32_t version;
    enum https_health_listener_state listener_state;
    uint32_t connection_count;
    uint32_t accepted_connections;
    uint32_t rejected_connections;
    uint32_t failed_connections;
    uint32_t failure_counts[HTTPS_HEALTH_FAILURE_CATEGORY_COUNT];
};

struct https_health_snapshot {
    uint32_t version;
    uint32_t generation;
    uint32_t high_water;
    uint32_t correlation_id;
    struct https_health_metrics metrics;
};

struct https_health_store {
    uint32_t version;
    bool initialized;
    bool published;
    uint32_t generation;
    uint32_t high_water;
    uint32_t correlation_id;
    struct https_health_metrics published_metrics;
};

enum https_health_status https_health_init(
    struct https_health_store *store);
enum https_health_status https_health_publish(
    struct https_health_store *store,
    const struct https_health_metrics *metrics,
    struct event_ring *audit_ring,
    uint64_t *sequence_out);
enum https_health_status https_health_snapshot(
    const struct https_health_store *store,
    struct https_health_snapshot *snapshot_out);
const char *https_health_status_string(
    enum https_health_status status);
const char *https_health_listener_state_string(
    enum https_health_listener_state state);
const char *https_health_failure_category_string(
    enum https_health_failure_category category);

#endif
