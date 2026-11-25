/*
 * ITEM-374 focused host proof: network modes, honest offline decisions and
 * fail-closed management ingress binding.
 */

#ifdef HFOC_NETWORK_MODE_HOST_TEST

#include "appliance_management_bind.h"
#include "appliance_network_config.h"
#include "appliance_network_mode.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static int failures;
static int checks;

#define CHECK(condition, message)                                        \
    do {                                                                 \
        ++checks;                                                        \
        if (!(condition)) {                                              \
            ++failures;                                                  \
            fprintf(stderr, "FAIL: %s\n", (message));                    \
        }                                                                \
    } while (0)

static struct network_ipv4_config host_static_config(void)
{
    struct network_ipv4_config config;

    memset(&config, 0, sizeof(config));
    config.schema_version = NETWORK_IPV4_CONFIG_VERSION;
    config.address.octets[0] = 192U;
    config.address.octets[1] = 168U;
    config.address.octets[2] = 1U;
    config.address.octets[3] = 50U;
    config.prefix_length = 24U;
    config.gateway.octets[0] = 192U;
    config.gateway.octets[1] = 168U;
    config.gateway.octets[2] = 1U;
    config.gateway.octets[3] = 1U;
    config.dns.octets[0] = 192U;
    config.dns.octets[1] = 168U;
    config.dns.octets[2] = 1U;
    config.dns.octets[3] = 1U;
    return config;
}

static void host_offline_config(void)
{
    struct appliance_network_config config;
    struct network_ipv4_config static_config = host_static_config();

    memset(&config, 0, sizeof(config));
    CHECK(appliance_network_config_publish_offline(
              &config, "eth0", 4U) == APPLIANCE_NETWORK_CONFIG_OK &&
              config.mode == APPLIANCE_NETWORK_CONFIG_MODE_OFFLINE &&
              config.static_present == false &&
              config.initialized,
          "an offline configuration publishes without an address");
    CHECK(appliance_network_config_validate(&config) ==
              APPLIANCE_NETWORK_CONFIG_OK,
          "an offline configuration validates");
    CHECK(strcmp(appliance_network_config_mode_string(
                     APPLIANCE_NETWORK_CONFIG_MODE_OFFLINE),
                 "OFFLINE") == 0,
          "the offline mode has an exact string");
    CHECK(appliance_network_config_publish_offline(
              NULL, "eth0", 4U) == APPLIANCE_NETWORK_CONFIG_NULL,
          "offline publication refuses a null configuration");
    CHECK(appliance_network_config_publish_offline(
              &config, "eth0", 0U) ==
              APPLIANCE_NETWORK_CONFIG_INVALID_INTERFACE,
          "offline publication refuses an empty interface");
    CHECK(appliance_network_config_publish_static(
              &config, "eth0", 4U, &static_config) ==
              APPLIANCE_NETWORK_CONFIG_OK &&
              config.mode == APPLIANCE_NETWORK_CONFIG_MODE_STATIC &&
              config.static_present,
          "static mode still publishes its address");
}

static void host_mode_decisions(void)
{
    struct appliance_network_config config;
    struct network_ipv4_config static_config = host_static_config();
    struct appliance_network_mode_decision decision;

    memset(&config, 0, sizeof(config));
    memset(&decision, 0, sizeof(decision));
    CHECK(appliance_network_mode_decide(NULL, true, &decision) ==
              APPLIANCE_NETWORK_MODE_NULL &&
              appliance_network_mode_decide(&config, true, NULL) ==
                  APPLIANCE_NETWORK_MODE_NULL,
          "the mode decision refuses null arguments");

    CHECK(appliance_network_config_publish_dhcp(
              &config, "eth0", 4U) == APPLIANCE_NETWORK_CONFIG_OK &&
              appliance_network_mode_decide(&config, true, &decision) ==
                  APPLIANCE_NETWORK_MODE_OK &&
              decision.action == APPLIANCE_NETWORK_MODE_ACTION_DHCP &&
              decision.addressing && !decision.degraded,
          "DHCP with a device present runs the DHCP path");
    CHECK(appliance_network_mode_decide(&config, false, &decision) ==
              APPLIANCE_NETWORK_MODE_OK &&
              decision.action ==
                  APPLIANCE_NETWORK_MODE_ACTION_UNAVAILABLE &&
              !decision.addressing && decision.degraded,
          "a missing device publishes an honest degraded decision");

    CHECK(appliance_network_config_publish_static(
              &config, "eth0", 4U, &static_config) ==
              APPLIANCE_NETWORK_CONFIG_OK &&
              appliance_network_mode_decide(&config, true, &decision) ==
                  APPLIANCE_NETWORK_MODE_OK &&
              decision.action == APPLIANCE_NETWORK_MODE_ACTION_STATIC &&
              decision.addressing && !decision.degraded,
          "static mode applies its address without DHCP");

    CHECK(appliance_network_config_publish_offline(
              &config, "eth0", 4U) == APPLIANCE_NETWORK_CONFIG_OK &&
              appliance_network_mode_decide(&config, true, &decision) ==
                  APPLIANCE_NETWORK_MODE_OK &&
              decision.action == APPLIANCE_NETWORK_MODE_ACTION_OFFLINE &&
              !decision.addressing && decision.degraded,
          "offline mode publishes DEGRADED without an address");
    CHECK(strcmp(appliance_network_mode_action_string(
                     APPLIANCE_NETWORK_MODE_ACTION_OFFLINE),
                 "OFFLINE") == 0 &&
              strcmp(appliance_network_mode_status_string(
                         APPLIANCE_NETWORK_MODE_OK),
                     "OK") == 0,
          "mode decision strings are exact");
}

static void host_management_bind(void)
{
    struct appliance_management_bind bind;
    const uint8_t outer_peer[4U] = { 203U, 0U, 113U, 9U };
    const uint8_t management_peer[4U] = { 192U, 168U, 1U, 10U };
    const uint8_t allowed_peers[1U][4U] = { { 192U, 168U, 1U, 10U } };

    memset(&bind, 0, sizeof(bind));
    CHECK(appliance_management_bind_admit(
              &bind, false, outer_peer) ==
              APPLIANCE_MANAGEMENT_BIND_DENIED_UNCONFIGURED,
          "an unpublished binding denies network ingress");
    CHECK(appliance_management_bind_publish_default(&bind) ==
              APPLIANCE_MANAGEMENT_BIND_OK &&
              appliance_management_bind_is_published(&bind) &&
              appliance_management_bind_scope_of(&bind) ==
                  APPLIANCE_MANAGEMENT_BIND_SCOPE_LOCAL_ONLY,
          "the default binding is local only");
    CHECK(appliance_management_bind_admit(&bind, false, outer_peer) ==
              APPLIANCE_MANAGEMENT_BIND_DENIED_OUTER &&
              appliance_management_bind_admit(
                  &bind, true, management_peer) ==
                  APPLIANCE_MANAGEMENT_BIND_DENIED_OUTER,
          "local-only denies every network interface by default");

    CHECK(appliance_management_bind_publish_management_interface(
              &bind, "eth1", 4U, allowed_peers, 1U) ==
              APPLIANCE_MANAGEMENT_BIND_OK &&
              appliance_management_bind_scope_of(&bind) ==
                  APPLIANCE_MANAGEMENT_BIND_SCOPE_MANAGEMENT_INTERFACE,
          "a management interface binding publishes");
    CHECK(appliance_management_bind_admit(
              &bind, true, management_peer) ==
              APPLIANCE_MANAGEMENT_BIND_ALLOWED,
          "a declared management peer is admitted on the management interface");
    CHECK(appliance_management_bind_admit(
              &bind, true, outer_peer) ==
              APPLIANCE_MANAGEMENT_BIND_DENIED_PEER,
          "an undeclared peer is denied on the management interface");
    CHECK(appliance_management_bind_admit(&bind, false, management_peer) ==
              APPLIANCE_MANAGEMENT_BIND_DENIED_OUTER,
          "the outer interface stays denied");
    CHECK(appliance_management_bind_admit(
              &bind, true, NULL) ==
              APPLIANCE_MANAGEMENT_BIND_NULL,
          "a null ingress address is refused");

    CHECK(appliance_management_bind_publish_outer_interface(&bind) ==
              APPLIANCE_MANAGEMENT_BIND_OK &&
              appliance_management_bind_scope_of(&bind) ==
                  APPLIANCE_MANAGEMENT_BIND_SCOPE_OUTER_INTERFACE &&
              appliance_management_bind_admit(&bind, false, outer_peer) ==
                  APPLIANCE_MANAGEMENT_BIND_ALLOWED,
          "an explicit outer-interface binding opts in");
    CHECK(appliance_management_bind_publish_management_interface(
              &bind, "", 0U, NULL, 0U) ==
              APPLIANCE_MANAGEMENT_BIND_BAD_INTERFACE &&
              appliance_management_bind_publish_management_interface(
                  &bind, "eth1", 4U, allowed_peers,
                  APPLIANCE_MANAGEMENT_BIND_PEER_MAX + 1U) ==
                  APPLIANCE_MANAGEMENT_BIND_BAD_PEER,
          "invalid interface and peer sets are refused");
    CHECK(appliance_management_bind_publish_default(NULL) ==
              APPLIANCE_MANAGEMENT_BIND_NULL &&
              appliance_management_bind_admit(NULL, false, outer_peer) ==
                  APPLIANCE_MANAGEMENT_BIND_NULL,
          "the binding refuses null arguments");
    CHECK(strcmp(appliance_management_bind_scope_string(
                     APPLIANCE_MANAGEMENT_BIND_SCOPE_LOCAL_ONLY),
                 "LOCAL_ONLY") == 0 &&
              strcmp(appliance_management_bind_status_string(
                         APPLIANCE_MANAGEMENT_BIND_DENIED_OUTER),
                     "DENIED_OUTER") == 0,
          "management binding strings are exact");
}

int main(void)
{
    host_offline_config();
    host_mode_decisions();
    host_management_bind();

    if (failures != 0) {
        fprintf(stderr, "Appliance network mode host tests: %d FAILED\n",
                failures);
        return 1;
    }
    printf("Appliance network mode host tests: ALL OK (%d checks)\n",
           checks);
    return 0;
}

#else
int appliance_network_mode_host_translation_unit_marker;
#endif
