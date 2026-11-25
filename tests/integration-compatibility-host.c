#include "integration_compatibility.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifdef HFOC_INTEGRATION_COMPATIBILITY_HOST_TEST

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

static void fill_nonce(uint8_t *nonce, uint8_t first_byte)
{
    memset(nonce, 0, INTEGRATION_SESSION_NONCE_SIZE);
    nonce[0] = first_byte;
}

static struct integration_compatibility_policy make_policy(void)
{
    struct integration_compatibility_policy policy;

    memset(&policy, 0xA5, sizeof(policy));
    CHECK(integration_compatibility_policy_init(&policy) ==
              INTEGRATION_COMPATIBILITY_OK,
          "integration compatibility fixture initializes policy");
    return policy;
}

static struct integration_compatibility_request make_request(
    uint32_t schema_version,
    uint32_t transport_version)
{
    struct integration_compatibility_request request;

    memset(&request, 0, sizeof(request));
    request.schema_version = schema_version;
    request.transport_version = transport_version;
    return request;
}

static struct integration_session_request make_session_request(
    uint32_t schema_version,
    uint32_t transport_version,
    uint32_t peer_id,
    uint8_t first_nonce_byte)
{
    struct integration_session_request request;

    memset(&request, 0, sizeof(request));
    request.schema_version = schema_version;
    request.transport_version = transport_version;
    request.peer_id = peer_id;
    fill_nonce(request.nonce, first_nonce_byte);
    return request;
}

static struct integration_session_nonce_registry make_registry(
    uint32_t capacity)
{
    struct integration_session_nonce_registry registry;

    memset(&registry, 0xA5, sizeof(registry));
    CHECK(integration_session_nonce_registry_init(&registry, capacity) ==
              INTEGRATION_SESSION_OK,
          "integration compatibility fixture initializes session registry");
    return registry;
}

static void capture_policy(
    const struct integration_compatibility_policy *policy,
    unsigned char *original)
{
    memcpy(original, policy, sizeof(*policy));
}

static void check_policy_unchanged(
    const struct integration_compatibility_policy *policy,
    const unsigned char *original,
    const char *message)
{
    CHECK(memcmp(policy, original, sizeof(*policy)) == 0, message);
}

static void capture_registry(
    const struct integration_session_nonce_registry *registry,
    unsigned char *original)
{
    memcpy(original, registry, sizeof(*registry));
}

static void check_registry_unchanged(
    const struct integration_session_nonce_registry *registry,
    const unsigned char *original,
    const char *message)
{
    CHECK(memcmp(registry, original, sizeof(*registry)) == 0, message);
}

static void check_no_allocation(void)
{
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "integration compatibility performs no allocation");
}

static void test_exact_constants(void)
{
    CHECK(INTEGRATION_COMPATIBILITY_VERSION == 1U,
          "integration compatibility policy version is exactly 1");
    CHECK(INTEGRATION_COMPATIBILITY_SCHEMA_VERSION_MIN ==
              INTEGRATION_BOUNDARY_SCHEMA_VERSION_MIN &&
              INTEGRATION_COMPATIBILITY_SCHEMA_VERSION_MIN == 1U &&
              INTEGRATION_COMPATIBILITY_SCHEMA_VERSION_MAX ==
                  INTEGRATION_BOUNDARY_SCHEMA_VERSION_MAX &&
              INTEGRATION_COMPATIBILITY_SCHEMA_VERSION_MAX == 1U,
          "integration compatibility schema range is exact version 1");
    CHECK(INTEGRATION_COMPATIBILITY_TRANSPORT_VERSION_MIN ==
              INTEGRATION_BOUNDARY_TRANSPORT_VERSION &&
              INTEGRATION_COMPATIBILITY_TRANSPORT_VERSION_MIN == 1U &&
              INTEGRATION_COMPATIBILITY_TRANSPORT_VERSION_MAX ==
                  INTEGRATION_BOUNDARY_TRANSPORT_VERSION &&
              INTEGRATION_COMPATIBILITY_TRANSPORT_VERSION_MAX == 1U,
          "integration compatibility transport range is exact version 1");
    CHECK(INTEGRATION_COMPATIBILITY_DEPRECATION_ACTIVE == 1U &&
              INTEGRATION_COMPATIBILITY_DEPRECATION_DEPRECATED == 2U,
          "integration compatibility deprecation states are exact");
    check_no_allocation();
}

static void test_policy_init(void)
{
    struct integration_compatibility_policy policy;

    CHECK(integration_compatibility_policy_init(NULL) ==
              INTEGRATION_COMPATIBILITY_NULL,
          "integration compatibility policy init rejects null policy");
    CHECK(integration_compatibility_policy_init(&policy) ==
              INTEGRATION_COMPATIBILITY_OK &&
              policy.version == INTEGRATION_COMPATIBILITY_VERSION &&
              policy.schema_version_min ==
                  INTEGRATION_COMPATIBILITY_SCHEMA_VERSION_MIN &&
              policy.schema_version_max ==
                  INTEGRATION_COMPATIBILITY_SCHEMA_VERSION_MAX &&
              policy.transport_version_min ==
                  INTEGRATION_COMPATIBILITY_TRANSPORT_VERSION_MIN &&
              policy.transport_version_max ==
                  INTEGRATION_COMPATIBILITY_TRANSPORT_VERSION_MAX &&
              policy.deprecation_state ==
                  INTEGRATION_COMPATIBILITY_DEPRECATION_ACTIVE,
          "integration compatibility policy init publishes exact active policy");
    check_no_allocation();
}

static void test_policy_validate(void)
{
    struct integration_compatibility_policy policy = make_policy();
    struct integration_compatibility_policy bad = policy;
    unsigned char original[sizeof(policy)];

    capture_policy(&policy, original);
    CHECK(integration_compatibility_policy_validate(NULL) ==
              INTEGRATION_COMPATIBILITY_NULL,
          "integration compatibility policy validate rejects null policy");
    check_policy_unchanged(&policy, original,
                           "null policy validation leaves policy unchanged");

    bad = policy;
    bad.version = 0U;
    CHECK(integration_compatibility_policy_validate(&bad) ==
              INTEGRATION_COMPATIBILITY_BAD_VERSION,
          "integration compatibility policy validate rejects bad version");
    bad = policy;
    bad.schema_version_min = 0U;
    CHECK(integration_compatibility_policy_validate(&bad) ==
              INTEGRATION_COMPATIBILITY_BAD_STATE,
          "integration compatibility policy validate rejects low schema minimum");
    bad = policy;
    bad.schema_version_max = 2U;
    CHECK(integration_compatibility_policy_validate(&bad) ==
              INTEGRATION_COMPATIBILITY_BAD_STATE,
          "integration compatibility policy validate rejects high schema maximum");
    bad = policy;
    bad.schema_version_min = 2U;
    bad.schema_version_max = 1U;
    CHECK(integration_compatibility_policy_validate(&bad) ==
              INTEGRATION_COMPATIBILITY_BAD_STATE,
          "integration compatibility policy validate rejects inverted schema range");
    bad = policy;
    bad.transport_version_min = 0U;
    CHECK(integration_compatibility_policy_validate(&bad) ==
              INTEGRATION_COMPATIBILITY_BAD_STATE,
          "integration compatibility policy validate rejects low transport minimum");
    bad = policy;
    bad.transport_version_max = 2U;
    CHECK(integration_compatibility_policy_validate(&bad) ==
              INTEGRATION_COMPATIBILITY_BAD_STATE,
          "integration compatibility policy validate rejects high transport maximum");
    bad = policy;
    bad.transport_version_min = 2U;
    bad.transport_version_max = 1U;
    CHECK(integration_compatibility_policy_validate(&bad) ==
              INTEGRATION_COMPATIBILITY_BAD_STATE,
          "integration compatibility policy validate rejects inverted transport range");
    bad = policy;
    bad.deprecation_state = 0U;
    CHECK(integration_compatibility_policy_validate(&bad) ==
              INTEGRATION_COMPATIBILITY_BAD_STATE,
          "integration compatibility policy validate rejects zero deprecation state");
    bad = policy;
    bad.deprecation_state = 3U;
    CHECK(integration_compatibility_policy_validate(&bad) ==
              INTEGRATION_COMPATIBILITY_BAD_STATE,
          "integration compatibility policy validate rejects unknown deprecation state");
    CHECK(integration_compatibility_policy_validate(&policy) ==
              INTEGRATION_COMPATIBILITY_OK,
          "integration compatibility policy validate accepts exact active policy");
    check_no_allocation();
}

static void test_policy_deprecate(void)
{
    struct integration_compatibility_policy policy = make_policy();
    struct integration_compatibility_policy bad = policy;
    unsigned char original[sizeof(policy)];

    bad.version = 0U;
    capture_policy(&bad, original);
    CHECK(integration_compatibility_policy_deprecate(NULL) ==
              INTEGRATION_COMPATIBILITY_NULL,
          "integration compatibility policy deprecate rejects null policy");
    CHECK(integration_compatibility_policy_deprecate(&bad) ==
              INTEGRATION_COMPATIBILITY_BAD_VERSION,
          "integration compatibility policy deprecate rejects bad version");
    check_policy_unchanged(&bad, original,
                           "invalid policy deprecation leaves policy unchanged");

    CHECK(integration_compatibility_policy_deprecate(&policy) ==
              INTEGRATION_COMPATIBILITY_OK &&
              policy.deprecation_state ==
                  INTEGRATION_COMPATIBILITY_DEPRECATION_DEPRECATED &&
              policy.schema_version_min ==
                  INTEGRATION_COMPATIBILITY_SCHEMA_VERSION_MIN &&
              policy.schema_version_max ==
                  INTEGRATION_COMPATIBILITY_SCHEMA_VERSION_MAX,
          "integration compatibility policy deprecate publishes deprecated state");

    capture_policy(&policy, original);
    CHECK(integration_compatibility_policy_deprecate(&policy) ==
              INTEGRATION_COMPATIBILITY_BAD_STATE,
          "integration compatibility policy deprecate rejects second deprecation");
    check_policy_unchanged(&policy, original,
                           "second deprecation leaves deprecated policy unchanged");
    check_no_allocation();
}

static void test_check(void)
{
    struct integration_compatibility_policy policy = make_policy();
    struct integration_compatibility_policy deprecated = policy;
    struct integration_compatibility_request valid = make_request(1U, 1U);
    struct integration_compatibility_request bad_schema_low =
        make_request(0U, 1U);
    struct integration_compatibility_request bad_schema_high =
        make_request(2U, 1U);
    struct integration_compatibility_request bad_transport_low =
        make_request(1U, 0U);
    struct integration_compatibility_request bad_transport_high =
        make_request(1U, 2U);

    CHECK(integration_compatibility_check(NULL, &valid) ==
              INTEGRATION_COMPATIBILITY_NULL,
          "integration compatibility check rejects null policy");
    CHECK(integration_compatibility_check(&policy, NULL) ==
              INTEGRATION_COMPATIBILITY_NULL,
          "integration compatibility check rejects null request");
    CHECK(integration_compatibility_check(&policy, &valid) ==
              INTEGRATION_COMPATIBILITY_OK,
          "integration compatibility check accepts exact active request");
    CHECK(integration_compatibility_check(
              &policy, &bad_schema_low) ==
              INTEGRATION_COMPATIBILITY_UNSUPPORTED_SCHEMA_VERSION,
          "integration compatibility check rejects low schema version");
    CHECK(integration_compatibility_check(
              &policy, &bad_schema_high) ==
              INTEGRATION_COMPATIBILITY_UNSUPPORTED_SCHEMA_VERSION,
          "integration compatibility check rejects high schema version");
    CHECK(integration_compatibility_check(
              &policy, &bad_transport_low) ==
              INTEGRATION_COMPATIBILITY_UNSUPPORTED_TRANSPORT_VERSION,
          "integration compatibility check rejects low transport version");
    CHECK(integration_compatibility_check(
              &policy, &bad_transport_high) ==
              INTEGRATION_COMPATIBILITY_UNSUPPORTED_TRANSPORT_VERSION,
          "integration compatibility check rejects high transport version");
    CHECK(integration_compatibility_policy_deprecate(&deprecated) ==
              INTEGRATION_COMPATIBILITY_OK,
          "integration compatibility check fixture deprecates policy");
    CHECK(integration_compatibility_check(&deprecated, &valid) ==
              INTEGRATION_COMPATIBILITY_DEPRECATED,
          "integration compatibility check rejects deprecated policy");
    check_no_allocation();
}

static void check_session_rejection(
    const struct integration_compatibility_policy *policy,
    const struct integration_session_request *session_request,
    struct integration_session_nonce_registry *registry,
    enum integration_compatibility_status expected,
    const char *message)
{
    unsigned char original[sizeof(*registry)];

    capture_registry(registry, original);
    CHECK(integration_compatibility_session_validate(
              policy, session_request, registry) == expected,
          message);
    check_registry_unchanged(registry, original,
                             "rejected compatibility session leaves registry unchanged");
}

static void test_session_gate(void)
{
    struct integration_compatibility_policy policy = make_policy();
    struct integration_compatibility_policy deprecated = policy;
    struct integration_session_nonce_registry registry = make_registry(4U);
    struct integration_session_request valid = make_session_request(
        1U, 1U, 7U, 0x11U);
    struct integration_session_request bad_schema = make_session_request(
        0U, 1U, 7U, 0x11U);
    struct integration_session_request bad_transport = make_session_request(
        1U, 0U, 7U, 0x11U);
    struct integration_session_request invalid_peer = make_session_request(
        1U, 1U, 0U, 0x11U);

    CHECK(integration_compatibility_session_validate(
              NULL, &valid, &registry) == INTEGRATION_COMPATIBILITY_NULL,
          "compatibility session gate rejects null policy");
    CHECK(integration_compatibility_session_validate(
              &policy, NULL, &registry) == INTEGRATION_COMPATIBILITY_NULL,
          "compatibility session gate rejects null session request");
    CHECK(integration_compatibility_session_validate(
              &policy, &valid, NULL) == INTEGRATION_COMPATIBILITY_NULL,
          "compatibility session gate rejects null registry");

    check_session_rejection(
        &policy, &bad_schema, &registry,
        INTEGRATION_COMPATIBILITY_UNSUPPORTED_SCHEMA_VERSION,
        "compatibility session gate rejects unsupported schema without session");
    check_session_rejection(
        &policy, &bad_transport, &registry,
        INTEGRATION_COMPATIBILITY_UNSUPPORTED_TRANSPORT_VERSION,
        "compatibility session gate rejects unsupported transport without session");

    CHECK(integration_compatibility_policy_deprecate(&deprecated) ==
              INTEGRATION_COMPATIBILITY_OK,
          "integration compatibility session fixture deprecates policy");
    check_session_rejection(
        &deprecated, &valid, &registry,
        INTEGRATION_COMPATIBILITY_DEPRECATED,
        "compatibility session gate rejects deprecated policy without session");

    check_session_rejection(
        &policy, &invalid_peer, &registry,
        INTEGRATION_COMPATIBILITY_SESSION_REJECTED,
        "compatibility session gate rejects invalid session request");

    CHECK(integration_compatibility_session_validate(
              &policy, &valid, &registry) == INTEGRATION_COMPATIBILITY_OK &&
              registry.count == 1U &&
              registry.entries[0].peer_id == 7U &&
              memcmp(registry.entries[0].nonce,
                     valid.nonce,
                     INTEGRATION_SESSION_NONCE_SIZE) == 0,
          "compatibility session gate publishes exact live session nonce");
    check_no_allocation();
}

static const char *expected_status_string(
    enum integration_compatibility_status status)
{
    switch (status) {
    case INTEGRATION_COMPATIBILITY_OK:
        return "INTEGRATION_COMPATIBILITY_OK";
    case INTEGRATION_COMPATIBILITY_NULL:
        return "INTEGRATION_COMPATIBILITY_NULL";
    case INTEGRATION_COMPATIBILITY_BAD_VERSION:
        return "INTEGRATION_COMPATIBILITY_BAD_VERSION";
    case INTEGRATION_COMPATIBILITY_BAD_STATE:
        return "INTEGRATION_COMPATIBILITY_BAD_STATE";
    case INTEGRATION_COMPATIBILITY_UNSUPPORTED_SCHEMA_VERSION:
        return "INTEGRATION_COMPATIBILITY_UNSUPPORTED_SCHEMA_VERSION";
    case INTEGRATION_COMPATIBILITY_UNSUPPORTED_TRANSPORT_VERSION:
        return "INTEGRATION_COMPATIBILITY_UNSUPPORTED_TRANSPORT_VERSION";
    case INTEGRATION_COMPATIBILITY_DEPRECATED:
        return "INTEGRATION_COMPATIBILITY_DEPRECATED";
    case INTEGRATION_COMPATIBILITY_SESSION_REJECTED:
        return "INTEGRATION_COMPATIBILITY_SESSION_REJECTED";
    default:
        return NULL;
    }
}

static void test_status_strings(void)
{
    enum integration_compatibility_status status;

    for (status = INTEGRATION_COMPATIBILITY_OK;
         status <= INTEGRATION_COMPATIBILITY_SESSION_REJECTED; ++status) {
        const char *string = integration_compatibility_status_string(status);
        const char *expected = expected_status_string(status);

        CHECK(string != NULL && expected != NULL &&
                  strcmp(string, expected) == 0,
              "integration compatibility status string is exact");
    }
    CHECK(integration_compatibility_status_string(
              (enum integration_compatibility_status)8U) == NULL,
          "integration compatibility undefined status is rejected");
    check_no_allocation();
}

int main(void)
{
    test_exact_constants();
    test_policy_init();
    test_policy_validate();
    test_policy_deprecate();
    test_check();
    test_session_gate();
    test_status_strings();

    if (failures != 0) {
        fprintf(stderr, "Integration compatibility host tests: FAILED (%d)\n",
                failures);
        return 1;
    }
    printf("Integration compatibility host tests: ALL OK\n");
    return 0;
}

#endif
