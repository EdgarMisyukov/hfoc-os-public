#include "appliance_runtime_log.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifdef APPLIANCE_RUNTIME_LOG_HOST_TEST

static int failures;

#define CHECK(cond, msg) do { \
    if (!(cond)) { \
        fprintf(stderr, "FAIL: %s\n", msg); \
        ++failures; \
    } else { \
        printf("PASS: %s\n", msg); \
    } \
} while (0)

static struct appliance_runtime_log_record make_record(void)
{
    struct appliance_runtime_log_record record;

    memset(&record, 0, sizeof(record));
    record.version = APPLIANCE_RUNTIME_LOG_VERSION;
    record.severity = APPLIANCE_RUNTIME_LOG_SEVERITY_INFO;
    record.category = APPLIANCE_RUNTIME_LOG_CATEGORY_CONFIGURATION;
    record.operation = 3U;
    record.result = 0U;
    record.fault = 0U;
    record.context_generation = 7U;
    record.ticks = 1000U;
    record.detail = UINT64_C(0x1234);
    return record;
}

static bool entry_eq(const struct appliance_runtime_log_entry *entry,
                     uint64_t sequence,
                     const struct appliance_runtime_log_record *record)
{
    return entry->sequence == sequence &&
           entry->ticks == record->ticks &&
           entry->severity == record->severity &&
           entry->category == record->category &&
           entry->operation == record->operation &&
           entry->result == record->result &&
           entry->fault == record->fault &&
           entry->context_generation == record->context_generation &&
           entry->detail == record->detail;
}

static bool state_ok(const struct appliance_runtime_log_state *state,
                     uint32_t count)
{
    return state->published &&
           state->version == APPLIANCE_RUNTIME_LOG_VERSION &&
           state->count == count &&
           state->capacity == APPLIANCE_RUNTIME_LOG_CAPACITY &&
           state->available ==
               APPLIANCE_RUNTIME_LOG_CAPACITY - count &&
           state->utilization_percent ==
               (count * 100U) / APPLIANCE_RUNTIME_LOG_CAPACITY;
}

static void test_strings(void)
{
    static const struct {
        enum appliance_runtime_log_status status;
        const char *string;
    } statuses[] = {
        {APPLIANCE_RUNTIME_LOG_OK, "APPLIANCE_RUNTIME_LOG_OK"},
        {APPLIANCE_RUNTIME_LOG_NULL, "APPLIANCE_RUNTIME_LOG_NULL"},
        {APPLIANCE_RUNTIME_LOG_BAD_STATE,
         "APPLIANCE_RUNTIME_LOG_BAD_STATE"},
        {APPLIANCE_RUNTIME_LOG_BAD_VERSION,
         "APPLIANCE_RUNTIME_LOG_BAD_VERSION"},
        {APPLIANCE_RUNTIME_LOG_BAD_SEVERITY,
         "APPLIANCE_RUNTIME_LOG_BAD_SEVERITY"},
        {APPLIANCE_RUNTIME_LOG_BAD_CATEGORY,
         "APPLIANCE_RUNTIME_LOG_BAD_CATEGORY"},
        {APPLIANCE_RUNTIME_LOG_BAD_OPERATION,
         "APPLIANCE_RUNTIME_LOG_BAD_OPERATION"},
        {APPLIANCE_RUNTIME_LOG_BAD_RESULT,
         "APPLIANCE_RUNTIME_LOG_BAD_RESULT"},
        {APPLIANCE_RUNTIME_LOG_BAD_FAULT,
         "APPLIANCE_RUNTIME_LOG_BAD_FAULT"},
        {APPLIANCE_RUNTIME_LOG_BAD_GENERATION,
         "APPLIANCE_RUNTIME_LOG_BAD_GENERATION"},
        {APPLIANCE_RUNTIME_LOG_BAD_DETAIL,
         "APPLIANCE_RUNTIME_LOG_BAD_DETAIL"},
        {APPLIANCE_RUNTIME_LOG_FULL, "APPLIANCE_RUNTIME_LOG_FULL"},
        {APPLIANCE_RUNTIME_LOG_EMPTY, "APPLIANCE_RUNTIME_LOG_EMPTY"},
        {APPLIANCE_RUNTIME_LOG_SNAPSHOT_OVERFLOW,
         "APPLIANCE_RUNTIME_LOG_SNAPSHOT_OVERFLOW"},
    };
    static const struct {
        enum appliance_runtime_log_severity severity;
        const char *string;
    } severities[] = {
        {APPLIANCE_RUNTIME_LOG_SEVERITY_NONE, "NONE"},
        {APPLIANCE_RUNTIME_LOG_SEVERITY_DEBUG, "DEBUG"},
        {APPLIANCE_RUNTIME_LOG_SEVERITY_INFO, "INFO"},
        {APPLIANCE_RUNTIME_LOG_SEVERITY_WARN, "WARN"},
        {APPLIANCE_RUNTIME_LOG_SEVERITY_ERROR, "ERROR"},
        {APPLIANCE_RUNTIME_LOG_SEVERITY_CRITICAL, "CRITICAL"},
    };
    static const struct {
        enum appliance_runtime_log_category category;
        const char *string;
    } categories[] = {
        {APPLIANCE_RUNTIME_LOG_CATEGORY_NONE, "NONE"},
        {APPLIANCE_RUNTIME_LOG_CATEGORY_BOOT, "BOOT"},
        {APPLIANCE_RUNTIME_LOG_CATEGORY_STORAGE, "STORAGE"},
        {APPLIANCE_RUNTIME_LOG_CATEGORY_CONFIGURATION, "CONFIGURATION"},
        {APPLIANCE_RUNTIME_LOG_CATEGORY_NETWORK, "NETWORK"},
        {APPLIANCE_RUNTIME_LOG_CATEGORY_SERVICE, "SERVICE"},
        {APPLIANCE_RUNTIME_LOG_CATEGORY_HTTPS_MANAGEMENT,
         "HTTPS_MANAGEMENT"},
        {APPLIANCE_RUNTIME_LOG_CATEGORY_MANAGEMENT, "MANAGEMENT"},
        {APPLIANCE_RUNTIME_LOG_CATEGORY_SECURITY, "SECURITY"},
        {APPLIANCE_RUNTIME_LOG_CATEGORY_PROCESS, "PROCESS"},
    };
    size_t index;

    for (index = 0U;
         index < sizeof(statuses) / sizeof(statuses[0]);
         ++index) {
        const char *string = appliance_runtime_log_status_string(
            statuses[index].status);

        CHECK(string != NULL &&
                  strcmp(string, statuses[index].string) == 0,
              "appliance runtime log status string is exact");
    }
    for (index = 0U;
         index < sizeof(severities) / sizeof(severities[0]);
         ++index) {
        const char *string = appliance_runtime_log_severity_string(
            severities[index].severity);

        CHECK(string != NULL &&
                  strcmp(string, severities[index].string) == 0,
              "appliance runtime log severity string is exact");
    }
    for (index = 0U;
         index < sizeof(categories) / sizeof(categories[0]);
         ++index) {
        const char *string = appliance_runtime_log_category_string(
            categories[index].category);

        CHECK(string != NULL &&
                  strcmp(string, categories[index].string) == 0,
              "appliance runtime log category string is exact");
    }
    CHECK(appliance_runtime_log_status_string(
              (enum appliance_runtime_log_status)99U) == NULL,
          "appliance runtime log rejects undefined status string");
    CHECK(appliance_runtime_log_severity_string(
              (enum appliance_runtime_log_severity)99U) == NULL,
          "appliance runtime log rejects undefined severity string");
    CHECK(appliance_runtime_log_category_string(
              (enum appliance_runtime_log_category)99U) == NULL,
          "appliance runtime log rejects undefined category string");
}

static void test_init_destroy_and_bad_state(void)
{
    struct appliance_runtime_log log;
    struct appliance_runtime_log_state state;
    struct appliance_runtime_log_record record = make_record();
    struct appliance_runtime_log_entry entry;
    uint64_t sequence = UINT64_C(0xA5A5);
    uint32_t count = 0U;

    memset(&log, 0, sizeof(log));
    CHECK(appliance_runtime_log_append(&log, &record, &sequence) ==
              APPLIANCE_RUNTIME_LOG_BAD_STATE &&
              sequence == UINT64_C(0xA5A5) && log.count == 0U,
          "uninitialized appliance runtime log rejects append without mutation");
    memset(&entry, 0xA5, sizeof(entry));
    CHECK(appliance_runtime_log_pop(&log, &entry) ==
              APPLIANCE_RUNTIME_LOG_BAD_STATE,
          "uninitialized appliance runtime log rejects pop");
    CHECK(appliance_runtime_log_snapshot(&log, NULL, 0U, &count) ==
              APPLIANCE_RUNTIME_LOG_BAD_STATE,
          "uninitialized appliance runtime log rejects snapshot");
    CHECK(appliance_runtime_log_state(&log, &state) ==
              APPLIANCE_RUNTIME_LOG_BAD_STATE,
          "uninitialized appliance runtime log rejects state");
    CHECK(appliance_runtime_log_init(&log) == APPLIANCE_RUNTIME_LOG_OK,
          "appliance runtime log init succeeds from zeroed storage");
    memset(&state, 0, sizeof(state));
    CHECK(appliance_runtime_log_state(&log, &state) ==
              APPLIANCE_RUNTIME_LOG_OK && state_ok(&state, 0U),
          "appliance runtime log publishes empty state after init");
    CHECK(appliance_runtime_log_init(&log) ==
              APPLIANCE_RUNTIME_LOG_BAD_STATE,
          "appliance runtime log rejects double init");
    CHECK(appliance_runtime_log_destroy(&log) == APPLIANCE_RUNTIME_LOG_OK &&
              !log.published && log.count == 0U,
          "appliance runtime log destroy clears published state");
    CHECK(appliance_runtime_log_destroy(&log) ==
              APPLIANCE_RUNTIME_LOG_BAD_STATE,
          "appliance runtime log rejects double destroy");
}

static void test_append_pop_snapshot_state(void)
{
    struct appliance_runtime_log log;
    struct appliance_runtime_log_entry entries[2];
    struct appliance_runtime_log_entry popped;
    struct appliance_runtime_log_state state;
    struct appliance_runtime_log_record record = make_record();
    uint64_t sequence = 0U;
    uint32_t count = 0U;

    memset(&log, 0, sizeof(log));
    CHECK(appliance_runtime_log_init(&log) == APPLIANCE_RUNTIME_LOG_OK,
          "appliance runtime log append fixture init succeeds");
    CHECK(appliance_runtime_log_append(&log, &record, &sequence) ==
              APPLIANCE_RUNTIME_LOG_OK && sequence == 0U,
          "appliance runtime log append assigns first sequence");
    CHECK(appliance_runtime_log_state(&log, &state) ==
              APPLIANCE_RUNTIME_LOG_OK && state_ok(&state, 1U),
          "appliance runtime log state publishes count after append");
    record.severity = APPLIANCE_RUNTIME_LOG_SEVERITY_WARN;
    record.category = APPLIANCE_RUNTIME_LOG_CATEGORY_NETWORK;
    record.ticks = 2000U;
    CHECK(appliance_runtime_log_append(&log, &record, &sequence) ==
              APPLIANCE_RUNTIME_LOG_OK && sequence == 1U,
          "appliance runtime log append increments sequence");
    CHECK(appliance_runtime_log_snapshot(
              &log, entries, 2U, &count) == APPLIANCE_RUNTIME_LOG_OK &&
              count == 2U &&
              entry_eq(&entries[0], 0U,
                       &(struct appliance_runtime_log_record){
                           .version = APPLIANCE_RUNTIME_LOG_VERSION,
                           .severity = APPLIANCE_RUNTIME_LOG_SEVERITY_INFO,
                           .category =
                               APPLIANCE_RUNTIME_LOG_CATEGORY_CONFIGURATION,
                           .operation = 3U,
                           .context_generation = 7U,
                           .ticks = 1000U,
                           .detail = UINT64_C(0x1234),
                       }) &&
              entry_eq(&entries[1], 1U, &record),
          "appliance runtime log snapshot preserves append order");
    CHECK(appliance_runtime_log_state(&log, &state) ==
              APPLIANCE_RUNTIME_LOG_OK && state_ok(&state, 2U),
          "appliance runtime log snapshot leaves log unchanged");
    CHECK(appliance_runtime_log_pop(&log, &popped) ==
              APPLIANCE_RUNTIME_LOG_OK && popped.sequence == 0U,
          "appliance runtime log pops oldest first");
    CHECK(appliance_runtime_log_pop(&log, &popped) ==
              APPLIANCE_RUNTIME_LOG_OK && popped.sequence == 1U,
          "appliance runtime log pop advances sequence cursor");
    CHECK(appliance_runtime_log_pop(&log, &popped) ==
              APPLIANCE_RUNTIME_LOG_EMPTY,
          "appliance runtime log returns empty when drained");
}

static void test_full_empty_and_overflow(void)
{
    struct appliance_runtime_log log;
    struct appliance_runtime_log_entry entries[APPLIANCE_RUNTIME_LOG_CAPACITY];
    struct appliance_runtime_log_state state;
    struct appliance_runtime_log_record record = make_record();
    uint64_t sequence = 0U;
    uint32_t count = 0U;
    uint32_t index;

    memset(&log, 0, sizeof(log));
    CHECK(appliance_runtime_log_init(&log) == APPLIANCE_RUNTIME_LOG_OK,
          "appliance runtime log full fixture init succeeds");
    for (index = 0U; index < APPLIANCE_RUNTIME_LOG_CAPACITY; ++index) {
        CHECK(appliance_runtime_log_append(&log, &record, &sequence) ==
                  APPLIANCE_RUNTIME_LOG_OK &&
                  sequence == index,
              "appliance runtime log fills to capacity");
    }
    CHECK(appliance_runtime_log_append(&log, &record, &sequence) ==
              APPLIANCE_RUNTIME_LOG_FULL && sequence == 63U,
          "appliance runtime log reject full append without changing sequence");
    CHECK(appliance_runtime_log_state(&log, &state) ==
              APPLIANCE_RUNTIME_LOG_OK && state_ok(&state, 64U) &&
              state.available == 0U && state.utilization_percent == 100U,
          "appliance runtime log state publishes full at 100 percent");
    CHECK(appliance_runtime_log_snapshot(
              &log, NULL, 0U, &count) == APPLIANCE_RUNTIME_LOG_OK &&
              count == APPLIANCE_RUNTIME_LOG_CAPACITY,
          "appliance runtime log snapshot query reports required count");
    count = 0U;
    CHECK(appliance_runtime_log_snapshot(
              &log, entries, 63U, &count) ==
                  APPLIANCE_RUNTIME_LOG_SNAPSHOT_OVERFLOW &&
              count == APPLIANCE_RUNTIME_LOG_CAPACITY,
          "appliance runtime log snapshot overflow reports required count");
    CHECK(appliance_runtime_log_snapshot(
              &log, entries, APPLIANCE_RUNTIME_LOG_CAPACITY,
              &count) == APPLIANCE_RUNTIME_LOG_OK &&
              count == APPLIANCE_RUNTIME_LOG_CAPACITY,
          "appliance runtime log snapshot at capacity succeeds");
    CHECK(appliance_runtime_log_pop(&log, &entries[0]) ==
              APPLIANCE_RUNTIME_LOG_OK && entries[0].sequence == 0U,
          "appliance runtime log pop after full drains one entry");
    CHECK(appliance_runtime_log_append(&log, &record, &sequence) ==
              APPLIANCE_RUNTIME_LOG_OK && sequence == 64U,
          "appliance runtime log append reuses drained slot");
}

static void test_invalid_records_never_mutate(void)
{
    struct appliance_runtime_log log;
    struct appliance_runtime_log_state before;
    struct appliance_runtime_log_state after;
    struct appliance_runtime_log_record record = make_record();
    struct appliance_runtime_log_record valid;
    uint64_t sequence = UINT64_C(0xAA);

    memset(&log, 0, sizeof(log));
    CHECK(appliance_runtime_log_init(&log) == APPLIANCE_RUNTIME_LOG_OK,
          "appliance runtime log reject fixture init succeeds");
    CHECK(appliance_runtime_log_append(&log, &record, &sequence) ==
              APPLIANCE_RUNTIME_LOG_OK &&
              sequence == 0U,
          "appliance runtime log reject fixture seed append succeeds");
    sequence = UINT64_C(0xA5);
    CHECK(appliance_runtime_log_state(&log, &before) ==
              APPLIANCE_RUNTIME_LOG_OK,
          "appliance runtime log captures pre-reject state");

#define REJECT_LOG_RECORD(field, value, expected, msg) do { \
        struct appliance_runtime_log_record candidate = make_record(); \
        candidate.field = value; \
        CHECK(appliance_runtime_log_append(&log, &candidate, &sequence) == \
                  expected && sequence == UINT64_C(0xA5), \
              msg); \
    } while (0)

    REJECT_LOG_RECORD(version, 0U, APPLIANCE_RUNTIME_LOG_BAD_VERSION,
                      "appliance runtime log rejects bad record version");
    REJECT_LOG_RECORD(severity, APPLIANCE_RUNTIME_LOG_SEVERITY_NONE,
                      APPLIANCE_RUNTIME_LOG_BAD_SEVERITY,
                      "appliance runtime log rejects severity none");
    REJECT_LOG_RECORD(severity, APPLIANCE_RUNTIME_LOG_SEVERITY_COUNT,
                      APPLIANCE_RUNTIME_LOG_BAD_SEVERITY,
                      "appliance runtime log rejects severity count");
    REJECT_LOG_RECORD(category, APPLIANCE_RUNTIME_LOG_CATEGORY_NONE,
                      APPLIANCE_RUNTIME_LOG_BAD_CATEGORY,
                      "appliance runtime log rejects category none");
    REJECT_LOG_RECORD(category, APPLIANCE_RUNTIME_LOG_CATEGORY_COUNT,
                      APPLIANCE_RUNTIME_LOG_BAD_CATEGORY,
                      "appliance runtime log rejects category count");
    REJECT_LOG_RECORD(operation, 0U, APPLIANCE_RUNTIME_LOG_BAD_OPERATION,
                      "appliance runtime log rejects zero operation");
    REJECT_LOG_RECORD(operation, 256U, APPLIANCE_RUNTIME_LOG_BAD_OPERATION,
                      "appliance runtime log rejects oversized operation");
    REJECT_LOG_RECORD(result, 256U, APPLIANCE_RUNTIME_LOG_BAD_RESULT,
                      "appliance runtime log rejects oversized result");
    REJECT_LOG_RECORD(fault, 65536U, APPLIANCE_RUNTIME_LOG_BAD_FAULT,
                      "appliance runtime log rejects oversized fault");
    REJECT_LOG_RECORD(context_generation, 16777216U,
                      APPLIANCE_RUNTIME_LOG_BAD_GENERATION,
                      "appliance runtime log rejects oversized generation");
    REJECT_LOG_RECORD(detail, UINT64_C(0x100000000),
                      APPLIANCE_RUNTIME_LOG_BAD_DETAIL,
                      "appliance runtime log rejects oversized detail");

#undef REJECT_LOG_RECORD

    CHECK(appliance_runtime_log_state(&log, &after) ==
              APPLIANCE_RUNTIME_LOG_OK &&
              before.published == after.published &&
              before.count == after.count &&
              before.next_sequence == after.next_sequence &&
              before.write_cursor == after.write_cursor &&
              before.read_cursor == after.read_cursor &&
              sequence == UINT64_C(0xA5),
          "appliance runtime log invalid records never mutate state");
    CHECK(appliance_runtime_log_validate_record(NULL) ==
              APPLIANCE_RUNTIME_LOG_NULL,
          "appliance runtime log validator rejects null record");
    valid = make_record();
    CHECK(appliance_runtime_log_validate_record(&valid) ==
              APPLIANCE_RUNTIME_LOG_OK,
          "appliance runtime log validator accepts valid record");
}

static void test_null_arguments(void)
{
    struct appliance_runtime_log log;
    struct appliance_runtime_log_record record = make_record();
    struct appliance_runtime_log_entry entry;
    struct appliance_runtime_log_state state;

    memset(&log, 0, sizeof(log));
    CHECK(appliance_runtime_log_validate_record(&record) ==
              APPLIANCE_RUNTIME_LOG_OK,
          "appliance runtime log validation base result is OK");
    CHECK(appliance_runtime_log_init(NULL) == APPLIANCE_RUNTIME_LOG_NULL,
          "appliance runtime log init rejects null log");
    CHECK(appliance_runtime_log_destroy(NULL) == APPLIANCE_RUNTIME_LOG_NULL,
          "appliance runtime log destroy rejects null log");
    CHECK(appliance_runtime_log_append(NULL, &record, NULL) ==
              APPLIANCE_RUNTIME_LOG_NULL,
          "appliance runtime log append rejects null log");
    CHECK(appliance_runtime_log_append(&log, NULL, NULL) ==
              APPLIANCE_RUNTIME_LOG_NULL,
          "appliance runtime log append rejects null record");
    CHECK(appliance_runtime_log_snapshot(NULL, NULL, 0U, NULL) ==
              APPLIANCE_RUNTIME_LOG_NULL,
          "appliance runtime log snapshot rejects null log");
    memset(&log, 0, sizeof(log));
    CHECK(appliance_runtime_log_init(&log) == APPLIANCE_RUNTIME_LOG_OK,
          "appliance runtime log null argument fixture init succeeds");
    CHECK(appliance_runtime_log_append(&log, &record, NULL) ==
              APPLIANCE_RUNTIME_LOG_OK,
          "appliance runtime log append succeeds with null sequence pointer");
    CHECK(appliance_runtime_log_snapshot(&log, NULL, 0U, NULL) ==
              APPLIANCE_RUNTIME_LOG_NULL,
          "appliance runtime log snapshot rejects null count output");
    CHECK(appliance_runtime_log_snapshot(
              &log, NULL, 1U, &(uint32_t){0}) == APPLIANCE_RUNTIME_LOG_NULL,
          "appliance runtime log snapshot rejects null output with nonzero capacity");
    CHECK(appliance_runtime_log_snapshot(
              &log, &entry, 0U, &(uint32_t){0}) == APPLIANCE_RUNTIME_LOG_NULL,
          "appliance runtime log snapshot rejects zero capacity with output");
    CHECK(appliance_runtime_log_pop(NULL, &entry) ==
              APPLIANCE_RUNTIME_LOG_NULL,
          "appliance runtime log pop rejects null log");
    CHECK(appliance_runtime_log_pop(&log, NULL) ==
              APPLIANCE_RUNTIME_LOG_NULL,
          "appliance runtime log pop rejects null entry");
    CHECK(appliance_runtime_log_state(NULL, &state) ==
              APPLIANCE_RUNTIME_LOG_NULL,
          "appliance runtime log state rejects null log");
    CHECK(appliance_runtime_log_state(&log, NULL) ==
              APPLIANCE_RUNTIME_LOG_NULL,
          "appliance runtime log state rejects null output");
}

int main(void)
{
    test_strings();
    test_init_destroy_and_bad_state();
    test_append_pop_snapshot_state();
    test_full_empty_and_overflow();
    test_invalid_records_never_mutate();
    test_null_arguments();

    if (failures != 0) {
        fprintf(stderr, "%d appliance runtime log host test failures\n",
                failures);
        return 1;
    }
    printf("Appliance runtime log host tests: ALL OK\n");
    return 0;
}

#endif
