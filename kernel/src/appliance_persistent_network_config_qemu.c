#ifdef HFOC_APPLIANCE_PERSISTENT_NETWORK_CONFIG_QEMU_TEST

#include "appliance_persistent_network_config.h"
#include "block_device.h"
#include "data_region.h"
#include "serial.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define APPLIANCE_PERSISTENT_NETWORK_CONFIG_QEMU_BLOCK_SIZE 512U
#define APPLIANCE_PERSISTENT_NETWORK_CONFIG_QEMU_BLOCK_COUNT 8U
#define APPLIANCE_PERSISTENT_NETWORK_CONFIG_QEMU_CAPACITY \
    (APPLIANCE_PERSISTENT_NETWORK_CONFIG_QEMU_BLOCK_SIZE * \
     APPLIANCE_PERSISTENT_NETWORK_CONFIG_QEMU_BLOCK_COUNT)
#define APPLIANCE_PERSISTENT_NETWORK_CONFIG_QEMU_REGION_OFFSET 1U
#define APPLIANCE_PERSISTENT_NETWORK_CONFIG_QEMU_REGION_COUNT 7U

static unsigned char persistent_network_config_qemu_storage[
    APPLIANCE_PERSISTENT_NETWORK_CONFIG_QEMU_CAPACITY];
static size_t persistent_network_config_qemu_write_calls;

static void persistent_network_config_qemu_zero_bytes(
    void *destination,
    size_t length)
{
    unsigned char *bytes = destination;
    size_t index;

    for (index = 0U; index < length; ++index) {
        bytes[index] = 0U;
    }
}

static void persistent_network_config_qemu_copy_bytes(
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

static bool persistent_network_config_qemu_equal_bytes(
    const void *left,
    const void *right,
    size_t length)
{
    const unsigned char *a = left;
    const unsigned char *b = right;
    size_t index;

    for (index = 0U; index < length; ++index) {
        if (a[index] != b[index]) {
            return false;
        }
    }
    return true;
}

static void persistent_network_config_qemu_store_le64(
    unsigned char *bytes,
    uint64_t value)
{
    size_t index;

    for (index = 0U; index < 8U; ++index) {
        bytes[index] = (unsigned char)(value >> (8U * index));
    }
}

static void persistent_network_config_qemu_failure(
    const char *message) __attribute__((noreturn));

static void persistent_network_config_qemu_failure(const char *message)
{
    {
        const uint64_t serial_flags = serial_sequence_begin();
        serial_write("[NETWORK-PERSISTENT] failed: ");
        serial_write(message);
        serial_write("\n");
        serial_sequence_end(serial_flags);
    }
    for (;;) {
        __asm__ volatile ("hlt" : : : "memory");
    }
}

static void persistent_network_config_qemu_require(
    bool condition,
    const char *message)
{
    if (!condition) {
        persistent_network_config_qemu_failure(message);
    }
}

static enum block_device_status persistent_network_config_qemu_read(
    struct block_device *device,
    uint64_t offset,
    void *buffer,
    size_t length)
{
    (void)device;
    persistent_network_config_qemu_copy_bytes(
        buffer,
        persistent_network_config_qemu_storage + (size_t)offset,
        length);
    return BLOCK_DEVICE_OK;
}

static enum block_device_status persistent_network_config_qemu_write(
    struct block_device *device,
    uint64_t offset,
    const void *buffer,
    size_t length)
{
    (void)device;
    ++persistent_network_config_qemu_write_calls;
    persistent_network_config_qemu_copy_bytes(
        persistent_network_config_qemu_storage + (size_t)offset,
        buffer,
        length);
    return BLOCK_DEVICE_OK;
}

static void persistent_network_config_qemu_install_data_region(void)
{
    static const unsigned char magic[8U] = { 'H', 'F', 'O', 'C',
                                              'D', 'A', 'T', 'A' };

    persistent_network_config_qemu_zero_bytes(
        persistent_network_config_qemu_storage,
        sizeof(persistent_network_config_qemu_storage));
    persistent_network_config_qemu_copy_bytes(
        persistent_network_config_qemu_storage, magic, sizeof(magic));
    persistent_network_config_qemu_storage[8U] = 1U;
    persistent_network_config_qemu_store_le64(
        persistent_network_config_qemu_storage + 16U,
        APPLIANCE_PERSISTENT_NETWORK_CONFIG_QEMU_REGION_OFFSET);
    persistent_network_config_qemu_store_le64(
        persistent_network_config_qemu_storage + 24U,
        APPLIANCE_PERSISTENT_NETWORK_CONFIG_QEMU_REGION_COUNT);
}

static bool persistent_network_config_qemu_dhcp_is(
    const struct appliance_persistent_network_config *config,
    const char *interface,
    size_t interface_length)
{
    const struct appliance_network_config *values =
        appliance_persistent_network_config_current(config);

    return values != NULL &&
           appliance_network_config_mode(values) ==
               APPLIANCE_NETWORK_CONFIG_MODE_DHCP &&
           values->interface_length == interface_length &&
           persistent_network_config_qemu_equal_bytes(
               values->interface, interface, interface_length);
}

static bool persistent_network_config_qemu_static_is(
    const struct appliance_persistent_network_config *config,
    const struct network_ipv4_config *expected)
{
    const struct appliance_network_config *values =
        appliance_persistent_network_config_current(config);
    const struct network_ipv4_config *actual;

    if (values == NULL) {
        return false;
    }
    actual = appliance_network_config_static_config(values);
    return actual != NULL &&
           persistent_network_config_qemu_equal_bytes(
               actual, expected, sizeof(*actual));
}

void appliance_persistent_network_config_qemu_test_init(void)
{
    struct block_device device;
    struct appliance_persistent_network_config config;
    struct network_ipv4_config static_config;
    struct network_ipv4_config valid_static_config;
    static const char malformed_record[] =
        "v1:network.interface=eth0\nv1:network.mode=WRONG\n";
    size_t write_calls;

    persistent_network_config_qemu_zero_bytes(
        &device, sizeof(device));
    persistent_network_config_qemu_zero_bytes(
        &config, sizeof(config));
    persistent_network_config_qemu_zero_bytes(
        &static_config, sizeof(static_config));
    persistent_network_config_qemu_zero_bytes(
        &valid_static_config, sizeof(valid_static_config));
    persistent_network_config_qemu_install_data_region();

    static_config.schema_version = NETWORK_IPV4_CONFIG_VERSION;
    static_config.address.octets[0] = 10U;
    static_config.address.octets[1] = 0U;
    static_config.address.octets[2] = 2U;
    static_config.address.octets[3] = 15U;
    static_config.prefix_length = 24U;
    static_config.gateway.octets[0] = 10U;
    static_config.gateway.octets[1] = 0U;
    static_config.gateway.octets[2] = 2U;
    static_config.gateway.octets[3] = 2U;
    static_config.dns.octets[0] = 10U;
    static_config.dns.octets[1] = 0U;
    static_config.dns.octets[2] = 2U;
    static_config.dns.octets[3] = 3U;
    valid_static_config = static_config;

    device.block_size =
        APPLIANCE_PERSISTENT_NETWORK_CONFIG_QEMU_BLOCK_SIZE;
    device.block_count =
        APPLIANCE_PERSISTENT_NETWORK_CONFIG_QEMU_BLOCK_COUNT;
    device.read_fn = persistent_network_config_qemu_read;
    device.write_fn = persistent_network_config_qemu_write;

    serial_write("[NETWORK-PERSISTENT] proof init\n");

    persistent_network_config_qemu_require(
        appliance_persistent_network_config_load(
            &config, &device) ==
            APPLIANCE_PERSISTENT_NETWORK_CONFIG_OK &&
            appliance_persistent_network_config_source(&config) ==
                APPLIANCE_PERSISTENT_NETWORK_CONFIG_SOURCE_DEFAULT_MISSING &&
            persistent_network_config_qemu_dhcp_is(
                &config,
                APPLIANCE_NETWORK_CONFIG_DEFAULT_INTERFACE,
                sizeof(APPLIANCE_NETWORK_CONFIG_DEFAULT_INTERFACE) - 1U),
        "empty data region publishes safe DHCP default");
    serial_write(
        "[NETWORK-PERSISTENT] default DHCP config loaded\n");

    persistent_network_config_qemu_require(
        appliance_persistent_network_config_set_dhcp(
            &config,
            APPLIANCE_NETWORK_CONFIG_DEFAULT_INTERFACE,
            sizeof(APPLIANCE_NETWORK_CONFIG_DEFAULT_INTERFACE) - 1U) ==
            APPLIANCE_PERSISTENT_NETWORK_CONFIG_OK &&
            appliance_persistent_network_config_source(&config) ==
                APPLIANCE_PERSISTENT_NETWORK_CONFIG_SOURCE_COMMITTED &&
            persistent_network_config_qemu_dhcp_is(
                &config,
                APPLIANCE_NETWORK_CONFIG_DEFAULT_INTERFACE,
                sizeof(APPLIANCE_NETWORK_CONFIG_DEFAULT_INTERFACE) - 1U),
        "DHCP config commit");
    serial_write("[NETWORK-PERSISTENT] DHCP config committed\n");

    persistent_network_config_qemu_zero_bytes(
        &config, sizeof(config));
    persistent_network_config_qemu_require(
        appliance_persistent_network_config_load(
            &config, &device) ==
            APPLIANCE_PERSISTENT_NETWORK_CONFIG_OK &&
            persistent_network_config_qemu_dhcp_is(
                &config,
                APPLIANCE_NETWORK_CONFIG_DEFAULT_INTERFACE,
                sizeof(APPLIANCE_NETWORK_CONFIG_DEFAULT_INTERFACE) - 1U),
        "fresh load after DHCP commit");
    serial_write(
        "[NETWORK-PERSISTENT] fresh DHCP config read persisted\n");

    persistent_network_config_qemu_require(
        appliance_persistent_network_config_set_static(
            &config,
            APPLIANCE_NETWORK_CONFIG_DEFAULT_INTERFACE,
            sizeof(APPLIANCE_NETWORK_CONFIG_DEFAULT_INTERFACE) - 1U,
            &static_config) ==
            APPLIANCE_PERSISTENT_NETWORK_CONFIG_OK &&
            persistent_network_config_qemu_static_is(
                &config, &static_config),
        "STATIC config commit");
    serial_write("[NETWORK-PERSISTENT] STATIC config committed\n");

    persistent_network_config_qemu_zero_bytes(
        &config, sizeof(config));
    persistent_network_config_qemu_require(
        appliance_persistent_network_config_load(
            &config, &device) ==
            APPLIANCE_PERSISTENT_NETWORK_CONFIG_OK &&
            persistent_network_config_qemu_static_is(
                &config, &static_config),
        "fresh load after STATIC commit");
    serial_write(
        "[NETWORK-PERSISTENT] fresh STATIC config read persisted\n");

    static_config.address.octets[0] = 192U;
    static_config.address.octets[1] = 168U;
    write_calls = persistent_network_config_qemu_write_calls;
    persistent_network_config_qemu_require(
        appliance_persistent_network_config_set_static(
            &config,
            APPLIANCE_NETWORK_CONFIG_DEFAULT_INTERFACE,
            sizeof(APPLIANCE_NETWORK_CONFIG_DEFAULT_INTERFACE) - 1U,
            &static_config) ==
            APPLIANCE_PERSISTENT_NETWORK_CONFIG_BAD_CONFIG &&
            persistent_network_config_qemu_write_calls == write_calls &&
            persistent_network_config_qemu_static_is(
                &config, &valid_static_config),
        "invalid STATIC state rejected without journal write");

    persistent_network_config_qemu_require(
        persistent_network_config_qemu_static_is(
            &config, &valid_static_config),
        "invalid STATIC rejection preserves prior config");
    serial_write(
        "[NETWORK-PERSISTENT] invalid STATIC config rejected\n");

    write_calls = persistent_network_config_qemu_write_calls;
    persistent_network_config_qemu_require(
        config_journal_append(
            &config.journal, malformed_record,
            sizeof(malformed_record) - 1U) == CONFIG_JOURNAL_OK &&
            persistent_network_config_qemu_write_calls ==
                write_calls + 1U,
        "malformed journal record append");

    persistent_network_config_qemu_zero_bytes(
        &config, sizeof(config));
    persistent_network_config_qemu_require(
        appliance_persistent_network_config_load(
            &config, &device) ==
            APPLIANCE_PERSISTENT_NETWORK_CONFIG_OK &&
            appliance_persistent_network_config_source(&config) ==
                APPLIANCE_PERSISTENT_NETWORK_CONFIG_SOURCE_DEFAULT_INVALID &&
            persistent_network_config_qemu_dhcp_is(
                &config,
                APPLIANCE_NETWORK_CONFIG_DEFAULT_INTERFACE,
                sizeof(APPLIANCE_NETWORK_CONFIG_DEFAULT_INTERFACE) - 1U),
        "schema-invalid record recovers to safe DHCP default");
    serial_write(
        "[NETWORK-PERSISTENT] invalid record recovered to default\n");

    write_calls = persistent_network_config_qemu_write_calls;
    persistent_network_config_qemu_require(
        appliance_persistent_network_config_set_dhcp(
            &config,
            APPLIANCE_NETWORK_CONFIG_DEFAULT_INTERFACE,
            sizeof(APPLIANCE_NETWORK_CONFIG_DEFAULT_INTERFACE) - 1U) ==
            APPLIANCE_PERSISTENT_NETWORK_CONFIG_OK &&
            persistent_network_config_qemu_write_calls ==
                write_calls + 1U &&
            persistent_network_config_qemu_dhcp_is(
                &config,
                APPLIANCE_NETWORK_CONFIG_DEFAULT_INTERFACE,
                sizeof(APPLIANCE_NETWORK_CONFIG_DEFAULT_INTERFACE) - 1U),
        "post-recovery DHCP default write");

    persistent_network_config_qemu_zero_bytes(
        &config, sizeof(config));
    persistent_network_config_qemu_require(
        appliance_persistent_network_config_load(
            &config, &device) ==
            APPLIANCE_PERSISTENT_NETWORK_CONFIG_OK &&
            persistent_network_config_qemu_dhcp_is(
                &config,
                APPLIANCE_NETWORK_CONFIG_DEFAULT_INTERFACE,
                sizeof(APPLIANCE_NETWORK_CONFIG_DEFAULT_INTERFACE) - 1U),
        "fresh load after recovery write");
    serial_write(
        "[NETWORK-PERSISTENT] post-recovery persistence proven\n");

    serial_write("[NETWORK-PERSISTENT] clean shutdown\n");
    serial_write("[NETWORK-PERSISTENT] proof OK\n");
}

#else
typedef int appliance_persistent_network_config_qemu_translation_unit_guard;
#endif
