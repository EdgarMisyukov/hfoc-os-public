#ifdef HFOC_APPLIANCE_PERSISTENT_SERVICE_CONFIG_QEMU_TEST

#include "appliance_persistent_service_config.h"
#include "appliance_service_manager.h"
#include "block_device.h"
#include "config_journal.h"
#include "data_region.h"
#include "serial.h"
#include "service.h"
#include "service_supervisor.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define SVC_PERSIST_QEMU_BLOCK_SIZE 512U
#define SVC_PERSIST_QEMU_BLOCK_COUNT 8U
#define SVC_PERSIST_QEMU_CAPACITY \
    (SVC_PERSIST_QEMU_BLOCK_SIZE * SVC_PERSIST_QEMU_BLOCK_COUNT)
#define SVC_PERSIST_QEMU_REGION_OFFSET 1U
#define SVC_PERSIST_QEMU_REGION_COUNT 7U
#define SVC_PERSIST_QEMU_SERVICE_COUNT 3U

static unsigned char svc_persist_qemu_storage[SVC_PERSIST_QEMU_CAPACITY];
static struct service_supervisor svc_persist_qemu_supervisor_a;
static struct service_supervisor svc_persist_qemu_supervisor_b;
static struct appliance_service_manager svc_persist_qemu_manager_a;
static struct appliance_service_manager svc_persist_qemu_manager_b;
static struct appliance_persistent_service_config svc_persist_qemu_store;

static void svc_persist_qemu_zero_bytes(
    void *destination,
    size_t length)
{
    unsigned char *bytes = destination;
    size_t index;

    for (index = 0U; index < length; ++index) {
        bytes[index] = 0U;
    }
}

static void svc_persist_qemu_copy_bytes(
    void *destination,
    const void *source,
    size_t length)
{
    unsigned char *out = destination;
    const unsigned char *in = source;
    size_t index;

    for (index = 0U; index < length; ++index) {
        out[index] = in[index];
    }
}

static void svc_persist_qemu_store_le64(
    unsigned char *bytes,
    uint64_t value)
{
    size_t index;

    for (index = 0U; index < 8U; ++index) {
        bytes[index] = (unsigned char)(value >> (8U * index));
    }
}

static void svc_persist_qemu_failure(
    const char *message) __attribute__((noreturn));

static void svc_persist_qemu_failure(const char *message)
{
    {
        const uint64_t serial_flags = serial_sequence_begin();
        serial_write("[SVC-PERSIST] failed: ");
        serial_write(message);
        serial_write("\n");
        serial_sequence_end(serial_flags);
    }
    for (;;) {
        __asm__ volatile ("hlt" : : : "memory");
    }
}

static void svc_persist_qemu_require(bool condition, const char *message)
{
    if (!condition) {
        svc_persist_qemu_failure(message);
    }
}

static enum block_device_status svc_persist_qemu_read(
    struct block_device *device,
    uint64_t offset,
    void *buffer,
    size_t length)
{
    (void)device;
    svc_persist_qemu_copy_bytes(
        buffer, svc_persist_qemu_storage + (size_t)offset, length);
    return BLOCK_DEVICE_OK;
}

static enum block_device_status svc_persist_qemu_write(
    struct block_device *device,
    uint64_t offset,
    const void *buffer,
    size_t length)
{
    (void)device;
    svc_persist_qemu_copy_bytes(
        svc_persist_qemu_storage + (size_t)offset, buffer, length);
    return BLOCK_DEVICE_OK;
}

static void svc_persist_qemu_install_data_region(void)
{
    static const unsigned char magic[8U] = {
        'H', 'F', 'O', 'C', 'D', 'A', 'T', 'A'
    };

    svc_persist_qemu_zero_bytes(
        svc_persist_qemu_storage,
        sizeof(svc_persist_qemu_storage));
    svc_persist_qemu_copy_bytes(
        svc_persist_qemu_storage, magic, sizeof(magic));
    svc_persist_qemu_storage[8U] = DATA_REGION_FORMAT_VERSION;
    svc_persist_qemu_store_le64(
        svc_persist_qemu_storage + 16U,
        SVC_PERSIST_QEMU_REGION_OFFSET);
    svc_persist_qemu_store_le64(
        svc_persist_qemu_storage + 24U,
        SVC_PERSIST_QEMU_REGION_COUNT);
}

static void svc_persist_qemu_copy_name(
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

static struct service_descriptor svc_persist_qemu_descriptor(
    uint32_t id,
    const char *name,
    const uint8_t *dependency_ids,
    uint32_t dependency_count)
{
    struct service_descriptor descriptor;
    uint32_t index;

    svc_persist_qemu_zero_bytes(&descriptor, sizeof(descriptor));
    descriptor.id = id;
    descriptor.version = SERVICE_DESCRIPTOR_VERSION;
    descriptor.enabled = true;
    descriptor.restart_limit = 2U;
    descriptor.readiness_timeout_ms = 1000U;
    descriptor.cpu_ticks_limit = 0U;
    descriptor.user_memory_limit_pages = 0U;
    svc_persist_qemu_copy_name(descriptor.name, name);
    descriptor.dependency_count = dependency_count;
    for (index = 0U; index < dependency_count; ++index) {
        descriptor.dependencies[index] = dependency_ids[index];
    }
    return descriptor;
}

static void svc_persist_qemu_build_descriptors(
    struct service_descriptor descriptors[SVC_PERSIST_QEMU_SERVICE_COUNT])
{
    static const uint8_t middle_dependency[1] = {1U};
    static const uint8_t top_dependency[1] = {2U};

    descriptors[0] = svc_persist_qemu_descriptor(
        1U, "base", NULL, 0U);
    descriptors[1] = svc_persist_qemu_descriptor(
        2U, "middle", middle_dependency, 1U);
    descriptors[2] = svc_persist_qemu_descriptor(
        3U, "top", top_dependency, 1U);
}

static void svc_persist_qemu_entry_base(void) {
}

static void svc_persist_qemu_entry_middle(void) {
}

static void svc_persist_qemu_entry_top(void) {
}

static void svc_persist_qemu_build_entries(
    struct service_supervisor_entry entries[SVC_PERSIST_QEMU_SERVICE_COUNT])
{
    entries[0].service_id = 1U;
    entries[0].entry = svc_persist_qemu_entry_base;
    entries[1].service_id = 2U;
    entries[1].entry = svc_persist_qemu_entry_middle;
    entries[2].service_id = 3U;
    entries[2].entry = svc_persist_qemu_entry_top;
}

static enum service_supervisor_status svc_persist_qemu_start(
    void *context,
    const struct service_graph_snapshot *snapshot,
    const struct service_supervisor_entry *entries,
    uint32_t entry_count)
{
    return service_supervisor_start_ordered(
        (struct service_supervisor *)context,
        snapshot, entries, entry_count);
}

static enum service_supervisor_status svc_persist_qemu_stop(
    void *context,
    const struct service_graph_snapshot *snapshot)
{
    return service_supervisor_stop_ordered(
        (struct service_supervisor *)context, snapshot);
}

static enum service_supervisor_status svc_persist_qemu_restart(
    void *context,
    uint32_t service_id,
    uint64_t now_ticks,
    const struct service_graph_snapshot *snapshot,
    uint32_t *exit_status_out,
    bool *restarted)
{
    (void)context;
    (void)service_id;
    (void)now_ticks;
    (void)snapshot;
    (void)exit_status_out;
    (void)restarted;
    return SERVICE_SUPERVISOR_OK;
}

static enum service_supervisor_status svc_persist_qemu_snapshot(
    void *context,
    struct service_supervisor_snapshot *snapshot)
{
    return service_supervisor_snapshot(
        (const struct service_supervisor *)context, snapshot);
}

static void svc_persist_qemu_init_manager(
    struct service_supervisor *supervisor,
    struct appliance_service_manager *manager,
    uint64_t hhdm_offset)
{
    struct appliance_service_manager_ops ops;

    svc_persist_qemu_require(
        service_supervisor_init(supervisor, hhdm_offset) ==
            SERVICE_SUPERVISOR_OK,
        "persistent service supervisor init");
    svc_persist_qemu_zero_bytes(&ops, sizeof(ops));
    ops.start_ordered = svc_persist_qemu_start;
    ops.stop_ordered = svc_persist_qemu_stop;
    ops.restart_contained = svc_persist_qemu_restart;
    ops.snapshot = svc_persist_qemu_snapshot;
    ops.context = supervisor;
    svc_persist_qemu_require(
        appliance_service_manager_init(
            manager, &ops,
            APPLIANCE_SERVICE_MANAGER_STARTUP_ROLLBACK) ==
            APPLIANCE_SERVICE_MANAGER_OK,
        "persistent service manager init");
}

void appliance_persistent_service_config_qemu_test_init(
    uint64_t hhdm_offset)
{
    struct block_device device;
    struct service_descriptor descriptors[SVC_PERSIST_QEMU_SERVICE_COUNT];
    struct service_supervisor_entry entries[SVC_PERSIST_QEMU_SERVICE_COUNT];
    struct appliance_persistent_service_config_values disabled;

    svc_persist_qemu_zero_bytes(&device, sizeof(device));
    svc_persist_qemu_zero_bytes(&disabled, sizeof(disabled));
    svc_persist_qemu_install_data_region();
    svc_persist_qemu_build_descriptors(descriptors);
    svc_persist_qemu_build_entries(entries);
    device.block_size = SVC_PERSIST_QEMU_BLOCK_SIZE;
    device.block_count = SVC_PERSIST_QEMU_BLOCK_COUNT;
    device.read_fn = svc_persist_qemu_read;
    device.write_fn = svc_persist_qemu_write;

    serial_write("[SVC-PERSIST] proof init\n");

    svc_persist_qemu_zero_bytes(
        &svc_persist_qemu_store, sizeof(svc_persist_qemu_store));
    svc_persist_qemu_require(
        appliance_persistent_service_config_load(
            &svc_persist_qemu_store, &device,
            descriptors, SVC_PERSIST_QEMU_SERVICE_COUNT) ==
            APPLIANCE_PERSISTENT_SERVICE_CONFIG_OK &&
            appliance_persistent_service_config_source(
                &svc_persist_qemu_store) ==
                APPLIANCE_PERSISTENT_SERVICE_CONFIG_SOURCE_DEFAULT_MISSING &&
            appliance_persistent_service_config_enabled(
                &svc_persist_qemu_store, 1U) &&
            appliance_persistent_service_config_enabled(
                &svc_persist_qemu_store, 2U) &&
            appliance_persistent_service_config_enabled(
                &svc_persist_qemu_store, 3U),
        "empty persistent service store publishes safe default");
    svc_persist_qemu_init_manager(
        &svc_persist_qemu_supervisor_a,
        &svc_persist_qemu_manager_a,
        hhdm_offset);
    svc_persist_qemu_require(
        appliance_persistent_service_config_apply(
            &svc_persist_qemu_store,
            &svc_persist_qemu_manager_a,
            entries, SVC_PERSIST_QEMU_SERVICE_COUNT) ==
            APPLIANCE_PERSISTENT_SERVICE_CONFIG_OK &&
            svc_persist_qemu_manager_a.graph.published,
        "safe default persistent service table applies to manager");
    svc_persist_qemu_require(
        appliance_service_manager_start(
            &svc_persist_qemu_manager_a) ==
            APPLIANCE_SERVICE_MANAGER_OK &&
            svc_persist_qemu_supervisor_a.active_count ==
                SVC_PERSIST_QEMU_SERVICE_COUNT,
        "safe default manager startup");
    svc_persist_qemu_require(
        appliance_service_manager_stop(
            &svc_persist_qemu_manager_a) ==
            APPLIANCE_SERVICE_MANAGER_OK &&
            svc_persist_qemu_supervisor_a.active_count == 0U,
        "safe default manager clean shutdown");
    serial_write("[SVC-PERSIST] default apply and startup\n");

    disabled.enabled = false;
    disabled.restart_limit = 1U;
    disabled.readiness_timeout_ms = 500U;
    svc_persist_qemu_require(
        appliance_persistent_service_config_set_config(
            &svc_persist_qemu_store, 3U, &disabled) ==
            APPLIANCE_PERSISTENT_SERVICE_CONFIG_OK &&
            appliance_persistent_service_config_enabled(
                &svc_persist_qemu_store, 3U) == false,
        "persistent service disable commit");
    serial_write("[SVC-PERSIST] disabled service committed\n");

    svc_persist_qemu_zero_bytes(
        &svc_persist_qemu_store, sizeof(svc_persist_qemu_store));
    svc_persist_qemu_require(
        appliance_persistent_service_config_load(
            &svc_persist_qemu_store, &device,
            descriptors, SVC_PERSIST_QEMU_SERVICE_COUNT) ==
            APPLIANCE_PERSISTENT_SERVICE_CONFIG_OK &&
            appliance_persistent_service_config_source(
                &svc_persist_qemu_store) ==
                APPLIANCE_PERSISTENT_SERVICE_CONFIG_SOURCE_COMMITTED &&
            appliance_persistent_service_config_enabled(
                &svc_persist_qemu_store, 1U) &&
            appliance_persistent_service_config_enabled(
                &svc_persist_qemu_store, 2U) &&
            appliance_persistent_service_config_enabled(
                &svc_persist_qemu_store, 3U) == false,
        "fresh persistent service load restores disabled service");
    serial_write("[SVC-PERSIST] fresh load restores enablement\n");

    svc_persist_qemu_init_manager(
        &svc_persist_qemu_supervisor_b,
        &svc_persist_qemu_manager_b,
        hhdm_offset);
    svc_persist_qemu_require(
        appliance_persistent_service_config_apply(
            &svc_persist_qemu_store,
            &svc_persist_qemu_manager_b,
            entries, SVC_PERSIST_QEMU_SERVICE_COUNT - 1U) ==
            APPLIANCE_PERSISTENT_SERVICE_CONFIG_OK &&
            svc_persist_qemu_manager_b.graph.published &&
            svc_persist_qemu_manager_b.entry_count ==
                SVC_PERSIST_QEMU_SERVICE_COUNT - 1U,
        "restored persistent table applies with safe entry subset");
    svc_persist_qemu_require(
        appliance_service_manager_start(
            &svc_persist_qemu_manager_b) ==
            APPLIANCE_SERVICE_MANAGER_OK &&
            svc_persist_qemu_supervisor_b.active_count ==
                SVC_PERSIST_QEMU_SERVICE_COUNT - 1U,
        "persisted disabled service is not started");
    svc_persist_qemu_require(
        appliance_service_manager_stop(
            &svc_persist_qemu_manager_b) ==
            APPLIANCE_SERVICE_MANAGER_OK &&
            svc_persist_qemu_supervisor_b.active_count == 0U,
        "safe subset manager clean shutdown");
    serial_write("[SVC-PERSIST] restored startup with disabled service\n");

    serial_write("[SVC-PERSIST] clean shutdown\n");
    serial_write("[SVC-PERSIST] proof OK\n");
}

#else
typedef int appliance_persistent_service_config_qemu_translation_unit_guard;
#endif
