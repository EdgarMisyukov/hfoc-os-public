#include "appliance_management_authorization.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifdef APPLIANCE_MANAGEMENT_AUTHORIZATION_HOST_TEST

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

static struct appliance_admin_auth_decision make_decision(
    bool authenticated,
    uint32_t principal_id,
    uint32_t session_id,
    uint32_t credential_generation)
{
    struct appliance_admin_auth_decision decision;

    memset(&decision, 0, sizeof(decision));
    decision.authenticated = authenticated;
    decision.principal_id = principal_id;
    decision.session_id = session_id;
    decision.credential_generation = credential_generation;
    return decision;
}

static void init_policy(
    struct appliance_management_authorization_policy *policy)
{
    memset(policy, 0, sizeof(*policy));
    CHECK(appliance_management_authorization_policy_init(policy) ==
              APPLIANCE_MANAGEMENT_AUTHORIZATION_OK,
          "management authorization policy initializes");
}

static const char *expected_status_string(
    enum appliance_management_authorization_status status)
{
    switch (status) {
    case APPLIANCE_MANAGEMENT_AUTHORIZATION_OK:
        return "APPLIANCE_MANAGEMENT_AUTHORIZATION_OK";
    case APPLIANCE_MANAGEMENT_AUTHORIZATION_NULL:
        return "APPLIANCE_MANAGEMENT_AUTHORIZATION_NULL";
    case APPLIANCE_MANAGEMENT_AUTHORIZATION_BAD_STATE:
        return "APPLIANCE_MANAGEMENT_AUTHORIZATION_BAD_STATE";
    case APPLIANCE_MANAGEMENT_AUTHORIZATION_BAD_VERSION:
        return "APPLIANCE_MANAGEMENT_AUTHORIZATION_BAD_VERSION";
    case APPLIANCE_MANAGEMENT_AUTHORIZATION_NOT_MANAGEMENT:
        return "APPLIANCE_MANAGEMENT_AUTHORIZATION_NOT_MANAGEMENT";
    case APPLIANCE_MANAGEMENT_AUTHORIZATION_UNKNOWN_ROUTE:
        return "APPLIANCE_MANAGEMENT_AUTHORIZATION_UNKNOWN_ROUTE";
    case APPLIANCE_MANAGEMENT_AUTHORIZATION_METHOD_NOT_ALLOWED:
        return "APPLIANCE_MANAGEMENT_AUTHORIZATION_METHOD_NOT_ALLOWED";
    case APPLIANCE_MANAGEMENT_AUTHORIZATION_UNAUTHENTICATED:
        return "APPLIANCE_MANAGEMENT_AUTHORIZATION_UNAUTHENTICATED";
    case APPLIANCE_MANAGEMENT_AUTHORIZATION_DENIED_ROLE:
        return "APPLIANCE_MANAGEMENT_AUTHORIZATION_DENIED_ROLE";
    case APPLIANCE_MANAGEMENT_AUTHORIZATION_DENIED_CAPABILITY:
        return "APPLIANCE_MANAGEMENT_AUTHORIZATION_DENIED_CAPABILITY";
    case APPLIANCE_MANAGEMENT_AUTHORIZATION_BAD_AUTHORIZATION:
        return "APPLIANCE_MANAGEMENT_AUTHORIZATION_BAD_AUTHORIZATION";
    default:
        return NULL;
    }
}

static const char *expected_route_string(
    enum appliance_management_authorization_route route)
{
    switch (route) {
    case APPLIANCE_MANAGEMENT_AUTHORIZATION_ROUTE_NONE:
        return "APPLIANCE_MANAGEMENT_AUTHORIZATION_ROUTE_NONE";
    case APPLIANCE_MANAGEMENT_AUTHORIZATION_ROUTE_DISCOVERY:
        return "APPLIANCE_MANAGEMENT_AUTHORIZATION_ROUTE_DISCOVERY";
    case APPLIANCE_MANAGEMENT_AUTHORIZATION_ROUTE_SYSTEM_STATUS:
        return "APPLIANCE_MANAGEMENT_AUTHORIZATION_ROUTE_SYSTEM_STATUS";
    case APPLIANCE_MANAGEMENT_AUTHORIZATION_ROUTE_NETWORK_CONFIG:
        return "APPLIANCE_MANAGEMENT_AUTHORIZATION_ROUTE_NETWORK_CONFIG";
    case APPLIANCE_MANAGEMENT_AUTHORIZATION_ROUTE_NETWORK_STATUS:
        return "APPLIANCE_MANAGEMENT_AUTHORIZATION_ROUTE_NETWORK_STATUS";
    case APPLIANCE_MANAGEMENT_AUTHORIZATION_ROUTE_SERVICE_STATUS:
        return "APPLIANCE_MANAGEMENT_AUTHORIZATION_ROUTE_SERVICE_STATUS";
    case APPLIANCE_MANAGEMENT_AUTHORIZATION_ROUTE_PANEL:
        return "APPLIANCE_MANAGEMENT_AUTHORIZATION_ROUTE_PANEL";
    default:
        return NULL;
    }
}

static void test_strings(void)
{
    enum appliance_management_authorization_status status;
    enum appliance_management_authorization_route route;

    for (status = APPLIANCE_MANAGEMENT_AUTHORIZATION_OK;
         status <= APPLIANCE_MANAGEMENT_AUTHORIZATION_BAD_AUTHORIZATION;
         ++status) {
        const char *string =
            appliance_management_authorization_status_string(status);
        const char *expected = expected_status_string(status);

        CHECK(string != NULL && expected != NULL &&
                  strcmp(string, expected) == 0,
              "management authorization status string is exact");
    }
    CHECK(appliance_management_authorization_status_string(
              (enum appliance_management_authorization_status)11U) == NULL,
          "management authorization undefined status string is rejected");

    for (route = APPLIANCE_MANAGEMENT_AUTHORIZATION_ROUTE_NONE;
         route <= APPLIANCE_MANAGEMENT_AUTHORIZATION_ROUTE_SERVICE_STATUS;
         ++route) {
        const char *string =
            appliance_management_authorization_route_string(route);
        const char *expected = expected_route_string(route);

        CHECK(string != NULL && expected != NULL &&
                  strcmp(string, expected) == 0,
              "management authorization route string is exact");
    }
    CHECK(appliance_management_authorization_route_string(
              APPLIANCE_MANAGEMENT_AUTHORIZATION_ROUTE_PANEL) != NULL &&
              strcmp(
                  appliance_management_authorization_route_string(
                      APPLIANCE_MANAGEMENT_AUTHORIZATION_ROUTE_PANEL),
                  "APPLIANCE_MANAGEMENT_AUTHORIZATION_ROUTE_PANEL") == 0,
          "management authorization panel route string is exact");
    CHECK(appliance_management_authorization_route_string(
              (enum appliance_management_authorization_route)6U) == NULL,
          "management authorization undefined route string is rejected");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "management authorization string handling performs no allocation");
}

static void test_policy_validation(void)
{
    struct appliance_management_authorization_policy policy;
    struct appliance_management_authorization_policy bad;
    struct appliance_admin_auth_decision unauth =
        make_decision(false, 0U, 0U, 0U);
    enum appliance_management_authorization_route route =
        APPLIANCE_MANAGEMENT_AUTHORIZATION_ROUTE_NONE;

    CHECK(appliance_management_authorization_policy_init(NULL) ==
              APPLIANCE_MANAGEMENT_AUTHORIZATION_NULL,
          "management authorization policy rejects null policy");
    memset(&policy, 0, sizeof(policy));
    CHECK(appliance_management_authorization_policy_init(&policy) ==
              APPLIANCE_MANAGEMENT_AUTHORIZATION_OK &&
              policy.version ==
                  APPLIANCE_MANAGEMENT_AUTHORIZATION_VERSION &&
              policy.initialized &&
              policy.matrix.initialized &&
              policy.matrix.version == AUTHORIZATION_VERSION &&
              policy.matrix.count == 17U,
          "management authorization policy publishes four read routes, "
          "six mutation command ids, the rendered panel route, the "
          "ITEM-503 certificate import command, the ITEM-504 routing "
          "configuration command, the ITEM-505 system-hostname command, "
          "the ITEM-506 credential-change command, the ITEM-507 "
          "certificate-name command, the ITEM-508 ACME control command "
          "and the ITEM-516 management-gate command");

    CHECK(appliance_management_authorization_check_request(
              NULL,
              &unauth,
              HTTP_METHOD_GET,
              (const uint8_t *)APPLIANCE_MANAGEMENT_AUTHORIZATION_SYSTEM_PATH,
              (uint32_t)strlen(
                  APPLIANCE_MANAGEMENT_AUTHORIZATION_SYSTEM_PATH),
              &route) == APPLIANCE_MANAGEMENT_AUTHORIZATION_NULL,
          "management authorization rejects null policy");
    CHECK(appliance_management_authorization_check_request(
              &policy,
              NULL,
              HTTP_METHOD_GET,
              (const uint8_t *)APPLIANCE_MANAGEMENT_AUTHORIZATION_SYSTEM_PATH,
              (uint32_t)strlen(
                  APPLIANCE_MANAGEMENT_AUTHORIZATION_SYSTEM_PATH),
              &route) == APPLIANCE_MANAGEMENT_AUTHORIZATION_NULL,
          "management authorization rejects null decision");
    CHECK(appliance_management_authorization_check_request(
              &policy,
              &unauth,
              HTTP_METHOD_GET,
              NULL,
              0U,
              &route) == APPLIANCE_MANAGEMENT_AUTHORIZATION_NULL,
          "management authorization rejects null target");
    CHECK(appliance_management_authorization_check_request(
              &policy,
              &unauth,
              HTTP_METHOD_GET,
              (const uint8_t *)APPLIANCE_MANAGEMENT_AUTHORIZATION_SYSTEM_PATH,
              (uint32_t)strlen(
                  APPLIANCE_MANAGEMENT_AUTHORIZATION_SYSTEM_PATH),
              NULL) == APPLIANCE_MANAGEMENT_AUTHORIZATION_NULL,
          "management authorization rejects null route output");

    bad = policy;
    bad.version = APPLIANCE_MANAGEMENT_AUTHORIZATION_VERSION + 1U;
    CHECK(appliance_management_authorization_check_request(
              &bad,
              &unauth,
              HTTP_METHOD_GET,
              (const uint8_t *)APPLIANCE_MANAGEMENT_AUTHORIZATION_SYSTEM_PATH,
              (uint32_t)strlen(
                  APPLIANCE_MANAGEMENT_AUTHORIZATION_SYSTEM_PATH),
              &route) == APPLIANCE_MANAGEMENT_AUTHORIZATION_BAD_VERSION,
          "management authorization rejects unknown policy version");
    bad = policy;
    bad.initialized = false;
    CHECK(appliance_management_authorization_check_request(
              &bad,
              &unauth,
              HTTP_METHOD_GET,
              (const uint8_t *)APPLIANCE_MANAGEMENT_AUTHORIZATION_SYSTEM_PATH,
              (uint32_t)strlen(
                  APPLIANCE_MANAGEMENT_AUTHORIZATION_SYSTEM_PATH),
              &route) == APPLIANCE_MANAGEMENT_AUTHORIZATION_BAD_STATE,
          "management authorization rejects unpublished policy");
}

static void check_discovery(
    const struct appliance_management_authorization_policy *policy,
    enum http_method_kind method,
    enum appliance_management_authorization_status expected)
{
    struct appliance_admin_auth_decision decision =
        make_decision(false, 0U, 0U, 0U);
    enum appliance_management_authorization_route route =
        APPLIANCE_MANAGEMENT_AUTHORIZATION_ROUTE_NONE;

    CHECK(appliance_management_authorization_check_request(
              policy,
              &decision,
              method,
              (const uint8_t *)APPLIANCE_MANAGEMENT_AUTHORIZATION_DISCOVERY_PATH,
              (uint32_t)strlen(
                  APPLIANCE_MANAGEMENT_AUTHORIZATION_DISCOVERY_PATH),
              &route) == expected &&
              (expected == APPLIANCE_MANAGEMENT_AUTHORIZATION_OK
                   ? route ==
                         APPLIANCE_MANAGEMENT_AUTHORIZATION_ROUTE_DISCOVERY
                   : route ==
                         APPLIANCE_MANAGEMENT_AUTHORIZATION_ROUTE_DISCOVERY),
          "management discovery route classification is exact");
}

static void test_discovery_and_route_classification(void)
{
    struct appliance_management_authorization_policy policy;
    struct appliance_admin_auth_decision auth =
        make_decision(true,
                      APPLIANCE_ADMIN_PRINCIPAL_ID,
                      1U,
                      1U);
    struct appliance_admin_auth_decision unauth =
        make_decision(false, 0U, 0U, 0U);
    struct appliance_admin_auth_decision malformed_unauth =
        make_decision(false, APPLIANCE_ADMIN_PRINCIPAL_ID, 0U, 0U);
    enum appliance_management_authorization_route route =
        APPLIANCE_MANAGEMENT_AUTHORIZATION_ROUTE_NONE;

    init_policy(&policy);
    check_discovery(&policy, HTTP_METHOD_GET,
                    APPLIANCE_MANAGEMENT_AUTHORIZATION_OK);
    check_discovery(&policy, HTTP_METHOD_HEAD,
                    APPLIANCE_MANAGEMENT_AUTHORIZATION_OK);
    check_discovery(&policy, HTTP_METHOD_UNSUPPORTED,
                    APPLIANCE_MANAGEMENT_AUTHORIZATION_METHOD_NOT_ALLOWED);

    CHECK(appliance_management_authorization_check_request(
              &policy,
              &unauth,
              HTTP_METHOD_GET,
              (const uint8_t *)APPLIANCE_MANAGEMENT_AUTHORIZATION_SYSTEM_PATH,
              (uint32_t)strlen(
                  APPLIANCE_MANAGEMENT_AUTHORIZATION_SYSTEM_PATH),
              &route) == APPLIANCE_MANAGEMENT_AUTHORIZATION_UNAUTHENTICATED &&
              route ==
                  APPLIANCE_MANAGEMENT_AUTHORIZATION_ROUTE_SYSTEM_STATUS,
          "unauthenticated protected system route is denied");
    CHECK(appliance_management_authorization_check_request(
              &policy,
              &auth,
              HTTP_METHOD_GET,
              (const uint8_t *)APPLIANCE_MANAGEMENT_AUTHORIZATION_SYSTEM_PATH,
              (uint32_t)strlen(
                  APPLIANCE_MANAGEMENT_AUTHORIZATION_SYSTEM_PATH),
              &route) == APPLIANCE_MANAGEMENT_AUTHORIZATION_OK &&
              route ==
                  APPLIANCE_MANAGEMENT_AUTHORIZATION_ROUTE_SYSTEM_STATUS,
          "authenticated admin can read system status route");
    CHECK(appliance_management_authorization_check_request(
              &policy,
              &auth,
              HTTP_METHOD_HEAD,
              (const uint8_t *)APPLIANCE_MANAGEMENT_AUTHORIZATION_NETWORK_STATUS_PATH,
              (uint32_t)strlen(
                  APPLIANCE_MANAGEMENT_AUTHORIZATION_NETWORK_STATUS_PATH),
              &route) == APPLIANCE_MANAGEMENT_AUTHORIZATION_OK &&
              route ==
                  APPLIANCE_MANAGEMENT_AUTHORIZATION_ROUTE_NETWORK_STATUS,
          "authenticated admin can read network status route");
    CHECK(appliance_management_authorization_check_request(
              &policy,
              &auth,
              HTTP_METHOD_GET,
              (const uint8_t *)APPLIANCE_MANAGEMENT_AUTHORIZATION_NETWORK_PATH,
              (uint32_t)strlen(
                  APPLIANCE_MANAGEMENT_AUTHORIZATION_NETWORK_PATH),
              &route) == APPLIANCE_MANAGEMENT_AUTHORIZATION_OK &&
              route ==
                  APPLIANCE_MANAGEMENT_AUTHORIZATION_ROUTE_NETWORK_CONFIG,
          "authenticated admin can read network configuration route");
    CHECK(appliance_management_authorization_check_request(
              &policy,
              &auth,
              HTTP_METHOD_GET,
              (const uint8_t *)APPLIANCE_MANAGEMENT_AUTHORIZATION_SERVICE_PATH,
              (uint32_t)strlen(
                  APPLIANCE_MANAGEMENT_AUTHORIZATION_SERVICE_PATH),
              &route) == APPLIANCE_MANAGEMENT_AUTHORIZATION_OK &&
              route ==
                  APPLIANCE_MANAGEMENT_AUTHORIZATION_ROUTE_SERVICE_STATUS,
          "authenticated admin can read service status route");
    CHECK(appliance_management_authorization_check_request(
              &policy,
              &auth,
              HTTP_METHOD_UNSUPPORTED,
              (const uint8_t *)APPLIANCE_MANAGEMENT_AUTHORIZATION_SYSTEM_PATH,
              (uint32_t)strlen(
                  APPLIANCE_MANAGEMENT_AUTHORIZATION_SYSTEM_PATH),
              &route) ==
                  APPLIANCE_MANAGEMENT_AUTHORIZATION_METHOD_NOT_ALLOWED &&
              route ==
                  APPLIANCE_MANAGEMENT_AUTHORIZATION_ROUTE_SYSTEM_STATUS,
          "unsupported method on protected route is method-not-allowed");
    CHECK(appliance_management_authorization_check_request(
              &policy,
              &malformed_unauth,
              HTTP_METHOD_GET,
              (const uint8_t *)APPLIANCE_MANAGEMENT_AUTHORIZATION_SYSTEM_PATH,
              (uint32_t)strlen(
                  APPLIANCE_MANAGEMENT_AUTHORIZATION_SYSTEM_PATH),
              &route) == APPLIANCE_MANAGEMENT_AUTHORIZATION_BAD_STATE,
          "unauthenticated decision with principal claims is rejected");
    CHECK(appliance_management_authorization_check_request(
              &policy,
              &auth,
              HTTP_METHOD_GET,
              (const uint8_t *)"/api/v1/unknown",
              15U,
              &route) == APPLIANCE_MANAGEMENT_AUTHORIZATION_UNKNOWN_ROUTE &&
              route == APPLIANCE_MANAGEMENT_AUTHORIZATION_ROUTE_NONE,
          "unknown management route is denied by default");
    CHECK(appliance_management_authorization_check_request(
              &policy,
              &auth,
              HTTP_METHOD_GET,
              (const uint8_t *)"/api2/v1",
              8U,
              &route) ==
                  APPLIANCE_MANAGEMENT_AUTHORIZATION_NOT_MANAGEMENT &&
              route == APPLIANCE_MANAGEMENT_AUTHORIZATION_ROUTE_NONE,
          "non-management prefix is rejected");
    CHECK(appliance_management_authorization_check_request(
              &policy,
              &auth,
              HTTP_METHOD_GET,
              (const uint8_t *)"/api",
              4U,
              &route) == APPLIANCE_MANAGEMENT_AUTHORIZATION_UNKNOWN_ROUTE &&
              route == APPLIANCE_MANAGEMENT_AUTHORIZATION_ROUTE_NONE,
          "management namespace with no exact route is denied by default");
    CHECK(appliance_management_authorization_check_request(
              &policy,
              &auth,
              HTTP_METHOD_GET,
              (const uint8_t *)APPLIANCE_MANAGEMENT_AUTHORIZATION_PANEL_PATH,
              APPLIANCE_MANAGEMENT_AUTHORIZATION_PANEL_PATH_LENGTH,
              &route) == APPLIANCE_MANAGEMENT_AUTHORIZATION_OK &&
              route == APPLIANCE_MANAGEMENT_AUTHORIZATION_ROUTE_PANEL,
          "authenticated admin can read the rendered panel");
    CHECK(appliance_management_authorization_check_request(
              &policy,
              &auth,
              HTTP_METHOD_HEAD,
              (const uint8_t *)"/panel/diagnostics",
              18U,
              &route) == APPLIANCE_MANAGEMENT_AUTHORIZATION_OK &&
              route == APPLIANCE_MANAGEMENT_AUTHORIZATION_ROUTE_PANEL,
          "authenticated admin can head a rendered panel section");
    CHECK(appliance_management_authorization_check_request(
              &policy,
              &auth,
              HTTP_METHOD_GET,
              (const uint8_t *)
                  APPLIANCE_MANAGEMENT_AUTHORIZATION_PANEL_ACCOUNT_PATH,
              (uint32_t)strlen(
                  APPLIANCE_MANAGEMENT_AUTHORIZATION_PANEL_ACCOUNT_PATH),
              &route) == APPLIANCE_MANAGEMENT_AUTHORIZATION_OK &&
              route == APPLIANCE_MANAGEMENT_AUTHORIZATION_ROUTE_PANEL,
          "authenticated admin can read the rendered Account page");
    CHECK(appliance_management_authorization_check_request(
              &policy,
              &unauth,
              HTTP_METHOD_GET,
              (const uint8_t *)"/panel/system",
              13U,
              &route) ==
                  APPLIANCE_MANAGEMENT_AUTHORIZATION_UNAUTHENTICATED &&
              route == APPLIANCE_MANAGEMENT_AUTHORIZATION_ROUTE_PANEL,
          "unauthenticated panel request is denied by default");
    CHECK(appliance_management_authorization_check_request(
              &policy,
              &auth,
              HTTP_METHOD_GET,
              (const uint8_t *)"/panel/attack",
              13U,
              &route) == APPLIANCE_MANAGEMENT_AUTHORIZATION_UNKNOWN_ROUTE &&
              route == APPLIANCE_MANAGEMENT_AUTHORIZATION_ROUTE_NONE,
          "unknown panel target is denied by default");
    CHECK(appliance_management_authorization_check_request(
              &policy,
              &auth,
              HTTP_METHOD_GET,
              (const uint8_t *)"/panelish",
              9U,
              &route) ==
                  APPLIANCE_MANAGEMENT_AUTHORIZATION_NOT_MANAGEMENT &&
              route == APPLIANCE_MANAGEMENT_AUTHORIZATION_ROUTE_NONE,
          "panel prefix without a boundary is not a management target");
}

static void replace_route_requirements(
    struct appliance_management_authorization_policy *policy,
    uint32_t command_id,
    const uint8_t *roles,
    uint32_t role_count,
    const uint8_t *capabilities,
    uint32_t capability_count)
{
    size_t index;

    for (index = 0U;
         index < policy->matrix.count;
         ++index) {
        if (policy->matrix.entries[index].command_id == command_id) {
            memset(policy->matrix.entries[index].roles,
                   0,
                   sizeof(policy->matrix.entries[index].roles));
            memset(policy->matrix.entries[index].capabilities,
                   0,
                   sizeof(policy->matrix.entries[index].capabilities));
            policy->matrix.entries[index].role_count = role_count;
            if (role_count > 0U && roles != NULL) {
                memcpy(policy->matrix.entries[index].roles,
                       roles,
                       role_count);
            }
            policy->matrix.entries[index].capability_count =
                capability_count;
            if (capability_count > 0U && capabilities != NULL) {
                memcpy(policy->matrix.entries[index].capabilities,
                       capabilities,
                       capability_count);
            }
            return;
        }
    }
    CHECK(false, "management authorization fixture route entry is present");
}

static void test_authorization_denials(void)
{
    struct appliance_management_authorization_policy policy;
    struct appliance_admin_auth_decision auth =
        make_decision(true,
                      APPLIANCE_ADMIN_PRINCIPAL_ID,
                      1U,
                      1U);
    static const uint8_t operator_role[1] = {
        AUTHORIZATION_ROLE_OPERATOR
    };
    static const uint8_t admin_role[1] = {
        AUTHORIZATION_ROLE_ADMIN
    };
    static const uint8_t status_read[1] = {
        AUTHORIZATION_CAPABILITY_STATUS_READ
    };
    static const uint8_t config_write[1] = {
        AUTHORIZATION_CAPABILITY_CONFIG_WRITE
    };
    enum appliance_management_authorization_route route =
        APPLIANCE_MANAGEMENT_AUTHORIZATION_ROUTE_NONE;

    init_policy(&policy);
    replace_route_requirements(
        &policy,
        APPLIANCE_MANAGEMENT_AUTHORIZATION_ROUTE_SYSTEM_STATUS,
        operator_role,
        1U,
        status_read,
        1U);
    CHECK(appliance_management_authorization_check_request(
              &policy,
              &auth,
              HTTP_METHOD_GET,
              (const uint8_t *)APPLIANCE_MANAGEMENT_AUTHORIZATION_SYSTEM_PATH,
              (uint32_t)strlen(
                  APPLIANCE_MANAGEMENT_AUTHORIZATION_SYSTEM_PATH),
              &route) == APPLIANCE_MANAGEMENT_AUTHORIZATION_DENIED_ROLE &&
              route ==
                  APPLIANCE_MANAGEMENT_AUTHORIZATION_ROUTE_SYSTEM_STATUS,
          "missing required role denies protected route");

    init_policy(&policy);
    replace_route_requirements(
        &policy,
        APPLIANCE_MANAGEMENT_AUTHORIZATION_ROUTE_SYSTEM_STATUS,
        admin_role,
        1U,
        config_write,
        1U);
    CHECK(appliance_management_authorization_check_request(
              &policy,
              &auth,
              HTTP_METHOD_GET,
              (const uint8_t *)APPLIANCE_MANAGEMENT_AUTHORIZATION_SYSTEM_PATH,
              (uint32_t)strlen(
                  APPLIANCE_MANAGEMENT_AUTHORIZATION_SYSTEM_PATH),
              &route) ==
                  APPLIANCE_MANAGEMENT_AUTHORIZATION_DENIED_CAPABILITY,
          "missing required capability denies protected route");

    init_policy(&policy);
    policy.matrix.count = 0U;
    CHECK(appliance_management_authorization_check_request(
              &policy,
              &auth,
              HTTP_METHOD_GET,
              (const uint8_t *)APPLIANCE_MANAGEMENT_AUTHORIZATION_SYSTEM_PATH,
              (uint32_t)strlen(
                  APPLIANCE_MANAGEMENT_AUTHORIZATION_SYSTEM_PATH),
              &route) ==
                  APPLIANCE_MANAGEMENT_AUTHORIZATION_BAD_AUTHORIZATION,
          "invalid authorization matrix denies protected route");
}

static void test_mutation_classification(void)
{
    struct appliance_management_authorization_policy policy;
    struct appliance_admin_auth_decision admin =
        make_decision(true, APPLIANCE_ADMIN_PRINCIPAL_ID, 1U, 1U);
    struct appliance_admin_auth_decision anonymous =
        make_decision(false, 0U, 0U, 0U);
    enum appliance_management_authorization_mutation mutation =
        APPLIANCE_MANAGEMENT_AUTHORIZATION_MUTATION_NONE;
    enum appliance_management_authorization_route route =
        APPLIANCE_MANAGEMENT_AUTHORIZATION_ROUTE_NONE;
    static const uint8_t admin_role[1] = {
        AUTHORIZATION_ROLE_ADMIN
    };
    static const uint8_t audit_read[1] = {
        AUTHORIZATION_CAPABILITY_AUDIT_READ
    };

    init_policy(&policy);

    CHECK(strcmp(appliance_management_authorization_mutation_string(
                     APPLIANCE_MANAGEMENT_AUTHORIZATION_MUTATION_NONE),
                 "NONE") == 0 &&
              strcmp(appliance_management_authorization_mutation_string(
                         APPLIANCE_MANAGEMENT_AUTHORIZATION_MUTATION_NETWORK_CONFIG),
                     "NETWORK_CONFIG") == 0 &&
              strcmp(appliance_management_authorization_mutation_string(
                         APPLIANCE_MANAGEMENT_AUTHORIZATION_MUTATION_SERVICE_ACTION),
                     "SERVICE_ACTION") == 0 &&
              strcmp(appliance_management_authorization_mutation_string(
                         APPLIANCE_MANAGEMENT_AUTHORIZATION_MUTATION_UPDATE_ACCEPT),
                     "UPDATE_ACCEPT") == 0 &&
              strcmp(appliance_management_authorization_mutation_string(
                         APPLIANCE_MANAGEMENT_AUTHORIZATION_MUTATION_RECOVERY_ACTION),
                     "RECOVERY_ACTION") == 0 &&
              strcmp(appliance_management_authorization_mutation_string(
                         APPLIANCE_MANAGEMENT_AUTHORIZATION_MUTATION_FACTORY_RESET),
                     "FACTORY_RESET") == 0 &&
              strcmp(appliance_management_authorization_mutation_string(
                         APPLIANCE_MANAGEMENT_AUTHORIZATION_MUTATION_SYSTEM_HOSTNAME),
                     "SYSTEM_HOSTNAME") == 0 &&
              strcmp(appliance_management_authorization_mutation_string(
                         APPLIANCE_MANAGEMENT_AUTHORIZATION_MUTATION_ACCOUNT_CREDENTIAL),
                     "ACCOUNT_CREDENTIAL") == 0 &&
              strcmp(appliance_management_authorization_mutation_string(
                         APPLIANCE_MANAGEMENT_AUTHORIZATION_MUTATION_PANEL_GATE),
                     "PANEL_GATE") == 0 &&
              strcmp(appliance_management_authorization_mutation_string(
                         (enum appliance_management_authorization_mutation)99U),
                     "UNKNOWN") == 0,
          "management authorization mutation strings are exact");

    /* ITEM-516: the gate's panel page and API target are one mutation. */
    mutation = APPLIANCE_MANAGEMENT_AUTHORIZATION_MUTATION_NONE;
    CHECK(appliance_management_authorization_check_mutation_request(
              &policy, &admin,
              APPLIANCE_MANAGEMENT_AUTHORIZATION_METHOD_POST,
              (const uint8_t *)APPLIANCE_MANAGEMENT_AUTHORIZATION_GATE_PATH,
              (uint32_t)strlen(
                  APPLIANCE_MANAGEMENT_AUTHORIZATION_GATE_PATH),
              &mutation) == APPLIANCE_MANAGEMENT_AUTHORIZATION_OK &&
              mutation ==
                  APPLIANCE_MANAGEMENT_AUTHORIZATION_MUTATION_PANEL_GATE,
          "the gate API target is classified as the gate mutation");
    mutation = APPLIANCE_MANAGEMENT_AUTHORIZATION_MUTATION_NONE;
    CHECK(appliance_management_authorization_check_mutation_request(
              &policy, &admin,
              APPLIANCE_MANAGEMENT_AUTHORIZATION_METHOD_POST,
              (const uint8_t *)
                  APPLIANCE_MANAGEMENT_AUTHORIZATION_PANEL_GATE_PATH,
              (uint32_t)strlen(
                  APPLIANCE_MANAGEMENT_AUTHORIZATION_PANEL_GATE_PATH),
              &mutation) == APPLIANCE_MANAGEMENT_AUTHORIZATION_OK &&
              mutation ==
                  APPLIANCE_MANAGEMENT_AUTHORIZATION_MUTATION_PANEL_GATE &&
              appliance_management_authorization_check_mutation_request(
                  &policy, &admin,
                  APPLIANCE_MANAGEMENT_AUTHORIZATION_METHOD_PUT,
                  (const uint8_t *)
                      APPLIANCE_MANAGEMENT_AUTHORIZATION_PANEL_GATE_PATH,
                  (uint32_t)strlen(
                      APPLIANCE_MANAGEMENT_AUTHORIZATION_PANEL_GATE_PATH),
                  &mutation) ==
                  APPLIANCE_MANAGEMENT_AUTHORIZATION_METHOD_NOT_ALLOWED,
          "the gate panel target is POST-only and classified as the mutation");
    CHECK(appliance_management_authorization_check_request(
              &policy, &admin, HTTP_METHOD_GET,
              (const uint8_t *)
                  APPLIANCE_MANAGEMENT_AUTHORIZATION_PANEL_GATE_PATH,
              (uint32_t)strlen(
                  APPLIANCE_MANAGEMENT_AUTHORIZATION_PANEL_GATE_PATH),
              &route) == APPLIANCE_MANAGEMENT_AUTHORIZATION_OK &&
              route == APPLIANCE_MANAGEMENT_AUTHORIZATION_ROUTE_PANEL,
          "the gate page reads through the rendered panel route");

    CHECK(appliance_management_authorization_check_mutation_request(
              NULL, &admin,
              APPLIANCE_MANAGEMENT_AUTHORIZATION_METHOD_PUT,
              (const uint8_t *)APPLIANCE_MANAGEMENT_AUTHORIZATION_NETWORK_PATH,
              (uint32_t)strlen(
                  APPLIANCE_MANAGEMENT_AUTHORIZATION_NETWORK_PATH),
              &mutation) == APPLIANCE_MANAGEMENT_AUTHORIZATION_NULL,
          "mutation authorization rejects null policy");

    mutation = APPLIANCE_MANAGEMENT_AUTHORIZATION_MUTATION_NONE;
    CHECK(appliance_management_authorization_check_mutation_request(
              &policy, &admin,
              APPLIANCE_MANAGEMENT_AUTHORIZATION_METHOD_PUT,
              (const uint8_t *)APPLIANCE_MANAGEMENT_AUTHORIZATION_NETWORK_PATH,
              (uint32_t)strlen(
                  APPLIANCE_MANAGEMENT_AUTHORIZATION_NETWORK_PATH),
              &mutation) == APPLIANCE_MANAGEMENT_AUTHORIZATION_OK &&
              mutation ==
                  APPLIANCE_MANAGEMENT_AUTHORIZATION_MUTATION_NETWORK_CONFIG,
          "network PUT is classified as an authorized configuration mutation");

    mutation = APPLIANCE_MANAGEMENT_AUTHORIZATION_MUTATION_NONE;
    CHECK(appliance_management_authorization_check_mutation_request(
              &policy, &admin,
              APPLIANCE_MANAGEMENT_AUTHORIZATION_METHOD_POST,
              (const uint8_t *)APPLIANCE_MANAGEMENT_AUTHORIZATION_SERVICE_PATH,
              (uint32_t)strlen(
                  APPLIANCE_MANAGEMENT_AUTHORIZATION_SERVICE_PATH),
              &mutation) == APPLIANCE_MANAGEMENT_AUTHORIZATION_OK &&
              mutation ==
                  APPLIANCE_MANAGEMENT_AUTHORIZATION_MUTATION_SERVICE_ACTION,
          "service POST is classified as an authorized service action");

    CHECK(appliance_management_authorization_check_mutation_request(
              &policy, &admin,
              APPLIANCE_MANAGEMENT_AUTHORIZATION_METHOD_PUT,
              (const uint8_t *)APPLIANCE_MANAGEMENT_AUTHORIZATION_SERVICE_PATH,
              (uint32_t)strlen(
                  APPLIANCE_MANAGEMENT_AUTHORIZATION_SERVICE_PATH),
              &mutation) ==
              APPLIANCE_MANAGEMENT_AUTHORIZATION_METHOD_NOT_ALLOWED,
          "service PUT has no mutation handler and is refused");

    mutation = APPLIANCE_MANAGEMENT_AUTHORIZATION_MUTATION_NONE;
    CHECK(appliance_management_authorization_check_mutation_request(
              &policy, &admin,
              APPLIANCE_MANAGEMENT_AUTHORIZATION_METHOD_POST,
              (const uint8_t *)
                  APPLIANCE_MANAGEMENT_AUTHORIZATION_RECOVERY_PATH,
              (uint32_t)strlen(
                  APPLIANCE_MANAGEMENT_AUTHORIZATION_RECOVERY_PATH),
              &mutation) == APPLIANCE_MANAGEMENT_AUTHORIZATION_OK &&
              mutation ==
                  APPLIANCE_MANAGEMENT_AUTHORIZATION_MUTATION_RECOVERY_ACTION,
          "recovery POST is classified as an authorized recovery action");

    CHECK(appliance_management_authorization_check_mutation_request(
              &policy, &admin,
              APPLIANCE_MANAGEMENT_AUTHORIZATION_METHOD_PUT,
              (const uint8_t *)
                  APPLIANCE_MANAGEMENT_AUTHORIZATION_RECOVERY_PATH,
              (uint32_t)strlen(
                  APPLIANCE_MANAGEMENT_AUTHORIZATION_RECOVERY_PATH),
              &mutation) ==
              APPLIANCE_MANAGEMENT_AUTHORIZATION_METHOD_NOT_ALLOWED,
          "recovery PUT has no mutation handler and is refused");

    mutation = APPLIANCE_MANAGEMENT_AUTHORIZATION_MUTATION_NONE;
    CHECK(appliance_management_authorization_check_mutation_request(
              &policy, &admin,
              APPLIANCE_MANAGEMENT_AUTHORIZATION_METHOD_POST,
              (const uint8_t *)
                  APPLIANCE_MANAGEMENT_AUTHORIZATION_FACTORY_RESET_PATH,
              (uint32_t)strlen(
                  APPLIANCE_MANAGEMENT_AUTHORIZATION_FACTORY_RESET_PATH),
              &mutation) == APPLIANCE_MANAGEMENT_AUTHORIZATION_OK &&
              mutation ==
                  APPLIANCE_MANAGEMENT_AUTHORIZATION_MUTATION_FACTORY_RESET,
          "factory reset POST is classified as an authorized reset");

    CHECK(appliance_management_authorization_check_mutation_request(
              &policy, &admin,
              APPLIANCE_MANAGEMENT_AUTHORIZATION_METHOD_PUT,
              (const uint8_t *)
                  APPLIANCE_MANAGEMENT_AUTHORIZATION_FACTORY_RESET_PATH,
              (uint32_t)strlen(
                  APPLIANCE_MANAGEMENT_AUTHORIZATION_FACTORY_RESET_PATH),
              &mutation) ==
              APPLIANCE_MANAGEMENT_AUTHORIZATION_METHOD_NOT_ALLOWED,
          "factory reset PUT has no mutation handler and is refused");

    CHECK(appliance_management_authorization_check_mutation_request(
              &policy, &admin,
              APPLIANCE_MANAGEMENT_AUTHORIZATION_METHOD_NONE,
              (const uint8_t *)APPLIANCE_MANAGEMENT_AUTHORIZATION_NETWORK_PATH,
              (uint32_t)strlen(
                  APPLIANCE_MANAGEMENT_AUTHORIZATION_NETWORK_PATH),
              &mutation) ==
              APPLIANCE_MANAGEMENT_AUTHORIZATION_METHOD_NOT_ALLOWED,
          "mutation authorization refuses a non-mutating method");

    mutation = APPLIANCE_MANAGEMENT_AUTHORIZATION_MUTATION_NONE;
    CHECK(appliance_management_authorization_check_mutation_request(
              &policy, &anonymous,
              APPLIANCE_MANAGEMENT_AUTHORIZATION_METHOD_PUT,
              (const uint8_t *)APPLIANCE_MANAGEMENT_AUTHORIZATION_NETWORK_PATH,
              (uint32_t)strlen(
                  APPLIANCE_MANAGEMENT_AUTHORIZATION_NETWORK_PATH),
              &mutation) ==
              APPLIANCE_MANAGEMENT_AUTHORIZATION_UNAUTHENTICATED &&
              mutation ==
                  APPLIANCE_MANAGEMENT_AUTHORIZATION_MUTATION_NETWORK_CONFIG,
          "anonymous mutation is classified but never authorized");

    mutation = APPLIANCE_MANAGEMENT_AUTHORIZATION_MUTATION_NONE;
    CHECK(appliance_management_authorization_check_mutation_request(
              &policy, &admin,
              APPLIANCE_MANAGEMENT_AUTHORIZATION_METHOD_POST,
              (const uint8_t *)APPLIANCE_MANAGEMENT_AUTHORIZATION_SYSTEM_PATH,
              (uint32_t)strlen(
                  APPLIANCE_MANAGEMENT_AUTHORIZATION_SYSTEM_PATH),
              &mutation) == APPLIANCE_MANAGEMENT_AUTHORIZATION_OK &&
              mutation ==
                  APPLIANCE_MANAGEMENT_AUTHORIZATION_MUTATION_SYSTEM_HOSTNAME,
          "system POST is classified as an authorized hostname mutation");

    mutation = APPLIANCE_MANAGEMENT_AUTHORIZATION_MUTATION_NONE;
    CHECK(appliance_management_authorization_check_mutation_request(
              &policy, &admin,
              APPLIANCE_MANAGEMENT_AUTHORIZATION_METHOD_POST,
              (const uint8_t *)
                  APPLIANCE_MANAGEMENT_AUTHORIZATION_PANEL_SYSTEM_PATH,
              (uint32_t)strlen(
                  APPLIANCE_MANAGEMENT_AUTHORIZATION_PANEL_SYSTEM_PATH),
              &mutation) == APPLIANCE_MANAGEMENT_AUTHORIZATION_OK &&
              mutation ==
                  APPLIANCE_MANAGEMENT_AUTHORIZATION_MUTATION_SYSTEM_HOSTNAME,
          "the panel's system form is the same hostname mutation");

    mutation = APPLIANCE_MANAGEMENT_AUTHORIZATION_MUTATION_NONE;
    CHECK(appliance_management_authorization_check_mutation_request(
              &policy, &admin,
              APPLIANCE_MANAGEMENT_AUTHORIZATION_METHOD_PUT,
              (const uint8_t *)APPLIANCE_MANAGEMENT_AUTHORIZATION_SYSTEM_PATH,
              (uint32_t)strlen(
                  APPLIANCE_MANAGEMENT_AUTHORIZATION_SYSTEM_PATH),
              &mutation) ==
              APPLIANCE_MANAGEMENT_AUTHORIZATION_METHOD_NOT_ALLOWED &&
              mutation == APPLIANCE_MANAGEMENT_AUTHORIZATION_MUTATION_NONE,
          "the hostname mutation is POST-only and refuses PUT");

    mutation = APPLIANCE_MANAGEMENT_AUTHORIZATION_MUTATION_NONE;
    CHECK(appliance_management_authorization_check_mutation_request(
              &policy, &admin,
              APPLIANCE_MANAGEMENT_AUTHORIZATION_METHOD_POST,
              (const uint8_t *)
                  APPLIANCE_MANAGEMENT_AUTHORIZATION_PANEL_ACCOUNT_PATH,
              (uint32_t)strlen(
                  APPLIANCE_MANAGEMENT_AUTHORIZATION_PANEL_ACCOUNT_PATH),
              &mutation) == APPLIANCE_MANAGEMENT_AUTHORIZATION_OK &&
              mutation ==
                  APPLIANCE_MANAGEMENT_AUTHORIZATION_MUTATION_ACCOUNT_CREDENTIAL,
          "the panel's account form is the credential-change mutation");

    mutation = APPLIANCE_MANAGEMENT_AUTHORIZATION_MUTATION_NONE;
    CHECK(appliance_management_authorization_check_mutation_request(
              &policy, &admin,
              APPLIANCE_MANAGEMENT_AUTHORIZATION_METHOD_POST,
              (const uint8_t *)APPLIANCE_MANAGEMENT_AUTHORIZATION_ACCOUNT_PATH,
              (uint32_t)strlen(
                  APPLIANCE_MANAGEMENT_AUTHORIZATION_ACCOUNT_PATH),
              &mutation) == APPLIANCE_MANAGEMENT_AUTHORIZATION_OK &&
              mutation ==
                  APPLIANCE_MANAGEMENT_AUTHORIZATION_MUTATION_ACCOUNT_CREDENTIAL,
          "the scriptable account target is the same credential change");

    mutation = APPLIANCE_MANAGEMENT_AUTHORIZATION_MUTATION_NONE;
    CHECK(appliance_management_authorization_check_mutation_request(
              &policy, &admin,
              APPLIANCE_MANAGEMENT_AUTHORIZATION_METHOD_PUT,
              (const uint8_t *)
                  APPLIANCE_MANAGEMENT_AUTHORIZATION_PANEL_ACCOUNT_PATH,
              (uint32_t)strlen(
                  APPLIANCE_MANAGEMENT_AUTHORIZATION_PANEL_ACCOUNT_PATH),
              &mutation) ==
              APPLIANCE_MANAGEMENT_AUTHORIZATION_METHOD_NOT_ALLOWED &&
              mutation == APPLIANCE_MANAGEMENT_AUTHORIZATION_MUTATION_NONE,
          "the credential change is POST-only and refuses PUT");

    CHECK(appliance_management_authorization_check_mutation_request(
              &policy, &admin,
              APPLIANCE_MANAGEMENT_AUTHORIZATION_METHOD_PUT,
              (const uint8_t *)"/api/v1/other",
              (uint32_t)strlen("/api/v1/other"),
              &mutation) == APPLIANCE_MANAGEMENT_AUTHORIZATION_UNKNOWN_ROUTE &&
              mutation == APPLIANCE_MANAGEMENT_AUTHORIZATION_MUTATION_NONE,
          "unknown management target has no mutation route");

    replace_route_requirements(
        &policy,
        APPLIANCE_MANAGEMENT_AUTHORIZATION_COMMAND_NETWORK_MUTATION,
        admin_role,
        1U,
        audit_read,
        1U);
    mutation = APPLIANCE_MANAGEMENT_AUTHORIZATION_MUTATION_NONE;
    CHECK(appliance_management_authorization_check_mutation_request(
              &policy, &admin,
              APPLIANCE_MANAGEMENT_AUTHORIZATION_METHOD_PUT,
              (const uint8_t *)APPLIANCE_MANAGEMENT_AUTHORIZATION_NETWORK_PATH,
              (uint32_t)strlen(
                  APPLIANCE_MANAGEMENT_AUTHORIZATION_NETWORK_PATH),
              &mutation) ==
              APPLIANCE_MANAGEMENT_AUTHORIZATION_DENIED_CAPABILITY &&
              mutation ==
                  APPLIANCE_MANAGEMENT_AUTHORIZATION_MUTATION_NETWORK_CONFIG,
          "missing configuration-write capability denies the mutation");
}

static void test_auth_decision_validation(void)
{
    struct appliance_management_authorization_policy policy;
    struct appliance_admin_auth_decision wrong_principal =
        make_decision(true, 2U, 1U, 1U);
    struct appliance_admin_auth_decision bad_session =
        make_decision(true,
                      APPLIANCE_ADMIN_PRINCIPAL_ID,
                      0U,
                      1U);
    struct appliance_admin_auth_decision bad_generation =
        make_decision(true,
                      APPLIANCE_ADMIN_PRINCIPAL_ID,
                      1U,
                      0U);
    enum appliance_management_authorization_route route =
        APPLIANCE_MANAGEMENT_AUTHORIZATION_ROUTE_NONE;

    init_policy(&policy);
    CHECK(appliance_management_authorization_check_request(
              &policy,
              &wrong_principal,
              HTTP_METHOD_GET,
              (const uint8_t *)APPLIANCE_MANAGEMENT_AUTHORIZATION_SYSTEM_PATH,
              (uint32_t)strlen(
                  APPLIANCE_MANAGEMENT_AUTHORIZATION_SYSTEM_PATH),
              &route) == APPLIANCE_MANAGEMENT_AUTHORIZATION_BAD_STATE,
          "authenticated decision with non-admin principal is rejected");
    CHECK(appliance_management_authorization_check_request(
              &policy,
              &bad_session,
              HTTP_METHOD_GET,
              (const uint8_t *)APPLIANCE_MANAGEMENT_AUTHORIZATION_SYSTEM_PATH,
              (uint32_t)strlen(
                  APPLIANCE_MANAGEMENT_AUTHORIZATION_SYSTEM_PATH),
              &route) == APPLIANCE_MANAGEMENT_AUTHORIZATION_BAD_STATE,
          "authenticated decision with invalid session is rejected");
    CHECK(appliance_management_authorization_check_request(
              &policy,
              &bad_generation,
              HTTP_METHOD_GET,
              (const uint8_t *)APPLIANCE_MANAGEMENT_AUTHORIZATION_SYSTEM_PATH,
              (uint32_t)strlen(
                  APPLIANCE_MANAGEMENT_AUTHORIZATION_SYSTEM_PATH),
              &route) == APPLIANCE_MANAGEMENT_AUTHORIZATION_BAD_STATE,
          "authenticated decision with invalid generation is rejected");
}

int main(void)
{
    test_strings();
    test_policy_validation();
    test_discovery_and_route_classification();
    test_mutation_classification();
    test_authorization_denials();
    test_auth_decision_validation();
    if (failures != 0) {
        fprintf(stderr, "Appliance management authorization host tests: "
                        "%d FAILURES\n",
                failures);
        return 1;
    }
    printf("Appliance management authorization host tests: ALL OK\n");
    return 0;
}

#endif
