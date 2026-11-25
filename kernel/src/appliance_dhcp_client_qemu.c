#ifdef HFOC_APPLIANCE_NETWORK_CONFIG_QEMU_TEST

#include "appliance_dhcp_client.h"
#include "appliance_network_config.h"
#include "ipv4_config.h"
#include "serial.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

static void appliance_dhcp_qemu_zero_bytes(
    void *destination,
    size_t length)
{
    unsigned char *bytes = destination;
    size_t index;

    for (index = 0U; index < length; ++index) {
        bytes[index] = 0U;
    }
}

static bool appliance_dhcp_qemu_equal_bytes(
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

static void appliance_dhcp_qemu_failure(
    const char *message)
    __attribute__((noreturn));

static void appliance_dhcp_qemu_failure(
    const char *message)
{
    {
        const uint64_t serial_flags = serial_sequence_begin();
        serial_write("[DHCP-CLIENT] failed: ");
        serial_write(message);
        serial_write("\n");
        serial_sequence_end(serial_flags);
    }
    for (;;) {
        __asm__ volatile ("hlt" : : : "memory");
    }
}

static void appliance_dhcp_qemu_require(
    bool condition,
    const char *message)
{
    if (!condition) {
        appliance_dhcp_qemu_failure(message);
    }
}

static struct network_ipv4_config appliance_dhcp_qemu_lease(void)
{
    struct network_ipv4_config lease;

    appliance_dhcp_qemu_zero_bytes(&lease, sizeof(lease));
    lease.schema_version = NETWORK_IPV4_CONFIG_VERSION;
    lease.address.octets[0] = 10U;
    lease.address.octets[2] = 2U;
    lease.address.octets[3] = 15U;
    lease.prefix_length = 24U;
    lease.gateway.octets[0] = 10U;
    lease.gateway.octets[2] = 2U;
    lease.gateway.octets[3] = 2U;
    lease.dns.octets[0] = 10U;
    lease.dns.octets[2] = 2U;
    lease.dns.octets[3] = 3U;
    return lease;
}

static bool appliance_dhcp_qemu_render_is(
    const struct appliance_dhcp_client *client,
    const char *expected)
{
    char output[APPLIANCE_DHCP_CLIENT_OUTPUT_MAX + 1U];
    size_t output_length = 0U;
    size_t expected_length = 0U;

    while (expected[expected_length] != '\0') {
        ++expected_length;
    }
    return appliance_dhcp_client_render(
               client, output, sizeof(output), &output_length) ==
               APPLIANCE_DHCP_CLIENT_OK &&
           output_length == expected_length &&
           appliance_dhcp_qemu_equal_bytes(
               output, expected, expected_length);
}

void appliance_dhcp_client_qemu_test_init(void)
{
    struct appliance_network_config desired;
    struct appliance_dhcp_client client;
    struct network_ipv4_config lease = appliance_dhcp_qemu_lease();

    serial_write("[DHCP-CLIENT] proof init\n");
    appliance_dhcp_qemu_zero_bytes(&desired, sizeof(desired));
    appliance_dhcp_qemu_require(
        appliance_network_config_publish_dhcp(
            &desired, "eth0", 4U) == APPLIANCE_NETWORK_CONFIG_OK &&
        appliance_dhcp_client_init(&client, &desired) ==
            APPLIANCE_DHCP_CLIENT_OK &&
        appliance_dhcp_client_start(&client) ==
            APPLIANCE_DHCP_CLIENT_OK &&
        appliance_dhcp_client_offer(&client, &lease) ==
            APPLIANCE_DHCP_CLIENT_OK &&
        appliance_dhcp_client_ack(&client, &lease) ==
            APPLIANCE_DHCP_CLIENT_OK &&
        appliance_dhcp_client_state(&client) ==
            APPLIANCE_DHCP_CLIENT_STATE_BOUND &&
        appliance_dhcp_client_lease_published(&client) &&
        appliance_dhcp_qemu_render_is(
            &client,
            "dhcp_client v=1 interface=eth0 state=BOUND "
            "lease=published address=10.0.2.15 prefix=24 "
            "gateway=10.0.2.2 dns=10.0.2.3\n"),
        "DHCP discovery, offer, ack, and bound publication");
    serial_write("[DHCP-CLIENT] bound lease published\n");

    appliance_dhcp_qemu_require(
        appliance_dhcp_client_renew(&client) ==
            APPLIANCE_DHCP_CLIENT_OK &&
        appliance_dhcp_client_renew_timeout(&client) ==
            APPLIANCE_DHCP_CLIENT_OK &&
        appliance_dhcp_client_state(&client) ==
            APPLIANCE_DHCP_CLIENT_STATE_REBINDING &&
        appliance_dhcp_client_ack(&client, &lease) ==
            APPLIANCE_DHCP_CLIENT_OK &&
        appliance_dhcp_client_state(&client) ==
            APPLIANCE_DHCP_CLIENT_STATE_BOUND,
        "DHCP renew timeout and rebind acknowledgement");
    serial_write("[DHCP-CLIENT] renewal recovered\n");

    appliance_dhcp_qemu_require(
        appliance_dhcp_client_renew(&client) ==
            APPLIANCE_DHCP_CLIENT_OK &&
        appliance_dhcp_client_renew_timeout(&client) ==
            APPLIANCE_DHCP_CLIENT_OK &&
        appliance_dhcp_client_rebind_timeout(&client) ==
            APPLIANCE_DHCP_CLIENT_EXHAUSTED &&
        appliance_dhcp_client_state(&client) ==
            APPLIANCE_DHCP_CLIENT_STATE_FAILED &&
        !appliance_dhcp_client_lease_published(&client),
        "DHCP rebind timeout clears lease and enters fallback failure");
    serial_write("[DHCP-CLIENT] timeout fallback proven\n");

    appliance_dhcp_qemu_require(
        appliance_dhcp_client_reset(&client) ==
            APPLIANCE_DHCP_CLIENT_OK &&
        appliance_dhcp_client_init(&client, &desired) ==
            APPLIANCE_DHCP_CLIENT_OK &&
        appliance_dhcp_client_start(&client) ==
            APPLIANCE_DHCP_CLIENT_OK &&
        appliance_dhcp_client_offer(&client, &lease) ==
            APPLIANCE_DHCP_CLIENT_OK &&
        appliance_dhcp_client_ack(&client, &lease) ==
            APPLIANCE_DHCP_CLIENT_OK &&
        appliance_dhcp_client_link_down(&client) ==
            APPLIANCE_DHCP_CLIENT_OK &&
        appliance_dhcp_client_state(&client) ==
            APPLIANCE_DHCP_CLIENT_STATE_IDLE &&
        !appliance_dhcp_client_lease_published(&client),
        "DHCP link loss clears active lease");
    serial_write("[DHCP-CLIENT] clean shutdown\n");
    serial_write("[DHCP-CLIENT] proof OK\n");
}

#else
typedef int appliance_dhcp_client_qemu_translation_unit_guard;
#endif
