#include "health_check.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifdef HFOC_HEALTH_CHECK_HOST_TEST

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

static struct health_check_descriptor make_descriptor(
    uint32_t check_id,
    enum health_check_severity severity,
    uint32_t cadence_ms)
{
    struct health_check_descriptor descriptor;

    memset(&descriptor, 0, sizeof(descriptor));
    descriptor.version = HEALTH_CHECK_VERSION;
    descriptor.check_id = check_id;
    descriptor.severity = severity;
    descriptor.cadence_ms = cadence_ms;
    return descriptor;
}

static bool descriptor_equal(
    const struct health_check_descriptor *left,
    const struct health_check_descriptor *right)
{
    return left->version == right->version &&
           left->check_id == right->check_id &&
           left->severity == right->severity &&
           left->cadence_ms == right->cadence_ms;
}

static void test_status_strings(void)
{
    static const struct {
        enum health_check_status status;
        const char *string;
    } statuses[] = {
        {HEALTH_CHECK_OK, "HEALTH_CHECK_OK"},
        {HEALTH_CHECK_NULL, "HEALTH_CHECK_NULL"},
        {HEALTH_CHECK_BAD_STATE, "HEALTH_CHECK_BAD_STATE"},
        {HEALTH_CHECK_ALREADY_PUBLISHED,
         "HEALTH_CHECK_ALREADY_PUBLISHED"},
        {HEALTH_CHECK_BAD_VERSION, "HEALTH_CHECK_BAD_VERSION"},
        {HEALTH_CHECK_BAD_COUNT, "HEALTH_CHECK_BAD_COUNT"},
        {HEALTH_CHECK_BAD_ID, "HEALTH_CHECK_BAD_ID"},
        {HEALTH_CHECK_BAD_SEVERITY, "HEALTH_CHECK_BAD_SEVERITY"},
        {HEALTH_CHECK_BAD_CADENCE, "HEALTH_CHECK_BAD_CADENCE"},
        {HEALTH_CHECK_DUPLICATE, "HEALTH_CHECK_DUPLICATE"},
        {HEALTH_CHECK_NOT_PUBLISHED, "HEALTH_CHECK_NOT_PUBLISHED"},
        {HEALTH_CHECK_UNKNOWN, "HEALTH_CHECK_UNKNOWN"},
        {HEALTH_CHECK_BAD_RESULT, "HEALTH_CHECK_BAD_RESULT"},
    };
    static const struct {
        enum health_check_severity severity;
        const char *string;
    } severities[] = {
        {HEALTH_CHECK_SEVERITY_INFO, "HEALTH_CHECK_SEVERITY_INFO"},
        {HEALTH_CHECK_SEVERITY_WARNING,
         "HEALTH_CHECK_SEVERITY_WARNING"},
        {HEALTH_CHECK_SEVERITY_CRITICAL,
         "HEALTH_CHECK_SEVERITY_CRITICAL"},
    };
    static const struct {
        enum health_check_result_state result;
        const char *string;
    } results[] = {
        {HEALTH_CHECK_RESULT_UNKNOWN,
         "HEALTH_CHECK_RESULT_UNKNOWN"},
        {HEALTH_CHECK_RESULT_PASS, "HEALTH_CHECK_RESULT_PASS"},
        {HEALTH_CHECK_RESULT_WARN, "HEALTH_CHECK_RESULT_WARN"},
        {HEALTH_CHECK_RESULT_FAIL, "HEALTH_CHECK_RESULT_FAIL"},
    };
    size_t index;

    for (index = 0U;
         index < sizeof(statuses) / sizeof(statuses[0]);
         ++index) {
        CHECK(health_check_status_string(statuses[index].status) != NULL &&
                  strcmp(health_check_status_string(statuses[index].status),
                         statuses[index].string) == 0,
              "health-check status string is exact");
    }
    CHECK(health_check_status_string((enum health_check_status)13U) == NULL,
          "health-check undefined status is rejected");

    for (index = 0U;
         index < sizeof(severities) / sizeof(severities[0]);
         ++index) {
        CHECK(health_check_severity_string(
                  severities[index].severity) != NULL &&
                  strcmp(health_check_severity_string(
                             severities[index].severity),
                         severities[index].string) == 0,
              "health-check severity string is exact");
    }
    CHECK(health_check_severity_string(
              (enum health_check_severity)0U) == NULL,
          "health-check undefined severity is rejected");
    CHECK(health_check_severity_string(
              (enum health_check_severity)4U) == NULL,
          "health-check over-limit severity is rejected");

    for (index = 0U;
         index < sizeof(results) / sizeof(results[0]);
         ++index) {
        CHECK(health_check_result_string(
                  results[index].result) != NULL &&
                  strcmp(health_check_result_string(
                             results[index].result),
                         results[index].string) == 0,
              "health-check result string is exact");
    }
    CHECK(health_check_result_string(
              (enum health_check_result_state)4U) == NULL,
          "health-check undefined result is rejected");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "health-check status handling performs no allocation");
}

static void test_init_and_null_arguments(void)
{
    struct health_check_store store;
    struct health_check_descriptor descriptor =
        make_descriptor(1U, HEALTH_CHECK_SEVERITY_INFO, 1000U);
    struct health_check_snapshot snapshot;
    struct health_check_descriptor descriptor_out;

    memset(&store, 0, sizeof(store));
    CHECK(health_check_init(NULL) == HEALTH_CHECK_NULL,
          "health-check init rejects null store");
    CHECK(health_check_init(&store) == HEALTH_CHECK_OK &&
              store.version == HEALTH_CHECK_VERSION &&
              store.initialized,
          "health-check init publishes clean store");
    CHECK(health_check_init(&store) == HEALTH_CHECK_BAD_STATE,
          "health-check init rejects reinitialization");

    CHECK(health_check_publish(NULL, &descriptor, 1U) ==
              HEALTH_CHECK_NULL,
          "health-check publish rejects null store");
    CHECK(health_check_publish(&store, NULL, 1U) == HEALTH_CHECK_NULL,
          "health-check publish rejects null descriptors");
    memset(&store, 0, sizeof(store));
    CHECK(health_check_publish(&store, &descriptor, 1U) ==
              HEALTH_CHECK_BAD_STATE,
          "health-check publish rejects uninitialized store");

    CHECK(health_check_result(NULL, 1U, HEALTH_CHECK_RESULT_PASS) ==
              HEALTH_CHECK_NULL,
          "health-check result rejects null store");
    memset(&store, 0, sizeof(store));
    CHECK(health_check_result(&store, 1U, HEALTH_CHECK_RESULT_PASS) ==
              HEALTH_CHECK_BAD_STATE,
          "health-check result rejects uninitialized store");

    CHECK(health_check_lookup(NULL, 1U, &descriptor_out) ==
              HEALTH_CHECK_NULL,
          "health-check lookup rejects null store");
    memset(&store, 0, sizeof(store));
    CHECK(health_check_lookup(&store, 1U, &descriptor_out) ==
              HEALTH_CHECK_BAD_STATE,
          "health-check lookup rejects uninitialized store");
    CHECK(health_check_lookup(&store, 1U, NULL) == HEALTH_CHECK_NULL,
          "health-check lookup rejects null descriptor output");

    CHECK(health_check_snapshot(NULL, &snapshot) == HEALTH_CHECK_NULL,
          "health-check snapshot rejects null store");
    memset(&store, 0, sizeof(store));
    CHECK(health_check_snapshot(&store, NULL) == HEALTH_CHECK_NULL,
          "health-check snapshot rejects null snapshot output");
    CHECK(health_check_snapshot(&store, &snapshot) ==
              HEALTH_CHECK_BAD_STATE,
          "health-check snapshot rejects uninitialized store");
}

static void check_publish_failure(
    const struct health_check_descriptor *descriptors,
    size_t descriptor_count,
    enum health_check_status expected,
    const char *message)
{
    struct health_check_store store;
    unsigned char before[sizeof(store)];

    memset(&store, 0, sizeof(store));
    CHECK(health_check_init(&store) == HEALTH_CHECK_OK,
          "health-check invalid-publish fixture initializes");
    memcpy(before, &store, sizeof(before));
    CHECK(health_check_publish(&store, descriptors, descriptor_count) ==
              expected &&
              memcmp(&store, before, sizeof(before)) == 0,
          message);
}

static void test_invalid_descriptors_never_publish(void)
{
    struct health_check_descriptor descriptor;
    struct health_check_descriptor duplicates[2];
    struct health_check_descriptor many[
        HEALTH_CHECK_MAX_DESCRIPTORS + 1U];
    size_t index;

    descriptor = make_descriptor(
        1U, HEALTH_CHECK_SEVERITY_INFO, 1000U);
    check_publish_failure(&descriptor, 0U, HEALTH_CHECK_BAD_COUNT,
                          "health-check publish rejects zero count");

    for (index = 0U;
         index < sizeof(many) / sizeof(many[0]);
         ++index) {
        many[index] = make_descriptor(
            (uint32_t)index + 1U,
            HEALTH_CHECK_SEVERITY_INFO, 1000U);
    }
    check_publish_failure(
        many, sizeof(many) / sizeof(many[0]),
        HEALTH_CHECK_BAD_COUNT,
        "health-check publish rejects oversized descriptor set");

    descriptor = make_descriptor(
        1U, HEALTH_CHECK_SEVERITY_INFO, 1000U);
    descriptor.version = HEALTH_CHECK_VERSION - 1U;
    check_publish_failure(&descriptor, 1U, HEALTH_CHECK_BAD_VERSION,
                          "health-check publish rejects old version");

    descriptor = make_descriptor(
        1U, HEALTH_CHECK_SEVERITY_INFO, 1000U);
    descriptor.version = HEALTH_CHECK_VERSION + 1U;
    check_publish_failure(&descriptor, 1U, HEALTH_CHECK_BAD_VERSION,
                          "health-check publish rejects future version");

    descriptor = make_descriptor(
        0U, HEALTH_CHECK_SEVERITY_INFO, 1000U);
    check_publish_failure(&descriptor, 1U, HEALTH_CHECK_BAD_ID,
                          "health-check publish rejects zero ID");

    descriptor = make_descriptor(
        HEALTH_CHECK_ID_MAX + 1U,
        HEALTH_CHECK_SEVERITY_INFO, 1000U);
    check_publish_failure(&descriptor, 1U, HEALTH_CHECK_BAD_ID,
                          "health-check publish rejects over-limit ID");

    descriptor = make_descriptor(
        1U, (enum health_check_severity)0U, 1000U);
    check_publish_failure(&descriptor, 1U, HEALTH_CHECK_BAD_SEVERITY,
                          "health-check publish rejects zero severity");

    descriptor = make_descriptor(
        1U, (enum health_check_severity)4U, 1000U);
    check_publish_failure(&descriptor, 1U, HEALTH_CHECK_BAD_SEVERITY,
                          "health-check publish rejects over-limit severity");

    descriptor = make_descriptor(
        1U, HEALTH_CHECK_SEVERITY_INFO,
        HEALTH_CHECK_CADENCE_MIN_MS - 1U);
    check_publish_failure(&descriptor, 1U, HEALTH_CHECK_BAD_CADENCE,
                          "health-check publish rejects under-limit cadence");

    descriptor = make_descriptor(
        1U, HEALTH_CHECK_SEVERITY_INFO,
        HEALTH_CHECK_CADENCE_MAX_MS + 1U);
    check_publish_failure(&descriptor, 1U, HEALTH_CHECK_BAD_CADENCE,
                          "health-check publish rejects over-limit cadence");

    duplicates[0] = make_descriptor(
        1U, HEALTH_CHECK_SEVERITY_INFO, 1000U);
    duplicates[1] = make_descriptor(
        1U, HEALTH_CHECK_SEVERITY_CRITICAL, 2000U);
    check_publish_failure(
        duplicates, 2U, HEALTH_CHECK_DUPLICATE,
        "health-check publish rejects duplicate IDs");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "health-check invalid publication performs no allocation");
}

static void test_successful_publish_lookup_and_snapshot(void)
{
    struct health_check_store store;
    struct health_check_descriptor descriptors[3];
    struct health_check_descriptor descriptor_out;
    struct health_check_snapshot snapshot;
    unsigned char before[sizeof(store)];

    descriptors[0] = make_descriptor(
        1U, HEALTH_CHECK_SEVERITY_INFO, 1000U);
    descriptors[1] = make_descriptor(
        2U, HEALTH_CHECK_SEVERITY_WARNING, 5000U);
    descriptors[2] = make_descriptor(
        3U, HEALTH_CHECK_SEVERITY_CRITICAL,
        HEALTH_CHECK_CADENCE_MAX_MS);

    memset(&store, 0, sizeof(store));
    CHECK(health_check_init(&store) == HEALTH_CHECK_OK,
          "health-check successful-publish fixture initializes");
    CHECK(health_check_publish(
              &store, descriptors,
              sizeof(descriptors) / sizeof(descriptors[0])) ==
              HEALTH_CHECK_OK &&
              store.published &&
              store.generation == 1U &&
              store.descriptor_count == 3U &&
              descriptor_equal(&store.entries[0].descriptor,
                               &descriptors[0]) &&
              descriptor_equal(&store.entries[1].descriptor,
                               &descriptors[1]) &&
              descriptor_equal(&store.entries[2].descriptor,
                               &descriptors[2]) &&
              store.entries[0].result == HEALTH_CHECK_RESULT_UNKNOWN &&
              store.entries[1].result == HEALTH_CHECK_RESULT_UNKNOWN &&
              store.entries[2].result == HEALTH_CHECK_RESULT_UNKNOWN,
          "health-check publish copies exact descriptor set");

    memcpy(before, &store, sizeof(before));
    CHECK(health_check_publish(
              &store, descriptors,
              sizeof(descriptors) / sizeof(descriptors[0])) ==
              HEALTH_CHECK_ALREADY_PUBLISHED &&
              memcmp(&store, before, sizeof(before)) == 0,
          "health-check publish is one-shot and unchanged on re-publish");

    CHECK(health_check_lookup(&store, 1U, &descriptor_out) ==
              HEALTH_CHECK_OK &&
              descriptor_equal(&descriptor_out, &descriptors[0]),
          "health-check lookup returns first descriptor");
    CHECK(health_check_lookup(&store, 2U, &descriptor_out) ==
              HEALTH_CHECK_OK &&
              descriptor_equal(&descriptor_out, &descriptors[1]),
          "health-check lookup returns second descriptor");
    CHECK(health_check_lookup(&store, 3U, &descriptor_out) ==
              HEALTH_CHECK_OK &&
              descriptor_equal(&descriptor_out, &descriptors[2]),
          "health-check lookup returns third descriptor");
    CHECK(health_check_lookup(&store, 4U, &descriptor_out) ==
              HEALTH_CHECK_UNKNOWN,
          "health-check lookup rejects unknown ID");

    memset(&snapshot, 0xA5, sizeof(snapshot));
    CHECK(health_check_snapshot(&store, &snapshot) == HEALTH_CHECK_OK &&
              snapshot.version == HEALTH_CHECK_VERSION &&
              snapshot.generation == 1U &&
              snapshot.descriptor_count == 3U &&
              descriptor_equal(&snapshot.entries[0].descriptor,
                               &descriptors[0]) &&
              descriptor_equal(&snapshot.entries[1].descriptor,
                               &descriptors[1]) &&
              descriptor_equal(&snapshot.entries[2].descriptor,
                               &descriptors[2]) &&
              snapshot.entries[0].result ==
                  HEALTH_CHECK_RESULT_UNKNOWN &&
              snapshot.entries[1].result ==
                  HEALTH_CHECK_RESULT_UNKNOWN &&
              snapshot.entries[2].result ==
                  HEALTH_CHECK_RESULT_UNKNOWN,
          "health-check snapshot is exact and coherent");
}

static void test_result_updates(void)
{
    struct health_check_store store;
    struct health_check_descriptor descriptor =
        make_descriptor(1U, HEALTH_CHECK_SEVERITY_CRITICAL, 60000U);
    struct health_check_snapshot snapshot;

    memset(&store, 0, sizeof(store));
    CHECK(health_check_init(&store) == HEALTH_CHECK_OK,
          "health-check result fixture initializes");
    CHECK(health_check_result(&store, 1U, HEALTH_CHECK_RESULT_PASS) ==
              HEALTH_CHECK_NOT_PUBLISHED,
          "health-check result rejects unpublished store");
    CHECK(health_check_publish(&store, &descriptor, 1U) ==
              HEALTH_CHECK_OK,
          "health-check result fixture publishes descriptor");

    CHECK(health_check_result(&store, 1U,
              (enum health_check_result_state)4U) ==
              HEALTH_CHECK_BAD_RESULT,
          "health-check result rejects undefined state");
    CHECK(health_check_result(&store, 2U, HEALTH_CHECK_RESULT_PASS) ==
              HEALTH_CHECK_UNKNOWN,
          "health-check result rejects unknown ID");

    CHECK(health_check_result(&store, 1U, HEALTH_CHECK_RESULT_PASS) ==
              HEALTH_CHECK_OK,
          "health-check result accepts PASS");
    CHECK(health_check_result(&store, 1U, HEALTH_CHECK_RESULT_WARN) ==
              HEALTH_CHECK_OK,
          "health-check result accepts WARN");
    CHECK(health_check_result(&store, 1U, HEALTH_CHECK_RESULT_FAIL) ==
              HEALTH_CHECK_OK,
          "health-check result accepts FAIL");

    memset(&snapshot, 0, sizeof(snapshot));
    CHECK(health_check_snapshot(&store, &snapshot) == HEALTH_CHECK_OK &&
              snapshot.entries[0].result == HEALTH_CHECK_RESULT_FAIL,
          "health-check result update persists in snapshot");
}

static void test_lookup_and_snapshot_before_publish(void)
{
    struct health_check_store store;
    struct health_check_descriptor descriptor_out;
    struct health_check_snapshot snapshot;

    memset(&store, 0, sizeof(store));
    CHECK(health_check_init(&store) == HEALTH_CHECK_OK,
          "health-check lookup/snapshot fixture initializes");
    CHECK(health_check_lookup(&store, 1U, &descriptor_out) ==
              HEALTH_CHECK_NOT_PUBLISHED,
          "health-check lookup rejects unpublished store");
    CHECK(health_check_snapshot(&store, &snapshot) ==
              HEALTH_CHECK_NOT_PUBLISHED,
          "health-check snapshot rejects unpublished store");
}

int main(void)
{
    test_status_strings();
    test_init_and_null_arguments();
    test_invalid_descriptors_never_publish();
    test_successful_publish_lookup_and_snapshot();
    test_result_updates();
    test_lookup_and_snapshot_before_publish();

    if (failures != 0) {
        fprintf(stderr, "%d health-check host test failures\n",
                failures);
        return 1;
    }
    printf("Health-check host tests: ALL OK\n");
    return 0;
}

#endif
