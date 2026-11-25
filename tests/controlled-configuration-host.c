#include "controlled_configuration.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifdef HFOC_CONTROLLED_CONFIGURATION_HOST_TEST

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

static bool fixture_initialize(struct config_snapshot *snapshot,
                               struct event_ring *ring)
{
    struct appliance_config initial;

    memset(snapshot, 0, sizeof(*snapshot));
    memset(ring, 0, sizeof(*ring));
    return config_schema_parse(initial_text, sizeof(initial_text) - 1U,
                               &initial) == CONFIG_OK &&
           config_snapshot_init(snapshot, &initial) ==
               CONFIG_SNAPSHOT_OK &&
           event_ring_init(ring) == EVENT_RING_OK;
}

static bool audit_record_matches(const struct audit_record *record,
                                 uint32_t result)
{
    return record != NULL &&
           record->type == AUDIT_RECORD_CAPABILITY &&
           record->operation ==
               CONTROLLED_CONFIGURATION_AUDIT_OPERATION_ACTIVATE &&
           record->result == result &&
           record->pid == 0U &&
           record->domain == 0U &&
           record->generation == 0U &&
           record->detail == CONTROLLED_CONFIGURATION_COMMAND_ID;
}

static void test_status_strings(void)
{
    enum controlled_configuration_status status;

    for (status = CONTROLLED_CONFIGURATION_OK;
         status <= CONTROLLED_CONFIGURATION_AUDIT_FAILED; ++status) {
        const char *string =
            controlled_configuration_status_string(status);

        CHECK(string != NULL &&
                  (status == CONTROLLED_CONFIGURATION_OK ?
                       strcmp(string,
                              "CONTROLLED_CONFIGURATION_OK") == 0 :
                   status == CONTROLLED_CONFIGURATION_NULL ?
                       strcmp(string,
                              "CONTROLLED_CONFIGURATION_NULL") == 0 :
                   status == CONTROLLED_CONFIGURATION_BAD_STATE ?
                       strcmp(string,
                              "CONTROLLED_CONFIGURATION_BAD_STATE") == 0 :
                   status == CONTROLLED_CONFIGURATION_TOO_LONG ?
                       strcmp(string,
                              "CONTROLLED_CONFIGURATION_TOO_LONG") == 0 :
                   status == CONTROLLED_CONFIGURATION_PARSE_FAILED ?
                       strcmp(string,
                              "CONTROLLED_CONFIGURATION_PARSE_FAILED") == 0 :
                   status == CONTROLLED_CONFIGURATION_COMMIT_FAILED ?
                       strcmp(string,
                              "CONTROLLED_CONFIGURATION_COMMIT_FAILED") == 0 :
                       strcmp(string,
                              "CONTROLLED_CONFIGURATION_AUDIT_FAILED") == 0),
              "controlled configuration status string is exact");
    }
    CHECK(controlled_configuration_status_string(
              (enum controlled_configuration_status)7U) == NULL,
          "controlled configuration undefined status is rejected");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "controlled configuration status handling performs no allocation");
}

static void test_command_integration(void)
{
    struct management_command_descriptor descriptor;
    struct management_command_registry registry;
    struct management_command_header header;
    struct authorization_entry authorization;
    struct authorization_matrix matrix;
    static const uint8_t admin_role[] = { AUTHORIZATION_ROLE_ADMIN };
    static const uint8_t config_write[] = {
        AUTHORIZATION_CAPABILITY_CONFIG_WRITE
    };
    static const char request_byte = 'x';

    CHECK(controlled_configuration_command_descriptor(NULL) ==
              CONTROLLED_CONFIGURATION_NULL,
          "controlled configuration descriptor init rejects null descriptor");
    CHECK(controlled_configuration_command_descriptor(&descriptor) ==
              CONTROLLED_CONFIGURATION_OK &&
              descriptor.version == MANAGEMENT_COMMAND_VERSION &&
              descriptor.command_id ==
                  CONTROLLED_CONFIGURATION_COMMAND_ID &&
              descriptor.request_min_length ==
                  CONTROLLED_CONFIGURATION_REQUEST_MIN &&
              descriptor.request_max_length ==
                  CONTROLLED_CONFIGURATION_REQUEST_MAX &&
              descriptor.response_min_length ==
                  CONTROLLED_CONFIGURATION_RESPONSE_MIN &&
              descriptor.response_max_length ==
                  CONTROLLED_CONFIGURATION_RESPONSE_MAX,
          "controlled configuration descriptor publishes exact bounds");
    CHECK(management_command_registry_init(&registry, &descriptor, 1U) ==
              MANAGEMENT_COMMAND_OK,
          "controlled configuration descriptor is registry-valid");

    memset(&header, 0, sizeof(header));
    header.version = MANAGEMENT_COMMAND_VERSION;
    header.command_id = CONTROLLED_CONFIGURATION_COMMAND_ID;
    CHECK(management_command_validate_frame(
              &registry, MANAGEMENT_COMMAND_DIRECTION_REQUEST,
              &header, &request_byte, 1U,
              MANAGEMENT_COMMAND_HEADER_LENGTH + 1U) ==
              MANAGEMENT_COMMAND_OK,
          "controlled configuration bounded request frame validates");
    CHECK(management_command_validate_frame(
              &registry, MANAGEMENT_COMMAND_DIRECTION_RESPONSE,
              &header, NULL, 0U, MANAGEMENT_COMMAND_HEADER_LENGTH) ==
              MANAGEMENT_COMMAND_OK,
          "controlled configuration empty response frame validates");
    CHECK(management_command_validate_frame(
              &registry, MANAGEMENT_COMMAND_DIRECTION_RESPONSE,
              &header, &request_byte, 1U,
              MANAGEMENT_COMMAND_HEADER_LENGTH + 1U) ==
              MANAGEMENT_COMMAND_BAD_LENGTH,
          "controlled configuration nonempty response is rejected");

    CHECK(controlled_configuration_authorization_entry(NULL) ==
              CONTROLLED_CONFIGURATION_NULL,
          "controlled configuration authorization init rejects null entry");
    CHECK(controlled_configuration_authorization_entry(&authorization) ==
              CONTROLLED_CONFIGURATION_OK &&
              authorization.version == AUTHORIZATION_VERSION &&
              authorization.command_id ==
                  CONTROLLED_CONFIGURATION_COMMAND_ID &&
              authorization.role_count == 1U &&
              authorization.roles[0] == AUTHORIZATION_ROLE_ADMIN &&
              authorization.capability_count == 1U &&
              authorization.capabilities[0] ==
                  AUTHORIZATION_CAPABILITY_CONFIG_WRITE,
          "controlled configuration authorization publishes exact requirement");
    CHECK(authorization_matrix_init(&matrix, &authorization, 1U) ==
              AUTHORIZATION_OK,
          "controlled configuration authorization entry is matrix-valid");
    CHECK(authorization_check(
              &matrix, CONTROLLED_CONFIGURATION_COMMAND_ID,
              admin_role, 1U, config_write, 1U) == AUTHORIZATION_OK,
          "controlled configuration write is granted to admin with config capability");
    CHECK(authorization_check(
              &matrix, CONTROLLED_CONFIGURATION_COMMAND_ID,
              admin_role, 1U, NULL, 0U) ==
              AUTHORIZATION_MISSING_CAPABILITY,
          "controlled configuration denies missing config capability");
    CHECK(authorization_check(
              &matrix, CONTROLLED_CONFIGURATION_COMMAND_ID,
              NULL, 0U, config_write, 1U) ==
              AUTHORIZATION_MISSING_ROLE,
          "controlled configuration denies missing admin role");
    CHECK(authorization_check(
              &matrix, 99U, admin_role, 1U, config_write, 1U) ==
              AUTHORIZATION_UNKNOWN_COMMAND,
          "controlled configuration denies unknown command");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "controlled configuration command integration performs no allocation");
}

static void test_apply_success(void)
{
    struct config_snapshot snapshot;
    struct event_ring ring;
    struct appliance_config expected;
    struct audit_record record;
    uint64_t sequence = UINT64_MAX;

    CHECK(fixture_initialize(&snapshot, &ring),
          "success fixture initializes snapshot and audit ring");
    CHECK(config_schema_parse(replacement_text,
                              sizeof(replacement_text) - 1U,
                              &expected) == CONFIG_OK,
          "success fixture parses expected candidate");
    CHECK(controlled_configuration_apply(
              &snapshot, replacement_text,
              sizeof(replacement_text) - 1U,
              commit_accept, NULL, &ring, &sequence) ==
              CONTROLLED_CONFIGURATION_OK &&
              sequence == 0U,
          "controlled configuration apply activates candidate");
    CHECK(config_snapshot_active(&snapshot) != NULL &&
              config_equal(config_snapshot_active(&snapshot), &expected) &&
              !config_snapshot_has_pending(&snapshot),
          "successful apply publishes exact active config and no pending");
    CHECK(event_ring_count(&ring) == 1U &&
              audit_record_pop(&ring, &record) == AUDIT_OK &&
              record.sequence == 0U &&
              audit_record_matches(
                  &record, CONTROLLED_CONFIGURATION_AUDIT_RESULT_OK),
          "successful apply publishes exact success audit record");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "successful apply performs no allocation");
}

static void test_apply_parse_failure(void)
{
    struct config_snapshot snapshot;
    struct event_ring ring;
    struct appliance_config active_before;
    struct audit_record record;
    uint64_t sequence = UINT64_MAX;

    CHECK(fixture_initialize(&snapshot, &ring),
          "parse-failure fixture initializes snapshot and audit ring");
    active_before = *config_snapshot_active(&snapshot);
    CHECK(controlled_configuration_apply(
              &snapshot, malformed_text,
              sizeof(malformed_text) - 1U,
              commit_accept, NULL, &ring, &sequence) ==
              CONTROLLED_CONFIGURATION_PARSE_FAILED,
          "controlled configuration rejects malformed request");
    CHECK(config_equal(config_snapshot_active(&snapshot), &active_before) &&
              !config_snapshot_has_pending(&snapshot),
          "parse failure preserves active config and leaves no pending");
    CHECK(event_ring_count(&ring) == 1U &&
              audit_record_pop(&ring, &record) == AUDIT_OK &&
              audit_record_matches(
                  &record, CONTROLLED_CONFIGURATION_AUDIT_RESULT_FAILED),
          "parse failure publishes failed audit record");
    CHECK(sequence == 0U,
          "parse failure publishes failed audit with exact sequence");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "parse failure performs no allocation");
}

static void test_apply_commit_rejected(void)
{
    struct config_snapshot snapshot;
    struct event_ring ring;
    struct appliance_config active_before;
    struct audit_record record;
    uint64_t sequence = UINT64_MAX;

    CHECK(fixture_initialize(&snapshot, &ring),
          "commit-rejection fixture initializes snapshot and audit ring");
    active_before = *config_snapshot_active(&snapshot);
    CHECK(controlled_configuration_apply(
              &snapshot, replacement_text,
              sizeof(replacement_text) - 1U,
              commit_reject, NULL, &ring, &sequence) ==
              CONTROLLED_CONFIGURATION_COMMIT_FAILED,
          "controlled configuration rejects validator failure");
    CHECK(config_equal(config_snapshot_active(&snapshot), &active_before) &&
              !config_snapshot_has_pending(&snapshot),
          "validator failure rolls back pending and preserves active config");
    CHECK(event_ring_count(&ring) == 1U &&
              audit_record_pop(&ring, &record) == AUDIT_OK &&
              audit_record_matches(
                  &record, CONTROLLED_CONFIGURATION_AUDIT_RESULT_FAILED),
          "validator failure publishes failed audit record");
    CHECK(sequence == 0U,
          "commit failure publishes failed audit with exact sequence");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "validator failure performs no allocation");
}

static void test_rejection_paths(void)
{
    struct config_snapshot snapshot;
    struct event_ring ring;
    struct appliance_config active_before;
    char too_long[CONTROLLED_CONFIGURATION_REQUEST_MAX + 1U];
    uint64_t sequence = UINT64_MAX;

    memset(&snapshot, 0, sizeof(snapshot));
    memset(&ring, 0, sizeof(ring));
    CHECK(controlled_configuration_apply(
              NULL, replacement_text, sizeof(replacement_text) - 1U,
              commit_accept, NULL, &ring, &sequence) ==
              CONTROLLED_CONFIGURATION_NULL,
          "apply rejects null snapshot");
    CHECK(controlled_configuration_apply(
              &snapshot, NULL, 1U, commit_accept, NULL, &ring,
              &sequence) == CONTROLLED_CONFIGURATION_NULL,
          "apply rejects null input");
    CHECK(controlled_configuration_apply(
              &snapshot, replacement_text,
              sizeof(replacement_text) - 1U,
              commit_accept, NULL, NULL, &sequence) ==
              CONTROLLED_CONFIGURATION_NULL,
          "apply rejects null audit ring");

    CHECK(fixture_initialize(&snapshot, &ring),
          "rejection fixture initializes snapshot and audit ring");
    active_before = *config_snapshot_active(&snapshot);
    CHECK(controlled_configuration_apply(
              &snapshot, replacement_text, 0U,
              commit_accept, NULL, &ring, &sequence) ==
              CONTROLLED_CONFIGURATION_TOO_LONG,
          "apply rejects zero-length request");
    memset(too_long, 'x', sizeof(too_long));
    CHECK(controlled_configuration_apply(
              &snapshot, too_long, sizeof(too_long),
              commit_accept, NULL, &ring, &sequence) ==
              CONTROLLED_CONFIGURATION_TOO_LONG,
          "apply rejects over-limit request");
    CHECK(config_equal(config_snapshot_active(&snapshot), &active_before) &&
              !config_snapshot_has_pending(&snapshot) &&
              event_ring_count(&ring) == 0U,
          "too-long rejection leaves config and audit unchanged");

    memset(&snapshot, 0, sizeof(snapshot));
    CHECK(controlled_configuration_apply(
              &snapshot, replacement_text,
              sizeof(replacement_text) - 1U,
              commit_accept, NULL, &ring, &sequence) ==
              CONTROLLED_CONFIGURATION_BAD_STATE,
          "apply rejects unpublished snapshot");

    CHECK(fixture_initialize(&snapshot, &ring),
          "unpublished-ring fixture initializes snapshot");
    memset(&ring, 0, sizeof(ring));
    CHECK(controlled_configuration_apply(
              &snapshot, replacement_text,
              sizeof(replacement_text) - 1U,
              commit_accept, NULL, &ring, &sequence) ==
              CONTROLLED_CONFIGURATION_BAD_STATE,
          "apply rejects unpublished audit ring");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "rejection paths perform no allocation");
}

static void test_full_ring_rejection(void)
{
    struct config_snapshot snapshot;
    struct event_ring ring;
    struct audit_record filler;
    struct appliance_config active_before;
    uint64_t sequence = UINT64_MAX;
    uint32_t index;

    CHECK(fixture_initialize(&snapshot, &ring),
          "full-ring fixture initializes snapshot and audit ring");
    active_before = *config_snapshot_active(&snapshot);
    for (index = 0U; index < KERNEL_EVENT_RING_CAPACITY; ++index) {
        CHECK(audit_record_init(
                  &filler, AUDIT_RECORD_PROCESS, 1U, 0U,
                  index % 256U, 0U, 0U, index) == AUDIT_OK &&
                  audit_record_append(&ring, &filler, NULL) == AUDIT_OK,
              "full-ring fixture fills audit ring");
    }
    CHECK(controlled_configuration_apply(
              &snapshot, replacement_text,
              sizeof(replacement_text) - 1U,
              commit_accept, NULL, &ring, &sequence) ==
              CONTROLLED_CONFIGURATION_AUDIT_FAILED,
          "full audit ring rejects controlled configuration");
    CHECK(config_equal(config_snapshot_active(&snapshot), &active_before) &&
              !config_snapshot_has_pending(&snapshot) &&
              event_ring_count(&ring) == KERNEL_EVENT_RING_CAPACITY &&
              sequence == UINT64_MAX,
          "full-ring rejection leaves config, pending, audit, and sequence unchanged");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "full-ring rejection performs no allocation");
}

int main(void) {
    test_status_strings();
    test_command_integration();
    test_apply_success();
    test_apply_parse_failure();
    test_apply_commit_rejected();
    test_rejection_paths();
    test_full_ring_rejection();

    if (failures != 0) {
        fprintf(stderr,
                "Controlled configuration host tests: %d FAILURES\n",
                failures);
        return 1;
    }
    puts("Controlled configuration host tests: ALL OK");
    return 0;
}

#endif
