#include "service_restart_policy.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifdef HFOC_SERVICE_RESTART_POLICY_HOST_TEST

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

static void test_status_strings(void) {
    enum service_restart_policy_status status;

    for (status = SERVICE_RESTART_POLICY_OK;
         status <= SERVICE_RESTART_POLICY_TIME_OVERFLOW; ++status) {
        const char *string = service_restart_policy_status_string(status);

        CHECK(string != NULL &&
                  (status == SERVICE_RESTART_POLICY_OK ?
                       strcmp(string, "SERVICE_RESTART_POLICY_OK") == 0 :
                   status == SERVICE_RESTART_POLICY_NULL ?
                       strcmp(string, "SERVICE_RESTART_POLICY_NULL") == 0 :
                   status == SERVICE_RESTART_POLICY_BAD_STATE ?
                       strcmp(string,
                              "SERVICE_RESTART_POLICY_BAD_STATE") == 0 :
                   status == SERVICE_RESTART_POLICY_BAD_VERSION ?
                       strcmp(string,
                              "SERVICE_RESTART_POLICY_BAD_VERSION") == 0 :
                   status == SERVICE_RESTART_POLICY_BAD_BUDGET ?
                       strcmp(string,
                              "SERVICE_RESTART_POLICY_BAD_BUDGET") == 0 :
                   status == SERVICE_RESTART_POLICY_BAD_INTERVAL ?
                       strcmp(string,
                              "SERVICE_RESTART_POLICY_BAD_INTERVAL") == 0 :
                   status == SERVICE_RESTART_POLICY_INTERVAL ?
                       strcmp(string,
                              "SERVICE_RESTART_POLICY_INTERVAL") == 0 :
                   status == SERVICE_RESTART_POLICY_EXHAUSTED ?
                       strcmp(string,
                              "SERVICE_RESTART_POLICY_EXHAUSTED") == 0 :
                   status == SERVICE_RESTART_POLICY_DISABLED ?
                       strcmp(string,
                              "SERVICE_RESTART_POLICY_DISABLED") == 0 :
                       strcmp(string,
                              "SERVICE_RESTART_POLICY_TIME_OVERFLOW") == 0),
              "restart policy status string is exact for every status");
    }
    CHECK(service_restart_policy_status_string(
              (enum service_restart_policy_status)10U) == NULL,
          "restart policy undefined status is rejected");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "restart policy status handling performs no allocation");
}

static void test_state_strings(void) {
    enum service_restart_policy_state state;

    for (state = SERVICE_RESTART_POLICY_STATE_ACTIVE;
         state <= SERVICE_RESTART_POLICY_STATE_DISABLED; ++state) {
        const char *string = service_restart_policy_state_string(state);

        CHECK(string != NULL &&
                  (state == SERVICE_RESTART_POLICY_STATE_ACTIVE ?
                       strcmp(string,
                              "SERVICE_RESTART_POLICY_STATE_ACTIVE") == 0 :
                   state == SERVICE_RESTART_POLICY_STATE_EXHAUSTED ?
                       strcmp(string,
                              "SERVICE_RESTART_POLICY_STATE_EXHAUSTED") == 0 :
                       strcmp(string,
                              "SERVICE_RESTART_POLICY_STATE_DISABLED") == 0),
              "restart policy state string is exact for every state");
    }
    CHECK(service_restart_policy_state_string(
              (enum service_restart_policy_state)3U) == NULL,
          "restart policy undefined state is rejected");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "restart policy state handling performs no allocation");
}

static void test_input_and_validation_failures(void) {
    struct service_restart_policy policy;
    struct service_restart_policy unpublished;
    unsigned char before[sizeof(policy)];
    bool allowed = true;

    memset(&policy, 0, sizeof(policy));
    memset(&unpublished, 0, sizeof(unpublished));

    CHECK(service_restart_policy_validate(NULL) ==
              SERVICE_RESTART_POLICY_NULL,
          "restart policy validate rejects null policy");
    CHECK(service_restart_policy_init(NULL, 1U, 1U, false) ==
              SERVICE_RESTART_POLICY_NULL,
          "restart policy init rejects null output");
    CHECK(service_restart_policy_can_restart(NULL, 0U, &allowed) ==
              SERVICE_RESTART_POLICY_NULL,
          "restart policy can restart rejects null policy");
    CHECK(service_restart_policy_can_restart(&policy, 0U, NULL) ==
              SERVICE_RESTART_POLICY_NULL,
          "restart policy can restart rejects null allowed output");
    CHECK(service_restart_policy_record_restart(NULL, 0U) ==
              SERVICE_RESTART_POLICY_NULL,
          "restart policy record restart rejects null policy");
    CHECK(service_restart_policy_reset(NULL) ==
              SERVICE_RESTART_POLICY_NULL,
          "restart policy reset rejects null policy");
    CHECK(service_restart_policy_restart_count(NULL) == 0U,
          "restart policy count rejects null policy");
    CHECK(service_restart_policy_state(NULL) ==
              SERVICE_RESTART_POLICY_STATE_ACTIVE,
          "restart policy state rejects null policy");

    memset(&policy, 0xA5, sizeof(policy));
    memcpy(before, &policy, sizeof(before));
    CHECK(service_restart_policy_init(
              &policy, SERVICE_RESTART_BUDGET_MAX + 1U, 1U, false) ==
              SERVICE_RESTART_POLICY_BAD_BUDGET &&
              memcmp(&policy, before, sizeof(policy)) == 0,
          "over-limit restart budget is rejected without publication");

    memset(&policy, 0xA5, sizeof(policy));
    memcpy(before, &policy, sizeof(before));
    CHECK(service_restart_policy_init(
              &policy, 1U, 0U, false) ==
              SERVICE_RESTART_POLICY_BAD_INTERVAL &&
              memcmp(&policy, before, sizeof(policy)) == 0,
          "zero restart interval is rejected without publication");

    memset(&policy, 0xA5, sizeof(policy));
    memcpy(before, &policy, sizeof(before));
    CHECK(service_restart_policy_init(
              &policy, 1U, SERVICE_RESTART_INTERVAL_MAX_TICKS + 1U, false) ==
              SERVICE_RESTART_POLICY_BAD_INTERVAL &&
              memcmp(&policy, before, sizeof(policy)) == 0,
          "over-limit restart interval is rejected without publication");

    memset(&policy, 0, sizeof(policy));
    policy.version = SERVICE_RESTART_POLICY_VERSION + 1U;
    CHECK(service_restart_policy_validate(&policy) ==
              SERVICE_RESTART_POLICY_BAD_VERSION,
          "unknown policy version is rejected");

    allowed = true;
    CHECK(service_restart_policy_can_restart(
              &unpublished, 0U, &allowed) ==
              SERVICE_RESTART_POLICY_BAD_STATE && !allowed,
          "unpublished policy cannot restart");
    CHECK(service_restart_policy_record_restart(&unpublished, 0U) ==
              SERVICE_RESTART_POLICY_BAD_STATE,
          "unpublished policy cannot record restart");
    CHECK(service_restart_policy_reset(&unpublished) ==
              SERVICE_RESTART_POLICY_BAD_STATE,
          "unpublished policy cannot reset");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "restart policy input handling performs no allocation");
}

static void test_valid_init_and_boundaries(void) {
    struct service_restart_policy policy;

    CHECK(service_restart_policy_init(&policy, 3U, 5U, false) ==
              SERVICE_RESTART_POLICY_OK,
          "valid restart policy initializes");
    CHECK(policy.version == SERVICE_RESTART_POLICY_VERSION &&
              policy.restart_budget == 3U &&
              policy.restart_interval_ticks == 5U &&
              !policy.disabled_after_exhaustion &&
              policy.published &&
              policy.state == SERVICE_RESTART_POLICY_STATE_ACTIVE &&
              policy.restart_count == 0U &&
              policy.next_allowed_tick == 0U,
          "valid restart policy publishes exact initial state");
    CHECK(service_restart_policy_validate(&policy) ==
              SERVICE_RESTART_POLICY_OK,
          "published restart policy validates");

    CHECK(service_restart_policy_init(
              &policy, SERVICE_RESTART_BUDGET_MAX,
              SERVICE_RESTART_INTERVAL_MAX_TICKS, true) ==
              SERVICE_RESTART_POLICY_OK &&
              policy.restart_budget == SERVICE_RESTART_BUDGET_MAX &&
              policy.restart_interval_ticks ==
                  SERVICE_RESTART_INTERVAL_MAX_TICKS &&
              policy.disabled_after_exhaustion,
          "restart policy boundary values publish exactly");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "valid restart policy publication performs no allocation");
}

static void test_zero_budget_allows_no_restart(void) {
    struct service_restart_policy policy;
    bool allowed = true;

    CHECK(service_restart_policy_init(&policy, 0U, 1U, false) ==
              SERVICE_RESTART_POLICY_OK,
          "zero-budget restart policy initializes");
    CHECK(service_restart_policy_can_restart(&policy, 0U, &allowed) ==
              SERVICE_RESTART_POLICY_EXHAUSTED && !allowed,
          "zero-budget policy cannot restart");
    CHECK(service_restart_policy_record_restart(&policy, 0U) ==
              SERVICE_RESTART_POLICY_EXHAUSTED &&
              policy.restart_count == 0U &&
              policy.next_allowed_tick == 0U,
          "zero-budget policy records no restart");
    CHECK(service_restart_policy_restart_count(&policy) == 0U,
          "zero-budget policy publishes zero restart count");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "zero-budget handling performs no allocation");
}

static void test_interval_and_budget_bound_respawns(void) {
    struct service_restart_policy policy;
    bool allowed = true;

    CHECK(service_restart_policy_init(&policy, 2U, 5U, false) ==
              SERVICE_RESTART_POLICY_OK,
          "bounded respawn fixture initializes policy");
    CHECK(service_restart_policy_can_restart(&policy, 0U, &allowed) ==
              SERVICE_RESTART_POLICY_OK && allowed,
          "first restart is allowed at tick zero");
    CHECK(service_restart_policy_record_restart(&policy, 0U) ==
              SERVICE_RESTART_POLICY_OK &&
              policy.restart_count == 1U &&
              policy.next_allowed_tick == 5U &&
              policy.state == SERVICE_RESTART_POLICY_STATE_ACTIVE,
          "first restart commits count one and next allowed tick");

    allowed = true;
    CHECK(service_restart_policy_can_restart(&policy, 2U, &allowed) ==
              SERVICE_RESTART_POLICY_INTERVAL && !allowed,
          "restart before interval is not allowed");
    CHECK(service_restart_policy_record_restart(&policy, 2U) ==
              SERVICE_RESTART_POLICY_INTERVAL &&
              policy.restart_count == 1U &&
              policy.next_allowed_tick == 5U,
          "restart before interval consumes no budget");

    allowed = true;
    CHECK(service_restart_policy_can_restart(&policy, 5U, &allowed) ==
              SERVICE_RESTART_POLICY_OK && allowed,
          "second restart is allowed at interval boundary");
    CHECK(service_restart_policy_record_restart(&policy, 5U) ==
              SERVICE_RESTART_POLICY_OK &&
              policy.restart_count == 2U &&
              policy.next_allowed_tick == 10U &&
              policy.state == SERVICE_RESTART_POLICY_STATE_EXHAUSTED,
          "final budget restart publishes exhausted state");

    allowed = true;
    CHECK(service_restart_policy_can_restart(&policy, 100U, &allowed) ==
              SERVICE_RESTART_POLICY_EXHAUSTED && !allowed,
          "exhausted policy cannot restart later");
    CHECK(service_restart_policy_record_restart(&policy, 100U) ==
              SERVICE_RESTART_POLICY_EXHAUSTED &&
              policy.restart_count == 2U &&
              policy.next_allowed_tick == 10U,
          "exhausted policy never exceeds restart budget");
    CHECK(service_restart_policy_restart_count(&policy) == 2U,
          "exhausted policy reports exact restart count");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "bounded respawn handling performs no allocation");
}

static void test_disabled_after_exhaustion_is_permanent(void) {
    struct service_restart_policy policy;
    unsigned char before[sizeof(policy)];
    bool allowed = true;

    CHECK(service_restart_policy_init(&policy, 1U, 1U, true) ==
              SERVICE_RESTART_POLICY_OK,
          "disabled-after-exhaustion fixture initializes policy");
    CHECK(service_restart_policy_record_restart(&policy, 0U) ==
              SERVICE_RESTART_POLICY_OK &&
              policy.restart_count == 1U &&
              policy.state == SERVICE_RESTART_POLICY_STATE_DISABLED,
          "disabled-after-exhaustion publishes disabled state");
    allowed = true;
    CHECK(service_restart_policy_can_restart(&policy, 10U, &allowed) ==
              SERVICE_RESTART_POLICY_DISABLED && !allowed,
          "disabled policy cannot restart");
    CHECK(service_restart_policy_record_restart(&policy, 10U) ==
              SERVICE_RESTART_POLICY_DISABLED &&
              policy.restart_count == 1U,
          "disabled policy records no further restart");

    memcpy(before, &policy, sizeof(before));
    CHECK(service_restart_policy_reset(&policy) ==
              SERVICE_RESTART_POLICY_DISABLED &&
              memcmp(&policy, before, sizeof(policy)) == 0,
          "disabled-after-exhaustion rejects reset without mutation");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "disabled-after-exhaustion handling performs no allocation");
}

static void test_exhausted_reset_starts_new_generation(void) {
    struct service_restart_policy policy;

    CHECK(service_restart_policy_init(&policy, 1U, 1U, false) ==
              SERVICE_RESTART_POLICY_OK,
          "exhausted-reset fixture initializes policy");
    CHECK(service_restart_policy_record_restart(&policy, 0U) ==
              SERVICE_RESTART_POLICY_OK &&
              policy.state == SERVICE_RESTART_POLICY_STATE_EXHAUSTED,
          "final restart publishes exhausted state");
    CHECK(service_restart_policy_reset(&policy) ==
              SERVICE_RESTART_POLICY_OK &&
              policy.state == SERVICE_RESTART_POLICY_STATE_ACTIVE &&
              policy.restart_count == 0U &&
              policy.next_allowed_tick == 0U &&
              policy.published,
          "exhausted policy resets to a fresh active generation");
    CHECK(service_restart_policy_record_restart(&policy, 10U) ==
              SERVICE_RESTART_POLICY_OK &&
              policy.restart_count == 1U &&
              policy.next_allowed_tick == 11U,
          "reset generation can consume one fresh restart");

    CHECK(service_restart_policy_init(&policy, 1U, 1U, false) ==
              SERVICE_RESTART_POLICY_OK,
          "active reset fixture initializes policy");
    CHECK(service_restart_policy_reset(&policy) ==
              SERVICE_RESTART_POLICY_BAD_STATE &&
              policy.restart_count == 0U,
          "active policy reset is rejected without mutation");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "reset handling performs no allocation");
}

static void test_time_overflow_is_rejected_without_mutation(void) {
    struct service_restart_policy policy;
    bool allowed = true;

    CHECK(service_restart_policy_init(
              &policy, 1U, SERVICE_RESTART_INTERVAL_MAX_TICKS, false) ==
              SERVICE_RESTART_POLICY_OK,
          "time-overflow fixture initializes policy");
    CHECK(service_restart_policy_can_restart(
              &policy, UINT64_MAX, &allowed) ==
              SERVICE_RESTART_POLICY_TIME_OVERFLOW && !allowed,
          "next allowed tick overflow is rejected before commit");
    CHECK(service_restart_policy_record_restart(
              &policy, UINT64_MAX) ==
              SERVICE_RESTART_POLICY_TIME_OVERFLOW &&
              policy.restart_count == 0U &&
              policy.next_allowed_tick == 0U &&
              policy.state == SERVICE_RESTART_POLICY_STATE_ACTIVE,
          "time overflow records no restart");
    CHECK(service_restart_policy_record_restart(&policy, 1U) ==
              SERVICE_RESTART_POLICY_OK &&
              policy.restart_count == 1U &&
              policy.next_allowed_tick == UINT64_MAX,
          "maximum interval boundary commits without overflow");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "time overflow handling performs no allocation");
}

int main(void) {
    test_status_strings();
    test_state_strings();
    test_input_and_validation_failures();
    test_valid_init_and_boundaries();
    test_zero_budget_allows_no_restart();
    test_interval_and_budget_bound_respawns();
    test_disabled_after_exhaustion_is_permanent();
    test_exhausted_reset_starts_new_generation();
    test_time_overflow_is_rejected_without_mutation();

    if (failures != 0) {
        fprintf(stderr, "Restart policy host tests: %d FAILURES\n",
                failures);
        return 1;
    }
    puts("Restart policy host tests: ALL OK");
    return 0;
}

#endif
