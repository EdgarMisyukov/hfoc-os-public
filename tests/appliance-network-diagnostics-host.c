#include "appliance_network_diagnostics.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifdef HFOC_APPLIANCE_NETWORK_DIAGNOSTICS_HOST_TEST

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

static struct network_ipv4_config make_active(void)
{
    struct network_ipv4_config config;

    memset(&config, 0, sizeof(config));
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

static struct appliance_network_config make_static_desired(void)
{
    struct appliance_network_config desired;
    struct network_ipv4_config active = make_active();

    memset(&desired, 0, sizeof(desired));
    if (appliance_network_config_publish_static(
            &desired, "eth0", 4U, &active) !=
        APPLIANCE_NETWORK_CONFIG_OK) {
        fprintf(stderr, "fixture failed to publish static desired\n");
        __builtin_unreachable();
    }
    return desired;
}

static struct appliance_network_config make_dhcp_desired(void)
{
    struct appliance_network_config desired;

    memset(&desired, 0, sizeof(desired));
    if (appliance_network_config_publish_dhcp(
            &desired, "eth0", 4U) != APPLIANCE_NETWORK_CONFIG_OK) {
        fprintf(stderr, "fixture failed to publish DHCP desired\n");
        __builtin_unreachable();
    }
    return desired;
}

static struct network_link_state_record make_link(
    enum network_link_state state)
{
    struct network_link_state_record link;

    memset(&link, 0, sizeof(link));
    link.version = NETWORK_LINK_STATE_VERSION;
    link.device_id = 1U;
    link.state = state;
    link.generation = 7U;
    return link;
}

static void test_status_strings(void)
{
    enum appliance_network_diagnostics_status status;
    enum appliance_network_reachability reachability;
    enum appliance_network_diagnostics_fault fault;

    for (status = APPLIANCE_NETWORK_DIAGNOSTICS_OK;
         status <= APPLIANCE_NETWORK_DIAGNOSTICS_OVERFLOW;
         ++status) {
        const char *string =
            appliance_network_diagnostics_status_string(status);

        CHECK(string != NULL &&
                  (status == APPLIANCE_NETWORK_DIAGNOSTICS_OK ?
                       strcmp(string,
                              "APPLIANCE_NETWORK_DIAGNOSTICS_OK") == 0 :
                   status == APPLIANCE_NETWORK_DIAGNOSTICS_NULL ?
                       strcmp(string,
                              "APPLIANCE_NETWORK_DIAGNOSTICS_NULL") == 0 :
                   status == APPLIANCE_NETWORK_DIAGNOSTICS_BAD_VERSION ?
                       strcmp(string,
                              "APPLIANCE_NETWORK_DIAGNOSTICS_BAD_VERSION") == 0 :
                   status == APPLIANCE_NETWORK_DIAGNOSTICS_BAD_STATE ?
                       strcmp(string,
                              "APPLIANCE_NETWORK_DIAGNOSTICS_BAD_STATE") == 0 :
                   status == APPLIANCE_NETWORK_DIAGNOSTICS_BAD_CONFIG ?
                       strcmp(string,
                              "APPLIANCE_NETWORK_DIAGNOSTICS_BAD_CONFIG") == 0 :
                   status == APPLIANCE_NETWORK_DIAGNOSTICS_BAD_LINK ?
                       strcmp(string,
                              "APPLIANCE_NETWORK_DIAGNOSTICS_BAD_LINK") == 0 :
                   status == APPLIANCE_NETWORK_DIAGNOSTICS_BAD_ACTIVE ?
                       strcmp(string,
                              "APPLIANCE_NETWORK_DIAGNOSTICS_BAD_ACTIVE") == 0 :
                   status == APPLIANCE_NETWORK_DIAGNOSTICS_BAD_REACHABILITY ?
                       strcmp(string,
                              "APPLIANCE_NETWORK_DIAGNOSTICS_BAD_REACHABILITY") == 0 :
                       strcmp(string,
                              "APPLIANCE_NETWORK_DIAGNOSTICS_OVERFLOW") == 0),
              "network diagnostics status string is exact for every status");
    }
    CHECK(appliance_network_diagnostics_status_string(
              (enum appliance_network_diagnostics_status)9U) == NULL,
          "network diagnostics rejects undefined status string");

    for (reachability = APPLIANCE_NETWORK_REACHABILITY_UNKNOWN;
         reachability <= APPLIANCE_NETWORK_REACHABILITY_UNREACHABLE;
         ++reachability) {
        const char *string =
            appliance_network_reachability_string(reachability);

        CHECK(string != NULL &&
                  (reachability == APPLIANCE_NETWORK_REACHABILITY_UNKNOWN ?
                       strcmp(string, "UNKNOWN") == 0 :
                   reachability == APPLIANCE_NETWORK_REACHABILITY_REACHABLE ?
                       strcmp(string, "REACHABLE") == 0 :
                       strcmp(string, "UNREACHABLE") == 0),
              "network reachability string is exact for every state");
    }
    CHECK(appliance_network_reachability_string(
              (enum appliance_network_reachability)3U) == NULL,
          "network diagnostics rejects undefined reachability string");

    for (fault = APPLIANCE_NETWORK_DIAGNOSTICS_FAULT_NONE;
         fault <= APPLIANCE_NETWORK_DIAGNOSTICS_FAULT_REACHABILITY;
         ++fault) {
        const char *string =
            appliance_network_diagnostics_fault_string(fault);

        CHECK(string != NULL &&
                  (fault == APPLIANCE_NETWORK_DIAGNOSTICS_FAULT_NONE ?
                       strcmp(string, "NONE") == 0 :
                   fault == APPLIANCE_NETWORK_DIAGNOSTICS_FAULT_LINK ?
                       strcmp(string, "LINK") == 0 :
                   fault == APPLIANCE_NETWORK_DIAGNOSTICS_FAULT_ADDRESS ?
                       strcmp(string, "ADDRESS") == 0 :
                   fault == APPLIANCE_NETWORK_DIAGNOSTICS_FAULT_CONFIG ?
                       strcmp(string, "CONFIG") == 0 :
                       strcmp(string, "REACHABILITY") == 0),
              "network diagnostics fault string is exact for every fault");
    }
    CHECK(appliance_network_diagnostics_fault_string(
              (enum appliance_network_diagnostics_fault)5U) == NULL,
          "network diagnostics rejects undefined fault string");
}

static void test_null_and_invalid(void)
{
    struct appliance_network_diagnostics diagnostics;
    struct appliance_network_config static_desired = make_static_desired();
    struct appliance_network_config dhcp_desired = make_dhcp_desired();
    struct network_link_state_record link = make_link(NETWORK_LINK_UP);
    struct network_ipv4_config active = make_active();
    struct appliance_network_config bad_desired;
    struct network_link_state_record bad_link;
    struct network_ipv4_config bad_active;

    memset(&diagnostics, 0, sizeof(diagnostics));
    memset(&bad_desired, 0, sizeof(bad_desired));
    memset(&bad_link, 0, sizeof(bad_link));
    memset(&bad_active, 0, sizeof(bad_active));

    CHECK(appliance_network_diagnostics_publish(
              NULL, &dhcp_desired, &link, NULL,
              APPLIANCE_NETWORK_REACHABILITY_UNKNOWN,
              APPLIANCE_NETWORK_REACHABILITY_UNKNOWN) ==
              APPLIANCE_NETWORK_DIAGNOSTICS_NULL,
          "network diagnostics publish rejects null snapshot");
    CHECK(appliance_network_diagnostics_publish(
              &diagnostics, NULL, &link, NULL,
              APPLIANCE_NETWORK_REACHABILITY_UNKNOWN,
              APPLIANCE_NETWORK_REACHABILITY_UNKNOWN) ==
              APPLIANCE_NETWORK_DIAGNOSTICS_NULL,
          "network diagnostics publish rejects null desired config");
    CHECK(appliance_network_diagnostics_publish(
              &diagnostics, &dhcp_desired, NULL, NULL,
              APPLIANCE_NETWORK_REACHABILITY_UNKNOWN,
              APPLIANCE_NETWORK_REACHABILITY_UNKNOWN) ==
              APPLIANCE_NETWORK_DIAGNOSTICS_NULL,
          "network diagnostics publish rejects null link");
    CHECK(appliance_network_diagnostics_publish(
              &diagnostics, &bad_desired, &link, NULL,
              APPLIANCE_NETWORK_REACHABILITY_UNKNOWN,
              APPLIANCE_NETWORK_REACHABILITY_UNKNOWN) ==
              APPLIANCE_NETWORK_DIAGNOSTICS_BAD_CONFIG,
          "network diagnostics publish rejects invalid desired config");
    bad_link = link;
    bad_link.version = 0U;
    CHECK(appliance_network_diagnostics_publish(
              &diagnostics, &dhcp_desired, &bad_link, NULL,
              APPLIANCE_NETWORK_REACHABILITY_UNKNOWN,
              APPLIANCE_NETWORK_REACHABILITY_UNKNOWN) ==
              APPLIANCE_NETWORK_DIAGNOSTICS_BAD_LINK,
          "network diagnostics publish rejects invalid link");
    bad_active.schema_version = 0U;
    CHECK(appliance_network_diagnostics_publish(
              &diagnostics, &dhcp_desired, &link, &bad_active,
              APPLIANCE_NETWORK_REACHABILITY_UNKNOWN,
              APPLIANCE_NETWORK_REACHABILITY_UNKNOWN) ==
              APPLIANCE_NETWORK_DIAGNOSTICS_BAD_ACTIVE,
          "network diagnostics publish rejects invalid active config");
    CHECK(appliance_network_diagnostics_publish(
              &diagnostics, &dhcp_desired, &link, NULL,
              (enum appliance_network_reachability)9U,
              APPLIANCE_NETWORK_REACHABILITY_UNKNOWN) ==
              APPLIANCE_NETWORK_DIAGNOSTICS_BAD_REACHABILITY,
          "network diagnostics publish rejects invalid gateway reachability");
    CHECK(appliance_network_diagnostics_publish(
              &diagnostics, &dhcp_desired, &link, NULL,
              APPLIANCE_NETWORK_REACHABILITY_UNKNOWN,
              (enum appliance_network_reachability)9U) ==
              APPLIANCE_NETWORK_DIAGNOSTICS_BAD_REACHABILITY,
          "network diagnostics publish rejects invalid DNS reachability");
    CHECK(!diagnostics.published,
          "failed network diagnostics publication leaves snapshot unpublished");

    CHECK(appliance_network_diagnostics_publish(
              &diagnostics, &static_desired, &link, NULL,
              APPLIANCE_NETWORK_REACHABILITY_UNKNOWN,
              APPLIANCE_NETWORK_REACHABILITY_UNKNOWN) ==
              APPLIANCE_NETWORK_DIAGNOSTICS_OK &&
              diagnostics.result == HEALTH_CHECK_RESULT_FAIL &&
              diagnostics.fault ==
                  APPLIANCE_NETWORK_DIAGNOSTICS_FAULT_CONFIG,
          "STATIC without active config publishes FAIL");
    memset(&diagnostics, 0, sizeof(diagnostics));
    CHECK(appliance_network_diagnostics_publish(
              &diagnostics, &static_desired, &link, &active,
              APPLIANCE_NETWORK_REACHABILITY_REACHABLE,
              APPLIANCE_NETWORK_REACHABILITY_REACHABLE) ==
              APPLIANCE_NETWORK_DIAGNOSTICS_OK,
          "network diagnostics publish accepts validated inputs");
    CHECK(appliance_network_diagnostics_publish(
              &diagnostics, &dhcp_desired, &link, NULL,
              APPLIANCE_NETWORK_REACHABILITY_UNKNOWN,
              APPLIANCE_NETWORK_REACHABILITY_UNKNOWN) ==
              APPLIANCE_NETWORK_DIAGNOSTICS_OK,
          "network diagnostics publish can be republished");
}

static void test_dhcp_health(void)
{
    struct appliance_network_diagnostics diagnostics;
    struct appliance_network_config desired = make_dhcp_desired();
    struct network_link_state_record link = make_link(NETWORK_LINK_UP);
    struct network_ipv4_config active = make_active();

    memset(&diagnostics, 0, sizeof(diagnostics));
    CHECK(appliance_network_diagnostics_publish(
              &diagnostics, &desired, &link, NULL,
              APPLIANCE_NETWORK_REACHABILITY_UNKNOWN,
              APPLIANCE_NETWORK_REACHABILITY_UNKNOWN) ==
              APPLIANCE_NETWORK_DIAGNOSTICS_OK &&
              diagnostics.result == HEALTH_CHECK_RESULT_WARN &&
              diagnostics.fault ==
                  APPLIANCE_NETWORK_DIAGNOSTICS_FAULT_ADDRESS,
          "DHCP without active lease publishes WARN address");

    CHECK(appliance_network_diagnostics_publish(
              &diagnostics, &desired, &link, &active,
              APPLIANCE_NETWORK_REACHABILITY_REACHABLE,
              APPLIANCE_NETWORK_REACHABILITY_REACHABLE) ==
              APPLIANCE_NETWORK_DIAGNOSTICS_OK &&
              diagnostics.result == HEALTH_CHECK_RESULT_PASS &&
              diagnostics.fault ==
                  APPLIANCE_NETWORK_DIAGNOSTICS_FAULT_NONE,
          "DHCP with active config and reachable paths publishes PASS");

    CHECK(appliance_network_diagnostics_publish(
              &diagnostics, &desired, &link, &active,
              APPLIANCE_NETWORK_REACHABILITY_UNKNOWN,
              APPLIANCE_NETWORK_REACHABILITY_REACHABLE) ==
              APPLIANCE_NETWORK_DIAGNOSTICS_OK &&
              diagnostics.result == HEALTH_CHECK_RESULT_WARN &&
              diagnostics.fault ==
                  APPLIANCE_NETWORK_DIAGNOSTICS_FAULT_REACHABILITY,
          "unknown reachability publishes WARN");

    CHECK(appliance_network_diagnostics_publish(
              &diagnostics, &desired, &link, &active,
              APPLIANCE_NETWORK_REACHABILITY_REACHABLE,
              APPLIANCE_NETWORK_REACHABILITY_UNREACHABLE) ==
              APPLIANCE_NETWORK_DIAGNOSTICS_OK &&
              diagnostics.result == HEALTH_CHECK_RESULT_FAIL &&
              diagnostics.fault ==
                  APPLIANCE_NETWORK_DIAGNOSTICS_FAULT_REACHABILITY,
          "unreachable DNS publishes FAIL");

    link = make_link(NETWORK_LINK_DOWN);
    CHECK(appliance_network_diagnostics_publish(
              &diagnostics, &desired, &link, &active,
              APPLIANCE_NETWORK_REACHABILITY_REACHABLE,
              APPLIANCE_NETWORK_REACHABILITY_REACHABLE) ==
              APPLIANCE_NETWORK_DIAGNOSTICS_OK &&
              diagnostics.result == HEALTH_CHECK_RESULT_FAIL &&
              diagnostics.fault ==
                  APPLIANCE_NETWORK_DIAGNOSTICS_FAULT_LINK,
          "down link publishes FAIL link");

    link = make_link(NETWORK_LINK_INITIALIZING);
    CHECK(appliance_network_diagnostics_publish(
              &diagnostics, &desired, &link, NULL,
              APPLIANCE_NETWORK_REACHABILITY_UNKNOWN,
              APPLIANCE_NETWORK_REACHABILITY_UNKNOWN) ==
              APPLIANCE_NETWORK_DIAGNOSTICS_OK &&
              diagnostics.result == HEALTH_CHECK_RESULT_UNKNOWN &&
              diagnostics.fault ==
                  APPLIANCE_NETWORK_DIAGNOSTICS_FAULT_NONE,
          "initializing link publishes UNKNOWN");
}

static void test_static_health(void)
{
    struct appliance_network_diagnostics diagnostics;
    struct appliance_network_config desired = make_static_desired();
    struct network_link_state_record link = make_link(NETWORK_LINK_UP);
    struct network_ipv4_config active = make_active();
    struct network_ipv4_config changed = make_active();

    memset(&diagnostics, 0, sizeof(diagnostics));
    CHECK(appliance_network_diagnostics_publish(
              &diagnostics, &desired, &link, &active,
              APPLIANCE_NETWORK_REACHABILITY_REACHABLE,
              APPLIANCE_NETWORK_REACHABILITY_REACHABLE) ==
              APPLIANCE_NETWORK_DIAGNOSTICS_OK &&
              diagnostics.result == HEALTH_CHECK_RESULT_PASS &&
              diagnostics.fault ==
                  APPLIANCE_NETWORK_DIAGNOSTICS_FAULT_NONE,
          "matching STATIC active config publishes PASS");

    changed.address.octets[3] = 20U;
    CHECK(appliance_network_diagnostics_publish(
              &diagnostics, &desired, &link, &changed,
              APPLIANCE_NETWORK_REACHABILITY_REACHABLE,
              APPLIANCE_NETWORK_REACHABILITY_REACHABLE) ==
              APPLIANCE_NETWORK_DIAGNOSTICS_OK &&
              diagnostics.result == HEALTH_CHECK_RESULT_FAIL &&
              diagnostics.fault ==
                  APPLIANCE_NETWORK_DIAGNOSTICS_FAULT_CONFIG,
          "mismatched STATIC active config publishes FAIL config");

    changed = make_active();
    changed.gateway.octets[3] = 99U;
    CHECK(appliance_network_diagnostics_publish(
              &diagnostics, &desired, &link, &changed,
              APPLIANCE_NETWORK_REACHABILITY_REACHABLE,
              APPLIANCE_NETWORK_REACHABILITY_REACHABLE) ==
              APPLIANCE_NETWORK_DIAGNOSTICS_OK &&
              diagnostics.fault ==
                  APPLIANCE_NETWORK_DIAGNOSTICS_FAULT_CONFIG,
          "changed STATIC gateway is still a config mismatch");
}

static void test_render(void)
{
    struct appliance_network_diagnostics diagnostics;
    struct appliance_network_config static_desired = make_static_desired();
    struct appliance_network_config dhcp_desired = make_dhcp_desired();
    struct network_link_state_record link = make_link(NETWORK_LINK_UP);
    struct network_ipv4_config active = make_active();
    char output[APPLIANCE_NETWORK_DIAGNOSTICS_OUTPUT_MAX + 1U];
    size_t length = 0U;

    CHECK(appliance_network_diagnostics_render(
              NULL, output, sizeof(output), &length) ==
              APPLIANCE_NETWORK_DIAGNOSTICS_NULL,
          "network diagnostics render rejects null snapshot");
    memset(&diagnostics, 0, sizeof(diagnostics));
    CHECK(appliance_network_diagnostics_render(
              &diagnostics, output, sizeof(output), &length) ==
              APPLIANCE_NETWORK_DIAGNOSTICS_BAD_STATE,
          "network diagnostics render rejects unpublished snapshot");

    memset(&diagnostics, 0, sizeof(diagnostics));
    CHECK(appliance_network_diagnostics_publish(
              &diagnostics, &static_desired, &link, &active,
              APPLIANCE_NETWORK_REACHABILITY_REACHABLE,
              APPLIANCE_NETWORK_REACHABILITY_REACHABLE) ==
              APPLIANCE_NETWORK_DIAGNOSTICS_OK,
          "network diagnostics render fixture publishes STATIC");
    length = 0U;
    memset(output, 0, sizeof(output));
    CHECK(appliance_network_diagnostics_render(
              &diagnostics, output, sizeof(output), &length) ==
              APPLIANCE_NETWORK_DIAGNOSTICS_OK &&
              strcmp(output,
                     "network_diag v=1 interface=eth0 mode=STATIC "
                     "device_id=1 link=UP active=yes "
                     "address=10.0.2.15 prefix=24 gateway=10.0.2.2 "
                     "dns=10.0.2.3 gateway_reach=REACHABLE "
                     "dns_reach=REACHABLE result=PASS fault=NONE\n") == 0,
          "network diagnostics renders exact STATIC record");

    memset(&diagnostics, 0, sizeof(diagnostics));
    CHECK(appliance_network_diagnostics_publish(
              &diagnostics, &dhcp_desired, &link, NULL,
              APPLIANCE_NETWORK_REACHABILITY_UNKNOWN,
              APPLIANCE_NETWORK_REACHABILITY_UNKNOWN) ==
              APPLIANCE_NETWORK_DIAGNOSTICS_OK,
          "network diagnostics render fixture publishes DHCP");
    length = 0U;
    memset(output, 0, sizeof(output));
    CHECK(appliance_network_diagnostics_render(
              &diagnostics, output, sizeof(output), &length) ==
              APPLIANCE_NETWORK_DIAGNOSTICS_OK &&
              strcmp(output,
                     "network_diag v=1 interface=eth0 mode=DHCP "
                     "device_id=1 link=UP active=no address=none "
                     "prefix=0 gateway=none dns=none "
                     "gateway_reach=UNKNOWN dns_reach=UNKNOWN "
                     "result=WARN fault=ADDRESS\n") == 0,
          "network diagnostics renders exact DHCP record");

    memset(output, 'x', 8U);
    length = 0U;
    CHECK(appliance_network_diagnostics_render(
              &diagnostics, output, 8U, &length) ==
              APPLIANCE_NETWORK_DIAGNOSTICS_OVERFLOW &&
              memcmp(output, "xxxxxxxx", 8U) == 0,
          "network diagnostics render overflow leaves output unchanged");
}

int main(void)
{
    test_status_strings();
    test_null_and_invalid();
    test_dhcp_health();
    test_static_health();
    test_render();

    if (alloc_attempts != 0U || free_attempts != 0U) {
        fprintf(stderr, "FAIL: network diagnostics performed allocation\n");
        ++failures;
    } else {
        printf("PASS: network diagnostics performs zero allocation\n");
    }

    if (failures != 0) {
        fprintf(stderr, "network diagnostics host failures: %d\n", failures);
        return 1;
    }
    printf("Appliance network diagnostics host test: OK\n");
    return 0;
}

#else
typedef int appliance_network_diagnostics_host_translation_unit_guard;
#endif
