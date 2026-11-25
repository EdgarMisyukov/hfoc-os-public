#include "audit_ring_health.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifdef HFOC_AUDIT_RING_HEALTH_HOST_TEST

static int failures;
static unsigned int alloc_attempts;
static unsigned int free_attempts;
static struct event_ring ring;

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

static struct kernel_event make_event(uint32_t index)
{
    struct kernel_event event;

    memset(&event, 0, sizeof(event));
    event.type = 1U;
    event.flags = 1U;
    event.data[0] = index;
    event.data[1] = index + 1U;
    return event;
}

static void reset_fixture(void)
{
    memset(&ring, 0, sizeof(ring));
    alloc_attempts = 0U;
    free_attempts = 0U;
}

static struct audit_ring_health_input make_input(
    uint32_t warn_threshold,
    uint32_t fail_threshold)
{
    struct audit_ring_health_input input;

    memset(&input, 0, sizeof(input));
    input.version = AUDIT_RING_HEALTH_VERSION;
    input.ring = &ring;
    input.warn_threshold = warn_threshold;
    input.fail_threshold = fail_threshold;
    return input;
}

static void expect_no_verdict(
    const struct audit_ring_health_input *input,
    enum audit_ring_health_status expected,
    const char *message)
{
    struct audit_ring_health_verdict verdict;
    unsigned char before[sizeof(verdict)];

    memset(&verdict, 0xA5, sizeof(verdict));
    memcpy(before, &verdict, sizeof(before));
    CHECK(audit_ring_health_evaluate(input, &verdict) == expected &&
              memcmp(&verdict, before, sizeof(before)) == 0,
          message);
}

static void fill_ring(uint32_t count)
{
    uint32_t index;

    CHECK(event_ring_init(&ring) == EVENT_RING_OK,
          "ring pressure fixture initializes event ring");
    for (index = 0U; index < count; ++index) {
        const struct kernel_event event = make_event(index);

        CHECK(event_ring_push(&ring, &event) == EVENT_RING_OK,
              "ring pressure fixture fills requested events");
    }
}

static void test_status_and_fault_strings(void)
{
    static const struct {
        enum audit_ring_health_status status;
        const char *string;
    } statuses[] = {
        {AUDIT_RING_HEALTH_OK, "AUDIT_RING_HEALTH_OK"},
        {AUDIT_RING_HEALTH_NULL, "AUDIT_RING_HEALTH_NULL"},
        {AUDIT_RING_HEALTH_BAD_VERSION,
         "AUDIT_RING_HEALTH_BAD_VERSION"},
        {AUDIT_RING_HEALTH_BAD_THRESHOLD,
         "AUDIT_RING_HEALTH_BAD_THRESHOLD"},
        {AUDIT_RING_HEALTH_BAD_STATE,
         "AUDIT_RING_HEALTH_BAD_STATE"},
    };
    static const struct {
        enum audit_ring_health_fault fault;
        const char *string;
    } faults[] = {
        {AUDIT_RING_HEALTH_FAULT_NONE,
         "AUDIT_RING_HEALTH_FAULT_NONE"},
        {AUDIT_RING_HEALTH_FAULT_PRESSURE,
         "AUDIT_RING_HEALTH_FAULT_PRESSURE"},
        {AUDIT_RING_HEALTH_FAULT_FULL,
         "AUDIT_RING_HEALTH_FAULT_FULL"},
    };
    size_t index;

    reset_fixture();
    for (index = 0U;
         index < sizeof(statuses) / sizeof(statuses[0]);
         ++index) {
        const char *string = audit_ring_health_status_string(
            statuses[index].status);

        CHECK(string != NULL &&
                  strcmp(string, statuses[index].string) == 0,
              "audit-ring health status string is exact");
    }
    CHECK(audit_ring_health_status_string(
              (enum audit_ring_health_status)5U) == NULL,
          "audit-ring health undefined status is rejected");

    for (index = 0U;
         index < sizeof(faults) / sizeof(faults[0]);
         ++index) {
        const char *string = audit_ring_health_fault_string(
            faults[index].fault);

        CHECK(string != NULL &&
                  strcmp(string, faults[index].string) == 0,
              "audit-ring health fault string is exact");
    }
    CHECK(audit_ring_health_fault_string(
              (enum audit_ring_health_fault)3U) == NULL,
          "audit-ring health undefined fault is rejected");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "audit-ring health strings perform no allocation");
}

static void test_null_and_version_failures(void)
{
    struct audit_ring_health_input input;
    struct audit_ring_health_verdict verdict;

    reset_fixture();
    input = make_input(16U, 32U);
    memset(&verdict, 0xA5, sizeof(verdict));

    CHECK(audit_ring_health_evaluate(NULL, &verdict) ==
              AUDIT_RING_HEALTH_NULL,
          "audit-ring health evaluate rejects null input");
    CHECK(audit_ring_health_evaluate(&input, NULL) ==
              AUDIT_RING_HEALTH_NULL,
          "audit-ring health evaluate rejects null verdict");

    input.version = 0U;
    expect_no_verdict(&input, AUDIT_RING_HEALTH_BAD_VERSION,
                      "audit-ring health rejects zero version");
    input.version = AUDIT_RING_HEALTH_VERSION + 1U;
    expect_no_verdict(&input, AUDIT_RING_HEALTH_BAD_VERSION,
                      "audit-ring health rejects future version");

    input = make_input(16U, 32U);
    input.ring = NULL;
    CHECK(audit_ring_health_evaluate(&input, &verdict) ==
              AUDIT_RING_HEALTH_NULL,
          "audit-ring health rejects null ring");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "audit-ring health null and version paths perform no allocation");
}

static void test_threshold_failures(void)
{
    struct audit_ring_health_input input;
    struct audit_ring_health_verdict verdict;

    reset_fixture();
    CHECK(event_ring_init(&ring) == EVENT_RING_OK,
          "threshold fixture initializes event ring");
    input = make_input(0U, 16U);
    expect_no_verdict(&input, AUDIT_RING_HEALTH_BAD_THRESHOLD,
                      "audit-ring health rejects zero warn threshold");
    input = make_input(1U, 0U);
    expect_no_verdict(&input, AUDIT_RING_HEALTH_BAD_THRESHOLD,
                      "audit-ring health rejects zero fail threshold");
    input = make_input(KERNEL_EVENT_RING_CAPACITY + 1U, 16U);
    expect_no_verdict(&input, AUDIT_RING_HEALTH_BAD_THRESHOLD,
                      "audit-ring health rejects over-capacity warn threshold");
    input = make_input(16U, KERNEL_EVENT_RING_CAPACITY + 1U);
    expect_no_verdict(&input, AUDIT_RING_HEALTH_BAD_THRESHOLD,
                      "audit-ring health rejects over-capacity fail threshold");
    input = make_input(32U, 16U);
    expect_no_verdict(&input, AUDIT_RING_HEALTH_BAD_THRESHOLD,
                      "audit-ring health rejects inverted thresholds");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "audit-ring health threshold failures perform no allocation");
    (void)verdict;
}

static void test_invalid_ring_state_fails_closed(void)
{
    struct audit_ring_health_input input;

    reset_fixture();
    input = make_input(16U, 32U);
    expect_no_verdict(&input, AUDIT_RING_HEALTH_BAD_STATE,
                      "audit-ring health rejects unpublished ring");

    CHECK(event_ring_init(&ring) == EVENT_RING_OK,
          "invalid-state fixture initializes event ring");
    input = make_input(16U, 32U);
    ring.count = KERNEL_EVENT_RING_CAPACITY + 1U;
    expect_no_verdict(&input, AUDIT_RING_HEALTH_BAD_STATE,
                      "audit-ring health rejects over-capacity count");

    reset_fixture();
    CHECK(event_ring_init(&ring) == EVENT_RING_OK,
          "invalid-cursor fixture initializes event ring");
    ring.write_cursor = 0U;
    ring.read_cursor = 1U;
    ring.count = 1U;
    input = make_input(16U, 32U);
    expect_no_verdict(&input, AUDIT_RING_HEALTH_BAD_STATE,
                      "audit-ring health rejects read cursor past write cursor");

    reset_fixture();
    CHECK(event_ring_init(&ring) == EVENT_RING_OK,
          "mismatched-count fixture initializes event ring");
    ring.write_cursor = 5U;
    ring.count = 4U;
    input = make_input(16U, 32U);
    expect_no_verdict(&input, AUDIT_RING_HEALTH_BAD_STATE,
                      "audit-ring health rejects cursor/count mismatch");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "audit-ring health invalid-state paths perform no allocation");
}

static void test_pass_pressure_verdict(void)
{
    struct audit_ring_health_input input;
    struct audit_ring_health_verdict verdict;

    reset_fixture();
    fill_ring(8U);
    input = make_input(16U, 32U);
    CHECK(audit_ring_health_evaluate(&input, &verdict) ==
              AUDIT_RING_HEALTH_OK &&
              verdict.version == AUDIT_RING_HEALTH_VERSION &&
              verdict.healthy &&
              !verdict.ring_full &&
              verdict.reject_on_full &&
              !verdict.overwrite_allowed &&
              !verdict.silent_loss_possible &&
              verdict.result == HEALTH_CHECK_RESULT_PASS &&
              verdict.severity == HEALTH_CHECK_SEVERITY_INFO &&
              verdict.fault == AUDIT_RING_HEALTH_FAULT_NONE &&
              verdict.capacity == KERNEL_EVENT_RING_CAPACITY &&
              verdict.count == 8U &&
              verdict.available ==
                  KERNEL_EVENT_RING_CAPACITY - 8U &&
              verdict.utilization_percent ==
                  (8U * 100U) / KERNEL_EVENT_RING_CAPACITY &&
              verdict.write_cursor == 8U &&
              verdict.read_cursor == 0U,
          "below-warn ring produces healthy pass verdict with exact fields");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "pass pressure path performs no allocation");
}

static void test_warn_pressure_verdict(void)
{
    struct audit_ring_health_input input;
    struct audit_ring_health_verdict verdict;

    reset_fixture();
    fill_ring(20U);
    input = make_input(16U, 32U);
    CHECK(audit_ring_health_evaluate(&input, &verdict) ==
              AUDIT_RING_HEALTH_OK &&
              !verdict.healthy &&
              !verdict.ring_full &&
              verdict.reject_on_full &&
              !verdict.overwrite_allowed &&
              !verdict.silent_loss_possible &&
              verdict.result == HEALTH_CHECK_RESULT_WARN &&
              verdict.severity == HEALTH_CHECK_SEVERITY_WARNING &&
              verdict.fault == AUDIT_RING_HEALTH_FAULT_PRESSURE &&
              verdict.count == 20U &&
              verdict.available ==
                  KERNEL_EVENT_RING_CAPACITY - 20U,
          "warn-threshold ring produces warning pressure verdict");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "warn pressure path performs no allocation");
}

static void test_fail_pressure_verdict(void)
{
    struct audit_ring_health_input input;
    struct audit_ring_health_verdict verdict;

    reset_fixture();
    fill_ring(40U);
    input = make_input(16U, 32U);
    CHECK(audit_ring_health_evaluate(&input, &verdict) ==
              AUDIT_RING_HEALTH_OK &&
              !verdict.healthy &&
              !verdict.ring_full &&
              verdict.reject_on_full &&
              !verdict.overwrite_allowed &&
              !verdict.silent_loss_possible &&
              verdict.result == HEALTH_CHECK_RESULT_FAIL &&
              verdict.severity == HEALTH_CHECK_SEVERITY_CRITICAL &&
              verdict.fault == AUDIT_RING_HEALTH_FAULT_PRESSURE &&
              verdict.count == 40U &&
              verdict.available ==
                  KERNEL_EVENT_RING_CAPACITY - 40U,
          "fail-threshold ring produces critical pressure verdict");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "fail pressure path performs no allocation");
}

static void test_full_ring_no_silent_loss(void)
{
    struct audit_ring_health_input input;
    struct audit_ring_health_verdict verdict;
    struct kernel_event event;
    struct kernel_event output;
    uint32_t index;

    reset_fixture();
    fill_ring(KERNEL_EVENT_RING_CAPACITY);
    input = make_input(16U, 32U);
    CHECK(audit_ring_health_evaluate(&input, &verdict) ==
              AUDIT_RING_HEALTH_OK &&
              !verdict.healthy &&
              verdict.ring_full &&
              verdict.reject_on_full &&
              !verdict.overwrite_allowed &&
              !verdict.silent_loss_possible &&
              verdict.result == HEALTH_CHECK_RESULT_FAIL &&
              verdict.severity == HEALTH_CHECK_SEVERITY_CRITICAL &&
              verdict.fault == AUDIT_RING_HEALTH_FAULT_FULL &&
              verdict.count == KERNEL_EVENT_RING_CAPACITY &&
              verdict.available == 0U &&
              verdict.utilization_percent == 100U &&
              verdict.write_cursor == KERNEL_EVENT_RING_CAPACITY &&
              verdict.read_cursor == 0U,
          "full ring produces critical full verdict with exact policy evidence");

    event = make_event(0xFFFFU);
    CHECK(event_ring_push(&ring, &event) == EVENT_RING_FULL &&
              event_ring_count(&ring) == KERNEL_EVENT_RING_CAPACITY &&
              event_ring_write_cursor(&ring) ==
                  KERNEL_EVENT_RING_CAPACITY,
          "full ring rejects extra event without overwriting producer state");
    for (index = 0U; index < KERNEL_EVENT_RING_CAPACITY; ++index) {
        CHECK(event_ring_pop(&ring, &output) == EVENT_RING_OK &&
                  output.sequence == index &&
                  output.type == 1U &&
                  output.data[0] == index,
              "full ring keeps every existing event readable");
    }
    CHECK(event_ring_pop(&ring, &output) == EVENT_RING_EMPTY,
          "full ring drains to empty after exact capacity reads");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "full-ring no-silent-loss path performs no allocation");
}

int main(void)
{
    test_status_and_fault_strings();
    test_null_and_version_failures();
    test_threshold_failures();
    test_invalid_ring_state_fails_closed();
    test_pass_pressure_verdict();
    test_warn_pressure_verdict();
    test_fail_pressure_verdict();
    test_full_ring_no_silent_loss();

    if (failures != 0) {
        fprintf(stderr, "Audit-ring health host tests: %d FAILURES\n",
                failures);
        return 1;
    }
    puts("Audit-ring health host tests: ALL OK");
    return 0;
}

#endif
