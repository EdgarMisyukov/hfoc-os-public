#include "config_schema.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifdef HFOC_CONFIG_SNAPSHOT_HOST_TEST

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

static const char initial_text[] =
    "schema_version=1\n"
    "recovery_mode=1\n"
    "max_processes=32\n"
    "max_user_memory_pages=512\n"
    "audit_ring_capacity=64\n"
    "boot_timeout_ms=5000\n"
    "restart_limit=3\n"
    "service_health_timeout_ms=10000\n";

static const char replacement_text[] =
    "schema_version=1\n"
    "recovery_mode=0\n"
    "max_processes=48\n"
    "max_user_memory_pages=1024\n"
    "audit_ring_capacity=32\n"
    "boot_timeout_ms=7000\n"
    "restart_limit=5\n"
    "service_health_timeout_ms=12000\n";

static const char malformed_text[] =
    "schema_version=1\n"
    "recovery_mode=1\n"
    "max_processes=48\n";

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

static bool config_equal(const struct appliance_config *left,
                         const struct appliance_config *right)
{
    return left != NULL && right != NULL &&
           memcmp(left, right, sizeof(*left)) == 0;
}

static bool commit_accept(const struct appliance_config *candidate,
                          void *context)
{
    (void)candidate;
    (void)context;
    return true;
}

static bool commit_reject(const struct appliance_config *candidate,
                          void *context)
{
    (void)candidate;
    (void)context;
    return false;
}

static void test_validation_and_null_arguments(void) {
    struct config_snapshot snapshot;
    struct appliance_config initial;

    memset(&snapshot, 0, sizeof(snapshot));
    memset(&initial, 0, sizeof(initial));

    CHECK(config_snapshot_init(NULL, &initial) == CONFIG_SNAPSHOT_NULL,
          "snapshot init rejects null snapshot");
    CHECK(config_snapshot_init(&snapshot, NULL) == CONFIG_SNAPSHOT_NULL,
          "snapshot init rejects null initial config");
    CHECK(config_snapshot_prepare(NULL, initial_text,
                                  sizeof(initial_text) - 1U) ==
              CONFIG_SNAPSHOT_NULL,
          "snapshot prepare rejects null snapshot");
    CHECK(config_snapshot_commit(NULL, commit_accept, NULL) ==
              CONFIG_SNAPSHOT_NULL,
          "snapshot commit rejects null snapshot");
    CHECK(config_snapshot_rollback(NULL) == CONFIG_SNAPSHOT_NULL,
          "snapshot rollback rejects null snapshot");
    CHECK(config_snapshot_destroy(NULL) == CONFIG_SNAPSHOT_NULL,
          "snapshot destroy rejects null snapshot");
    CHECK(config_snapshot_active(NULL) == NULL,
          "snapshot active rejects null snapshot");
    CHECK(!config_snapshot_has_pending(NULL),
          "snapshot pending query rejects null snapshot");
    CHECK(config_snapshot_status_string(CONFIG_SNAPSHOT_OK) != NULL &&
              config_snapshot_status_string(
                  CONFIG_SNAPSHOT_INVALID_CONFIG) != NULL,
          "snapshot status strings are present");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "snapshot validation performs no allocation");
}

static void test_initial_publication_and_invalid_config(void) {
    struct config_snapshot snapshot;
    struct appliance_config initial;
    struct appliance_config invalid;

    memset(&snapshot, 0, sizeof(snapshot));
    CHECK(config_schema_parse(initial_text, sizeof(initial_text) - 1U,
                              &initial) == CONFIG_OK,
          "initial fixture parses");
    CHECK(config_snapshot_init(&snapshot, &initial) == CONFIG_SNAPSHOT_OK &&
              config_snapshot_active(&snapshot) != NULL &&
              config_equal(config_snapshot_active(&snapshot), &initial),
          "valid initial config publishes active snapshot");
    CHECK(!config_snapshot_has_pending(&snapshot),
          "initial publication leaves no pending candidate");
    CHECK(config_snapshot_init(&snapshot, &initial) ==
              CONFIG_SNAPSHOT_BAD_STATE,
          "snapshot rejects reinitialization");

    memset(&snapshot, 0, sizeof(snapshot));
    invalid = initial;
    invalid.max_processes = 0U;
    CHECK(config_snapshot_init(&snapshot, &invalid) ==
              CONFIG_SNAPSHOT_INVALID_CONFIG &&
              config_snapshot_active(&snapshot) == NULL &&
              !config_snapshot_has_pending(&snapshot),
          "invalid initial config is rejected without publication");

    memset(&snapshot, 0, sizeof(snapshot));
    CHECK(config_snapshot_init(&snapshot, &initial) == CONFIG_SNAPSHOT_OK &&
              config_snapshot_destroy(&snapshot) == CONFIG_SNAPSHOT_OK &&
              config_snapshot_active(&snapshot) == NULL &&
              !config_snapshot_has_pending(&snapshot),
          "destroy clears published snapshot");
    CHECK(config_snapshot_destroy(&snapshot) == CONFIG_SNAPSHOT_BAD_STATE,
          "destroy rejects unpublished snapshot");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "initial publication performs no allocation");
}

static void test_prepare_and_active_isolation(void) {
    struct config_snapshot snapshot;
    struct appliance_config initial;
    struct appliance_config pending;

    memset(&snapshot, 0, sizeof(snapshot));
    CHECK(config_schema_parse(initial_text, sizeof(initial_text) - 1U,
                              &initial) == CONFIG_OK &&
              config_snapshot_init(&snapshot, &initial) ==
                  CONFIG_SNAPSHOT_OK,
          "snapshot fixture initializes active config");

    CHECK(config_snapshot_prepare(&snapshot, malformed_text,
                                  sizeof(malformed_text) - 1U) ==
              CONFIG_SNAPSHOT_PARSE_FAILED &&
              !config_snapshot_has_pending(&snapshot) &&
              config_equal(config_snapshot_active(&snapshot), &initial),
          "malformed prepare fails without pending or active mutation");

    CHECK(config_snapshot_prepare(&snapshot, replacement_text,
                                  sizeof(replacement_text) - 1U) ==
              CONFIG_SNAPSHOT_OK &&
              config_snapshot_has_pending(&snapshot) &&
              config_equal(config_snapshot_active(&snapshot), &initial),
          "valid prepare creates pending without active mutation");
    CHECK(config_schema_parse(replacement_text,
                              sizeof(replacement_text) - 1U,
                              &pending) == CONFIG_OK,
          "replacement fixture parses");
    CHECK(config_snapshot_commit(&snapshot, NULL, NULL) ==
              CONFIG_SNAPSHOT_OK &&
              !config_snapshot_has_pending(&snapshot) &&
              config_equal(config_snapshot_active(&snapshot), &pending),
          "null validator commit publishes exact pending fields");

    CHECK(config_snapshot_commit(&snapshot, commit_accept, NULL) ==
              CONFIG_SNAPSHOT_NO_PENDING,
          "commit without pending fails");
    CHECK(config_equal(config_snapshot_active(&snapshot), &pending),
          "commit without pending leaves active unchanged");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "prepare and successful commit perform no allocation");
}

static void test_failed_commit_and_rollback(void) {
    struct config_snapshot snapshot;
    struct appliance_config initial;
    struct appliance_config pending;

    memset(&snapshot, 0, sizeof(snapshot));
    CHECK(config_schema_parse(initial_text, sizeof(initial_text) - 1U,
                              &initial) == CONFIG_OK &&
              config_schema_parse(replacement_text,
                                  sizeof(replacement_text) - 1U,
                                  &pending) == CONFIG_OK &&
              config_snapshot_init(&snapshot, &initial) ==
                  CONFIG_SNAPSHOT_OK,
          "failed-commit fixture initializes");

    CHECK(config_snapshot_prepare(&snapshot, replacement_text,
                                  sizeof(replacement_text) - 1U) ==
              CONFIG_SNAPSHOT_OK &&
              config_snapshot_has_pending(&snapshot),
          "failed-commit fixture prepares candidate");
    CHECK(config_snapshot_commit(&snapshot, commit_reject, NULL) ==
              CONFIG_SNAPSHOT_COMMIT_FAILED &&
              !config_snapshot_has_pending(&snapshot) &&
              config_equal(config_snapshot_active(&snapshot), &initial),
          "validator-rejected commit clears pending and keeps active");
    CHECK(config_snapshot_rollback(&snapshot) ==
              CONFIG_SNAPSHOT_NO_PENDING &&
              config_equal(config_snapshot_active(&snapshot), &initial),
          "rollback after failed commit is a no-op on active");

    CHECK(config_snapshot_prepare(&snapshot, replacement_text,
                                  sizeof(replacement_text) - 1U) ==
              CONFIG_SNAPSHOT_OK &&
              config_snapshot_rollback(&snapshot) == CONFIG_SNAPSHOT_OK &&
              !config_snapshot_has_pending(&snapshot) &&
              config_equal(config_snapshot_active(&snapshot), &initial),
          "rollback discards pending and preserves active");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "failed commit and rollback perform no allocation");
}

static void test_second_prepare_replaces_pending(void) {
    struct config_snapshot snapshot;
    struct appliance_config initial;
    struct appliance_config pending;

    memset(&snapshot, 0, sizeof(snapshot));
    CHECK(config_schema_parse(initial_text, sizeof(initial_text) - 1U,
                              &initial) == CONFIG_OK &&
              config_schema_parse(replacement_text,
                                  sizeof(replacement_text) - 1U,
                                  &pending) == CONFIG_OK &&
              config_snapshot_init(&snapshot, &initial) ==
                  CONFIG_SNAPSHOT_OK,
          "replace-pending fixture initializes");

    CHECK(config_snapshot_prepare(&snapshot, initial_text,
                                  sizeof(initial_text) - 1U) ==
              CONFIG_SNAPSHOT_OK &&
              config_snapshot_has_pending(&snapshot),
          "first prepare creates pending");
    CHECK(config_snapshot_prepare(&snapshot, replacement_text,
                                  sizeof(replacement_text) - 1U) ==
              CONFIG_SNAPSHOT_OK &&
              config_snapshot_has_pending(&snapshot) &&
              config_snapshot_commit(&snapshot, commit_accept, NULL) ==
                  CONFIG_SNAPSHOT_OK &&
              config_equal(config_snapshot_active(&snapshot), &pending),
          "second prepare replaces pending before commit");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "replace-pending transaction performs no allocation");
}

int main(void) {
    test_validation_and_null_arguments();
    test_initial_publication_and_invalid_config();
    test_prepare_and_active_isolation();
    test_failed_commit_and_rollback();
    test_second_prepare_replaces_pending();

    if (failures != 0) {
        fprintf(stderr, "Configuration snapshot host tests: %d FAILURES\n",
                failures);
        return 1;
    }
    puts("Configuration snapshot host tests: ALL OK");
    return 0;
}

#endif
