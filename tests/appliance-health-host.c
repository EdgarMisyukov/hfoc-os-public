#include "appliance_health.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifdef APPLIANCE_HEALTH_HOST_TEST

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

static struct appliance_health_input make_input(void)
{
    struct appliance_health_input input;
    static const enum appliance_health_domain domains[
        APPLIANCE_HEALTH_DOMAIN_COUNT] = {
        APPLIANCE_HEALTH_DOMAIN_BOOT,
        APPLIANCE_HEALTH_DOMAIN_STORAGE,
        APPLIANCE_HEALTH_DOMAIN_CONFIGURATION,
        APPLIANCE_HEALTH_DOMAIN_NETWORK,
        APPLIANCE_HEALTH_DOMAIN_SERVICE,
        APPLIANCE_HEALTH_DOMAIN_HTTPS_MANAGEMENT,
    };
    uint32_t index;

    memset(&input, 0, sizeof(input));
    input.version = APPLIANCE_HEALTH_VERSION;
    input.generation = 42U;
    for (index = 0U;
         index < APPLIANCE_HEALTH_DOMAIN_COUNT;
         ++index) {
        input.domains[index].version = APPLIANCE_HEALTH_VERSION;
        input.domains[index].domain = domains[index];
        input.domains[index].state = APPLIANCE_HEALTH_STATE_PASS;
        input.domains[index].fault = APPLIANCE_HEALTH_FAULT_NONE;
        input.domains[index].last_checked_ticks =
            (uint64_t)index + 1U;
    }
    return input;
}

static bool output_ok(const struct appliance_health_output *output)
{
    return output->published &&
           output->version == APPLIANCE_HEALTH_VERSION &&
           output->state == APPLIANCE_HEALTH_STATE_OK &&
           output->healthy &&
           !output->critical &&
           output->first_fault_domain ==
               APPLIANCE_HEALTH_DOMAIN_NONE &&
           output->first_fault == APPLIANCE_HEALTH_FAULT_NONE;
}

static void check_verdict_failure(
    struct appliance_health_input *input,
    enum appliance_health_status expected,
    const char *message)
{
    struct appliance_health_output output;
    unsigned char before[sizeof(output)];

    memset(before, 0xA5, sizeof(before));
    memcpy(&output, before, sizeof(output));
    CHECK(appliance_health_evaluate(input, &output) == expected &&
              memcmp(&output, before, sizeof(output)) == 0,
          message);
}

static void test_strings(void)
{
    static const struct {
        enum appliance_health_status status;
        const char *string;
    } statuses[] = {
        {APPLIANCE_HEALTH_STATUS_OK, "APPLIANCE_HEALTH_STATUS_OK"},
        {APPLIANCE_HEALTH_STATUS_NULL, "APPLIANCE_HEALTH_STATUS_NULL"},
        {APPLIANCE_HEALTH_STATUS_BAD_VERSION,
         "APPLIANCE_HEALTH_STATUS_BAD_VERSION"},
        {APPLIANCE_HEALTH_STATUS_BAD_STATE,
         "APPLIANCE_HEALTH_STATUS_BAD_STATE"},
        {APPLIANCE_HEALTH_STATUS_BAD_DOMAIN,
         "APPLIANCE_HEALTH_STATUS_BAD_DOMAIN"},
        {APPLIANCE_HEALTH_STATUS_BAD_FAULT,
         "APPLIANCE_HEALTH_STATUS_BAD_FAULT"},
    };
    static const struct {
        enum appliance_health_state state;
        const char *string;
    } states[] = {
        {APPLIANCE_HEALTH_STATE_UNKNOWN, "UNKNOWN"},
        {APPLIANCE_HEALTH_STATE_OK, "OK"},
        {APPLIANCE_HEALTH_STATE_DEGRADED, "DEGRADED"},
        {APPLIANCE_HEALTH_STATE_FAILED, "FAILED"},
    };
    static const struct {
        enum appliance_health_domain domain;
        const char *string;
    } domains[] = {
        {APPLIANCE_HEALTH_DOMAIN_NONE, "NONE"},
        {APPLIANCE_HEALTH_DOMAIN_BOOT, "BOOT"},
        {APPLIANCE_HEALTH_DOMAIN_STORAGE, "STORAGE"},
        {APPLIANCE_HEALTH_DOMAIN_CONFIGURATION, "CONFIGURATION"},
        {APPLIANCE_HEALTH_DOMAIN_NETWORK, "NETWORK"},
        {APPLIANCE_HEALTH_DOMAIN_SERVICE, "SERVICE"},
        {APPLIANCE_HEALTH_DOMAIN_HTTPS_MANAGEMENT,
         "HTTPS_MANAGEMENT"},
    };
    static const struct {
        enum appliance_health_fault fault;
        const char *string;
    } faults[] = {
        {APPLIANCE_HEALTH_FAULT_NONE, "NONE"},
        {APPLIANCE_HEALTH_BOOT_FAULT_INCOMPLETE, "INCOMPLETE"},
        {APPLIANCE_HEALTH_BOOT_FAULT_PANIC, "PANIC"},
        {APPLIANCE_HEALTH_STORAGE_FAULT_JOURNAL, "JOURNAL"},
        {APPLIANCE_HEALTH_STORAGE_FAULT_CHECKPOINT, "CHECKPOINT"},
        {APPLIANCE_HEALTH_STORAGE_FAULT_BACKUP, "BACKUP"},
        {APPLIANCE_HEALTH_CONFIG_FAULT_MISSING, "MISSING"},
        {APPLIANCE_HEALTH_CONFIG_FAULT_INVALID, "INVALID"},
        {APPLIANCE_HEALTH_CONFIG_FAULT_COMMIT, "COMMIT"},
        {APPLIANCE_HEALTH_NETWORK_FAULT_LINK, "LINK"},
        {APPLIANCE_HEALTH_NETWORK_FAULT_ADDRESS, "ADDRESS"},
        {APPLIANCE_HEALTH_NETWORK_FAULT_REACHABILITY,
         "REACHABILITY"},
        {APPLIANCE_HEALTH_SERVICE_FAULT_NOT_READY, "NOT_READY"},
        {APPLIANCE_HEALTH_SERVICE_FAULT_STOPPED, "STOPPED"},
        {APPLIANCE_HEALTH_SERVICE_FAULT_FAILED, "FAILED"},
        {APPLIANCE_HEALTH_HTTPS_FAULT_LISTENER, "LISTENER"},
        {APPLIANCE_HEALTH_HTTPS_FAULT_AUTH, "AUTH"},
        {APPLIANCE_HEALTH_HTTPS_FAULT_UNAVAILABLE,
         "UNAVAILABLE"},
    };
    size_t index;

    for (index = 0U;
         index < sizeof(statuses) / sizeof(statuses[0]);
         ++index) {
        const char *string = appliance_health_status_string(
            statuses[index].status);

        CHECK(string != NULL &&
                  strcmp(string, statuses[index].string) == 0,
              "appliance health status string is exact");
    }
    for (index = 0U;
         index < sizeof(states) / sizeof(states[0]);
         ++index) {
        const char *string = appliance_health_state_string(
            states[index].state);

        CHECK(string != NULL &&
                  strcmp(string, states[index].string) == 0,
              "appliance health state string is exact");
    }
    for (index = 0U;
         index < sizeof(domains) / sizeof(domains[0]);
         ++index) {
        const char *string = appliance_health_domain_string(
            domains[index].domain);

        CHECK(string != NULL &&
                  strcmp(string, domains[index].string) == 0,
              "appliance health domain string is exact");
    }
    for (index = 0U;
         index < sizeof(faults) / sizeof(faults[0]);
         ++index) {
        const char *string = appliance_health_fault_string(
            faults[index].fault);

        CHECK(string != NULL &&
                  strcmp(string, faults[index].string) == 0,
              "appliance health fault string is exact");
    }
    CHECK(appliance_health_status_string(
              (enum appliance_health_status)99U) == NULL,
          "appliance health rejects undefined status string");
    CHECK(appliance_health_state_string(
              (enum appliance_health_state)9U) == NULL,
          "appliance health rejects undefined state string");
    CHECK(appliance_health_domain_string(
              (enum appliance_health_domain)9U) == NULL,
          "appliance health rejects undefined domain string");
    CHECK(appliance_health_fault_string(
              (enum appliance_health_fault)999U) == NULL,
          "appliance health rejects undefined fault string");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "appliance health strings perform no allocation");
}

static void test_null_and_bad_inputs_never_publish(void)
{
    struct appliance_health_input input = make_input();
    struct appliance_health_output output;
    unsigned char before[sizeof(output)];

    memset(before, 0xA5, sizeof(before));
    memcpy(&output, before, sizeof(output));
    CHECK(appliance_health_evaluate(NULL, &output) ==
              APPLIANCE_HEALTH_STATUS_NULL,
          "appliance health evaluate rejects null input");
    memset(&output, 0, sizeof(output));
    CHECK(appliance_health_evaluate(&input, NULL) ==
              APPLIANCE_HEALTH_STATUS_NULL,
          "appliance health evaluate rejects null output");
    CHECK(memcmp(&output, &(struct appliance_health_output){0},
                 sizeof(output)) == 0,
          "appliance health null-output rejection leaves output zero");

    input.version = APPLIANCE_HEALTH_VERSION - 1U;
    check_verdict_failure(&input, APPLIANCE_HEALTH_STATUS_BAD_VERSION,
                          "appliance health rejects bad input version");
    input = make_input();
    input.domains[0].version = APPLIANCE_HEALTH_VERSION - 1U;
    check_verdict_failure(&input, APPLIANCE_HEALTH_STATUS_BAD_VERSION,
                          "appliance health rejects bad verdict version");
    input = make_input();
    input.domains[0].domain = APPLIANCE_HEALTH_DOMAIN_NONE;
    check_verdict_failure(&input, APPLIANCE_HEALTH_STATUS_BAD_DOMAIN,
                          "appliance health rejects missing first domain");
    input = make_input();
    input.domains[1].domain = APPLIANCE_HEALTH_DOMAIN_BOOT;
    check_verdict_failure(&input, APPLIANCE_HEALTH_STATUS_BAD_DOMAIN,
                          "appliance health rejects duplicate domain");
    input = make_input();
    input.domains[0].state = (enum appliance_health_state)9U;
    check_verdict_failure(&input, APPLIANCE_HEALTH_STATUS_BAD_STATE,
                          "appliance health rejects undefined state");
    input = make_input();
    input.domains[0].state = APPLIANCE_HEALTH_STATE_PASS;
    input.domains[0].fault = APPLIANCE_HEALTH_BOOT_FAULT_PANIC;
    check_verdict_failure(&input, APPLIANCE_HEALTH_STATUS_BAD_FAULT,
                          "appliance health rejects PASS with fault");
    input = make_input();
    input.domains[0].state = APPLIANCE_HEALTH_STATE_FAIL;
    input.domains[0].fault = APPLIANCE_HEALTH_FAULT_NONE;
    check_verdict_failure(&input, APPLIANCE_HEALTH_STATUS_BAD_FAULT,
                          "appliance health rejects FAIL without fault");
    input = make_input();
    input.domains[4].state = APPLIANCE_HEALTH_STATE_WARN;
    input.domains[4].fault = APPLIANCE_HEALTH_BOOT_FAULT_PANIC;
    check_verdict_failure(&input, APPLIANCE_HEALTH_STATUS_BAD_FAULT,
                          "appliance health rejects domain/kind mismatch");
}

static void test_aggregate_ok_unknown_degraded_failed(void)
{
    struct appliance_health_input input = make_input();
    struct appliance_health_output output;
    struct appliance_health_input unknown = make_input();
    struct appliance_health_input degraded = make_input();
    struct appliance_health_input failed = make_input();

    memset(&output, 0xA5, sizeof(output));
    CHECK(appliance_health_evaluate(&input, &output) ==
              APPLIANCE_HEALTH_STATUS_OK &&
              output_ok(&output),
          "appliance health aggregate is OK when all domains pass");

    unknown.domains[0].state = APPLIANCE_HEALTH_STATE_UNKNOWN;
    unknown.domains[0].fault = APPLIANCE_HEALTH_FAULT_NONE;
    memset(&output, 0, sizeof(output));
    CHECK(appliance_health_evaluate(&unknown, &output) ==
              APPLIANCE_HEALTH_STATUS_OK &&
              output.published &&
              output.state == APPLIANCE_HEALTH_STATE_UNKNOWN &&
              !output.healthy &&
              !output.critical &&
              output.first_fault_domain ==
                  APPLIANCE_HEALTH_DOMAIN_BOOT &&
              output.first_fault == APPLIANCE_HEALTH_FAULT_NONE,
          "appliance health aggregate is UNKNOWN when any domain unknown");

    degraded.domains[4].state = APPLIANCE_HEALTH_STATE_WARN;
    degraded.domains[4].fault =
        APPLIANCE_HEALTH_SERVICE_FAULT_NOT_READY;
    memset(&output, 0, sizeof(output));
    CHECK(appliance_health_evaluate(&degraded, &output) ==
              APPLIANCE_HEALTH_STATUS_OK &&
              output.published &&
              output.state == APPLIANCE_HEALTH_STATE_DEGRADED &&
              !output.healthy &&
              !output.critical &&
              output.first_fault_domain ==
                  APPLIANCE_HEALTH_DOMAIN_SERVICE &&
              output.first_fault ==
                  APPLIANCE_HEALTH_SERVICE_FAULT_NOT_READY,
          "appliance health aggregate is DEGRADED on first warn");

    failed.domains[2].state = APPLIANCE_HEALTH_STATE_FAIL;
    failed.domains[2].fault =
        APPLIANCE_HEALTH_CONFIG_FAULT_INVALID;
    memset(&output, 0, sizeof(output));
    CHECK(appliance_health_evaluate(&failed, &output) ==
              APPLIANCE_HEALTH_STATUS_OK &&
              output.published &&
              output.state == APPLIANCE_HEALTH_STATE_FAILED &&
              !output.healthy &&
              !output.critical &&
              output.first_fault_domain ==
                  APPLIANCE_HEALTH_DOMAIN_CONFIGURATION &&
              output.first_fault ==
                  APPLIANCE_HEALTH_CONFIG_FAULT_INVALID,
          "appliance health aggregate is FAILED on fail");
}

static void test_critical_and_first_fault_order(void)
{
    struct appliance_health_input warn_then_fail = make_input();
    struct appliance_health_input fail_then_warn = make_input();
    struct appliance_health_input critical = make_input();
    struct appliance_health_output output;

    warn_then_fail.domains[0].state = APPLIANCE_HEALTH_STATE_WARN;
    warn_then_fail.domains[0].fault = APPLIANCE_HEALTH_BOOT_FAULT_INCOMPLETE;
    warn_then_fail.domains[1].state = APPLIANCE_HEALTH_STATE_FAIL;
    warn_then_fail.domains[1].fault = APPLIANCE_HEALTH_STORAGE_FAULT_JOURNAL;
    memset(&output, 0, sizeof(output));
    CHECK(appliance_health_evaluate(&warn_then_fail, &output) ==
              APPLIANCE_HEALTH_STATUS_OK &&
              output.state == APPLIANCE_HEALTH_STATE_FAILED &&
              output.first_fault_domain == APPLIANCE_HEALTH_DOMAIN_BOOT &&
              output.first_fault == APPLIANCE_HEALTH_BOOT_FAULT_INCOMPLETE,
          "appliance health reports earlier warn as first fault");

    fail_then_warn = make_input();
    fail_then_warn.domains[4].state = APPLIANCE_HEALTH_STATE_FAIL;
    fail_then_warn.domains[4].fault = APPLIANCE_HEALTH_SERVICE_FAULT_FAILED;
    fail_then_warn.domains[5].state = APPLIANCE_HEALTH_STATE_WARN;
    fail_then_warn.domains[5].fault =
        APPLIANCE_HEALTH_HTTPS_FAULT_LISTENER;
    memset(&output, 0, sizeof(output));
    CHECK(appliance_health_evaluate(&fail_then_warn, &output) ==
              APPLIANCE_HEALTH_STATUS_OK &&
              output.state == APPLIANCE_HEALTH_STATE_FAILED &&
              output.first_fault_domain == APPLIANCE_HEALTH_DOMAIN_SERVICE &&
              output.first_fault == APPLIANCE_HEALTH_SERVICE_FAULT_FAILED,
          "appliance health prefers earlier fail over later warn");

    critical.domains[1].state = APPLIANCE_HEALTH_STATE_WARN;
    critical.domains[1].fault = APPLIANCE_HEALTH_STORAGE_FAULT_BACKUP;
    critical.critical = true;
    memset(&output, 0, sizeof(output));
    CHECK(appliance_health_evaluate(&critical, &output) ==
              APPLIANCE_HEALTH_STATUS_OK &&
              output.state == APPLIANCE_HEALTH_STATE_FAILED &&
              output.healthy == false &&
              output.critical &&
              output.first_fault_domain == APPLIANCE_HEALTH_DOMAIN_STORAGE &&
              output.first_fault == APPLIANCE_HEALTH_STORAGE_FAULT_BACKUP,
          "appliance health reports critical fail and retains first fault");
}

static void test_generation_and_tick_passthrough_not_required(void)
{
    struct appliance_health_input input = make_input();
    struct appliance_health_output output;

    input.generation = UINT64_C(0);
    memset(&output, 0, sizeof(output));
    CHECK(appliance_health_evaluate(&input, &output) ==
              APPLIANCE_HEALTH_STATUS_OK &&
              output_ok(&output),
          "appliance health accepts zero generation as caller metadata");
}

int main(void)
{
    test_strings();
    test_null_and_bad_inputs_never_publish();
    test_aggregate_ok_unknown_degraded_failed();
    test_critical_and_first_fault_order();
    test_generation_and_tick_passthrough_not_required();

    if (failures != 0) {
        fprintf(stderr, "%d appliance health host test failures\n",
                failures);
        return 1;
    }
    printf("Appliance health host tests: ALL OK\n");
    return 0;
}

#endif
