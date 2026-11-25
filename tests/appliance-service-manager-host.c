#include "appliance_service_manager.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifdef HFOC_APPLIANCE_SERVICE_MANAGER_HOST_TEST

static int failures;

#define CHECK(cond, msg) do { \
    if (!(cond)) { \
        fprintf(stderr, "FAIL: %s\n", msg); \
        ++failures; \
    } else { \
        printf("PASS: %s\n", msg); \
    } \
} while (0)

struct manager_fake {
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
    bool enabled,
    const char *name)
{
    struct service_descriptor descriptor;

    memset(&descriptor, 0, sizeof(descriptor));
    descriptor.id = id;
    descriptor.version = SERVICE_DESCRIPTOR_VERSION;
    descriptor.enabled = enabled;
    descriptor.restart_limit = 1U;
    descriptor.readiness_timeout_ms = 1000U;
    if (name != NULL) {
        memcpy(descriptor.name, name, strlen(name) + 1U);
    }
    return descriptor;
}

static struct service_descriptor make_descriptor_with_dependency(
    uint32_t id,
    uint32_t dependency_id,
    const char *name)
{
    struct service_descriptor descriptor = make_descriptor(id, true, name);

    descriptor.dependency_count = 1U;
    descriptor.dependencies[0] = (uint8_t)dependency_id;
    return descriptor;
}

static void manager_test_entry(void) {
}

static struct service_supervisor_entry make_entry(
    uint32_t service_id)
{
    struct service_supervisor_entry entry;

    memset(&entry, 0, sizeof(entry));
    entry.service_id = service_id;
    entry.entry = manager_test_entry;
    return entry;
}

static enum service_supervisor_status fake_start(
    void *context,
    const struct service_graph_snapshot *snapshot,
    const struct service_supervisor_entry *entries,
    uint32_t entry_count)
{
    struct manager_fake *fake = context;

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
    struct manager_fake *fake = context;

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
    struct manager_fake *fake = context;

    (void)snapshot;
    (void)exit_status_out;
    ++fake->restart_calls;
    fake->last_service_id = service_id;
    fake->last_now_ticks = now_ticks;
    if (restarted != NULL) {
        *restarted = fake->last_restarted;
    }
    return fake->restart_status;
}

static enum service_supervisor_status fake_snapshot(
    void *context,
    struct service_supervisor_snapshot *snapshot)
{
    struct manager_fake *fake = context;

    ++fake->snapshot_calls;
    if (fake->snapshot_status != SERVICE_SUPERVISOR_OK) {
        return fake->snapshot_status;
    }
    *snapshot = fake->supervisor_snapshot;
    return SERVICE_SUPERVISOR_OK;
}

static struct appliance_service_manager_ops make_ops(
    struct manager_fake *fake)
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

static void reset_fake(struct manager_fake *fake)
{
    memset(fake, 0, sizeof(*fake));
    fake->start_status = SERVICE_SUPERVISOR_OK;
    fake->stop_status = SERVICE_SUPERVISOR_OK;
    fake->restart_status = SERVICE_SUPERVISOR_OK;
    fake->snapshot_status = SERVICE_SUPERVISOR_OK;
    fake->last_restarted = true;
}

static void fill_healthy_snapshot(
    struct service_supervisor_snapshot *snapshot)
{
    uint32_t index;

    memset(snapshot, 0, sizeof(*snapshot));
    snapshot->version = SERVICE_SUPERVISOR_SNAPSHOT_VERSION;
    snapshot->published = true;
    snapshot->generation = 7U;
    snapshot->active_count = 2U;
    snapshot->entry_count = 2U;
    for (index = 0U; index < 2U; ++index) {
        struct service_descriptor source =
            make_descriptor(index + 1U, true, "ready");
        struct service_supervisor_snapshot_entry *const entry =
            &snapshot->entries[index];

        (void)service_descriptor_init(&entry->descriptor, &source);
        entry->state = SERVICE_SUPERVISOR_STATE_RUNNING;
        entry->health_state = SERVICE_HEALTH_READY;
    }
}

static void fill_failed_snapshot(
    struct service_supervisor_snapshot *snapshot)
{
    struct service_descriptor ready =
        make_descriptor(1U, true, "ready");
    struct service_descriptor failed =
        make_descriptor(2U, true, "failed");

    fill_healthy_snapshot(snapshot);
    (void)service_descriptor_init(&snapshot->entries[0].descriptor, &ready);
    (void)service_descriptor_init(&snapshot->entries[1].descriptor, &failed);
    snapshot->entries[0].health_state = SERVICE_HEALTH_READY;
    snapshot->entries[1].state = SERVICE_SUPERVISOR_STATE_EXITED;
    snapshot->entries[1].health_state = SERVICE_HEALTH_PENDING;
}

static void test_strings(void)
{
    enum appliance_service_manager_status status;
    enum appliance_service_manager_state state;
    enum appliance_service_manager_startup_policy policy;

    for (status = APPLIANCE_SERVICE_MANAGER_OK;
         status <= APPLIANCE_SERVICE_MANAGER_OVERFLOW;
         ++status) {
        CHECK(appliance_service_manager_status_string(status) != NULL,
              "manager status string is present for every status");
    }
    CHECK(appliance_service_manager_status_string(
              (enum appliance_service_manager_status)13U) == NULL,
          "manager status string rejects undefined status");

    for (state = APPLIANCE_SERVICE_MANAGER_STATE_STOPPED;
         state <= APPLIANCE_SERVICE_MANAGER_STATE_STOP_FAILED;
         ++state) {
        CHECK(appliance_service_manager_state_string(state) != NULL &&
                  strcmp(appliance_service_manager_state_string(state),
                         state == APPLIANCE_SERVICE_MANAGER_STATE_STOPPED ?
                             "STOPPED" :
                         state == APPLIANCE_SERVICE_MANAGER_STATE_RUNNING ?
                             "RUNNING" :
                         state == APPLIANCE_SERVICE_MANAGER_STATE_START_FAILED ?
                             "START_FAILED" :
                             "STOP_FAILED") == 0,
              "manager state string is exact");
    }
    CHECK(appliance_service_manager_state_string(
              (enum appliance_service_manager_state)4U) == NULL,
          "manager state string rejects undefined state");

    for (policy = APPLIANCE_SERVICE_MANAGER_STARTUP_ROLLBACK;
         policy <= APPLIANCE_SERVICE_MANAGER_STARTUP_PRESERVE;
         ++policy) {
        CHECK(appliance_service_manager_startup_policy_string(policy) != NULL &&
                  strcmp(appliance_service_manager_startup_policy_string(policy),
                         policy == APPLIANCE_SERVICE_MANAGER_STARTUP_ROLLBACK ?
                             "ROLLBACK" : "PRESERVE") == 0,
              "manager startup policy string is exact");
    }
    CHECK(appliance_service_manager_startup_policy_string(
              (enum appliance_service_manager_startup_policy)2U) == NULL,
          "manager startup policy string rejects undefined policy");
}

static void test_init_null_and_invalid(void)
{
    struct manager_fake fake;
    struct appliance_service_manager manager;
    struct appliance_service_manager_ops ops;

    reset_fake(&fake);
    ops = make_ops(&fake);
    memset(&manager, 0, sizeof(manager));

    CHECK(appliance_service_manager_init(
              NULL, &ops, APPLIANCE_SERVICE_MANAGER_STARTUP_ROLLBACK) ==
              APPLIANCE_SERVICE_MANAGER_NULL,
          "manager init rejects null manager");
    CHECK(appliance_service_manager_init(
              &manager, NULL, APPLIANCE_SERVICE_MANAGER_STARTUP_ROLLBACK) ==
              APPLIANCE_SERVICE_MANAGER_NULL,
          "manager init rejects null ops");

    ops.start_ordered = NULL;
    CHECK(appliance_service_manager_init(
              &manager, &ops, APPLIANCE_SERVICE_MANAGER_STARTUP_ROLLBACK) ==
              APPLIANCE_SERVICE_MANAGER_BAD_STATE,
          "manager init rejects missing start callback");
    ops = make_ops(&fake);
    ops.stop_ordered = NULL;
    CHECK(appliance_service_manager_init(
              &manager, &ops, APPLIANCE_SERVICE_MANAGER_STARTUP_ROLLBACK) ==
              APPLIANCE_SERVICE_MANAGER_BAD_STATE,
          "manager init rejects missing stop callback");
    ops = make_ops(&fake);
    ops.restart_contained = NULL;
    CHECK(appliance_service_manager_init(
              &manager, &ops, APPLIANCE_SERVICE_MANAGER_STARTUP_ROLLBACK) ==
              APPLIANCE_SERVICE_MANAGER_BAD_STATE,
          "manager init rejects missing restart callback");
    ops = make_ops(&fake);
    ops.snapshot = NULL;
    CHECK(appliance_service_manager_init(
              &manager, &ops, APPLIANCE_SERVICE_MANAGER_STARTUP_ROLLBACK) ==
              APPLIANCE_SERVICE_MANAGER_BAD_STATE,
          "manager init rejects missing snapshot callback");
    ops = make_ops(&fake);
    CHECK(appliance_service_manager_init(
              &manager, &ops,
              (enum appliance_service_manager_startup_policy)7U) ==
              APPLIANCE_SERVICE_MANAGER_BAD_STATE,
          "manager init rejects invalid startup policy");

    CHECK(appliance_service_manager_init(
              &manager, &ops, APPLIANCE_SERVICE_MANAGER_STARTUP_PRESERVE) ==
              APPLIANCE_SERVICE_MANAGER_OK &&
              manager.initialized &&
              manager.version == APPLIANCE_SERVICE_MANAGER_VERSION &&
              manager.state == APPLIANCE_SERVICE_MANAGER_STATE_STOPPED &&
              manager.startup_policy ==
                  APPLIANCE_SERVICE_MANAGER_STARTUP_PRESERVE,
          "manager init publishes preserve policy");
    CHECK(appliance_service_manager_init(
              &manager, &ops, APPLIANCE_SERVICE_MANAGER_STARTUP_ROLLBACK) ==
              APPLIANCE_SERVICE_MANAGER_BAD_STATE,
          "manager double init returns BAD_STATE");
}

static void test_configure_null_invalid(void)
{
    struct manager_fake fake;
    struct appliance_service_manager manager;
    struct appliance_service_manager_ops ops;
    struct service_descriptor descriptor =
        make_descriptor(1U, true, "service-a");
    struct service_supervisor_entry entry = make_entry(1U);

    reset_fake(&fake);
    ops = make_ops(&fake);
    memset(&manager, 0, sizeof(manager));
    CHECK(appliance_service_manager_init(
              &manager, &ops, APPLIANCE_SERVICE_MANAGER_STARTUP_ROLLBACK) ==
              APPLIANCE_SERVICE_MANAGER_OK,
          "configure fixture initializes manager");

    CHECK(appliance_service_manager_configure(
              NULL, &descriptor, 1U, &entry, 1U) ==
              APPLIANCE_SERVICE_MANAGER_NULL,
          "manager configure rejects null manager");
    CHECK(appliance_service_manager_configure(
              &manager, NULL, 1U, &entry, 1U) ==
              APPLIANCE_SERVICE_MANAGER_NULL,
          "manager configure rejects null descriptors with count");
    CHECK(appliance_service_manager_configure(
              &manager, &descriptor, 1U, NULL, 1U) ==
              APPLIANCE_SERVICE_MANAGER_NULL,
          "manager configure rejects null entries with count");

    memset(&manager, 0, sizeof(manager));
    CHECK(appliance_service_manager_configure(
              &manager, &descriptor, 1U, &entry, 1U) ==
              APPLIANCE_SERVICE_MANAGER_BAD_STATE,
          "manager configure rejects uninitialized manager");

    CHECK(appliance_service_manager_init(
              &manager, &ops, APPLIANCE_SERVICE_MANAGER_STARTUP_ROLLBACK) ==
              APPLIANCE_SERVICE_MANAGER_OK,
          "configure fixture reinitializes manager");
    descriptor.version = 99U;
    CHECK(appliance_service_manager_configure(
              &manager, &descriptor, 1U, &entry, 1U) ==
              APPLIANCE_SERVICE_MANAGER_INVALID_DESCRIPTOR_SET,
          "manager configure rejects malformed descriptor set");
    descriptor = make_descriptor(1U, true, "service-a");
    entry.service_id = 2U;
    CHECK(appliance_service_manager_configure(
              &manager, &descriptor, 1U, &entry, 1U) ==
              APPLIANCE_SERVICE_MANAGER_INVALID_ENTRIES,
          "manager configure rejects entry for unknown service");
    entry = make_entry(1U);
    entry.entry = NULL;
    CHECK(appliance_service_manager_configure(
              &manager, &descriptor, 1U, &entry, 1U) ==
              APPLIANCE_SERVICE_MANAGER_INVALID_ENTRIES,
          "manager configure rejects null entry function");
}

static void test_configure_success(void)
{
    struct manager_fake fake;
    struct appliance_service_manager manager;
    struct appliance_service_manager_ops ops;
    struct service_descriptor descriptors[2];
    struct service_supervisor_entry entries[2];

    descriptors[0] = make_descriptor(1U, true, "base");
    descriptors[1] = make_descriptor_with_dependency(2U, 1U, "app");
    entries[0] = make_entry(1U);
    entries[1] = make_entry(2U);
    reset_fake(&fake);
    ops = make_ops(&fake);
    memset(&manager, 0, sizeof(manager));
    CHECK(appliance_service_manager_init(
              &manager, &ops, APPLIANCE_SERVICE_MANAGER_STARTUP_ROLLBACK) ==
              APPLIANCE_SERVICE_MANAGER_OK,
          "configure success fixture initializes manager");
    CHECK(appliance_service_manager_configure(
              &manager, descriptors, 2U, entries, 2U) ==
              APPLIANCE_SERVICE_MANAGER_OK &&
              manager.graph.published &&
              manager.entry_count == 2U &&
              service_graph_snapshot_start_id(&manager.graph, 0U) == 1U &&
              service_graph_snapshot_start_id(&manager.graph, 1U) == 2U,
          "manager configure publishes bounded dependency graph");

    memset(&manager, 0, sizeof(manager));
    CHECK(appliance_service_manager_init(
              &manager, &ops, APPLIANCE_SERVICE_MANAGER_STARTUP_ROLLBACK) ==
              APPLIANCE_SERVICE_MANAGER_OK,
          "configure duplicate fixture initializes manager");
    descriptors[1] = make_descriptor(1U, true, "duplicate");
    CHECK(appliance_service_manager_configure(
              &manager, descriptors, 2U, entries, 2U) ==
              APPLIANCE_SERVICE_MANAGER_INVALID_DESCRIPTOR_SET &&
              !manager.graph.published,
          "manager configure rejects duplicate set without publication");
}

static void test_start_stop_policies(void)
{
    struct manager_fake fake;
    struct appliance_service_manager manager;
    struct appliance_service_manager_ops ops;
    struct service_descriptor descriptor =
        make_descriptor(1U, true, "service-a");
    struct service_supervisor_entry entry = make_entry(1U);

    reset_fake(&fake);
    ops = make_ops(&fake);
    memset(&manager, 0, sizeof(manager));
    CHECK(appliance_service_manager_init(
              &manager, &ops, APPLIANCE_SERVICE_MANAGER_STARTUP_ROLLBACK) ==
              APPLIANCE_SERVICE_MANAGER_OK,
          "start rollback fixture initializes manager");
    CHECK(appliance_service_manager_start(&manager) ==
              APPLIANCE_SERVICE_MANAGER_NOT_CONFIGURED,
          "manager start rejects unconfigured graph");
    CHECK(appliance_service_manager_configure(
              &manager, &descriptor, 1U, &entry, 1U) ==
              APPLIANCE_SERVICE_MANAGER_OK,
          "start fixture configures one service");
    CHECK(appliance_service_manager_start(&manager) ==
              APPLIANCE_SERVICE_MANAGER_OK &&
              manager.state == APPLIANCE_SERVICE_MANAGER_STATE_RUNNING &&
              manager.generation == 1U &&
              fake.start_calls == 1U,
          "manager start delegates and publishes RUNNING");
    CHECK(appliance_service_manager_start(&manager) ==
              APPLIANCE_SERVICE_MANAGER_BAD_STATE &&
              fake.start_calls == 1U,
          "manager second start returns BAD_STATE");
    CHECK(appliance_service_manager_stop(&manager) ==
              APPLIANCE_SERVICE_MANAGER_OK &&
              manager.state == APPLIANCE_SERVICE_MANAGER_STATE_STOPPED &&
              fake.stop_calls == 1U,
          "manager stop delegates and returns STOPPED");

    reset_fake(&fake);
    fake.start_status = SERVICE_SUPERVISOR_PROCESS_FAILED;
    memset(&manager, 0, sizeof(manager));
    CHECK(appliance_service_manager_init(
              &manager, &ops, APPLIANCE_SERVICE_MANAGER_STARTUP_ROLLBACK) ==
              APPLIANCE_SERVICE_MANAGER_OK &&
              appliance_service_manager_configure(
                  &manager, &descriptor, 1U, &entry, 1U) ==
                  APPLIANCE_SERVICE_MANAGER_OK,
          "rollback failure fixture configures manager");
    CHECK(appliance_service_manager_start(&manager) ==
              APPLIANCE_SERVICE_MANAGER_START_FAILED &&
              manager.state == APPLIANCE_SERVICE_MANAGER_STATE_STOPPED &&
              fake.start_calls == 1U &&
              fake.stop_calls == 1U,
          "manager rollback policy stops partial start on failure");

    reset_fake(&fake);
    fake.start_status = SERVICE_SUPERVISOR_PROCESS_FAILED;
    memset(&manager, 0, sizeof(manager));
    CHECK(appliance_service_manager_init(
              &manager, &ops, APPLIANCE_SERVICE_MANAGER_STARTUP_PRESERVE) ==
              APPLIANCE_SERVICE_MANAGER_OK &&
              appliance_service_manager_configure(
                  &manager, &descriptor, 1U, &entry, 1U) ==
                  APPLIANCE_SERVICE_MANAGER_OK &&
              appliance_service_manager_start(&manager) ==
                  APPLIANCE_SERVICE_MANAGER_START_FAILED &&
              manager.state ==
                  APPLIANCE_SERVICE_MANAGER_STATE_START_FAILED &&
              fake.stop_calls == 0U,
          "manager preserve policy retains partial start for stop");
    CHECK(appliance_service_manager_stop(&manager) ==
              APPLIANCE_SERVICE_MANAGER_OK &&
              manager.state == APPLIANCE_SERVICE_MANAGER_STATE_STOPPED &&
              fake.stop_calls == 1U,
          "manager stop cleans preserve-start failure");

    reset_fake(&fake);
    fake.stop_status = SERVICE_SUPERVISOR_PROCESS_FAILED;
    memset(&manager, 0, sizeof(manager));
    CHECK(appliance_service_manager_init(
              &manager, &ops, APPLIANCE_SERVICE_MANAGER_STARTUP_PRESERVE) ==
              APPLIANCE_SERVICE_MANAGER_OK &&
              appliance_service_manager_configure(
                  &manager, &descriptor, 1U, &entry, 1U) ==
                  APPLIANCE_SERVICE_MANAGER_OK &&
              appliance_service_manager_start(&manager) ==
                  APPLIANCE_SERVICE_MANAGER_OK &&
              appliance_service_manager_stop(&manager) ==
                  APPLIANCE_SERVICE_MANAGER_STOP_FAILED &&
              manager.state == APPLIANCE_SERVICE_MANAGER_STATE_STOP_FAILED,
          "manager stop failure publishes STOP_FAILED");
}

static void test_restart(void)
{
    struct manager_fake fake;
    struct appliance_service_manager manager;
    struct appliance_service_manager_ops ops;
    struct service_descriptor descriptor =
        make_descriptor(1U, true, "service-a");
    struct service_supervisor_entry entry = make_entry(1U);
    uint32_t exit_status = 0U;
    bool restarted = false;

    reset_fake(&fake);
    ops = make_ops(&fake);
    memset(&manager, 0, sizeof(manager));
    CHECK(appliance_service_manager_init(
              &manager, &ops, APPLIANCE_SERVICE_MANAGER_STARTUP_ROLLBACK) ==
              APPLIANCE_SERVICE_MANAGER_OK &&
              appliance_service_manager_configure(
                  &manager, &descriptor, 1U, &entry, 1U) ==
                  APPLIANCE_SERVICE_MANAGER_OK &&
              appliance_service_manager_start(&manager) ==
                  APPLIANCE_SERVICE_MANAGER_OK,
          "restart fixture starts manager");
    CHECK(appliance_service_manager_restart(
              &manager, 1U, 50U, &exit_status, &restarted) ==
              APPLIANCE_SERVICE_MANAGER_OK &&
              restarted &&
              fake.restart_calls == 1U &&
              fake.last_service_id == 1U &&
              fake.last_now_ticks == 50U,
          "manager restart delegates contained restart");

    fake.last_restarted = false;
    CHECK(appliance_service_manager_restart(
              &manager, 1U, 60U, &exit_status, &restarted) ==
              APPLIANCE_SERVICE_MANAGER_RESTART_DISABLED &&
              !restarted,
          "manager reports disabled contained restart");

    fake.restart_status = SERVICE_SUPERVISOR_DEPENDENCY_BLOCKED;
    CHECK(appliance_service_manager_restart(
              &manager, 1U, 70U, &exit_status, &restarted) ==
              APPLIANCE_SERVICE_MANAGER_RESTART_FAILED,
          "manager maps restart failure");
}

static void test_snapshot_and_render(void)
{
    struct manager_fake fake;
    struct appliance_service_manager manager;
    struct appliance_service_manager_ops ops;
    struct service_descriptor descriptor =
        make_descriptor(1U, true, "service-a");
    struct service_supervisor_entry entry = make_entry(1U);
    struct appliance_service_manager_snapshot snapshot;
    struct appliance_service_manager_snapshot failed_snapshot;
    char output[APPLIANCE_SERVICE_MANAGER_OUTPUT_MAX + 1U];
    size_t output_length = 0U;

    reset_fake(&fake);
    fill_healthy_snapshot(&fake.supervisor_snapshot);
    ops = make_ops(&fake);
    memset(&manager, 0, sizeof(manager));
    CHECK(appliance_service_manager_init(
              &manager, &ops, APPLIANCE_SERVICE_MANAGER_STARTUP_ROLLBACK) ==
              APPLIANCE_SERVICE_MANAGER_OK &&
              appliance_service_manager_configure(
                  &manager, &descriptor, 1U, &entry, 1U) ==
                  APPLIANCE_SERVICE_MANAGER_OK &&
              appliance_service_manager_start(&manager) ==
                  APPLIANCE_SERVICE_MANAGER_OK,
          "snapshot fixture starts manager");
    memset(&snapshot, 0, sizeof(snapshot));
    CHECK(appliance_service_manager_snapshot(&manager, &snapshot) ==
              APPLIANCE_SERVICE_MANAGER_OK &&
              snapshot.published &&
              snapshot.version == APPLIANCE_SERVICE_MANAGER_VERSION &&
              snapshot.state == APPLIANCE_SERVICE_MANAGER_STATE_RUNNING &&
              snapshot.configured_count == 1U &&
              snapshot.active_count == 2U &&
              snapshot.ready_count == 2U &&
              snapshot.pending_count == 0U &&
              snapshot.failed_count == 0U &&
              snapshot.health_result == HEALTH_CHECK_RESULT_PASS,
          "manager snapshot publishes exact health counts");

    memset(output, 0, sizeof(output));
    output_length = 0U;
    CHECK(appliance_service_manager_render(
              &snapshot, output, sizeof(output), &output_length) ==
              APPLIANCE_SERVICE_MANAGER_OK &&
              strcmp(output,
                     "appliance_services v=1 state=RUNNING "
                     "policy=ROLLBACK generation=1 configured=1 active=2 "
                     "ready=2 pending=0 failed=0 exhausted=0 health=PASS\n") == 0,
          "manager snapshot renders canonical line");

    fill_failed_snapshot(&fake.supervisor_snapshot);
    CHECK(appliance_service_manager_snapshot(&manager, &failed_snapshot) ==
              APPLIANCE_SERVICE_MANAGER_OK &&
              failed_snapshot.ready_count == 1U &&
              failed_snapshot.failed_count == 1U &&
              failed_snapshot.health_result == HEALTH_CHECK_RESULT_FAIL,
          "manager snapshot exposes failure evidence");

    memset(output, 0, sizeof(output));
    CHECK(appliance_service_manager_render(
              NULL, output, sizeof(output), &output_length) ==
              APPLIANCE_SERVICE_MANAGER_NULL,
          "manager render rejects null snapshot");
    CHECK(appliance_service_manager_render(
              &snapshot, NULL, sizeof(output), &output_length) ==
              APPLIANCE_SERVICE_MANAGER_NULL,
          "manager render rejects null output");
    output_length = sizeof(output);
    CHECK(appliance_service_manager_render(
              &snapshot, output, 4U, &output_length) ==
              APPLIANCE_SERVICE_MANAGER_OVERFLOW,
          "manager render rejects overflow");

    fake.snapshot_status = SERVICE_SUPERVISOR_SNAPSHOT_INVALID;
    CHECK(appliance_service_manager_snapshot(&manager, &snapshot) ==
              APPLIANCE_SERVICE_MANAGER_SNAPSHOT_FAILED,
          "manager snapshot maps supervisor snapshot failure");
}

int main(void)
{
    test_strings();
    test_init_null_and_invalid();
    test_configure_null_invalid();
    test_configure_success();
    test_start_stop_policies();
    test_restart();
    test_snapshot_and_render();

    if (failures != 0) {
        fprintf(stderr, "Appliance service manager host tests: %d FAILURES\n",
                failures);
        return 1;
    }
    puts("Appliance service manager host tests: ALL OK");
    return 0;
}

#else
#error "appliance-service-manager-host.c requires HFOC_APPLIANCE_SERVICE_MANAGER_HOST_TEST"
#endif
