#include "local_principal.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifdef HFOC_LOCAL_PRINCIPAL_HOST_TEST

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

static struct local_principal make_principal(
    uint32_t id,
    uint32_t version,
    enum local_principal_state state,
    const char *name)
{
    struct local_principal principal;
    size_t name_length;

    memset(&principal, 0, sizeof(principal));
    principal.id = id;
    principal.version = version;
    principal.state = state;
    if (name != NULL) {
        name_length = strlen(name);
        if (name_length > LOCAL_PRINCIPAL_NAME_MAX) {
            name_length = LOCAL_PRINCIPAL_NAME_MAX;
        }
        memcpy(principal.name, name, name_length);
    }
    return principal;
}

static void set_roles(struct local_principal *principal,
                      uint32_t role_count,
                      const uint8_t *roles)
{
    principal->role_count = role_count;
    if (roles != NULL && role_count > 0U) {
        memcpy(principal->roles, roles, role_count);
    }
}

static void check_init_failure(const struct local_principal *principal,
                               enum local_principal_status expected,
                               const char *message)
{
    struct local_principal published;
    unsigned char original[sizeof(published)];

    memset(&published, 0xA5, sizeof(published));
    memcpy(original, &published, sizeof(original));
    CHECK(local_principal_init(&published, principal) == expected &&
              memcmp(&published, original, sizeof(original)) == 0,
          message);
}

static void test_status_strings(void) {
    enum local_principal_status status;
    enum local_principal_set_status set_status;

    for (status = LOCAL_PRINCIPAL_OK;
         status <= LOCAL_PRINCIPAL_DUPLICATE_ROLE; ++status) {
        const char *string = local_principal_status_string(status);

        CHECK(string != NULL &&
                  (status == LOCAL_PRINCIPAL_OK ?
                       strcmp(string, "LOCAL_PRINCIPAL_OK") == 0 :
                   status == LOCAL_PRINCIPAL_NULL ?
                       strcmp(string, "LOCAL_PRINCIPAL_NULL") == 0 :
                   status == LOCAL_PRINCIPAL_INVALID_ID ?
                       strcmp(string, "LOCAL_PRINCIPAL_INVALID_ID") == 0 :
                   status == LOCAL_PRINCIPAL_INVALID_VERSION ?
                       strcmp(string,
                              "LOCAL_PRINCIPAL_INVALID_VERSION") == 0 :
                   status == LOCAL_PRINCIPAL_INVALID_NAME ?
                       strcmp(string, "LOCAL_PRINCIPAL_INVALID_NAME") == 0 :
                   status == LOCAL_PRINCIPAL_INVALID_STATE ?
                       strcmp(string, "LOCAL_PRINCIPAL_INVALID_STATE") == 0 :
                   status == LOCAL_PRINCIPAL_INVALID_ROLE_COUNT ?
                       strcmp(string,
                              "LOCAL_PRINCIPAL_INVALID_ROLE_COUNT") == 0 :
                   status == LOCAL_PRINCIPAL_INVALID_ROLE_ID ?
                       strcmp(string, "LOCAL_PRINCIPAL_INVALID_ROLE_ID") == 0 :
                       strcmp(string,
                              "LOCAL_PRINCIPAL_DUPLICATE_ROLE") == 0),
              "local principal status string is exact for every status");
    }
    CHECK(local_principal_status_string(
              (enum local_principal_status)9U) == NULL,
          "local principal undefined status is rejected");

    for (set_status = LOCAL_PRINCIPAL_SET_OK;
         set_status <= LOCAL_PRINCIPAL_SET_DUPLICATE_PRINCIPAL; ++set_status) {
        const char *string = local_principal_set_status_string(set_status);

        CHECK(string != NULL &&
                  (set_status == LOCAL_PRINCIPAL_SET_OK ?
                       strcmp(string, "LOCAL_PRINCIPAL_SET_OK") == 0 :
                   set_status == LOCAL_PRINCIPAL_SET_NULL ?
                       strcmp(string, "LOCAL_PRINCIPAL_SET_NULL") == 0 :
                   set_status == LOCAL_PRINCIPAL_SET_BAD_CAPACITY ?
                       strcmp(string,
                              "LOCAL_PRINCIPAL_SET_BAD_CAPACITY") == 0 :
                   set_status == LOCAL_PRINCIPAL_SET_INVALID_DESCRIPTOR ?
                       strcmp(string,
                              "LOCAL_PRINCIPAL_SET_INVALID_DESCRIPTOR") == 0 :
                       strcmp(string,
                              "LOCAL_PRINCIPAL_SET_DUPLICATE_PRINCIPAL") == 0),
              "local principal set status string is exact for every status");
    }
    CHECK(local_principal_set_status_string(
              (enum local_principal_set_status)5U) == NULL,
          "local principal undefined set status is rejected");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "local principal status handling performs no allocation");
}

static void test_input_and_descriptor_failures(void) {
    struct local_principal principal;
    struct local_principal set_principal;

    CHECK(local_principal_validate(NULL) == LOCAL_PRINCIPAL_NULL,
          "local principal validate rejects null principal");
    CHECK(local_principal_init(NULL, NULL) == LOCAL_PRINCIPAL_NULL,
          "local principal init rejects null published output");
    CHECK(local_principal_init(NULL, &principal) == LOCAL_PRINCIPAL_NULL,
          "local principal init rejects null principal");
    CHECK(local_principal_set_validate(NULL, 0U) ==
              LOCAL_PRINCIPAL_SET_OK,
          "local principal empty set validates");
    CHECK(local_principal_set_validate(NULL, 1U) ==
              LOCAL_PRINCIPAL_SET_NULL,
          "local principal non-empty null set is rejected");
    CHECK(local_principal_set_validate(NULL,
              LOCAL_PRINCIPAL_SET_MAX + 1U) ==
              LOCAL_PRINCIPAL_SET_BAD_CAPACITY,
          "local principal over-limit set is rejected");

    principal = make_principal(0U, LOCAL_PRINCIPAL_DESCRIPTOR_VERSION,
                               LOCAL_PRINCIPAL_STATE_ENABLED, "admin");
    check_init_failure(&principal, LOCAL_PRINCIPAL_INVALID_ID,
                       "zero principal id is rejected without publication");

    principal = make_principal(LOCAL_PRINCIPAL_ID_MAX + 1U,
                               LOCAL_PRINCIPAL_DESCRIPTOR_VERSION,
                               LOCAL_PRINCIPAL_STATE_ENABLED, "admin");
    check_init_failure(&principal, LOCAL_PRINCIPAL_INVALID_ID,
                       "over-limit principal id is rejected without publication");

    principal = make_principal(1U, LOCAL_PRINCIPAL_DESCRIPTOR_VERSION + 1U,
                               LOCAL_PRINCIPAL_STATE_ENABLED, "admin");
    check_init_failure(&principal, LOCAL_PRINCIPAL_INVALID_VERSION,
                       "unknown principal version is rejected");

    principal = make_principal(1U, LOCAL_PRINCIPAL_DESCRIPTOR_VERSION,
                               LOCAL_PRINCIPAL_STATE_ENABLED, "");
    check_init_failure(&principal, LOCAL_PRINCIPAL_INVALID_NAME,
                       "empty principal name is rejected");

    memset(&principal, 0, sizeof(principal));
    principal.id = 1U;
    principal.version = LOCAL_PRINCIPAL_DESCRIPTOR_VERSION;
    principal.state = LOCAL_PRINCIPAL_STATE_ENABLED;
    memset(principal.name, 'a', LOCAL_PRINCIPAL_NAME_MAX + 1U);
    check_init_failure(&principal, LOCAL_PRINCIPAL_INVALID_NAME,
                       "overlong principal name is rejected");

    principal = make_principal(1U, LOCAL_PRINCIPAL_DESCRIPTOR_VERSION,
                               LOCAL_PRINCIPAL_STATE_ENABLED, "bad\nname");
    check_init_failure(&principal, LOCAL_PRINCIPAL_INVALID_NAME,
                       "control character in principal name is rejected");

    principal = make_principal(1U, LOCAL_PRINCIPAL_DESCRIPTOR_VERSION,
                               LOCAL_PRINCIPAL_STATE_ENABLED, " admin");
    check_init_failure(&principal, LOCAL_PRINCIPAL_INVALID_NAME,
                       "leading space in principal name is rejected");

    principal = make_principal(1U, LOCAL_PRINCIPAL_DESCRIPTOR_VERSION,
                               LOCAL_PRINCIPAL_STATE_ENABLED, "admin ");
    check_init_failure(&principal, LOCAL_PRINCIPAL_INVALID_NAME,
                       "trailing space in principal name is rejected");

    memset(&principal, 0, sizeof(principal));
    principal.id = 1U;
    principal.version = LOCAL_PRINCIPAL_DESCRIPTOR_VERSION;
    principal.state = LOCAL_PRINCIPAL_STATE_ENABLED;
    memcpy(principal.name, "admin", 6U);
    principal.name[6U] = (char)0xA5;
    check_init_failure(&principal, LOCAL_PRINCIPAL_INVALID_NAME,
                       "non-zero tail after principal name is rejected");

    principal = make_principal(1U, LOCAL_PRINCIPAL_DESCRIPTOR_VERSION,
                               (enum local_principal_state)2U, "admin");
    check_init_failure(&principal, LOCAL_PRINCIPAL_INVALID_STATE,
                       "invalid principal state is rejected");

    principal = make_principal(1U, LOCAL_PRINCIPAL_DESCRIPTOR_VERSION,
                               LOCAL_PRINCIPAL_STATE_ENABLED, "admin");
    set_roles(&principal, LOCAL_PRINCIPAL_ROLE_MAX + 1U, NULL);
    check_init_failure(&principal, LOCAL_PRINCIPAL_INVALID_ROLE_COUNT,
                       "over-limit role count is rejected");

    principal = make_principal(1U, LOCAL_PRINCIPAL_DESCRIPTOR_VERSION,
                               LOCAL_PRINCIPAL_STATE_ENABLED, "admin");
    set_roles(&principal, 1U,
              (const uint8_t[]){LOCAL_PRINCIPAL_ROLE_ID_MIN - 1U});
    check_init_failure(&principal, LOCAL_PRINCIPAL_INVALID_ROLE_ID,
                       "zero role id is rejected");

    principal = make_principal(1U, LOCAL_PRINCIPAL_DESCRIPTOR_VERSION,
                               LOCAL_PRINCIPAL_STATE_ENABLED, "admin");
    set_roles(&principal, 2U, (const uint8_t[]){2U, 2U});
    check_init_failure(&principal, LOCAL_PRINCIPAL_DUPLICATE_ROLE,
                       "duplicate principal role is rejected");

    set_principal = make_principal(1U, LOCAL_PRINCIPAL_DESCRIPTOR_VERSION,
                                   LOCAL_PRINCIPAL_STATE_ENABLED, "admin");
    set_roles(&set_principal, 1U, (const uint8_t[]){0U});
    CHECK(local_principal_set_validate(&set_principal, 1U) ==
              LOCAL_PRINCIPAL_SET_INVALID_DESCRIPTOR,
          "set validation rejects malformed principal");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "local principal input handling performs no allocation");
}

static void test_valid_descriptors_and_immutability(void) {
    struct local_principal principal;
    struct local_principal published;
    struct local_principal source;
    struct local_principal expected;
    struct local_principal principals[2];

    principal = make_principal(1U, LOCAL_PRINCIPAL_DESCRIPTOR_VERSION,
                               LOCAL_PRINCIPAL_STATE_DISABLED, "operator");
    CHECK(local_principal_validate(&principal) == LOCAL_PRINCIPAL_OK &&
              local_principal_init(&published, &principal) ==
                  LOCAL_PRINCIPAL_OK &&
              published.id == 1U &&
              published.version == LOCAL_PRINCIPAL_DESCRIPTOR_VERSION &&
              published.state == LOCAL_PRINCIPAL_STATE_DISABLED &&
              strcmp(published.name, "operator") == 0 &&
              published.role_count == 0U,
          "valid disabled principal publishes exactly");

    principal = make_principal(2U, LOCAL_PRINCIPAL_DESCRIPTOR_VERSION,
                               LOCAL_PRINCIPAL_STATE_ENABLED, "auditor");
    set_roles(&principal, 3U, (const uint8_t[]){2U, 7U, 9U});
    CHECK(local_principal_init(&published, &principal) ==
              LOCAL_PRINCIPAL_OK &&
              published.id == 2U &&
              published.state == LOCAL_PRINCIPAL_STATE_ENABLED &&
              published.role_count == 3U &&
              memcmp(published.roles,
                     (const uint8_t[]){2U, 7U, 9U}, 3U) == 0,
          "valid enabled principal publishes exact bounded roles");

    source = make_principal(3U, LOCAL_PRINCIPAL_DESCRIPTOR_VERSION,
                            LOCAL_PRINCIPAL_STATE_ENABLED, "admin");
    set_roles(&source, 1U, (const uint8_t[]){1U});
    expected = source;
    CHECK(local_principal_init(&published, &source) == LOCAL_PRINCIPAL_OK,
          "valid principal initializes for immutable copy");
    source.id = 99U;
    source.state = LOCAL_PRINCIPAL_STATE_DISABLED;
    source.name[0] = 'x';
    source.roles[0] = 88U;
    CHECK(memcmp(&published, &expected, sizeof(published)) == 0,
          "published principal is an immutable exact copy");

    principals[0] = make_principal(1U, LOCAL_PRINCIPAL_DESCRIPTOR_VERSION,
                                   LOCAL_PRINCIPAL_STATE_ENABLED, "admin");
    set_roles(&principals[0], 1U, (const uint8_t[]){1U});
    principals[1] = make_principal(2U, LOCAL_PRINCIPAL_DESCRIPTOR_VERSION,
                                   LOCAL_PRINCIPAL_STATE_DISABLED, "auditor");
    set_roles(&principals[1], 2U, (const uint8_t[]){2U, 3U});
    CHECK(local_principal_set_validate(principals, 2U) ==
              LOCAL_PRINCIPAL_SET_OK,
          "valid bounded principal set validates");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "valid local principal publication performs no allocation");
}

static void test_duplicate_principal_rejection(void) {
    struct local_principal principals[2];

    principals[0] = make_principal(1U, LOCAL_PRINCIPAL_DESCRIPTOR_VERSION,
                                   LOCAL_PRINCIPAL_STATE_ENABLED, "admin");
    principals[1] = make_principal(1U, LOCAL_PRINCIPAL_DESCRIPTOR_VERSION,
                                   LOCAL_PRINCIPAL_STATE_DISABLED, "operator");
    CHECK(local_principal_set_validate(principals, 2U) ==
              LOCAL_PRINCIPAL_SET_DUPLICATE_PRINCIPAL,
          "duplicate principal id is rejected");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "duplicate principal rejection performs no allocation");
}

int main(void) {
    test_status_strings();
    test_input_and_descriptor_failures();
    test_valid_descriptors_and_immutability();
    test_duplicate_principal_rejection();

    if (failures != 0) {
        fprintf(stderr, "Local principal host tests: %d FAILURES\n",
                failures);
        return 1;
    }
    printf("Local principal host tests: ALL OK\n");
    return 0;
}

#else
#error "This host test must be built with HFOC_LOCAL_PRINCIPAL_HOST_TEST"
#endif
