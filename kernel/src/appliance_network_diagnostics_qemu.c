#ifdef HFOC_APPLIANCE_NETWORK_CONFIG_QEMU_TEST

#include "appliance_network_config.h"
#include "appliance_network_diagnostics.h"
#include "ipv4_config.h"
#include "link_state.h"
#include "serial.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

static void network_diagnostics_qemu_zero_bytes(
    void *destination,
    size_t length)
{
    unsigned char *bytes = destination;
    size_t index;

    for (index = 0U; index < length; ++index) {
        bytes[index] = 0U;
    }
}

static bool network_diagnostics_qemu_equal_bytes(
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

static void network_diagnostics_qemu_fill_static(
    struct network_ipv4_config *config)
{
    network_diagnostics_qemu_zero_bytes(config, sizeof(*config));
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

static void network_diagnostics_qemu_failure(
    const char *message)
    __attribute__((noreturn));

static void network_diagnostics_qemu_failure(const char *message)
{
    {
        const uint64_t serial_flags = serial_sequence_begin();
        serial_write("[NET-DIAG] failed: ");
        serial_write(message);
        serial_write("\n");
        serial_sequence_end(serial_flags);
    }
    for (;;) {
        __asm__ volatile ("hlt" : : : "memory");
    }
}

static void network_diagnostics_qemu_require(
    bool condition,
    const char *message)
{
    if (!condition) {
        network_diagnostics_qemu_failure(message);
    }
}

void appliance_network_diagnostics_qemu_test_init(void)
{
    struct appliance_network_config dhcp_desired;
    struct appliance_network_config static_desired;
    struct network_ipv4_config active;
    struct network_ipv4_config changed;
    struct network_link_state_record link;
    struct appliance_network_diagnostics diagnostics;
    char output[APPLIANCE_NETWORK_DIAGNOSTICS_OUTPUT_MAX + 1U];
    size_t output_length;

    network_diagnostics_qemu_zero_bytes(
        &dhcp_desired, sizeof(dhcp_desired));
    network_diagnostics_qemu_zero_bytes(
        &static_desired, sizeof(static_desired));
    network_diagnostics_qemu_zero_bytes(
        &diagnostics, sizeof(diagnostics));
    network_diagnostics_qemu_zero_bytes(&link, sizeof(link));
    network_diagnostics_qemu_fill_static(&active);
    changed = active;

    network_diagnostics_qemu_require(
        appliance_network_config_publish_dhcp(
            &dhcp_desired, "eth0", 4U) ==
            APPLIANCE_NETWORK_CONFIG_OK,
        "DHCP desired fixture");
    network_diagnostics_qemu_require(
        appliance_network_config_publish_static(
            &static_desired, "eth0", 4U, &active) ==
            APPLIANCE_NETWORK_CONFIG_OK,
        "STATIC desired fixture");

    link.version = NETWORK_LINK_STATE_VERSION;
    link.device_id = 1U;
    link.state = NETWORK_LINK_UP;
    link.generation = 7U;

    serial_write("[NET-DIAG] proof init\n");

    network_diagnostics_qemu_require(
        appliance_network_diagnostics_publish(
            &diagnostics, &dhcp_desired, &link, &active,
            APPLIANCE_NETWORK_REACHABILITY_REACHABLE,
            APPLIANCE_NETWORK_REACHABILITY_REACHABLE) ==
            APPLIANCE_NETWORK_DIAGNOSTICS_OK &&
        diagnostics.result == HEALTH_CHECK_RESULT_PASS &&
        diagnostics.fault == APPLIANCE_NETWORK_DIAGNOSTICS_FAULT_NONE,
        "DHCP diagnostics PASS");
    serial_write("[NET-DIAG] DHCP healthy diagnostics published\n");

    network_diagnostics_qemu_require(
        appliance_network_diagnostics_publish(
            &diagnostics, &dhcp_desired, &link, NULL,
            APPLIANCE_NETWORK_REACHABILITY_UNKNOWN,
            APPLIANCE_NETWORK_REACHABILITY_UNKNOWN) ==
            APPLIANCE_NETWORK_DIAGNOSTICS_OK &&
        diagnostics.result == HEALTH_CHECK_RESULT_WARN &&
        diagnostics.fault ==
            APPLIANCE_NETWORK_DIAGNOSTICS_FAULT_ADDRESS,
        "DHCP diagnostics WARN without lease");
    serial_write("[NET-DIAG] DHCP pending lease warned\n");

    output_length = 0U;
    network_diagnostics_qemu_require(
        appliance_network_diagnostics_publish(
            &diagnostics, &static_desired, &link, &active,
            APPLIANCE_NETWORK_REACHABILITY_REACHABLE,
            APPLIANCE_NETWORK_REACHABILITY_REACHABLE) ==
            APPLIANCE_NETWORK_DIAGNOSTICS_OK &&
        appliance_network_diagnostics_render(
            &diagnostics, output, sizeof(output), &output_length) ==
            APPLIANCE_NETWORK_DIAGNOSTICS_OK &&
        output_length == 200U &&
        network_diagnostics_qemu_equal_bytes(
            output,
            "network_diag v=1 interface=eth0 mode=STATIC "
            "device_id=1 link=UP active=yes address=10.0.2.15 "
            "prefix=24 gateway=10.0.2.2 dns=10.0.2.3 "
            "gateway_reach=REACHABLE dns_reach=REACHABLE "
            "result=PASS fault=NONE\n",
            200U),
        "STATIC diagnostics canonical render");
    serial_write("[NET-DIAG] STATIC diagnostics rendered\n");

    changed.address.octets[3] = 20U;
    network_diagnostics_qemu_require(
        appliance_network_diagnostics_publish(
            &diagnostics, &static_desired, &link, &changed,
            APPLIANCE_NETWORK_REACHABILITY_REACHABLE,
            APPLIANCE_NETWORK_REACHABILITY_REACHABLE) ==
            APPLIANCE_NETWORK_DIAGNOSTICS_OK &&
        diagnostics.result == HEALTH_CHECK_RESULT_FAIL &&
        diagnostics.fault ==
            APPLIANCE_NETWORK_DIAGNOSTICS_FAULT_CONFIG,
        "STATIC diagnostics config mismatch");
    serial_write("[NET-DIAG] STATIC mismatch failed\n");

    link.state = NETWORK_LINK_DOWN;
    network_diagnostics_qemu_require(
        appliance_network_diagnostics_publish(
            &diagnostics, &static_desired, &link, &active,
            APPLIANCE_NETWORK_REACHABILITY_REACHABLE,
            APPLIANCE_NETWORK_REACHABILITY_REACHABLE) ==
            APPLIANCE_NETWORK_DIAGNOSTICS_OK &&
        diagnostics.result == HEALTH_CHECK_RESULT_FAIL &&
        diagnostics.fault ==
            APPLIANCE_NETWORK_DIAGNOSTICS_FAULT_LINK,
        "link failure diagnostics");
    serial_write("[NET-DIAG] link failure published\n");

    serial_write("[NET-DIAG] clean shutdown\n");
    serial_write("[NET-DIAG] proof OK\n");
}

#else
typedef int appliance_network_diagnostics_qemu_translation_unit_guard;
#endif
