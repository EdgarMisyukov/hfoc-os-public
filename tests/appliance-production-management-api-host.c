/*
 * ITEM-400 host proof for the ordinary-boot read-only management snapshot
 * owner.  The owner renders the four read-only management bodies from live
 * production evidence; this proof drives that owner directly with synthetic
 * evidence and asserts the canonical bodies, the honest degraded rendering
 * and the route-resolution edges.
 */
#include "appliance_production_management_api.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static int failures;
static uint32_t alloc_attempts;
static uint32_t free_attempts;

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

static struct appliance_network_config desired_static;
static struct network_ipv4_config active_static;
static struct network_device_snapshot test_device;

static void publish_static_config(void)
{
    memset(&active_static, 0, sizeof(active_static));
    active_static.schema_version = NETWORK_IPV4_CONFIG_VERSION;
    active_static.address.octets[0] = 10U;
    active_static.address.octets[2] = 0U;
    active_static.address.octets[3] = 15U;
    active_static.prefix_length = 24U;
    active_static.gateway.octets[0] = 10U;
    active_static.gateway.octets[2] = 0U;
    active_static.gateway.octets[3] = 2U;
    active_static.dns.octets[0] = 10U;
    active_static.dns.octets[2] = 0U;
    active_static.dns.octets[3] = 3U;
    memset(&desired_static, 0, sizeof(desired_static));
    (void)appliance_network_config_publish_static(
        &desired_static, "eth0", 4U, &active_static);
}

static void publish_test_device(void)
{
    memset(&test_device, 0, sizeof(test_device));
    test_device.version = NETWORK_DEVICE_SNAPSHOT_VERSION;
    test_device.device_id = 1U;
    test_device.vendor_id = 0x10ECU;
    test_device.product_id = 0x8168U;
    test_device.mac_address[0] = 0x52U;
    test_device.mac_address[1] = 0x54U;
    test_device.mac_address[2] = 0x00U;
    test_device.mac_address[3] = 0x12U;
    test_device.mac_address[4] = 0x34U;
    test_device.mac_address[5] = 0x56U;
}

static void evidence_ready(
    struct appliance_production_management_api_evidence *evidence,
    bool static_network)
{
    memset(evidence, 0, sizeof(*evidence));
    evidence->version =
        APPLIANCE_PRODUCTION_MANAGEMENT_API_EVIDENCE_VERSION;
    evidence->identity = appliance_identity_get();
    evidence->boot_readiness = APPLIANCE_PRODUCTION_BOOT_READINESS_READY;
    evidence->boot_mode = APPLIANCE_PRODUCTION_BOOT_MODE_NORMAL;
    evidence->storage_ready = true;
    evidence->config_source = APPLIANCE_CONFIG_VALIDATION_SOURCE_COMMITTED;
    evidence->config_hostname = "node-390";
    evidence->config_hostname_length = 8U;
    if (static_network) {
        evidence->network_desired = &desired_static;
        evidence->network_config_source =
            APPLIANCE_NETWORK_API_CONFIG_SOURCE_COMMITTED;
        evidence->network_active = &active_static;
    } else {
        evidence->network_desired = NULL;
        evidence->network_config_source =
            APPLIANCE_NETWORK_API_CONFIG_SOURCE_NONE;
        evidence->network_active = NULL;
    }
    evidence->network_device = &test_device;
    evidence->network_link_up = true;
    evidence->gateway_reachable = true;
    evidence->dns_reachable = true;
    evidence->https_listening = true;
    evidence->service_present = true;
    evidence->service_state =
        APPLIANCE_PRODUCTION_SERVICE_ACTIVATION_STATE_RUNNING;
    evidence->uptime_ticks = 4242U;
}

static bool body_contains(
    const struct appliance_production_management_api_result *result,
    const char *needle)
{
    const size_t needle_length = strlen(needle);
    uint32_t index;

    if (result->response.body == NULL ||
        result->response.body_length < needle_length) {
        return false;
    }
    for (index = 0U;
         (uint32_t)needle_length + index <= result->response.body_length;
         ++index) {
        if (memcmp(
                result->response.body + index,
                needle,
                needle_length) == 0) {
            return true;
        }
    }
    return false;
}

static enum appliance_production_management_api_status dispatch(
    const struct appliance_production_management_api *owner,
    enum http_method_kind method,
    const char *target,
    struct appliance_production_management_api_result *result)
{
    memset(result, 0, sizeof(*result));
    return appliance_production_management_api_dispatch(
        owner,
        method,
        (const uint8_t *)target,
        (uint32_t)strlen(target),
        result);
}

static void test_publish_validation(void)
{
    struct appliance_production_management_api owner;
    struct appliance_production_management_api_evidence evidence;
    struct appliance_production_management_api_result result;

    memset(&owner, 0, sizeof(owner));
    memset(&evidence, 0, sizeof(evidence));
    CHECK(appliance_production_management_api_publish(NULL, &evidence) ==
              APPLIANCE_PRODUCTION_MANAGEMENT_API_NULL,
          "a missing owner is refused");
    CHECK(appliance_production_management_api_publish(&owner, NULL) ==
              APPLIANCE_PRODUCTION_MANAGEMENT_API_NULL,
          "missing evidence is refused");
    evidence.version =
        APPLIANCE_PRODUCTION_MANAGEMENT_API_EVIDENCE_VERSION;
    CHECK(appliance_production_management_api_publish(&owner, &evidence) ==
              APPLIANCE_PRODUCTION_MANAGEMENT_API_BAD_EVIDENCE,
          "evidence without an identity is refused");
    CHECK(!appliance_production_management_api_published(&owner),
          "a refused publication leaves the owner unpublished");
    memset(&result, 0, sizeof(result));
    CHECK(dispatch(&owner, HTTP_METHOD_GET, "/api/v1/system", &result) ==
              APPLIANCE_PRODUCTION_MANAGEMENT_API_BAD_STATE,
          "an unpublished owner refuses to dispatch");
    CHECK(!appliance_production_management_api_owns(
              &owner, (const uint8_t *)"/api/v1/system", 14U),
          "an unpublished owner owns no target");

    evidence_ready(&evidence, true);
    evidence.version =
        APPLIANCE_PRODUCTION_MANAGEMENT_API_EVIDENCE_VERSION + 1U;
    CHECK(appliance_production_management_api_publish(&owner, &evidence) ==
              APPLIANCE_PRODUCTION_MANAGEMENT_API_BAD_EVIDENCE,
          "evidence with an unknown version is refused");
    evidence_ready(&evidence, true);
    CHECK(appliance_production_management_api_publish(&owner, &evidence) ==
              APPLIANCE_PRODUCTION_MANAGEMENT_API_OK &&
              appliance_production_management_api_published(&owner),
          "complete evidence publishes the owner");
}

static void test_route_resolution(void)
{
    struct appliance_production_management_api owner;
    struct appliance_production_management_api_evidence evidence;
    struct appliance_production_management_api_result result;

    evidence_ready(&evidence, true);
    memset(&owner, 0, sizeof(owner));
    CHECK(appliance_production_management_api_publish(&owner, &evidence) ==
              APPLIANCE_PRODUCTION_MANAGEMENT_API_OK,
          "the ready owner publishes for route resolution");
    CHECK(appliance_production_management_api_owns(
              &owner, (const uint8_t *)"/api/v1/system", 14U),
          "the system target is owned");
    CHECK(appliance_production_management_api_owns(
              &owner, (const uint8_t *)"/api/v1/network", 15U),
          "the network configuration target is owned");
    CHECK(appliance_production_management_api_owns(
              &owner, (const uint8_t *)"/api/v1/network/status", 22U),
          "the network status target is owned");
    CHECK(appliance_production_management_api_owns(
              &owner, (const uint8_t *)"/api/v1/service", 15U),
          "the service target is owned");
    CHECK(appliance_production_management_api_owns(
              &owner, (const uint8_t *)"/api/v1/health", 14U),
          "the health target is owned");
    CHECK(!appliance_production_management_api_owns(
              &owner, (const uint8_t *)"/api/v1", 7U),
          "the discovery target stays with the management API module");
    CHECK(!appliance_production_management_api_owns(
              &owner, (const uint8_t *)"/api/v1/not-a-route", 19U),
          "an unknown target is not owned");
    CHECK(!appliance_production_management_api_owns(
              &owner, (const uint8_t *)"/panel", 6U),
          "a panel target is not owned");

    CHECK(dispatch(&owner, HTTP_METHOD_GET, "/api/v1/not-a-route",
                   &result) ==
              APPLIANCE_PRODUCTION_MANAGEMENT_API_NOT_OWNED,
          "dispatching an unknown target reports NOT_OWNED");
    CHECK(dispatch(&owner, HTTP_METHOD_NONE, "/api/v1/system", &result) ==
              APPLIANCE_PRODUCTION_MANAGEMENT_API_METHOD_NOT_ALLOWED &&
              result.response.status == HTTP_STATUS_405_METHOD_NOT_ALLOWED,
          "an unsupported method on the system target is refused with 405");
}

static void test_ready_bodies(void)
{
    struct appliance_production_management_api owner;
    struct appliance_production_management_api_evidence evidence;
    struct appliance_production_management_api_result result;

    evidence_ready(&evidence, true);
    memset(&owner, 0, sizeof(owner));
    CHECK(appliance_production_management_api_publish(&owner, &evidence) ==
              APPLIANCE_PRODUCTION_MANAGEMENT_API_OK,
          "the ready owner publishes for the body proof");

    CHECK(dispatch(&owner, HTTP_METHOD_GET, "/api/v1/system", &result) ==
              APPLIANCE_PRODUCTION_MANAGEMENT_API_OK &&
              result.response.status == HTTP_STATUS_200_OK,
          "the system target is served");
    CHECK(body_contains(&result, "\"name\":\"system\"") &&
              body_contains(&result, "\"config_state\":\"ok\"") &&
              body_contains(&result, "\"config_hostname\":\"node-390\"") &&
              body_contains(&result, "\"health\":\"ok\""),
          "the system body carries identity, committed config and OK health");

    CHECK(dispatch(&owner, HTTP_METHOD_HEAD, "/api/v1/system", &result) ==
              APPLIANCE_PRODUCTION_MANAGEMENT_API_OK &&
              result.response.status == HTTP_STATUS_200_OK,
          "the system target answers HEAD");

    CHECK(dispatch(&owner, HTTP_METHOD_GET, "/api/v1/network", &result) ==
              APPLIANCE_PRODUCTION_MANAGEMENT_API_OK &&
              body_contains(&result, "\"name\":\"network\"") &&
              body_contains(&result, "\"config_state\":\"ok\""),
          "the network configuration body is served from the desired config");

    CHECK(dispatch(&owner, HTTP_METHOD_GET, "/api/v1/network/status",
                   &result) == APPLIANCE_PRODUCTION_MANAGEMENT_API_OK &&
              body_contains(&result, "\"name\":\"network-status\"") &&
              body_contains(&result, "\"link\":\"up\""),
          "the network status body carries the published link diagnostics");

    CHECK(dispatch(&owner, HTTP_METHOD_GET, "/api/v1/service", &result) ==
              APPLIANCE_PRODUCTION_MANAGEMENT_API_OK &&
              body_contains(&result, "\"name\":\"service\"") &&
              body_contains(&result, "\"state\":\"running\"") &&
              body_contains(&result, "\"health\":\"pass\""),
          "the service body carries the running fixed service");

    CHECK(dispatch(&owner, HTTP_METHOD_GET, "/api/v1/health", &result) ==
              APPLIANCE_PRODUCTION_MANAGEMENT_API_OK &&
              body_contains(&result, "\"name\":\"health\"") &&
              body_contains(&result, "\"state\":\"OK\"") &&
              body_contains(&result, "\"healthy\":true"),
          "the health body carries the aggregate PASS");

    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "the snapshot owner performs no heap allocation");
}

static void test_honest_degraded(void)
{
    struct appliance_production_management_api owner;
    struct appliance_production_management_api_evidence evidence;
    struct appliance_production_management_api_result result;
    struct appliance_network_config desired_dhcp;

    memset(&desired_dhcp, 0, sizeof(desired_dhcp));
    (void)appliance_network_config_publish_default(&desired_dhcp);

    evidence_ready(&evidence, false);
    evidence.boot_readiness = APPLIANCE_PRODUCTION_BOOT_READINESS_DEGRADED;
    evidence.boot_mode = APPLIANCE_PRODUCTION_BOOT_MODE_DEGRADED;
    evidence.config_source =
        APPLIANCE_CONFIG_VALIDATION_SOURCE_DEFAULT_MISSING;
    evidence.network_desired = &desired_dhcp;
    evidence.network_config_source =
        APPLIANCE_NETWORK_API_CONFIG_SOURCE_DEFAULT_MISSING;
    evidence.network_active = NULL;
    evidence.network_link_up = true;
    evidence.gateway_reachable = false;
    evidence.dns_reachable = false;
    evidence.https_listening = false;
    evidence.service_state =
        APPLIANCE_PRODUCTION_SERVICE_ACTIVATION_STATE_AWAITING_NETWORK;

    memset(&owner, 0, sizeof(owner));
    CHECK(appliance_production_management_api_publish(&owner, &evidence) ==
              APPLIANCE_PRODUCTION_MANAGEMENT_API_OK,
          "degraded evidence still publishes an honest owner");

    CHECK(dispatch(&owner, HTTP_METHOD_GET, "/api/v1/network", &result) ==
              APPLIANCE_PRODUCTION_MANAGEMENT_API_OK &&
              body_contains(&result, "\"config_state\":\"ok\"") &&
              body_contains(&result, "\"config_source\":\"default_missing\""),
          "a default network configuration is published as a default");

    CHECK(dispatch(&owner, HTTP_METHOD_GET, "/api/v1/network/status",
                   &result) == APPLIANCE_PRODUCTION_MANAGEMENT_API_OK &&
              body_contains(&result, "\"diagnostics\":\"available\"") &&
              body_contains(&result, "\"health\":\"warn\"") &&
              body_contains(&result, "\"fault\":\"address\""),
          "an addressless DHCP network degrades the link diagnostics");

    CHECK(dispatch(&owner, HTTP_METHOD_GET, "/api/v1/service", &result) ==
              APPLIANCE_PRODUCTION_MANAGEMENT_API_OK &&
              body_contains(&result, "\"health\":\"warn\"") &&
              body_contains(&result, "\"fault\":\"dependency\""),
          "an awaiting service is published as a warning, never as a pass");

    CHECK(dispatch(&owner, HTTP_METHOD_GET, "/api/v1/health", &result) ==
              APPLIANCE_PRODUCTION_MANAGEMENT_API_OK &&
              body_contains(&result, "\"state\":\"DEGRADED\"") &&
              body_contains(&result, "\"healthy\":false"),
          "the aggregate health is DEGRADED, not a fabricated PASS");

    CHECK(dispatch(&owner, HTTP_METHOD_GET, "/api/v1/system", &result) ==
              APPLIANCE_PRODUCTION_MANAGEMENT_API_OK &&
              body_contains(&result, "\"config_state\":\"degraded\"") &&
              body_contains(&result, "\"health\":\"degraded\"") &&
              body_contains(&result, "\"recovery_mode\":1"),
          "the system body publishes the degraded configuration and health");

    /*
     * The boot vocabulary numbers FAIL_CLOSED=0 and NORMAL=2, the system
     * vocabulary NORMAL=0 and FAIL_CLOSED=2; the two must not be cast.
     */
    evidence.boot_readiness =
        APPLIANCE_PRODUCTION_BOOT_READINESS_UNAVAILABLE;
    evidence.boot_mode = APPLIANCE_PRODUCTION_BOOT_MODE_FAIL_CLOSED;
    memset(&owner, 0, sizeof(owner));
    CHECK(appliance_production_management_api_publish(&owner, &evidence) ==
              APPLIANCE_PRODUCTION_MANAGEMENT_API_OK,
          "fail-closed evidence publishes");
    CHECK(dispatch(&owner, HTTP_METHOD_GET, "/api/v1/system", &result) ==
              APPLIANCE_PRODUCTION_MANAGEMENT_API_OK &&
              body_contains(&result, "\"recovery_mode\":2") &&
              body_contains(&result, "\"health\":\"failed\""),
          "a fail-closed boot publishes recovery mode 2 and FAILED health");
}

static void test_unavailable_network(void)
{
    struct appliance_production_management_api owner;
    struct appliance_production_management_api_evidence evidence;
    struct appliance_production_management_api_result result;

    evidence_ready(&evidence, false);
    evidence.network_device = NULL;
    evidence.network_link_up = false;
    evidence.gateway_reachable = false;
    evidence.dns_reachable = false;
    memset(&owner, 0, sizeof(owner));
    CHECK(appliance_production_management_api_publish(&owner, &evidence) ==
              APPLIANCE_PRODUCTION_MANAGEMENT_API_OK,
          "an appliance without a NIC still publishes honest evidence");
    CHECK(dispatch(&owner, HTTP_METHOD_GET, "/api/v1/network", &result) ==
              APPLIANCE_PRODUCTION_MANAGEMENT_API_OK &&
              body_contains(&result, "\"config_state\":\"unavailable\""),
          "an absent network configuration is published as unavailable");
    CHECK(dispatch(&owner, HTTP_METHOD_GET, "/api/v1/network/status",
                   &result) == APPLIANCE_PRODUCTION_MANAGEMENT_API_OK &&
              body_contains(&result, "\"name\":\"network-status\""),
          "the network status target is still resolved without a NIC");
}

int main(void)
{
    CHECK(appliance_identity_boot_init() == APPLIANCE_IDENTITY_OK,
          "the ordinary-boot identity publishes");
    publish_static_config();
    publish_test_device();

    test_publish_validation();
    test_route_resolution();
    test_ready_bodies();
    test_honest_degraded();
    test_unavailable_network();

    if (failures != 0) {
        fprintf(stderr, "Appliance production management API host tests: %d FAILURES\n",
                failures);
        return 1;
    }
    printf("Appliance production management API host tests: ALL OK\n");
    return 0;
}
