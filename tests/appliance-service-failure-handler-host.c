#include "appliance_service_failure_handler.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifdef HFOC_APPLIANCE_SERVICE_FAILURE_HANDLER_HOST_TEST

static int failures;

#define CHECK(cond, msg) do { \
    if (!(cond)) { \
        fprintf(stderr, "FAIL: %s\n", msg); \
        ++failures; \
    } else { \
        printf("PASS: %s\n", msg); \
    } \
} while (0)

struct handler_fake {
    uint32_t start_calls;
    uint32_t stop_calls;
    uint32_t restart_calls;
    uint32_t snapshot_calls;
    uint32_t last_service_id;
    uint64_t last_now_ticks;
    bool last_restarted;
    enum service_supervisor_status start_status;
    enum service_supervisor_status stop_status;
    enum service_supervisor_status restart_status;
    enum service_supervisor_status snapshot_status;
    struct service_supervisor_snapshot supervisor_snapshot;
};

static struct service_descriptor make_descriptor(
    uint32_t id,
    const char *name)
{
    struct service_descriptor descriptor;

    memset(&descriptor, 0, sizeof(descriptor));
    descriptor.id = id;
    descriptor.version = SERVICE_DESCRIPTOR_VERSION;
    descriptor.enabled = true;
    descriptor.restart_limit = 2U;
    descriptor.readiness_timeout_ms = 1000U;
    memcpy(descriptor.name, name, strlen(name) + 1U);
    return descriptor;
}

static struct service_descriptor make_descriptor_with_dependency(
    uint32_t id,
    uint32_t dependency_id,
    const char *name)
{
    struct service_descriptor descriptor = make_descriptor(id, name);

    descriptor.dependency_count = 1U;
    descriptor.dependencies[0] = (uint8_t)dependency_id;
    return descriptor;
}

static void handler_test_entry(void) {
}

static struct service_supervisor_entry make_entry(
    uint32_t service_id)
{
    struct service_supervisor_entry entry;

    memset(&entry, 0, sizeof(entry));
    entry.service_id = service_id;
    entry.entry = handler_test_entry;
    return entry;
}

static enum service_supervisor_status fake_start(
    void *context,
    const struct service_graph_snapshot *snapshot,
    const struct service_supervisor_entry *entries,
    uint32_t entry_count)
{
    struct handler_fake *fake = context;

    (void)snapshot;
    (void)entries;
    (void)entry_count;
    ++fake->start_calls;
    return fake->start_status;
}

static enum service_supervisor_status fake_stop(
    void *context,
    const struct service_graph_snapshot *snapshot)
{
    struct handler_fake *fake = context;

    (void)snapshot;
    ++fake->stop_calls;
    return fake->stop_status;
}

static enum service_supervisor_status fake_restart(
    void *context,
    uint32_t service_id,
    uint64_t now_ticks,
    const struct service_graph_snapshot *snapshot,
    uint32_t *exit_status_out,
    bool *restarted)
{
    struct handler_fake *fake = context;

    (void)snapshot;
    ++fake->restart_calls;
    fake->last_service_id = service_id;
    fake->last_now_ticks = now_ticks;
    if (exit_status_out != NULL) {
        *exit_status_out = 7U;
    }
    if (restarted != NULL) {
        *restarted = fake->last_restarted;
    }
    return fake->restart_status;
}

static enum service_supervisor_status fake_snapshot(
    void *context,
    struct service_supervisor_snapshot *snapshot)
{
    struct handler_fake *fake = context;

    ++fake->snapshot_calls;
    if (fake->snapshot_status != SERVICE_SUPERVISOR_OK) {
        return fake->snapshot_status;
    }
    *snapshot = fake->supervisor_snapshot;
    return SERVICE_SUPERVISOR_OK;
}

static struct appliance_service_manager_ops make_ops(
    struct handler_fake *fake)
{
    struct appliance_service_manager_ops ops;

    memset(&ops, 0, sizeof(ops));
    ops.start_ordered = fake_start;
    ops.stop_ordered = fake_stop;
    ops.restart_contained = fake_restart;
    ops.snapshot = fake_snapshot;
    ops.context = fake;
    return ops;
}

static void reset_fake(struct handler_fake *fake)
{
    memset(fake, 0, sizeof(*fake));
    fake->start_status = SERVICE_SUPERVISOR_OK;
    fake->stop_status = SERVICE_SUPERVISOR_OK;
    fake->restart_status = SERVICE_SUPERVISOR_OK;
    fake->snapshot_status = SERVICE_SUPERVISOR_OK;
    fake->last_restarted = true;
}

static void fill_healthy_snapshot(
    struct service_supervisor_snapshot *snapshot,
    uint32_t service_count)
{
    uint32_t index;

    memset(snapshot, 0, sizeof(*snapshot));
    snapshot->version = SERVICE_SUPERVISOR_SNAPSHOT_VERSION;
    snapshot->published = true;
    snapshot->generation = 7U;
    snapshot->active_count = service_count;
    snapshot->entry_count = service_count;
    for (index = 0U; index < service_count; ++index) {
        struct service_supervisor_snapshot_entry *const entry =
            &snapshot->entries[index];
        struct service_descriptor descriptor =
            make_descriptor(index + 1U, "ready");

        (void)service_descriptor_init(&entry->descriptor, &descriptor);
        entry->state = SERVICE_SUPERVISOR_STATE_RUNNING;
        entry->health_state = SERVICE_HEALTH_READY;
        entry->restart_count = 0U;
    }
}

static void fill_exited_snapshot(
    struct service_supervisor_snapshot *snapshot)
{
    struct service_descriptor base = make_descriptor(1U, "base");
    struct service_descriptor middle =
        make_descriptor_with_dependency(2U, 1U, "middle");
    struct service_descriptor failed =
        make_descriptor_with_dependency(3U, 2U, "failed");
    struct service_supervisor_snapshot_entry *entry;

    fill_healthy_snapshot(snapshot, 3U);
    (void)service_descriptor_init(&snapshot->entries[0].descriptor, &base);
    (void)service_descriptor_init(
        &snapshot->entries[1].descriptor, &middle);
    (void)service_descriptor_init(
        &snapshot->entries[2].descriptor, &failed);
    entry = &snapshot->entries[2];
    entry->state = SERVICE_SUPERVISOR_STATE_EXITED;
    entry->health_state = SERVICE_HEALTH_PENDING;
    entry->exit_status = 7U;
    entry->restart_count = 1U;
}

static bool start_running_manager(
    struct handler_fake *fake,
    struct appliance_service_manager *manager)
{
    struct appliance_service_manager_ops ops;
    struct service_descriptor descriptors[3];
    struct service_supervisor_entry entries[3];

    memset(manager, 0, sizeof(*manager));
    descriptors[0] = make_descriptor(1U, "base");
    descriptors[1] = make_descriptor_with_dependency(2U, 1U, "middle");
    descriptors[2] = make_descriptor_with_dependency(3U, 2U, "failed");
    entries[0] = make_entry(1U);
    entries[1] = make_entry(2U);
    entries[2] = make_entry(3U);

    ops = make_ops(fake);
    if (appliance_service_manager_init(
            manager, &ops,
            APPLIANCE_SERVICE_MANAGER_STARTUP_ROLLBACK) !=
        APPLIANCE_SERVICE_MANAGER_OK) {
        return false;
    }
    if (appliance_service_manager_configure(
            manager, descriptors, 3U, entries, 3U) !=
        APPLIANCE_SERVICE_MANAGER_OK) {
        return false;
    }
    return appliance_service_manager_start(manager) ==
        APPLIANCE_SERVICE_MANAGER_OK;
}

static void test_strings(void)
{
    enum appliance_service_failure_handler_status status;
    enum appliance_service_failure_kind kind;
    enum appliance_service_failure_action action;
    enum appliance_service_failure_result result;

    for (status = APPLIANCE_SERVICE_FAILURE_HANDLER_OK;
         status <= APPLIANCE_SERVICE_FAILURE_HANDLER_OVERFLOW;
         ++status) {
        CHECK(appliance_service_failure_handler_status_string(status) != NULL,
              "handler status string is present for every status");
    }
    CHECK(appliance_service_failure_handler_status_string(
              (enum appliance_service_failure_handler_status)8U) == NULL,
          "handler status string rejects undefined status");

    for (kind = APPLIANCE_SERVICE_FAILURE_KIND_NONE;
         kind <= APPLIANCE_SERVICE_FAILURE_KIND_LIVENESS_FAILED;
         ++kind) {
        CHECK(appliance_service_failure_kind_string(kind) != NULL,
              "handler kind string is present for every kind");
    }
    CHECK(appliance_service_failure_kind_string(
              (enum appliance_service_failure_kind)4U) == NULL,
          "handler kind string rejects undefined kind");

    for (action = APPLIANCE_SERVICE_FAILURE_ACTION_NONE;
         action <= APPLIANCE_SERVICE_FAILURE_ACTION_ISOLATE;
         ++action) {
        CHECK(appliance_service_failure_action_string(action) != NULL,
              "handler action string is present for every action");
    }
    CHECK(appliance_service_failure_action_string(
              (enum appliance_service_failure_action)4U) == NULL,
          "handler action string rejects undefined action");

    for (result = APPLIANCE_SERVICE_FAILURE_RESULT_NONE;
         result <= APPLIANCE_SERVICE_FAILURE_RESULT_NOT_RESTARTED;
         ++result) {
        CHECK(appliance_service_failure_result_string(result) != NULL,
              "handler result string is present for every result");
    }
    CHECK(appliance_service_failure_result_string(
              (enum appliance_service_failure_result)5U) == NULL,
          "handler result string rejects undefined result");
}

static void test_init_null_and_bad_state(void)
{
    struct handler_fake fake;
    struct appliance_service_manager manager;
    struct appliance_service_failure_handler handler;

    reset_fake(&fake);
    memset(&manager, 0, sizeof(manager));
    memset(&handler, 0, sizeof(handler));

    CHECK(appliance_service_failure_handler_init(NULL, &manager) ==
              APPLIANCE_SERVICE_FAILURE_HANDLER_NULL,
          "handler init rejects null handler");
    CHECK(appliance_service_failure_handler_init(&handler, NULL) ==
              APPLIANCE_SERVICE_FAILURE_HANDLER_NULL,
          "handler init rejects null manager");
    CHECK(appliance_service_failure_handler_init(&handler, &manager) ==
              APPLIANCE_SERVICE_FAILURE_HANDLER_BAD_STATE,
          "handler init rejects uninitialized manager");

    if (!start_running_manager(&fake, &manager)) {
        CHECK(false, "handler init fixture starts manager");
        return;
    }
    CHECK(appliance_service_failure_handler_init(
              &handler, &manager) ==
              APPLIANCE_SERVICE_FAILURE_HANDLER_OK &&
              handler.initialized &&
              handler.version ==
                  APPLIANCE_SERVICE_FAILURE_HANDLER_VERSION &&
              handler.manager == &manager,
          "handler init publishes running manager binding");
    CHECK(appliance_service_failure_handler_init(
              &handler, &manager) ==
              APPLIANCE_SERVICE_FAILURE_HANDLER_BAD_STATE,
          "handler double init returns BAD_STATE");
}

static void test_poll_no_failure(void)
{
    struct handler_fake fake;
    struct appliance_service_manager manager;
    struct appliance_service_failure_handler handler;
    struct appliance_service_failure_event event;

    reset_fake(&fake);
    fill_healthy_snapshot(&fake.supervisor_snapshot, 3U);
    memset(&handler, 0, sizeof(handler));
    if (!start_running_manager(&fake, &manager)) {
        CHECK(false, "no-failure fixture starts manager");
        return;
    }
    CHECK(appliance_service_failure_handler_init(
              &handler, &manager) ==
              APPLIANCE_SERVICE_FAILURE_HANDLER_OK,
          "no-failure fixture initializes handler");

    CHECK(appliance_service_failure_handler_poll(
              &handler, 100U, &event) ==
              APPLIANCE_SERVICE_FAILURE_HANDLER_NO_FAILURE,
          "handler poll returns NO_FAILURE on healthy snapshot");
    CHECK(fake.restart_calls == 0U,
          "no-failure poll never invokes restart");
    CHECK(handler.observed_count == 0U,
          "no-failure poll does not publish counters");
}

static void test_poll_recovery_event(void)
{
    struct handler_fake fake;
    struct appliance_service_manager manager;
    struct appliance_service_failure_handler handler;
    struct appliance_service_failure_event event;

    reset_fake(&fake);
    fill_exited_snapshot(&fake.supervisor_snapshot);
    memset(&handler, 0, sizeof(handler));
    if (!start_running_manager(&fake, &manager)) {
        CHECK(false, "recovery fixture starts manager");
        return;
    }
    CHECK(appliance_service_failure_handler_init(
              &handler, &manager) ==
              APPLIANCE_SERVICE_FAILURE_HANDLER_OK,
          "recovery fixture initializes handler");

    CHECK(appliance_service_failure_handler_poll(
              &handler, 200U, &event) ==
              APPLIANCE_SERVICE_FAILURE_HANDLER_OK &&
              event.published &&
              event.version ==
                  APPLIANCE_SERVICE_FAILURE_HANDLER_VERSION &&
              event.service_id == 3U &&
              event.kind == APPLIANCE_SERVICE_FAILURE_KIND_EXIT &&
              event.action == APPLIANCE_SERVICE_FAILURE_ACTION_RESTART &&
              event.result ==
                  APPLIANCE_SERVICE_FAILURE_RESULT_RESTARTED &&
              event.exit_status == 7U &&
              event.restart_count == 1U &&
              event.now_ticks == 200U,
          "handler poll publishes exact recovered event");
    CHECK(fake.restart_calls == 1U &&
              fake.last_service_id == 3U &&
              fake.last_now_ticks == 200U,
          "handler poll restarts only the exited service");
    CHECK(handler.observed_count == 1U &&
              handler.recovered_count == 1U &&
              handler.deferred_count == 0U &&
              handler.exhausted_count == 0U,
          "handler recovery increments exact counters");
    CHECK(handler.last_event.service_id == 3U &&
              handler.last_event.result ==
                  APPLIANCE_SERVICE_FAILURE_RESULT_RESTARTED,
          "handler stores exact latest event");
}

static void test_poll_exhausted_result(void)
{
    struct handler_fake fake;
    struct appliance_service_manager manager;
    struct appliance_service_failure_handler handler;
    struct appliance_service_failure_event event;

    reset_fake(&fake);
    fill_exited_snapshot(&fake.supervisor_snapshot);
    fake.restart_status = SERVICE_SUPERVISOR_OK;
    fake.last_restarted = false;
    memset(&handler, 0, sizeof(handler));
    if (!start_running_manager(&fake, &manager)) {
        CHECK(false, "exhausted fixture starts manager");
        return;
    }
    CHECK(appliance_service_failure_handler_init(
              &handler, &manager) ==
              APPLIANCE_SERVICE_FAILURE_HANDLER_OK,
          "exhausted fixture initializes handler");

    CHECK(appliance_service_failure_handler_poll(
              &handler, 300U, &event) ==
              APPLIANCE_SERVICE_FAILURE_HANDLER_OK &&
              event.service_id == 3U &&
              event.kind == APPLIANCE_SERVICE_FAILURE_KIND_EXIT &&
              event.action == APPLIANCE_SERVICE_FAILURE_ACTION_ISOLATE &&
              event.result ==
                  APPLIANCE_SERVICE_FAILURE_RESULT_EXHAUSTED,
          "handler maps disabled restart to exhausted isolation");
    CHECK(handler.observed_count == 1U &&
              handler.exhausted_count == 1U,
          "handler exhaustion increments exact counters");
}

static void test_poll_deferred_result(void)
{
    struct handler_fake fake;
    struct appliance_service_manager manager;
    struct appliance_service_failure_handler handler;
    struct appliance_service_failure_event event;

    reset_fake(&fake);
    fill_exited_snapshot(&fake.supervisor_snapshot);
    fake.restart_status = SERVICE_SUPERVISOR_RESTART_INTERVAL;
    fake.last_restarted = false;
    memset(&handler, 0, sizeof(handler));
    if (!start_running_manager(&fake, &manager)) {
        CHECK(false, "deferred fixture starts manager");
        return;
    }
    CHECK(appliance_service_failure_handler_init(
              &handler, &manager) ==
              APPLIANCE_SERVICE_FAILURE_HANDLER_OK,
          "deferred fixture initializes handler");

    CHECK(appliance_service_failure_handler_poll(
              &handler, 400U, &event) ==
              APPLIANCE_SERVICE_FAILURE_HANDLER_OK &&
              event.service_id == 3U &&
              event.kind == APPLIANCE_SERVICE_FAILURE_KIND_EXIT &&
              event.action == APPLIANCE_SERVICE_FAILURE_ACTION_DEFER &&
              event.result ==
                  APPLIANCE_SERVICE_FAILURE_RESULT_DEFERRED,
          "handler maps blocked restart to deferred event");
    CHECK(handler.observed_count == 1U &&
              handler.deferred_count == 1U &&
              handler.exhausted_count == 0U,
          "handler deferral increments exact counters");
}

static void test_snapshot_and_render(void)
{
    struct handler_fake fake;
    struct appliance_service_manager manager;
    struct appliance_service_failure_handler handler;
    struct appliance_service_failure_event event;
    struct appliance_service_failure_handler_snapshot snapshot;
    char output[APPLIANCE_SERVICE_FAILURE_HANDLER_OUTPUT_MAX + 1U];
    size_t output_length = 0U;

    reset_fake(&fake);
    fill_exited_snapshot(&fake.supervisor_snapshot);
    memset(&handler, 0, sizeof(handler));
    if (!start_running_manager(&fake, &manager)) {
        CHECK(false, "snapshot fixture starts manager");
        return;
    }
    CHECK(appliance_service_failure_handler_init(
              &handler, &manager) ==
              APPLIANCE_SERVICE_FAILURE_HANDLER_OK &&
              appliance_service_failure_handler_poll(
                  &handler, 200U, &event) ==
                  APPLIANCE_SERVICE_FAILURE_HANDLER_OK,
          "snapshot fixture records recovered event");

    fill_healthy_snapshot(&fake.supervisor_snapshot, 3U);
    memset(&snapshot, 0, sizeof(snapshot));
    CHECK(appliance_service_failure_handler_snapshot(
              &handler, &snapshot) ==
              APPLIANCE_SERVICE_FAILURE_HANDLER_OK &&
              snapshot.published &&
              snapshot.version ==
                  APPLIANCE_SERVICE_FAILURE_HANDLER_VERSION &&
              snapshot.observed_count == 1U &&
              snapshot.recovered_count == 1U &&
              snapshot.deferred_count == 0U &&
              snapshot.exhausted_count == 0U &&
              snapshot.last_event.service_id == 3U &&
              snapshot.manager_state ==
                  APPLIANCE_SERVICE_MANAGER_STATE_RUNNING &&
              snapshot.manager_health == HEALTH_CHECK_RESULT_PASS,
          "handler snapshot publishes exact aggregate state");

    memset(output, 0, sizeof(output));
    output_length = 0U;
    CHECK(appliance_service_failure_handler_render(
              &snapshot, output, sizeof(output), &output_length) ==
              APPLIANCE_SERVICE_FAILURE_HANDLER_OK &&
              strcmp(output,
                     "appliance_failures v=1 observed=1 recovered=1 "
                     "deferred=0 exhausted=0 "
                     "last=EXIT:3:RESTART:RECOVERED "
                     "manager=RUNNING health=PASS\n") == 0,
          "handler snapshot renders canonical line");

    CHECK(appliance_service_failure_handler_render(
              NULL, output, sizeof(output), &output_length) ==
              APPLIANCE_SERVICE_FAILURE_HANDLER_NULL,
          "handler render rejects null snapshot");
    CHECK(appliance_service_failure_handler_render(
              &snapshot, NULL, sizeof(output), &output_length) ==
              APPLIANCE_SERVICE_FAILURE_HANDLER_NULL,
          "handler render rejects null output");
    output_length = sizeof(output);
    CHECK(appliance_service_failure_handler_render(
              &snapshot, output, 4U, &output_length) ==
              APPLIANCE_SERVICE_FAILURE_HANDLER_OVERFLOW,
          "handler render rejects overflow");
}

int main(void)
{
    test_strings();
    test_init_null_and_bad_state();
    test_poll_no_failure();
    test_poll_recovery_event();
    test_poll_exhausted_result();
    test_poll_deferred_result();
    test_snapshot_and_render();

    if (failures != 0) {
        fprintf(stderr,
                "Appliance service failure handler host tests: %d FAILURES\n",
                failures);
        return 1;
    }
    puts("Appliance service failure handler host tests: ALL OK");
    return 0;
}

#else
#error "appliance-service-failure-handler-host.c requires HFOC_APPLIANCE_SERVICE_FAILURE_HANDLER_HOST_TEST"
#endif
