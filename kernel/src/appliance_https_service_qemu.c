#ifdef HFOC_APPLIANCE_HTTPS_SERVICE_QEMU_TEST

#include "appliance_https_service.h"
#include "appliance_service_manager.h"
#include "https_service.h"
#include "https_service_lifecycle.h"
#include "serial.h"
#include "service.h"
#include "service_supervisor.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

static struct service_supervisor appliance_https_qemu_supervisor;
static struct appliance_service_manager appliance_https_qemu_manager;
static struct appliance_https_service appliance_https_qemu_service;

static void appliance_https_qemu_fail(const char *message)
    __attribute__((noreturn));

static void appliance_https_qemu_fail(const char *message)
{
    {
        const uint64_t serial_flags = serial_sequence_begin();
        serial_write("[APP-HTTPS] failed: ");
        serial_write(message);
        serial_write("\n");
        serial_sequence_end(serial_flags);
    }
    for (;;) {
        __asm__ volatile ("hlt" : : : "memory");
    }
}

static void appliance_https_qemu_require(
    bool condition,
    const char *message)
{
    if (!condition) {
        appliance_https_qemu_fail(message);
    }
}

static void appliance_https_qemu_copy_name(
    char *destination,
    const char *source)
{
    uint32_t index = 0U;

    while (source[index] != '\0' && index < SERVICE_NAME_MAX) {
        destination[index] = source[index];
        ++index;
    }
    destination[index] = '\0';
}

static struct service_descriptor appliance_https_qemu_descriptor(void)
{
    struct service_descriptor descriptor;
    uint32_t index;

    for (index = 0U; index < sizeof(descriptor); ++index) {
        ((unsigned char *)&descriptor)[index] = 0U;
    }
    descriptor.id = HTTPS_SERVICE_ID;
    descriptor.version = SERVICE_DESCRIPTOR_VERSION;
    descriptor.enabled = true;
    descriptor.restart_limit = HTTPS_SERVICE_RESTART_BUDGET_DEFAULT;
    descriptor.readiness_timeout_ms =
        HTTPS_SERVICE_READINESS_DEADLINE_DEFAULT_MS;
    appliance_https_qemu_copy_name(
        descriptor.name, HTTPS_SERVICE_NAME);
    return descriptor;
}

static struct https_service_descriptor appliance_https_qemu_https_descriptor(
    void)
{
    struct https_service_descriptor descriptor;
    uint32_t index;

    for (index = 0U; index < sizeof(descriptor); ++index) {
        ((unsigned char *)&descriptor)[index] = 0U;
    }
    descriptor.version = HTTPS_SERVICE_DESCRIPTOR_VERSION;
    descriptor.id = HTTPS_SERVICE_ID;
    descriptor.restart_budget = HTTPS_SERVICE_RESTART_BUDGET_DEFAULT;
    descriptor.readiness_deadline_ms =
        HTTPS_SERVICE_READINESS_DEADLINE_DEFAULT_MS;
    descriptor.envelope.connection_capacity =
        HTTPS_SERVICE_CONNECTION_CAPACITY;
    descriptor.envelope.tls_context_capacity =
        HTTPS_SERVICE_TLS_CONTEXT_CAPACITY;
    descriptor.envelope.http_parser_capacity =
        HTTPS_SERVICE_HTTP_PARSER_CAPACITY;
    descriptor.envelope.packet_pool_capacity =
        HTTPS_SERVICE_PACKET_POOL_CAPACITY_DEFAULT;
    appliance_https_qemu_copy_name(
        descriptor.name, HTTPS_SERVICE_NAME);
    return descriptor;
}

static void appliance_https_qemu_entry(void) {
}

static enum service_supervisor_status appliance_https_qemu_start(
    void *context,
    const struct service_graph_snapshot *snapshot,
    const struct service_supervisor_entry *entries,
    uint32_t entry_count)
{
    return service_supervisor_start_ordered(
        (struct service_supervisor *)context,
        snapshot, entries, entry_count);
}

static enum service_supervisor_status appliance_https_qemu_stop(
    void *context,
    const struct service_graph_snapshot *snapshot)
{
    return service_supervisor_stop_ordered(
        (struct service_supervisor *)context, snapshot);
}

static enum service_supervisor_status appliance_https_qemu_restart(
    void *context,
    uint32_t service_id,
    uint64_t now_ticks,
    const struct service_graph_snapshot *snapshot,
    uint32_t *exit_status_out,
    bool *restarted)
{
    return service_supervisor_reap_contained(
        (struct service_supervisor *)context,
        service_id, now_ticks, snapshot,
        exit_status_out, restarted);
}

static enum service_supervisor_status appliance_https_qemu_snapshot(
    void *context,
    struct service_supervisor_snapshot *snapshot)
{
    return service_supervisor_snapshot(
        (const struct service_supervisor *)context, snapshot);
}

static bool appliance_https_qemu_stop_admission(
    void *context,
    uint32_t generation)
{
    (void)context;
    (void)generation;
    return true;
}

static bool appliance_https_qemu_drain_connection(
    void *context,
    uint32_t generation,
    uint32_t slot)
{
    (void)context;
    (void)generation;
    (void)slot;
    return true;
}

static bool appliance_https_qemu_abort_connection(
    void *context,
    uint32_t generation,
    uint32_t slot)
{
    (void)context;
    (void)generation;
    (void)slot;
    return true;
}

static bool appliance_https_qemu_zeroize_tls(
    void *context,
    uint32_t generation,
    uint32_t count)
{
    (void)context;
    (void)generation;
    (void)count;
    return true;
}

static bool appliance_https_qemu_release_http(
    void *context,
    uint32_t generation,
    uint32_t count)
{
    (void)context;
    (void)generation;
    (void)count;
    return true;
}

static bool appliance_https_qemu_release_packet(
    void *context,
    uint32_t generation,
    uint32_t count)
{
    (void)context;
    (void)generation;
    (void)count;
    return true;
}

static bool appliance_https_qemu_restart_admission(
    void *context,
    uint32_t generation)
{
    (void)context;
    (void)generation;
    return true;
}

void appliance_https_service_qemu_test_init(uint64_t hhdm_offset)
{
    struct service_descriptor service_descriptor =
        appliance_https_qemu_descriptor();
    struct service_supervisor_entry service_entry;
    struct appliance_service_manager_ops manager_ops;
    struct https_service_descriptor https_descriptor =
        appliance_https_qemu_https_descriptor();
    struct https_service_lifecycle_config lifecycle_config;
    struct https_service_lifecycle_ops lifecycle_ops;
    struct https_service_readiness readiness;
    struct appliance_https_service_snapshot snapshot;
    uint32_t exit_status = 0U;
    bool restarted = false;
    bool published = false;

    service_entry.service_id = HTTPS_SERVICE_ID;
    service_entry.entry = appliance_https_qemu_entry;
    manager_ops.start_ordered = appliance_https_qemu_start;
    manager_ops.stop_ordered = appliance_https_qemu_stop;
    manager_ops.restart_contained = appliance_https_qemu_restart;
    manager_ops.snapshot = appliance_https_qemu_snapshot;
    manager_ops.context = &appliance_https_qemu_supervisor;

    appliance_https_qemu_require(
        service_supervisor_init(
            &appliance_https_qemu_supervisor, hhdm_offset) ==
            SERVICE_SUPERVISOR_OK,
        "appliance HTTPS supervisor init");
    appliance_https_qemu_require(
        appliance_service_manager_init(
            &appliance_https_qemu_manager, &manager_ops,
            APPLIANCE_SERVICE_MANAGER_STARTUP_ROLLBACK) ==
            APPLIANCE_SERVICE_MANAGER_OK,
        "appliance HTTPS manager init");
    appliance_https_qemu_require(
        appliance_service_manager_configure(
            &appliance_https_qemu_manager,
            &service_descriptor, 1U,
            &service_entry, 1U) ==
            APPLIANCE_SERVICE_MANAGER_OK,
        "appliance HTTPS manager configure");

    lifecycle_config.version = HTTPS_SERVICE_LIFECYCLE_VERSION;
    lifecycle_config.drain_timeout_ms = 100U;
    lifecycle_config.connection_capacity =
        HTTPS_SERVICE_LIFECYCLE_CONNECTION_CAPACITY;
    lifecycle_config.tls_context_capacity =
        HTTPS_SERVICE_LIFECYCLE_TLS_CONTEXT_CAPACITY;
    lifecycle_config.http_parser_capacity =
        HTTPS_SERVICE_LIFECYCLE_HTTP_PARSER_CAPACITY;
    lifecycle_config.packet_pool_capacity =
        HTTPS_SERVICE_PACKET_POOL_CAPACITY_DEFAULT;
    lifecycle_ops.stop_admission = appliance_https_qemu_stop_admission;
    lifecycle_ops.drain_connection =
        appliance_https_qemu_drain_connection;
    lifecycle_ops.abort_connection =
        appliance_https_qemu_abort_connection;
    lifecycle_ops.zeroize_tls = appliance_https_qemu_zeroize_tls;
    lifecycle_ops.release_http = appliance_https_qemu_release_http;
    lifecycle_ops.release_packet = appliance_https_qemu_release_packet;
    lifecycle_ops.restart_admission =
        appliance_https_qemu_restart_admission;
    lifecycle_ops.context = &appliance_https_qemu_service;

    readiness.version = HTTPS_SERVICE_READINESS_VERSION;
    readiness.available_mask = HTTPS_SERVICE_REQUIRED_DEPENDENCY_MASK;

    appliance_https_qemu_require(
        appliance_https_service_init(
            &appliance_https_qemu_service,
            &appliance_https_qemu_manager,
            &https_descriptor,
            &lifecycle_config,
            &lifecycle_ops) == APPLIANCE_HTTPS_SERVICE_OK,
        "appliance HTTPS composition init");
    appliance_https_qemu_require(
        appliance_https_service_start(
            &appliance_https_qemu_service,
            &readiness) == APPLIANCE_HTTPS_SERVICE_OK &&
        appliance_https_qemu_service.state ==
            APPLIANCE_HTTPS_SERVICE_STATE_STARTING,
        "appliance HTTPS dependency-gated start");
    serial_write("[APP-HTTPS] dependency-gated start\n");

    appliance_https_qemu_require(
        service_supervisor_report_ready(
            &appliance_https_qemu_supervisor,
            HTTPS_SERVICE_ID, 10U, &published) ==
            SERVICE_SUPERVISOR_OK && published,
        "appliance HTTPS supervisor readiness");
    appliance_https_qemu_require(
        appliance_https_service_ready(
            &appliance_https_qemu_service,
            &readiness) == APPLIANCE_HTTPS_SERVICE_OK &&
        appliance_https_qemu_service.state ==
            APPLIANCE_HTTPS_SERVICE_STATE_RUNNING,
        "appliance HTTPS ready publication");
    serial_write("[APP-HTTPS] ready listening\n");

    appliance_https_qemu_require(
        appliance_https_service_snapshot(
            &appliance_https_qemu_service, &snapshot) ==
            APPLIANCE_HTTPS_SERVICE_OK &&
        snapshot.state == APPLIANCE_HTTPS_SERVICE_STATE_RUNNING &&
        snapshot.service_health == SERVICE_HEALTH_READY &&
        snapshot.lifecycle_phase ==
            HTTPS_SERVICE_LIFECYCLE_PHASE_LISTENING &&
        snapshot.active_connection_count ==
            HTTPS_SERVICE_LIFECYCLE_CONNECTION_CAPACITY &&
        snapshot.manager_health == HEALTH_CHECK_RESULT_PASS,
        "appliance HTTPS healthy snapshot");
    serial_write("[APP-HTTPS] healthy snapshot\n");

    appliance_https_qemu_require(
        service_supervisor_exit(
            &appliance_https_qemu_supervisor,
            HTTPS_SERVICE_ID, 7U) == SERVICE_SUPERVISOR_OK,
        "appliance HTTPS service exit");
    appliance_https_qemu_require(
        appliance_https_service_restart(
            &appliance_https_qemu_service,
            &readiness, 100U, 200U,
            &exit_status, &restarted) ==
            APPLIANCE_HTTPS_SERVICE_OK &&
        restarted &&
        exit_status == 7U &&
        appliance_https_qemu_service.state ==
            APPLIANCE_HTTPS_SERVICE_STATE_RUNNING &&
        appliance_https_qemu_service.lifecycle.phase ==
            HTTPS_SERVICE_LIFECYCLE_PHASE_RESTARTED,
        "appliance HTTPS contained restart");
    published = false;
    appliance_https_qemu_require(
        service_supervisor_report_ready(
            &appliance_https_qemu_supervisor,
            HTTPS_SERVICE_ID, 201U, &published) ==
            SERVICE_SUPERVISOR_OK && published,
        "appliance HTTPS post-restart readiness");
    appliance_https_qemu_require(
        appliance_https_service_snapshot(
            &appliance_https_qemu_service, &snapshot) ==
            APPLIANCE_HTTPS_SERVICE_OK &&
        snapshot.restart_count == 1U &&
        snapshot.service_health == SERVICE_HEALTH_READY &&
        snapshot.lifecycle_phase ==
            HTTPS_SERVICE_LIFECYCLE_PHASE_RESTARTED,
        "appliance HTTPS post-restart snapshot");
    serial_write("[APP-HTTPS] contained restart lifecycle\n");

    appliance_https_qemu_require(
        appliance_https_service_stop_admission(
            &appliance_https_qemu_service, 300U) ==
            APPLIANCE_HTTPS_SERVICE_OK &&
        appliance_https_qemu_service.state ==
            APPLIANCE_HTTPS_SERVICE_STATE_STOPPING,
        "appliance HTTPS stop admission");
    appliance_https_qemu_require(
        appliance_https_service_stop_step(
            &appliance_https_qemu_service, 301U) ==
            APPLIANCE_HTTPS_SERVICE_OK &&
        appliance_https_qemu_service.state ==
            APPLIANCE_HTTPS_SERVICE_STATE_STOPPED &&
        appliance_https_qemu_supervisor.active_count == 0U,
        "appliance HTTPS clean shutdown");
    serial_write("[APP-HTTPS] clean shutdown\n");
    serial_write("[APP-HTTPS] proof OK\n");
}

#else
typedef int appliance_https_service_qemu_translation_unit_guard;
#endif
