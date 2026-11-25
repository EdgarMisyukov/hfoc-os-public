#include "management_transport.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifdef HFOC_MANAGEMENT_TRANSPORT_HOST_TEST

static int failures;
static unsigned int alloc_attempts;
static unsigned int free_attempts;
static struct management_transport_policy policy;
static unsigned char frame[MANAGEMENT_TRANSPORT_FRAME_MAX + 1U];

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

static void reset_policy(void)
{
    memset(&policy, 0, sizeof(policy));
    policy.version = MANAGEMENT_TRANSPORT_VERSION;
    policy.port = 3000U;
    policy.payload_max = MANAGEMENT_TRANSPORT_PAYLOAD_MAX;
    alloc_attempts = 0U;
    free_attempts = 0U;
}

static void write_header(
    unsigned char *data,
    uint32_t version,
    uint16_t port,
    uint16_t payload_length,
    uint32_t flags)
{
    memset(data, 0, MANAGEMENT_TRANSPORT_HEADER_SIZE);
    data[0] = (unsigned char)(version & 0xFFU);
    data[1] = (unsigned char)((version >> 8U) & 0xFFU);
    data[2] = (unsigned char)((version >> 16U) & 0xFFU);
    data[3] = (unsigned char)((version >> 24U) & 0xFFU);
    data[4] = (unsigned char)(port & 0xFFU);
    data[5] = (unsigned char)((port >> 8U) & 0xFFU);
    data[6] = (unsigned char)(payload_length & 0xFFU);
    data[7] = (unsigned char)((payload_length >> 8U) & 0xFFU);
    data[8] = (unsigned char)(flags & 0xFFU);
    data[9] = (unsigned char)((flags >> 8U) & 0xFFU);
    data[10] = (unsigned char)((flags >> 16U) & 0xFFU);
    data[11] = (unsigned char)((flags >> 24U) & 0xFFU);
}

static void write_valid_frame(
    unsigned char *data,
    size_t payload_length,
    uint32_t direction,
    uint16_t port)
{
    size_t offset;

    memset(data, 0, MANAGEMENT_TRANSPORT_FRAME_MAX);
    write_header(data, MANAGEMENT_TRANSPORT_VERSION, port,
                 (uint16_t)payload_length, direction);
    for (offset = 0U; offset < payload_length; ++offset) {
        data[MANAGEMENT_TRANSPORT_HEADER_SIZE + offset] =
            (unsigned char)(offset * 5U + 11U);
    }
}

static void test_status_strings(void)
{
    static const struct {
        enum management_transport_status status;
        const char *string;
    } statuses[] = {
        {MANAGEMENT_TRANSPORT_OK, "MANAGEMENT_TRANSPORT_OK"},
        {MANAGEMENT_TRANSPORT_NULL, "MANAGEMENT_TRANSPORT_NULL"},
        {MANAGEMENT_TRANSPORT_BAD_POLICY,
         "MANAGEMENT_TRANSPORT_BAD_POLICY"},
        {MANAGEMENT_TRANSPORT_BAD_VERSION,
         "MANAGEMENT_TRANSPORT_BAD_VERSION"},
        {MANAGEMENT_TRANSPORT_BAD_PORT,
         "MANAGEMENT_TRANSPORT_BAD_PORT"},
        {MANAGEMENT_TRANSPORT_BAD_DIRECTION,
         "MANAGEMENT_TRANSPORT_BAD_DIRECTION"},
        {MANAGEMENT_TRANSPORT_BAD_FLAGS,
         "MANAGEMENT_TRANSPORT_BAD_FLAGS"},
        {MANAGEMENT_TRANSPORT_OVERSIZED,
         "MANAGEMENT_TRANSPORT_OVERSIZED"},
        {MANAGEMENT_TRANSPORT_PARTIAL,
         "MANAGEMENT_TRANSPORT_PARTIAL"},
        {MANAGEMENT_TRANSPORT_TRAILING_FIELDS,
         "MANAGEMENT_TRANSPORT_TRAILING_FIELDS"},
    };
    size_t index;

    for (index = 0U;
         index < sizeof(statuses) / sizeof(statuses[0]);
         ++index) {
        const char *string = management_transport_status_string(
            statuses[index].status);

        CHECK(string != NULL &&
                  strcmp(string, statuses[index].string) == 0,
              "management transport status string is exact");
    }
    CHECK(management_transport_status_string(
              (enum management_transport_status)10U) == NULL,
          "management transport undefined status is rejected");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "management transport status handling performs no allocation");
}

static void test_policy_and_null_validation(void)
{
    struct management_transport_policy bad;
    const void *payload = (const void *)(uintptr_t)0x1234U;
    size_t payload_length = 0xDEADU;

    reset_policy();
    write_valid_frame(frame, 0U,
                      MANAGEMENT_TRANSPORT_DIRECTION_REQUEST,
                      policy.port);
    CHECK(management_transport_validate_frame(
              NULL, frame, MANAGEMENT_TRANSPORT_HEADER_SIZE,
              &payload, &payload_length) == MANAGEMENT_TRANSPORT_NULL &&
              payload == (const void *)(uintptr_t)0x1234U &&
              payload_length == 0xDEADU,
          "null policy is rejected without output mutation");
    CHECK(management_transport_validate_frame(
              &policy, NULL, MANAGEMENT_TRANSPORT_HEADER_SIZE,
              &payload, &payload_length) == MANAGEMENT_TRANSPORT_NULL &&
              payload == (const void *)(uintptr_t)0x1234U &&
              payload_length == 0xDEADU,
          "null frame is rejected without output mutation");
    CHECK(management_transport_validate_frame(
              &policy, frame, MANAGEMENT_TRANSPORT_HEADER_SIZE,
              NULL, &payload_length) == MANAGEMENT_TRANSPORT_NULL &&
              payload_length == 0xDEADU,
          "null payload pointer is rejected without output mutation");
    CHECK(management_transport_validate_frame(
              &policy, frame, MANAGEMENT_TRANSPORT_HEADER_SIZE,
              &payload, NULL) == MANAGEMENT_TRANSPORT_NULL &&
              payload == (const void *)(uintptr_t)0x1234U,
          "null payload length pointer is rejected without output mutation");

    bad = policy;
    bad.version = MANAGEMENT_TRANSPORT_VERSION + 1U;
    CHECK(management_transport_validate_frame(
              &bad, frame, MANAGEMENT_TRANSPORT_HEADER_SIZE,
              &payload, &payload_length) ==
              MANAGEMENT_TRANSPORT_BAD_POLICY &&
              payload == (const void *)(uintptr_t)0x1234U &&
              payload_length == 0xDEADU,
          "invalid policy version is rejected without output mutation");
    bad = policy;
    bad.port = MANAGEMENT_TRANSPORT_PORT_MIN - 1U;
    CHECK(management_transport_validate_frame(
              &bad, frame, MANAGEMENT_TRANSPORT_HEADER_SIZE,
              &payload, &payload_length) ==
              MANAGEMENT_TRANSPORT_BAD_POLICY &&
              payload == (const void *)(uintptr_t)0x1234U &&
              payload_length == 0xDEADU,
          "zero policy port is rejected without output mutation");
    bad = policy;
    bad.payload_max = MANAGEMENT_TRANSPORT_PAYLOAD_MAX + 1U;
    CHECK(management_transport_validate_frame(
              &bad, frame, MANAGEMENT_TRANSPORT_HEADER_SIZE,
              &payload, &payload_length) ==
              MANAGEMENT_TRANSPORT_BAD_POLICY &&
              payload == (const void *)(uintptr_t)0x1234U &&
              payload_length == 0xDEADU,
          "over-limit policy payload maximum is rejected");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "policy and null validation performs no allocation");
}

static void test_valid_request_and_response_frames(void)
{
    const void *payload = NULL;
    size_t payload_length = 0U;
    size_t offset;

    reset_policy();
    write_valid_frame(frame, 0U,
                      MANAGEMENT_TRANSPORT_DIRECTION_REQUEST,
                      policy.port);
    CHECK(management_transport_validate_frame(
              &policy, frame, MANAGEMENT_TRANSPORT_HEADER_SIZE,
              &payload, &payload_length) == MANAGEMENT_TRANSPORT_OK &&
              payload == frame + MANAGEMENT_TRANSPORT_HEADER_SIZE &&
              payload_length == 0U,
          "valid empty request frame publishes exact empty payload");

    payload = NULL;
    payload_length = 0U;
    write_valid_frame(frame, 5U,
                      MANAGEMENT_TRANSPORT_DIRECTION_RESPONSE,
                      policy.port);
    CHECK(management_transport_validate_frame(
              &policy, frame,
              MANAGEMENT_TRANSPORT_HEADER_SIZE + 5U,
              &payload, &payload_length) == MANAGEMENT_TRANSPORT_OK &&
              payload == frame + MANAGEMENT_TRANSPORT_HEADER_SIZE &&
              payload_length == 5U,
          "valid response frame publishes exact payload view");
    for (offset = 0U; offset < 5U; ++offset) {
        if (((const unsigned char *)payload)[offset] !=
            (unsigned char)(offset * 5U + 11U)) {
            CHECK(false, "valid response payload bytes are preserved");
            return;
        }
    }
    CHECK(true, "valid response payload bytes are preserved");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "valid frame validation performs no allocation");
}

static void test_failure_statuses(void)
{
    const void *payload = NULL;
    size_t payload_length = 0U;

    reset_policy();
    write_valid_frame(frame, 0U,
                      MANAGEMENT_TRANSPORT_DIRECTION_REQUEST,
                      policy.port);
    payload = (const void *)(uintptr_t)0x1234U;
    payload_length = 0xDEADU;
    frame[0] = (unsigned char)(MANAGEMENT_TRANSPORT_VERSION + 1U);
    CHECK(management_transport_validate_frame(
              &policy, frame, MANAGEMENT_TRANSPORT_HEADER_SIZE,
              &payload, &payload_length) ==
              MANAGEMENT_TRANSPORT_BAD_VERSION &&
              payload == (const void *)(uintptr_t)0x1234U &&
              payload_length == 0xDEADU,
          "unknown transport version fails closed");

    write_valid_frame(frame, 0U,
                      MANAGEMENT_TRANSPORT_DIRECTION_REQUEST,
                      (uint16_t)(policy.port + 1U));
    payload = (const void *)(uintptr_t)0x1234U;
    payload_length = 0xDEADU;
    CHECK(management_transport_validate_frame(
              &policy, frame, MANAGEMENT_TRANSPORT_HEADER_SIZE,
              &payload, &payload_length) ==
              MANAGEMENT_TRANSPORT_BAD_PORT &&
              payload == (const void *)(uintptr_t)0x1234U &&
              payload_length == 0xDEADU,
          "wrong configured port fails closed");

    write_valid_frame(frame, 0U, 2U, policy.port);
    payload = (const void *)(uintptr_t)0x1234U;
    payload_length = 0xDEADU;
    CHECK(management_transport_validate_frame(
              &policy, frame, MANAGEMENT_TRANSPORT_HEADER_SIZE,
              &payload, &payload_length) ==
              MANAGEMENT_TRANSPORT_BAD_DIRECTION &&
              payload == (const void *)(uintptr_t)0x1234U &&
              payload_length == 0xDEADU,
          "unknown direction fails closed");

    write_valid_frame(frame, 0U,
                      MANAGEMENT_TRANSPORT_DIRECTION_REQUEST,
                      policy.port);
    frame[8] |= 0x04U;
    payload = (const void *)(uintptr_t)0x1234U;
    payload_length = 0xDEADU;
    CHECK(management_transport_validate_frame(
              &policy, frame, MANAGEMENT_TRANSPORT_HEADER_SIZE,
              &payload, &payload_length) ==
              MANAGEMENT_TRANSPORT_BAD_FLAGS &&
              payload == (const void *)(uintptr_t)0x1234U &&
              payload_length == 0xDEADU,
          "reserved transport flags fail closed");

    policy.payload_max = 8U;
    write_header(frame, MANAGEMENT_TRANSPORT_VERSION, policy.port,
                 9U, MANAGEMENT_TRANSPORT_DIRECTION_REQUEST);
    payload = (const void *)(uintptr_t)0x1234U;
    payload_length = 0xDEADU;
    CHECK(management_transport_validate_frame(
              &policy, frame, MANAGEMENT_TRANSPORT_HEADER_SIZE + 9U,
              &payload, &payload_length) ==
              MANAGEMENT_TRANSPORT_OVERSIZED &&
              payload == (const void *)(uintptr_t)0x1234U &&
              payload_length == 0xDEADU,
          "oversized declared payload fails closed");

    reset_policy();
    write_valid_frame(frame, 0U,
                      MANAGEMENT_TRANSPORT_DIRECTION_REQUEST,
                      policy.port);
    frame[6] = (unsigned char)(MANAGEMENT_TRANSPORT_PAYLOAD_MAX & 0xFFU);
    frame[7] = (unsigned char)(
        (MANAGEMENT_TRANSPORT_PAYLOAD_MAX >> 8U) & 0xFFU);
    payload = (const void *)(uintptr_t)0x1234U;
    payload_length = 0xDEADU;
    CHECK(management_transport_validate_frame(
              &policy, frame, MANAGEMENT_TRANSPORT_FRAME_MAX + 1U,
              &payload, &payload_length) ==
              MANAGEMENT_TRANSPORT_OVERSIZED &&
              payload == (const void *)(uintptr_t)0x1234U &&
              payload_length == 0xDEADU,
          "frame beyond transport maximum fails closed");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "failure-status validation performs no allocation");
}

static void test_partial_and_trailing_frames(void)
{
    const void *payload = NULL;
    size_t payload_length = 0U;

    reset_policy();
    write_valid_frame(frame, 0U,
                      MANAGEMENT_TRANSPORT_DIRECTION_REQUEST,
                      policy.port);
    payload = (const void *)(uintptr_t)0x1234U;
    payload_length = 0xDEADU;
    CHECK(management_transport_validate_frame(
              &policy, frame, MANAGEMENT_TRANSPORT_HEADER_SIZE - 1U,
              &payload, &payload_length) ==
              MANAGEMENT_TRANSPORT_PARTIAL &&
              payload == (const void *)(uintptr_t)0x1234U &&
              payload_length == 0xDEADU,
          "header-only partial frame fails closed");

    write_valid_frame(frame, 5U,
                      MANAGEMENT_TRANSPORT_DIRECTION_RESPONSE,
                      policy.port);
    payload = (const void *)(uintptr_t)0x1234U;
    payload_length = 0xDEADU;
    CHECK(management_transport_validate_frame(
              &policy, frame, MANAGEMENT_TRANSPORT_HEADER_SIZE + 4U,
              &payload, &payload_length) ==
              MANAGEMENT_TRANSPORT_PARTIAL &&
              payload == (const void *)(uintptr_t)0x1234U &&
              payload_length == 0xDEADU,
          "truncated declared payload fails closed");

    write_valid_frame(frame, 5U,
                      MANAGEMENT_TRANSPORT_DIRECTION_REQUEST,
                      policy.port);
    payload = (const void *)(uintptr_t)0x1234U;
    payload_length = 0xDEADU;
    CHECK(management_transport_validate_frame(
              &policy, frame, MANAGEMENT_TRANSPORT_HEADER_SIZE + 6U,
              &payload, &payload_length) ==
              MANAGEMENT_TRANSPORT_TRAILING_FIELDS &&
              payload == (const void *)(uintptr_t)0x1234U &&
              payload_length == 0xDEADU,
          "trailing bytes after declared payload fail closed");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "partial and trailing validation performs no allocation");
}

static void test_no_partial_publication_cycle(void)
{
    const void *payload = NULL;
    size_t payload_length = 0U;
    size_t offset;

    reset_policy();
    write_valid_frame(frame, 7U,
                      MANAGEMENT_TRANSPORT_DIRECTION_REQUEST,
                      policy.port);

    payload = NULL;
    payload_length = 0U;
    CHECK(management_transport_validate_frame(
              &policy, frame, MANAGEMENT_TRANSPORT_HEADER_SIZE + 7U,
              &payload, &payload_length) == MANAGEMENT_TRANSPORT_OK &&
              payload == frame + MANAGEMENT_TRANSPORT_HEADER_SIZE &&
              payload_length == 7U,
          "no-partial cycle starts with one valid frame");
    for (offset = 0U; offset < 7U; ++offset) {
        CHECK(((const unsigned char *)payload)[offset] ==
                  (unsigned char)(offset * 5U + 11U),
              "no-partial cycle preserves exact payload bytes");
    }

    payload = (const void *)(uintptr_t)0x1234U;
    payload_length = 0xDEADU;
    CHECK(management_transport_validate_frame(
              &policy, frame, MANAGEMENT_TRANSPORT_HEADER_SIZE - 1U,
              &payload, &payload_length) ==
              MANAGEMENT_TRANSPORT_PARTIAL &&
              payload == (const void *)(uintptr_t)0x1234U &&
              payload_length == 0xDEADU,
          "partial frame never publishes payload output");

    frame[0] = (unsigned char)(MANAGEMENT_TRANSPORT_VERSION + 1U);
    payload = (const void *)(uintptr_t)0x1234U;
    payload_length = 0xDEADU;
    CHECK(management_transport_validate_frame(
              &policy, frame, MANAGEMENT_TRANSPORT_HEADER_SIZE + 7U,
              &payload, &payload_length) ==
              MANAGEMENT_TRANSPORT_BAD_VERSION &&
              payload == (const void *)(uintptr_t)0x1234U &&
              payload_length == 0xDEADU,
          "unknown-version frame never publishes payload output");

    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "no-partial cycle performs no allocation");
}

int main(void)
{
    test_status_strings();
    test_policy_and_null_validation();
    test_valid_request_and_response_frames();
    test_failure_statuses();
    test_partial_and_trailing_frames();
    test_no_partial_publication_cycle();

    if (failures == 0) {
        printf("Management transport host tests: ALL OK\n");
    }
    return failures != 0;
}

#else
#error "This host test must be built with HFOC_MANAGEMENT_TRANSPORT_HOST_TEST"
#endif
