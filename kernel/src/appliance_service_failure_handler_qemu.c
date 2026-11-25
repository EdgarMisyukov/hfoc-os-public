#ifdef HFOC_APPLIANCE_SERVICE_FAILURE_HANDLER_QEMU_TEST

#include "appliance_service_failure_handler.h"
#include "appliance_service_manager.h"
#include "serial.h"
#include "service.h"
#include "service_supervisor.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define FAIL_HANDLER_QEMU_SERVICE_COUNT 3U

static struct service_supervisor fail_handler_qemu_supervisor;
static struct appliance_service_manager fail_handler_qemu_manager;
static struct appliance_service_failure_handler fail_handler_qemu_handler;

static void fail_handler_qemu_fail(const char *message)
    __attribute__((noreturn));

static void fail_handler_qemu_fail(const char *message)
{
    {
        const uint64_t serial_flags = serial_sequence_begin();
        serial_write("[FAIL-HANDLER] failed: ");
        serial_write(message);
        serial_write("\n");
        serial_sequence_end(serial_flags);
    }
    for (;;) {
        __asm__ volatile ("hlt" : : : "memory");
    }
}

static void fail_handler_qemu_require(
    bool condition,
    const char *message)
{
    if (!condition) {
        fail_handler_qemu_fail(message);
    }
}

static void fail_handler_qemu_zero_bytes(
    void *destination,
    size_t count)
{
    unsigned char *bytes = destination;
    size_t index;

    for (index = 0U; index < count; ++index) {
        bytes[index] = 0U;
    }
}

static void fail_handler_qemu_copy_name(
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

static struct service_descriptor fail_handler_qemu_descriptor(
    uint32_t id,
    const char *name,
    const uint8_t *dependency_ids,
    uint32_t dependency_count)
{
    struct service_descriptor descriptor;
    uint32_t index;

    fail_handler_qemu_zero_bytes(
        &descriptor, sizeof(descriptor));
    descriptor.id = id;
    descriptor.version = SERVICE_DESCRIPTOR_VERSION;
    descriptor.enabled = true;
    descriptor.restart_limit = 2U;
    descriptor.readiness_timeout_ms = 1000U;
    descriptor.cpu_ticks_limit = 0U;
    descriptor.user_memory_limit_pages = 0U;
    fail_handler_qemu_copy_name(descriptor.name, name);
    descriptor.dependency_count = dependency_count;
    for (index = 0U; index < dependency_count; ++index) {
        descriptor.dependencies[index] = dependency_ids[index];
    }
    return descriptor;
}

static void fail_handler_qemu_service_base(void) {
}

static void fail_handler_qemu_service_middle(void) {
}

static void fail_handler_qemu_service_top(void) {
}

static enum service_supervisor_status fail_handler_qemu_start(
    void *context,
    const struct service_graph_snapshot *snapshot,
    const struct service_supervisor_entry *entries,
    uint32_t entry_count)
{
    return service_supervisor_start_ordered(
        (struct service_supervisor *)context,
        snapshot, entries, entry_count);
}

static enum service_supervisor_status fail_handler_qemu_stop(
    void *context,
    const struct service_graph_snapshot *snapshot)
{
    return service_supervisor_stop_ordered(
        (struct service_supervisor *)context, snapshot);
}

static enum service_supervisor_status fail_handler_qemu_restart(
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

static enum service_supervisor_status fail_handler_qemu_snapshot(
    void *context,
    struct service_supervisor_snapshot *snapshot)
{
    return service_supervisor_snapshot(
        (const struct service_supervisor *)context, snapshot);
}

static void fail_handler_qemu_build_graph_and_entries(
    struct service_descriptor descriptors[
        FAIL_HANDLER_QEMU_SERVICE_COUNT],
    struct service_supervisor_entry entries[
        FAIL_HANDLER_QEMU_SERVICE_COUNT])
{
    static const uint8_t middle_dependency[1] = {1U};
    static const uint8_t top_dependency[1] = {2U};

    descriptors[0] = fail_handler_qemu_descriptor(
        1U, "base", NULL, 0U);
    descriptors[1] = fail_handler_qemu_descriptor(
        2U, "middle", middle_dependency, 1U);
    descriptors[2] = fail_handler_qemu_descriptor(
        3U, "top", top_dependency, 1U);
    entries[0].service_id = 1U;
    entries[0].entry = fail_handler_qemu_service_base;
    entries[1].service_id = 2U;
    entries[1].entry = fail_handler_qemu_service_middle;
    entries[2].service_id = 3U;
    entries[2].entry = fail_handler_qemu_service_top;
}

void appliance_service_failure_handler_qemu_test_init(
    uint64_t hhdm_offset)
{
    struct service_descriptor descriptors[
        FAIL_HANDLER_QEMU_SERVICE_COUNT];
    struct service_supervisor_entry entries[
        FAIL_HANDLER_QEMU_SERVICE_COUNT];
    struct appliance_service_manager_ops ops;
    struct appliance_service_failure_event event;
    struct appliance_service_failure_handler_snapshot snapshot;
    char output[APPLIANCE_SERVICE_FAILURE_HANDLER_OUTPUT_MAX + 1U];
    size_t output_length = 0U;
    bool published = false;

    fail_handler_qemu_zero_bytes(&ops, sizeof(ops));
    ops.start_ordered = fail_handler_qemu_start;
    ops.stop_ordered = fail_handler_qemu_stop;
    ops.restart_contained = fail_handler_qemu_restart;
    ops.snapshot = fail_handler_qemu_snapshot;
    ops.context = &fail_handler_qemu_supervisor;

    fail_handler_qemu_require(
        service_supervisor_init(
            &fail_handler_qemu_supervisor, hhdm_offset) ==
            SERVICE_SUPERVISOR_OK,
        "failure handler supervisor init");
    fail_handler_qemu_build_graph_and_entries(
        descriptors, entries);
    fail_handler_qemu_require(
        appliance_service_manager_init(
            &fail_handler_qemu_manager, &ops,
            APPLIANCE_SERVICE_MANAGER_STARTUP_ROLLBACK) ==
            APPLIANCE_SERVICE_MANAGER_OK,
        "failure handler manager init");
    fail_handler_qemu_require(
        appliance_service_manager_configure(
            &fail_handler_qemu_manager,
            descriptors, FAIL_HANDLER_QEMU_SERVICE_COUNT,
            entries, FAIL_HANDLER_QEMU_SERVICE_COUNT) ==
            APPLIANCE_SERVICE_MANAGER_OK,
        "failure handler manager configure");
    fail_handler_qemu_require(
        appliance_service_manager_start(
            &fail_handler_qemu_manager) ==
            APPLIANCE_SERVICE_MANAGER_OK,
        "failure handler manager start");
    fail_handler_qemu_require(
        service_supervisor_report_ready(
            &fail_handler_qemu_supervisor, 1U, 10U,
            &published) == SERVICE_SUPERVISOR_OK && published &&
        service_supervisor_report_ready(
            &fail_handler_qemu_supervisor, 2U, 20U,
            &published) == SERVICE_SUPERVISOR_OK && published &&
        service_supervisor_report_ready(
            &fail_handler_qemu_supervisor, 3U, 30U,
            &published) == SERVICE_SUPERVISOR_OK && published,
        "failure handler readiness publication");
    fail_handler_qemu_require(
        appliance_service_failure_handler_init(
            &fail_handler_qemu_handler,
            &fail_handler_qemu_manager) ==
            APPLIANCE_SERVICE_FAILURE_HANDLER_OK,
        "failure handler init");
    serial_write("[FAIL-HANDLER] proof init\n");

    fail_handler_qemu_require(
        appliance_service_failure_handler_poll(
            &fail_handler_qemu_handler, 40U, &event) ==
            APPLIANCE_SERVICE_FAILURE_HANDLER_NO_FAILURE,
        "healthy failure handler no-action poll");
    serial_write("[FAIL-HANDLER] healthy no failure\n");

    fail_handler_qemu_require(
        service_supervisor_exit(
            &fail_handler_qemu_supervisor, 3U, 7U) ==
            SERVICE_SUPERVISOR_OK,
        "failure handler service exit");
    fail_handler_qemu_require(
        appliance_service_failure_handler_poll(
            &fail_handler_qemu_handler, 100U, &event) ==
            APPLIANCE_SERVICE_FAILURE_HANDLER_OK &&
            event.published &&
            event.service_id == 3U &&
            event.kind == APPLIANCE_SERVICE_FAILURE_KIND_EXIT &&
            event.action == APPLIANCE_SERVICE_FAILURE_ACTION_RESTART &&
            event.result == APPLIANCE_SERVICE_FAILURE_RESULT_RESTARTED &&
            event.exit_status == 7U &&
            service_supervisor_state(
                &fail_handler_qemu_supervisor, 3U) ==
                SERVICE_SUPERVISOR_STATE_RUNNING,
        "failure handler automatic contained recovery");
    fail_handler_qemu_require(
        service_supervisor_health_state(
            &fail_handler_qemu_supervisor, 1U) ==
            SERVICE_HEALTH_READY &&
        service_supervisor_health_state(
            &fail_handler_qemu_supervisor, 2U) ==
            SERVICE_HEALTH_READY,
        "failure handler unrelated service isolation");
    serial_write("[FAIL-HANDLER] automatic recovery service=3\n");

    published = false;
    fail_handler_qemu_require(
        service_supervisor_report_ready(
            &fail_handler_qemu_supervisor, 3U, 101U,
            &published) == SERVICE_SUPERVISOR_OK && published,
        "failure handler recovered readiness");
    fail_handler_qemu_require(
        appliance_service_failure_handler_snapshot(
            &fail_handler_qemu_handler, &snapshot) ==
            APPLIANCE_SERVICE_FAILURE_HANDLER_OK &&
            snapshot.observed_count == 1U &&
            snapshot.recovered_count == 1U &&
            snapshot.deferred_count == 0U &&
            snapshot.exhausted_count == 0U &&
            snapshot.last_event.service_id == 3U &&
            snapshot.manager_health == HEALTH_CHECK_RESULT_PASS,
        "failure handler healthy post-recovery snapshot");
    fail_handler_qemu_zero_bytes(output, sizeof(output));
    fail_handler_qemu_require(
        appliance_service_failure_handler_render(
            &snapshot, output, sizeof(output), &output_length) ==
            APPLIANCE_SERVICE_FAILURE_HANDLER_OK &&
            output_length > 0U,
        "failure handler canonical render");
    serial_write("[FAIL-HANDLER] recovered readiness\n");

    fail_handler_qemu_require(
        appliance_service_manager_stop(
            &fail_handler_qemu_manager) ==
            APPLIANCE_SERVICE_MANAGER_OK &&
            fail_handler_qemu_supervisor.active_count == 0U,
        "failure handler clean shutdown");
    serial_write("[FAIL-HANDLER] clean shutdown\n");
    serial_write("[FAIL-HANDLER] proof OK\n");
}

#else
typedef int appliance_service_failure_handler_qemu_translation_unit_guard;
#endif
