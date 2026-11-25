#include "authorization.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifdef HFOC_AUTHORIZATION_HOST_TEST

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

static void init_test_entries(struct authorization_entry *entries)
{
    static const uint8_t admin_role[] = { AUTHORIZATION_ROLE_ADMIN };
    static const uint8_t status_cap[] = {
        AUTHORIZATION_CAPABILITY_STATUS_READ
    };
    static const uint8_t operator_role[] = {
        AUTHORIZATION_ROLE_OPERATOR
    };
    static const uint8_t config_cap[] = {
        AUTHORIZATION_CAPABILITY_CONFIG_WRITE
    };

    entries[0] = make_entry(1U, admin_role, 1U, status_cap, 1U);
    entries[1] = make_entry(2U, operator_role, 1U, NULL, 0U);
    entries[2] = make_entry(3U, NULL, 0U, config_cap, 1U);
}

static struct authorization_matrix make_matrix(void)
{
    struct authorization_entry entries[3];
    struct authorization_matrix matrix;

    init_test_entries(entries);
    CHECK(authorization_matrix_init(&matrix, entries, 3U) ==
              AUTHORIZATION_OK,
          "authorization matrix fixture initializes");
    return matrix;
}

static const char *expected_status_string(
    enum authorization_status status)
{
    switch (status) {
    case AUTHORIZATION_OK:
        return "AUTHORIZATION_OK";
    case AUTHORIZATION_NULL:
        return "AUTHORIZATION_NULL";
    case AUTHORIZATION_BAD_CAPACITY:
        return "AUTHORIZATION_BAD_CAPACITY";
    case AUTHORIZATION_BAD_VERSION:
        return "AUTHORIZATION_BAD_VERSION";
    case AUTHORIZATION_UNKNOWN_COMMAND:
        return "AUTHORIZATION_UNKNOWN_COMMAND";
    case AUTHORIZATION_INVALID_DESCRIPTOR:
        return "AUTHORIZATION_INVALID_DESCRIPTOR";
    case AUTHORIZATION_DUPLICATE_COMMAND:
        return "AUTHORIZATION_DUPLICATE_COMMAND";
    case AUTHORIZATION_INVALID_ROLE_SET:
        return "AUTHORIZATION_INVALID_ROLE_SET";
    case AUTHORIZATION_INVALID_CAPABILITY_SET:
        return "AUTHORIZATION_INVALID_CAPABILITY_SET";
    case AUTHORIZATION_MISSING_ROLE:
        return "AUTHORIZATION_MISSING_ROLE";
    case AUTHORIZATION_MISSING_CAPABILITY:
        return "AUTHORIZATION_MISSING_CAPABILITY";
    case AUTHORIZATION_BAD_STATE:
        return "AUTHORIZATION_BAD_STATE";
    default:
        return NULL;
    }
}

static void test_status_strings(void)
{
    enum authorization_status status;

    for (status = AUTHORIZATION_OK;
         status <= AUTHORIZATION_BAD_STATE; ++status) {
        const char *string = authorization_status_string(status);
        const char *expected = expected_status_string(status);

        CHECK(string != NULL && expected != NULL &&
                  strcmp(string, expected) == 0,
              "authorization status string is exact for every status");
    }
    CHECK(authorization_status_string(
              (enum authorization_status)12U) == NULL,
          "authorization undefined status is rejected");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "authorization status handling performs no allocation");
}

static void test_matrix_init(void)
{
    struct authorization_entry entries[3];
    struct authorization_entry bad_entry;
    struct authorization_matrix matrix;
    static const uint8_t admin_role[] = { AUTHORIZATION_ROLE_ADMIN };
    static const uint8_t status_cap[] = {
        AUTHORIZATION_CAPABILITY_STATUS_READ
    };
    static const uint8_t duplicate_roles[] = {
        AUTHORIZATION_ROLE_ADMIN, AUTHORIZATION_ROLE_ADMIN
    };
    static const uint8_t duplicate_caps[] = {
        AUTHORIZATION_CAPABILITY_STATUS_READ,
        AUTHORIZATION_CAPABILITY_STATUS_READ
    };
    unsigned char before[sizeof(matrix)];
    uint32_t index;

    init_test_entries(entries);

    CHECK(authorization_matrix_init(NULL, entries, 3U) ==
              AUTHORIZATION_NULL,
          "authorization matrix init rejects null matrix");
    CHECK(authorization_matrix_init(&matrix, NULL, 0U) ==
              AUTHORIZATION_BAD_CAPACITY,
          "authorization matrix init rejects zero capacity");
    CHECK(authorization_matrix_init(&matrix, NULL, 1U) ==
              AUTHORIZATION_NULL,
          "authorization matrix init rejects null entries");
    CHECK(authorization_matrix_init(
              &matrix, entries,
              AUTHORIZATION_MATRIX_CAPACITY_MAX + 1U) ==
              AUTHORIZATION_BAD_CAPACITY,
          "authorization matrix init rejects over-limit capacity");

    bad_entry = make_entry(1U, admin_role, 1U, status_cap, 1U);
    bad_entry.version = AUTHORIZATION_VERSION + 1U;
    CHECK(authorization_matrix_init(&matrix, &bad_entry, 1U) ==
              AUTHORIZATION_BAD_VERSION,
          "authorization matrix init rejects unknown entry version");

    bad_entry = make_entry(0U, admin_role, 1U, status_cap, 1U);
    CHECK(authorization_matrix_init(&matrix, &bad_entry, 1U) ==
              AUTHORIZATION_INVALID_DESCRIPTOR,
          "authorization matrix init rejects zero command id");

    bad_entry = make_entry(AUTHORIZATION_COMMAND_ID_MAX + 1U,
                           admin_role, 1U, status_cap, 1U);
    CHECK(authorization_matrix_init(&matrix, &bad_entry, 1U) ==
              AUTHORIZATION_INVALID_DESCRIPTOR,
          "authorization matrix init rejects over-limit command id");

    bad_entry = make_entry(1U, NULL, 0U, NULL, 0U);
    bad_entry.role_count = AUTHORIZATION_ROLE_SET_MAX + 1U;
    CHECK(authorization_matrix_init(&matrix, &bad_entry, 1U) ==
              AUTHORIZATION_INVALID_DESCRIPTOR,
          "authorization matrix init rejects over-limit role count");

    bad_entry = make_entry(1U, duplicate_roles, 2U, status_cap, 1U);
    CHECK(authorization_matrix_init(&matrix, &bad_entry, 1U) ==
              AUTHORIZATION_INVALID_DESCRIPTOR,
          "authorization matrix init rejects duplicate role");

    bad_entry = make_entry(1U, NULL, 0U, NULL, 0U);
    bad_entry.role_count = 1U;
    bad_entry.roles[0] = 0U;
    CHECK(authorization_matrix_init(&matrix, &bad_entry, 1U) ==
              AUTHORIZATION_INVALID_DESCRIPTOR,
          "authorization matrix init rejects zero role id");

    bad_entry = make_entry(1U, admin_role, 1U, NULL, 0U);
    bad_entry.capability_count =
        AUTHORIZATION_CAPABILITY_SET_MAX + 1U;
    CHECK(authorization_matrix_init(&matrix, &bad_entry, 1U) ==
              AUTHORIZATION_INVALID_DESCRIPTOR,
          "authorization matrix init rejects over-limit capability count");

    bad_entry = make_entry(1U, admin_role, 1U, duplicate_caps, 2U);
    CHECK(authorization_matrix_init(&matrix, &bad_entry, 1U) ==
              AUTHORIZATION_INVALID_DESCRIPTOR,
          "authorization matrix init rejects duplicate capability");

    bad_entry = make_entry(1U, admin_role, 1U, NULL, 0U);
    bad_entry.capability_count = 1U;
    bad_entry.capabilities[0] = 0U;
    CHECK(authorization_matrix_init(&matrix, &bad_entry, 1U) ==
              AUTHORIZATION_INVALID_DESCRIPTOR,
          "authorization matrix init rejects zero capability id");

    bad_entry = make_entry(1U, NULL, 0U, NULL, 0U);
    CHECK(authorization_matrix_init(&matrix, &bad_entry, 1U) ==
              AUTHORIZATION_INVALID_DESCRIPTOR,
          "authorization matrix init rejects empty requirement");

    init_test_entries(entries);
    entries[1].command_id = entries[0].command_id;
    memset(&matrix, 0x5A, sizeof(matrix));
    memcpy(before, &matrix, sizeof(before));
    CHECK(authorization_matrix_init(&matrix, entries, 3U) ==
              AUTHORIZATION_DUPLICATE_COMMAND &&
              memcmp(&matrix, before, sizeof(before)) == 0,
          "authorization matrix init rejects duplicates without mutation");

    init_test_entries(entries);
    memset(&matrix, 0, sizeof(matrix));
    CHECK(authorization_matrix_init(&matrix, entries, 3U) ==
              AUTHORIZATION_OK &&
              matrix.version == AUTHORIZATION_VERSION &&
              matrix.count == 3U &&
              matrix.initialized,
          "authorization matrix init publishes exact configuration");
    for (index = 0U; index < AUTHORIZATION_MATRIX_CAPACITY_MAX;
         ++index) {
        if (index < 3U) {
            CHECK(matrix.entries[index].version ==
                      AUTHORIZATION_VERSION &&
                      matrix.entries[index].command_id ==
                          entries[index].command_id &&
                      matrix.entries[index].role_count ==
                          entries[index].role_count &&
                      memcmp(matrix.entries[index].roles,
                             entries[index].roles,
                             sizeof(matrix.entries[index].roles)) == 0 &&
                      matrix.entries[index].capability_count ==
                          entries[index].capability_count &&
                      memcmp(matrix.entries[index].capabilities,
                             entries[index].capabilities,
                             sizeof(matrix.entries[index].capabilities)) == 0,
                  "authorization matrix init copies exact entries");
        } else {
            CHECK(matrix.entries[index].version == 0U &&
                      matrix.entries[index].command_id == 0U &&
                      matrix.entries[index].role_count == 0U &&
                      matrix.entries[index].capability_count == 0U,
                  "authorization matrix init zeroes unused entries");
        }
    }
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "authorization matrix init performs no allocation");
}

static void test_check_failures(void)
{
    struct authorization_matrix matrix = make_matrix();
    struct authorization_matrix bad_matrix = matrix;
    uint8_t roles[AUTHORIZATION_ROLE_SET_MAX + 1U] = {
        AUTHORIZATION_ROLE_ADMIN,
        AUTHORIZATION_ROLE_OPERATOR,
        AUTHORIZATION_ROLE_AUDITOR
    };
    uint8_t capabilities[AUTHORIZATION_CAPABILITY_SET_MAX + 1U] = {
        AUTHORIZATION_CAPABILITY_STATUS_READ,
        AUTHORIZATION_CAPABILITY_CONFIG_WRITE,
        AUTHORIZATION_CAPABILITY_AUDIT_READ
    };

    CHECK(authorization_check(NULL, 1U, roles, 1U,
                              capabilities, 1U) == AUTHORIZATION_NULL,
          "authorization check rejects null matrix");
    CHECK(authorization_check(&matrix, 1U, NULL, 1U,
                              capabilities, 1U) == AUTHORIZATION_NULL,
          "authorization check rejects null role set with length");
    CHECK(authorization_check(&matrix, 1U, roles, 1U,
                              NULL, 1U) == AUTHORIZATION_NULL,
          "authorization check rejects null capability set with length");
    CHECK(authorization_check(&matrix, 1U, NULL,
                              AUTHORIZATION_ROLE_SET_MAX + 1U,
                              capabilities, 1U) == AUTHORIZATION_NULL,
          "authorization check rejects null over-limit role set as null");
    CHECK(authorization_check(&matrix, 1U, roles, 1U,
                              NULL,
                              AUTHORIZATION_CAPABILITY_SET_MAX + 1U) ==
              AUTHORIZATION_NULL,
          "authorization check rejects null over-limit capability set as null");

    bad_matrix.initialized = false;
    CHECK(authorization_check(&bad_matrix, 1U, roles, 1U,
                              capabilities, 1U) == AUTHORIZATION_BAD_STATE,
          "authorization check rejects uninitialized matrix");

    bad_matrix = matrix;
    bad_matrix.count = 0U;
    CHECK(authorization_check(&bad_matrix, 1U, roles, 1U,
                              capabilities, 1U) == AUTHORIZATION_BAD_STATE,
          "authorization check rejects zero matrix count");

    bad_matrix = matrix;
    bad_matrix.version = AUTHORIZATION_VERSION + 1U;
    CHECK(authorization_check(&bad_matrix, 1U, roles, 1U,
                              capabilities, 1U) == AUTHORIZATION_BAD_VERSION,
          "authorization check rejects unknown matrix version");

    CHECK(authorization_check(&matrix, 0U, roles, 1U,
                              capabilities, 1U) ==
              AUTHORIZATION_UNKNOWN_COMMAND,
          "authorization check rejects zero command id");
    CHECK(authorization_check(&matrix,
                              AUTHORIZATION_COMMAND_ID_MAX + 1U,
                              roles, 1U, capabilities, 1U) ==
              AUTHORIZATION_UNKNOWN_COMMAND,
          "authorization check rejects over-limit command id");
    CHECK(authorization_check(&matrix, 99U, roles, 1U,
                              capabilities, 1U) ==
              AUTHORIZATION_UNKNOWN_COMMAND,
          "authorization check rejects unregistered command id");

    CHECK(authorization_check(&matrix, 1U, roles,
                              AUTHORIZATION_ROLE_SET_MAX + 1U,
                              capabilities, 1U) ==
              AUTHORIZATION_INVALID_ROLE_SET,
          "authorization check rejects over-limit role set");

    roles[0] = 0U;
    CHECK(authorization_check(&matrix, 1U, roles, 1U,
                              capabilities, 1U) ==
              AUTHORIZATION_INVALID_ROLE_SET,
          "authorization check rejects zero role id");
    roles[0] = AUTHORIZATION_ROLE_ADMIN;
    roles[1] = AUTHORIZATION_ROLE_ADMIN;
    CHECK(authorization_check(&matrix, 1U, roles, 2U,
                              capabilities, 1U) ==
              AUTHORIZATION_INVALID_ROLE_SET,
          "authorization check rejects duplicate role");
    roles[0] = AUTHORIZATION_ROLE_ADMIN;

    CHECK(authorization_check(&matrix, 1U, roles, 1U,
                              capabilities,
                              AUTHORIZATION_CAPABILITY_SET_MAX + 1U) ==
              AUTHORIZATION_INVALID_CAPABILITY_SET,
          "authorization check rejects over-limit capability set");

    capabilities[0] = 0U;
    CHECK(authorization_check(&matrix, 1U, roles, 1U,
                              capabilities, 1U) ==
              AUTHORIZATION_INVALID_CAPABILITY_SET,
          "authorization check rejects zero capability id");
    capabilities[0] = AUTHORIZATION_CAPABILITY_STATUS_READ;
    capabilities[1] = AUTHORIZATION_CAPABILITY_STATUS_READ;
    CHECK(authorization_check(&matrix, 1U, roles, 1U,
                              capabilities, 2U) ==
              AUTHORIZATION_INVALID_CAPABILITY_SET,
          "authorization check rejects duplicate capability");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "authorization check failures perform no allocation");
}

static void check_denied_no_side_effect(
    const struct authorization_matrix *matrix,
    uint32_t command_id,
    const uint8_t *roles,
    size_t role_count,
    const uint8_t *capabilities,
    size_t capability_count,
    enum authorization_status expected,
    const char *message)
{
    unsigned char matrix_before[sizeof(*matrix)];
    unsigned char roles_before[sizeof(uint8_t) * AUTHORIZATION_ROLE_SET_MAX];
    unsigned char caps_before[
        sizeof(uint8_t) * AUTHORIZATION_CAPABILITY_SET_MAX];

    memcpy(matrix_before, matrix, sizeof(*matrix));
    memcpy(roles_before, roles, sizeof(roles_before));
    memcpy(caps_before, capabilities, sizeof(caps_before));
    CHECK(authorization_check(matrix, command_id, roles, role_count,
                              capabilities, capability_count) == expected &&
              memcmp(matrix, matrix_before, sizeof(*matrix)) == 0 &&
              memcmp(roles, roles_before, sizeof(roles_before)) == 0 &&
              memcmp(capabilities, caps_before, sizeof(caps_before)) == 0 &&
              alloc_attempts == 0U && free_attempts == 0U,
          message);
}

static void test_authorized_and_denied(void)
{
    struct authorization_matrix matrix = make_matrix();
    uint8_t admin_roles[AUTHORIZATION_ROLE_SET_MAX] = {
        AUTHORIZATION_ROLE_ADMIN
    };
    uint8_t operator_roles[AUTHORIZATION_ROLE_SET_MAX] = {
        AUTHORIZATION_ROLE_OPERATOR
    };
    uint8_t status_caps[AUTHORIZATION_CAPABILITY_SET_MAX] = {
        AUTHORIZATION_CAPABILITY_STATUS_READ
    };
    uint8_t config_caps[AUTHORIZATION_CAPABILITY_SET_MAX] = {
        AUTHORIZATION_CAPABILITY_CONFIG_WRITE
    };

    CHECK(authorization_check(&matrix, 1U, admin_roles, 1U,
                              status_caps, 1U) == AUTHORIZATION_OK,
          "role and capability requirement is satisfied");
    CHECK(authorization_check(&matrix, 2U, operator_roles, 1U,
                              NULL, 0U) == AUTHORIZATION_OK,
          "role-only requirement is satisfied");
    CHECK(authorization_check(&matrix, 3U, NULL, 0U,
                              config_caps, 1U) == AUTHORIZATION_OK,
          "capability-only requirement is satisfied");

    check_denied_no_side_effect(
        &matrix, 1U, admin_roles, 1U, config_caps, 1U,
        AUTHORIZATION_MISSING_CAPABILITY,
        "missing capability denies command with no side effect");
    check_denied_no_side_effect(
        &matrix, 1U, operator_roles, 1U, status_caps, 1U,
        AUTHORIZATION_MISSING_ROLE,
        "missing role denies command with no side effect");
    check_denied_no_side_effect(
        &matrix, 2U, admin_roles, 1U, status_caps, 1U,
        AUTHORIZATION_MISSING_ROLE,
        "role-only command denies missing role with no side effect");
    check_denied_no_side_effect(
        &matrix, 3U, admin_roles, 1U, status_caps, 1U,
        AUTHORIZATION_MISSING_CAPABILITY,
        "capability-only command denies missing capability with no side effect");
}

int main(void)
{
    test_status_strings();
    test_matrix_init();
    test_check_failures();
    test_authorized_and_denied();

    if (failures == 0) {
        printf("Authorization matrix host tests: ALL OK\n");
        return 0;
    }
    fprintf(stderr, "Authorization matrix host tests: FAILED (%d)\n",
            failures);
    return 1;
}

#else
#error "This host test must be built with HFOC_AUTHORIZATION_HOST_TEST"
#endif
