#ifdef HFOC_SERVICE_SUPERVISOR_QEMU_TEST

#include "audit.h"
#include "event_ring.h"
#include "keyboard.h"
#include "serial.h"
#include "service.h"
#include "service_supervisor.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define SUPERVISOR_QEMU_SERVICE_BASE_ID 1U
#define SUPERVISOR_QEMU_SERVICE_MIDDLE_ID 2U
#define SUPERVISOR_QEMU_SERVICE_TOP_ID 3U
#define SUPERVISOR_QEMU_SERVICE_COUNT 3U

static struct service_supervisor supervisor_qemu_state;
static struct service_graph_snapshot supervisor_qemu_snapshot;
static bool supervisor_qemu_proof_initialized;
static bool supervisor_qemu_proof_ok;

static void supervisor_qemu_fail(const char *message)
    __attribute__((noreturn));

static void supervisor_qemu_fail(const char *message)
{
    {
        const uint64_t serial_flags = serial_sequence_begin();
        serial_write("[SUPERVISOR] failed: ");
        serial_write(message);
        serial_write("\n");
        serial_sequence_end(serial_flags);
    }
    for (;;) {
        __asm__ volatile ("hlt" : : : "memory");
    }
}

static void supervisor_qemu_require(bool condition, const char *message)
{
    if (!condition) {
        supervisor_qemu_fail(message);
    }
}

static void supervisor_qemu_copy_name(char *destination,
                                      const char *source)
{
    uint32_t index = 0U;

    while (source[index] != '\0' && index < SERVICE_NAME_MAX) {
        destination[index] = source[index];
        ++index;
    }
    destination[index] = '\0';
}

static struct service_descriptor supervisor_qemu_descriptor(
    uint32_t id,
    const char *name,
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
    descriptor.restart_limit = 1U;
    descriptor.readiness_timeout_ms = 1000U;
    descriptor.cpu_ticks_limit = 0U;
    descriptor.user_memory_limit_pages = 0U;
    supervisor_qemu_copy_name(descriptor.name, name);
    descriptor.dependency_count = dependency_count;
    for (index = 0U; index < dependency_count; ++index) {
        descriptor.dependencies[index] = dependencies[index];
    }
    return descriptor;
}

static void supervisor_qemu_service_a_entry(void) {
}

static void supervisor_qemu_service_b_entry(void) {
}

static void supervisor_qemu_service_c_entry(void) {
}

static void supervisor_qemu_verify_snapshot(void)
{
    struct service_supervisor_snapshot snapshot;
    uint32_t index;

    supervisor_qemu_require(
        service_supervisor_snapshot(&supervisor_qemu_state, &snapshot) ==
            SERVICE_SUPERVISOR_OK &&
            service_supervisor_snapshot_published(&snapshot),
        "service snapshot publication");
    supervisor_qemu_require(
        snapshot.version == SERVICE_SUPERVISOR_SNAPSHOT_VERSION &&
        snapshot.active_count == SUPERVISOR_QEMU_SERVICE_COUNT &&
        snapshot.entry_count == SUPERVISOR_QEMU_SERVICE_COUNT,
        "service snapshot counts");
    for (index = 0U; index < SUPERVISOR_QEMU_SERVICE_COUNT; ++index) {
        const struct service_supervisor_snapshot_entry *const entry =
            service_supervisor_snapshot_entry(&snapshot, index);

        supervisor_qemu_require(
            entry != NULL &&
            entry->descriptor.id == index + 1U &&
            entry->state == SERVICE_SUPERVISOR_STATE_RUNNING &&
            entry->health_state == SERVICE_HEALTH_PENDING,
            "service snapshot sorted entries");
    }
}

static void supervisor_qemu_audit_proof(void)
{
    struct event_ring *ring = kernel_event_ring_get();
    struct audit_record record;
    struct audit_record consumed;
    uint64_t sequence = 0U;

    supervisor_qemu_require(ring != NULL &&
                                event_ring_is_published(ring),
                            "kernel event ring");
    supervisor_qemu_require(
        audit_record_init(&record, AUDIT_RECORD_PROCESS, 0x77U, 0U,
                          1U, 0U, 0U, UINT64_C(0x120)) == AUDIT_OK &&
            audit_record_append(ring, &record, &sequence) == AUDIT_OK &&
            sequence == 0U,
        "audit record append");
    supervisor_qemu_require(
        audit_record_pop(ring, &consumed) == AUDIT_OK &&
        consumed.sequence == 0U &&
        consumed.type == AUDIT_RECORD_PROCESS &&
        consumed.operation == 0x77U &&
        consumed.result == 0U &&
        consumed.pid == 1U &&
        consumed.domain == 0U &&
        consumed.generation == 0U &&
        consumed.detail == UINT64_C(0x120),
        "audit record exact consume");
    serial_write("[SUPERVISOR] audit record verified\n");
}

void service_supervisor_qemu_test_init(uint64_t hhdm_offset)
{
    struct service_descriptor descriptors[
        SUPERVISOR_QEMU_SERVICE_COUNT];
    struct service_supervisor_entry entries[
        SUPERVISOR_QEMU_SERVICE_COUNT];
    const uint8_t dependency_middle[1] = {1U};
    const uint8_t dependency_top[1] = {2U};
    bool published = false;
    bool timed_out = false;
    bool restarted = false;
    uint32_t exit_status = 0U;

    descriptors[0] = supervisor_qemu_descriptor(
        SUPERVISOR_QEMU_SERVICE_BASE_ID, "base", NULL, 0U);
    descriptors[1] = supervisor_qemu_descriptor(
        SUPERVISOR_QEMU_SERVICE_MIDDLE_ID, "middle",
        dependency_middle, 1U);
    descriptors[2] = supervisor_qemu_descriptor(
        SUPERVISOR_QEMU_SERVICE_TOP_ID, "top",
        dependency_top, 1U);

    supervisor_qemu_require(
        service_graph_snapshot_build(&supervisor_qemu_snapshot,
                                     descriptors,
                                     SUPERVISOR_QEMU_SERVICE_COUNT) ==
            SERVICE_GRAPH_OK,
        "service graph build");
    supervisor_qemu_require(
        service_graph_snapshot_start_id(&supervisor_qemu_snapshot, 0U) == 1U &&
        service_graph_snapshot_start_id(&supervisor_qemu_snapshot, 1U) == 2U &&
        service_graph_snapshot_start_id(&supervisor_qemu_snapshot, 2U) == 3U,
        "dependency start order");
    serial_write("[SUPERVISOR] dependency order 1 2 3\n");

    supervisor_qemu_require(
        service_supervisor_init(&supervisor_qemu_state,
                                hhdm_offset) ==
            SERVICE_SUPERVISOR_OK,
        "supervisor init");
    entries[0].service_id = 1U;
    entries[0].entry = supervisor_qemu_service_a_entry;
    entries[1].service_id = 2U;
    entries[1].entry = supervisor_qemu_service_b_entry;
    entries[2].service_id = 3U;
    entries[2].entry = supervisor_qemu_service_c_entry;
    supervisor_qemu_require(
        service_supervisor_start_ordered(
            &supervisor_qemu_state, &supervisor_qemu_snapshot,
            entries, SUPERVISOR_QEMU_SERVICE_COUNT) ==
            SERVICE_SUPERVISOR_OK,
        "ordered service start");
    supervisor_qemu_verify_snapshot();
    serial_write("[SUPERVISOR] composition init\n");

    supervisor_qemu_require(
        service_supervisor_set_readiness_deadline(
            &supervisor_qemu_state, 3U, 10U, 90U) ==
            SERVICE_SUPERVISOR_OK,
        "readiness deadline arm");

    supervisor_qemu_require(
        service_supervisor_report_ready(
            &supervisor_qemu_state, 1U, 10U, &published) ==
            SERVICE_SUPERVISOR_OK && published &&
        service_supervisor_report_ready(
            &supervisor_qemu_state, 2U, 20U, &published) ==
            SERVICE_SUPERVISOR_OK && published &&
        service_supervisor_health_state(
            &supervisor_qemu_state, 1U) == SERVICE_HEALTH_READY &&
        service_supervisor_health_state(
            &supervisor_qemu_state, 2U) == SERVICE_HEALTH_READY,
        "dependency readiness publication");
    serial_write("[SUPERVISOR] readiness published 1 2\n");

    supervisor_qemu_require(
        service_supervisor_check_readiness(
            &supervisor_qemu_state, 3U, 106U, &timed_out) ==
            SERVICE_SUPERVISOR_READINESS_TIMEOUT && timed_out &&
        service_supervisor_health_state(
            &supervisor_qemu_state, 3U) ==
            SERVICE_HEALTH_READINESS_TIMEOUT,
        "readiness timeout publication");
    serial_write("[SUPERVISOR] timeout published service=3\n");

    published = false;
    supervisor_qemu_require(
        service_supervisor_report_ready(
            &supervisor_qemu_state, 3U, 107U, &published) ==
            SERVICE_SUPERVISOR_READINESS_TIMEOUT && !published &&
        service_supervisor_health_state(
            &supervisor_qemu_state, 3U) ==
            SERVICE_HEALTH_READINESS_TIMEOUT,
        "late ready rejection");
    serial_write("[SUPERVISOR] late ready rejected\n");

    supervisor_qemu_require(
        service_supervisor_exit(
            &supervisor_qemu_state, 3U, 7U) ==
            SERVICE_SUPERVISOR_OK &&
        service_supervisor_reap_contained(
            &supervisor_qemu_state, 3U, 200U,
            &supervisor_qemu_snapshot, &exit_status,
            &restarted) ==
            SERVICE_SUPERVISOR_OK && restarted &&
        exit_status == 7U &&
        service_supervisor_health_state(
            &supervisor_qemu_state, 3U) == SERVICE_HEALTH_PENDING &&
        service_supervisor_health_state(
            &supervisor_qemu_state, 1U) == SERVICE_HEALTH_READY &&
        service_supervisor_health_state(
            &supervisor_qemu_state, 2U) == SERVICE_HEALTH_READY,
        "contained restart isolation");
    published = false;
    supervisor_qemu_require(
        service_supervisor_report_ready(
            &supervisor_qemu_state, 3U, 201U, &published) ==
            SERVICE_SUPERVISOR_OK && published,
        "restarted service readiness");
    serial_write("[SUPERVISOR] contained restart service=3 only\n");
    serial_write("[SUPERVISOR] restarted readiness service=3\n");

    supervisor_qemu_audit_proof();

    supervisor_qemu_require(
        service_supervisor_stop_ordered(
            &supervisor_qemu_state, &supervisor_qemu_snapshot) ==
            SERVICE_SUPERVISOR_OK &&
        supervisor_qemu_state.active_count == 0U,
        "ordered clean shutdown");
    serial_write("[SUPERVISOR] clean shutdown\n");

    supervisor_qemu_proof_initialized = true;
}

void service_supervisor_qemu_poll(void)
{
    struct keyboard_event event;

    if (!supervisor_qemu_proof_initialized || supervisor_qemu_proof_ok) {
        return;
    }
    if (keyboard_irq_count() == 0U) {
        return;
    }
    while (keyboard_pop_event(&event)) {
        if (event.type == KEYBOARD_EVENT_CHARACTER &&
            event.character == 'a') {
            serial_write("[SUPERVISOR] keyboard responsive\n");
            serial_write("[SUPERVISOR] proof OK\n");
            supervisor_qemu_proof_ok = true;
            return;
        }
    }
}

#else
typedef int service_supervisor_qemu_translation_unit_guard;
#endif
