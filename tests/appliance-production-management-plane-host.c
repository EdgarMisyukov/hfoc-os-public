#include "appliance_production_management_plane.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifdef HFOC_APPLIANCE_PRODUCTION_MANAGEMENT_PLANE_HOST_TEST

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

static void make_token(
    uint8_t token[APPLIANCE_ADMIN_AUTH_TOKEN_LENGTH],
    uint8_t seed)
{
    size_t index;

    for (index = 0U; index < APPLIANCE_ADMIN_AUTH_TOKEN_LENGTH; ++index) {
        token[index] = (uint8_t)(seed + (uint8_t)index);
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

static void make_evidence(
    struct appliance_production_management_evidence *evidence,
    uint64_t generation,
    bool degraded)
{
    memset(evidence, 0, sizeof(*evidence));
    evidence->version = APPLIANCE_PRODUCTION_MANAGEMENT_EVIDENCE_VERSION;
    evidence->published = true;
    evidence->source_generation = generation;
    evidence->system_identity_present = true;
    evidence->configuration_ready = true;
    evidence->network_required = true;
    evidence->network_active = true;
    evidence->network_health_ok = true;
    evidence->production_services_ready = true;
    evidence->https_management_ready = true;
    evidence->authorization_available = true;
    evidence->degraded_requested = degraded;
}

static void test_init_and_fail_closed(void)
{
    struct appliance_production_management management;
    struct appliance_production_auth auth;
    struct appliance_production_management_plane plane;
    char output[APPLIANCE_PRODUCTION_MANAGEMENT_PLANE_OUTPUT_MAX + 1U];
    size_t output_length = 0U;
    static const uint8_t system_target[] = "/api/v1/system";

    memset(&management, 0, sizeof(management));
    memset(&auth, 0, sizeof(auth));
    memset(&plane, 0, sizeof(plane));
    CHECK(appliance_production_management_plane_init(NULL, &management, &auth) ==
              APPLIANCE_PRODUCTION_MANAGEMENT_PLANE_NULL,
          "plane init rejects null plane");
    CHECK(appliance_production_management_plane_init(&plane, NULL, &auth) ==
              APPLIANCE_PRODUCTION_MANAGEMENT_PLANE_NULL,
          "plane init rejects null management");
    CHECK(appliance_production_management_plane_init(&plane, &management, NULL) ==
              APPLIANCE_PRODUCTION_MANAGEMENT_PLANE_NULL,
          "plane init rejects null auth");
    CHECK(appliance_production_management_plane_ready(&plane) == false,
          "uninitialized plane is not ready");

    CHECK(appliance_production_management_init(&management) ==
              APPLIANCE_PRODUCTION_MANAGEMENT_OK,
          "management composition initializes");
    CHECK(appliance_production_auth_init(&auth) == APPLIANCE_PRODUCTION_AUTH_OK,
          "auth composition initializes");
    CHECK(appliance_production_management_plane_init(&plane, &management, &auth) ==
              APPLIANCE_PRODUCTION_MANAGEMENT_PLANE_OK,
          "plane binds both compositions");
    CHECK(appliance_production_management_plane_state(&plane) ==
              APPLIANCE_PRODUCTION_MANAGEMENT_PLANE_STATE_PREPARED,
          "bound plane is prepared");
    CHECK(appliance_production_management_plane_ready(&plane) == false,
          "prepared plane without evidence is not ready");

    output[0] = 'x';
    output_length = 123U;
    CHECK(appliance_production_management_plane_admit(
              &plane, NULL, 0U, HTTP_METHOD_GET,
              system_target, sizeof(system_target) - 1U, 1U,
              output, sizeof(output), &output_length) ==
              APPLIANCE_PRODUCTION_MANAGEMENT_PLANE_UNAVAILABLE,
          "missing credential keeps the read-only plane unavailable");
    CHECK(output[0] == 'x' && output_length == 123U,
          "unavailable admission writes no response");
    CHECK(appliance_production_management_plane_unavailable_count(&plane) == 1U,
          "unavailable admission is counted");
}

static void test_readonly_admission(void)
{
    struct appliance_production_management management;
    struct appliance_production_auth auth;
    struct appliance_production_management_plane plane;
    struct appliance_admin_auth_credential credential;
    struct appliance_admin_auth_proof proof;
    struct appliance_production_management_evidence evidence;
    uint8_t token[APPLIANCE_ADMIN_AUTH_TOKEN_LENGTH];
    uint8_t header[APPLIANCE_ADMIN_AUTH_BEARER_PREFIX_LENGTH +
                   APPLIANCE_ADMIN_AUTH_TOKEN_LENGTH];
    uint8_t other_token[APPLIANCE_ADMIN_AUTH_TOKEN_LENGTH];
    uint8_t other_header[APPLIANCE_ADMIN_AUTH_BEARER_PREFIX_LENGTH +
                         APPLIANCE_ADMIN_AUTH_TOKEN_LENGTH];
    char output[APPLIANCE_PRODUCTION_MANAGEMENT_PLANE_OUTPUT_MAX + 1U];
    size_t output_length = 0U;
    uint32_t session_id = 0U;
    static const uint8_t discovery_target[] = "/api/v1";
    static const uint8_t system_target[] = "/api/v1/system";
    static const uint8_t unknown_target[] = "/api/v1/unknown";

    memset(&management, 0, sizeof(management));
    memset(&auth, 0, sizeof(auth));
    memset(&plane, 0, sizeof(plane));
    memset(output, 0, sizeof(output));
    CHECK(appliance_production_management_init(&management) ==
              APPLIANCE_PRODUCTION_MANAGEMENT_OK &&
          appliance_production_auth_init(&auth) == APPLIANCE_PRODUCTION_AUTH_OK &&
          appliance_production_management_plane_init(&plane, &management, &auth) ==
              APPLIANCE_PRODUCTION_MANAGEMENT_PLANE_OK,
          "plane binding succeeds");

    make_credential(&credential, 0x11U);
    make_proof(&proof, 0x11U);
    make_token(token, 0x21U);
    make_token(other_token, 0x5AU);
    make_bearer_header(header, token);
    make_bearer_header(other_header, other_token);
    make_evidence(&evidence, 1U, false);

    CHECK(appliance_production_management_publish(&management, &evidence) ==
              APPLIANCE_PRODUCTION_MANAGEMENT_OK,
          "management evidence publishes");
    CHECK(appliance_production_auth_publish_credential(&auth, &credential) ==
              APPLIANCE_PRODUCTION_AUTH_OK,
          "administrator credential publishes");
    CHECK(appliance_admin_auth_authenticate_local(
              &auth.admin, &proof, token, 10U, 100U, &session_id) ==
              APPLIANCE_ADMIN_AUTH_OK && session_id == 1U,
          "administrator session is issued");
    CHECK(appliance_production_management_plane_ready(&plane) == true,
          "published management and credential make the plane ready");
    CHECK(appliance_production_management_plane_state(&plane) ==
              APPLIANCE_PRODUCTION_MANAGEMENT_PLANE_STATE_READY,
          "ready plane publishes READY state");

    output_length = 0U;
    CHECK(appliance_production_management_plane_admit(
              &plane, NULL, 0U, HTTP_METHOD_GET,
              system_target, sizeof(system_target) - 1U, 1U,
              output, sizeof(output), &output_length) ==
              APPLIANCE_PRODUCTION_MANAGEMENT_PLANE_DENIED,
          "anonymous protected read is denied");
    CHECK(appliance_production_management_plane_denied_count(&plane) == 1U,
          "denied admission is counted");

    output_length = 0U;
    CHECK(appliance_production_management_plane_admit(
              &plane, header, sizeof(header), HTTP_METHOD_GET,
              system_target, sizeof(system_target) - 1U, 1U,
              output, sizeof(output), &output_length) ==
              APPLIANCE_PRODUCTION_MANAGEMENT_PLANE_ALLOWED,
          "authorized read-only status request is allowed");
    CHECK(output_length > 0U && output[output_length] == '\0',
          "allowed admission publishes a terminated record");
    CHECK(strstr(output, "production_management") != NULL,
          "allowed admission renders the canonical management record");
    CHECK(appliance_production_management_plane_allowed_count(&plane) == 1U,
          "allowed admission is counted");

    output_length = 0U;
    CHECK(appliance_production_management_plane_admit(
              &plane, header, sizeof(header), HTTP_METHOD_UNSUPPORTED,
              system_target, sizeof(system_target) - 1U, 1U,
              output, sizeof(output), &output_length) ==
              APPLIANCE_PRODUCTION_MANAGEMENT_PLANE_DENIED,
          "mutation method is denied by the read-only plane");
    CHECK(output_length == 0U,
          "denied mutation writes no response");

    output_length = 0U;
    CHECK(appliance_production_management_plane_admit(
              &plane, other_header, sizeof(other_header), HTTP_METHOD_GET,
              system_target, sizeof(system_target) - 1U, 1U,
              output, sizeof(output), &output_length) ==
              APPLIANCE_PRODUCTION_MANAGEMENT_PLANE_DENIED,
          "unknown bearer is denied");

    output_length = 0U;
    CHECK(appliance_production_management_plane_admit(
              &plane, header, sizeof(header), HTTP_METHOD_GET,
              unknown_target, sizeof(unknown_target) - 1U, 1U,
              output, sizeof(output), &output_length) ==
              APPLIANCE_PRODUCTION_MANAGEMENT_PLANE_DENIED,
          "unknown target is denied by default");

    output_length = 0U;
    CHECK(appliance_production_management_plane_admit(
              &plane, NULL, 0U, HTTP_METHOD_GET,
              discovery_target, sizeof(discovery_target) - 1U, 1U,
              output, sizeof(output), &output_length) ==
              APPLIANCE_PRODUCTION_MANAGEMENT_PLANE_ALLOWED,
          "public discovery target stays anonymous");
}

static void test_buffer_and_degraded(void)
{
    struct appliance_production_management management;
    struct appliance_production_auth auth;
    struct appliance_production_management_plane plane;
    struct appliance_admin_auth_credential credential;
    struct appliance_admin_auth_proof proof;
    struct appliance_production_management_evidence evidence;
    uint8_t token[APPLIANCE_ADMIN_AUTH_TOKEN_LENGTH];
    uint8_t header[APPLIANCE_ADMIN_AUTH_BEARER_PREFIX_LENGTH +
                   APPLIANCE_ADMIN_AUTH_TOKEN_LENGTH];
    char small[4];
    char output[APPLIANCE_PRODUCTION_MANAGEMENT_PLANE_OUTPUT_MAX + 1U];
    size_t output_length = 0U;
    uint32_t session_id = 0U;
    static const uint8_t system_target[] = "/api/v1/system";

    memset(&management, 0, sizeof(management));
    memset(&auth, 0, sizeof(auth));
    memset(&plane, 0, sizeof(plane));
    make_credential(&credential, 0x21U);
    make_proof(&proof, 0x21U);
    make_token(token, 0x31U);
    make_bearer_header(header, token);
    make_evidence(&evidence, 7U, true);

    CHECK(appliance_production_management_init(&management) ==
              APPLIANCE_PRODUCTION_MANAGEMENT_OK &&
          appliance_production_auth_init(&auth) == APPLIANCE_PRODUCTION_AUTH_OK &&
          appliance_production_management_plane_init(&plane, &management, &auth) ==
              APPLIANCE_PRODUCTION_MANAGEMENT_PLANE_OK &&
          appliance_production_management_publish(&management, &evidence) ==
              APPLIANCE_PRODUCTION_MANAGEMENT_OK &&
          appliance_production_auth_publish_credential(&auth, &credential) ==
              APPLIANCE_PRODUCTION_AUTH_OK,
          "degraded plane binding succeeds");
    CHECK(appliance_admin_auth_authenticate_local(
              &auth.admin, &proof, token, 10U, 100U, &session_id) ==
              APPLIANCE_ADMIN_AUTH_OK && session_id == 1U,
          "degraded administrator session is issued");
    CHECK(appliance_production_management_plane_state(&plane) ==
              APPLIANCE_PRODUCTION_MANAGEMENT_PLANE_STATE_DEGRADED,
          "degraded management evidence publishes DEGRADED, not READY");
    CHECK(appliance_production_management_plane_ready(&plane) == true,
          "degraded plane still admits read-only requests");

    output_length = 0U;
    CHECK(appliance_production_management_plane_admit(
              &plane, header, sizeof(header), HTTP_METHOD_GET,
              system_target, sizeof(system_target) - 1U, 1U,
              small, sizeof(small), &output_length) ==
              APPLIANCE_PRODUCTION_MANAGEMENT_PLANE_BUFFER_TOO_SMALL,
          "small response buffer is rejected");
    CHECK(output_length == 0U,
          "rejected buffer reports no response length");

    output_length = 0U;
    CHECK(appliance_production_management_plane_admit(
              &plane, header, sizeof(header), HTTP_METHOD_GET,
              system_target, sizeof(system_target) - 1U, 1U,
              output, sizeof(output), &output_length) ==
              APPLIANCE_PRODUCTION_MANAGEMENT_PLANE_ALLOWED &&
          strstr(output, "state=DEGRADED") != NULL,
          "degraded read-only admission renders DEGRADED honestly");

    CHECK(appliance_production_management_plane_fail_closed(NULL) ==
              APPLIANCE_PRODUCTION_MANAGEMENT_PLANE_NULL,
          "fail-closed rejects null plane");
    CHECK(appliance_production_management_plane_admit(
              &plane, header, sizeof(header), HTTP_METHOD_GET,
              system_target, sizeof(system_target) - 1U, 1U,
              output, sizeof(output), &output_length) ==
              APPLIANCE_PRODUCTION_MANAGEMENT_PLANE_ALLOWED,
          "plane keeps serving after a rejected buffer");
}

int main(void)
{
    test_init_and_fail_closed();
    test_readonly_admission();
    test_buffer_and_degraded();

    if (failures != 0) {
        fprintf(stderr,
                "Appliance production management plane host tests: %d FAILED\n",
                failures);
        return 1;
    }
    printf("Appliance production management plane host tests: ALL OK\n");
    return 0;
}

#endif
