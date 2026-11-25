#ifdef HFOC_APPLIANCE_NETWORK_CONFIG_QEMU_TEST

#include "appliance_dns_resolver.h"
#include "ipv4_config.h"
#include "serial.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

static void appliance_dns_qemu_zero_bytes(
    void *destination,
    size_t length)
{
    unsigned char *bytes = destination;
    size_t index;

    for (index = 0U; index < length; ++index) {
        bytes[index] = 0U;
    }
}

static bool appliance_dns_qemu_equal_bytes(
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

static void appliance_dns_qemu_failure(
    const char *message)
    __attribute__((noreturn));

static void appliance_dns_qemu_failure(
    const char *message)
{
    {
        const uint64_t serial_flags = serial_sequence_begin();
        serial_write("[DNS-RESOLVER] failed: ");
        serial_write(message);
        serial_write("\n");
        serial_sequence_end(serial_flags);
    }
    for (;;) {
        __asm__ volatile ("hlt" : : : "memory");
    }
}

static void appliance_dns_qemu_require(
    bool condition,
    const char *message)
{
    if (!condition) {
        appliance_dns_qemu_failure(message);
    }
}

static struct network_ipv4_config appliance_dns_qemu_config(void)
{
    struct network_ipv4_config config;

    appliance_dns_qemu_zero_bytes(&config, sizeof(config));
    config.schema_version = NETWORK_IPV4_CONFIG_VERSION;
    config.address.octets[0] = 10U;
    config.address.octets[2] = 2U;
    config.address.octets[3] = 15U;
    config.prefix_length = 24U;
    config.gateway.octets[0] = 10U;
    config.gateway.octets[2] = 2U;
    config.gateway.octets[3] = 2U;
    config.dns.octets[0] = 10U;
    config.dns.octets[2] = 2U;
    config.dns.octets[3] = 3U;
    return config;
}

static struct network_ipv4_address appliance_dns_qemu_answer(void)
{
    struct network_ipv4_address answer;

    appliance_dns_qemu_zero_bytes(&answer, sizeof(answer));
    answer.octets[0] = 10U;
    answer.octets[2] = 2U;
    answer.octets[3] = 15U;
    return answer;
}

static bool appliance_dns_qemu_render_is(
    const struct appliance_dns_resolver *resolver,
    const char *expected)
{
    char output[APPLIANCE_DNS_RESOLVER_OUTPUT_MAX + 1U];
    size_t output_length = 0U;
    size_t expected_length = 0U;

    while (expected[expected_length] != '\0') {
        ++expected_length;
    }
    return appliance_dns_resolver_render(
               resolver, output, sizeof(output), &output_length) ==
               APPLIANCE_DNS_RESOLVER_OK &&
           output_length == expected_length &&
           appliance_dns_qemu_equal_bytes(
               output, expected, expected_length);
}

void appliance_dns_resolver_qemu_test_init(void)
{
    struct network_ipv4_config config = appliance_dns_qemu_config();
    struct network_ipv4_address answer = appliance_dns_qemu_answer();
    struct appliance_dns_resolver resolver;

    serial_write("[DNS-RESOLVER] proof init\n");
    appliance_dns_qemu_require(
        appliance_dns_resolver_init(
            &resolver, &config, "eth0", 4U) ==
            APPLIANCE_DNS_RESOLVER_OK &&
        appliance_dns_resolver_query(
            &resolver, "api.hfoc.test", 13U) ==
            APPLIANCE_DNS_RESOLVER_OK &&
        appliance_dns_resolver_timeout(&resolver) ==
            APPLIANCE_DNS_RESOLVER_OK &&
        appliance_dns_resolver_response(
            &resolver, &answer) ==
            APPLIANCE_DNS_RESOLVER_OK &&
        appliance_dns_resolver_state(&resolver) ==
            APPLIANCE_DNS_RESOLVER_STATE_RESOLVED &&
        appliance_dns_resolver_answer_published(&resolver) &&
        appliance_dns_qemu_render_is(
            &resolver,
            "dns_resolver v=1 interface=eth0 server=10.0.2.3 "
            "host=api.hfoc.test state=RESOLVED result=10.0.2.15 "
            "attempts=2\n"),
        "DNS query timeout retry and answer publication");
    serial_write("[DNS-RESOLVER] resolution published\n");

    appliance_dns_qemu_require(
        appliance_dns_resolver_clear(&resolver) ==
            APPLIANCE_DNS_RESOLVER_OK &&
        appliance_dns_resolver_query(
            &resolver, "api.hfoc.test", 13U) ==
            APPLIANCE_DNS_RESOLVER_OK &&
        appliance_dns_resolver_timeout(&resolver) ==
            APPLIANCE_DNS_RESOLVER_OK &&
        appliance_dns_resolver_timeout(&resolver) ==
            APPLIANCE_DNS_RESOLVER_EXHAUSTED &&
        appliance_dns_resolver_state(&resolver) ==
            APPLIANCE_DNS_RESOLVER_STATE_FAILED &&
        !appliance_dns_resolver_answer_published(&resolver),
        "DNS timeout retry exhaustion fails without answer");
    serial_write("[DNS-RESOLVER] timeout fallback proven\n");

    appliance_dns_qemu_require(
        appliance_dns_resolver_clear(&resolver) ==
            APPLIANCE_DNS_RESOLVER_OK &&
        appliance_dns_resolver_query(
            &resolver, "api.hfoc.test", 13U) ==
            APPLIANCE_DNS_RESOLVER_OK &&
        appliance_dns_resolver_server_failure(&resolver) ==
            APPLIANCE_DNS_RESOLVER_OK &&
        appliance_dns_resolver_server_failure(&resolver) ==
            APPLIANCE_DNS_RESOLVER_EXHAUSTED &&
        appliance_dns_resolver_state(&resolver) ==
            APPLIANCE_DNS_RESOLVER_STATE_FAILED,
        "DNS server failure is bounded");
    serial_write("[DNS-RESOLVER] server failure fallback proven\n");

    appliance_dns_qemu_require(
        appliance_dns_resolver_clear(&resolver) ==
            APPLIANCE_DNS_RESOLVER_OK &&
        appliance_dns_resolver_query(
            &resolver, "api.hfoc.test", 13U) ==
            APPLIANCE_DNS_RESOLVER_OK &&
        appliance_dns_resolver_response(
            &resolver, &answer) ==
            APPLIANCE_DNS_RESOLVER_OK &&
        appliance_dns_resolver_reset(&resolver) ==
            APPLIANCE_DNS_RESOLVER_OK &&
        appliance_dns_resolver_state(&resolver) ==
            APPLIANCE_DNS_RESOLVER_STATE_NONE,
        "DNS clear and reset restore clean state");
    serial_write("[DNS-RESOLVER] clean shutdown\n");
    serial_write("[DNS-RESOLVER] proof OK\n");
}

#else
typedef int appliance_dns_resolver_qemu_translation_unit_guard;
#endif
