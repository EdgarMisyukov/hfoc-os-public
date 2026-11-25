#include "appliance_persistent_network_config.h"
#include "block_device.h"
#include "config_journal.h"
#include "data_region.h"
#include "sha256.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifdef HFOC_APPLIANCE_PERSISTENT_NETWORK_CONFIG_HOST_TEST

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

static struct network_ipv4_config make_static_config(void)
{
    struct network_ipv4_config config;

    zero_bytes(&config, sizeof(config));
    config.schema_version = NETWORK_IPV4_CONFIG_VERSION;
    config.address.octets[0] = 10U;
    config.address.octets[1] = 0U;
    config.address.octets[2] = 2U;
    config.address.octets[3] = 15U;
    config.prefix_length = 24U;
    config.gateway.octets[0] = 10U;
    config.gateway.octets[1] = 0U;
    config.gateway.octets[2] = 2U;
    config.gateway.octets[3] = 2U;
    config.dns.octets[0] = 10U;
    config.dns.octets[1] = 0U;
    config.dns.octets[2] = 2U;
    config.dns.octets[3] = 3U;
    return config;
}

static enum appliance_persistent_network_config_status load_fresh(
    struct appliance_persistent_network_config *config,
    struct block_device *device)
{
    zero_bytes(config, sizeof(*config));
    return appliance_persistent_network_config_load(config, device);
}

static bool config_is_dhcp(
    const struct appliance_persistent_network_config *config,
    const char *interface,
    size_t interface_length)
{
    const struct appliance_network_config *values;
    enum appliance_network_config_mode mode;

    values = appliance_persistent_network_config_current(config);
    if (values == NULL) {
        return false;
    }
    mode = appliance_network_config_mode(values);
    return mode == APPLIANCE_NETWORK_CONFIG_MODE_DHCP &&
           values->interface_length == interface_length &&
           memcmp(values->interface, interface, interface_length) == 0;
}

static bool config_is_static(
    const struct appliance_persistent_network_config *config,
    const struct network_ipv4_config *expected)
{
    const struct appliance_network_config *values;
    const struct network_ipv4_config *actual;

    values = appliance_persistent_network_config_current(config);
    if (values == NULL) {
        return false;
    }
    actual = appliance_network_config_static_config(values);
    return actual != NULL &&
           memcmp(actual, expected, sizeof(*actual)) == 0;
}

static void test_status_strings(void)
{
    static const enum appliance_persistent_network_config_status statuses[] = {
        APPLIANCE_PERSISTENT_NETWORK_CONFIG_OK,
        APPLIANCE_PERSISTENT_NETWORK_CONFIG_NULL,
        APPLIANCE_PERSISTENT_NETWORK_CONFIG_BAD_STATE,
        APPLIANCE_PERSISTENT_NETWORK_CONFIG_BAD_DEVICE,
        APPLIANCE_PERSISTENT_NETWORK_CONFIG_IO_ERROR,
        APPLIANCE_PERSISTENT_NETWORK_CONFIG_CORRUPT,
        APPLIANCE_PERSISTENT_NETWORK_CONFIG_FULL,
        APPLIANCE_PERSISTENT_NETWORK_CONFIG_BAD_RECORD,
        APPLIANCE_PERSISTENT_NETWORK_CONFIG_BAD_CONFIG,
        APPLIANCE_PERSISTENT_NETWORK_CONFIG_BUFFER_TOO_SMALL,
    };
    static const char *expected[] = {
        "APPLIANCE_PERSISTENT_NETWORK_CONFIG_OK",
        "APPLIANCE_PERSISTENT_NETWORK_CONFIG_NULL",
        "APPLIANCE_PERSISTENT_NETWORK_CONFIG_BAD_STATE",
        "APPLIANCE_PERSISTENT_NETWORK_CONFIG_BAD_DEVICE",
        "APPLIANCE_PERSISTENT_NETWORK_CONFIG_IO_ERROR",
        "APPLIANCE_PERSISTENT_NETWORK_CONFIG_CORRUPT",
        "APPLIANCE_PERSISTENT_NETWORK_CONFIG_FULL",
        "APPLIANCE_PERSISTENT_NETWORK_CONFIG_BAD_RECORD",
        "APPLIANCE_PERSISTENT_NETWORK_CONFIG_BAD_CONFIG",
        "APPLIANCE_PERSISTENT_NETWORK_CONFIG_BUFFER_TOO_SMALL",
    };
    static const enum appliance_persistent_network_config_source sources[] = {
        APPLIANCE_PERSISTENT_NETWORK_CONFIG_SOURCE_NONE,
        APPLIANCE_PERSISTENT_NETWORK_CONFIG_SOURCE_COMMITTED,
        APPLIANCE_PERSISTENT_NETWORK_CONFIG_SOURCE_DEFAULT_MISSING,
        APPLIANCE_PERSISTENT_NETWORK_CONFIG_SOURCE_DEFAULT_INVALID,
    };
    static const char *expected_sources[] = {
        "APPLIANCE_PERSISTENT_NETWORK_CONFIG_SOURCE_NONE",
        "APPLIANCE_PERSISTENT_NETWORK_CONFIG_SOURCE_COMMITTED",
        "APPLIANCE_PERSISTENT_NETWORK_CONFIG_SOURCE_DEFAULT_MISSING",
        "APPLIANCE_PERSISTENT_NETWORK_CONFIG_SOURCE_DEFAULT_INVALID",
    };
    size_t index;

    CHECK(sizeof(statuses) / sizeof(statuses[0]) ==
              sizeof(expected) / sizeof(expected[0]),
          "persistent network config status table is complete");
    for (index = 0U;
         index < sizeof(statuses) / sizeof(statuses[0]);
         ++index) {
        CHECK(strcmp(appliance_persistent_network_config_status_string(
                         statuses[index]),
                     expected[index]) == 0,
              "persistent network config status string is exact");
    }
    CHECK(appliance_persistent_network_config_status_string(
              (enum appliance_persistent_network_config_status)10U) == NULL,
          "persistent network config rejects undefined status string");
    CHECK(sizeof(sources) / sizeof(sources[0]) ==
              sizeof(expected_sources) / sizeof(expected_sources[0]),
          "persistent network config source table is complete");
    for (index = 0U;
         index < sizeof(sources) / sizeof(sources[0]);
         ++index) {
        CHECK(strcmp(appliance_persistent_network_config_source_string(
                         sources[index]),
                     expected_sources[index]) == 0,
              "persistent network config source string is exact");
    }
    CHECK(appliance_persistent_network_config_source_string(
              (enum appliance_persistent_network_config_source)4U) == NULL,
          "persistent network config rejects undefined source string");
}

static void test_load_default(void)
{
    struct fixture_device fixture;
    struct block_device device;
    struct appliance_persistent_network_config config;

    init_fixture(&fixture);
    device = make_device(&fixture);

    CHECK(load_fresh(&config, &device) ==
              APPLIANCE_PERSISTENT_NETWORK_CONFIG_OK &&
              appliance_persistent_network_config_source(&config) ==
                  APPLIANCE_PERSISTENT_NETWORK_CONFIG_SOURCE_DEFAULT_MISSING &&
              config_is_dhcp(
                  &config,
                  APPLIANCE_NETWORK_CONFIG_DEFAULT_INTERFACE,
                  sizeof(APPLIANCE_NETWORK_CONFIG_DEFAULT_INTERFACE) - 1U),
          "empty persistent network region publishes safe DHCP default");
    CHECK(load_fresh(&config, &device) ==
              APPLIANCE_PERSISTENT_NETWORK_CONFIG_OK &&
              config_is_dhcp(
                  &config,
                  APPLIANCE_NETWORK_CONFIG_DEFAULT_INTERFACE,
                  sizeof(APPLIANCE_NETWORK_CONFIG_DEFAULT_INTERFACE) - 1U),
          "repeated load from empty region remains deterministic");
}

static void test_null_and_bad_state(void)
{
    struct fixture_device fixture;
    struct block_device device;
    struct appliance_persistent_network_config config;
    struct network_ipv4_config static_config = make_static_config();

    init_fixture(&fixture);
    device = make_device(&fixture);

    CHECK(appliance_persistent_network_config_load(
              NULL, &device) == APPLIANCE_PERSISTENT_NETWORK_CONFIG_NULL,
          "persistent network config load rejects null config");
    CHECK(appliance_persistent_network_config_load(
              &config, NULL) == APPLIANCE_PERSISTENT_NETWORK_CONFIG_NULL,
          "persistent network config load rejects null device");
    CHECK(appliance_persistent_network_config_set_dhcp(
              NULL, "eth0", 4U) == APPLIANCE_PERSISTENT_NETWORK_CONFIG_NULL,
          "persistent network config DHCP-set rejects null config");
    zero_bytes(&config, sizeof(config));
    CHECK(appliance_persistent_network_config_set_static(
              &config, "eth0", 4U, &static_config) ==
              APPLIANCE_PERSISTENT_NETWORK_CONFIG_BAD_STATE,
          "persistent network config static-set rejects unloaded store");

    CHECK(load_fresh(&config, &device) ==
              APPLIANCE_PERSISTENT_NETWORK_CONFIG_OK,
          "persistent network config load publishes safe default");
    CHECK(appliance_persistent_network_config_load(
              &config, &device) == APPLIANCE_PERSISTENT_NETWORK_CONFIG_BAD_STATE,
          "persistent network config rejects second load without clearing");
    CHECK(appliance_persistent_network_config_render(
              &config, NULL, 0U, NULL) ==
              APPLIANCE_PERSISTENT_NETWORK_CONFIG_NULL,
          "persistent network config render rejects null arguments");
    CHECK(appliance_persistent_network_config_current(NULL) == NULL,
          "persistent network config current rejects null config");
    CHECK(appliance_persistent_network_config_source(NULL) ==
              APPLIANCE_PERSISTENT_NETWORK_CONFIG_SOURCE_NONE,
          "persistent network config source rejects null config");
}

static void test_dhcp_commit_and_reload(void)
{
    struct fixture_device fixture;
    struct block_device device;
    struct appliance_persistent_network_config config;
    size_t write_calls;

    init_fixture(&fixture);
    device = make_device(&fixture);
    CHECK(load_fresh(&config, &device) ==
              APPLIANCE_PERSISTENT_NETWORK_CONFIG_OK,
          "persistent network config DHCP fixture load initializes");

    write_calls = fixture.write_calls;
    CHECK(appliance_persistent_network_config_set_dhcp(
              &config, "eth0", 4U) == APPLIANCE_PERSISTENT_NETWORK_CONFIG_OK &&
              fixture.write_calls == write_calls + 1U &&
              appliance_persistent_network_config_source(&config) ==
                  APPLIANCE_PERSISTENT_NETWORK_CONFIG_SOURCE_COMMITTED &&
              config_is_dhcp(&config, "eth0", 4U),
          "persistent network config DHCP-set commits source and value");

    write_calls = fixture.write_calls;
    CHECK(appliance_persistent_network_config_set_dhcp(
              &config, "eth0", 4U) == APPLIANCE_PERSISTENT_NETWORK_CONFIG_OK &&
              fixture.write_calls == write_calls &&
              config_is_dhcp(&config, "eth0", 4U),
          "persistent network config same DHCP value is a no-op");

    CHECK(load_fresh(&config, &device) ==
              APPLIANCE_PERSISTENT_NETWORK_CONFIG_OK &&
              appliance_persistent_network_config_source(&config) ==
                  APPLIANCE_PERSISTENT_NETWORK_CONFIG_SOURCE_COMMITTED &&
              config_is_dhcp(&config, "eth0", 4U),
          "fresh persistent network load reads committed DHCP value");
}

static void test_long_interface_commit_and_reload(void)
{
    static const char interface[] = "eth-lan-abcdefg";
    struct fixture_device fixture;
    struct block_device device;
    struct appliance_persistent_network_config config;

    init_fixture(&fixture);
    device = make_device(&fixture);
    CHECK(load_fresh(&config, &device) ==
              APPLIANCE_PERSISTENT_NETWORK_CONFIG_OK,
          "persistent network config long-interface fixture load initializes");
    CHECK(appliance_persistent_network_config_set_dhcp(
              &config, interface, sizeof(interface) - 1U) ==
              APPLIANCE_PERSISTENT_NETWORK_CONFIG_OK &&
              appliance_persistent_network_config_source(&config) ==
                  APPLIANCE_PERSISTENT_NETWORK_CONFIG_SOURCE_COMMITTED &&
              config_is_dhcp(
                  &config, interface, sizeof(interface) - 1U),
          "persistent network config accepts full interface length");
    CHECK(load_fresh(&config, &device) ==
              APPLIANCE_PERSISTENT_NETWORK_CONFIG_OK &&
              appliance_persistent_network_config_source(&config) ==
                  APPLIANCE_PERSISTENT_NETWORK_CONFIG_SOURCE_COMMITTED &&
              config_is_dhcp(
                  &config, interface, sizeof(interface) - 1U),
          "persistent network reload reads full interface length");
}

static void test_static_commit_and_reload(void)
{
    struct fixture_device fixture;
    struct block_device device;
    struct appliance_persistent_network_config config;
    struct network_ipv4_config static_config = make_static_config();

    init_fixture(&fixture);
    device = make_device(&fixture);
    CHECK(load_fresh(&config, &device) ==
              APPLIANCE_PERSISTENT_NETWORK_CONFIG_OK,
          "persistent network config STATIC fixture load initializes");
    CHECK(appliance_persistent_network_config_set_static(
              &config, "eth0", 4U, &static_config) ==
              APPLIANCE_PERSISTENT_NETWORK_CONFIG_OK &&
              appliance_persistent_network_config_source(&config) ==
                  APPLIANCE_PERSISTENT_NETWORK_CONFIG_SOURCE_COMMITTED &&
              config_is_static(&config, &static_config),
          "persistent network config STATIC-set commits exact IPv4 state");
    CHECK(load_fresh(&config, &device) ==
              APPLIANCE_PERSISTENT_NETWORK_CONFIG_OK &&
              appliance_persistent_network_config_source(&config) ==
                  APPLIANCE_PERSISTENT_NETWORK_CONFIG_SOURCE_COMMITTED &&
              config_is_static(&config, &static_config),
          "fresh persistent network load reads committed STATIC value");
}

static void test_offline_commit_and_reload(void)
{
    struct fixture_device fixture;
    struct block_device device;
    struct appliance_persistent_network_config config;
    char rendered[APPLIANCE_NETWORK_CONFIG_OUTPUT_MAX + 64U];
    size_t rendered_length = 0U;

    init_fixture(&fixture);
    device = make_device(&fixture);
    CHECK(load_fresh(&config, &device) ==
              APPLIANCE_PERSISTENT_NETWORK_CONFIG_OK,
          "persistent network config OFFLINE fixture load initializes");
    CHECK(appliance_persistent_network_config_set_offline(
              &config, "eth0", 4U) ==
              APPLIANCE_PERSISTENT_NETWORK_CONFIG_OK &&
              appliance_persistent_network_config_source(&config) ==
                  APPLIANCE_PERSISTENT_NETWORK_CONFIG_SOURCE_COMMITTED &&
              appliance_network_config_mode(
                  &config.values) ==
                  APPLIANCE_NETWORK_CONFIG_MODE_OFFLINE,
          "persistent network config OFFLINE-set commits the mode");
    CHECK(appliance_persistent_network_config_render(
              &config, rendered, sizeof(rendered), &rendered_length) ==
              APPLIANCE_PERSISTENT_NETWORK_CONFIG_OK &&
              rendered_length > 0U &&
              strstr(rendered, "mode=OFFLINE") != NULL,
          "the offline network config renders its mode");
    CHECK(load_fresh(&config, &device) ==
              APPLIANCE_PERSISTENT_NETWORK_CONFIG_OK &&
              appliance_persistent_network_config_source(&config) ==
                  APPLIANCE_PERSISTENT_NETWORK_CONFIG_SOURCE_COMMITTED &&
              appliance_network_config_mode(&config.values) ==
                  APPLIANCE_NETWORK_CONFIG_MODE_OFFLINE,
          "fresh persistent network load reads committed OFFLINE value");
}

static void test_invalid_static_preserves_previous(void)
{
    struct fixture_device fixture;
    struct block_device device;
    struct appliance_persistent_network_config config;
    struct network_ipv4_config static_config = make_static_config();
    struct network_ipv4_config invalid = static_config;
    size_t write_calls;

    init_fixture(&fixture);
    device = make_device(&fixture);
    CHECK(load_fresh(&config, &device) ==
              APPLIANCE_PERSISTENT_NETWORK_CONFIG_OK,
          "persistent network config invalid fixture load initializes");
    CHECK(appliance_persistent_network_config_set_static(
              &config, "eth0", 4U, &static_config) ==
              APPLIANCE_PERSISTENT_NETWORK_CONFIG_OK,
          "persistent network config invalid fixture sets static value");

    invalid.address.octets[0] = 192U;
    invalid.address.octets[1] = 168U;
    write_calls = fixture.write_calls;
    CHECK(appliance_persistent_network_config_set_static(
              &config, "eth0", 4U, &invalid) ==
              APPLIANCE_PERSISTENT_NETWORK_CONFIG_BAD_CONFIG &&
              fixture.write_calls == write_calls &&
              config_is_static(&config, &static_config),
          "invalid persistent STATIC value causes no write or mutation");
    CHECK(load_fresh(&config, &device) ==
              APPLIANCE_PERSISTENT_NETWORK_CONFIG_OK &&
              config_is_static(&config, &static_config),
          "fresh load after invalid STATIC preserves previous committed value");
}

static void test_failed_append_preserves_value(void)
{
    struct fixture_device fixture;
    struct block_device device;
    struct appliance_persistent_network_config config;
    size_t write_calls;

    init_fixture(&fixture);
    device = make_device(&fixture);
    CHECK(load_fresh(&config, &device) ==
              APPLIANCE_PERSISTENT_NETWORK_CONFIG_OK,
          "persistent network config failed-append fixture load initializes");
    CHECK(appliance_persistent_network_config_set_dhcp(
              &config, "lan0", 4U) == APPLIANCE_PERSISTENT_NETWORK_CONFIG_OK,
          "persistent network config failed-append fixture set succeeds");

    fixture.fail_write = true;
    write_calls = fixture.write_calls;
    CHECK(appliance_persistent_network_config_set_dhcp(
              &config, "eth0", 4U) ==
              APPLIANCE_PERSISTENT_NETWORK_CONFIG_IO_ERROR &&
              fixture.write_calls == write_calls + 1U &&
              config_is_dhcp(&config, "lan0", 4U),
          "persistent network failed append preserves previous value");
    CHECK(load_fresh(&config, &device) ==
              APPLIANCE_PERSISTENT_NETWORK_CONFIG_OK &&
              config_is_dhcp(&config, "lan0", 4U),
          "fresh load after failed append reads previous committed value");
}

static void test_invalid_record_recovery(void)
{
    struct fixture_device fixture;
    struct block_device device;
    struct appliance_persistent_network_config config;
    static const char invalid_record[] =
        "v1:network.interface=eth0\nv1:network.mode=WRONG\n";
    const size_t default_length =
        sizeof(APPLIANCE_NETWORK_CONFIG_DEFAULT_INTERFACE) - 1U;
    size_t write_calls;

    init_fixture(&fixture);
    device = make_device(&fixture);
    CHECK(load_fresh(&config, &device) ==
              APPLIANCE_PERSISTENT_NETWORK_CONFIG_OK,
          "persistent network config invalid-record fixture load initializes");
    write_calls = fixture.write_calls;
    CHECK(config_journal_append(
              &config.journal, invalid_record,
              sizeof(invalid_record) - 1U) == CONFIG_JOURNAL_OK &&
              fixture.write_calls == write_calls + 1U,
          "persistent network invalid-record fixture appends malformed value");

    CHECK(load_fresh(&config, &device) ==
              APPLIANCE_PERSISTENT_NETWORK_CONFIG_OK &&
              appliance_persistent_network_config_source(&config) ==
                  APPLIANCE_PERSISTENT_NETWORK_CONFIG_SOURCE_DEFAULT_INVALID &&
              config_is_dhcp(
                  &config,
                  APPLIANCE_NETWORK_CONFIG_DEFAULT_INTERFACE,
                  default_length),
          "invalid persistent network record recovers to safe DHCP default");
    CHECK(load_fresh(&config, &device) ==
              APPLIANCE_PERSISTENT_NETWORK_CONFIG_OK &&
              appliance_persistent_network_config_source(&config) ==
                  APPLIANCE_PERSISTENT_NETWORK_CONFIG_SOURCE_DEFAULT_INVALID &&
              config_is_dhcp(
                  &config,
                  APPLIANCE_NETWORK_CONFIG_DEFAULT_INTERFACE,
                  default_length),
          "invalid persistent network recovery is deterministic");

    write_calls = fixture.write_calls;
    CHECK(appliance_persistent_network_config_set_dhcp(
              &config,
              APPLIANCE_NETWORK_CONFIG_DEFAULT_INTERFACE,
              default_length) == APPLIANCE_PERSISTENT_NETWORK_CONFIG_OK &&
              fixture.write_calls == write_calls + 1U &&
              appliance_persistent_network_config_source(&config) ==
                  APPLIANCE_PERSISTENT_NETWORK_CONFIG_SOURCE_COMMITTED &&
              config_is_dhcp(
                  &config,
                  APPLIANCE_NETWORK_CONFIG_DEFAULT_INTERFACE,
                  default_length),
          "persistent network recovery write persists safe DHCP default");
    CHECK(load_fresh(&config, &device) ==
              APPLIANCE_PERSISTENT_NETWORK_CONFIG_OK &&
              appliance_persistent_network_config_source(&config) ==
                  APPLIANCE_PERSISTENT_NETWORK_CONFIG_SOURCE_COMMITTED &&
              config_is_dhcp(
                  &config,
                  APPLIANCE_NETWORK_CONFIG_DEFAULT_INTERFACE,
                  default_length),
          "fresh load after recovery reads committed safe DHCP default");
}

static void test_oversized_prefix_record_recovery(void)
{
    static const char invalid_record[] =
        "v1:network.interface=eth0\n"
        "v1:network.mode=STATIC\n"
        "v1:network.ipv4_address=10.0.2.15\n"
        "v1:network.ipv4_prefix=18446744073709551640\n"
        "v1:network.ipv4_gateway=10.0.2.2\n"
        "v1:network.ipv4_dns=10.0.2.3\n";
    const size_t default_length =
        sizeof(APPLIANCE_NETWORK_CONFIG_DEFAULT_INTERFACE) - 1U;
    struct fixture_device fixture;
    struct block_device device;
    struct appliance_persistent_network_config config;
    size_t write_calls;

    init_fixture(&fixture);
    device = make_device(&fixture);
    CHECK(load_fresh(&config, &device) ==
              APPLIANCE_PERSISTENT_NETWORK_CONFIG_OK,
          "persistent network config oversized-prefix fixture load initializes");
    write_calls = fixture.write_calls;
    CHECK(config_journal_append(
              &config.journal, invalid_record,
              sizeof(invalid_record) - 1U) == CONFIG_JOURNAL_OK &&
              fixture.write_calls == write_calls + 1U,
          "persistent network oversized-prefix fixture appends malformed value");

    CHECK(load_fresh(&config, &device) ==
              APPLIANCE_PERSISTENT_NETWORK_CONFIG_OK &&
              appliance_persistent_network_config_source(&config) ==
                  APPLIANCE_PERSISTENT_NETWORK_CONFIG_SOURCE_DEFAULT_INVALID &&
              config_is_dhcp(
                  &config,
                  APPLIANCE_NETWORK_CONFIG_DEFAULT_INTERFACE,
                  default_length),
          "oversized persistent network prefix recovers to safe DHCP default");
}

static void test_render_and_copy_bounds(void)
{
    struct fixture_device fixture;
    struct block_device device;
    struct appliance_persistent_network_config config;
    char output[APPLIANCE_NETWORK_CONFIG_OUTPUT_MAX + 1U];
    char small[1] = {'K'};
    size_t output_length = 0U;

    init_fixture(&fixture);
    device = make_device(&fixture);
    CHECK(load_fresh(&config, &device) ==
              APPLIANCE_PERSISTENT_NETWORK_CONFIG_OK,
          "persistent network config bounds fixture load initializes");
    CHECK(appliance_persistent_network_config_render(
              &config, small, sizeof(small), &output_length) ==
              APPLIANCE_PERSISTENT_NETWORK_CONFIG_BUFFER_TOO_SMALL &&
              small[0] == 'K' && output_length == 0U,
          "persistent network render rejects small buffer without mutation");
    CHECK(appliance_persistent_network_config_render(
              &config, output, sizeof(output), &output_length) ==
              APPLIANCE_PERSISTENT_NETWORK_CONFIG_OK &&
              strcmp(output,
                     "network v=1 interface=eth0 mode=DHCP\n") == 0 &&
              output_length == strlen(
                  "network v=1 interface=eth0 mode=DHCP\n"),
          "persistent network render exposes exact canonical DHCP record");
}

int main(void)
{
    test_status_strings();
    test_load_default();
    test_null_and_bad_state();
    test_dhcp_commit_and_reload();
    test_long_interface_commit_and_reload();
    test_static_commit_and_reload();
    test_offline_commit_and_reload();
    test_invalid_static_preserves_previous();
    test_failed_append_preserves_value();
    test_invalid_record_recovery();
    test_oversized_prefix_record_recovery();
    test_render_and_copy_bounds();
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "persistent network config performs no allocation");
    if (failures != 0) {
        fprintf(stderr,
                "Appliance persistent network config host tests: %d FAILURES\n",
                failures);
        return 1;
    }
    puts("Appliance persistent network config host tests: ALL OK");
    return 0;
}

#endif
