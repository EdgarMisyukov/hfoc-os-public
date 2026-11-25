/*
 * ITEM-508 increment 9: host proof of the renewal decision.
 *
 * The proof owns no clock and no network: it fixes instants and expiries and
 * pins the policy - nothing starts on an unvalidated clock, an appliance
 * without a certificate has nothing to renew, renewal begins inside the
 * thirty-day window and not a second earlier, an expired certificate is due,
 * a failed attempt waits out its six-hour window, the clock that moved
 * backwards is not a validated clock, the attempt budget is bounded and the
 * operator's reset reopens it, a verified certificate clears the counter and
 * moves the expiry, and no record ever carries material.
 */

#include "appliance_acme_renewal.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

static int failures;

#define CHECK(condition, ...) do { \
    if (!(condition)) { \
        fprintf(stderr, "FAIL: "); \
        fprintf(stderr, __VA_ARGS__); \
        fprintf(stderr, "\n"); \
        ++failures; \
    } else { \
        printf("PASS: "); \
        printf(__VA_ARGS__); \
        printf("\n"); \
    } \
} while (0)

#define WINDOW APPLIANCE_ACME_RENEWAL_WINDOW_SECONDS
#define RETRY APPLIANCE_ACME_RENEWAL_RETRY_SECONDS
#define ATTEMPT_MAX APPLIANCE_ACME_RENEWAL_ATTEMPT_MAX

static bool record_names(const struct appliance_acme_renewal *renewal,
                         const char *fact)
{
    return strncmp(appliance_acme_renewal_record(renewal),
                   "[ACME-RENEWAL] ", 15U) == 0 &&
           strstr(appliance_acme_renewal_record(renewal), fact) != NULL;
}

static void test_the_empty_state(void)
{
    struct appliance_acme_renewal renewal;
    struct appliance_acme_renewal uninitialized;

    memset(&uninitialized, 0, sizeof(uninitialized));
    appliance_acme_renewal_init(&renewal);

    CHECK(renewal.version == APPLIANCE_ACME_RENEWAL_VERSION,
          "the state carries the published version");
    CHECK(renewal.initialized,
          "an initialized state says so");
    CHECK(appliance_acme_renewal_expires_at(&renewal) == 0U,
          "an appliance without a certificate has no expiry");
    CHECK(appliance_acme_renewal_attempts(&renewal) == 0U,
          "an untouched state has no attempts");
    CHECK(appliance_acme_renewal_last_outcome(&renewal) ==
              APPLIANCE_ACME_RENEWAL_OUTCOME_NONE,
          "an untouched state has no outcome");
    CHECK(appliance_acme_renewal_last_decision(&renewal) ==
              APPLIANCE_ACME_RENEWAL_DECISION_NONE,
          "an untouched state has no decision");
    CHECK(appliance_acme_renewal_next_attempt_epoch_seconds(&renewal) == 0U,
          "an untouched state waits for no attempt");
    CHECK(strcmp(appliance_acme_renewal_record(&renewal), "") == 0,
          "an untouched state publishes no record");

    /* A zeroed structure is not an initialized one: it decides nothing. */
    CHECK(appliance_acme_renewal_evaluate(&uninitialized, 1000U) ==
              APPLIANCE_ACME_RENEWAL_DECISION_NONE,
          "a state that was never initialized decides nothing");
    CHECK(appliance_acme_renewal_attempt_started(&uninitialized, 1000U) ==
              APPLIANCE_ACME_RENEWAL_BAD_STATE,
          "an attempt cannot start on an uninitialized state");
    CHECK(appliance_acme_renewal_certificate_obtained(
              &uninitialized, 1000U, 2000U) ==
              APPLIANCE_ACME_RENEWAL_BAD_STATE,
          "a certificate cannot be recorded on an uninitialized state");

    CHECK(appliance_acme_renewal_evaluate(NULL, 1000U) ==
              APPLIANCE_ACME_RENEWAL_DECISION_NONE,
          "a NULL state decides nothing");
    CHECK(appliance_acme_renewal_attempt_started(NULL, 1000U) ==
              APPLIANCE_ACME_RENEWAL_NULL,
          "a NULL state refuses to start an attempt");
    CHECK(appliance_acme_renewal_attempt_failed(NULL, 1000U) ==
              APPLIANCE_ACME_RENEWAL_NULL,
          "a NULL state refuses a failure");
    CHECK(appliance_acme_renewal_certificate_obtained(NULL, 1000U, 2000U) ==
              APPLIANCE_ACME_RENEWAL_NULL,
          "a NULL state refuses a certificate");
    CHECK(appliance_acme_renewal_attempts_reset(NULL) ==
              APPLIANCE_ACME_RENEWAL_NULL,
          "a NULL state refuses the operator's reset");
    CHECK(appliance_acme_renewal_expires_at(NULL) == 0U &&
              appliance_acme_renewal_attempts(NULL) == 0U &&
              appliance_acme_renewal_last_attempt_epoch_seconds(NULL) == 0U &&
              appliance_acme_renewal_next_attempt_epoch_seconds(NULL) == 0U,
          "every reader of a NULL state answers zero");
    CHECK(strcmp(appliance_acme_renewal_record(NULL), "") == 0,
          "a NULL state publishes no record");

    /* The reset returns an emptied state, not a broken one. */
    appliance_acme_renewal_reset(&renewal);
    CHECK(renewal.initialized &&
              appliance_acme_renewal_expires_at(&renewal) == 0U &&
              appliance_acme_renewal_attempts(&renewal) == 0U,
          "the reset returns the empty state");
}

static void test_the_clock(void)
{
    struct appliance_acme_renewal renewal;

    appliance_acme_renewal_init(&renewal);
    CHECK(appliance_acme_renewal_evaluate(&renewal, 0U) ==
              APPLIANCE_ACME_RENEWAL_DECISION_BAD_CLOCK,
          "an unvalidated clock refuses the decision");
    CHECK(record_names(&renewal, "decision=bad-clock"),
          "the refusal is published by name");
    CHECK(renewal.evaluation_count == 1U,
          "the refused decision is still counted");
    CHECK(appliance_acme_renewal_attempt_started(&renewal, 0U) ==
              APPLIANCE_ACME_RENEWAL_BAD_CLOCK,
          "no attempt starts on an unvalidated clock");
    CHECK(appliance_acme_renewal_attempts(&renewal) == 0U,
          "the refused attempt counted nothing");
    CHECK(appliance_acme_renewal_certificate_obtained(&renewal, 0U, 1000U) ==
              APPLIANCE_ACME_RENEWAL_BAD_CLOCK,
          "no certificate is recorded at an unvalidated instant");
    CHECK(appliance_acme_renewal_expires_at(&renewal) == 0U,
          "the refused certificate moved no expiry");

    /* With a certificate in hand, the zero clock is still refused first. */
    CHECK(appliance_acme_renewal_certificate_obtained(
              &renewal, 1000U, 1000U + RETRY) ==
              APPLIANCE_ACME_RENEWAL_OK,
          "a certificate that outlives its instant is accepted");
    CHECK(appliance_acme_renewal_evaluate(&renewal, 0U) ==
              APPLIANCE_ACME_RENEWAL_DECISION_BAD_CLOCK,
          "the zero clock is refused before the window is computed");
}

static void test_no_certificate(void)
{
    struct appliance_acme_renewal renewal;

    appliance_acme_renewal_init(&renewal);
    CHECK(appliance_acme_renewal_evaluate(&renewal, 1000U) ==
              APPLIANCE_ACME_RENEWAL_DECISION_NO_CERTIFICATE,
          "an appliance without a certificate has nothing to renew");
    CHECK(record_names(&renewal, "decision=no-certificate"),
          "the enrolment decision is published by name");
    CHECK(appliance_acme_renewal_attempt_started(&renewal, 1000U) ==
              APPLIANCE_ACME_RENEWAL_NO_CERTIFICATE,
          "a renewal attempt is not an enrolment");
    CHECK(appliance_acme_renewal_attempts(&renewal) == 0U,
          "the refused enrolment attempt counted nothing");
}

static void test_certificate_obtained(void)
{
    struct appliance_acme_renewal renewal;
    const uint32_t now = 1000000U;

    appliance_acme_renewal_init(&renewal);
    CHECK(appliance_acme_renewal_certificate_obtained(&renewal, now, now) ==
              APPLIANCE_ACME_RENEWAL_BAD_VALUE,
          "an expiry that does not outlive the instant is refused");
    CHECK(appliance_acme_renewal_certificate_obtained(&renewal, now, 0U) ==
              APPLIANCE_ACME_RENEWAL_BAD_VALUE,
          "a zero expiry is refused");
    CHECK(appliance_acme_renewal_expires_at(&renewal) == 0U,
          "both refusals moved no expiry");

    CHECK(appliance_acme_renewal_certificate_obtained(
              &renewal, now, now + WINDOW) == APPLIANCE_ACME_RENEWAL_OK,
          "a certificate that outlives the instant is accepted");
    CHECK(appliance_acme_renewal_expires_at(&renewal) == now + WINDOW,
          "the accepted expiry is the one handed in");
    CHECK(appliance_acme_renewal_attempts(&renewal) == 0U,
          "a verified certificate clears the attempt counter");
    CHECK(appliance_acme_renewal_last_outcome(&renewal) ==
              APPLIANCE_ACME_RENEWAL_OUTCOME_SUCCEEDED,
          "the success is recorded");
    CHECK(appliance_acme_renewal_last_attempt_epoch_seconds(&renewal) == now,
          "the success records when it happened");
    CHECK(appliance_acme_renewal_last_decision(&renewal) ==
              APPLIANCE_ACME_RENEWAL_DECISION_NONE,
          "a success is not a decision: only evaluate decides");
    CHECK(record_names(&renewal, "attempt=succeeded"),
          "the success is published by name");
    CHECK(record_names(&renewal, "attempts=0"),
          "the record carries no attempt");
    CHECK(appliance_acme_renewal_evaluate(&renewal, now) ==
              APPLIANCE_ACME_RENEWAL_DECISION_DUE,
          "the decision about the new certificate is computed, not assumed");

    /* A renewal is the same event as a first issuance. */
    CHECK(appliance_acme_renewal_certificate_obtained(
              &renewal, now + 1U, now + 2U * WINDOW) ==
              APPLIANCE_ACME_RENEWAL_OK,
          "a renewed certificate replaces the expiry");
    CHECK(appliance_acme_renewal_expires_at(&renewal) == now + 2U * WINDOW,
          "the newer expiry is the one that stands");
}

static void test_the_window(void)
{
    struct appliance_acme_renewal renewal;
    const uint32_t now = 2000000U;
    const uint32_t expires = now + 2U * WINDOW;

    appliance_acme_renewal_init(&renewal);
    CHECK(appliance_acme_renewal_certificate_obtained(
              &renewal, now, expires) == APPLIANCE_ACME_RENEWAL_OK,
          "the fixture certificate is in place");

    CHECK(appliance_acme_renewal_evaluate(&renewal, now) ==
              APPLIANCE_ACME_RENEWAL_DECISION_NOT_NEEDED,
          "a fresh certificate is not renewed");
    CHECK(appliance_acme_renewal_attempt_started(&renewal, now) ==
              APPLIANCE_ACME_RENEWAL_NOT_DUE,
          "an attempt does not start outside the window");
    CHECK(appliance_acme_renewal_attempts(&renewal) == 0U,
          "the refused attempt counted nothing");
    CHECK(appliance_acme_renewal_next_attempt_epoch_seconds(&renewal) == 0U,
          "a decision that is not a retry waits for nothing");

    /* One second outside the window is still outside it. */
    CHECK(appliance_acme_renewal_evaluate(
              &renewal, expires - WINDOW - 1U) ==
              APPLIANCE_ACME_RENEWAL_DECISION_NOT_NEEDED,
          "one second before the window is not due");
    CHECK(appliance_acme_renewal_evaluate(&renewal, expires - WINDOW) ==
              APPLIANCE_ACME_RENEWAL_DECISION_DUE,
          "the first second inside the window is due");
    CHECK(record_names(&renewal, "decision=due"),
          "the due decision is published by name");
    CHECK(appliance_acme_renewal_evaluate(&renewal, expires - 1U) ==
              APPLIANCE_ACME_RENEWAL_DECISION_DUE,
          "the last second before expiry is due");
    CHECK(appliance_acme_renewal_evaluate(&renewal, expires) ==
              APPLIANCE_ACME_RENEWAL_DECISION_DUE,
          "an expired certificate is due");
    CHECK(appliance_acme_renewal_evaluate(&renewal, expires + 1000U) ==
              APPLIANCE_ACME_RENEWAL_DECISION_DUE,
          "a long-expired certificate is due");
    CHECK(renewal.evaluation_count == 7U,
          "every decision is counted, including the attempt's own");
}

static void test_attempts_and_retry(void)
{
    struct appliance_acme_renewal renewal;
    const uint32_t now = 3000000U;
    const uint32_t expires = now + WINDOW;

    appliance_acme_renewal_init(&renewal);
    CHECK(appliance_acme_renewal_certificate_obtained(
              &renewal, now, expires) == APPLIANCE_ACME_RENEWAL_OK,
          "the fixture certificate is in place");
    CHECK(appliance_acme_renewal_attempt_failed(&renewal, now + 1U) ==
              APPLIANCE_ACME_RENEWAL_NOT_ATTEMPTED,
          "a failure without an attempt is refused");
    CHECK(appliance_acme_renewal_attempts_reset(&renewal) ==
              APPLIANCE_ACME_RENEWAL_OK,
          "the operator's reset is accepted");
    CHECK(record_names(&renewal, "attempts=reset") &&
              appliance_acme_renewal_expires_at(&renewal) == expires,
          "the reset keeps the certificate and publishes itself");

    /* Due, started and failed: the retry window runs from the failure. */
    const uint32_t due_at = expires - WINDOW;
    CHECK(appliance_acme_renewal_attempt_started(&renewal, due_at) ==
              APPLIANCE_ACME_RENEWAL_OK,
          "an attempt starts on a due certificate");
    CHECK(appliance_acme_renewal_attempts(&renewal) == 1U,
          "the started attempt is counted");
    CHECK(appliance_acme_renewal_last_attempt_epoch_seconds(&renewal) == due_at,
          "the attempt records when it started");
    CHECK(appliance_acme_renewal_last_outcome(&renewal) ==
              APPLIANCE_ACME_RENEWAL_OUTCOME_NONE,
          "an attempt in flight has no outcome");
    CHECK(record_names(&renewal, "attempt=started"),
          "the started attempt is published by name");

    CHECK(appliance_acme_renewal_attempt_started(&renewal, 0U) ==
              APPLIANCE_ACME_RENEWAL_BAD_CLOCK,
          "the zero clock keeps its precedence while an attempt is in flight");
    CHECK(appliance_acme_renewal_attempts(&renewal) == 1U,
          "the clock refusal counted nothing");

    CHECK(appliance_acme_renewal_attempt_started(&renewal, due_at + 1U) ==
              APPLIANCE_ACME_RENEWAL_NOT_DUE,
          "a second attempt does not start while the first is in flight");
    CHECK(appliance_acme_renewal_attempts(&renewal) == 1U,
          "the refused second attempt counted nothing");

    const uint32_t failed_at = due_at + 100U;
    CHECK(appliance_acme_renewal_attempt_failed(&renewal, failed_at) ==
              APPLIANCE_ACME_RENEWAL_OK,
          "the started attempt ends as failed");
    CHECK(appliance_acme_renewal_last_outcome(&renewal) ==
              APPLIANCE_ACME_RENEWAL_OUTCOME_FAILED,
          "the failure is recorded");
    CHECK(appliance_acme_renewal_last_attempt_epoch_seconds(&renewal) ==
              failed_at,
          "the retry window runs from the failure");
    CHECK(record_names(&renewal, "attempt=failed"),
          "the failure is published by name");
    CHECK(appliance_acme_renewal_next_attempt_epoch_seconds(&renewal) ==
              failed_at + RETRY,
          "the record names the next attempt instant");

    CHECK(appliance_acme_renewal_evaluate(&renewal, failed_at) ==
              APPLIANCE_ACME_RENEWAL_DECISION_RETRY_AFTER,
          "the same instant is still inside the retry window");
    CHECK(appliance_acme_renewal_evaluate(&renewal, failed_at + RETRY - 1U) ==
              APPLIANCE_ACME_RENEWAL_DECISION_RETRY_AFTER,
          "the last second of the retry window waits");
    CHECK(appliance_acme_renewal_evaluate(&renewal, failed_at + RETRY) ==
              APPLIANCE_ACME_RENEWAL_DECISION_DUE,
          "the first second after the retry window is due");
    CHECK(appliance_acme_renewal_attempt_started(
              &renewal, failed_at + RETRY) == APPLIANCE_ACME_RENEWAL_OK,
          "the retry starts after the window");
    CHECK(appliance_acme_renewal_attempts(&renewal) == 2U,
          "the retry is counted");
    CHECK(appliance_acme_renewal_attempt_failed(&renewal, failed_at + RETRY) ==
              APPLIANCE_ACME_RENEWAL_OK,
          "the retry fails at once");
    CHECK(appliance_acme_renewal_attempt_failed(
              &renewal, failed_at + RETRY) ==
              APPLIANCE_ACME_RENEWAL_NOT_ATTEMPTED,
          "a second failure report without an attempt is refused");
}

static void test_the_budget(void)
{
    struct appliance_acme_renewal renewal;
    const uint32_t now = 4000000U;
    const uint32_t expires = now + WINDOW;
    uint32_t index;

    appliance_acme_renewal_init(&renewal);
    CHECK(appliance_acme_renewal_certificate_obtained(
              &renewal, now, expires) == APPLIANCE_ACME_RENEWAL_OK,
          "the fixture certificate is in place");
    for (index = 0U; index < ATTEMPT_MAX; ++index) {
        const uint32_t instant = now + index * RETRY;

        CHECK(appliance_acme_renewal_attempt_started(&renewal, instant) ==
                  APPLIANCE_ACME_RENEWAL_OK,
              "attempt %u starts inside the window",
              (unsigned)(index + 1U));
        CHECK(appliance_acme_renewal_attempt_failed(&renewal, instant) ==
                  APPLIANCE_ACME_RENEWAL_OK,
              "attempt %u fails",
              (unsigned)(index + 1U));
    }
    CHECK(appliance_acme_renewal_attempts(&renewal) == ATTEMPT_MAX,
          "the attempt budget is spent exactly at its bound");
    CHECK(appliance_acme_renewal_evaluate(&renewal, now + 10U * RETRY) ==
              APPLIANCE_ACME_RENEWAL_DECISION_ATTEMPTS_EXHAUSTED,
          "a spent budget stops asking");
    CHECK(record_names(&renewal, "decision=attempts-exhausted"),
          "the exhausted budget is published by name");
    CHECK(appliance_acme_renewal_attempt_started(&renewal, now + 10U * RETRY) ==
              APPLIANCE_ACME_RENEWAL_ATTEMPTS_EXHAUSTED,
          "no attempt starts on a spent budget");
    CHECK(appliance_acme_renewal_attempts(&renewal) == ATTEMPT_MAX,
          "the refused attempt counted nothing");

    CHECK(appliance_acme_renewal_attempts_reset(&renewal) ==
              APPLIANCE_ACME_RENEWAL_OK,
          "the operator's reset reopens the budget");
    CHECK(appliance_acme_renewal_attempts(&renewal) == 0U &&
              appliance_acme_renewal_last_outcome(&renewal) ==
                  APPLIANCE_ACME_RENEWAL_OUTCOME_NONE &&
              appliance_acme_renewal_last_attempt_epoch_seconds(&renewal) == 0U,
          "the reset clears the counter and the failure");
    CHECK(appliance_acme_renewal_expires_at(&renewal) == expires,
          "the reset keeps the certificate it had");
    CHECK(appliance_acme_renewal_evaluate(&renewal, now + 10U * RETRY) ==
              APPLIANCE_ACME_RENEWAL_DECISION_DUE,
          "the reset certificate is due again");

    /* A verified certificate clears everything the failures left behind. */
    CHECK(appliance_acme_renewal_attempt_started(&renewal, now + 10U * RETRY) ==
              APPLIANCE_ACME_RENEWAL_OK,
          "a new attempt starts after the reset");
    CHECK(appliance_acme_renewal_attempt_failed(
              &renewal, now + 10U * RETRY) == APPLIANCE_ACME_RENEWAL_OK,
          "the new attempt fails");
    CHECK(appliance_acme_renewal_certificate_obtained(
              &renewal, now + 11U * RETRY, expires + 2U * WINDOW) ==
              APPLIANCE_ACME_RENEWAL_OK,
          "the renewal succeeds");
    CHECK(appliance_acme_renewal_attempts(&renewal) == 0U &&
              appliance_acme_renewal_last_outcome(&renewal) ==
                  APPLIANCE_ACME_RENEWAL_OUTCOME_SUCCEEDED,
          "the success clears the failures");
    CHECK(appliance_acme_renewal_evaluate(&renewal, now + 11U * RETRY) ==
              APPLIANCE_ACME_RENEWAL_DECISION_NOT_NEEDED,
          "the renewed certificate is not due");
}

static void test_clock_backwards_and_overflow(void)
{
    struct appliance_acme_renewal renewal;
    const uint32_t now = 5000000U;

    appliance_acme_renewal_init(&renewal);
    CHECK(appliance_acme_renewal_certificate_obtained(
              &renewal, now, now + WINDOW) == APPLIANCE_ACME_RENEWAL_OK,
          "the fixture certificate is in place");
    CHECK(appliance_acme_renewal_attempt_started(&renewal, now + 10U) ==
              APPLIANCE_ACME_RENEWAL_OK,
          "the fixture attempt starts");
    CHECK(appliance_acme_renewal_attempt_failed(&renewal, now + 20U) ==
              APPLIANCE_ACME_RENEWAL_OK,
          "the fixture attempt fails");

    CHECK(appliance_acme_renewal_evaluate(&renewal, now + 19U) ==
              APPLIANCE_ACME_RENEWAL_DECISION_BAD_CLOCK,
          "a clock that moved backwards is not a validated clock");
    CHECK(appliance_acme_renewal_attempt_failed(&renewal, now + 19U) ==
              APPLIANCE_ACME_RENEWAL_BAD_CLOCK,
          "a failure at a moved-back instant is refused");

    /* A retry instant at the top of the clock saturates instead of wrapping. */
    struct appliance_acme_renewal edge;
    const uint32_t early = UINT32_MAX - 3U * WINDOW;

    appliance_acme_renewal_init(&edge);
    CHECK(appliance_acme_renewal_certificate_obtained(
              &edge, early, UINT32_MAX) ==
              APPLIANCE_ACME_RENEWAL_OK,
          "a certificate valid to the end of the clock is accepted");
    CHECK(appliance_acme_renewal_evaluate(&edge, early) ==
              APPLIANCE_ACME_RENEWAL_DECISION_NOT_NEEDED,
          "a certificate three windows from the end is not due");
    CHECK(appliance_acme_renewal_attempt_started(&edge, early) ==
              APPLIANCE_ACME_RENEWAL_NOT_DUE,
          "the far expiry starts no attempt");
    const uint32_t late = early + 2U * WINDOW + 1U;
    CHECK(appliance_acme_renewal_attempt_started(&edge, late) ==
              APPLIANCE_ACME_RENEWAL_OK,
          "the last window before the expiry is due");
    CHECK(appliance_acme_renewal_attempt_failed(&edge, UINT32_MAX - 1U) ==
              APPLIANCE_ACME_RENEWAL_OK,
          "the failure near the end of the clock is accepted");
    CHECK(appliance_acme_renewal_next_attempt_epoch_seconds(&edge) ==
              UINT32_MAX,
          "the next attempt saturates instead of wrapping");
    CHECK(appliance_acme_renewal_evaluate(&edge, UINT32_MAX) ==
              APPLIANCE_ACME_RENEWAL_DECISION_RETRY_AFTER,
          "the saturated retry window still waits");
}

static void test_the_record(void)
{
    struct appliance_acme_renewal renewal;
    const uint32_t now = 6000000U;

    appliance_acme_renewal_init(&renewal);
    CHECK(appliance_acme_renewal_certificate_obtained(
              &renewal, now, now + 3U * WINDOW) == APPLIANCE_ACME_RENEWAL_OK,
          "the fixture certificate is in place");
    CHECK(strncmp(appliance_acme_renewal_record(&renewal), "[ACME-RENEWAL] ",
                  15U) == 0,
          "every published record carries the module's marker");
    CHECK(strlen(appliance_acme_renewal_record(&renewal)) <
              APPLIANCE_ACME_RENEWAL_RECORD_MAX,
          "the record fits the published bound");
    CHECK(record_names(&renewal, "expires="),
          "the record names the expiry the decision runs on");
    CHECK(record_names(&renewal, "next=0"),
          "a settled state waits for no attempt");

    /* The record names the decision the state holds, and holds no material. */
    CHECK(appliance_acme_renewal_evaluate(&renewal, now + 100U) ==
              APPLIANCE_ACME_RENEWAL_DECISION_NOT_NEEDED,
          "the fixture decision is not-needed");
    CHECK(strstr(appliance_acme_renewal_record(&renewal),
                 "decision=not-needed") != NULL,
          "the record names the decision itself");
    CHECK(strstr(appliance_acme_renewal_record(&renewal), "key") == NULL &&
              strstr(appliance_acme_renewal_record(&renewal), "token") == NULL,
          "no record mentions a key or a token");
}

static void test_the_names(void)
{
    CHECK(strcmp(appliance_acme_renewal_decision_string(
                     APPLIANCE_ACME_RENEWAL_DECISION_DUE), "due") == 0 &&
              strcmp(appliance_acme_renewal_decision_string(
                     APPLIANCE_ACME_RENEWAL_DECISION_RETRY_AFTER),
                     "retry-after") == 0 &&
              strcmp(appliance_acme_renewal_decision_string(
                     APPLIANCE_ACME_RENEWAL_DECISION_ATTEMPTS_EXHAUSTED),
                     "attempts-exhausted") == 0,
          "every decision has its published name");
    CHECK(strcmp(appliance_acme_renewal_status_string(
                     APPLIANCE_ACME_RENEWAL_OK), "ok") == 0 &&
              strcmp(appliance_acme_renewal_status_string(
                     APPLIANCE_ACME_RENEWAL_NOT_DUE), "not-due") == 0 &&
              strcmp(appliance_acme_renewal_status_string(
                     APPLIANCE_ACME_RENEWAL_BAD_VALUE), "bad-value") == 0,
          "every status has its published name");
    CHECK(strcmp(appliance_acme_renewal_outcome_string(
                     APPLIANCE_ACME_RENEWAL_OUTCOME_SUCCEEDED),
                 "succeeded") == 0 &&
              strcmp(appliance_acme_renewal_outcome_string(
                     APPLIANCE_ACME_RENEWAL_OUTCOME_FAILED), "failed") == 0,
          "every outcome has its published name");
    CHECK(strcmp(appliance_acme_renewal_decision_string(
                     (enum appliance_acme_renewal_decision)99), "unknown") == 0 &&
              strcmp(appliance_acme_renewal_status_string(
                     (enum appliance_acme_renewal_status)99), "unknown") == 0 &&
              strcmp(appliance_acme_renewal_outcome_string(
                     (enum appliance_acme_renewal_outcome)99), "unknown") == 0,
          "an unknown value is named unknown");
}

int main(void)
{
    if (APPLIANCE_ACME_RENEWAL_VERSION != 1U ||
        APPLIANCE_ACME_RENEWAL_WINDOW_SECONDS != UINT32_C(2592000) ||
        APPLIANCE_ACME_RENEWAL_RETRY_SECONDS != UINT32_C(21600) ||
        APPLIANCE_ACME_RENEWAL_ATTEMPT_MAX != 5U) {
        fprintf(stderr, "FAIL: the published constants changed\n");
        return 1;
    }
    test_the_empty_state();
    test_the_clock();
    test_no_certificate();
    test_certificate_obtained();
    test_the_window();
    test_attempts_and_retry();
    test_the_budget();
    test_clock_backwards_and_overflow();
    test_the_record();
    test_the_names();

    if (failures != 0) {
        fprintf(stderr, "appliance acme renewal host test: %d failure(s)\n",
                failures);
        return 1;
    }
    printf("appliance acme renewal host test: OK\n");
    return 0;
}
