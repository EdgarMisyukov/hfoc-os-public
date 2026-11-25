#include "service_health.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifdef HFOC_SERVICE_HEALTH_HOST_TEST

static int failures;
static unsigned int alloc_attempts;
static unsigned int free_attempts;

#define CHECK(cond, msg) do { \
    if (!(cond)) { \
        fprintf(stderr, "FAIL: %s\n", msg); \
        ++failures; \
    } else { \
        printf("PASS: %s\n", msg); \
    } \
} while (0)

void *kmalloc(size_t size)
{
    (void)size;
    ++alloc_attempts;
    return NULL;
}

bool kfree(void *ptr)
{
    (void)ptr;
    ++free_attempts;
    return false;
}

static struct service_descriptor make_descriptor(
    uint32_t service_id,
    bool enabled,
    uint32_t restart_limit,
    const uint8_t *dependencies,
    uint32_t dependency_count)
{
    struct service_descriptor descriptor;
    static const char *const names[] = {
        "alpha", "beta", "gamma", "delta", "epsilon", "zeta", "eta",
        "theta"
    };
    size_t name_length;

    memset(&descriptor, 0, sizeof(descriptor));
    descriptor.id = service_id;
    descriptor.version = SERVICE_DESCRIPTOR_VERSION;
    descriptor.enabled = enabled;
    descriptor.restart_limit = restart_limit;
    descriptor.readiness_timeout_ms = 1000U;
    descriptor.cpu_ticks_limit = 100U;
    descriptor.user_memory_limit_pages = 16U;
    if (service_id > 0U && service_id <= 8U) {
        name_length = strlen(names[service_id - 1U]);
        memcpy(descriptor.name, names[service_id - 1U],
               name_length + 1U);
    } else {
        memcpy(descriptor.name, "service", 8U);
    }
    descriptor.dependency_count = dependency_count;
    if (dependency_count > 0U) {
        memcpy(descriptor.dependencies, dependencies,
               dependency_count);
    }
    return descriptor;
}

static struct service_descriptor make_published_descriptor(
    uint32_t service_id,
    bool enabled,
    uint32_t restart_limit,
    const uint8_t *dependencies,
    uint32_t dependency_count)
{
    struct service_descriptor source = make_descriptor(
        service_id, enabled, restart_limit,
        dependencies, dependency_count);
    struct service_descriptor published;

    memset(&published, 0, sizeof(published));
    (void)service_descriptor_init(&published, &source);
    return published;
}

static struct service_supervisor_snapshot_entry make_entry(
    const struct service_descriptor *descriptor,
    enum service_supervisor_state state,
    enum service_health_state health_state,
    uint32_t restart_count)
{
    struct service_supervisor_snapshot_entry entry;

    memset(&entry, 0, sizeof(entry));
    entry.descriptor = *descriptor;
    entry.state = state;
    entry.health_state = health_state;
    entry.restart_count = restart_count;
    entry.exit_status = 0U;
    entry.ready_tick = 100U;
    entry.last_liveness_tick = 100U;
    entry.liveness_generation = 1U;
    return entry;
}

static void publish_snapshot(
    struct service_supervisor_snapshot *snapshot,
    const struct service_supervisor_snapshot_entry *entries,
    uint32_t entry_count)
{
    memset(snapshot, 0, sizeof(*snapshot));
    snapshot->published = true;
    snapshot->version = SERVICE_SUPERVISOR_SNAPSHOT_VERSION;
    snapshot->generation = 1U;
    snapshot->active_count = entry_count;
    snapshot->entry_count = entry_count;
    memcpy(snapshot->entries, entries,
           entry_count * sizeof(snapshot->entries[0]));
}

static void expect_bad_state(
    const struct service_supervisor_snapshot *snapshot,
    const char *message)
{
    struct service_health_input input;
    struct service_health_verdict verdict;
    unsigned char before[sizeof(verdict)];

    memset(&input, 0, sizeof(input));
    input.version = SERVICE_HEALTH_VERSION;
    input.snapshot = snapshot;
    memset(&verdict, 0xA5, sizeof(verdict));
    memcpy(before, &verdict, sizeof(before));
    CHECK(service_health_evaluate(&input, &verdict) ==
              SERVICE_HEALTH_BAD_STATE &&
              memcmp(&verdict, before, sizeof(before)) == 0,
          message);
}

static void test_status_and_fault_strings(void)
{
    static const struct {
        enum service_health_status status;
        const char *string;
    } statuses[] = {
        {SERVICE_HEALTH_OK, "SERVICE_HEALTH_OK"},
        {SERVICE_HEALTH_NULL, "SERVICE_HEALTH_NULL"},
        {SERVICE_HEALTH_BAD_VERSION, "SERVICE_HEALTH_BAD_VERSION"},
        {SERVICE_HEALTH_BAD_STATE, "SERVICE_HEALTH_BAD_STATE"},
    };
    static const struct {
        enum service_health_fault fault;
        const char *string;
    } faults[] = {
        {SERVICE_HEALTH_FAULT_NONE, "SERVICE_HEALTH_FAULT_NONE"},
        {SERVICE_HEALTH_FAULT_READINESS,
         "SERVICE_HEALTH_FAULT_READINESS"},
        {SERVICE_HEALTH_FAULT_RESTART,
         "SERVICE_HEALTH_FAULT_RESTART"},
        {SERVICE_HEALTH_FAULT_DEPENDENCY,
         "SERVICE_HEALTH_FAULT_DEPENDENCY"},
    };
    size_t index;

    for (index = 0U;
         index < sizeof(statuses) / sizeof(statuses[0]);
         ++index) {
        const char *string = service_health_status_string(
            statuses[index].status);

        CHECK(string != NULL &&
                  strcmp(string, statuses[index].string) == 0,
              "service health status string is exact");
    }
    CHECK(service_health_status_string(
              (enum service_health_status)4U) == NULL,
          "service health undefined status is rejected");

    for (index = 0U;
         index < sizeof(faults) / sizeof(faults[0]);
         ++index) {
        const char *string = service_health_fault_string(
            faults[index].fault);

        CHECK(string != NULL &&
                  strcmp(string, faults[index].string) == 0,
              "service health fault string is exact");
    }
    CHECK(service_health_fault_string(
              (enum service_health_fault)4U) == NULL,
          "service health undefined fault is rejected");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "service health strings perform no allocation");
}

static void test_null_and_version_failures(void)
{
    struct service_descriptor descriptor = make_published_descriptor(
        1U, true, 2U, NULL, 0U);
    struct service_supervisor_snapshot_entry entry = make_entry(
        &descriptor, SERVICE_SUPERVISOR_STATE_RUNNING,
        SERVICE_HEALTH_READY, 0U);
    struct service_supervisor_snapshot snapshot;
    struct service_health_input input;
    struct service_health_verdict verdict;

    publish_snapshot(&snapshot, &entry, 1U);
    memset(&input, 0, sizeof(input));
    input.version = SERVICE_HEALTH_VERSION;
    input.snapshot = &snapshot;
    memset(&verdict, 0xA5, sizeof(verdict));

    CHECK(service_health_evaluate(NULL, &verdict) ==
              SERVICE_HEALTH_NULL,
          "service health evaluate rejects null input");
    CHECK(service_health_evaluate(&input, NULL) ==
              SERVICE_HEALTH_NULL,
          "service health evaluate rejects null verdict");

    input.version = 0U;
    CHECK(service_health_evaluate(&input, &verdict) ==
              SERVICE_HEALTH_BAD_VERSION,
          "service health evaluate rejects zero version");
    input.version = SERVICE_HEALTH_VERSION + 1U;
    CHECK(service_health_evaluate(&input, &verdict) ==
              SERVICE_HEALTH_BAD_VERSION,
          "service health evaluate rejects future version");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "service health null and version paths perform no allocation");
}

static void test_invalid_snapshots_fail_closed(void)
{
    struct service_descriptor descriptor = make_published_descriptor(
        1U, true, 2U, NULL, 0U);
    struct service_supervisor_snapshot_entry entry = make_entry(
        &descriptor, SERVICE_SUPERVISOR_STATE_RUNNING,
        SERVICE_HEALTH_READY, 0U);
    struct service_supervisor_snapshot snapshot;
    struct service_supervisor_snapshot invalid;
    struct service_supervisor_snapshot_entry entries[2];
    struct service_descriptor duplicate;
    struct service_descriptor disabled;
    struct service_descriptor invalid_descriptor;
    uint8_t dependencies[1];

    publish_snapshot(&snapshot, &entry, 1U);
    invalid = snapshot;
    invalid.published = false;
    expect_bad_state(&invalid,
                     "service health rejects unpublished snapshot");
    invalid = snapshot;
    invalid.version = SERVICE_SUPERVISOR_SNAPSHOT_VERSION + 1U;
    expect_bad_state(&invalid,
                     "service health rejects future snapshot version");
    invalid = snapshot;
    invalid.active_count = 0U;
    expect_bad_state(&invalid,
                     "service health rejects active/entry count mismatch");
    invalid = snapshot;
    invalid.entry_count = SERVICE_SUPERVISOR_CAPACITY + 1U;
    expect_bad_state(&invalid,
                     "service health rejects over-limit entry count");

    disabled = make_published_descriptor(
        1U, false, 2U, NULL, 0U);
    entries[0] = make_entry(
        &disabled, SERVICE_SUPERVISOR_STATE_RUNNING,
        SERVICE_HEALTH_READY, 0U);
    publish_snapshot(&snapshot, entries, 1U);
    expect_bad_state(&snapshot,
                     "service health rejects disabled snapshot entry");

    invalid_descriptor = descriptor;
    invalid_descriptor.version = 0U;
    entries[0] = make_entry(
        &invalid_descriptor, SERVICE_SUPERVISOR_STATE_RUNNING,
        SERVICE_HEALTH_READY, 0U);
    publish_snapshot(&snapshot, entries, 1U);
    expect_bad_state(&snapshot,
                     "service health rejects invalid descriptor");

    duplicate = descriptor;
    entries[0] = make_entry(
        &descriptor, SERVICE_SUPERVISOR_STATE_RUNNING,
        SERVICE_HEALTH_READY, 0U);
    entries[1] = make_entry(
        &duplicate, SERVICE_SUPERVISOR_STATE_RUNNING,
        SERVICE_HEALTH_READY, 0U);
    publish_snapshot(&snapshot, entries, 2U);
    expect_bad_state(&snapshot,
                     "service health rejects duplicate service IDs");

    {
        struct service_descriptor second = make_published_descriptor(
            2U, true, 2U, NULL, 0U);

        entries[0] = make_entry(
            &second, SERVICE_SUPERVISOR_STATE_RUNNING,
            SERVICE_HEALTH_READY, 0U);
        entries[1] = make_entry(
            &descriptor, SERVICE_SUPERVISOR_STATE_RUNNING,
            SERVICE_HEALTH_READY, 0U);
    }
    publish_snapshot(&snapshot, entries, 2U);
    expect_bad_state(&snapshot,
                     "service health rejects unsorted service IDs");

    entries[0] = make_entry(
        &descriptor, SERVICE_SUPERVISOR_STATE_STOPPED,
        SERVICE_HEALTH_PENDING, 0U);
    publish_snapshot(&snapshot, entries, 1U);
    expect_bad_state(&snapshot,
                     "service health rejects stopped snapshot entry");

    entries[0] = make_entry(
        &descriptor, (enum service_supervisor_state)9U,
        SERVICE_HEALTH_READY, 0U);
    publish_snapshot(&snapshot, entries, 1U);
    expect_bad_state(&snapshot,
                     "service health rejects undefined supervisor state");

    entries[0] = make_entry(
        &descriptor, SERVICE_SUPERVISOR_STATE_RUNNING,
        (enum service_health_state)9U, 0U);
    publish_snapshot(&snapshot, entries, 1U);
    expect_bad_state(&snapshot,
                     "service health rejects undefined health state");

    dependencies[0] = 2U;
    {
        struct service_descriptor dependent = make_published_descriptor(
            1U, true, 2U, dependencies, 1U);

        (void)dependent;
    }
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "invalid snapshot paths perform no allocation");
}

static void test_healthy_and_empty_snapshots(void)
{
    struct service_descriptor first = make_published_descriptor(
        1U, true, 2U, NULL, 0U);
    struct service_descriptor second = make_published_descriptor(
        2U, true, 2U, NULL, 0U);
    struct service_supervisor_snapshot_entry entries[2];
    struct service_supervisor_snapshot snapshot;
    struct service_health_input input;
    struct service_health_verdict verdict;

    entries[0] = make_entry(
        &first, SERVICE_SUPERVISOR_STATE_RUNNING,
        SERVICE_HEALTH_READY, 0U);
    entries[1] = make_entry(
        &second, SERVICE_SUPERVISOR_STATE_RUNNING,
        SERVICE_HEALTH_READY, 0U);
    publish_snapshot(&snapshot, entries, 2U);
    memset(&input, 0, sizeof(input));
    input.version = SERVICE_HEALTH_VERSION;
    input.snapshot = &snapshot;
    memset(&verdict, 0xA5, sizeof(verdict));

    CHECK(service_health_evaluate(&input, &verdict) ==
              SERVICE_HEALTH_OK &&
              verdict.version == SERVICE_HEALTH_VERSION &&
              verdict.healthy &&
              verdict.result == HEALTH_CHECK_RESULT_PASS &&
              verdict.severity == HEALTH_CHECK_SEVERITY_INFO &&
              verdict.fault == SERVICE_HEALTH_FAULT_NONE &&
              verdict.service_count == 2U &&
              verdict.ready_count == 2U &&
              verdict.pending_count == 0U &&
              verdict.failed_count == 0U &&
              verdict.exhausted_count == 0U &&
              verdict.dependency_blocked_count == 0U &&
              verdict.worst_service_id == 1U,
          "all-ready snapshot publishes healthy PASS with exact counts");

    publish_snapshot(&snapshot, NULL, 0U);
    input.snapshot = &snapshot;
    memset(&verdict, 0xA5, sizeof(verdict));
    CHECK(service_health_evaluate(&input, &verdict) ==
              SERVICE_HEALTH_OK &&
              verdict.healthy &&
              verdict.result == HEALTH_CHECK_RESULT_PASS &&
              verdict.severity == HEALTH_CHECK_SEVERITY_INFO &&
              verdict.service_count == 0U &&
              verdict.worst_service_id == 0U,
          "empty snapshot publishes healthy PASS with zero service ID");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "healthy and empty aggregation performs no allocation");
}

static void test_pending_and_readiness_failures(void)
{
    struct service_descriptor descriptor = make_published_descriptor(
        1U, true, 2U, NULL, 0U);
    struct service_supervisor_snapshot_entry entry = make_entry(
        &descriptor, SERVICE_SUPERVISOR_STATE_RUNNING,
        SERVICE_HEALTH_PENDING, 0U);
    struct service_supervisor_snapshot snapshot;
    struct service_health_input input;
    struct service_health_verdict verdict;

    publish_snapshot(&snapshot, &entry, 1U);
    memset(&input, 0, sizeof(input));
    input.version = SERVICE_HEALTH_VERSION;
    input.snapshot = &snapshot;
    memset(&verdict, 0, sizeof(verdict));
    CHECK(service_health_evaluate(&input, &verdict) ==
              SERVICE_HEALTH_OK &&
              !verdict.healthy &&
              verdict.result == HEALTH_CHECK_RESULT_UNKNOWN &&
              verdict.severity == HEALTH_CHECK_SEVERITY_INFO &&
              verdict.fault == SERVICE_HEALTH_FAULT_NONE &&
              verdict.pending_count == 1U &&
              verdict.worst_service_id == 1U,
          "pending readiness publishes UNKNOWN and is not healthy");

    entry.health_state = SERVICE_HEALTH_LIVENESS_FAILED;
    publish_snapshot(&snapshot, &entry, 1U);
    memset(&verdict, 0, sizeof(verdict));
    CHECK(service_health_evaluate(&input, &verdict) ==
              SERVICE_HEALTH_OK &&
              !verdict.healthy &&
              verdict.result == HEALTH_CHECK_RESULT_FAIL &&
              verdict.severity == HEALTH_CHECK_SEVERITY_CRITICAL &&
              verdict.fault == SERVICE_HEALTH_FAULT_READINESS &&
              verdict.failed_count == 1U &&
              verdict.worst_service_id == 1U,
          "liveness failure publishes FAIL with readiness fault");

    entry.health_state = SERVICE_HEALTH_READINESS_TIMEOUT;
    publish_snapshot(&snapshot, &entry, 1U);
    memset(&verdict, 0, sizeof(verdict));
    CHECK(service_health_evaluate(&input, &verdict) ==
              SERVICE_HEALTH_OK &&
              verdict.result == HEALTH_CHECK_RESULT_FAIL &&
              verdict.fault == SERVICE_HEALTH_FAULT_READINESS &&
              verdict.failed_count == 1U,
          "readiness timeout publishes FAIL with readiness fault");

    entry = make_entry(
        &descriptor, SERVICE_SUPERVISOR_STATE_EXITED,
        SERVICE_HEALTH_PENDING, 0U);
    publish_snapshot(&snapshot, &entry, 1U);
    memset(&verdict, 0, sizeof(verdict));
    CHECK(service_health_evaluate(&input, &verdict) ==
              SERVICE_HEALTH_OK &&
              verdict.result == HEALTH_CHECK_RESULT_FAIL &&
              verdict.fault == SERVICE_HEALTH_FAULT_READINESS &&
              verdict.failed_count == 1U,
          "exited service publishes FAIL with readiness fault");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "readiness aggregation performs no allocation");
}

static void test_restart_exhaustion(void)
{
    struct service_descriptor descriptor = make_published_descriptor(
        1U, true, 1U, NULL, 0U);
    struct service_supervisor_snapshot_entry entry = make_entry(
        &descriptor, SERVICE_SUPERVISOR_STATE_RUNNING,
        SERVICE_HEALTH_READY, 1U);
    struct service_supervisor_snapshot snapshot;
    struct service_health_input input;
    struct service_health_verdict verdict;

    publish_snapshot(&snapshot, &entry, 1U);
    memset(&input, 0, sizeof(input));
    input.version = SERVICE_HEALTH_VERSION;
    input.snapshot = &snapshot;
    memset(&verdict, 0, sizeof(verdict));
    CHECK(service_health_evaluate(&input, &verdict) ==
              SERVICE_HEALTH_OK &&
              !verdict.healthy &&
              verdict.result == HEALTH_CHECK_RESULT_FAIL &&
              verdict.severity == HEALTH_CHECK_SEVERITY_CRITICAL &&
              verdict.fault == SERVICE_HEALTH_FAULT_RESTART &&
              verdict.exhausted_count == 1U &&
              verdict.failed_count == 1U &&
              verdict.ready_count == 0U &&
              verdict.worst_service_id == 1U,
          "running service at restart budget publishes FAIL with restart fault");

    entry = make_entry(
        &descriptor, SERVICE_SUPERVISOR_STATE_EXHAUSTED,
        SERVICE_HEALTH_PENDING, 0U);
    publish_snapshot(&snapshot, &entry, 1U);
    memset(&verdict, 0, sizeof(verdict));
    CHECK(service_health_evaluate(&input, &verdict) ==
              SERVICE_HEALTH_OK &&
              verdict.result == HEALTH_CHECK_RESULT_FAIL &&
              verdict.fault == SERVICE_HEALTH_FAULT_RESTART &&
              verdict.exhausted_count == 1U,
          "exhausted supervisor state publishes FAIL with restart fault");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "restart aggregation performs no allocation");
}

static void test_dependency_blocking_and_severity_ordering(void)
{
    uint8_t dependencies[1];
    struct service_descriptor first = make_published_descriptor(
        1U, true, 2U, NULL, 0U);
    struct service_descriptor third = make_published_descriptor(
        3U, true, 2U, NULL, 0U);
    struct service_supervisor_snapshot_entry entries[2];
    struct service_supervisor_snapshot snapshot;
    struct service_health_input input;
    struct service_health_verdict verdict;

    dependencies[0] = 3U;
    {
        struct service_descriptor second = make_published_descriptor(
            2U, true, 2U, dependencies, 1U);

        entries[0] = make_entry(
            &first, SERVICE_SUPERVISOR_STATE_RUNNING,
            SERVICE_HEALTH_READY, 0U);
        entries[1] = make_entry(
            &second, SERVICE_SUPERVISOR_STATE_RUNNING,
            SERVICE_HEALTH_READY, 0U);
    }
    publish_snapshot(&snapshot, entries, 2U);
    memset(&input, 0, sizeof(input));
    input.version = SERVICE_HEALTH_VERSION;
    input.snapshot = &snapshot;
    memset(&verdict, 0, sizeof(verdict));
    CHECK(service_health_evaluate(&input, &verdict) ==
              SERVICE_HEALTH_OK &&
              !verdict.healthy &&
              verdict.result == HEALTH_CHECK_RESULT_WARN &&
              verdict.severity == HEALTH_CHECK_SEVERITY_WARNING &&
              verdict.fault == SERVICE_HEALTH_FAULT_DEPENDENCY &&
              verdict.dependency_blocked_count == 1U &&
              verdict.ready_count == 1U &&
              verdict.worst_service_id == 2U,
          "absent dependency publishes WARN with dependency fault");

    entries[0] = make_entry(
        &first, SERVICE_SUPERVISOR_STATE_RUNNING,
        SERVICE_HEALTH_PENDING, 0U);
    entries[1] = make_entry(
        &third, SERVICE_SUPERVISOR_STATE_RUNNING,
        SERVICE_HEALTH_READY, 0U);
    publish_snapshot(&snapshot, entries, 2U);
    memset(&verdict, 0, sizeof(verdict));
    CHECK(service_health_evaluate(&input, &verdict) ==
              SERVICE_HEALTH_OK &&
              verdict.result == HEALTH_CHECK_RESULT_UNKNOWN &&
              verdict.severity == HEALTH_CHECK_SEVERITY_INFO,
          "UNKNOWN dominates PASS in severity ordering");

    entries[0] = make_entry(
        &first, SERVICE_SUPERVISOR_STATE_RUNNING,
        SERVICE_HEALTH_READY, 0U);
    entries[1] = make_entry(
        &third, SERVICE_SUPERVISOR_STATE_RUNNING,
        SERVICE_HEALTH_LIVENESS_FAILED, 0U);
    publish_snapshot(&snapshot, entries, 2U);
    memset(&verdict, 0, sizeof(verdict));
    CHECK(service_health_evaluate(&input, &verdict) ==
              SERVICE_HEALTH_OK &&
              verdict.result == HEALTH_CHECK_RESULT_FAIL &&
              verdict.severity == HEALTH_CHECK_SEVERITY_CRITICAL &&
              verdict.fault == SERVICE_HEALTH_FAULT_READINESS &&
              verdict.worst_service_id == 3U,
          "FAIL dominates WARN and PASS in severity ordering");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "dependency and ordering aggregation performs no allocation");
}

int main(void)
{
    test_status_and_fault_strings();
    test_null_and_version_failures();
    test_invalid_snapshots_fail_closed();
    test_healthy_and_empty_snapshots();
    test_pending_and_readiness_failures();
    test_restart_exhaustion();
    test_dependency_blocking_and_severity_ordering();

    if (failures != 0) {
        fprintf(stderr, "Service health host tests: FAILED\n");
        return 1;
    }
    printf("Service health host tests: ALL OK\n");
    return 0;
}

#endif
