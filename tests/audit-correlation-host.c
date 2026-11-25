#include "audit_correlation.h"
#include "authorization.h"
#include "config_schema.h"
#include "controlled_configuration.h"
#include "event_ring.h"
#include "management_command.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifdef HFOC_AUDIT_CORRELATION_HOST_TEST

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

static struct audit_correlation make_correlation(uint32_t result)
{
    struct audit_correlation correlation;

    memset(&correlation, 0, sizeof(correlation));
    correlation.version = AUDIT_CORRELATION_VERSION;
    correlation.session_id = 7U;
    correlation.principal_id = 11U;
    correlation.command_id = CONTROLLED_CONFIGURATION_COMMAND_ID;
    correlation.result = result;
    return correlation;
}

static bool management_record_matches(
    const struct audit_record *record,
    uint64_t sequence,
    uint32_t operation,
    uint32_t result,
    uint32_t session_id,
    uint32_t principal_id,
    uint32_t command_id,
    uint64_t detail)
{
    return record != NULL &&
           record->sequence == sequence &&
           record->type == AUDIT_RECORD_MANAGEMENT &&
           record->operation == operation &&
           record->result == result &&
           record->pid == 0U &&
           record->domain == 0U &&
           record->generation == 0U &&
           record->session_id == session_id &&
           record->principal_id == principal_id &&
           record->command_id == command_id &&
           record->detail == detail;
}

static void test_validation(void)
{
    struct audit_correlation valid = make_correlation(
        AUDIT_CORRELATION_RESULT_OK);
    struct audit_correlation invalid;

    memset(&invalid, 0, sizeof(invalid));
    CHECK(audit_correlation_validate(NULL) == AUDIT_CORRELATION_NULL,
          "correlation validation rejects null descriptor");
    CHECK(audit_correlation_validate(&invalid) ==
              AUDIT_CORRELATION_BAD_VERSION,
          "correlation validation rejects bad version");

    invalid = valid;
    invalid.session_id = 0U;
    CHECK(audit_correlation_validate(&invalid) ==
              AUDIT_CORRELATION_INVALID_ID,
          "correlation validation rejects zero session id");
    invalid = valid;
    invalid.principal_id = 256U;
    CHECK(audit_correlation_validate(&invalid) ==
              AUDIT_CORRELATION_INVALID_ID,
          "correlation validation rejects out-of-range principal id");
    invalid = valid;
    invalid.command_id = 0U;
    CHECK(audit_correlation_validate(&invalid) ==
              AUDIT_CORRELATION_INVALID_ID,
          "correlation validation rejects zero command id");
    invalid = valid;
    invalid.result = 2U;
    CHECK(audit_correlation_validate(&invalid) ==
              AUDIT_CORRELATION_INVALID_RESULT,
          "correlation validation rejects unknown result");
    CHECK(audit_correlation_validate(&valid) == AUDIT_CORRELATION_OK,
          "correlation validation accepts exact bounded descriptor");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "correlation validation performs no allocation");
}

static void test_status_strings(void)
{
    enum audit_correlation_status status;

    for (status = AUDIT_CORRELATION_OK;
         status <= AUDIT_CORRELATION_AUDIT_FAILED; ++status) {
        const char *string = audit_correlation_status_string(status);

        CHECK(string != NULL &&
                  (status == AUDIT_CORRELATION_OK ?
                       strcmp(string, "AUDIT_CORRELATION_OK") == 0 :
                   status == AUDIT_CORRELATION_NULL ?
                       strcmp(string, "AUDIT_CORRELATION_NULL") == 0 :
                   status == AUDIT_CORRELATION_BAD_VERSION ?
                       strcmp(string, "AUDIT_CORRELATION_BAD_VERSION") == 0 :
                   status == AUDIT_CORRELATION_INVALID_ID ?
                       strcmp(string, "AUDIT_CORRELATION_INVALID_ID") == 0 :
                   status == AUDIT_CORRELATION_INVALID_OPERATION ?
                       strcmp(string,
                              "AUDIT_CORRELATION_INVALID_OPERATION") == 0 :
                   status == AUDIT_CORRELATION_INVALID_RESULT ?
                       strcmp(string, "AUDIT_CORRELATION_INVALID_RESULT") == 0 :
                   status == AUDIT_CORRELATION_BAD_STATE ?
                       strcmp(string, "AUDIT_CORRELATION_BAD_STATE") == 0 :
                       strcmp(string,
                              "AUDIT_CORRELATION_AUDIT_FAILED") == 0),
              "audit correlation status string is exact");
    }
    CHECK(audit_correlation_status_string(
              (enum audit_correlation_status)8U) == NULL,
          "audit correlation undefined status is rejected");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "audit correlation status handling performs no allocation");
}

static void test_exact_success_and_denied_records(void)
{
    struct event_ring ring;
    struct audit_correlation success = make_correlation(
        AUDIT_CORRELATION_RESULT_OK);
    struct audit_correlation denied = make_correlation(
        AUDIT_CORRELATION_RESULT_DENIED);
    struct audit_record record;
    struct kernel_event event;
    uint64_t sequence = UINT64_MAX;

    memset(&ring, 0, sizeof(ring));
    CHECK(event_ring_init(&ring) == EVENT_RING_OK,
          "exact record fixture initializes audit ring");
    CHECK(audit_correlation_append(
              &ring, CONTROLLED_CONFIGURATION_AUDIT_OPERATION_ACTIVATE,
              &success, UINT64_C(0x4242), &sequence) ==
              AUDIT_CORRELATION_OK &&
              sequence == 0U,
          "successful correlation appends and returns sequence zero");
    CHECK(event_ring_pop(&ring, &event) == EVENT_RING_OK &&
              event.data[2] ==
                  ((uint64_t)success.session_id |
                   ((uint64_t)success.principal_id << 8U) |
                   ((uint64_t)success.command_id << 16U)) &&
              audit_record_from_event(&event, &record) == AUDIT_OK &&
              management_record_matches(
                  &record, 0U,
                  CONTROLLED_CONFIGURATION_AUDIT_OPERATION_ACTIVATE,
                  AUDIT_CORRELATION_RESULT_OK,
                  success.session_id, success.principal_id,
                  success.command_id, UINT64_C(0x4242)),
          "successful correlation record round-trips exact fields");

    sequence = UINT64_MAX;
    CHECK(audit_correlation_append(
              &ring, CONTROLLED_CONFIGURATION_AUDIT_OPERATION_ACTIVATE,
              &denied, UINT64_C(0x4242), &sequence) ==
              AUDIT_CORRELATION_OK &&
              sequence == 1U,
          "denied correlation appends with next sequence");
    CHECK(audit_record_pop(&ring, &record) == AUDIT_OK &&
              management_record_matches(
                  &record, 1U,
                  CONTROLLED_CONFIGURATION_AUDIT_OPERATION_ACTIVATE,
                  AUDIT_CORRELATION_RESULT_DENIED,
                  denied.session_id, denied.principal_id,
                  denied.command_id, UINT64_C(0x4242)),
          "denied and successful records differ only by result");
    CHECK(event_ring_count(&ring) == 0U,
          "exact records drain without leakage");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "exact record publication performs no allocation");
}

static void test_malformed_event_rejection(void)
{
    struct event_ring ring;
    struct audit_correlation correlation = make_correlation(
        AUDIT_CORRELATION_RESULT_OK);
    struct audit_record record;
    struct kernel_event event;
    uint64_t sequence = UINT64_MAX;

    memset(&ring, 0, sizeof(ring));
    CHECK(event_ring_init(&ring) == EVENT_RING_OK,
          "malformed-event fixture initializes audit ring");
    CHECK(audit_correlation_append(
              &ring, 1U, &correlation, 0U, &sequence) ==
              AUDIT_CORRELATION_OK &&
              event_ring_pop(&ring, &event) == EVENT_RING_OK,
          "malformed-event fixture appends management record");

    event.data[2] |= UINT64_C(1) << 24U;
    memset(&record, 0, sizeof(record));
    CHECK(audit_record_from_event(&event, &record) == AUDIT_INVALID_EVENT &&
              record.type == 0U,
          "management decode rejects reserved correlation bits");

    memset(&event, 0, sizeof(event));
    event.type = AUDIT_RECORD_CAPABILITY;
    event.flags = AUDIT_EVENT_FLAG_STRUCTURED;
    event.data[0] = AUDIT_RECORD_CAPABILITY;
    event.data[2] = 1U;
    CHECK(audit_record_from_event(&event, &record) == AUDIT_INVALID_EVENT,
          "non-management decode rejects nonzero correlation data");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "malformed-event rejection performs no allocation");
}

static void test_ring_rejection_paths(void)
{
    struct event_ring ring;
    struct audit_correlation correlation = make_correlation(
        AUDIT_CORRELATION_RESULT_OK);
    struct audit_record filler;
    uint64_t sequence = UINT64_MAX;
    uint32_t index;

    memset(&ring, 0, sizeof(ring));
    CHECK(audit_correlation_append(
              NULL, 1U, &correlation, 0U, &sequence) ==
              AUDIT_CORRELATION_NULL,
          "correlation append rejects null ring");
    CHECK(audit_correlation_append(
              &ring, 1U, NULL, 0U, &sequence) ==
              AUDIT_CORRELATION_NULL,
          "correlation append rejects null descriptor");
    CHECK(audit_correlation_append(
              &ring, 0U, &correlation, 0U, &sequence) ==
              AUDIT_CORRELATION_INVALID_OPERATION,
          "correlation append rejects zero operation");
    CHECK(audit_correlation_append(
              &ring, 256U, &correlation, 0U, &sequence) ==
              AUDIT_CORRELATION_INVALID_OPERATION,
          "correlation append rejects out-of-range operation");
    CHECK(audit_correlation_append(
              &ring, 1U, &correlation, 0U, &sequence) ==
              AUDIT_CORRELATION_BAD_STATE,
          "correlation append rejects unpublished ring");
    CHECK(sequence == UINT64_MAX && event_ring_count(&ring) == 0U,
          "ring rejection leaves sequence and ring unchanged");

    CHECK(event_ring_init(&ring) == EVENT_RING_OK,
          "full-ring fixture initializes audit ring");
    for (index = 0U; index < KERNEL_EVENT_RING_CAPACITY; ++index) {
        CHECK(audit_record_init(
                  &filler, AUDIT_RECORD_PROCESS, 1U, 0U,
                  index % 256U, 0U, 0U, index) == AUDIT_OK &&
                  audit_record_append(&ring, &filler, NULL) == AUDIT_OK,
              "full-ring fixture fills audit ring");
    }
    sequence = UINT64_MAX;
    CHECK(audit_correlation_append(
              &ring, 1U, &correlation, 0U, &sequence) ==
              AUDIT_CORRELATION_AUDIT_FAILED &&
              sequence == UINT64_MAX &&
              event_ring_count(&ring) == KERNEL_EVENT_RING_CAPACITY,
          "full ring rejects correlation append without state change");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "ring rejection paths perform no allocation");
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

static void test_controlled_configuration_correlated(void)
{
    struct config_snapshot snapshot;
    struct event_ring ring;
    struct appliance_config active_before;
    struct audit_correlation correlation = make_correlation(
        AUDIT_CORRELATION_RESULT_OK);
    struct audit_record record;
    uint64_t sequence = UINT64_MAX;

    CHECK(fixture_initialize(&snapshot, &ring),
          "correlated controlled configuration fixture initializes");
    active_before = *config_snapshot_active(&snapshot);
    CHECK(controlled_configuration_apply_correlated(
              &snapshot, replacement_text,
              sizeof(replacement_text) - 1U,
              commit_accept, NULL, &correlation, &ring, &sequence) ==
              CONTROLLED_CONFIGURATION_OK &&
              sequence == 0U &&
              !config_snapshot_has_pending(&snapshot),
          "correlated controlled configuration activates candidate");
    CHECK(audit_record_pop(&ring, &record) == AUDIT_OK &&
              management_record_matches(
                  &record, 0U,
                  CONTROLLED_CONFIGURATION_AUDIT_OPERATION_ACTIVATE,
                  CONTROLLED_CONFIGURATION_AUDIT_RESULT_OK,
                  correlation.session_id, correlation.principal_id,
                  correlation.command_id,
                  CONTROLLED_CONFIGURATION_COMMAND_ID),
          "correlated success publishes exact management audit record");

    CHECK(fixture_initialize(&snapshot, &ring),
          "correlated parse-failure fixture initializes");
    active_before = *config_snapshot_active(&snapshot);
    sequence = UINT64_MAX;
    CHECK(controlled_configuration_apply_correlated(
              &snapshot, malformed_text,
              sizeof(malformed_text) - 1U,
              commit_accept, NULL, &correlation, &ring, &sequence) ==
              CONTROLLED_CONFIGURATION_PARSE_FAILED &&
              config_snapshot_active(&snapshot) != NULL &&
              memcmp(config_snapshot_active(&snapshot), &active_before,
                     sizeof(active_before)) == 0 &&
              !config_snapshot_has_pending(&snapshot) &&
              sequence == 0U,
          "correlated parse failure preserves active config and publishes audit");
    CHECK(audit_record_pop(&ring, &record) == AUDIT_OK &&
              management_record_matches(
                  &record, 0U,
                  CONTROLLED_CONFIGURATION_AUDIT_OPERATION_ACTIVATE,
                  CONTROLLED_CONFIGURATION_AUDIT_RESULT_FAILED,
                  correlation.session_id, correlation.principal_id,
                  correlation.command_id,
                  CONTROLLED_CONFIGURATION_COMMAND_ID),
          "correlated parse failure publishes denied audit record");

    CHECK(fixture_initialize(&snapshot, &ring),
          "correlated commit-rejection fixture initializes");
    active_before = *config_snapshot_active(&snapshot);
    sequence = UINT64_MAX;
    CHECK(controlled_configuration_apply_correlated(
              &snapshot, replacement_text,
              sizeof(replacement_text) - 1U,
              commit_reject, NULL, &correlation, &ring, &sequence) ==
              CONTROLLED_CONFIGURATION_COMMIT_FAILED &&
              config_snapshot_active(&snapshot) != NULL &&
              memcmp(config_snapshot_active(&snapshot), &active_before,
                     sizeof(active_before)) == 0 &&
              !config_snapshot_has_pending(&snapshot) &&
              sequence == 0U,
          "correlated validator failure rolls back and publishes audit");
    CHECK(audit_record_pop(&ring, &record) == AUDIT_OK &&
              management_record_matches(
                  &record, 0U,
                  CONTROLLED_CONFIGURATION_AUDIT_OPERATION_ACTIVATE,
                  CONTROLLED_CONFIGURATION_AUDIT_RESULT_FAILED,
                  correlation.session_id, correlation.principal_id,
                  correlation.command_id,
                  CONTROLLED_CONFIGURATION_COMMAND_ID),
          "correlated validator failure publishes denied audit record");

    CHECK(fixture_initialize(&snapshot, &ring),
          "invalid-correlation fixture initializes");
    active_before = *config_snapshot_active(&snapshot);
    correlation.version = 2U;
    sequence = UINT64_MAX;
    CHECK(controlled_configuration_apply_correlated(
              &snapshot, replacement_text,
              sizeof(replacement_text) - 1U,
              commit_accept, NULL, &correlation, &ring, &sequence) ==
              CONTROLLED_CONFIGURATION_BAD_STATE &&
              config_snapshot_active(&snapshot) != NULL &&
              memcmp(config_snapshot_active(&snapshot), &active_before,
                     sizeof(active_before)) == 0 &&
              !config_snapshot_has_pending(&snapshot) &&
              event_ring_count(&ring) == 0U &&
              sequence == UINT64_MAX,
          "invalid correlation is rejected before controlled configuration mutation");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "correlated controlled configuration performs no allocation");
}

static void test_legacy_apply_preserved(void)
{
    struct config_snapshot snapshot;
    struct event_ring ring;
    struct audit_record record;
    uint64_t sequence = UINT64_MAX;

    CHECK(fixture_initialize(&snapshot, &ring),
          "legacy apply fixture initializes");
    CHECK(controlled_configuration_apply(
              &snapshot, replacement_text,
              sizeof(replacement_text) - 1U,
              commit_accept, NULL, &ring, &sequence) ==
              CONTROLLED_CONFIGURATION_OK &&
              sequence == 0U,
          "legacy controlled configuration apply remains functional");
    CHECK(audit_record_pop(&ring, &record) == AUDIT_OK &&
              record.type == AUDIT_RECORD_CAPABILITY &&
              record.session_id == 0U &&
              record.principal_id == 0U &&
              record.command_id == 0U &&
              record.detail == CONTROLLED_CONFIGURATION_COMMAND_ID,
          "legacy controlled configuration audit record remains unchanged");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "legacy apply performs no allocation");
}

int main(void)
{
    test_validation();
    test_status_strings();
    test_exact_success_and_denied_records();
    test_malformed_event_rejection();
    test_ring_rejection_paths();
    test_controlled_configuration_correlated();
    test_legacy_apply_preserved();

    if (failures != 0) {
        fprintf(stderr, "Audit correlation host tests: %d FAILURES\n",
                failures);
        return 1;
    }
    puts("Audit correlation host tests: ALL OK");
    return 0;
}

#endif
