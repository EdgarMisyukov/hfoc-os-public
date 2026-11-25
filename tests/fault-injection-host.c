#include "fault_injection.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifdef HFOC_FAULT_INJECTION_HOST_TEST

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

static void test_catalogue_bounds(void)
{
    const struct fault_injection_site *site = NULL;

    alloc_attempts = 0U;
    free_attempts = 0U;

    CHECK(fault_injection_catalogue_count() ==
              FAULT_INJECTION_CATALOGUE_CAPACITY,
          "catalogue count equals declared capacity");
    CHECK(fault_injection_catalogue_at(
              FAULT_INJECTION_CATALOGUE_CAPACITY,
              &site) == FAULT_INJECTION_OUT_OF_RANGE &&
              site == NULL,
          "catalogue rejects out-of-range index");
    CHECK(fault_injection_catalogue_at(999U, &site) ==
              FAULT_INJECTION_OUT_OF_RANGE &&
              site == NULL,
          "catalogue rejects far out-of-range index");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "catalogue bounds perform no allocation");
}

static void test_catalogue_entries(void)
{
    size_t index;

    alloc_attempts = 0U;
    free_attempts = 0U;

    for (index = 0U;
         index < FAULT_INJECTION_CATALOGUE_CAPACITY;
         ++index) {
        const struct fault_injection_site *site = NULL;
        const struct fault_injection_site *lookup = NULL;
        enum fault_injection_id expected_id =
            (enum fault_injection_id)(index + 1U);

        CHECK(fault_injection_catalogue_at(index, &site) ==
                  FAULT_INJECTION_OK &&
                  site != NULL,
              "catalogue index returns valid site");
        if (site == NULL) {
            continue;
        }
        CHECK(site->id == expected_id,
              "catalogue ids ascend exactly from one");
        CHECK(site->name != NULL && site->name[0] != '\0',
              "catalogue site has non-empty name");
        CHECK(site->expected_outcome != NULL &&
                  site->expected_outcome[0] != '\0',
              "catalogue site has non-empty expected outcome");
        CHECK(site->subsystem != FAULT_INJECTION_SUBSYSTEM_UNKNOWN,
              "catalogue site subsystem is explicit");
        CHECK(site->bounded && site->test_only,
              "catalogue site is bounded and test-only");
        CHECK(fault_injection_site_by_id(site->id, &lookup) ==
                  FAULT_INJECTION_OK &&
                  lookup == site,
              "catalogue identifier lookup matches index entry");
        CHECK(strcmp(fault_injection_id_string(site->id),
                     site->name) == 0,
              "catalogue id string matches site name");
    }
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "catalogue lookups perform no allocation");
}

static void test_unknown_lookups(void)
{
    const struct fault_injection_site *site =
        (const struct fault_injection_site *)0x1000U;

    alloc_attempts = 0U;
    free_attempts = 0U;

    CHECK(fault_injection_site_by_id(
              (enum fault_injection_id)0U,
              &site) == FAULT_INJECTION_BAD_ID &&
              site == NULL,
          "catalogue rejects zero id without output");
    site = (const struct fault_injection_site *)0x1000U;
    CHECK(fault_injection_site_by_id(
              (enum fault_injection_id)
                  (FAULT_INJECTION_CATALOGUE_CAPACITY + 1U),
              &site) == FAULT_INJECTION_BAD_ID &&
              site == NULL,
          "catalogue rejects unknown id without output");
    CHECK(fault_injection_id_string((enum fault_injection_id)0U) == NULL,
          "undefined id string is rejected");
    CHECK(fault_injection_id_string(
              (enum fault_injection_id)999U) == NULL,
          "unknown id string is rejected");
    CHECK(fault_injection_subsystem_string(
              (enum fault_injection_subsystem)999U) == NULL,
          "undefined subsystem string is rejected");
    CHECK(fault_injection_status_string(
              (enum fault_injection_status)999U) == NULL,
          "undefined status string is rejected");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "unknown lookup paths perform no allocation");
}

static void test_strings(void)
{
    static const struct {
        enum fault_injection_subsystem subsystem;
        const char *string;
    } subsystems[] = {
        {FAULT_INJECTION_SUBSYSTEM_STORAGE,
         "FAULT_INJECTION_SUBSYSTEM_STORAGE"},
        {FAULT_INJECTION_SUBSYSTEM_SERVICE,
         "FAULT_INJECTION_SUBSYSTEM_SERVICE"},
        {FAULT_INJECTION_SUBSYSTEM_LOCAL_CONTROL,
         "FAULT_INJECTION_SUBSYSTEM_LOCAL_CONTROL"},
        {FAULT_INJECTION_SUBSYSTEM_NETWORK,
         "FAULT_INJECTION_SUBSYSTEM_NETWORK"},
        {FAULT_INJECTION_SUBSYSTEM_TRUST,
         "FAULT_INJECTION_SUBSYSTEM_TRUST"},
        {FAULT_INJECTION_SUBSYSTEM_UPDATE,
         "FAULT_INJECTION_SUBSYSTEM_UPDATE"},
        {FAULT_INJECTION_SUBSYSTEM_INTEGRATION,
         "FAULT_INJECTION_SUBSYSTEM_INTEGRATION"},
        {FAULT_INJECTION_SUBSYSTEM_SCHEDULER,
         "FAULT_INJECTION_SUBSYSTEM_SCHEDULER"},
    };
    static const struct {
        enum fault_injection_status status;
        const char *string;
    } statuses[] = {
        {FAULT_INJECTION_OK, "FAULT_INJECTION_OK"},
        {FAULT_INJECTION_NULL, "FAULT_INJECTION_NULL"},
        {FAULT_INJECTION_BAD_VERSION, "FAULT_INJECTION_BAD_VERSION"},
        {FAULT_INJECTION_BAD_ID, "FAULT_INJECTION_BAD_ID"},
        {FAULT_INJECTION_BAD_SEQUENCE,
         "FAULT_INJECTION_BAD_SEQUENCE"},
        {FAULT_INJECTION_NOT_PERMITTED,
         "FAULT_INJECTION_NOT_PERMITTED"},
        {FAULT_INJECTION_OUT_OF_RANGE,
         "FAULT_INJECTION_OUT_OF_RANGE"},
    };
    size_t index;

    alloc_attempts = 0U;
    free_attempts = 0U;

    for (index = 0U;
         index < sizeof(subsystems) / sizeof(subsystems[0]);
         ++index) {
        const char *string = fault_injection_subsystem_string(
            subsystems[index].subsystem);

        CHECK(string != NULL &&
                  strcmp(string, subsystems[index].string) == 0,
              "subsystem string is exact");
    }
    for (index = 0U;
         index < sizeof(statuses) / sizeof(statuses[0]);
         ++index) {
        const char *string = fault_injection_status_string(
            statuses[index].status);

        CHECK(string != NULL &&
                  strcmp(string, statuses[index].string) == 0,
              "status string is exact");
    }
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "string lookups perform no allocation");
}

static void test_activation_validation(void)
{
    struct fault_injection_activation activation;
    size_t index;

    alloc_attempts = 0U;
    free_attempts = 0U;

    CHECK(fault_injection_activation_validate(NULL) ==
              FAULT_INJECTION_NULL,
          "activation validator rejects null request");

    memset(&activation, 0, sizeof(activation));
    activation.version = FAULT_INJECTION_ACTIVATION_VERSION;
    activation.id = FAULT_INJECTION_BLOCK_DEVICE_READ;
    activation.sequence = 1U;
    CHECK(fault_injection_activation_validate(&activation) ==
              FAULT_INJECTION_OK,
          "activation validator accepts valid request");

    activation.version = 0U;
    CHECK(fault_injection_activation_validate(&activation) ==
              FAULT_INJECTION_BAD_VERSION,
          "activation validator rejects zero version");
    activation.version = FAULT_INJECTION_ACTIVATION_VERSION + 1U;
    CHECK(fault_injection_activation_validate(&activation) ==
              FAULT_INJECTION_BAD_VERSION,
          "activation validator rejects unsupported version");

    activation.version = FAULT_INJECTION_ACTIVATION_VERSION;
    activation.id = (enum fault_injection_id)0U;
    CHECK(fault_injection_activation_validate(&activation) ==
              FAULT_INJECTION_BAD_ID,
          "activation validator rejects zero id");
    activation.id = (enum fault_injection_id)999U;
    CHECK(fault_injection_activation_validate(&activation) ==
              FAULT_INJECTION_BAD_ID,
          "activation validator rejects unknown id");

    activation.id = FAULT_INJECTION_BLOCK_DEVICE_READ;
    activation.sequence = 0U;
    CHECK(fault_injection_activation_validate(&activation) ==
              FAULT_INJECTION_BAD_SEQUENCE,
          "activation validator rejects zero sequence");
    activation.sequence = FAULT_INJECTION_SEQUENCE_MAX + 1U;
    CHECK(fault_injection_activation_validate(&activation) ==
              FAULT_INJECTION_BAD_SEQUENCE,
          "activation validator rejects over-limit sequence");

    for (index = 0U;
         index < FAULT_INJECTION_CATALOGUE_CAPACITY;
         ++index) {
        activation.sequence = 1U;
        activation.id =
            (enum fault_injection_id)(index + 1U);
        CHECK(fault_injection_activation_validate(&activation) ==
                  FAULT_INJECTION_OK,
              "activation validator permits every catalogue id");
    }
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "activation validation performs no allocation");
}

int main(void)
{
    test_catalogue_bounds();
    test_catalogue_entries();
    test_unknown_lookups();
    test_strings();
    test_activation_validation();

    if (failures != 0) {
        fprintf(stderr,
                "Fault injection catalogue host tests: %d FAILURES\n",
                failures);
        return 1;
    }
    printf("Fault injection catalogue host tests: ALL OK\n");
    return 0;
}

#else
#error "fault-injection-host.c must be compiled with HFOC_FAULT_INJECTION_HOST_TEST"
#endif
