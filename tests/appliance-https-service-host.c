#include "appliance_https_service.h"

#include "appliance_service_manager.h"
#include "https_service.h"
#include "https_service_lifecycle.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifdef HFOC_APPLIANCE_HTTPS_SERVICE_HOST_TEST

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

static void service_entry(void) {
}

static struct service_descriptor make_service_descriptor(void)
{
    struct service_descriptor descriptor;

    memset(&descriptor, 0, sizeof(descriptor));
    descriptor.id = HTTPS_SERVICE_ID;
    descriptor.version = SERVICE_DESCRIPTOR_VERSION;
    descriptor.enabled = true;
    descriptor.restart_limit = HTTPS_SERVICE_RESTART_BUDGET_DEFAULT;
    descriptor.readiness_timeout_ms =
        HTTPS_SERVICE_READINESS_DEADLINE_DEFAULT_MS;
    memcpy(descriptor.name, HTTPS_SERVICE_NAME,
           HTTPS_SERVICE_NAME_LENGTH + 1U);
    return descriptor;
}

static struct service_supervisor_entry make_service_entry(void)
{
    struct service_supervisor_entry entry;

    memset(&entry, 0, sizeof(entry));
    entry.service_id = HTTPS_SERVICE_ID;
    entry.entry = service_entry;
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

static struct appliance_service_manager_ops make_manager_ops(
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

static void reset_manager_fake(struct manager_fake *fake)
{
    memset(fake, 0, sizeof(*fake));
    fake->start_status = SERVICE_SUPERVISOR_OK;
    fake->stop_status = SERVICE_SUPERVISOR_OK;
    fake->restart_status = SERVICE_SUPERVISOR_OK;
    fake->snapshot_status = SERVICE_SUPERVISOR_OK;
    fake->last_restarted = true;
}

static void fill_https_snapshot(struct service_supervisor_snapshot *snapshot)
{
    struct service_descriptor descriptor = make_service_descriptor();
    struct service_supervisor_snapshot_entry *entry = &snapshot->entries[0];

    memset(snapshot, 0, sizeof(*snapshot));
    snapshot->version = SERVICE_SUPERVISOR_SNAPSHOT_VERSION;
    snapshot->published = true;
    snapshot->generation = 1U;
    snapshot->active_count = 1U;
    snapshot->entry_count = 1U;
    (void)service_descriptor_init(&entry->descriptor, &descriptor);
    entry->state = SERVICE_SUPERVISOR_STATE_RUNNING;
    entry->health_state = SERVICE_HEALTH_READY;
}

static void setup_manager(
    struct manager_fake *fake,
    struct appliance_service_manager *manager)
{
    struct service_descriptor descriptor = make_service_descriptor();
    struct service_supervisor_entry entry = make_service_entry();
    struct appliance_service_manager_ops ops;

    reset_manager_fake(fake);
    fill_https_snapshot(&fake->supervisor_snapshot);
    ops = make_manager_ops(fake);
    memset(manager, 0, sizeof(*manager));
    CHECK(appliance_service_manager_init(
              manager, &ops,
              APPLIANCE_SERVICE_MANAGER_STARTUP_ROLLBACK) ==
              APPLIANCE_SERVICE_MANAGER_OK &&
              appliance_service_manager_configure(
                  manager, &descriptor, 1U, &entry, 1U) ==
                  APPLIANCE_SERVICE_MANAGER_OK,
          "HTTPS composition fixture configures manager");
}

static struct https_service_resource_envelope make_envelope(void)
{
    struct https_service_resource_envelope envelope;

    memset(&envelope, 0, sizeof(envelope));
    envelope.connection_capacity = HTTPS_SERVICE_CONNECTION_CAPACITY;
    envelope.tls_context_capacity = HTTPS_SERVICE_TLS_CONTEXT_CAPACITY;
    envelope.http_parser_capacity = HTTPS_SERVICE_HTTP_PARSER_CAPACITY;
    envelope.packet_pool_capacity =
        HTTPS_SERVICE_PACKET_POOL_CAPACITY_DEFAULT;
    return envelope;
}

static struct https_service_descriptor make_https_descriptor(void)
{
    struct https_service_descriptor descriptor;

    memset(&descriptor, 0, sizeof(descriptor));
    descriptor.version = HTTPS_SERVICE_DESCRIPTOR_VERSION;
    descriptor.id = HTTPS_SERVICE_ID;
    descriptor.restart_budget = HTTPS_SERVICE_RESTART_BUDGET_DEFAULT;
    descriptor.readiness_deadline_ms =
        HTTPS_SERVICE_READINESS_DEADLINE_DEFAULT_MS;
    descriptor.envelope = make_envelope();
    memcpy(descriptor.name, HTTPS_SERVICE_NAME,
           HTTPS_SERVICE_NAME_LENGTH + 1U);
    return descriptor;
}

static struct https_service_lifecycle_config make_lifecycle_config(void)
{
    struct https_service_lifecycle_config config;

    memset(&config, 0, sizeof(config));
    config.version = HTTPS_SERVICE_LIFECYCLE_VERSION;
    config.drain_timeout_ms = 100U;
    config.connection_capacity =
        HTTPS_SERVICE_LIFECYCLE_CONNECTION_CAPACITY;
    config.tls_context_capacity =
        HTTPS_SERVICE_LIFECYCLE_TLS_CONTEXT_CAPACITY;
    config.http_parser_capacity =
        HTTPS_SERVICE_LIFECYCLE_HTTP_PARSER_CAPACITY;
    config.packet_pool_capacity =
        HTTPS_SERVICE_PACKET_POOL_CAPACITY_DEFAULT;
    return config;
}

struct lifecycle_callback_state {
    bool stop_result;
    bool drain_results[HTTPS_SERVICE_LIFECYCLE_CONNECTION_CAPACITY];
    bool abort_results[HTTPS_SERVICE_LIFECYCLE_CONNECTION_CAPACITY];
    bool zeroize_result;
    bool http_result;
    bool packet_result;
    bool restart_result;
    uint32_t stop_calls;
    uint32_t drain_calls;
    uint32_t abort_calls;
    uint32_t zeroize_calls;
    uint32_t http_calls;
    uint32_t packet_calls;
    uint32_t restart_calls;
};

static bool callback_stop_admission(void *context, uint32_t generation)
{
    struct lifecycle_callback_state *state = context;

    (void)generation;
    ++state->stop_calls;
    return state->stop_result;
}

static bool callback_drain_connection(
    void *context,
    uint32_t generation,
    uint32_t slot)
{
    struct lifecycle_callback_state *state = context;
    bool result;

    (void)generation;
    ++state->drain_calls;
    result = state->drain_results[slot];
    state->drain_results[slot] = false;
    return result;
}

static bool callback_abort_connection(
    void *context,
    uint32_t generation,
    uint32_t slot)
{
    struct lifecycle_callback_state *state = context;
    bool result;

    (void)generation;
    ++state->abort_calls;
    result = state->abort_results[slot];
    state->abort_results[slot] = false;
    return result;
}

static bool callback_zeroize_tls(
    void *context,
    uint32_t generation,
    uint32_t count)
{
    struct lifecycle_callback_state *state = context;

    (void)generation;
    (void)count;
    ++state->zeroize_calls;
    return state->zeroize_result;
}

static bool callback_release_http(
    void *context,
    uint32_t generation,
    uint32_t count)
{
    struct lifecycle_callback_state *state = context;

    (void)generation;
    (void)count;
    ++state->http_calls;
    return state->http_result;
}

static bool callback_release_packet(
    void *context,
    uint32_t generation,
    uint32_t count)
{
    struct lifecycle_callback_state *state = context;

    (void)generation;
    (void)count;
    ++state->packet_calls;
    return state->packet_result;
}

static bool callback_restart_admission(void *context, uint32_t generation)
{
    struct lifecycle_callback_state *state = context;

    (void)generation;
    ++state->restart_calls;
    return state->restart_result;
}

static void reset_lifecycle_callback_state(
    struct lifecycle_callback_state *state)
{
    uint32_t index;

    memset(state, 0, sizeof(*state));
    state->stop_result = true;
    state->zeroize_result = true;
    state->http_result = true;
    state->packet_result = true;
    state->restart_result = true;
    for (index = 0U;
         index < HTTPS_SERVICE_LIFECYCLE_CONNECTION_CAPACITY;
         ++index) {
        state->drain_results[index] = true;
        state->abort_results[index] = true;
    }
}

static struct https_service_lifecycle_ops make_lifecycle_ops(
    struct lifecycle_callback_state *state)
{
    struct https_service_lifecycle_ops ops;

    memset(&ops, 0, sizeof(ops));
    ops.stop_admission = callback_stop_admission;
    ops.drain_connection = callback_drain_connection;
    ops.abort_connection = callback_abort_connection;
    ops.zeroize_tls = callback_zeroize_tls;
    ops.release_http = callback_release_http;
    ops.release_packet = callback_release_packet;
    ops.restart_admission = callback_restart_admission;
    ops.context = state;
    return ops;
}

static struct https_service_readiness make_readiness(uint32_t mask)
{
    struct https_service_readiness readiness;

    memset(&readiness, 0, sizeof(readiness));
    readiness.version = HTTPS_SERVICE_READINESS_VERSION;
    readiness.available_mask = mask;
    return readiness;
}

static void test_strings(void)
{
    enum appliance_https_service_status status;
    enum appliance_https_service_state state;
    enum appliance_https_service_failure failure;

    for (state = APPLIANCE_HTTPS_SERVICE_STATE_STOPPED;
         state <= APPLIANCE_HTTPS_SERVICE_STATE_FAILED;
         ++state) {
        const char *string = appliance_https_service_state_string(state);

        CHECK(string != NULL &&
                  (state == APPLIANCE_HTTPS_SERVICE_STATE_STOPPED ?
                       strcmp(string, "STOPPED") == 0 :
                   state == APPLIANCE_HTTPS_SERVICE_STATE_STARTING ?
                       strcmp(string, "STARTING") == 0 :
                   state == APPLIANCE_HTTPS_SERVICE_STATE_RUNNING ?
                       strcmp(string, "RUNNING") == 0 :
                   state == APPLIANCE_HTTPS_SERVICE_STATE_STOPPING ?
                       strcmp(string, "STOPPING") == 0 :
                       strcmp(string, "FAILED") == 0),
              "HTTPS composition state string is exact");
    }
    CHECK(appliance_https_service_state_string(
              (enum appliance_https_service_state)5U) == NULL,
          "HTTPS composition rejects undefined state string");

    for (failure = APPLIANCE_HTTPS_SERVICE_FAILURE_NONE;
         failure <= APPLIANCE_HTTPS_SERVICE_FAILURE_SNAPSHOT;
         ++failure) {
        const char *string = appliance_https_service_failure_string(failure);

        CHECK(string != NULL &&
                  (failure == APPLIANCE_HTTPS_SERVICE_FAILURE_NONE ?
                       strcmp(string, "NONE") == 0 :
                   failure == APPLIANCE_HTTPS_SERVICE_FAILURE_START ?
                       strcmp(string, "START") == 0 :
                   failure == APPLIANCE_HTTPS_SERVICE_FAILURE_READY ?
                       strcmp(string, "READY") == 0 :
                   failure == APPLIANCE_HTTPS_SERVICE_FAILURE_STOP ?
                       strcmp(string, "STOP") == 0 :
                   failure == APPLIANCE_HTTPS_SERVICE_FAILURE_RESTART ?
                       strcmp(string, "RESTART") == 0 :
                       strcmp(string, "SNAPSHOT") == 0),
              "HTTPS composition failure string is exact");
    }
    CHECK(appliance_https_service_failure_string(
              (enum appliance_https_service_failure)6U) == NULL,
          "HTTPS composition rejects undefined failure string");

    for (status = APPLIANCE_HTTPS_SERVICE_OK;
         status <= APPLIANCE_HTTPS_SERVICE_OVERFLOW;
         ++status) {
        CHECK(appliance_https_service_status_string(status) != NULL,
              "HTTPS composition status string is present for every status");
    }
    CHECK(appliance_https_service_status_string(
              (enum appliance_https_service_status)17U) == NULL,
          "HTTPS composition rejects undefined status string");
}

static void test_init_null_and_invalid(void)
{
    struct manager_fake fake;
    struct appliance_service_manager manager;
    struct appliance_https_service service;
    struct https_service_descriptor descriptor = make_https_descriptor();
    struct https_service_lifecycle_config config = make_lifecycle_config();
    struct lifecycle_callback_state callback_state;
    struct https_service_lifecycle_ops ops;

    setup_manager(&fake, &manager);
    reset_lifecycle_callback_state(&callback_state);
    ops = make_lifecycle_ops(&callback_state);
    memset(&service, 0, sizeof(service));

    CHECK(appliance_https_service_init(
              NULL, &manager, &descriptor, &config, &ops) ==
              APPLIANCE_HTTPS_SERVICE_NULL,
          "HTTPS composition init rejects null service");
    CHECK(appliance_https_service_init(
              &service, NULL, &descriptor, &config, &ops) ==
              APPLIANCE_HTTPS_SERVICE_NULL,
          "HTTPS composition init rejects null manager");
    CHECK(appliance_https_service_init(
              &service, &manager, NULL, &config, &ops) ==
              APPLIANCE_HTTPS_SERVICE_NULL,
          "HTTPS composition init rejects null descriptor");
    CHECK(appliance_https_service_init(
              &service, &manager, &descriptor, NULL, &ops) ==
              APPLIANCE_HTTPS_SERVICE_NULL,
          "HTTPS composition init rejects null lifecycle config");
    CHECK(appliance_https_service_init(
              &service, &manager, &descriptor, &config, NULL) ==
              APPLIANCE_HTTPS_SERVICE_NULL,
          "HTTPS composition init rejects null lifecycle ops");

    memset(&service, 0, sizeof(service));
    memset(&manager, 0, sizeof(manager));
    CHECK(appliance_https_service_init(
              &service, &manager, &descriptor, &config, &ops) ==
              APPLIANCE_HTTPS_SERVICE_INVALID_MANAGER,
          "HTTPS composition rejects uninitialized manager");

    setup_manager(&fake, &manager);
    memset(&manager.graph, 0, sizeof(manager.graph));
    memset(&service, 0, sizeof(service));
    CHECK(appliance_https_service_init(
              &service, &manager, &descriptor, &config, &ops) ==
              APPLIANCE_HTTPS_SERVICE_INVALID_MANAGER,
          "HTTPS composition rejects manager without configured graph");

    setup_manager(&fake, &manager);
    memset(&service, 0, sizeof(service));
    descriptor.version = HTTPS_SERVICE_DESCRIPTOR_VERSION + 1U;
    CHECK(appliance_https_service_init(
              &service, &manager, &descriptor, &config, &ops) ==
              APPLIANCE_HTTPS_SERVICE_INVALID_DESCRIPTOR,
          "HTTPS composition rejects unknown HTTPS descriptor version");

    setup_manager(&fake, &manager);
    descriptor = make_https_descriptor();
    config.packet_pool_capacity =
        HTTPS_SERVICE_PACKET_POOL_CAPACITY_DEFAULT + 1U;
    memset(&service, 0, sizeof(service));
    CHECK(appliance_https_service_init(
              &service, &manager, &descriptor, &config, &ops) ==
              APPLIANCE_HTTPS_SERVICE_INVALID_CONFIG,
          "HTTPS composition rejects lifecycle/descriptor envelope mismatch");

    setup_manager(&fake, &manager);
    config = make_lifecycle_config();
    ops.stop_admission = NULL;
    memset(&service, 0, sizeof(service));
    CHECK(appliance_https_service_init(
              &service, &manager, &descriptor, &config, &ops) ==
              APPLIANCE_HTTPS_SERVICE_INVALID_CONFIG,
          "HTTPS composition rejects missing lifecycle callback");
}

static void test_init_valid_and_double(void)
{
    struct manager_fake fake;
    struct appliance_service_manager manager;
    struct appliance_https_service service;
    struct https_service_descriptor descriptor = make_https_descriptor();
    struct https_service_lifecycle_config config = make_lifecycle_config();
    struct lifecycle_callback_state callback_state;
    struct https_service_lifecycle_ops ops;

    setup_manager(&fake, &manager);
    reset_lifecycle_callback_state(&callback_state);
    ops = make_lifecycle_ops(&callback_state);
    memset(&service, 0, sizeof(service));

    CHECK(appliance_https_service_init(
              &service, &manager, &descriptor, &config, &ops) ==
              APPLIANCE_HTTPS_SERVICE_OK &&
              appliance_https_service_published(&service) &&
              service.initialized &&
              service.version == APPLIANCE_HTTPS_SERVICE_VERSION &&
              service.state == APPLIANCE_HTTPS_SERVICE_STATE_STOPPED &&
              service.manager == &manager &&
              https_service_descriptor_published(&service.descriptor) &&
              service.lifecycle.phase ==
                  HTTPS_SERVICE_LIFECYCLE_PHASE_IDLE,
          "HTTPS composition init publishes descriptor and lifecycle");
    CHECK(appliance_https_service_init(
              &service, &manager, &descriptor, &config, &ops) ==
              APPLIANCE_HTTPS_SERVICE_ALREADY_PUBLISHED,
          "HTTPS composition init rejects double publication");
}

static void setup_ready_service(
    struct manager_fake *fake,
    struct appliance_service_manager *manager,
    struct lifecycle_callback_state *callback_state,
    struct appliance_https_service *service,
    struct https_service_lifecycle_ops *ops)
{
    struct https_service_descriptor descriptor = make_https_descriptor();
    struct https_service_lifecycle_config config = make_lifecycle_config();
    struct https_service_readiness readiness =
        make_readiness(HTTPS_SERVICE_REQUIRED_DEPENDENCY_MASK);

    setup_manager(fake, manager);
    reset_lifecycle_callback_state(callback_state);
    *ops = make_lifecycle_ops(callback_state);
    memset(service, 0, sizeof(*service));
    CHECK(appliance_https_service_init(
              service, manager, &descriptor, &config, ops) ==
              APPLIANCE_HTTPS_SERVICE_OK,
          "ready fixture initializes HTTPS composition");
    CHECK(appliance_https_service_start(service, &readiness) ==
              APPLIANCE_HTTPS_SERVICE_OK &&
              service->state == APPLIANCE_HTTPS_SERVICE_STATE_STARTING &&
              fake->start_calls == 1U,
          "ready fixture starts manager");
    CHECK(appliance_https_service_ready(service, &readiness) ==
              APPLIANCE_HTTPS_SERVICE_OK &&
              service->state == APPLIANCE_HTTPS_SERVICE_STATE_RUNNING &&
              service->lifecycle.phase ==
                  HTTPS_SERVICE_LIFECYCLE_PHASE_LISTENING &&
              https_service_lifecycle_active_connection_count(
                  &service->lifecycle) ==
                  HTTPS_SERVICE_LIFECYCLE_CONNECTION_CAPACITY,
          "ready fixture publishes HTTPS lifecycle");
}

static void test_start_ready_gating(void)
{
    struct manager_fake fake;
    struct appliance_service_manager manager;
    struct lifecycle_callback_state callback_state;
    struct appliance_https_service service;
    struct https_service_lifecycle_ops ops;
    struct https_service_descriptor descriptor = make_https_descriptor();
    struct https_service_lifecycle_config config = make_lifecycle_config();
    struct https_service_readiness missing =
        make_readiness(HTTPS_SERVICE_REQUIRED_DEPENDENCY_MASK &
                       ~HTTPS_SERVICE_DEPENDENCY_NETWORK_READY);
    struct https_service_readiness malformed;

    setup_manager(&fake, &manager);
    reset_lifecycle_callback_state(&callback_state);
    ops = make_lifecycle_ops(&callback_state);
    memset(&service, 0, sizeof(service));
    CHECK(appliance_https_service_init(
              &service, &manager, &descriptor, &config, &ops) ==
              APPLIANCE_HTTPS_SERVICE_OK,
          "start gating fixture initializes service");

    CHECK(appliance_https_service_start(&service, &missing) ==
              APPLIANCE_HTTPS_SERVICE_DEPENDENCY_UNAVAILABLE &&
              service.state == APPLIANCE_HTTPS_SERVICE_STATE_STOPPED &&
              fake.start_calls == 0U,
          "HTTPS composition missing dependency prevents manager start");

    malformed = make_readiness(HTTPS_SERVICE_REQUIRED_DEPENDENCY_MASK);
    malformed.version = HTTPS_SERVICE_READINESS_VERSION + 1U;
    CHECK(appliance_https_service_start(&service, &malformed) ==
              APPLIANCE_HTTPS_SERVICE_INVALID_READINESS &&
              fake.start_calls == 0U,
          "HTTPS composition rejects malformed readiness before start");

    setup_ready_service(
        &fake, &manager, &callback_state, &service, &ops);
    CHECK(appliance_https_service_start(&service, &missing) ==
              APPLIANCE_HTTPS_SERVICE_BAD_STATE,
          "HTTPS composition rejects second start while starting");

    CHECK(appliance_https_service_snapshot(&service, NULL) ==
              APPLIANCE_HTTPS_SERVICE_NULL,
          "HTTPS composition snapshot rejects null output");
}

static void test_stop(void)
{
    struct manager_fake fake;
    struct appliance_service_manager manager;
    struct lifecycle_callback_state callback_state;
    struct appliance_https_service service;
    struct https_service_lifecycle_ops ops;

    setup_ready_service(
        &fake, &manager, &callback_state, &service, &ops);
    CHECK(appliance_https_service_stop_admission(&service, 100U) ==
              APPLIANCE_HTTPS_SERVICE_OK &&
              service.state == APPLIANCE_HTTPS_SERVICE_STATE_STOPPING &&
              service.lifecycle.phase ==
                  HTTPS_SERVICE_LIFECYCLE_PHASE_DRAINING &&
              callback_state.stop_calls == 1U,
          "HTTPS composition stop admission closes HTTPS listener");
    CHECK(appliance_https_service_stop_step(&service, 101U) ==
              APPLIANCE_HTTPS_SERVICE_OK &&
              service.state == APPLIANCE_HTTPS_SERVICE_STATE_STOPPED &&
              service.lifecycle.phase ==
                  HTTPS_SERVICE_LIFECYCLE_PHASE_STOPPED &&
              callback_state.drain_calls ==
                  HTTPS_SERVICE_LIFECYCLE_CONNECTION_CAPACITY &&
              callback_state.zeroize_calls == 1U &&
              callback_state.http_calls == 1U &&
              callback_state.packet_calls == 1U &&
              fake.stop_calls == 1U,
          "HTTPS composition stop drains, cleans, and stops manager");
    CHECK(appliance_https_service_stop_admission(&service, 102U) ==
              APPLIANCE_HTTPS_SERVICE_OK &&
              fake.stop_calls == 1U,
          "HTTPS composition stop is idempotent after clean stop");

    setup_ready_service(
        &fake, &manager, &callback_state, &service, &ops);
    CHECK(appliance_https_service_stop_admission(&service, 200U) ==
              APPLIANCE_HTTPS_SERVICE_OK,
          "HTTPS composition stop failure fixture closes admission");
    callback_state.zeroize_result = false;
    CHECK(appliance_https_service_stop_step(&service, 201U) ==
              APPLIANCE_HTTPS_SERVICE_STOP_FAILED &&
              service.state == APPLIANCE_HTTPS_SERVICE_STATE_FAILED &&
              service.failure == APPLIANCE_HTTPS_SERVICE_FAILURE_STOP,
          "HTTPS composition stop cleanup failure publishes STOP failure");
}

static void test_restart(void)
{
    struct manager_fake fake;
    struct appliance_service_manager manager;
    struct lifecycle_callback_state callback_state;
    struct appliance_https_service service;
    struct https_service_lifecycle_ops ops;
    struct https_service_readiness readiness =
        make_readiness(HTTPS_SERVICE_REQUIRED_DEPENDENCY_MASK);
    uint32_t exit_status = 0U;
    bool restarted = false;

    setup_ready_service(
        &fake, &manager, &callback_state, &service, &ops);
    CHECK(appliance_https_service_restart(
              &service, &readiness, 300U, 40U,
              &exit_status, &restarted) ==
              APPLIANCE_HTTPS_SERVICE_OK &&
              restarted &&
              service.state == APPLIANCE_HTTPS_SERVICE_STATE_RUNNING &&
              service.restart_count == 1U &&
              service.lifecycle.phase ==
                  HTTPS_SERVICE_LIFECYCLE_PHASE_RESTARTED &&
              service.lifecycle.generation == 2U &&
              callback_state.stop_calls == 1U &&
              callback_state.abort_calls ==
                  HTTPS_SERVICE_LIFECYCLE_CONNECTION_CAPACITY &&
              callback_state.zeroize_calls == 1U &&
              callback_state.restart_calls == 1U &&
              fake.restart_calls == 1U &&
              fake.last_service_id == HTTPS_SERVICE_ID &&
              fake.last_now_ticks == 40U,
          "HTTPS composition restart cleans and contains one HTTPS service");

    setup_ready_service(
        &fake, &manager, &callback_state, &service, &ops);
    fake.last_restarted = false;
    CHECK(appliance_https_service_restart(
              &service, &readiness, 400U, 50U,
              &exit_status, &restarted) ==
              APPLIANCE_HTTPS_SERVICE_RESTART_FAILED &&
              service.state == APPLIANCE_HTTPS_SERVICE_STATE_FAILED &&
              service.failure == APPLIANCE_HTTPS_SERVICE_FAILURE_RESTART,
          "HTTPS composition disabled restart publishes RESTART failure");
}

static void test_snapshot_render_failure(void)
{
    struct manager_fake fake;
    struct appliance_service_manager manager;
    struct lifecycle_callback_state callback_state;
    struct appliance_https_service service;
    struct https_service_lifecycle_ops ops;
    struct appliance_https_service_snapshot snapshot;
    char output[APPLIANCE_HTTPS_SERVICE_OUTPUT_MAX + 1U];
    size_t output_length = 0U;
    const char *expected =
        "appliance_https v=1 state=RUNNING service=https "
        "manager=RUNNING health=PASS service_health=READY "
        "lifecycle=LISTENING generation=1 connections=4 "
        "start=1 stop=0 restart=0 failure=NONE\n";

    setup_ready_service(
        &fake, &manager, &callback_state, &service, &ops);
    memset(&snapshot, 0, sizeof(snapshot));
    CHECK(appliance_https_service_snapshot(&service, &snapshot) ==
              APPLIANCE_HTTPS_SERVICE_OK &&
              snapshot.published &&
              snapshot.version == APPLIANCE_HTTPS_SERVICE_VERSION &&
              snapshot.state == APPLIANCE_HTTPS_SERVICE_STATE_RUNNING &&
              snapshot.manager_state ==
                  APPLIANCE_SERVICE_MANAGER_STATE_RUNNING &&
              snapshot.manager_health == HEALTH_CHECK_RESULT_PASS &&
              snapshot.service_health == SERVICE_HEALTH_READY &&
              snapshot.lifecycle_phase ==
                  HTTPS_SERVICE_LIFECYCLE_PHASE_LISTENING &&
              snapshot.lifecycle_generation == 1U &&
              snapshot.active_connection_count ==
                  HTTPS_SERVICE_LIFECYCLE_CONNECTION_CAPACITY &&
              snapshot.start_count == 1U,
          "HTTPS composition snapshot publishes exact running state");

    memset(output, 0, sizeof(output));
    CHECK(appliance_https_service_render(
              &snapshot, output, sizeof(output), &output_length) ==
              APPLIANCE_HTTPS_SERVICE_OK &&
              strcmp(output, expected) == 0,
          "HTTPS composition renderer emits canonical line");
    CHECK(appliance_https_service_render(
              NULL, output, sizeof(output), &output_length) ==
              APPLIANCE_HTTPS_SERVICE_NULL,
          "HTTPS composition renderer rejects null snapshot");
    CHECK(appliance_https_service_render(
              &snapshot, NULL, sizeof(output), &output_length) ==
              APPLIANCE_HTTPS_SERVICE_NULL,
          "HTTPS composition renderer rejects null output");
    output_length = sizeof(output);
    CHECK(appliance_https_service_render(
              &snapshot, output, 4U, &output_length) ==
              APPLIANCE_HTTPS_SERVICE_OVERFLOW,
          "HTTPS composition renderer rejects overflow");

    fake.snapshot_status = SERVICE_SUPERVISOR_SNAPSHOT_INVALID;
    CHECK(appliance_https_service_snapshot(&service, &snapshot) ==
              APPLIANCE_HTTPS_SERVICE_SNAPSHOT_FAILED,
          "HTTPS composition snapshot maps manager snapshot failure");
}

int main(void)
{
    test_strings();
    test_init_null_and_invalid();
    test_init_valid_and_double();
    test_start_ready_gating();
    test_stop();
    test_restart();
    test_snapshot_render_failure();

    if (alloc_attempts != 0U || free_attempts != 0U) {
        fprintf(stderr, "Appliance HTTPS service host tests: allocation observed\n");
        return 1;
    }
    if (failures != 0) {
        fprintf(stderr, "Appliance HTTPS service host tests: %d FAILURES\n",
                failures);
        return 1;
    }
    puts("Appliance HTTPS service host tests: ALL OK");
    return 0;
}

#else
#error "appliance-https-service-host.c requires HFOC_APPLIANCE_HTTPS_SERVICE_HOST_TEST"
#endif
