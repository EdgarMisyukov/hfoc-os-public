#ifdef HFOC_APPLIANCE_NETWORK_CONFIG_QEMU_TEST

#include "appliance_network_config.h"
#include "serial.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

static void appliance_network_config_qemu_zero_bytes(
    void *destination,
    size_t length)
{
    unsigned char *bytes = destination;
    size_t index;

    for (index = 0U; index < length; ++index) {
        bytes[index] = 0U;
    }
}

static bool appliance_network_config_qemu_equal_bytes(
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

static void appliance_network_config_qemu_failure(
    const char *message)
    __attribute__((noreturn));

static void appliance_network_config_qemu_failure(
    const char *message)
{
    {
        const uint64_t serial_flags = serial_sequence_begin();
        serial_write("[NET-CONFIG] failed: ");
        serial_write(message);
        serial_write("\n");
        serial_sequence_end(serial_flags);
    }
    for (;;) {
        __asm__ volatile ("hlt" : : : "memory");
    }
}

static void appliance_network_config_qemu_require(
    bool condition,
    const char *message)
{
    if (!condition) {
        appliance_network_config_qemu_failure(message);
    }
}

static void appliance_network_config_qemu_fill_static(
    struct network_ipv4_config *config)
{
    appliance_network_config_qemu_zero_bytes(
        config, sizeof(*config));
    config->schema_version = NETWORK_IPV4_CONFIG_VERSION;
    config->address.octets[0] = 10U;
    config->address.octets[2] = 2U;
    config->address.octets[3] = 15U;
    config->prefix_length = 24U;
    config->gateway.octets[0] = 10U;
    config->gateway.octets[2] = 2U;
    config->gateway.octets[3] = 2U;
    config->dns.octets[0] = 10U;
    config->dns.octets[2] = 2U;
    config->dns.octets[3] = 3U;
}

static bool appliance_network_config_qemu_render_is(
    const struct appliance_network_config *config,
    const char *expected)
{
    char output[APPLIANCE_NETWORK_CONFIG_OUTPUT_MAX + 1U];
    size_t output_length = 0U;
    size_t expected_length = 0U;

    while (expected[expected_length] != '\0') {
        ++expected_length;
    }
    return appliance_network_config_render(
               config, output, sizeof(output), &output_length) ==
               APPLIANCE_NETWORK_CONFIG_OK &&
           output_length == expected_length &&
           appliance_network_config_qemu_equal_bytes(
               output, expected, expected_length);
}

void appliance_network_config_qemu_test_init(void)
{
    struct appliance_network_config config;
    struct appliance_network_config unchanged;
    struct network_ipv4_config static_config;
    struct network_ipv4_config invalid;

    serial_write("[NET-CONFIG] proof init\n");

    appliance_network_config_qemu_zero_bytes(
        &config, sizeof(config));
    appliance_network_config_qemu_require(
        appliance_network_config_publish_default(&config) ==
            APPLIANCE_NETWORK_CONFIG_OK &&
        appliance_network_config_validate(&config) ==
            APPLIANCE_NETWORK_CONFIG_OK &&
        appliance_network_config_mode(&config) ==
            APPLIANCE_NETWORK_CONFIG_MODE_DHCP &&
        appliance_network_config_qemu_render_is(
            &config, "network v=1 interface=eth0 mode=DHCP\n"),
        "safe DHCP default");
    serial_write("[NET-CONFIG] safe default published\n");

    appliance_network_config_qemu_fill_static(&static_config);
    appliance_network_config_qemu_require(
        appliance_network_config_publish_static(
            &config, "lan0", 4U, &static_config) ==
            APPLIANCE_NETWORK_CONFIG_OK &&
        appliance_network_config_validate(&config) ==
            APPLIANCE_NETWORK_CONFIG_OK &&
        appliance_network_config_mode(&config) ==
            APPLIANCE_NETWORK_CONFIG_MODE_STATIC &&
        appliance_network_config_qemu_render_is(
            &config,
            "network v=1 interface=lan0 mode=STATIC "
            "ipv4_address=10.0.2.15 ipv4_prefix=24 "
            "ipv4_gateway=10.0.2.2 ipv4_dns=10.0.2.3\n"),
        "static network config publication");
    serial_write("[NET-CONFIG] static config committed\n");

    appliance_network_config_qemu_zero_bytes(
        &unchanged, sizeof(unchanged));
    appliance_network_config_qemu_require(
        appliance_network_config_publish_default(
            &unchanged) == APPLIANCE_NETWORK_CONFIG_OK,
        "invalid static fixture default");
    invalid = static_config;
    invalid.address.octets[0] = 192U;
    invalid.address.octets[1] = 168U;
    appliance_network_config_qemu_require(
        appliance_network_config_publish_static(
            &unchanged, "lan0", 4U, &invalid) ==
            APPLIANCE_NETWORK_CONFIG_INVALID_STATIC_CONFIG &&
        appliance_network_config_mode(&unchanged) ==
            APPLIANCE_NETWORK_CONFIG_MODE_DHCP &&
        appliance_network_config_qemu_render_is(
            &unchanged, "network v=1 interface=eth0 mode=DHCP\n"),
        "invalid static publication preserves default");
    serial_write("[NET-CONFIG] invalid static rejected\n");

    serial_write("[NET-CONFIG] clean shutdown\n");
    serial_write("[NET-CONFIG] proof OK\n");
}

#else
typedef int appliance_network_config_qemu_translation_unit_guard;
#endif
