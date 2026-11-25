#include "appliance_production_auth.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifdef HFOC_APPLIANCE_PRODUCTION_AUTH_HOST_TEST

static int failures;

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
    return NULL;
}

bool kfree(void *ptr)
{
    (void)ptr;
    return false;
}

static void make_credential(
    struct appliance_admin_auth_credential *credential,
    uint8_t seed)
{
    size_t index;

    memset(credential, 0, sizeof(*credential));
    credential->version = APPLIANCE_ADMIN_CREDENTIAL_VERSION;
    for (index = 0U; index < sizeof(credential->verifier); ++index) {
        credential->verifier[index] = (uint8_t)(seed + (uint8_t)index);
    }
}

static void make_proof(
    struct appliance_admin_auth_proof *proof,
    uint8_t seed)
{
    size_t index;

    memset(proof, 0, sizeof(*proof));
    proof->version = APPLIANCE_ADMIN_CREDENTIAL_VERSION;
    for (index = 0U; index < sizeof(proof->proof); ++index) {
        proof->proof[index] = (uint8_t)(seed + (uint8_t)index);
    }
}

static void make_token(
    uint8_t token[APPLIANCE_ADMIN_AUTH_TOKEN_LENGTH],
    uint8_t seed)
{
    size_t index;

    for (index = 0U; index < APPLIANCE_ADMIN_AUTH_TOKEN_LENGTH; ++index) {
        token[index] = (uint8_t)(seed + (uint8_t)index);
    }
}

static void make_bearer_header(
    uint8_t header[APPLIANCE_ADMIN_AUTH_BEARER_PREFIX_LENGTH +
                   APPLIANCE_ADMIN_AUTH_TOKEN_LENGTH],
    const uint8_t token[APPLIANCE_ADMIN_AUTH_TOKEN_LENGTH])
{
    memcpy(header, APPLIANCE_ADMIN_AUTH_BEARER_PREFIX,
           APPLIANCE_ADMIN_AUTH_BEARER_PREFIX_LENGTH);
    memcpy(header + APPLIANCE_ADMIN_AUTH_BEARER_PREFIX_LENGTH,
           token, APPLIANCE_ADMIN_AUTH_TOKEN_LENGTH);
}

static bool render_contains(const char *text, const char *needle)
{
    return strstr(text, needle) != NULL;
}

static void test_init_and_unavailable(void)
{
    struct appliance_production_auth auth;
    struct appliance_admin_auth_decision decision;
    enum appliance_management_authorization_route route =
        APPLIANCE_MANAGEMENT_AUTHORIZATION_ROUTE_SYSTEM_STATUS;
    char output[APPLIANCE_PRODUCTION_AUTH_OUTPUT_MAX + 1U];
    size_t output_length = 0U;
    static const uint8_t system_target[] = "/api/v1/system";

    memset(&auth, 0, sizeof(auth));
    CHECK(appliance_production_auth_init(NULL) ==
              APPLIANCE_PRODUCTION_AUTH_NULL,
          "init rejects null");
    CHECK(appliance_production_auth_init(&auth) ==
              APPLIANCE_PRODUCTION_AUTH_OK &&
              auth.state ==
                  APPLIANCE_PRODUCTION_AUTH_STATE_AWAITING_CREDENTIAL &&
              auth.version == APPLIANCE_PRODUCTION_AUTH_VERSION &&
              auth.admin.state == APPLIANCE_ADMIN_AUTH_STATE_ABSENT &&
              !appliance_production_auth_available(&auth),
          "init prepares an unavailable composition");
    CHECK(appliance_production_auth_init(&auth) ==
              APPLIANCE_PRODUCTION_AUTH_ALREADY_INITIALIZED,
          "duplicate init rejected");
    CHECK(appliance_production_auth_available(NULL) == false,
          "available rejects null");

    memset(&decision, 0, sizeof(decision));
    CHECK(appliance_production_auth_authorize_request(
              &auth,
              NULL,
              0U,
              HTTP_METHOD_GET,
              system_target,
              sizeof(system_target) - 1U,
              10U,
              &decision,
              &route) == APPLIANCE_PRODUCTION_AUTH_UNAVAILABLE &&
              !decision.authenticated &&
              route == APPLIANCE_MANAGEMENT_AUTHORIZATION_ROUTE_NONE &&
              auth.unavailable_count == 1U,
          "unavailable composition fails closed");
    CHECK(appliance_production_auth_authorize_request(
              &auth,
              NULL,
              0U,
              HTTP_METHOD_GET,
              NULL,
              0U,
              10U,
              &decision,
              &route) == APPLIANCE_PRODUCTION_AUTH_NULL,
          "authorize rejects null target");

    CHECK(appliance_production_auth_render(
              &auth, output, sizeof(output), &output_length) ==
              APPLIANCE_PRODUCTION_AUTH_OK &&
              render_contains(output,
                              "production_auth v=1 state=AWAITING_CREDENTIAL auth=UNAVAILABLE") &&
              output_length > 0U,
          "unavailable render is honest");
}

static void test_anonymous_deny_authorized_allow_and_default_deny(void)
{
    struct appliance_production_auth auth;
    struct appliance_admin_auth_credential credential;
    struct appliance_admin_auth_proof proof;
    struct appliance_admin_auth_decision decision;
    enum appliance_management_authorization_route route =
        APPLIANCE_MANAGEMENT_AUTHORIZATION_ROUTE_NONE;
    uint8_t token[APPLIANCE_ADMIN_AUTH_TOKEN_LENGTH];
    uint8_t other_token[APPLIANCE_ADMIN_AUTH_TOKEN_LENGTH];
    uint8_t header[APPLIANCE_ADMIN_AUTH_BEARER_PREFIX_LENGTH +
                   APPLIANCE_ADMIN_AUTH_TOKEN_LENGTH];
    uint8_t other_header[sizeof(header)];
    uint32_t session_id = 0U;
    char output[APPLIANCE_PRODUCTION_AUTH_OUTPUT_MAX + 1U];
    size_t output_length = 0U;
    static const uint8_t discovery_target[] = "/api/v1";
    static const uint8_t system_target[] = "/api/v1/system";
    static const uint8_t unknown_target[] = "/api/v1/unknown";
    static const uint8_t outside_target[] = "/status";

    make_credential(&credential, 0x11U);
    make_proof(&proof, 0x11U);
    make_token(token, 0x21U);
    make_token(other_token, 0x31U);
    make_bearer_header(header, token);
    make_bearer_header(other_header, other_token);

    memset(&auth, 0, sizeof(auth));
    CHECK(appliance_production_auth_init(&auth) ==
              APPLIANCE_PRODUCTION_AUTH_OK,
          "auth composition init");
    CHECK(appliance_production_auth_publish_credential(
              &auth, &credential) == APPLIANCE_PRODUCTION_AUTH_OK &&
              auth.state == APPLIANCE_PRODUCTION_AUTH_STATE_ACTIVE &&
              appliance_production_auth_available(&auth) &&
              auth.admin.credential_generation == 1U,
          "credential publication activates auth");
    CHECK(appliance_production_auth_publish_credential(&auth, NULL) ==
              APPLIANCE_PRODUCTION_AUTH_NULL,
          "credential publication rejects null");

    CHECK(appliance_admin_auth_authenticate_local(
              &auth.admin, &proof, token, 10U, 100U, &session_id) ==
              APPLIANCE_ADMIN_AUTH_OK &&
              session_id == 1U,
          "authorized session issued");

    memset(&decision, 0, sizeof(decision));
    route = APPLIANCE_MANAGEMENT_AUTHORIZATION_ROUTE_NONE;
    CHECK(appliance_production_auth_authorize_request(
              &auth,
              NULL,
              0U,
              HTTP_METHOD_GET,
              system_target,
              sizeof(system_target) - 1U,
              20U,
              &decision,
              &route) == APPLIANCE_PRODUCTION_AUTH_DENIED &&
              !decision.authenticated &&
              route == APPLIANCE_MANAGEMENT_AUTHORIZATION_ROUTE_SYSTEM_STATUS &&
              auth.denied_count == 1U,
          "anonymous protected request denied");

    memset(&decision, 0, sizeof(decision));
    route = APPLIANCE_MANAGEMENT_AUTHORIZATION_ROUTE_NONE;
    CHECK(appliance_production_auth_authorize_request(
              &auth,
              NULL,
              0U,
              HTTP_METHOD_GET,
              discovery_target,
              sizeof(discovery_target) - 1U,
              20U,
              &decision,
              &route) == APPLIANCE_PRODUCTION_AUTH_ALLOWED &&
              !decision.authenticated &&
              route == APPLIANCE_MANAGEMENT_AUTHORIZATION_ROUTE_DISCOVERY &&
              auth.allowed_count == 1U &&
              auth.anonymous_count == 1U,
          "public discovery allowed anonymously");

    memset(&decision, 0, sizeof(decision));
    route = APPLIANCE_MANAGEMENT_AUTHORIZATION_ROUTE_NONE;
    CHECK(appliance_production_auth_authorize_request(
              &auth,
              header,
              sizeof(header),
              HTTP_METHOD_GET,
              system_target,
              sizeof(system_target) - 1U,
              30U,
              &decision,
              &route) == APPLIANCE_PRODUCTION_AUTH_ALLOWED &&
              decision.authenticated &&
              decision.principal_id == APPLIANCE_ADMIN_PRINCIPAL_ID &&
              decision.session_id == 1U &&
              route == APPLIANCE_MANAGEMENT_AUTHORIZATION_ROUTE_SYSTEM_STATUS &&
              auth.allowed_count == 2U,
          "authorized bearer request allowed");

    memset(&decision, 0, sizeof(decision));
    route = APPLIANCE_MANAGEMENT_AUTHORIZATION_ROUTE_NONE;
    CHECK(appliance_production_auth_authorize_request(
              &auth,
              other_header,
              sizeof(other_header),
              HTTP_METHOD_GET,
              system_target,
              sizeof(system_target) - 1U,
              30U,
              &decision,
              &route) == APPLIANCE_PRODUCTION_AUTH_DENIED &&
              !decision.authenticated &&
              auth.denied_count == 2U,
          "unknown bearer denied");

    memset(&decision, 0, sizeof(decision));
    route = APPLIANCE_MANAGEMENT_AUTHORIZATION_ROUTE_NONE;
    CHECK(appliance_production_auth_authorize_request(
              &auth,
              header,
              sizeof(header),
              HTTP_METHOD_GET,
              unknown_target,
              sizeof(unknown_target) - 1U,
              30U,
              &decision,
              &route) == APPLIANCE_PRODUCTION_AUTH_DEFAULT_DENIED &&
              route == APPLIANCE_MANAGEMENT_AUTHORIZATION_ROUTE_NONE &&
              auth.denied_count == 3U,
          "unknown route denied by default");

    memset(&decision, 0, sizeof(decision));
    route = APPLIANCE_MANAGEMENT_AUTHORIZATION_ROUTE_NONE;
    CHECK(appliance_production_auth_authorize_request(
              &auth,
              header,
              sizeof(header),
              HTTP_METHOD_UNSUPPORTED,
              system_target,
              sizeof(system_target) - 1U,
              30U,
              &decision,
              &route) == APPLIANCE_PRODUCTION_AUTH_DEFAULT_DENIED &&
              route == APPLIANCE_MANAGEMENT_AUTHORIZATION_ROUTE_SYSTEM_STATUS &&
              auth.denied_count == 4U,
          "unsupported method denied by default");

    memset(&decision, 0, sizeof(decision));
    route = APPLIANCE_MANAGEMENT_AUTHORIZATION_ROUTE_NONE;
    CHECK(appliance_production_auth_authorize_request(
              &auth,
              header,
              sizeof(header),
              HTTP_METHOD_GET,
              outside_target,
              sizeof(outside_target) - 1U,
              30U,
              &decision,
              &route) == APPLIANCE_PRODUCTION_AUTH_DEFAULT_DENIED &&
              route == APPLIANCE_MANAGEMENT_AUTHORIZATION_ROUTE_NONE &&
              auth.denied_count == 5U,
          "non-management target denied by default");

    CHECK(appliance_production_auth_render(
              &auth, output, sizeof(output), &output_length) ==
              APPLIANCE_PRODUCTION_AUTH_OK &&
              render_contains(output,
                              "production_auth v=1 state=ACTIVE auth=AVAILABLE") &&
              render_contains(output,
                              "allowed=2 denied=5 anonymous=1 unavailable=0"),
          "active render publishes availability and counters");
}

static void test_stop_reset_and_rotation(void)
{
    struct appliance_production_auth auth;
    struct appliance_admin_auth_credential credential;
    struct appliance_admin_auth_credential replacement;
    struct appliance_admin_auth_proof proof;
    struct appliance_admin_auth_decision decision;
    enum appliance_management_authorization_route route =
        APPLIANCE_MANAGEMENT_AUTHORIZATION_ROUTE_NONE;
    uint8_t token[APPLIANCE_ADMIN_AUTH_TOKEN_LENGTH];
    uint8_t header[APPLIANCE_ADMIN_AUTH_BEARER_PREFIX_LENGTH +
                   APPLIANCE_ADMIN_AUTH_TOKEN_LENGTH];
    uint32_t session_id = 0U;
    static const uint8_t system_target[] = "/api/v1/system";

    make_credential(&credential, 0x41U);
    make_credential(&replacement, 0x51U);
    make_proof(&proof, 0x41U);
    make_token(token, 0x61U);
    make_bearer_header(header, token);

    memset(&auth, 0, sizeof(auth));
    CHECK(appliance_production_auth_init(&auth) ==
              APPLIANCE_PRODUCTION_AUTH_OK &&
              appliance_production_auth_publish_credential(
                  &auth, &credential) == APPLIANCE_PRODUCTION_AUTH_OK &&
              appliance_admin_auth_authenticate_local(
                  &auth.admin, &proof, token, 0U, 100U, &session_id) ==
                  APPLIANCE_ADMIN_AUTH_OK,
          "rotation fixture active");

    CHECK(appliance_production_auth_stop(&auth) ==
              APPLIANCE_PRODUCTION_AUTH_OK &&
              auth.state == APPLIANCE_PRODUCTION_AUTH_STATE_STOPPED &&
              !appliance_production_auth_available(&auth),
          "stop revokes availability");
    memset(&decision, 0, sizeof(decision));
    CHECK(appliance_production_auth_authorize_request(
              &auth,
              header,
              sizeof(header),
              HTTP_METHOD_GET,
              system_target,
              sizeof(system_target) - 1U,
              10U,
              &decision,
              &route) == APPLIANCE_PRODUCTION_AUTH_UNAVAILABLE &&
              auth.unavailable_count == 1U,
          "stopped composition fails closed");
    CHECK(appliance_production_auth_publish_credential(
              &auth, &credential) == APPLIANCE_PRODUCTION_AUTH_BAD_STATE,
          "stopped composition rejects credential");

    CHECK(appliance_production_auth_reset(&auth) ==
              APPLIANCE_PRODUCTION_AUTH_OK &&
              auth.state ==
                  APPLIANCE_PRODUCTION_AUTH_STATE_AWAITING_CREDENTIAL &&
              auth.admin.state ==
                  APPLIANCE_ADMIN_AUTH_STATE_RESET_PENDING,
          "reset awaits replacement credential");
    CHECK(appliance_production_auth_publish_credential(
              &auth, &replacement) == APPLIANCE_PRODUCTION_AUTH_OK &&
              appliance_production_auth_available(&auth) &&
              /* publish->1, disable->2, replacement publish->3. */
              auth.admin.credential_generation == 3U,
          "replacement credential activates");
    memset(&decision, 0, sizeof(decision));
    CHECK(appliance_production_auth_authorize_request(
              &auth,
              header,
              sizeof(header),
              HTTP_METHOD_GET,
              system_target,
              sizeof(system_target) - 1U,
              20U,
              &decision,
              &route) == APPLIANCE_PRODUCTION_AUTH_DENIED &&
              !decision.authenticated,
          "rotation rejects the old bearer");
    CHECK(appliance_production_auth_reset(&auth) ==
              APPLIANCE_PRODUCTION_AUTH_OK &&
              appliance_production_auth_reset(&auth) ==
                  APPLIANCE_PRODUCTION_AUTH_OK,
          "repeated reset is stable");

    CHECK(appliance_production_auth_render(
              NULL, NULL, 0U, NULL) == APPLIANCE_PRODUCTION_AUTH_NULL,
          "render rejects null");
    CHECK(appliance_production_auth_render(
              &auth, NULL, 0U, NULL) == APPLIANCE_PRODUCTION_AUTH_NULL,
          "render rejects null output");
    CHECK(appliance_production_auth_state_string(
              APPLIANCE_PRODUCTION_AUTH_STATE_ACTIVE) != NULL &&
              strcmp(appliance_production_auth_state_string(
                         APPLIANCE_PRODUCTION_AUTH_STATE_ACTIVE),
                     "ACTIVE") == 0,
          "state string is stable");
}

int main(void)
{
    test_init_and_unavailable();
    test_anonymous_deny_authorized_allow_and_default_deny();
    test_stop_reset_and_rotation();

    if (failures != 0) {
        fprintf(stderr,
                "Appliance production auth host tests: %d FAILED\n",
                failures);
        return 1;
    }
    printf("Appliance production auth host tests: ALL OK\n");
    return 0;
}

#else

typedef int appliance_production_auth_host_translation_unit_guard;

#endif
