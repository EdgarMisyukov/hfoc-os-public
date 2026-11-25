#include "first_boot_health.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifdef HFOC_FIRST_BOOT_HEALTH_HOST_TEST

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

static struct first_boot_health_requirements make_requirements(void)
{
    struct first_boot_health_requirements requirements;
    static const struct first_boot_health_milestone milestones[] = {
        {FIRST_BOOT_HEALTH_SUBSYSTEM_BOOT, 1U},
        {FIRST_BOOT_HEALTH_SUBSYSTEM_STORAGE, 1U},
        {FIRST_BOOT_HEALTH_SUBSYSTEM_SERVICE, 1U},
        {FIRST_BOOT_HEALTH_SUBSYSTEM_MANAGEMENT, 1U},
    };
    size_t index;

    memset(&requirements, 0, sizeof(requirements));
    requirements.version = FIRST_BOOT_HEALTH_VERSION;
    requirements.milestone_count =
        sizeof(milestones) / sizeof(milestones[0]);
    for (index = 0U;
         index < requirements.milestone_count;
         ++index) {
        requirements.milestones[index] = milestones[index];
    }
    return requirements;
}

static bool init_store(struct first_boot_health_store *store)
{
    struct first_boot_health_requirements requirements =
        make_requirements();

    memset(store, 0, sizeof(*store));
    return first_boot_health_init(store) == FIRST_BOOT_HEALTH_OK &&
           first_boot_health_define(store, &requirements) ==
               FIRST_BOOT_HEALTH_OK;
}

static enum first_boot_health_status report_milestone(
    struct first_boot_health_store *store,
    enum first_boot_health_subsystem subsystem,
    uint32_t milestone_id,
    bool healthy)
{
    struct first_boot_health_report report;

    memset(&report, 0, sizeof(report));
    report.version = FIRST_BOOT_HEALTH_VERSION;
    report.milestone.subsystem = subsystem;
    report.milestone.milestone_id = milestone_id;
    report.healthy = healthy;
    return first_boot_health_report(store, &report);
}

static bool milestone_equal(
    const struct first_boot_health_milestone *left,
    const struct first_boot_health_milestone *right)
{
    return left->subsystem == right->subsystem &&
           left->milestone_id == right->milestone_id;
}

static bool requirements_equal(
    const struct first_boot_health_requirements *left,
    const struct first_boot_health_requirements *right)
{
    size_t index;

    if (left->version != right->version ||
        left->milestone_count != right->milestone_count) {
        return false;
    }
    for (index = 0U;
         index < left->milestone_count;
         ++index) {
        if (!milestone_equal(&left->milestones[index],
                             &right->milestones[index])) {
            return false;
        }
    }
    return true;
}

static void test_status_strings(void)
{
    static const struct {
        enum first_boot_health_status status;
        const char *string;
    } statuses[] = {
        {FIRST_BOOT_HEALTH_OK, "FIRST_BOOT_HEALTH_OK"},
        {FIRST_BOOT_HEALTH_NULL, "FIRST_BOOT_HEALTH_NULL"},
        {FIRST_BOOT_HEALTH_BAD_STATE,
         "FIRST_BOOT_HEALTH_BAD_STATE"},
        {FIRST_BOOT_HEALTH_BAD_REQUEST,
         "FIRST_BOOT_HEALTH_BAD_REQUEST"},
        {FIRST_BOOT_HEALTH_BAD_SUBSYSTEM,
         "FIRST_BOOT_HEALTH_BAD_SUBSYSTEM"},
        {FIRST_BOOT_HEALTH_BAD_MILESTONE,
         "FIRST_BOOT_HEALTH_BAD_MILESTONE"},
        {FIRST_BOOT_HEALTH_DUPLICATE,
         "FIRST_BOOT_HEALTH_DUPLICATE"},
        {FIRST_BOOT_HEALTH_UNKNOWN, "FIRST_BOOT_HEALTH_UNKNOWN"},
        {FIRST_BOOT_HEALTH_NOT_READY,
         "FIRST_BOOT_HEALTH_NOT_READY"},
        {FIRST_BOOT_HEALTH_FAILED, "FIRST_BOOT_HEALTH_FAILED"},
        {FIRST_BOOT_HEALTH_ALREADY_DEFINED,
         "FIRST_BOOT_HEALTH_ALREADY_DEFINED"},
        {FIRST_BOOT_HEALTH_MISSING_SUBSYSTEM,
         "FIRST_BOOT_HEALTH_MISSING_SUBSYSTEM"},
    };
    size_t index;

    for (index = 0U;
         index < sizeof(statuses) / sizeof(statuses[0]);
         ++index) {
        const char *string = first_boot_health_status_string(
            statuses[index].status);

        CHECK(string != NULL &&
                  strcmp(string, statuses[index].string) == 0,
              "first-boot health status string is exact");
    }
    CHECK(first_boot_health_status_string(
              (enum first_boot_health_status)12U) == NULL,
          "first-boot health undefined status is rejected");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "first-boot health status handling performs no allocation");
}

static void test_init_and_define(void)
{
    struct first_boot_health_store store;
    struct first_boot_health_requirements requirements =
        make_requirements();
    struct first_boot_health_requirements invalid = requirements;

    CHECK(first_boot_health_init(NULL) == FIRST_BOOT_HEALTH_NULL,
          "first-boot health init rejects null store");
    CHECK(first_boot_health_define(NULL, &requirements) ==
              FIRST_BOOT_HEALTH_NULL,
          "first-boot health define rejects null store");
    memset(&store, 0, sizeof(store));
    CHECK(first_boot_health_define(&store, NULL) ==
              FIRST_BOOT_HEALTH_NULL,
          "first-boot health define rejects null requirements");
    CHECK(first_boot_health_define(&store, &requirements) ==
              FIRST_BOOT_HEALTH_BAD_STATE,
          "first-boot health define rejects uninitialized store");
    CHECK(first_boot_health_init(&store) == FIRST_BOOT_HEALTH_OK &&
              store.version == FIRST_BOOT_HEALTH_VERSION &&
              store.initialized,
          "first-boot health init publishes clean store");
    CHECK(first_boot_health_init(&store) == FIRST_BOOT_HEALTH_BAD_STATE,
          "first-boot health init rejects reinitialization");
    CHECK(first_boot_health_define(&store, &requirements) ==
              FIRST_BOOT_HEALTH_OK &&
              requirements_equal(&store.requirements, &requirements),
          "first-boot health define copies exact requirements");
    CHECK(first_boot_health_define(&store, &requirements) ==
              FIRST_BOOT_HEALTH_ALREADY_DEFINED,
          "first-boot health define is one-shot");

    memset(&store, 0, sizeof(store));
    CHECK(first_boot_health_init(&store) == FIRST_BOOT_HEALTH_OK,
          "first-boot health invalid-requirements fixture initializes");
    invalid.version = 0U;
    CHECK(first_boot_health_define(&store, &invalid) ==
              FIRST_BOOT_HEALTH_BAD_REQUEST,
          "first-boot health define rejects version mismatch");

    invalid = make_requirements();
    invalid.milestone_count = 0U;
    memset(&store, 0, sizeof(store));
    CHECK(first_boot_health_init(&store) == FIRST_BOOT_HEALTH_OK,
          "first-boot health empty-requirements fixture initializes");
    CHECK(first_boot_health_define(&store, &invalid) ==
              FIRST_BOOT_HEALTH_BAD_REQUEST,
          "first-boot health define rejects empty requirements");

    invalid = make_requirements();
    invalid.milestone_count = FIRST_BOOT_HEALTH_MAX_MILESTONES + 1U;
    memset(&store, 0, sizeof(store));
    CHECK(first_boot_health_init(&store) == FIRST_BOOT_HEALTH_OK,
          "first-boot health oversized-requirements fixture initializes");
    CHECK(first_boot_health_define(&store, &invalid) ==
              FIRST_BOOT_HEALTH_BAD_REQUEST,
          "first-boot health define rejects oversized requirements");

    invalid = make_requirements();
    invalid.milestones[0].subsystem =
        (enum first_boot_health_subsystem)0U;
    memset(&store, 0, sizeof(store));
    CHECK(first_boot_health_init(&store) == FIRST_BOOT_HEALTH_OK,
          "first-boot health bad-subsystem fixture initializes");
    CHECK(first_boot_health_define(&store, &invalid) ==
              FIRST_BOOT_HEALTH_BAD_SUBSYSTEM,
          "first-boot health define rejects invalid subsystem");

    invalid = make_requirements();
    invalid.milestones[0].milestone_id = 0U;
    memset(&store, 0, sizeof(store));
    CHECK(first_boot_health_init(&store) == FIRST_BOOT_HEALTH_OK,
          "first-boot health bad-milestone fixture initializes");
    CHECK(first_boot_health_define(&store, &invalid) ==
              FIRST_BOOT_HEALTH_BAD_MILESTONE,
          "first-boot health define rejects invalid milestone ID");

    invalid = make_requirements();
    invalid.milestones[1] = invalid.milestones[0];
    memset(&store, 0, sizeof(store));
    CHECK(first_boot_health_init(&store) == FIRST_BOOT_HEALTH_OK,
          "first-boot health duplicate fixture initializes");
    CHECK(first_boot_health_define(&store, &invalid) ==
              FIRST_BOOT_HEALTH_DUPLICATE,
          "first-boot health define rejects duplicate milestones");

    invalid = make_requirements();
    invalid.milestone_count = 3U;
    memset(&store, 0, sizeof(store));
    CHECK(first_boot_health_init(&store) == FIRST_BOOT_HEALTH_OK,
          "first-boot health missing-subsystem fixture initializes");
    CHECK(first_boot_health_define(&store, &invalid) ==
              FIRST_BOOT_HEALTH_MISSING_SUBSYSTEM,
          "first-boot health define rejects missing subsystem category");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "first-boot health define performs no allocation");
}

static void test_report_validation(void)
{
    struct first_boot_health_store store;
    struct first_boot_health_report report;
    struct first_boot_health_verdict verdict;

    memset(&store, 0, sizeof(store));
    CHECK(first_boot_health_report(NULL, NULL) ==
              FIRST_BOOT_HEALTH_NULL,
          "first-boot health report rejects null store and report");
    memset(&report, 0, sizeof(report));
    report.version = FIRST_BOOT_HEALTH_VERSION;
    report.milestone.subsystem = FIRST_BOOT_HEALTH_SUBSYSTEM_BOOT;
    report.milestone.milestone_id = 1U;
    report.healthy = true;
    CHECK(first_boot_health_report(&store, &report) ==
              FIRST_BOOT_HEALTH_BAD_STATE,
          "first-boot health report rejects uninitialized store");
    CHECK(first_boot_health_init(&store) == FIRST_BOOT_HEALTH_OK,
          "first-boot health report fixture initializes store");
    CHECK(first_boot_health_report(&store, &report) ==
              FIRST_BOOT_HEALTH_BAD_STATE,
          "first-boot health report rejects undefined store");

    CHECK(init_store(&store),
          "first-boot health report fixture defines store");
    CHECK(first_boot_health_report(&store, NULL) ==
              FIRST_BOOT_HEALTH_NULL,
          "first-boot health report rejects null report");
    report.version = 0U;
    CHECK(first_boot_health_report(&store, &report) ==
              FIRST_BOOT_HEALTH_BAD_REQUEST,
          "first-boot health report rejects version mismatch");
    report.version = FIRST_BOOT_HEALTH_VERSION;
    report.milestone.subsystem =
        (enum first_boot_health_subsystem)0U;
    CHECK(first_boot_health_report(&store, &report) ==
              FIRST_BOOT_HEALTH_BAD_SUBSYSTEM,
          "first-boot health report rejects invalid subsystem");
    report.milestone.subsystem = FIRST_BOOT_HEALTH_SUBSYSTEM_BOOT;
    report.milestone.milestone_id = 0U;
    CHECK(first_boot_health_report(&store, &report) ==
              FIRST_BOOT_HEALTH_BAD_MILESTONE,
          "first-boot health report rejects invalid milestone ID");
    report.milestone.milestone_id = 2U;
    CHECK(first_boot_health_report(&store, &report) ==
              FIRST_BOOT_HEALTH_UNKNOWN,
          "first-boot health report rejects unknown milestone");
    report.milestone.milestone_id = 1U;
    CHECK(first_boot_health_report(&store, &report) ==
              FIRST_BOOT_HEALTH_OK,
          "first-boot health report accepts exact required milestone");
    CHECK(first_boot_health_report(&store, &report) ==
              FIRST_BOOT_HEALTH_DUPLICATE,
          "first-boot health report rejects duplicate milestone");
    CHECK(store.observed_count == 1U && store.observed[0] &&
              store.healthy[0],
          "first-boot health duplicate leaves one exact observation");
    CHECK(first_boot_health_evaluate(&store, &verdict) ==
              FIRST_BOOT_HEALTH_NOT_READY &&
              !verdict.ready && verdict.missing_count == 3U,
          "first-boot health report fixture remains not ready");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "first-boot health report performs no allocation");
}

static void test_absent_milestone_prevents_acceptance(void)
{
    static const enum first_boot_health_subsystem subsystems[] = {
        FIRST_BOOT_HEALTH_SUBSYSTEM_BOOT,
        FIRST_BOOT_HEALTH_SUBSYSTEM_STORAGE,
        FIRST_BOOT_HEALTH_SUBSYSTEM_SERVICE,
        FIRST_BOOT_HEALTH_SUBSYSTEM_MANAGEMENT,
    };
    size_t absent_index;

    for (absent_index = 0U;
         absent_index < sizeof(subsystems) / sizeof(subsystems[0]);
         ++absent_index) {
        struct first_boot_health_store store;
        struct first_boot_health_verdict verdict;
        struct first_boot_health_milestone expected;
        size_t report_index;

        memset(&expected, 0, sizeof(expected));
        expected.subsystem = subsystems[absent_index];
        expected.milestone_id = 1U;
        CHECK(init_store(&store),
              "first-boot health absent-milestone fixture defines store");
        for (report_index = 0U;
             report_index < sizeof(subsystems) / sizeof(subsystems[0]);
             ++report_index) {
            if (report_index != absent_index) {
                CHECK(report_milestone(
                          &store,
                          subsystems[report_index],
                          1U,
                          true) == FIRST_BOOT_HEALTH_OK,
                      "absent-milestone fixture reports healthy subsystem");
            }
        }
        CHECK(first_boot_health_evaluate(&store, &verdict) ==
                  FIRST_BOOT_HEALTH_NOT_READY &&
                  !verdict.ready &&
                  !verdict.failed &&
                  verdict.required_count == 4U &&
                  verdict.observed_count == 3U &&
                  verdict.missing_count == 1U &&
                  milestone_equal(&verdict.first_missing, &expected),
              "absent boot/storage/service/management milestone prevents acceptance");
    }
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "absent-milestone evaluation performs no allocation");
}

static void test_complete_and_failed_contracts(void)
{
    struct first_boot_health_store store;
    struct first_boot_health_verdict verdict;
    size_t index;

    CHECK(init_store(&store),
          "first-boot health complete fixture defines store");
    for (index = 0U; index < 4U; ++index) {
        enum first_boot_health_subsystem subsystem =
            (enum first_boot_health_subsystem)(index + 1U);

        CHECK(report_milestone(
                  &store, subsystem, 1U, true) == FIRST_BOOT_HEALTH_OK,
              "complete fixture reports healthy subsystem");
    }
    CHECK(first_boot_health_evaluate(&store, &verdict) ==
              FIRST_BOOT_HEALTH_OK &&
              verdict.ready &&
              !verdict.failed &&
              verdict.required_count == 4U &&
              verdict.observed_count == 4U &&
              verdict.missing_count == 0U,
          "all healthy first-boot milestones permit acceptance");

    CHECK(init_store(&store),
          "first-boot health failed fixture defines store");
    CHECK(report_milestone(
              &store,
              FIRST_BOOT_HEALTH_SUBSYSTEM_BOOT,
              1U,
              true) == FIRST_BOOT_HEALTH_OK,
          "failed fixture reports healthy boot milestone");
    CHECK(report_milestone(
              &store,
              FIRST_BOOT_HEALTH_SUBSYSTEM_STORAGE,
              1U,
              false) == FIRST_BOOT_HEALTH_OK &&
              store.failed,
          "failed fixture records unhealthy storage milestone");
    CHECK(first_boot_health_evaluate(&store, &verdict) ==
              FIRST_BOOT_HEALTH_FAILED &&
              !verdict.ready &&
              verdict.failed &&
              verdict.observed_count == 2U,
          "unhealthy first-boot milestone prevents acceptance");
    CHECK(report_milestone(
              &store,
              FIRST_BOOT_HEALTH_SUBSYSTEM_SERVICE,
              1U,
              true) == FIRST_BOOT_HEALTH_FAILED,
          "failed first-boot store rejects later healthy report");
    CHECK(first_boot_health_evaluate(&store, &verdict) ==
              FIRST_BOOT_HEALTH_FAILED &&
              !verdict.ready && verdict.failed,
          "later healthy reports cannot reverse failed contract");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "complete and failed evaluation performs no allocation");
}

int main(void)
{
    test_status_strings();
    test_init_and_define();
    test_report_validation();
    test_absent_milestone_prevents_acceptance();
    test_complete_and_failed_contracts();

    if (failures != 0) {
        fprintf(stderr, "First-boot health host tests: FAILED\n");
        return 1;
    }
    printf("First-boot health host tests: ALL OK\n");
    return 0;
}

#else
#error "This host test must be built with HFOC_FIRST_BOOT_HEALTH_HOST_TEST"
#endif
