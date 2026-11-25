#include "integration_principal.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifdef HFOC_INTEGRATION_PRINCIPAL_HOST_TEST

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

void *kmalloc(size_t size) {
    (void)size;
    ++alloc_attempts;
    return NULL;
}

bool kfree(void *ptr) {
    (void)ptr;
    ++free_attempts;
    return false;
}

static struct authorization_entry make_entry(
    uint32_t command_id,
    const uint8_t *roles,
    uint32_t role_count,
    const uint8_t *capabilities,
    uint32_t capability_count)
{
    struct authorization_entry entry;
    uint32_t index;

    memset(&entry, 0, sizeof(entry));
    entry.version = AUTHORIZATION_VERSION;
    entry.command_id = command_id;
    entry.role_count = role_count;
    for (index = 0U; index < role_count; ++index) {
        entry.roles[index] = roles[index];
    }
    entry.capability_count = capability_count;
    for (index = 0U; index < capability_count; ++index) {
        entry.capabilities[index] = capabilities[index];
    }
    return entry;
}

static struct authorization_matrix make_matrix(void)
{
    static const uint8_t admin_role[] = { AUTHORIZATION_ROLE_ADMIN };
    static const uint8_t status_cap[] = {
        AUTHORIZATION_CAPABILITY_STATUS_READ
    };
    static const uint8_t config_cap[] = {
        AUTHORIZATION_CAPABILITY_CONFIG_WRITE
    };
    struct authorization_entry entries[3];
    struct authorization_matrix matrix;

    entries[0] = make_entry(1U, admin_role, 1U, status_cap, 1U);
    entries[1] = make_entry(2U, admin_role, 1U, config_cap, 1U);
    entries[2] = make_entry(3U, NULL, 0U, config_cap, 1U);
    CHECK(authorization_matrix_init(&matrix, entries, 3U) ==
              AUTHORIZATION_OK,
          "integration principal authorization fixture initializes");
    return matrix;
}

static struct integration_principal_scope make_scope(
    uint32_t version,
    uint8_t role,
    uint8_t capability)
{
    struct integration_principal_scope scope;

    memset(&scope, 0, sizeof(scope));
    scope.version = version;
    scope.role = role;
    scope.capability = capability;
    return scope;
}

static void check_init_failure(
    const struct integration_principal_scope *scope,
    enum integration_principal_status expected,
    const char *message)
{
    struct integration_principal_scope published;
    unsigned char original[sizeof(published)];

    memset(&published, 0xA5, sizeof(published));
    memcpy(original, &published, sizeof(original));
    CHECK(integration_principal_scope_init(&published, scope) == expected &&
              memcmp(&published, original, sizeof(original)) == 0,
          message);
}

static void test_exact_ids(void)
{
    CHECK(AUTHORIZATION_ROLE_INTEGRATION == 4U,
          "integration role ID is exactly 4");
    CHECK(AUTHORIZATION_CAPABILITY_INTEGRATION_SESSION == 4U,
          "integration session capability ID is exactly 4");
    CHECK(AUTHORIZATION_ROLE_INTEGRATION != AUTHORIZATION_ROLE_ADMIN &&
              AUTHORIZATION_ROLE_INTEGRATION !=
                  AUTHORIZATION_ROLE_OPERATOR &&
              AUTHORIZATION_ROLE_INTEGRATION !=
                  AUTHORIZATION_ROLE_AUDITOR,
          "integration role is distinct from local admin/operator/auditor");
    CHECK(AUTHORIZATION_CAPABILITY_INTEGRATION_SESSION !=
              AUTHORIZATION_CAPABILITY_STATUS_READ &&
              AUTHORIZATION_CAPABILITY_INTEGRATION_SESSION !=
                  AUTHORIZATION_CAPABILITY_CONFIG_WRITE &&
              AUTHORIZATION_CAPABILITY_INTEGRATION_SESSION !=
                  AUTHORIZATION_CAPABILITY_AUDIT_READ,
          "integration capability is distinct from local control capabilities");
}

static void test_validation_and_publication(void)
{
    struct integration_principal_scope valid = make_scope(
        INTEGRATION_PRINCIPAL_VERSION,
        AUTHORIZATION_ROLE_INTEGRATION,
        AUTHORIZATION_CAPABILITY_INTEGRATION_SESSION);
    struct integration_principal_scope bad_version = make_scope(
        INTEGRATION_PRINCIPAL_VERSION + 1U,
        AUTHORIZATION_ROLE_INTEGRATION,
        AUTHORIZATION_CAPABILITY_INTEGRATION_SESSION);
    struct integration_principal_scope admin_role = make_scope(
        INTEGRATION_PRINCIPAL_VERSION,
        AUTHORIZATION_ROLE_ADMIN,
        AUTHORIZATION_CAPABILITY_INTEGRATION_SESSION);
    struct integration_principal_scope config_capability = make_scope(
        INTEGRATION_PRINCIPAL_VERSION,
        AUTHORIZATION_ROLE_INTEGRATION,
        AUTHORIZATION_CAPABILITY_CONFIG_WRITE);
    struct integration_principal_scope published;
    struct integration_principal_scope source;

    CHECK(integration_principal_scope_validate(NULL) ==
              INTEGRATION_PRINCIPAL_NULL,
          "integration scope validate rejects null scope");
    CHECK(integration_principal_scope_validate(&valid) ==
              INTEGRATION_PRINCIPAL_OK,
          "integration scope validate accepts exact scope");
    CHECK(integration_principal_scope_validate(&bad_version) ==
              INTEGRATION_PRINCIPAL_BAD_VERSION,
          "integration scope validate rejects unknown version");
    CHECK(integration_principal_scope_validate(&admin_role) ==
              INTEGRATION_PRINCIPAL_INVALID_SCOPE,
          "integration scope validate rejects local admin role");
    CHECK(integration_principal_scope_validate(&config_capability) ==
              INTEGRATION_PRINCIPAL_INVALID_SCOPE,
          "integration scope validate rejects local config capability");
    CHECK(integration_principal_scope_init(NULL, NULL) ==
              INTEGRATION_PRINCIPAL_NULL,
          "integration scope init rejects null output and source");
    check_init_failure(&bad_version, INTEGRATION_PRINCIPAL_BAD_VERSION,
                       "integration scope bad version init writes nothing");
    check_init_failure(&admin_role, INTEGRATION_PRINCIPAL_INVALID_SCOPE,
                       "integration scope admin role init writes nothing");
    check_init_failure(
        &config_capability, INTEGRATION_PRINCIPAL_INVALID_SCOPE,
        "integration scope config capability init writes nothing");

    memset(&published, 0, sizeof(published));
    source = valid;
    CHECK(integration_principal_scope_init(&published, &source) ==
              INTEGRATION_PRINCIPAL_OK &&
              published.version == valid.version &&
              published.role == valid.role &&
              published.capability == valid.capability,
          "integration scope init publishes exact immutable copy");
    source.role = AUTHORIZATION_ROLE_OPERATOR;
    CHECK(published.role == AUTHORIZATION_ROLE_INTEGRATION,
          "published integration scope is not changed by source mutation");
}

static void test_local_admin_denial(void)
{
    struct authorization_matrix matrix = make_matrix();
    struct integration_principal_scope integration = make_scope(
        INTEGRATION_PRINCIPAL_VERSION,
        AUTHORIZATION_ROLE_INTEGRATION,
        AUTHORIZATION_CAPABILITY_INTEGRATION_SESSION);
    uint8_t admin_role[] = { AUTHORIZATION_ROLE_ADMIN };
    uint8_t status_cap[] = { AUTHORIZATION_CAPABILITY_STATUS_READ };
    uint8_t config_cap[] = { AUTHORIZATION_CAPABILITY_CONFIG_WRITE };

    CHECK(integration_principal_scope_check(NULL, &matrix, 1U) ==
              INTEGRATION_PRINCIPAL_NULL,
          "integration scope check rejects null scope");
    CHECK(integration_principal_scope_check(&integration, NULL, 1U) ==
              INTEGRATION_PRINCIPAL_NULL,
          "integration scope check rejects null matrix");
    CHECK(integration_principal_scope_check(
              &integration, &matrix, 1U) ==
              INTEGRATION_PRINCIPAL_MISSING_ROLE,
          "integration scope cannot access read-only status command");
    CHECK(integration_principal_scope_check(
              &integration, &matrix, 2U) ==
              INTEGRATION_PRINCIPAL_MISSING_ROLE,
          "integration scope cannot access controlled configuration command");
    CHECK(integration_principal_scope_check(
              &integration, &matrix, 3U) ==
              INTEGRATION_PRINCIPAL_MISSING_CAPABILITY,
          "integration scope cannot satisfy local config-write capability");
    CHECK(integration_principal_scope_check(
              &integration, &matrix, 200U) ==
              INTEGRATION_PRINCIPAL_BAD_STATE,
          "integration scope check rejects unknown command");
    CHECK(authorization_check(
              &matrix, 1U, admin_role, 1U, status_cap, 1U) ==
              AUTHORIZATION_OK &&
              authorization_check(
                  &matrix, 2U, admin_role, 1U, config_cap, 1U) ==
                  AUTHORIZATION_OK,
          "admin caller still passes local-admin-only authorization");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "integration scope check performs no allocation");
}

static const char *expected_status_string(
    enum integration_principal_status status)
{
    switch (status) {
    case INTEGRATION_PRINCIPAL_OK:
        return "INTEGRATION_PRINCIPAL_OK";
    case INTEGRATION_PRINCIPAL_NULL:
        return "INTEGRATION_PRINCIPAL_NULL";
    case INTEGRATION_PRINCIPAL_BAD_VERSION:
        return "INTEGRATION_PRINCIPAL_BAD_VERSION";
    case INTEGRATION_PRINCIPAL_INVALID_SCOPE:
        return "INTEGRATION_PRINCIPAL_INVALID_SCOPE";
    case INTEGRATION_PRINCIPAL_MISSING_ROLE:
        return "INTEGRATION_PRINCIPAL_MISSING_ROLE";
    case INTEGRATION_PRINCIPAL_MISSING_CAPABILITY:
        return "INTEGRATION_PRINCIPAL_MISSING_CAPABILITY";
    case INTEGRATION_PRINCIPAL_BAD_STATE:
        return "INTEGRATION_PRINCIPAL_BAD_STATE";
    default:
        return NULL;
    }
}

static void test_status_strings(void)
{
    enum integration_principal_status status;

    for (status = INTEGRATION_PRINCIPAL_OK;
         status <= INTEGRATION_PRINCIPAL_BAD_STATE; ++status) {
        const char *string = integration_principal_status_string(status);
        const char *expected = expected_status_string(status);

        CHECK(string != NULL && expected != NULL &&
                  strcmp(string, expected) == 0,
              "integration principal status string is exact");
    }
    CHECK(integration_principal_status_string(
              (enum integration_principal_status)7U) == NULL,
          "integration principal undefined status is rejected");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "integration principal status handling performs no allocation");
}

int main(void)
{
    test_exact_ids();
    test_validation_and_publication();
    test_local_admin_denial();
    test_status_strings();

    if (failures != 0) {
        fprintf(stderr, "Integration principal host tests: FAILED (%d)\n",
                failures);
        return 1;
    }
    printf("Integration principal host tests: ALL OK\n");
    return 0;
}

#endif
