#include "appliance_network_api.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifdef HFOC_APPLIANCE_NETWORK_API_HOST_TEST

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

static const uint8_t method_body[] =
    "{\"api\":\"hfoc\",\"version\":1,\"name\":\"network\","
    "\"error\":\"method_not_allowed\",\"auth\":\"unavailable\","
    "\"mutating\":false}";
static const uint8_t unavailable_config_body[] =
    "{\"api\":\"hfoc\",\"version\":1,\"name\":\"network\","
    "\"status\":\"ok\",\"auth\":\"unavailable\",\"mutating\":false,"
    "\"config_state\":\"unavailable\",\"config_source\":\"none\"}";
static const uint8_t static_config_body[] =
    "{\"api\":\"hfoc\",\"version\":1,\"name\":\"network\","
    "\"status\":\"ok\",\"auth\":\"unavailable\",\"mutating\":false,"
    "\"config_state\":\"ok\",\"config_source\":\"committed\","
    "\"interface\":\"eth0\",\"mode\":\"static\","
    "\"ipv4_address\":\"10.0.2.15\",\"ipv4_prefix\":24,"
    "\"ipv4_gateway\":\"10.0.2.2\",\"ipv4_dns\":\"10.0.2.3\"}";
static const uint8_t dhcp_config_body[] =
    "{\"api\":\"hfoc\",\"version\":1,\"name\":\"network\","
    "\"status\":\"ok\",\"auth\":\"unavailable\",\"mutating\":false,"
    "\"config_state\":\"ok\",\"config_source\":\"committed\","
    "\"interface\":\"eth0\",\"mode\":\"dhcp\"}";
static const uint8_t unavailable_status_body[] =
    "{\"api\":\"hfoc\",\"version\":1,\"name\":\"network-status\","
    "\"status\":\"ok\",\"auth\":\"unavailable\",\"mutating\":false,"
    "\"config_state\":\"unavailable\",\"diagnostics\":\"unavailable\","
    "\"interface\":\"none\",\"mode\":\"none\",\"active\":false,"
    "\"ipv4_address\":\"none\",\"ipv4_prefix\":0,"
    "\"ipv4_gateway\":\"none\",\"ipv4_dns\":\"none\","
    "\"device_id\":0,\"link\":\"none\","
    "\"gateway_reachability\":\"unknown\","
    "\"dns_reachability\":\"unknown\",\"health\":\"unavailable\","
    "\"fault\":\"none\"}";
static const uint8_t static_status_body[] =
    "{\"api\":\"hfoc\",\"version\":1,\"name\":\"network-status\","
    "\"status\":\"ok\",\"auth\":\"unavailable\",\"mutating\":false,"
    "\"config_state\":\"ok\",\"diagnostics\":\"available\","
    "\"interface\":\"eth0\",\"mode\":\"static\",\"active\":true,"
    "\"ipv4_address\":\"10.0.2.15\",\"ipv4_prefix\":24,"
    "\"ipv4_gateway\":\"10.0.2.2\",\"ipv4_dns\":\"10.0.2.3\","
    "\"device_id\":1,\"link\":\"up\","
    "\"gateway_reachability\":\"reachable\","
    "\"dns_reachability\":\"reachable\",\"health\":\"pass\","
    "\"fault\":\"none\"}";
static const uint8_t no_diagnostics_status_body[] =
    "{\"api\":\"hfoc\",\"version\":1,\"name\":\"network-status\","
    "\"status\":\"ok\",\"auth\":\"unavailable\",\"mutating\":false,"
    "\"config_state\":\"ok\",\"diagnostics\":\"unavailable\","
    "\"interface\":\"eth0\",\"mode\":\"dhcp\",\"active\":false,"
    "\"ipv4_address\":\"none\",\"ipv4_prefix\":0,"
    "\"ipv4_gateway\":\"none\",\"ipv4_dns\":\"none\","
    "\"device_id\":0,\"link\":\"none\","
    "\"gateway_reachability\":\"unknown\","
    "\"dns_reachability\":\"unknown\",\"health\":\"unavailable\","
    "\"fault\":\"none\"}";

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

static struct appliance_network_config make_desired(
    enum appliance_network_config_mode mode)
{
    struct appliance_network_config desired;
    struct network_ipv4_config active = make_active();

    memset(&desired, 0, sizeof(desired));
    if (mode == APPLIANCE_NETWORK_CONFIG_MODE_STATIC) {
        if (appliance_network_config_publish_static(
                &desired, "eth0", 4U, &active) !=
            APPLIANCE_NETWORK_CONFIG_OK) {
            fprintf(stderr, "fixture failed static desired\n");
            __builtin_unreachable();
        }
    } else {
        if (appliance_network_config_publish_dhcp(
                &desired, "eth0", 4U) !=
            APPLIANCE_NETWORK_CONFIG_OK) {
            fprintf(stderr, "fixture failed DHCP desired\n");
            __builtin_unreachable();
        }
    }
    return desired;
}

static struct network_link_state_record make_link(void)
{
    struct network_link_state_record link;

    memset(&link, 0, sizeof(link));
    link.version = NETWORK_LINK_STATE_VERSION;
    link.device_id = 1U;
    link.state = NETWORK_LINK_UP;
    link.generation = 7U;
    return link;
}

static struct appliance_network_diagnostics make_diagnostics(
    const struct appliance_network_config *desired)
{
    struct appliance_network_diagnostics diagnostics;
    struct network_link_state_record link = make_link();
    struct network_ipv4_config active = make_active();

    memset(&diagnostics, 0, sizeof(diagnostics));
    if (appliance_network_diagnostics_publish(
            &diagnostics,
            desired,
            &link,
            &active,
            APPLIANCE_NETWORK_REACHABILITY_REACHABLE,
            APPLIANCE_NETWORK_REACHABILITY_REACHABLE) !=
        APPLIANCE_NETWORK_DIAGNOSTICS_OK) {
        fprintf(stderr, "fixture failed diagnostics publish\n");
        __builtin_unreachable();
    }
    return diagnostics;
}

static struct appliance_network_api_snapshot make_snapshot(
    bool available,
    bool diagnostics_present)
{
    struct appliance_network_api_snapshot snapshot;
    struct appliance_network_config desired =
        make_desired(APPLIANCE_NETWORK_CONFIG_MODE_STATIC);
    struct appliance_network_config dhcp =
        make_desired(APPLIANCE_NETWORK_CONFIG_MODE_DHCP);
    struct appliance_network_diagnostics diagnostics =
        make_diagnostics(&desired);

    memset(&snapshot, 0, sizeof(snapshot));
    if (!available) {
        if (appliance_network_api_snapshot_init(
                &snapshot,
                NULL,
                APPLIANCE_NETWORK_API_CONFIG_SOURCE_NONE,
                NULL) != APPLIANCE_NETWORK_API_OK) {
            fprintf(stderr, "fixture failed unavailable snapshot\n");
            __builtin_unreachable();
        }
        return snapshot;
    }
    if (diagnostics_present) {
        if (appliance_network_api_snapshot_init(
                &snapshot,
                &desired,
                APPLIANCE_NETWORK_API_CONFIG_SOURCE_COMMITTED,
                &diagnostics) != APPLIANCE_NETWORK_API_OK) {
            fprintf(stderr, "fixture failed diagnostics snapshot\n");
            __builtin_unreachable();
        }
        return snapshot;
    }
    if (appliance_network_api_snapshot_init(
            &snapshot,
            &dhcp,
            APPLIANCE_NETWORK_API_CONFIG_SOURCE_COMMITTED,
            NULL) != APPLIANCE_NETWORK_API_OK) {
        fprintf(stderr, "fixture failed config-only snapshot\n");
        __builtin_unreachable();
    }
    return snapshot;
}

static bool body_is(
    const struct appliance_network_api_result *result,
    const uint8_t *expected,
    size_t expected_length)
{
    return result->published &&
           result->response.body != NULL &&
           result->response.body_length == expected_length &&
           memcmp(result->response.body, expected, expected_length) == 0;
}

static void test_strings(void)
{
    enum appliance_network_api_status status;
    enum appliance_network_api_route route;

    for (status = APPLIANCE_NETWORK_API_OK;
         status <= APPLIANCE_NETWORK_API_OVERFLOW; ++status) {
        const char *string = appliance_network_api_status_string(status);

        CHECK(string != NULL &&
                  (status == APPLIANCE_NETWORK_API_OK ?
                       strcmp(string,
                              "APPLIANCE_NETWORK_API_OK") == 0 :
                   status == APPLIANCE_NETWORK_API_NULL ?
                       strcmp(string,
                              "APPLIANCE_NETWORK_API_NULL") == 0 :
                   status == APPLIANCE_NETWORK_API_NOT_NETWORK ?
                       strcmp(string,
                              "APPLIANCE_NETWORK_API_NOT_NETWORK") == 0 :
                   status == APPLIANCE_NETWORK_API_METHOD_NOT_ALLOWED ?
                       strcmp(string,
                              "APPLIANCE_NETWORK_API_METHOD_NOT_ALLOWED") == 0 :
                   status == APPLIANCE_NETWORK_API_BAD_STATE ?
                       strcmp(string,
                              "APPLIANCE_NETWORK_API_BAD_STATE") == 0 :
                   status == APPLIANCE_NETWORK_API_BAD_VERSION ?
                       strcmp(string,
                              "APPLIANCE_NETWORK_API_BAD_VERSION") == 0 :
                   status == APPLIANCE_NETWORK_API_BAD_CONFIG ?
                       strcmp(string,
                              "APPLIANCE_NETWORK_API_BAD_CONFIG") == 0 :
                   status == APPLIANCE_NETWORK_API_BAD_DIAGNOSTICS ?
                       strcmp(string,
                              "APPLIANCE_NETWORK_API_BAD_DIAGNOSTICS") == 0 :
                       strcmp(string,
                              "APPLIANCE_NETWORK_API_OVERFLOW") == 0),
              "network API status string is exact for every status");
    }
    CHECK(appliance_network_api_status_string(
              (enum appliance_network_api_status)9U) == NULL,
          "network API rejects undefined status string");
    for (route = APPLIANCE_NETWORK_API_ROUTE_NONE;
         route <= APPLIANCE_NETWORK_API_ROUTE_STATUS; ++route) {
        const char *string = appliance_network_api_route_string(route);

        CHECK(string != NULL &&
                  (route == APPLIANCE_NETWORK_API_ROUTE_NONE ?
                       strcmp(string,
                              "APPLIANCE_NETWORK_API_ROUTE_NONE") == 0 :
                   route == APPLIANCE_NETWORK_API_ROUTE_CONFIG ?
                       strcmp(string,
                              "APPLIANCE_NETWORK_API_ROUTE_CONFIG") == 0 :
                       strcmp(string,
                              "APPLIANCE_NETWORK_API_ROUTE_STATUS") == 0),
              "network API route string is exact for every route");
    }
    CHECK(appliance_network_api_route_string(
              (enum appliance_network_api_route)3U) == NULL,
          "network API rejects undefined route string");
    CHECK(strcmp(appliance_network_api_config_state_string(
                     APPLIANCE_NETWORK_API_CONFIG_UNAVAILABLE),
                 "unavailable") == 0 &&
              strcmp(appliance_network_api_config_state_string(
                         APPLIANCE_NETWORK_API_CONFIG_OK),
                     "ok") == 0,
          "network API config state string is exact");
    CHECK(appliance_network_api_config_state_string(
              (enum appliance_network_api_config_state)2U) == NULL,
          "network API rejects undefined config state string");
    CHECK(strcmp(appliance_network_api_config_source_string(
                     APPLIANCE_NETWORK_API_CONFIG_SOURCE_NONE),
                 "none") == 0 &&
              strcmp(appliance_network_api_config_source_string(
                         APPLIANCE_NETWORK_API_CONFIG_SOURCE_COMMITTED),
                     "committed") == 0 &&
              strcmp(appliance_network_api_config_source_string(
                         APPLIANCE_NETWORK_API_CONFIG_SOURCE_DEFAULT_MISSING),
                     "default_missing") == 0 &&
              strcmp(appliance_network_api_config_source_string(
                         APPLIANCE_NETWORK_API_CONFIG_SOURCE_DEFAULT_INVALID),
                     "default_invalid") == 0,
          "network API config source string is exact");
    CHECK(appliance_network_api_config_source_string(
              (enum appliance_network_api_config_source)4U) == NULL,
          "network API rejects undefined config source string");
}

static void test_snapshot_init_validation(void)
{
    struct appliance_network_config desired =
        make_desired(APPLIANCE_NETWORK_CONFIG_MODE_STATIC);
    struct appliance_network_config dhcp =
        make_desired(APPLIANCE_NETWORK_CONFIG_MODE_DHCP);
    struct appliance_network_diagnostics diagnostics =
        make_diagnostics(&desired);
    struct appliance_network_diagnostics bad = diagnostics;
    struct appliance_network_api_snapshot snapshot;

    memset(&snapshot, 0, sizeof(snapshot));
    CHECK(appliance_network_api_snapshot_init(
              NULL, &desired,
              APPLIANCE_NETWORK_API_CONFIG_SOURCE_COMMITTED,
              &diagnostics) == APPLIANCE_NETWORK_API_NULL,
          "network snapshot init rejects null snapshot");
    CHECK(appliance_network_api_snapshot_init(
              &snapshot, NULL,
              APPLIANCE_NETWORK_API_CONFIG_SOURCE_NONE,
              &diagnostics) == APPLIANCE_NETWORK_API_BAD_CONFIG,
          "network snapshot init rejects diagnostics with unavailable config");
    CHECK(appliance_network_api_snapshot_init(
              &snapshot, &desired,
              APPLIANCE_NETWORK_API_CONFIG_SOURCE_NONE,
              NULL) == APPLIANCE_NETWORK_API_BAD_CONFIG,
          "network snapshot init rejects available config without source");
    CHECK(appliance_network_api_snapshot_init(
              &snapshot, &dhcp,
              APPLIANCE_NETWORK_API_CONFIG_SOURCE_COMMITTED,
              &diagnostics) == APPLIANCE_NETWORK_API_BAD_DIAGNOSTICS,
          "network snapshot init rejects diagnostics interface mismatch");

    bad.version = 0U;
    CHECK(appliance_network_api_snapshot_init(
              &snapshot, &desired,
              APPLIANCE_NETWORK_API_CONFIG_SOURCE_COMMITTED,
              &bad) == APPLIANCE_NETWORK_API_BAD_DIAGNOSTICS,
          "network snapshot init rejects bad diagnostics version");
    bad = diagnostics;
    bad.device_id = 0U;
    CHECK(appliance_network_api_snapshot_init(
              &snapshot, &desired,
              APPLIANCE_NETWORK_API_CONFIG_SOURCE_COMMITTED,
              &bad) == APPLIANCE_NETWORK_API_BAD_DIAGNOSTICS,
          "network snapshot init rejects bad diagnostics device id");

    memset(&snapshot, 0, sizeof(snapshot));
    CHECK(appliance_network_api_snapshot_init(
              &snapshot, NULL,
              APPLIANCE_NETWORK_API_CONFIG_SOURCE_NONE,
              NULL) == APPLIANCE_NETWORK_API_OK &&
              snapshot.published &&
              snapshot.config_state ==
                  APPLIANCE_NETWORK_API_CONFIG_UNAVAILABLE,
          "network snapshot init publishes unavailable state");
    CHECK(appliance_network_api_snapshot_init(
              &snapshot, NULL,
              APPLIANCE_NETWORK_API_CONFIG_SOURCE_NONE,
              NULL) == APPLIANCE_NETWORK_API_BAD_STATE,
          "network snapshot init rejects already published snapshot");

    memset(&snapshot, 0, sizeof(snapshot));
    CHECK(appliance_network_api_snapshot_init(
              &snapshot, &desired,
              APPLIANCE_NETWORK_API_CONFIG_SOURCE_COMMITTED,
              NULL) == APPLIANCE_NETWORK_API_OK &&
              snapshot.config_state == APPLIANCE_NETWORK_API_CONFIG_OK,
          "network snapshot init publishes config-only state");
}

static void test_dispatch_semantics(void)
{
    struct appliance_network_api_snapshot unavailable =
        make_snapshot(false, false);
    struct appliance_network_api_snapshot available =
        make_snapshot(true, true);
    struct appliance_network_api_snapshot config_only =
        make_snapshot(true, false);
    struct appliance_network_api_result result;
    struct appliance_network_api_result original;
    uint8_t output[HTTP_RESPONSE_BUFFER_MAX];
    uint32_t written = 0U;

    memset(&result, 0, sizeof(result));
    CHECK(appliance_network_api_dispatch(
              HTTP_METHOD_GET, NULL, 1U, &available, &result) ==
              APPLIANCE_NETWORK_API_NULL,
          "network API dispatch rejects null target");
    CHECK(appliance_network_api_dispatch(
              HTTP_METHOD_GET,
              (const uint8_t *)APPLIANCE_NETWORK_API_CONFIG_PATH,
              APPLIANCE_NETWORK_API_CONFIG_PATH_LENGTH,
              NULL, &result) == APPLIANCE_NETWORK_API_NULL,
          "network API dispatch rejects null snapshot");
    CHECK(appliance_network_api_dispatch(
              HTTP_METHOD_GET,
              (const uint8_t *)APPLIANCE_NETWORK_API_CONFIG_PATH,
              APPLIANCE_NETWORK_API_CONFIG_PATH_LENGTH,
              &available, NULL) == APPLIANCE_NETWORK_API_NULL,
          "network API dispatch rejects null result");

    memset(&result, 0, sizeof(result));
    original = result;
    CHECK(appliance_network_api_dispatch(
              HTTP_METHOD_GET,
              (const uint8_t *)"/api/v1",
              7U, &available, &result) ==
              APPLIANCE_NETWORK_API_NOT_NETWORK,
          "version route is not network API");
    CHECK(memcmp(&result, &original, sizeof(result)) == 0,
          "non-network dispatch leaves result unchanged");
    CHECK(appliance_network_api_dispatch(
              HTTP_METHOD_GET,
              (const uint8_t *)"/api/v1/network/child",
              22U, &available, &result) ==
              APPLIANCE_NETWORK_API_NOT_NETWORK,
          "unknown network child route is not owned in this boundary");

    memset(&result, 0, sizeof(result));
    CHECK(appliance_network_api_dispatch(
              HTTP_METHOD_GET,
              (const uint8_t *)APPLIANCE_NETWORK_API_CONFIG_PATH,
              APPLIANCE_NETWORK_API_CONFIG_PATH_LENGTH,
              &available, &result) == APPLIANCE_NETWORK_API_OK &&
              result.route == APPLIANCE_NETWORK_API_ROUTE_CONFIG &&
              body_is(&result, static_config_body,
                      sizeof(static_config_body) - 1U) &&
              result.response.status == HTTP_STATUS_200_OK &&
              result.response.content_type == HTTP_CONTENT_TYPE_JSON,
          "GET network config publishes exact static body");
    CHECK(http_response_encode(
              &result.response, output, sizeof(output), &written) ==
              HTTP_RESPONSE_OK &&
              written > 0U &&
              memcmp(output, "HTTP/1.1 200 OK\r\n", 17U) == 0,
          "network config GET HTTP status is exact");

    memset(&result, 0, sizeof(result));
    CHECK(appliance_network_api_dispatch(
              HTTP_METHOD_HEAD,
              (const uint8_t *)APPLIANCE_NETWORK_API_CONFIG_PATH,
              APPLIANCE_NETWORK_API_CONFIG_PATH_LENGTH,
              &available, &result) == APPLIANCE_NETWORK_API_OK &&
              result.response.status == HTTP_STATUS_200_OK &&
              result.response.body_length ==
                  (uint32_t)(sizeof(static_config_body) - 1U),
          "HEAD network config publishes same bounded body");

    memset(&result, 0, sizeof(result));
    CHECK(appliance_network_api_dispatch(
              HTTP_METHOD_GET,
              (const uint8_t *)APPLIANCE_NETWORK_API_STATUS_PATH,
              APPLIANCE_NETWORK_API_STATUS_PATH_LENGTH,
              &available, &result) == APPLIANCE_NETWORK_API_OK &&
              result.route == APPLIANCE_NETWORK_API_ROUTE_STATUS &&
              body_is(&result, static_status_body,
                      sizeof(static_status_body) - 1U),
          "GET network status publishes exact static body");

    memset(&result, 0, sizeof(result));
    CHECK(appliance_network_api_dispatch(
              HTTP_METHOD_HEAD,
              (const uint8_t *)APPLIANCE_NETWORK_API_STATUS_PATH,
              APPLIANCE_NETWORK_API_STATUS_PATH_LENGTH,
              &available, &result) == APPLIANCE_NETWORK_API_OK &&
              result.response.status == HTTP_STATUS_200_OK &&
              result.response.body_length ==
                  (uint32_t)(sizeof(static_status_body) - 1U),
          "HEAD network status publishes same bounded body");

    memset(&result, 0, sizeof(result));
    CHECK(appliance_network_api_dispatch(
              HTTP_METHOD_UNSUPPORTED,
              (const uint8_t *)APPLIANCE_NETWORK_API_CONFIG_PATH,
              APPLIANCE_NETWORK_API_CONFIG_PATH_LENGTH,
              &available, &result) ==
              APPLIANCE_NETWORK_API_METHOD_NOT_ALLOWED &&
              result.route == APPLIANCE_NETWORK_API_ROUTE_CONFIG &&
              body_is(&result, method_body,
                      sizeof(method_body) - 1U) &&
              result.response.status ==
                  HTTP_STATUS_405_METHOD_NOT_ALLOWED,
          "unsupported method on network config returns API 405");

    memset(&result, 0, sizeof(result));
    CHECK(appliance_network_api_dispatch(
              HTTP_METHOD_GET,
              (const uint8_t *)APPLIANCE_NETWORK_API_CONFIG_PATH,
              APPLIANCE_NETWORK_API_CONFIG_PATH_LENGTH,
              &unavailable, &result) == APPLIANCE_NETWORK_API_OK &&
              body_is(&result, unavailable_config_body,
                      sizeof(unavailable_config_body) - 1U),
          "unavailable network config publishes exact unavailable body");

    memset(&result, 0, sizeof(result));
    CHECK(appliance_network_api_dispatch(
              HTTP_METHOD_GET,
              (const uint8_t *)APPLIANCE_NETWORK_API_STATUS_PATH,
              APPLIANCE_NETWORK_API_STATUS_PATH_LENGTH,
              &unavailable, &result) == APPLIANCE_NETWORK_API_OK &&
              body_is(&result, unavailable_status_body,
                      sizeof(unavailable_status_body) - 1U),
          "unavailable network status publishes exact unavailable body");

    memset(&result, 0, sizeof(result));
    CHECK(appliance_network_api_dispatch(
              HTTP_METHOD_GET,
              (const uint8_t *)APPLIANCE_NETWORK_API_STATUS_PATH,
              APPLIANCE_NETWORK_API_STATUS_PATH_LENGTH,
              &config_only, &result) == APPLIANCE_NETWORK_API_OK &&
              body_is(&result, no_diagnostics_status_body,
                      sizeof(no_diagnostics_status_body) - 1U),
          "config-only network status publishes exact no-diagnostics body");

    memset(&result, 0, sizeof(result));
    CHECK(appliance_network_api_dispatch(
              HTTP_METHOD_GET,
              (const uint8_t *)APPLIANCE_NETWORK_API_CONFIG_PATH,
              APPLIANCE_NETWORK_API_CONFIG_PATH_LENGTH,
              &config_only, &result) == APPLIANCE_NETWORK_API_OK &&
              body_is(&result, dhcp_config_body,
                      sizeof(dhcp_config_body) - 1U),
          "DHCP network config publishes exact DHCP body");
}

static void test_determinism_and_bounds(void)
{
    struct appliance_network_api_snapshot available =
        make_snapshot(true, true);
    struct appliance_network_api_result first;
    struct appliance_network_api_result second;

    memset(&first, 0, sizeof(first));
    memset(&second, 0, sizeof(second));
    CHECK(appliance_network_api_dispatch(
              HTTP_METHOD_GET,
              (const uint8_t *)APPLIANCE_NETWORK_API_STATUS_PATH,
              APPLIANCE_NETWORK_API_STATUS_PATH_LENGTH,
              &available, &first) == APPLIANCE_NETWORK_API_OK &&
              appliance_network_api_dispatch(
                  HTTP_METHOD_GET,
                  (const uint8_t *)APPLIANCE_NETWORK_API_STATUS_PATH,
                  APPLIANCE_NETWORK_API_STATUS_PATH_LENGTH,
                  &available, &second) == APPLIANCE_NETWORK_API_OK &&
              first.response.body_length ==
                  second.response.body_length &&
              memcmp(first.body, second.body,
                     sizeof(static_status_body) - 1U) == 0,
          "network API repeat dispatch is byte-identical");
    CHECK(first.response.body_length <= APPLIANCE_NETWORK_API_BODY_MAX &&
              second.response.body_length <= APPLIANCE_NETWORK_API_BODY_MAX,
          "network API bodies are bounded");
}

int main(void)
{
    test_strings();
    test_snapshot_init_validation();
    test_dispatch_semantics();
    test_determinism_and_bounds();
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "network API performs no allocation");
    if (failures != 0) {
        fprintf(stderr, "Appliance network API host failures: %d\n",
                failures);
        return 1;
    }
    puts("Appliance network API host test: ALL OK");
    return 0;
}

#endif
