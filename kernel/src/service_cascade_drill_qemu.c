#ifdef HFOC_SERVICE_CASCADE_DRILL_QEMU_TEST

#include "fault_injection.h"
#include "serial.h"
#include "service.h"
#include "service_supervisor.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define CASCADE_DRILL_SERVICE_BASE_ID 1U
#define CASCADE_DRILL_SERVICE_MIDDLE_ID 2U
#define CASCADE_DRILL_SERVICE_TOP_ID 3U
#define CASCADE_DRILL_SERVICE_COUNT 3U

static struct service_supervisor cascade_drill_supervisor;
static struct service_graph_snapshot cascade_drill_snapshot;

static void cascade_drill_failure(const char *message)
    __attribute__((noreturn));

static void cascade_drill_failure(const char *message)
{
    {
        const uint64_t serial_flags = serial_sequence_begin();
        serial_write("[SERVICE-CASCADE-DRILL] failed: ");
        serial_write(message);
        serial_write("\n");
        serial_sequence_end(serial_flags);
    }
    for (;;) {
        __asm__ volatile ("hlt" : : : "memory");
    }
}

static void cascade_drill_require(bool condition, const char *message)
{
    if (!condition) {
        cascade_drill_failure(message);
    }
}

static void cascade_drill_copy_name(char *destination,
                                    const char *source)
{
    uint32_t index = 0U;

    while (source[index] != '\0' && index < SERVICE_NAME_MAX) {
        destination[index] = source[index];
        ++index;
    }
    destination[index] = '\0';
}

static struct service_descriptor cascade_drill_descriptor(
    uint32_t id,
    const char *name,
    uint32_t restart_limit,
    const uint8_t *dependencies,
    uint32_t dependency_count)
{
    struct service_descriptor descriptor;
    uint32_t index;

    for (index = 0U; index < sizeof(descriptor); ++index) {
        ((unsigned char *)&descriptor)[index] = 0U;
    }
    descriptor.id = id;
    descriptor.version = SERVICE_DESCRIPTOR_VERSION;
    descriptor.enabled = true;
    descriptor.restart_limit = restart_limit;
    descriptor.readiness_timeout_ms = 1000U;
    descriptor.cpu_ticks_limit = 0U;
    descriptor.user_memory_limit_pages = 0U;
    cascade_drill_copy_name(descriptor.name, name);
    descriptor.dependency_count = dependency_count;
    for (index = 0U; index < dependency_count; ++index) {
        descriptor.dependencies[index] = dependencies[index];
    }
    return descriptor;
}

static void cascade_drill_service_a_entry(void) {
}

static void cascade_drill_service_b_entry(void) {
}

static void cascade_drill_service_c_entry(void) {
}

static void cascade_drill_check_service_faults(void)
{
    const struct fault_injection_site *site = NULL;

    cascade_drill_require(
        fault_injection_site_by_id(
            FAULT_INJECTION_SERVICE_DEPENDENCY, &site) ==
            FAULT_INJECTION_OK &&
            site->subsystem == FAULT_INJECTION_SUBSYSTEM_SERVICE &&
            site->bounded && site->test_only &&
        fault_injection_site_by_id(
            FAULT_INJECTION_SERVICE_RESTART_EXHAUSTION, &site) ==
            FAULT_INJECTION_OK &&
            site->subsystem == FAULT_INJECTION_SUBSYSTEM_SERVICE &&
            site->bounded && site->test_only &&
        fault_injection_site_by_id(
            FAULT_INJECTION_SERVICE_CONTROLLED_STOP, &site) ==
            FAULT_INJECTION_OK &&
            site->subsystem == FAULT_INJECTION_SUBSYSTEM_SERVICE &&
            site->bounded && site->test_only,
        "catalogue service fault points");
}

void service_cascade_drill_qemu_test_init(uint64_t hhdm_offset)
{
    const uint8_t dependency_middle[1] = {CASCADE_DRILL_SERVICE_BASE_ID};
    const uint8_t dependency_top[1] = {CASCADE_DRILL_SERVICE_MIDDLE_ID};
    struct service_descriptor descriptors[
        CASCADE_DRILL_SERVICE_COUNT];
    struct service_supervisor_entry entries[
        CASCADE_DRILL_SERVICE_COUNT];
    uint32_t exit_status = 0U;
    bool restarted = false;
    uint32_t index;

    serial_write("[SERVICE-CASCADE-DRILL] proof init\n");
    cascade_drill_check_service_faults();
    serial_write("[SERVICE-CASCADE-DRILL] catalogue service faults exact\n");

    descriptors[0] = cascade_drill_descriptor(
        CASCADE_DRILL_SERVICE_BASE_ID, "cascade-base", 2U,
        NULL, 0U);
    descriptors[1] = cascade_drill_descriptor(
        CASCADE_DRILL_SERVICE_MIDDLE_ID, "cascade-middle", 1U,
        dependency_middle, 1U);
    descriptors[2] = cascade_drill_descriptor(
        CASCADE_DRILL_SERVICE_TOP_ID, "cascade-top", 0U,
        dependency_top, 1U);

    cascade_drill_require(
        service_graph_snapshot_build(
            &cascade_drill_snapshot, descriptors,
            CASCADE_DRILL_SERVICE_COUNT) == SERVICE_GRAPH_OK &&
        service_graph_snapshot_start_id(
            &cascade_drill_snapshot, 0U) ==
            CASCADE_DRILL_SERVICE_BASE_ID &&
        service_graph_snapshot_start_id(
            &cascade_drill_snapshot, 1U) ==
            CASCADE_DRILL_SERVICE_MIDDLE_ID &&
        service_graph_snapshot_start_id(
            &cascade_drill_snapshot, 2U) ==
            CASCADE_DRILL_SERVICE_TOP_ID,
        "dependency start order");
    cascade_drill_require(
        service_supervisor_init(
            &cascade_drill_supervisor, hhdm_offset) ==
            SERVICE_SUPERVISOR_OK,
        "supervisor init");

    entries[0].service_id = CASCADE_DRILL_SERVICE_BASE_ID;
    entries[0].entry = cascade_drill_service_a_entry;
    entries[1].service_id = CASCADE_DRILL_SERVICE_MIDDLE_ID;
    entries[1].entry = cascade_drill_service_b_entry;
    entries[2].service_id = CASCADE_DRILL_SERVICE_TOP_ID;
    entries[2].entry = cascade_drill_service_c_entry;
    cascade_drill_require(
        service_supervisor_start_ordered(
            &cascade_drill_supervisor, &cascade_drill_snapshot,
            entries, CASCADE_DRILL_SERVICE_COUNT) ==
            SERVICE_SUPERVISOR_OK &&
        cascade_drill_supervisor.active_count ==
            CASCADE_DRILL_SERVICE_COUNT &&
        cascade_drill_supervisor.supervisor_pcb.child_count ==
            CASCADE_DRILL_SERVICE_COUNT,
        "ordered service start");
    serial_write("[SERVICE-CASCADE-DRILL] service chain started\n");

    cascade_drill_require(
        service_supervisor_exit(
            &cascade_drill_supervisor,
            CASCADE_DRILL_SERVICE_BASE_ID, 11U) ==
            SERVICE_SUPERVISOR_OK &&
        service_supervisor_reap_contained(
            &cascade_drill_supervisor,
            CASCADE_DRILL_SERVICE_BASE_ID, 20U,
            &cascade_drill_snapshot, &exit_status,
            &restarted) == SERVICE_SUPERVISOR_OK &&
        exit_status == 11U && restarted &&
        cascade_drill_supervisor.supervisor_pcb.child_count == 3U,
        "first contained restart");
    serial_write("[SERVICE-CASCADE-DRILL] base restart contained\n");

    restarted = false;
    cascade_drill_require(
        service_supervisor_exit(
            &cascade_drill_supervisor,
            CASCADE_DRILL_SERVICE_BASE_ID, 12U) ==
            SERVICE_SUPERVISOR_OK &&
        service_supervisor_reap_contained(
            &cascade_drill_supervisor,
            CASCADE_DRILL_SERVICE_BASE_ID, 20U,
            &cascade_drill_snapshot, &exit_status,
            &restarted) == SERVICE_SUPERVISOR_RESTART_INTERVAL &&
        exit_status == 12U && !restarted &&
        cascade_drill_supervisor.services[0].process == NULL &&
        cascade_drill_supervisor.supervisor_pcb.child_count == 2U,
        "restart interval no replacement");
    serial_write("[SERVICE-CASCADE-DRILL] restart interval bounded\n");

    restarted = false;
    cascade_drill_require(
        service_supervisor_exit(
            &cascade_drill_supervisor,
            CASCADE_DRILL_SERVICE_MIDDLE_ID, 21U) ==
            SERVICE_SUPERVISOR_OK &&
        service_supervisor_reap_contained(
            &cascade_drill_supervisor,
            CASCADE_DRILL_SERVICE_MIDDLE_ID, 20U,
            &cascade_drill_snapshot, &exit_status,
            &restarted) == SERVICE_SUPERVISOR_DEPENDENCY_BLOCKED &&
        exit_status == 21U && !restarted &&
        cascade_drill_supervisor.services[1].process == NULL &&
        cascade_drill_supervisor.supervisor_pcb.child_count == 1U,
        "dependent blocked while base unavailable");
    serial_write("[SERVICE-CASCADE-DRILL] dependent cascade blocked\n");

    restarted = false;
    cascade_drill_require(
        service_supervisor_restart_contained(
            &cascade_drill_supervisor,
            CASCADE_DRILL_SERVICE_BASE_ID, 30U,
            &cascade_drill_snapshot, &restarted) ==
            SERVICE_SUPERVISOR_OK && restarted &&
        cascade_drill_supervisor.services[0].process != NULL &&
        cascade_drill_supervisor.supervisor_pcb.child_count == 2U,
        "base recovery");
    restarted = false;
    cascade_drill_require(
        service_supervisor_restart_contained(
            &cascade_drill_supervisor,
            CASCADE_DRILL_SERVICE_MIDDLE_ID, 30U,
            &cascade_drill_snapshot, &restarted) ==
            SERVICE_SUPERVISOR_OK && restarted &&
        cascade_drill_supervisor.services[1].process != NULL &&
        cascade_drill_supervisor.supervisor_pcb.child_count == 3U,
        "dependent recovery");
    serial_write("[SERVICE-CASCADE-DRILL] later recovery published\n");

    restarted = false;
    cascade_drill_require(
        service_supervisor_exit(
            &cascade_drill_supervisor,
            CASCADE_DRILL_SERVICE_TOP_ID, 31U) ==
            SERVICE_SUPERVISOR_OK &&
        service_supervisor_reap_contained(
            &cascade_drill_supervisor,
            CASCADE_DRILL_SERVICE_TOP_ID, 30U,
            &cascade_drill_snapshot, &exit_status,
            &restarted) == SERVICE_SUPERVISOR_EXHAUSTED &&
        exit_status == 31U && !restarted &&
        cascade_drill_supervisor.services[2].process == NULL &&
        cascade_drill_supervisor.supervisor_pcb.child_count == 2U,
        "restart exhaustion no orphan");
    serial_write("[SERVICE-CASCADE-DRILL] exhaustion no orphan\n");

    cascade_drill_require(
        service_supervisor_stop_ordered(
            &cascade_drill_supervisor, &cascade_drill_snapshot) ==
            SERVICE_SUPERVISOR_OK &&
        cascade_drill_supervisor.active_count == 1U &&
        cascade_drill_supervisor.supervisor_pcb.child_count == 0U,
        "ordered stop only exhausted slot and no child");
    for (index = 0U;
         index < SERVICE_SUPERVISOR_CAPACITY;
         ++index) {
        cascade_drill_require(
            cascade_drill_supervisor.services[index].process == NULL,
            "no process pointer after stop");
    }
    serial_write("[SERVICE-CASCADE-DRILL] clean shutdown\n");
    serial_write("[SERVICE-CASCADE-DRILL] proof OK\n");
}

#else
typedef int service_cascade_drill_qemu_translation_unit_guard;
#endif
