#ifdef HFOC_SERVICE_SUPERVISOR_QEMU_TEST

#include "appliance_service_manager.h"
#include "serial.h"
#include "service.h"
#include "service_supervisor.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define APP_SERVICE_MANAGER_QEMU_SERVICE_COUNT 3U

static struct service_supervisor app_service_manager_qemu_supervisor;
static struct appliance_service_manager app_service_manager_qemu_manager;

static void app_service_manager_qemu_fail(const char *message)
    __attribute__((noreturn));

static void app_service_manager_qemu_fail(const char *message)
{
    {
        const uint64_t serial_flags = serial_sequence_begin();
        serial_write("[APP-SVC] failed: ");
        serial_write(message);
        serial_write("\n");
        serial_sequence_end(serial_flags);
    }
    for (;;) {
        __asm__ volatile ("hlt" : : : "memory");
    }
}

static void app_service_manager_qemu_require(
    bool condition,
    const char *message)
{
    if (!condition) {
        app_service_manager_qemu_fail(message);
    }
}

static void app_service_manager_qemu_zero_bytes(
    void *destination,
    size_t length)
{
    unsigned char *bytes = destination;
    size_t index;

    for (index = 0U; index < length; ++index) {
        bytes[index] = 0U;
    }
}

static bool app_service_manager_qemu_contains(
    const char *text,
    const char *needle)
{
    uint32_t index;
    uint32_t needle_index;

    for (index = 0U; text[index] != '\0'; ++index) {
        needle_index = 0U;
        while (needle[needle_index] != '\0' &&
               text[index + needle_index] == needle[needle_index]) {
            ++needle_index;
        }
        if (needle[needle_index] == '\0') {
            return true;
        }
    }
    return false;
}

static void app_service_manager_qemu_copy_name(
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

static struct service_descriptor app_service_manager_qemu_descriptor(
    uint32_t id,
    const char *name,
    const uint8_t *dependencies,
    uint32_t dependency_count)
{
    struct service_descriptor descriptor;
    uint32_t index;

    app_service_manager_qemu_zero_bytes(
        &descriptor, sizeof(descriptor));
    descriptor.id = id;
    descriptor.version = SERVICE_DESCRIPTOR_VERSION;
    descriptor.enabled = true;
    descriptor.restart_limit = 2U;
    descriptor.readiness_timeout_ms = 1000U;
    descriptor.cpu_ticks_limit = 0U;
    descriptor.user_memory_limit_pages = 0U;
    app_service_manager_qemu_copy_name(descriptor.name, name);
    descriptor.dependency_count = dependency_count;
    for (index = 0U; index < dependency_count; ++index) {
        descriptor.dependencies[index] = dependencies[index];
    }
    return descriptor;
}

static void app_service_manager_qemu_service_a_entry(void) {
}

static void app_service_manager_qemu_service_b_entry(void) {
}

static void app_service_manager_qemu_service_c_entry(void) {
}

static enum service_supervisor_status app_service_manager_qemu_start(
    void *context,
    const struct service_graph_snapshot *snapshot,
    const struct service_supervisor_entry *entries,
    uint32_t entry_count)
{
    return service_supervisor_start_ordered(
        (struct service_supervisor *)context,
        snapshot, entries, entry_count);
}

static enum service_supervisor_status app_service_manager_qemu_stop(
    void *context,
    const struct service_graph_snapshot *snapshot)
{
    return service_supervisor_stop_ordered(
        (struct service_supervisor *)context, snapshot);
}

static enum service_supervisor_status app_service_manager_qemu_restart(
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

static enum service_supervisor_status app_service_manager_qemu_snapshot(
    void *context,
    struct service_supervisor_snapshot *snapshot)
{
    return service_supervisor_snapshot(
        (const struct service_supervisor *)context, snapshot);
}

void appliance_service_manager_qemu_test_init(void)
{
    struct service_descriptor descriptors[
        APP_SERVICE_MANAGER_QEMU_SERVICE_COUNT];
    struct service_supervisor_entry entries[
        APP_SERVICE_MANAGER_QEMU_SERVICE_COUNT];
    struct appliance_service_manager_ops ops;
    struct appliance_service_manager_snapshot snapshot;
    char output[APPLIANCE_SERVICE_MANAGER_OUTPUT_MAX + 1U];
    size_t output_length = 0U;
    const uint8_t dependency_middle[1] = {1U};
    const uint8_t dependency_top[1] = {2U};
    bool published = false;
    bool restarted = false;
    uint32_t exit_status = 0U;

    descriptors[0] = app_service_manager_qemu_descriptor(
        1U, "base", NULL, 0U);
    descriptors[1] = app_service_manager_qemu_descriptor(
        2U, "middle", dependency_middle, 1U);
    descriptors[2] = app_service_manager_qemu_descriptor(
        3U, "top", dependency_top, 1U);
    entries[0].service_id = 1U;
    entries[0].entry = app_service_manager_qemu_service_a_entry;
    entries[1].service_id = 2U;
    entries[1].entry = app_service_manager_qemu_service_b_entry;
    entries[2].service_id = 3U;
    entries[2].entry = app_service_manager_qemu_service_c_entry;

    app_service_manager_qemu_zero_bytes(&ops, sizeof(ops));
    ops.start_ordered = app_service_manager_qemu_start;
    ops.stop_ordered = app_service_manager_qemu_stop;
    ops.restart_contained = app_service_manager_qemu_restart;
    ops.snapshot = app_service_manager_qemu_snapshot;
    ops.context = &app_service_manager_qemu_supervisor;

    app_service_manager_qemu_require(
        service_supervisor_init(
            &app_service_manager_qemu_supervisor, UINT64_C(0xFFFF800000000000)) ==
            SERVICE_SUPERVISOR_OK,
        "service manager supervisor init");
    app_service_manager_qemu_require(
        appliance_service_manager_init(
            &app_service_manager_qemu_manager, &ops,
            APPLIANCE_SERVICE_MANAGER_STARTUP_ROLLBACK) ==
            APPLIANCE_SERVICE_MANAGER_OK,
        "service manager init");
    app_service_manager_qemu_require(
        appliance_service_manager_configure(
            &app_service_manager_qemu_manager,
            descriptors, APP_SERVICE_MANAGER_QEMU_SERVICE_COUNT,
            entries, APP_SERVICE_MANAGER_QEMU_SERVICE_COUNT) ==
            APPLIANCE_SERVICE_MANAGER_OK &&
        service_graph_snapshot_start_id(
            &app_service_manager_qemu_manager.graph, 0U) == 1U &&
        service_graph_snapshot_start_id(
            &app_service_manager_qemu_manager.graph, 1U) == 2U &&
        service_graph_snapshot_start_id(
            &app_service_manager_qemu_manager.graph, 2U) == 3U,
        "service manager dependency configure");
    app_service_manager_qemu_require(
        appliance_service_manager_start(
            &app_service_manager_qemu_manager) ==
            APPLIANCE_SERVICE_MANAGER_OK &&
        app_service_manager_qemu_manager.state ==
            APPLIANCE_SERVICE_MANAGER_STATE_RUNNING,
        "service manager ordered start");
    serial_write("[APP-SVC] manager init\n");
    serial_write("[APP-SVC] dependency order 1 2 3\n");

    app_service_manager_qemu_require(
        service_supervisor_report_ready(
            &app_service_manager_qemu_supervisor, 1U, 10U,
            &published) == SERVICE_SUPERVISOR_OK && published &&
        service_supervisor_report_ready(
            &app_service_manager_qemu_supervisor, 2U, 20U,
            &published) == SERVICE_SUPERVISOR_OK && published &&
        service_supervisor_report_ready(
            &app_service_manager_qemu_supervisor, 3U, 30U,
            &published) == SERVICE_SUPERVISOR_OK && published,
        "service manager readiness publication");
    app_service_manager_qemu_require(
        appliance_service_manager_snapshot(
            &app_service_manager_qemu_manager, &snapshot) ==
            APPLIANCE_SERVICE_MANAGER_OK &&
        snapshot.ready_count == APP_SERVICE_MANAGER_QEMU_SERVICE_COUNT &&
        snapshot.health_result == HEALTH_CHECK_RESULT_PASS,
        "service manager healthy snapshot");
    app_service_manager_qemu_zero_bytes(
        output, sizeof(output));
    app_service_manager_qemu_require(
        appliance_service_manager_render(
            &snapshot, output, sizeof(output), &output_length) ==
            APPLIANCE_SERVICE_MANAGER_OK &&
        app_service_manager_qemu_contains(
            output, "state=RUNNING") &&
        app_service_manager_qemu_contains(
            output, "health=PASS"),
        "service manager canonical health render");
    serial_write("[APP-SVC] healthy snapshot\n");

    app_service_manager_qemu_require(
        service_supervisor_exit(
            &app_service_manager_qemu_supervisor, 3U, 7U) ==
            SERVICE_SUPERVISOR_OK &&
        appliance_service_manager_restart(
            &app_service_manager_qemu_manager, 3U, 200U,
            &exit_status, &restarted) ==
            APPLIANCE_SERVICE_MANAGER_OK &&
        restarted &&
        exit_status == 7U &&
        service_supervisor_health_state(
            &app_service_manager_qemu_supervisor, 3U) ==
            SERVICE_HEALTH_PENDING &&
        service_supervisor_health_state(
            &app_service_manager_qemu_supervisor, 1U) ==
            SERVICE_HEALTH_READY &&
        service_supervisor_health_state(
            &app_service_manager_qemu_supervisor, 2U) ==
            SERVICE_HEALTH_READY,
        "service manager contained restart");
    published = false;
    app_service_manager_qemu_require(
        service_supervisor_report_ready(
            &app_service_manager_qemu_supervisor, 3U, 201U,
            &published) == SERVICE_SUPERVISOR_OK && published,
        "restarted service readiness");
    app_service_manager_qemu_require(
        appliance_service_manager_snapshot(
            &app_service_manager_qemu_manager, &snapshot) ==
            APPLIANCE_SERVICE_MANAGER_OK,
        "post-restart manager snapshot");
    app_service_manager_qemu_require(
        snapshot.ready_count == APP_SERVICE_MANAGER_QEMU_SERVICE_COUNT &&
        snapshot.health_result == HEALTH_CHECK_RESULT_PASS,
        "post-restart manager health");
    serial_write("[APP-SVC] contained restart service=3\n");

    app_service_manager_qemu_require(
        appliance_service_manager_stop(
            &app_service_manager_qemu_manager) ==
            APPLIANCE_SERVICE_MANAGER_OK &&
        app_service_manager_qemu_manager.state ==
            APPLIANCE_SERVICE_MANAGER_STATE_STOPPED &&
        app_service_manager_qemu_supervisor.active_count == 0U,
        "service manager clean shutdown");
    serial_write("[APP-SVC] clean shutdown\n");
    serial_write("[APP-SVC] proof OK\n");
}

#else
typedef int appliance_service_manager_qemu_translation_unit_guard;
#endif
