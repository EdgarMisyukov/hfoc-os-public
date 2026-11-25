#include "icmp_reachability.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifdef HFOC_ICMP_REACHABILITY_HOST_TEST

#define TEST_BUDGET 4U
#define TEST_WINDOW 100U

static int failures;
static unsigned int alloc_attempts;
static unsigned int free_attempts;
static struct icmp_reachability_engine engine;
static struct icmp_reachability_policy policy;
static unsigned char request[ICMP_REACHABILITY_MESSAGE_MAX];
static unsigned char reply[ICMP_REACHABILITY_MESSAGE_MAX];

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

static void reset_engine(void)
{
    memset(&engine, 0, sizeof(engine));
    memset(&policy, 0, sizeof(policy));
    policy.version = ICMP_REACHABILITY_VERSION;
    policy.rate_budget = TEST_BUDGET;
    policy.rate_window_ticks = TEST_WINDOW;
    alloc_attempts = 0U;
    free_attempts = 0U;
}

static uint16_t icmp_checksum(
    const unsigned char *data, size_t length)
{
    uint32_t sum = 0U;
    size_t offset;

    for (offset = 0U; offset + 1U < length; offset += 2U) {
        sum += ((uint32_t)data[offset] << 8U) |
               (uint32_t)data[offset + 1U];
    }
    if ((length & 1U) != 0U) {
        sum += (uint32_t)data[length - 1U] << 8U;
    }
    while ((sum >> 16U) != 0U) {
        sum = (sum & 0xFFFFU) + (sum >> 16U);
    }
    return (uint16_t)~sum;
}

static void store_checksum(
    unsigned char *data, size_t length)
{
    uint16_t checksum;

    data[2] = 0U;
    data[3] = 0U;
    checksum = icmp_checksum(data, length);
    data[2] = (unsigned char)(checksum >> 8U);
    data[3] = (unsigned char)(checksum & 0xFFU);
}

static bool checksum_valid(
    const unsigned char *data, size_t length)
{
    return icmp_checksum(data, length) == 0U;
}

static void write_echo_request(
    unsigned char *data,
    size_t length,
    uint16_t identifier,
    uint16_t sequence)
{
    size_t offset;

    memset(data, 0, length);
    data[0] = (unsigned char)ICMP_REACHABILITY_TYPE_ECHO_REQUEST;
    data[1] = 0U;
    data[4] = (unsigned char)(identifier >> 8U);
    data[5] = (unsigned char)(identifier & 0xFFU);
    data[6] = (unsigned char)(sequence >> 8U);
    data[7] = (unsigned char)(sequence & 0xFFU);
    for (offset = ICMP_REACHABILITY_HEADER_SIZE;
         offset < length; ++offset) {
        data[offset] = (unsigned char)(offset * 7U + 3U);
    }
    store_checksum(data, length);
}

static bool buffer_filled_with(
    const unsigned char *data,
    size_t length,
    unsigned char value)
{
    size_t offset;

    for (offset = 0U; offset < length; ++offset) {
        if (data[offset] != value) {
            return false;
        }
    }
    return true;
}

static void test_status_strings(void)
{
    static const struct {
        enum icmp_reachability_status status;
        const char *string;
    } statuses[] = {
        {ICMP_REACHABILITY_OK, "ICMP_REACHABILITY_OK"},
        {ICMP_REACHABILITY_NULL, "ICMP_REACHABILITY_NULL"},
        {ICMP_REACHABILITY_BAD_STATE, "ICMP_REACHABILITY_BAD_STATE"},
        {ICMP_REACHABILITY_BAD_VERSION, "ICMP_REACHABILITY_BAD_VERSION"},
        {ICMP_REACHABILITY_BAD_BUDGET, "ICMP_REACHABILITY_BAD_BUDGET"},
        {ICMP_REACHABILITY_BAD_WINDOW, "ICMP_REACHABILITY_BAD_WINDOW"},
        {ICMP_REACHABILITY_ALREADY_PUBLISHED,
         "ICMP_REACHABILITY_ALREADY_PUBLISHED"},
        {ICMP_REACHABILITY_UNPUBLISHED,
         "ICMP_REACHABILITY_UNPUBLISHED"},
        {ICMP_REACHABILITY_DENIED, "ICMP_REACHABILITY_DENIED"},
        {ICMP_REACHABILITY_RATE_LIMITED,
         "ICMP_REACHABILITY_RATE_LIMITED"},
        {ICMP_REACHABILITY_MALFORMED,
         "ICMP_REACHABILITY_MALFORMED"},
        {ICMP_REACHABILITY_TOO_SHORT,
         "ICMP_REACHABILITY_TOO_SHORT"},
        {ICMP_REACHABILITY_TOO_LONG, "ICMP_REACHABILITY_TOO_LONG"},
        {ICMP_REACHABILITY_BAD_CHECKSUM,
         "ICMP_REACHABILITY_BAD_CHECKSUM"},
        {ICMP_REACHABILITY_BAD_TYPE, "ICMP_REACHABILITY_BAD_TYPE"},
        {ICMP_REACHABILITY_BAD_CODE, "ICMP_REACHABILITY_BAD_CODE"},
        {ICMP_REACHABILITY_OUTPUT_TOO_SMALL,
         "ICMP_REACHABILITY_OUTPUT_TOO_SMALL"},
        {ICMP_REACHABILITY_OVERLAPPING_BUFFERS,
         "ICMP_REACHABILITY_OVERLAPPING_BUFFERS"},
    };
    size_t index;

    for (index = 0U;
         index < sizeof(statuses) / sizeof(statuses[0]);
         ++index) {
        const char *string = icmp_reachability_status_string(
            statuses[index].status);

        CHECK(string != NULL &&
                  strcmp(string, statuses[index].string) == 0,
              "ICMP reachability status string is exact");
    }
    CHECK(icmp_reachability_status_string(
              (enum icmp_reachability_status)18U) == NULL,
          "ICMP reachability undefined status is rejected");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "ICMP status handling performs no allocation");
}

static void test_init_validation(void)
{
    struct icmp_reachability_policy bad;

    reset_engine();
    CHECK(icmp_reachability_engine_init(
              NULL, &policy) == ICMP_REACHABILITY_NULL,
          "engine init rejects null engine");
    CHECK(icmp_reachability_engine_init(
              &engine, NULL) == ICMP_REACHABILITY_NULL,
          "engine init rejects null policy");
    bad = policy;
    bad.version = ICMP_REACHABILITY_VERSION + 1U;
    CHECK(icmp_reachability_engine_init(
              &engine, &bad) == ICMP_REACHABILITY_BAD_VERSION,
          "engine init rejects invalid policy version");
    bad = policy;
    bad.rate_budget = ICMP_REACHABILITY_RATE_BUDGET_MIN - 1U;
    CHECK(icmp_reachability_engine_init(
              &engine, &bad) == ICMP_REACHABILITY_BAD_BUDGET,
          "engine init rejects zero rate budget");
    bad = policy;
    bad.rate_budget = ICMP_REACHABILITY_RATE_BUDGET_MAX + 1U;
    CHECK(icmp_reachability_engine_init(
              &engine, &bad) == ICMP_REACHABILITY_BAD_BUDGET,
          "engine init rejects over-limit rate budget");
    bad = policy;
    bad.rate_window_ticks = ICMP_REACHABILITY_WINDOW_MIN - 1U;
    CHECK(icmp_reachability_engine_init(
              &engine, &bad) == ICMP_REACHABILITY_BAD_WINDOW,
          "engine init rejects zero rate window");
    bad = policy;
    bad.rate_window_ticks = ICMP_REACHABILITY_WINDOW_MAX + 1U;
    CHECK(icmp_reachability_engine_init(
              &engine, &bad) == ICMP_REACHABILITY_BAD_WINDOW,
          "engine init rejects over-limit rate window");
    CHECK(icmp_reachability_published(NULL) == false &&
              icmp_reachability_window_count(NULL) == 0U,
          "null engine accessors return inactive state");
    CHECK(icmp_reachability_engine_init(
              &engine, &policy) == ICMP_REACHABILITY_OK &&
              icmp_reachability_published(&engine) &&
              icmp_reachability_window_count(&engine) == 0U,
          "valid engine init publishes an empty rate window");
    CHECK(icmp_reachability_engine_init(
              &engine, &policy) == ICMP_REACHABILITY_ALREADY_PUBLISHED &&
              icmp_reachability_window_count(&engine) == 0U,
          "duplicate engine init is rejected without mutation");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "engine init performs no allocation");
}

static void test_authorized_echo_reply(void)
{
    size_t length = 24U;
    size_t reply_length = 0U;
    size_t offset;

    reset_engine();
    CHECK(icmp_reachability_engine_init(
              &engine, &policy) == ICMP_REACHABILITY_OK,
          "authorized fixture initializes engine");
    write_echo_request(request, length, 0x1234U, 0xABCDU);
    memset(reply, 0xA5, sizeof(reply));
    CHECK(icmp_reachability_handle_request(
              &engine, request, length, true, 1U,
              reply, sizeof(reply), &reply_length) ==
              ICMP_REACHABILITY_OK &&
              reply_length == length &&
              reply[0] == (unsigned char)ICMP_REACHABILITY_TYPE_ECHO_REPLY &&
              reply[1] == 0U &&
              reply[4] == 0x12U && reply[5] == 0x34U &&
              reply[6] == 0xABU && reply[7] == 0xCDU &&
              checksum_valid(reply, reply_length),
          "authorized echo request builds an exact bounded reply");
    for (offset = ICMP_REACHABILITY_HEADER_SIZE;
         offset < length; ++offset) {
        if (reply[offset] != request[offset]) {
            CHECK(false, "echo reply preserves exact payload");
            return;
        }
    }
    CHECK(true, "echo reply preserves exact payload");
    CHECK(icmp_reachability_window_count(&engine) == 1U,
          "authorized echo reply consumes one rate slot");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "authorized echo handling performs no allocation");
}

static void test_denied_request(void)
{
    size_t length = 24U;
    size_t reply_length = 0xDEADU;

    reset_engine();
    CHECK(icmp_reachability_engine_init(
              &engine, &policy) == ICMP_REACHABILITY_OK,
          "denied fixture initializes engine");
    write_echo_request(request, length, 1U, 2U);
    memset(reply, 0x5A, sizeof(reply));
    reply_length = 0xDEADU;
    CHECK(icmp_reachability_handle_request(
              &engine, request, length, false, 7U,
              reply, sizeof(reply), &reply_length) ==
              ICMP_REACHABILITY_DENIED &&
              buffer_filled_with(reply, sizeof(reply), 0x5AU) &&
              reply_length == 0xDEADU &&
              icmp_reachability_window_count(&engine) == 0U,
          "unauthorized request is denied before parse or rate accounting");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "denied request performs no allocation");
}

static void test_malformed_rejection(void)
{
    size_t length = 24U;
    size_t reply_length = 0xDEADU;
    unsigned char oversized[ICMP_REACHABILITY_MESSAGE_MAX + 1U];

    reset_engine();
    CHECK(icmp_reachability_engine_init(
              &engine, &policy) == ICMP_REACHABILITY_OK,
          "malformed fixture initializes engine");

    memset(reply, 0x3C, sizeof(reply));
    reply_length = 0xDEADU;
    write_echo_request(request, 7U, 1U, 2U);
    CHECK(icmp_reachability_handle_request(
              &engine, request, 7U, true, 10U,
              reply, sizeof(reply), &reply_length) ==
              ICMP_REACHABILITY_TOO_SHORT &&
              buffer_filled_with(reply, sizeof(reply), 0x3CU) &&
              reply_length == 0xDEADU &&
              icmp_reachability_window_count(&engine) == 0U,
          "too-short ICMP request is rejected without side effects");

    memset(reply, 0x3C, sizeof(reply));
    reply_length = 0xDEADU;
    write_echo_request(oversized, ICMP_REACHABILITY_MESSAGE_MAX + 1U,
                       1U, 2U);
    CHECK(icmp_reachability_handle_request(
              &engine, oversized, ICMP_REACHABILITY_MESSAGE_MAX + 1U,
              true, 10U, reply, sizeof(reply), &reply_length) ==
              ICMP_REACHABILITY_TOO_LONG &&
              buffer_filled_with(reply, sizeof(reply), 0x3CU) &&
              reply_length == 0xDEADU &&
              icmp_reachability_window_count(&engine) == 0U,
          "too-long ICMP request is rejected without side effects");

    memset(reply, 0x3C, sizeof(reply));
    reply_length = 0xDEADU;
    write_echo_request(request, length, 1U, 2U);
    request[0] = (unsigned char)ICMP_REACHABILITY_TYPE_ECHO_REPLY;
    store_checksum(request, length);
    CHECK(icmp_reachability_handle_request(
              &engine, request, length, true, 10U,
              reply, sizeof(reply), &reply_length) ==
              ICMP_REACHABILITY_BAD_TYPE &&
              buffer_filled_with(reply, sizeof(reply), 0x3CU) &&
              reply_length == 0xDEADU &&
              icmp_reachability_window_count(&engine) == 0U,
          "non-echo ICMP type is rejected without side effects");

    memset(reply, 0x3C, sizeof(reply));
    reply_length = 0xDEADU;
    write_echo_request(request, length, 1U, 2U);
    request[1] = 1U;
    store_checksum(request, length);
    CHECK(icmp_reachability_handle_request(
              &engine, request, length, true, 10U,
              reply, sizeof(reply), &reply_length) ==
              ICMP_REACHABILITY_BAD_CODE &&
              buffer_filled_with(reply, sizeof(reply), 0x3CU) &&
              reply_length == 0xDEADU &&
              icmp_reachability_window_count(&engine) == 0U,
          "non-zero echo code is rejected without side effects");

    memset(reply, 0x3C, sizeof(reply));
    reply_length = 0xDEADU;
    write_echo_request(request, length, 1U, 2U);
    request[length - 1U] ^= 0x01U;
    CHECK(icmp_reachability_handle_request(
              &engine, request, length, true, 10U,
              reply, sizeof(reply), &reply_length) ==
              ICMP_REACHABILITY_BAD_CHECKSUM &&
              buffer_filled_with(reply, sizeof(reply), 0x3CU) &&
              reply_length == 0xDEADU &&
              icmp_reachability_window_count(&engine) == 0U,
          "invalid ICMP checksum is rejected without side effects");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "malformed rejection performs no allocation");
}

static void test_rate_limit_window(void)
{
    size_t length = 16U;
    size_t reply_length = 0U;

    reset_engine();
    policy.rate_budget = 2U;
    policy.rate_window_ticks = 10U;
    CHECK(icmp_reachability_engine_init(
              &engine, &policy) == ICMP_REACHABILITY_OK,
          "rate-limit fixture initializes engine");
    write_echo_request(request, length, 7U, 8U);

    memset(reply, 0xAA, sizeof(reply));
    CHECK(icmp_reachability_handle_request(
              &engine, request, length, true, 100U,
              reply, sizeof(reply), &reply_length) ==
              ICMP_REACHABILITY_OK &&
              reply_length == length &&
              icmp_reachability_window_count(&engine) == 1U,
          "first request inside rate window is accepted");
    CHECK(icmp_reachability_handle_request(
              &engine, request, length, true, 100U,
              reply, sizeof(reply), &reply_length) ==
              ICMP_REACHABILITY_OK &&
              reply_length == length &&
              icmp_reachability_window_count(&engine) == 2U,
          "second request inside rate window is accepted");

    memset(reply, 0x55, sizeof(reply));
    reply_length = 0xDEADU;
    CHECK(icmp_reachability_handle_request(
              &engine, request, length, true, 100U,
              reply, sizeof(reply), &reply_length) ==
              ICMP_REACHABILITY_RATE_LIMITED &&
              buffer_filled_with(reply, sizeof(reply), 0x55U) &&
              reply_length == 0xDEADU &&
              icmp_reachability_window_count(&engine) == 2U,
          "third request inside rate window is rate limited");
    CHECK(icmp_reachability_handle_request(
              &engine, request, length, true, 109U,
              reply, sizeof(reply), &reply_length) ==
              ICMP_REACHABILITY_RATE_LIMITED &&
              icmp_reachability_window_count(&engine) == 2U,
          "request before window expiry remains rate limited");
    CHECK(icmp_reachability_handle_request(
              &engine, request, length, true, 110U,
              reply, sizeof(reply), &reply_length) ==
              ICMP_REACHABILITY_OK &&
              reply_length == length &&
              icmp_reachability_window_count(&engine) == 1U,
          "request after window expiry resets the rate window");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "rate-limit handling performs no allocation");
}

static void test_output_and_overlap_failures(void)
{
    size_t length = 24U;
    size_t reply_length = 0xDEADU;
    size_t small_reply_length = 0xDEADU;
    unsigned char request_before[ICMP_REACHABILITY_MESSAGE_MAX];
    unsigned char small_reply[16U];

    reset_engine();
    CHECK(icmp_reachability_engine_init(
              &engine, &policy) == ICMP_REACHABILITY_OK,
          "output-failure fixture initializes engine");
    write_echo_request(request, length, 3U, 4U);
    memset(small_reply, 0x6B, sizeof(small_reply));
    CHECK(icmp_reachability_handle_request(
              &engine, request, length, true, 200U,
              small_reply, sizeof(small_reply), &small_reply_length) ==
              ICMP_REACHABILITY_OUTPUT_TOO_SMALL &&
              buffer_filled_with(small_reply,
                                 sizeof(small_reply), 0x6BU) &&
              small_reply_length == 0xDEADU &&
              icmp_reachability_window_count(&engine) == 0U,
          "insufficient reply capacity fails without output or rate mutation");

    memset(reply, 0x7C, sizeof(reply));
    reply_length = 0xDEADU;
    memcpy(request_before, request, length);
    CHECK(icmp_reachability_handle_request(
              &engine, request, length, true, 200U,
              request, length, &reply_length) ==
              ICMP_REACHABILITY_OVERLAPPING_BUFFERS &&
              memcmp(request_before, request, length) == 0 &&
              reply_length == 0xDEADU &&
              icmp_reachability_window_count(&engine) == 0U,
          "overlapping request/reply buffers fail without mutation");
    CHECK(icmp_reachability_handle_request(
              &engine, request, length, true, 200U,
              NULL, sizeof(reply), &reply_length) ==
              ICMP_REACHABILITY_NULL,
          "null reply is rejected");
    CHECK(icmp_reachability_handle_request(
              &engine, request, length, true, 200U,
              reply, sizeof(reply), NULL) ==
              ICMP_REACHABILITY_NULL,
          "null reply length is rejected");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "output and overlap failures perform no allocation");
}

int main(void)
{
    test_status_strings();
    test_init_validation();
    test_authorized_echo_reply();
    test_denied_request();
    test_malformed_rejection();
    test_rate_limit_window();
    test_output_and_overlap_failures();

    if (failures == 0) {
        printf("ICMP reachability host tests: ALL OK\n");
    }
    return failures != 0;
}

#else
#error "This host test must be built with HFOC_ICMP_REACHABILITY_HOST_TEST"
#endif
