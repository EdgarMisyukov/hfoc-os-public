#include "audit.h"
#include "event_ring.h"
#include "https_health.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#if defined(HTTPS_HEALTH_HOST_TEST)

static int failures;

#define CHECK(cond, msg) do { \
    if (!(cond)) { \
        fprintf(stderr, "FAIL: %s\n", msg); \
        ++failures; \
    } else { \
        printf("PASS: %s\n", msg); \
    } \
} while (0)

static struct https_health_metrics make_metrics(
    enum https_health_listener_state state,
    uint32_t connection_count,
    uint32_t accepted,
    uint32_t rejected,
    uint32_t failed)
{
    struct https_health_metrics metrics;

    memset(&metrics, 0, sizeof(metrics));
    metrics.version = HTTPS_HEALTH_VERSION;
    metrics.listener_state = state;
    metrics.connection_count = connection_count;
    metrics.accepted_connections = accepted;
    metrics.rejected_connections = rejected;
    metrics.failed_connections = failed;
    metrics.failure_counts[HTTPS_HEALTH_FAILURE_TLS_HANDSHAKE] = 1U;
    metrics.failure_counts[HTTPS_HEALTH_FAILURE_HTTP_PROTOCOL] = 2U;
    metrics.failure_counts[HTTPS_HEALTH_FAILURE_PEER_ABUSE] = 3U;
    metrics.failure_counts[HTTPS_HEALTH_FAILURE_RESOURCE_EXHAUSTION] = 4U;
    return metrics;
}

static bool init_store_ring(
    struct https_health_store *store,
    struct event_ring *ring)
{
    memset(store, 0, sizeof(*store));
    memset(ring, 0, sizeof(*ring));
    return https_health_init(store) == HTTPS_HEALTH_OK &&
           event_ring_init(ring) == EVENT_RING_OK;
}

static void check_status_strings(void)
{
    CHECK(strcmp(https_health_status_string(HTTPS_HEALTH_OK),
                 "HTTPS_HEALTH_OK") == 0,
          "status string HTTPS_HEALTH_OK");
    CHECK(strcmp(https_health_status_string(HTTPS_HEALTH_AUDIT_FAILED),
                 "HTTPS_HEALTH_AUDIT_FAILED") == 0,
          "status string HTTPS_HEALTH_AUDIT_FAILED");
    CHECK(https_health_status_string((enum https_health_status)999U) == NULL,
          "unknown status string is rejected");
    CHECK(strcmp(https_health_listener_state_string(
                     HTTPS_HEALTH_LISTENER_LISTENING),
                 "HTTPS_HEALTH_LISTENER_LISTENING") == 0,
          "listener state string LISTENING");
    CHECK(https_health_listener_state_string(
              (enum https_health_listener_state)99U) == NULL,
          "unknown listener state string is rejected");
    CHECK(strcmp(https_health_failure_category_string(
                     HTTPS_HEALTH_FAILURE_PEER_ABUSE),
                 "HTTPS_HEALTH_FAILURE_PEER_ABUSE") == 0,
          "failure category string PEER_ABUSE");
    CHECK(https_health_failure_category_string(
              (enum https_health_failure_category)99U) == NULL,
          "unknown failure category string is rejected");
}

static void check_preconditions(void)
{
    struct https_health_store store;
    struct event_ring ring;
    struct https_health_metrics metrics =
        make_metrics(HTTPS_HEALTH_LISTENER_CLOSED, 0U, 0U, 0U, 0U);
    struct https_health_snapshot snapshot;
    uint64_t sequence = 0U;

    memset(&store, 0, sizeof(store));
    memset(&ring, 0, sizeof(ring));
    CHECK(https_health_init(NULL) == HTTPS_HEALTH_NULL,
          "null store init is rejected");
    CHECK(https_health_publish(
              NULL, &metrics, &ring, &sequence) == HTTPS_HEALTH_NULL,
          "null store publish is rejected");
    CHECK(https_health_publish(
              &store, NULL, &ring, &sequence) == HTTPS_HEALTH_NULL,
          "null metrics publish is rejected");
    CHECK(https_health_snapshot(
              NULL, &snapshot) == HTTPS_HEALTH_NULL,
          "null store snapshot is rejected");
    CHECK(https_health_snapshot(
              &store, NULL) == HTTPS_HEALTH_NULL,
          "null snapshot output is rejected");
    CHECK(https_health_publish(
              &store, &metrics, &ring, &sequence) ==
              HTTPS_HEALTH_BAD_STATE,
          "uninitialized store publish is rejected");
    CHECK(https_health_snapshot(&store, &snapshot) ==
              HTTPS_HEALTH_BAD_STATE,
          "uninitialized store snapshot is rejected");

    CHECK(init_store_ring(&store, &ring),
          "store and audit ring initialize");
    metrics.version = 99U;
    CHECK(https_health_publish(
              &store, &metrics, &ring, &sequence) ==
              HTTPS_HEALTH_BAD_VERSION,
          "bad metrics version is rejected");
    metrics.version = HTTPS_HEALTH_VERSION;
    CHECK(https_health_snapshot(&store, &snapshot) ==
              HTTPS_HEALTH_NOT_PUBLISHED,
          "snapshot before publish is not published");
}

static void check_listener_states(void)
{
    struct https_health_store store;
    struct event_ring ring;
    struct https_health_snapshot snapshot;
    const enum https_health_listener_state states[] = {
        HTTPS_HEALTH_LISTENER_CLOSED,
        HTTPS_HEALTH_LISTENER_STARTING,
        HTTPS_HEALTH_LISTENER_LISTENING,
        HTTPS_HEALTH_LISTENER_DRAINING,
        HTTPS_HEALTH_LISTENER_STOPPED,
        HTTPS_HEALTH_LISTENER_FAILED,
    };
    uint32_t index;

    CHECK(init_store_ring(&store, &ring),
          "listener state store and ring initialize");
    for (index = 0U; index < sizeof(states) / sizeof(states[0]);
         ++index) {
        struct https_health_metrics metrics =
            make_metrics(states[index], 0U, 0U, 0U, 0U);

        CHECK(https_health_publish(
                  &store, &metrics, NULL, NULL) == HTTPS_HEALTH_OK,
              "every bounded listener state publishes");
        CHECK(https_health_snapshot(&store, &snapshot) ==
                  HTTPS_HEALTH_OK &&
                  snapshot.metrics.listener_state == states[index],
              "published listener state matches snapshot");
    }
}

static void check_high_water_and_monotonic_ids(void)
{
    struct https_health_store store;
    struct event_ring ring;
    struct https_health_snapshot snapshot;
    struct https_health_metrics metrics =
        make_metrics(HTTPS_HEALTH_LISTENER_LISTENING, 3U, 4U, 1U, 1U);
    uint32_t index;

    CHECK(init_store_ring(&store, &ring),
          "monotonic store and ring initialize");
    for (index = 1U; index <= 3U; ++index) {
        metrics.connection_count = index == 1U ? 3U : 2U;
        CHECK(https_health_publish(
                  &store, &metrics, NULL, NULL) == HTTPS_HEALTH_OK,
              "monotonic publish succeeds");
        CHECK(https_health_snapshot(&store, &snapshot) ==
                  HTTPS_HEALTH_OK &&
                  snapshot.generation == index &&
                  snapshot.correlation_id == index,
              "generation and correlation IDs increment");
    }
    CHECK(snapshot.high_water == 3U,
          "high-water retains maximum connection count");
    CHECK(snapshot.metrics.connection_count == 2U,
          "current connection count can fall below high-water");
}

static void check_capacity_and_counter_bounds(void)
{
    struct https_health_store store;
    struct event_ring ring;
    struct https_health_metrics metrics =
        make_metrics(HTTPS_HEALTH_LISTENER_LISTENING,
                     HTTPS_HEALTH_CONNECTION_CAPACITY + 1U,
                     0U, 0U, 0U);

    CHECK(init_store_ring(&store, &ring),
          "bound store and ring initialize");
    CHECK(https_health_publish(
              &store, &metrics, NULL, NULL) ==
              HTTPS_HEALTH_BAD_CONNECTION_COUNT,
          "fifth connection count is rejected");
    metrics.connection_count = HTTPS_HEALTH_CONNECTION_CAPACITY;
    metrics.listener_state = (enum https_health_listener_state)7U;
    CHECK(https_health_publish(
              &store, &metrics, NULL, NULL) ==
              HTTPS_HEALTH_BAD_LISTENER_STATE,
          "unknown listener state is rejected");
    metrics.listener_state = HTTPS_HEALTH_LISTENER_LISTENING;
    metrics.accepted_connections = UINT32_MAX;
    metrics.rejected_connections = 1U;
    CHECK(https_health_publish(
              &store, &metrics, NULL, NULL) ==
              HTTPS_HEALTH_BAD_COUNTER,
          "overflowing connection counters are rejected");
}

static void check_audit_publication(void)
{
    struct https_health_store store;
    struct event_ring ring;
    struct https_health_snapshot snapshot;
    struct audit_record records[KERNEL_EVENT_RING_CAPACITY];
    struct https_health_metrics metrics =
        make_metrics(HTTPS_HEALTH_LISTENER_LISTENING, 1U, 1U, 0U, 0U);
    uint64_t sequence = 0U;
    uint32_t count = 0U;

    CHECK(init_store_ring(&store, &ring),
          "audit store and ring initialize");
    CHECK(https_health_publish(
              &store, &metrics, &ring, &sequence) == HTTPS_HEALTH_OK &&
              sequence == 0U,
          "listening publication appends audit record");
    CHECK(audit_snapshot(
              &ring, records, KERNEL_EVENT_RING_CAPACITY, &count) ==
              AUDIT_OK &&
              count == 1U &&
              records[0].type == AUDIT_RECORD_NETWORK &&
              records[0].operation == HTTPS_HEALTH_AUDIT_OPERATION &&
              records[0].result == HTTPS_HEALTH_AUDIT_RESULT_OK &&
              records[0].generation == 1U &&
              records[0].detail == 1U &&
              records[0].session_id == 0U &&
              records[0].principal_id == 0U &&
              records[0].command_id == 0U,
          "audit record contains exact network metadata");

    metrics.listener_state = HTTPS_HEALTH_LISTENER_FAILED;
    metrics.connection_count = 0U;
    CHECK(https_health_publish(
              &store, &metrics, &ring, &sequence) == HTTPS_HEALTH_OK &&
              sequence == 1U,
          "failed publication appends audit record");
    count = 0U;
    CHECK(audit_snapshot(
              &ring, records, KERNEL_EVENT_RING_CAPACITY, &count) ==
              AUDIT_OK &&
              count == 2U &&
              records[1].result == HTTPS_HEALTH_AUDIT_RESULT_FAILED &&
              records[1].generation == 2U &&
              records[1].detail == 2U,
          "failed state maps to failed audit result");

    CHECK(https_health_snapshot(&store, &snapshot) ==
              HTTPS_HEALTH_OK &&
              snapshot.generation == 2U &&
              snapshot.correlation_id == 2U &&
              snapshot.high_water == 1U,
          "published snapshot reflects second audit generation");
}

static void check_audit_failures_do_not_mutate(void)
{
    struct https_health_store store;
    struct event_ring ring;
    struct https_health_snapshot before;
    struct https_health_snapshot after;
    struct kernel_event event;
    struct https_health_metrics metrics =
        make_metrics(HTTPS_HEALTH_LISTENER_LISTENING, 4U, 4U, 0U, 0U);
    uint32_t index;

    CHECK(init_store_ring(&store, &ring),
          "failure store and ring initialize");
    CHECK(https_health_publish(
              &store, &metrics, NULL, NULL) == HTTPS_HEALTH_OK,
          "initial no-ring publication succeeds");
    CHECK(https_health_snapshot(&store, &before) == HTTPS_HEALTH_OK,
          "initial snapshot succeeds");

    memset(&ring, 0, sizeof(ring));
    CHECK(https_health_publish(
              &store, &metrics, &ring, NULL) ==
              HTTPS_HEALTH_AUDIT_FAILED,
          "unpublished ring rejects publication");
    CHECK(https_health_snapshot(&store, &after) == HTTPS_HEALTH_OK &&
              after.generation == before.generation &&
              after.correlation_id == before.correlation_id,
          "unpublished ring failure leaves snapshot unchanged");

    CHECK(event_ring_init(&ring) == EVENT_RING_OK,
          "full-ring setup initializes");
    memset(&event, 0, sizeof(event));
    event.type = AUDIT_RECORD_NETWORK;
    event.flags = AUDIT_EVENT_FLAG_STRUCTURED;
    for (index = 0U; index < KERNEL_EVENT_RING_CAPACITY; ++index) {
        CHECK(event_ring_push(&ring, &event) == EVENT_RING_OK,
              "full-ring fill succeeds");
    }
    CHECK(event_ring_count(&ring) == KERNEL_EVENT_RING_CAPACITY,
          "audit ring is full");
    CHECK(https_health_publish(
              &store, &metrics, &ring, NULL) ==
              HTTPS_HEALTH_AUDIT_FAILED,
          "full ring rejects publication");
    CHECK(https_health_snapshot(&store, &after) == HTTPS_HEALTH_OK &&
              after.generation == before.generation &&
              after.correlation_id == before.correlation_id,
          "full ring failure leaves snapshot unchanged");
}

static void check_redaction_and_determinism(void)
{
    struct https_health_store store;
    struct event_ring ring;
    struct https_health_snapshot first;
    struct https_health_snapshot second;
    struct https_health_metrics metrics =
        make_metrics(HTTPS_HEALTH_LISTENER_LISTENING, 2U, 2U, 0U, 0U);

    CHECK(init_store_ring(&store, &ring),
          "redaction store and ring initialize");
    CHECK(https_health_publish(
              &store, &metrics, NULL, NULL) == HTTPS_HEALTH_OK,
          "deterministic publish succeeds");
    CHECK(https_health_snapshot(&store, &first) == HTTPS_HEALTH_OK,
          "first snapshot succeeds");
    CHECK(first.metrics.accepted_connections == 2U &&
              first.metrics.failure_counts[
                  HTTPS_HEALTH_FAILURE_TLS_HANDSHAKE] == 1U,
          "snapshot carries bounded scalar metrics");
    CHECK(https_health_publish(
              &store, &metrics, NULL, NULL) == HTTPS_HEALTH_OK,
          "repeat publish succeeds");
    CHECK(https_health_snapshot(&store, &second) == HTTPS_HEALTH_OK &&
              second.metrics.listener_state ==
                  first.metrics.listener_state &&
              second.metrics.connection_count ==
                  first.metrics.connection_count &&
              second.metrics.accepted_connections ==
                  first.metrics.accepted_connections &&
              second.generation == first.generation + 1U &&
              second.correlation_id == first.correlation_id + 1U,
          "repeat publish is deterministic except generation/correlation");
}

int main(void)
{
    check_status_strings();
    check_preconditions();
    check_listener_states();
    check_high_water_and_monotonic_ids();
    check_capacity_and_counter_bounds();
    check_audit_publication();
    check_audit_failures_do_not_mutate();
    check_redaction_and_determinism();

    if (failures != 0) {
        fprintf(stderr, "HTTPS health/audit/metrics host test: FAIL (%d)\n",
                failures);
        return 1;
    }
    printf("HTTPS health/audit/metrics host test: ALL OK\n");
    return 0;
}

#else

int main(void)
{
    return 0;
}

#endif
