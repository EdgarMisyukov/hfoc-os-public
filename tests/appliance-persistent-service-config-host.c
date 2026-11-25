#include "appliance_persistent_service_config.h"
#include "appliance_service_manager.h"
#include "block_device.h"
#include "config_journal.h"
#include "data_region.h"
#include "service.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifdef HFOC_APPLIANCE_PERSISTENT_SERVICE_CONFIG_HOST_TEST

#define TEST_BLOCK_SIZE 512U
#define TEST_BLOCK_COUNT 8U
#define TEST_CAPACITY (TEST_BLOCK_SIZE * TEST_BLOCK_COUNT)
#define TEST_REGION_OFFSET 1U
#define TEST_REGION_COUNT 7U

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

struct fixture_device {
    unsigned char storage[TEST_CAPACITY];
    bool fail_write;
    size_t read_calls;
    size_t write_calls;
};

static void zero_bytes(void *destination, size_t length)
{
    unsigned char *bytes = destination;
    size_t index;

    for (index = 0U; index < length; ++index) {
        bytes[index] = 0U;
    }
}

static void copy_bytes(void *destination,
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

static void store_le64(unsigned char *bytes, uint64_t value)
{
    size_t index;

    for (index = 0U; index < 8U; ++index) {
        bytes[index] = (unsigned char)(value >> (8U * index));
    }
}

static void init_fixture(struct fixture_device *fixture)
{
    static const unsigned char magic[DATA_REGION_MAGIC_SIZE] =
        DATA_REGION_MAGIC_BYTES;

    zero_bytes(fixture, sizeof(*fixture));
    copy_bytes(fixture->storage, magic, sizeof(magic));
    fixture->storage[8U] = DATA_REGION_FORMAT_VERSION;
    store_le64(fixture->storage + 16U, TEST_REGION_OFFSET);
    store_le64(fixture->storage + 24U, TEST_REGION_COUNT);
}

static enum block_device_status fixture_read(
    struct block_device *device,
    uint64_t offset,
    void *buffer,
    size_t length)
{
    struct fixture_device *fixture =
        (struct fixture_device *)device->context;

    ++fixture->read_calls;
    copy_bytes(buffer, fixture->storage + (size_t)offset, length);
    return BLOCK_DEVICE_OK;
}

static enum block_device_status fixture_write(
    struct block_device *device,
    uint64_t offset,
    const void *buffer,
    size_t length)
{
    struct fixture_device *fixture =
        (struct fixture_device *)device->context;

    ++fixture->write_calls;
    if (fixture->fail_write) {
        fixture->fail_write = false;
        return BLOCK_DEVICE_IO_ERROR;
    }
    copy_bytes(fixture->storage + (size_t)offset, buffer, length);
    return BLOCK_DEVICE_OK;
}

static struct block_device make_device(struct fixture_device *fixture)
{
    struct block_device device;

    zero_bytes(&device, sizeof(device));
    device.block_size = TEST_BLOCK_SIZE;
    device.block_count = TEST_BLOCK_COUNT;
    device.context = fixture;
    device.read_fn = fixture_read;
    device.write_fn = fixture_write;
    return device;
}

static struct service_descriptor make_descriptor(
    uint32_t id,
    const char *name,
    const uint32_t *dependency_ids,
    uint32_t dependency_count)
{
    struct service_descriptor descriptor;
    uint32_t index;

    zero_bytes(&descriptor, sizeof(descriptor));
    descriptor.id = id;
    descriptor.version = SERVICE_DESCRIPTOR_VERSION;
    descriptor.enabled = true;
    descriptor.restart_limit = 2U;
    descriptor.readiness_timeout_ms = 1000U;
    memcpy(descriptor.name, name, strlen(name) + 1U);
    descriptor.dependency_count = dependency_count;
    for (index = 0U; index < dependency_count; ++index) {
        descriptor.dependencies[index] =
            (uint8_t)dependency_ids[index];
    }
    return descriptor;
}

static struct service_descriptor make_baseline(void)
{
    return make_descriptor(1U, "base", NULL, 0U);
}

static struct service_descriptor make_middle(void)
{
    static const uint32_t dependency_ids[1] = {1U};

    return make_descriptor(
        2U, "middle", dependency_ids, 1U);
}

static struct service_descriptor make_top(void)
{
    static const uint32_t dependency_ids[1] = {2U};

    return make_descriptor(
        3U, "top", dependency_ids, 1U);
}

static void make_catalog(
    struct service_descriptor descriptors[3U])
{
    descriptors[0] = make_baseline();
    descriptors[1] = make_middle();
    descriptors[2] = make_top();
}

static void service_entry_void(void) {
}

static void make_entries(
    struct service_supervisor_entry entries[3U])
{
    entries[0].service_id = 1U;
    entries[0].entry = service_entry_void;
    entries[1].service_id = 2U;
    entries[1].entry = service_entry_void;
    entries[2].service_id = 3U;
    entries[2].entry = service_entry_void;
}

static bool descriptor_matches_config(
    const struct service_descriptor *descriptor,
    const struct appliance_persistent_service_config_values *values)
{
    return descriptor != NULL &&
           descriptor->enabled == values->enabled &&
           descriptor->restart_limit == values->restart_limit &&
           descriptor->readiness_timeout_ms ==
               values->readiness_timeout_ms;
}

static bool descriptor_equal(
    const struct service_descriptor *left,
    const struct service_descriptor *right)
{
    return left != NULL && right != NULL &&
           memcmp(left, right, sizeof(*left)) == 0;
}

static enum service_supervisor_status manager_start(
    void *context,
    const struct service_graph_snapshot *snapshot,
    const struct service_supervisor_entry *entries,
    uint32_t entry_count)
{
    (void)context;
    (void)snapshot;
    (void)entries;
    (void)entry_count;
    return SERVICE_SUPERVISOR_OK;
}

static enum service_supervisor_status manager_stop(
    void *context,
    const struct service_graph_snapshot *snapshot)
{
    (void)context;
    (void)snapshot;
    return SERVICE_SUPERVISOR_OK;
}

static enum service_supervisor_status manager_restart(
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

static enum service_supervisor_status manager_snapshot(
    void *context,
    struct service_supervisor_snapshot *snapshot)
{
    (void)context;
    (void)snapshot;
    return SERVICE_SUPERVISOR_OK;
}

static void make_manager(struct appliance_service_manager *manager)
{
    struct appliance_service_manager_ops ops;

    zero_bytes(manager, sizeof(*manager));
    zero_bytes(&ops, sizeof(ops));
    ops.start_ordered = manager_start;
    ops.stop_ordered = manager_stop;
    ops.restart_contained = manager_restart;
    ops.snapshot = manager_snapshot;
    CHECK(appliance_service_manager_init(
              manager, &ops,
              APPLIANCE_SERVICE_MANAGER_STARTUP_ROLLBACK) ==
              APPLIANCE_SERVICE_MANAGER_OK,
          "persistent service manager fixture init");
}

static enum appliance_persistent_service_config_status load_fresh(
    struct appliance_persistent_service_config *config,
    struct block_device *device)
{
    struct service_descriptor catalog[3U];

    make_catalog(catalog);
    zero_bytes(config, sizeof(*config));
    return appliance_persistent_service_config_load(
        config, device, catalog, 3U);
}

static void test_status_strings(void)
{
    static const enum appliance_persistent_service_config_status statuses[] = {
        APPLIANCE_PERSISTENT_SERVICE_CONFIG_OK,
        APPLIANCE_PERSISTENT_SERVICE_CONFIG_NULL,
        APPLIANCE_PERSISTENT_SERVICE_CONFIG_BAD_STATE,
        APPLIANCE_PERSISTENT_SERVICE_CONFIG_BAD_DEVICE,
        APPLIANCE_PERSISTENT_SERVICE_CONFIG_IO_ERROR,
        APPLIANCE_PERSISTENT_SERVICE_CONFIG_CORRUPT,
        APPLIANCE_PERSISTENT_SERVICE_CONFIG_FULL,
        APPLIANCE_PERSISTENT_SERVICE_CONFIG_BAD_RECORD,
        APPLIANCE_PERSISTENT_SERVICE_CONFIG_BAD_CONFIG,
        APPLIANCE_PERSISTENT_SERVICE_CONFIG_UNKNOWN_SERVICE,
        APPLIANCE_PERSISTENT_SERVICE_CONFIG_ENTRY_UNAVAILABLE,
        APPLIANCE_PERSISTENT_SERVICE_CONFIG_MANAGER_BAD_STATE,
    };
    static const char *expected[] = {
        "APPLIANCE_PERSISTENT_SERVICE_CONFIG_OK",
        "APPLIANCE_PERSISTENT_SERVICE_CONFIG_NULL",
        "APPLIANCE_PERSISTENT_SERVICE_CONFIG_BAD_STATE",
        "APPLIANCE_PERSISTENT_SERVICE_CONFIG_BAD_DEVICE",
        "APPLIANCE_PERSISTENT_SERVICE_CONFIG_IO_ERROR",
        "APPLIANCE_PERSISTENT_SERVICE_CONFIG_CORRUPT",
        "APPLIANCE_PERSISTENT_SERVICE_CONFIG_FULL",
        "APPLIANCE_PERSISTENT_SERVICE_CONFIG_BAD_RECORD",
        "APPLIANCE_PERSISTENT_SERVICE_CONFIG_BAD_CONFIG",
        "APPLIANCE_PERSISTENT_SERVICE_CONFIG_UNKNOWN_SERVICE",
        "APPLIANCE_PERSISTENT_SERVICE_CONFIG_ENTRY_UNAVAILABLE",
        "APPLIANCE_PERSISTENT_SERVICE_CONFIG_MANAGER_BAD_STATE",
    };
    static const enum appliance_persistent_service_config_source sources[] = {
        APPLIANCE_PERSISTENT_SERVICE_CONFIG_SOURCE_NONE,
        APPLIANCE_PERSISTENT_SERVICE_CONFIG_SOURCE_COMMITTED,
        APPLIANCE_PERSISTENT_SERVICE_CONFIG_SOURCE_DEFAULT_MISSING,
        APPLIANCE_PERSISTENT_SERVICE_CONFIG_SOURCE_DEFAULT_INVALID,
    };
    static const char *expected_sources[] = {
        "APPLIANCE_PERSISTENT_SERVICE_CONFIG_SOURCE_NONE",
        "APPLIANCE_PERSISTENT_SERVICE_CONFIG_SOURCE_COMMITTED",
        "APPLIANCE_PERSISTENT_SERVICE_CONFIG_SOURCE_DEFAULT_MISSING",
        "APPLIANCE_PERSISTENT_SERVICE_CONFIG_SOURCE_DEFAULT_INVALID",
    };
    size_t index;

    CHECK(sizeof(statuses) / sizeof(statuses[0]) ==
              sizeof(expected) / sizeof(expected[0]),
          "persistent service config status table is complete");
    for (index = 0U;
         index < sizeof(statuses) / sizeof(statuses[0]);
         ++index) {
        CHECK(strcmp(
                  appliance_persistent_service_config_status_string(
                      statuses[index]),
                  expected[index]) == 0,
              "persistent service config status string is exact");
    }
    CHECK(appliance_persistent_service_config_status_string(
              (enum appliance_persistent_service_config_status)12U) == NULL,
          "persistent service config rejects undefined status string");
    CHECK(sizeof(sources) / sizeof(sources[0]) ==
              sizeof(expected_sources) / sizeof(expected_sources[0]),
          "persistent service config source table is complete");
    for (index = 0U;
         index < sizeof(sources) / sizeof(sources[0]);
         ++index) {
        CHECK(strcmp(
                  appliance_persistent_service_config_source_string(
                      sources[index]),
                  expected_sources[index]) == 0,
              "persistent service config source string is exact");
    }
    CHECK(appliance_persistent_service_config_source_string(
              (enum appliance_persistent_service_config_source)4U) == NULL,
          "persistent service config rejects undefined source string");
}

static void test_load_default(void)
{
    struct fixture_device fixture;
    struct block_device device;
    struct appliance_persistent_service_config config;
    struct service_descriptor catalog[3U];

    init_fixture(&fixture);
    device = make_device(&fixture);
    make_catalog(catalog);
    zero_bytes(&config, sizeof(config));
    CHECK(appliance_persistent_service_config_load(
              &config, &device, catalog, 3U) ==
              APPLIANCE_PERSISTENT_SERVICE_CONFIG_OK &&
              appliance_persistent_service_config_source(&config) ==
                  APPLIANCE_PERSISTENT_SERVICE_CONFIG_SOURCE_DEFAULT_MISSING &&
              appliance_persistent_service_config_count(&config) == 3U &&
              descriptor_equal(
                  appliance_persistent_service_config_descriptor(&config, 0U),
                  &catalog[0]) &&
              descriptor_equal(
                  appliance_persistent_service_config_descriptor(&config, 1U),
                  &catalog[1]) &&
              descriptor_equal(
                  appliance_persistent_service_config_descriptor(&config, 2U),
                  &catalog[2]),
          "empty persistent service region publishes safe baseline");
}

static void test_null_and_bad_state(void)
{
    struct fixture_device fixture;
    struct block_device device;
    struct appliance_persistent_service_config config;
    struct service_descriptor catalog[3U];
    struct appliance_persistent_service_config_values values;

    init_fixture(&fixture);
    device = make_device(&fixture);
    make_catalog(catalog);
    zero_bytes(&values, sizeof(values));
    values.enabled = false;
    values.restart_limit = 2U;
    values.readiness_timeout_ms = 1000U;

    CHECK(appliance_persistent_service_config_load(
              NULL, &device, catalog, 3U) ==
              APPLIANCE_PERSISTENT_SERVICE_CONFIG_NULL,
          "persistent service config load rejects null config");
    CHECK(appliance_persistent_service_config_load(
              &config, NULL, catalog, 3U) ==
              APPLIANCE_PERSISTENT_SERVICE_CONFIG_NULL,
          "persistent service config load rejects null device");
    zero_bytes(&config, sizeof(config));
    CHECK(appliance_persistent_service_config_load(
              &config, &device, NULL, 3U) ==
              APPLIANCE_PERSISTENT_SERVICE_CONFIG_NULL,
          "persistent service config load rejects null baseline");
    CHECK(appliance_persistent_service_config_set_config(
              NULL, 1U, &values) ==
              APPLIANCE_PERSISTENT_SERVICE_CONFIG_NULL,
          "persistent service config set rejects null store");
    zero_bytes(&config, sizeof(config));
    CHECK(appliance_persistent_service_config_set_config(
              &config, 1U, &values) ==
              APPLIANCE_PERSISTENT_SERVICE_CONFIG_BAD_STATE,
          "persistent service config set rejects unloaded store");
    CHECK(load_fresh(&config, &device) ==
              APPLIANCE_PERSISTENT_SERVICE_CONFIG_OK,
          "persistent service config load publishes safe baseline");
    CHECK(appliance_persistent_service_config_load(
              &config, &device, catalog, 3U) ==
              APPLIANCE_PERSISTENT_SERVICE_CONFIG_BAD_STATE,
          "persistent service config rejects second load without clearing");
    CHECK(appliance_persistent_service_config_count(NULL) == 0U,
          "persistent service config count rejects null store");
    CHECK(appliance_persistent_service_config_descriptor(
              NULL, 0U) == NULL,
          "persistent service config descriptor rejects null store");
    CHECK(appliance_persistent_service_config_descriptor(
              &config, 4U) == NULL,
          "persistent service config descriptor rejects out-of-range");
    CHECK(appliance_persistent_service_config_enabled(
              NULL, 1U) == false,
          "persistent service config enabled rejects null store");
    CHECK(appliance_persistent_service_config_source(NULL) ==
              APPLIANCE_PERSISTENT_SERVICE_CONFIG_SOURCE_NONE,
          "persistent service config source rejects null store");
    CHECK(appliance_persistent_service_config_set_config(
              &config, 99U, &values) ==
              APPLIANCE_PERSISTENT_SERVICE_CONFIG_UNKNOWN_SERVICE,
          "persistent service config set rejects unknown service");
}

static void test_commit_and_reload(void)
{
    struct fixture_device fixture;
    struct block_device device;
    struct appliance_persistent_service_config config;
    struct appliance_persistent_service_config_values values;
    size_t write_calls;

    init_fixture(&fixture);
    device = make_device(&fixture);
    CHECK(load_fresh(&config, &device) ==
              APPLIANCE_PERSISTENT_SERVICE_CONFIG_OK,
          "persistent service config fixture load initializes");

    zero_bytes(&values, sizeof(values));
    values.enabled = false;
    values.restart_limit = 1U;
    values.readiness_timeout_ms = 500U;
    write_calls = fixture.write_calls;
    CHECK(appliance_persistent_service_config_set_config(
              &config, 3U, &values) ==
              APPLIANCE_PERSISTENT_SERVICE_CONFIG_OK &&
              fixture.write_calls == write_calls + 1U &&
              appliance_persistent_service_config_source(&config) ==
                  APPLIANCE_PERSISTENT_SERVICE_CONFIG_SOURCE_COMMITTED &&
              descriptor_matches_config(
                  appliance_persistent_service_config_descriptor(
                      &config, 2U),
                  &values),
          "persistent service config commit mutates store after journal write");

    zero_bytes(&config, sizeof(config));
    CHECK(load_fresh(&config, &device) ==
              APPLIANCE_PERSISTENT_SERVICE_CONFIG_OK &&
              appliance_persistent_service_config_source(&config) ==
                  APPLIANCE_PERSISTENT_SERVICE_CONFIG_SOURCE_COMMITTED &&
              descriptor_matches_config(
                  appliance_persistent_service_config_descriptor(
                      &config, 2U),
                  &values) &&
              appliance_persistent_service_config_enabled(&config, 1U) &&
              appliance_persistent_service_config_enabled(&config, 2U) &&
              !appliance_persistent_service_config_enabled(&config, 3U),
          "fresh load after commit restores multiple-service config");
}

static void test_unavailable_dependency_rejected(void)
{
    struct fixture_device fixture;
    struct block_device device;
    struct appliance_persistent_service_config config;
    struct appliance_persistent_service_config_values disabled;
    struct service_descriptor catalog[3U];
    size_t write_calls;

    init_fixture(&fixture);
    device = make_device(&fixture);
    make_catalog(catalog);
    CHECK(load_fresh(&config, &device) ==
              APPLIANCE_PERSISTENT_SERVICE_CONFIG_OK,
          "unavailable dependency fixture load initializes");
    zero_bytes(&disabled, sizeof(disabled));
    disabled.enabled = false;
    disabled.restart_limit = 2U;
    disabled.readiness_timeout_ms = 1000U;

    write_calls = fixture.write_calls;
    CHECK(appliance_persistent_service_config_set_config(
              &config, 1U, &disabled) ==
              APPLIANCE_PERSISTENT_SERVICE_CONFIG_BAD_CONFIG &&
              fixture.write_calls == write_calls &&
              appliance_persistent_service_config_enabled(&config, 1U) &&
              appliance_persistent_service_config_enabled(&config, 2U),
          "disabling required dependency is rejected without journal write");

    CHECK(appliance_persistent_service_config_set_config(
              &config, 3U, &disabled) ==
              APPLIANCE_PERSISTENT_SERVICE_CONFIG_OK,
          "disabling leaf service with no enabled dependent is accepted");
    CHECK(appliance_persistent_service_config_set_config(
              &config, 2U, &disabled) ==
              APPLIANCE_PERSISTENT_SERVICE_CONFIG_OK,
          "disabling service after its dependent is disabled is accepted");
    zero_bytes(&disabled, sizeof(disabled));
    disabled.enabled = true;
    disabled.restart_limit = 2U;
    disabled.readiness_timeout_ms = 1000U;
    write_calls = fixture.write_calls;
    CHECK(appliance_persistent_service_config_set_config(
              &config, 3U, &disabled) ==
              APPLIANCE_PERSISTENT_SERVICE_CONFIG_BAD_CONFIG &&
              fixture.write_calls == write_calls &&
              !appliance_persistent_service_config_enabled(&config, 3U),
          "enabling service with disabled dependency is rejected without write");
}

static void test_invalid_record_recovery(void)
{
    struct fixture_device fixture;
    struct block_device device;
    struct appliance_persistent_service_config config;
    static const char malformed_record[] =
        "v1:services=2\nv1:service.1=1,2,1000\nv1:service.2=1,2,1000\n";
    static const char unavailable_record[] =
        "v1:services=3\n"
        "v1:service.1=0,2,1000\n"
        "v1:service.2=1,2,1000\n"
        "v1:service.3=1,2,1000\n";
    struct service_descriptor catalog[3U];

    init_fixture(&fixture);
    device = make_device(&fixture);
    make_catalog(catalog);
    CHECK(load_fresh(&config, &device) ==
              APPLIANCE_PERSISTENT_SERVICE_CONFIG_OK,
          "invalid-record recovery fixture load initializes");
    CHECK(config_journal_append(
              &config.journal, malformed_record,
              sizeof(malformed_record) - 1U) == CONFIG_JOURNAL_OK,
          "malformed service record append");
    zero_bytes(&config, sizeof(config));
    CHECK(appliance_persistent_service_config_load(
              &config, &device, catalog, 3U) ==
              APPLIANCE_PERSISTENT_SERVICE_CONFIG_OK &&
              appliance_persistent_service_config_source(&config) ==
                  APPLIANCE_PERSISTENT_SERVICE_CONFIG_SOURCE_DEFAULT_INVALID &&
              appliance_persistent_service_config_enabled(&config, 1U) &&
              appliance_persistent_service_config_enabled(&config, 2U) &&
              appliance_persistent_service_config_enabled(&config, 3U),
          "malformed service record recovers to safe baseline");

    CHECK(load_fresh(&config, &device) ==
              APPLIANCE_PERSISTENT_SERVICE_CONFIG_OK,
          "unavailable record recovery fixture reload initializes");
    CHECK(config_journal_append(
              &config.journal, unavailable_record,
              sizeof(unavailable_record) - 1U) == CONFIG_JOURNAL_OK,
          "unavailable service record append");
    zero_bytes(&config, sizeof(config));
    CHECK(appliance_persistent_service_config_load(
              &config, &device, catalog, 3U) ==
              APPLIANCE_PERSISTENT_SERVICE_CONFIG_OK &&
              appliance_persistent_service_config_source(&config) ==
                  APPLIANCE_PERSISTENT_SERVICE_CONFIG_SOURCE_DEFAULT_INVALID &&
              appliance_persistent_service_config_enabled(&config, 1U) &&
              appliance_persistent_service_config_enabled(&config, 2U) &&
              appliance_persistent_service_config_enabled(&config, 3U),
          "unavailable dependency record recovers to safe baseline");
}

static void test_unknown_service_ignored(void)
{
    struct fixture_device fixture;
    struct block_device device;
    struct appliance_persistent_service_config config;
    static const char unknown_record[] =
        "v1:services=3\n"
        "v1:service.1=1,2,1000\n"
        "v1:service.2=1,2,1000\n"
        "v1:service.99=0,2,1000\n";
    struct service_descriptor catalog[3U];

    init_fixture(&fixture);
    device = make_device(&fixture);
    make_catalog(catalog);
    CHECK(load_fresh(&config, &device) ==
              APPLIANCE_PERSISTENT_SERVICE_CONFIG_OK,
          "unknown-service fixture load initializes");
    CHECK(config_journal_append(
              &config.journal, unknown_record,
              sizeof(unknown_record) - 1U) == CONFIG_JOURNAL_OK,
          "unknown-service record append");
    zero_bytes(&config, sizeof(config));
    CHECK(appliance_persistent_service_config_load(
              &config, &device, catalog, 3U) ==
              APPLIANCE_PERSISTENT_SERVICE_CONFIG_OK &&
              appliance_persistent_service_config_source(&config) ==
                  APPLIANCE_PERSISTENT_SERVICE_CONFIG_SOURCE_DEFAULT_INVALID &&
              appliance_persistent_service_config_enabled(&config, 1U) &&
              appliance_persistent_service_config_enabled(&config, 2U) &&
              appliance_persistent_service_config_enabled(&config, 3U),
          "unknown service record recovers to safe baseline");
}

static void test_apply_manager(void)
{
    struct fixture_device fixture;
    struct block_device device;
    struct appliance_persistent_service_config config;
    struct appliance_service_manager manager;
    struct appliance_persistent_service_config_values disabled;
    struct service_supervisor_entry entries[3U];

    init_fixture(&fixture);
    device = make_device(&fixture);
    make_entries(entries);
    CHECK(load_fresh(&config, &device) ==
              APPLIANCE_PERSISTENT_SERVICE_CONFIG_OK,
          "manager apply fixture load initializes");
    make_manager(&manager);
    CHECK(appliance_persistent_service_config_apply(
              &config, &manager, entries, 3U) ==
              APPLIANCE_PERSISTENT_SERVICE_CONFIG_OK &&
              manager.graph.published &&
              manager.entry_count == 3U,
          "default persistent service table applies to manager");

    zero_bytes(&disabled, sizeof(disabled));
    disabled.enabled = false;
    disabled.restart_limit = 1U;
    disabled.readiness_timeout_ms = 500U;
    CHECK(appliance_persistent_service_config_set_config(
              &config, 3U, &disabled) ==
              APPLIANCE_PERSISTENT_SERVICE_CONFIG_OK,
          "manager apply fixture commits disabled service");
    make_manager(&manager);
    CHECK(appliance_persistent_service_config_apply(
              &config, &manager, entries, 2U) ==
              APPLIANCE_PERSISTENT_SERVICE_CONFIG_OK &&
              manager.graph.published &&
              manager.entry_count == 2U,
          "persistent disabled service applies with reduced entry table");

    make_manager(&manager);
    CHECK(appliance_persistent_service_config_apply(
              &config, &manager, entries, 3U) ==
              APPLIANCE_PERSISTENT_SERVICE_CONFIG_ENTRY_UNAVAILABLE &&
              !manager.graph.published,
          "manager apply rejects extra entry for disabled service");

    zero_bytes(&manager, sizeof(manager));
    CHECK(appliance_persistent_service_config_apply(
              &config, &manager, entries, 2U) ==
              APPLIANCE_PERSISTENT_SERVICE_CONFIG_MANAGER_BAD_STATE,
          "manager apply rejects uninitialized manager");
}

static void test_failed_append_preserves_value(void)
{
    struct fixture_device fixture;
    struct block_device device;
    struct appliance_persistent_service_config config;
    struct appliance_persistent_service_config_values values;
    size_t write_calls;

    init_fixture(&fixture);
    device = make_device(&fixture);
    CHECK(load_fresh(&config, &device) ==
              APPLIANCE_PERSISTENT_SERVICE_CONFIG_OK,
          "failed-append fixture load initializes");
    fixture.fail_write = true;
    zero_bytes(&values, sizeof(values));
    values.enabled = false;
    values.restart_limit = 1U;
    values.readiness_timeout_ms = 500U;
    write_calls = fixture.write_calls;
    CHECK(appliance_persistent_service_config_set_config(
              &config, 3U, &values) ==
              APPLIANCE_PERSISTENT_SERVICE_CONFIG_IO_ERROR &&
              fixture.write_calls == write_calls + 1U &&
              appliance_persistent_service_config_enabled(&config, 3U),
          "failed journal append preserves prior table and source");
}

int main(void)
{
    test_status_strings();
    test_load_default();
    test_null_and_bad_state();
    test_commit_and_reload();
    test_unavailable_dependency_rejected();
    test_invalid_record_recovery();
    test_unknown_service_ignored();
    test_apply_manager();
    test_failed_append_preserves_value();
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "persistent service config performs no allocation");
    if (failures != 0) {
        fprintf(stderr,
                "Appliance persistent service config host tests: %d FAILURES\n",
                failures);
        return 1;
    }
    puts("Appliance persistent service config host tests: ALL OK");
    return 0;
}

#else
#error "appliance-persistent-service-config-host.c requires HFOC_APPLIANCE_PERSISTENT_SERVICE_CONFIG_HOST_TEST"
#endif
