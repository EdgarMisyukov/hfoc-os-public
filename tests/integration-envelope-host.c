#include "integration_envelope.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifdef HFOC_INTEGRATION_ENVELOPE_HOST_TEST

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

static void store_le32(unsigned char *bytes, uint32_t value)
{
    bytes[0] = (unsigned char)(value);
    bytes[1] = (unsigned char)(value >> 8U);
    bytes[2] = (unsigned char)(value >> 16U);
    bytes[3] = (unsigned char)(value >> 24U);
}

static uint32_t load_le32(const unsigned char *bytes)
{
    return (uint32_t)bytes[0] |
           ((uint32_t)bytes[1] << 8U) |
           ((uint32_t)bytes[2] << 16U) |
           ((uint32_t)bytes[3] << 24U);
}

static void fill_correlation_id(uint8_t *correlation_id)
{
    uint32_t index;

    for (index = 0U; index < INTEGRATION_ENVELOPE_CORRELATION_ID_SIZE;
         ++index) {
        correlation_id[index] = (uint8_t)(0x41U + index);
    }
}

static size_t make_valid_envelope(unsigned char *buffer, size_t capacity)
{
    static const unsigned char payload[] = "audit hand-off payload";
    uint8_t correlation_id[INTEGRATION_ENVELOPE_CORRELATION_ID_SIZE];
    size_t length = 0U;

    fill_correlation_id(correlation_id);
    CHECK(integration_envelope_seal(
              INTEGRATION_ENVELOPE_SCHEMA_VERSION_MIN,
              INTEGRATION_ENVELOPE_PURPOSE_AUDIT_HANDOFF,
              correlation_id, payload, sizeof(payload),
              buffer, capacity, &length) == INTEGRATION_ENVELOPE_OK &&
              length == INTEGRATION_ENVELOPE_HEADER_SIZE +
                  sizeof(payload) + INTEGRATION_ENVELOPE_DIGEST_SIZE,
          "envelope fixture seals a valid envelope");
    return length;
}

static void check_seal_failure(
    uint32_t schema_version,
    uint32_t purpose,
    const uint8_t *correlation_id,
    const void *payload,
    size_t payload_length,
    size_t capacity,
    enum integration_envelope_status expected,
    const char *message)
{
    unsigned char buffer[5120];
    unsigned char before[sizeof(buffer)];
    size_t length = 0x5A5A5A5A5A5A5A5AULL;

    memset(buffer, 0xA5, sizeof(buffer));
    memcpy(before, buffer, sizeof(before));
    CHECK(integration_envelope_seal(
              schema_version, purpose, correlation_id, payload,
              payload_length, buffer, capacity, &length) == expected &&
              length == 0x5A5A5A5A5A5A5A5AULL &&
              memcmp(buffer, before, sizeof(buffer)) == 0,
          message);
}

static void check_validate_failure(
    const unsigned char *envelope,
    size_t envelope_length,
    enum integration_envelope_status expected,
    const char *message)
{
    struct integration_envelope_layout layout;
    unsigned char before[sizeof(layout)];

    memset(&layout, 0x5A, sizeof(layout));
    memcpy(before, &layout, sizeof(layout));
    CHECK(integration_envelope_validate(
              envelope, envelope_length, &layout) == expected,
          message);
    CHECK(memcmp(&layout, before, sizeof(layout)) == 0,
          "failed envelope validation leaves layout unchanged");
}

static void test_exact_constants(void)
{
    static const unsigned char expected_id[INTEGRATION_ENVELOPE_ID_SIZE] =
        INTEGRATION_ENVELOPE_ID_BYTES;
    static const unsigned char id[INTEGRATION_ENVELOPE_ID_SIZE] = {
        'H', 'F', 'O', 'C', 'A', 'C', 'C', 'T',
        'H', 'A', 'N', 'D', 'O', 'F', 'F', '1',
    };

    CHECK(INTEGRATION_ENVELOPE_VERSION == 1U,
          "envelope version is exactly 1");
    CHECK(INTEGRATION_ENVELOPE_SCHEMA_VERSION_MIN ==
              INTEGRATION_BOUNDARY_SCHEMA_VERSION_MIN &&
              INTEGRATION_ENVELOPE_SCHEMA_VERSION_MIN == 1U,
          "envelope schema minimum is the boundary minimum");
    CHECK(INTEGRATION_ENVELOPE_SCHEMA_VERSION_MAX ==
              INTEGRATION_BOUNDARY_SCHEMA_VERSION_MAX &&
              INTEGRATION_ENVELOPE_SCHEMA_VERSION_MAX == 1U,
          "envelope schema maximum is the boundary maximum");
    CHECK(INTEGRATION_ENVELOPE_ID_SIZE == 16U &&
              memcmp(expected_id, id, sizeof(id)) == 0,
          "envelope ID is exact HFOCACCTHANDOFF1");
    CHECK(INTEGRATION_ENVELOPE_CORRELATION_ID_SIZE == 32U,
          "envelope correlation ID is exactly 32 bytes");
    CHECK(INTEGRATION_ENVELOPE_DIGEST_SIZE == SHA256_DIGEST_SIZE &&
              INTEGRATION_ENVELOPE_DIGEST_SIZE == 32U,
          "envelope digest is exactly 32 bytes");
    CHECK(INTEGRATION_ENVELOPE_HEADER_SIZE == 88U,
          "envelope header is exactly 88 bytes");
    CHECK(INTEGRATION_ENVELOPE_RESERVED_SIZE == 24U,
          "envelope reserved range is exactly 24 bytes");
    CHECK(INTEGRATION_ENVELOPE_PAYLOAD_LENGTH_MIN == 1U &&
              INTEGRATION_ENVELOPE_PAYLOAD_LENGTH_MAX == 4096U,
          "envelope payload bounds are exactly 1..4096");
    CHECK(INTEGRATION_ENVELOPE_MIN_ENVELOPE_LENGTH == 120U &&
              INTEGRATION_ENVELOPE_MAX_ENVELOPE_LENGTH == 4216U,
          "envelope length bounds are exact");
    CHECK(INTEGRATION_ENVELOPE_PURPOSE_MIN == 1U &&
              INTEGRATION_ENVELOPE_PURPOSE_AUDIT_HANDOFF == 1U &&
              INTEGRATION_ENVELOPE_PURPOSE_MAX == 1U,
          "envelope audit hand-off purpose is exactly 1");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "envelope constant checking performs no allocation");
}

static void test_seal_failures(void)
{
    static const unsigned char payload[] = "payload";
    uint8_t correlation_id[INTEGRATION_ENVELOPE_CORRELATION_ID_SIZE];
    uint8_t zero_correlation_id[INTEGRATION_ENVELOPE_CORRELATION_ID_SIZE];
    size_t null_length = 0x5A5A5A5A5A5A5A5AULL;

    fill_correlation_id(correlation_id);
    memset(zero_correlation_id, 0, sizeof(zero_correlation_id));
    CHECK(integration_envelope_seal(
              INTEGRATION_ENVELOPE_SCHEMA_VERSION_MIN,
              INTEGRATION_ENVELOPE_PURPOSE_AUDIT_HANDOFF,
              correlation_id, payload, sizeof(payload),
              NULL, 0U, &null_length) == INTEGRATION_ENVELOPE_NULL &&
              null_length == 0x5A5A5A5A5A5A5A5AULL,
          "envelope seal rejects null output buffer");
    check_seal_failure(
        INTEGRATION_ENVELOPE_SCHEMA_VERSION_MIN,
        INTEGRATION_ENVELOPE_PURPOSE_AUDIT_HANDOFF,
        correlation_id, payload, sizeof(payload), 0U,
        INTEGRATION_ENVELOPE_BUFFER_TOO_SMALL,
        "envelope seal rejects zero-capacity output buffer");
    check_seal_failure(
        INTEGRATION_ENVELOPE_SCHEMA_VERSION_MIN,
        INTEGRATION_ENVELOPE_PURPOSE_AUDIT_HANDOFF,
        NULL, payload, sizeof(payload), sizeof(payload),
        INTEGRATION_ENVELOPE_NULL,
        "envelope seal rejects null correlation ID");
    check_seal_failure(
        INTEGRATION_ENVELOPE_SCHEMA_VERSION_MIN,
        INTEGRATION_ENVELOPE_PURPOSE_AUDIT_HANDOFF,
        correlation_id, NULL, sizeof(payload), sizeof(payload),
        INTEGRATION_ENVELOPE_NULL,
        "envelope seal rejects null payload");
    check_seal_failure(
        INTEGRATION_ENVELOPE_SCHEMA_VERSION_MIN - 1U,
        INTEGRATION_ENVELOPE_PURPOSE_AUDIT_HANDOFF,
        correlation_id, payload, sizeof(payload), sizeof(payload),
        INTEGRATION_ENVELOPE_UNSUPPORTED_SCHEMA_VERSION,
        "envelope seal rejects low schema version");
    check_seal_failure(
        INTEGRATION_ENVELOPE_SCHEMA_VERSION_MAX + 1U,
        INTEGRATION_ENVELOPE_PURPOSE_AUDIT_HANDOFF,
        correlation_id, payload, sizeof(payload), sizeof(payload),
        INTEGRATION_ENVELOPE_UNSUPPORTED_SCHEMA_VERSION,
        "envelope seal rejects high schema version");
    check_seal_failure(
        INTEGRATION_ENVELOPE_SCHEMA_VERSION_MIN,
        INTEGRATION_ENVELOPE_PURPOSE_AUDIT_HANDOFF - 1U,
        correlation_id, payload, sizeof(payload), sizeof(payload),
        INTEGRATION_ENVELOPE_INVALID_PURPOSE,
        "envelope seal rejects low purpose");
    check_seal_failure(
        INTEGRATION_ENVELOPE_SCHEMA_VERSION_MIN,
        INTEGRATION_ENVELOPE_PURPOSE_MAX + 1U,
        correlation_id, payload, sizeof(payload), sizeof(payload),
        INTEGRATION_ENVELOPE_INVALID_PURPOSE,
        "envelope seal rejects high purpose");
    check_seal_failure(
        INTEGRATION_ENVELOPE_SCHEMA_VERSION_MIN,
        INTEGRATION_ENVELOPE_PURPOSE_AUDIT_HANDOFF,
        zero_correlation_id, payload, sizeof(payload), sizeof(payload),
        INTEGRATION_ENVELOPE_INVALID_CORRELATION_ID,
        "envelope seal rejects zero correlation ID");
    check_seal_failure(
        INTEGRATION_ENVELOPE_SCHEMA_VERSION_MIN,
        INTEGRATION_ENVELOPE_PURPOSE_AUDIT_HANDOFF,
        correlation_id, payload, 0U, sizeof(payload),
        INTEGRATION_ENVELOPE_INVALID_PAYLOAD_LENGTH,
        "envelope seal rejects empty payload");
    check_seal_failure(
        INTEGRATION_ENVELOPE_SCHEMA_VERSION_MIN,
        INTEGRATION_ENVELOPE_PURPOSE_AUDIT_HANDOFF,
        correlation_id, payload,
        INTEGRATION_ENVELOPE_PAYLOAD_LENGTH_MAX + 1U, sizeof(payload),
        INTEGRATION_ENVELOPE_INVALID_PAYLOAD_LENGTH,
        "envelope seal rejects over-limit payload");
    check_seal_failure(
        INTEGRATION_ENVELOPE_SCHEMA_VERSION_MIN,
        INTEGRATION_ENVELOPE_PURPOSE_AUDIT_HANDOFF,
        correlation_id, payload, sizeof(payload),
        INTEGRATION_ENVELOPE_HEADER_SIZE + sizeof(payload) +
            INTEGRATION_ENVELOPE_DIGEST_SIZE - 1U,
        INTEGRATION_ENVELOPE_BUFFER_TOO_SMALL,
        "envelope seal rejects too-small output buffer");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "envelope seal failures perform no allocation");
}

static void test_seal_and_validate_success(void)
{
    static const unsigned char payload[] = "audit hand-off payload";
    static const unsigned char expected_id[INTEGRATION_ENVELOPE_ID_SIZE] =
        INTEGRATION_ENVELOPE_ID_BYTES;
    unsigned char envelope[5120];
    unsigned char second[5120];
    uint8_t correlation_id[INTEGRATION_ENVELOPE_CORRELATION_ID_SIZE];
    struct integration_envelope_layout layout;
    size_t length;
    size_t second_length;
    size_t digest_offset;
    size_t index;
    unsigned char digest[INTEGRATION_ENVELOPE_DIGEST_SIZE];

    fill_correlation_id(correlation_id);
    length = make_valid_envelope(envelope, sizeof(envelope));
    CHECK(memcmp(envelope, expected_id, sizeof(expected_id)) == 0,
          "envelope seal writes exact envelope ID");
    CHECK(envelope[16U] == INTEGRATION_ENVELOPE_VERSION,
          "envelope seal writes exact envelope version");
    CHECK(load_le32(envelope + 20U) ==
              INTEGRATION_ENVELOPE_SCHEMA_VERSION_MIN,
          "envelope seal writes exact schema version");
    CHECK(load_le32(envelope + 24U) ==
              INTEGRATION_ENVELOPE_PURPOSE_AUDIT_HANDOFF,
          "envelope seal writes exact audit hand-off purpose");
    CHECK(load_le32(envelope + 28U) == sizeof(payload),
          "envelope seal writes exact payload length");
    CHECK(memcmp(envelope + 32U, correlation_id,
                 INTEGRATION_ENVELOPE_CORRELATION_ID_SIZE) == 0,
          "envelope seal writes exact correlation ID");
    for (index = 64U; index < INTEGRATION_ENVELOPE_HEADER_SIZE; ++index) {
        CHECK(envelope[index] == 0U,
              "envelope seal zeroes every reserved header byte");
    }
    CHECK(memcmp(envelope + INTEGRATION_ENVELOPE_HEADER_SIZE,
                 payload, sizeof(payload)) == 0,
          "envelope seal copies exact payload");
    digest_offset = INTEGRATION_ENVELOPE_HEADER_SIZE + sizeof(payload);
    CHECK(length == digest_offset + INTEGRATION_ENVELOPE_DIGEST_SIZE,
          "envelope seal places digest after payload");
    CHECK(sha256_compute(envelope, digest_offset, digest) == SHA256_OK &&
              memcmp(envelope + digest_offset, digest,
                     INTEGRATION_ENVELOPE_DIGEST_SIZE) == 0,
          "envelope seal writes matching SHA-256 digest");

    second_length = make_valid_envelope(second, sizeof(second));
    CHECK(second_length == length &&
              memcmp(second, envelope, length) == 0,
          "identical envelope inputs produce byte-identical output");

    memset(&layout, 0x5A, sizeof(layout));
    CHECK(integration_envelope_validate(
              envelope, length, &layout) == INTEGRATION_ENVELOPE_OK &&
              layout.valid &&
              layout.envelope_version == INTEGRATION_ENVELOPE_VERSION &&
              layout.schema_version ==
                  INTEGRATION_ENVELOPE_SCHEMA_VERSION_MIN &&
              layout.purpose ==
                  INTEGRATION_ENVELOPE_PURPOSE_AUDIT_HANDOFF &&
              layout.payload_length == sizeof(payload) &&
              layout.payload_offset == INTEGRATION_ENVELOPE_HEADER_SIZE &&
              layout.digest_offset == digest_offset &&
              layout.envelope_length == length,
          "envelope validation publishes exact layout");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "envelope seal and validation perform no allocation");
}

static void test_malformed_envelope_rejection(void)
{
    unsigned char envelope[5120];
    unsigned char oversized[INTEGRATION_ENVELOPE_MAX_ENVELOPE_LENGTH + 1U];
    size_t length = make_valid_envelope(envelope, sizeof(envelope));
    size_t digest_offset = length - INTEGRATION_ENVELOPE_DIGEST_SIZE;

    memset(oversized, 0, sizeof(oversized));
    check_validate_failure(NULL, length, INTEGRATION_ENVELOPE_NULL,
                           "envelope validation rejects null envelope");
    CHECK(integration_envelope_validate(
              envelope, length, NULL) == INTEGRATION_ENVELOPE_NULL,
          "envelope validation rejects null layout");
    check_validate_failure(envelope, 0U, INTEGRATION_ENVELOPE_TOO_SHORT,
                           "empty envelope is too short");
    check_validate_failure(
        envelope, INTEGRATION_ENVELOPE_MIN_ENVELOPE_LENGTH - 1U,
        INTEGRATION_ENVELOPE_TOO_SHORT,
        "truncated header envelope is too short");
    check_validate_failure(
        oversized, sizeof(oversized), INTEGRATION_ENVELOPE_TOO_LONG,
        "over-limit envelope is too long");

    memcpy(envelope + 0U, "BADIDBADIDBADID", 16U);
    check_validate_failure(envelope, length, INTEGRATION_ENVELOPE_BAD_ID,
                           "bad envelope ID is rejected");
    length = make_valid_envelope(envelope, sizeof(envelope));
    envelope[16U] = INTEGRATION_ENVELOPE_VERSION + 1U;
    check_validate_failure(
        envelope, length,
        INTEGRATION_ENVELOPE_UNSUPPORTED_ENVELOPE_VERSION,
        "unsupported envelope version is rejected");

    length = make_valid_envelope(envelope, sizeof(envelope));
    store_le32(envelope + 20U, INTEGRATION_ENVELOPE_SCHEMA_VERSION_MIN - 1U);
    check_validate_failure(
        envelope, length, INTEGRATION_ENVELOPE_UNSUPPORTED_SCHEMA_VERSION,
        "low schema version is rejected");
    length = make_valid_envelope(envelope, sizeof(envelope));
    store_le32(envelope + 20U, INTEGRATION_ENVELOPE_SCHEMA_VERSION_MAX + 1U);
    check_validate_failure(
        envelope, length, INTEGRATION_ENVELOPE_UNSUPPORTED_SCHEMA_VERSION,
        "high schema version is rejected");

    length = make_valid_envelope(envelope, sizeof(envelope));
    store_le32(envelope + 24U, INTEGRATION_ENVELOPE_PURPOSE_MIN - 1U);
    check_validate_failure(envelope, length,
                           INTEGRATION_ENVELOPE_INVALID_PURPOSE,
                           "low purpose is rejected");
    length = make_valid_envelope(envelope, sizeof(envelope));
    store_le32(envelope + 24U, INTEGRATION_ENVELOPE_PURPOSE_MAX + 1U);
    check_validate_failure(envelope, length,
                           INTEGRATION_ENVELOPE_INVALID_PURPOSE,
                           "high purpose is rejected");

    length = make_valid_envelope(envelope, sizeof(envelope));
    envelope[64U] = 1U;
    check_validate_failure(envelope, length,
                           INTEGRATION_ENVELOPE_CORRUPT_RESERVED,
                           "non-zero reserved byte is rejected");

    length = make_valid_envelope(envelope, sizeof(envelope));
    store_le32(envelope + 28U, 0U);
    check_validate_failure(
        envelope, length, INTEGRATION_ENVELOPE_INVALID_PAYLOAD_LENGTH,
        "zero payload length is rejected");
    length = make_valid_envelope(envelope, sizeof(envelope));
    store_le32(envelope + 28U,
               INTEGRATION_ENVELOPE_PAYLOAD_LENGTH_MAX + 1U);
    check_validate_failure(
        envelope, length, INTEGRATION_ENVELOPE_INVALID_PAYLOAD_LENGTH,
        "over-limit payload length is rejected");

    length = make_valid_envelope(envelope, sizeof(envelope));
    memset(envelope + 32U, 0, INTEGRATION_ENVELOPE_CORRELATION_ID_SIZE);
    check_validate_failure(
        envelope, length, INTEGRATION_ENVELOPE_INVALID_CORRELATION_ID,
        "zero correlation ID is rejected");

    length = make_valid_envelope(envelope, sizeof(envelope));
    envelope[INTEGRATION_ENVELOPE_HEADER_SIZE] ^= 0xFFU;
    check_validate_failure(envelope, length, INTEGRATION_ENVELOPE_BAD_DIGEST,
                           "digest mismatch is rejected");

    length = make_valid_envelope(envelope, sizeof(envelope));
    check_validate_failure(
        envelope, length - 1U,
        INTEGRATION_ENVELOPE_INVALID_PAYLOAD_LENGTH,
        "truncated payload length is rejected");
    length = make_valid_envelope(envelope, sizeof(envelope));
    check_validate_failure(
        envelope, length + 1U,
        INTEGRATION_ENVELOPE_INVALID_PAYLOAD_LENGTH,
        "trailing byte length is rejected");

    (void)digest_offset;
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "malformed envelope rejection performs no allocation");
}

static const char *expected_status_string(
    enum integration_envelope_status status)
{
    switch (status) {
    case INTEGRATION_ENVELOPE_OK:
        return "INTEGRATION_ENVELOPE_OK";
    case INTEGRATION_ENVELOPE_NULL:
        return "INTEGRATION_ENVELOPE_NULL";
    case INTEGRATION_ENVELOPE_TOO_SHORT:
        return "INTEGRATION_ENVELOPE_TOO_SHORT";
    case INTEGRATION_ENVELOPE_TOO_LONG:
        return "INTEGRATION_ENVELOPE_TOO_LONG";
    case INTEGRATION_ENVELOPE_BAD_ID:
        return "INTEGRATION_ENVELOPE_BAD_ID";
    case INTEGRATION_ENVELOPE_UNSUPPORTED_ENVELOPE_VERSION:
        return "INTEGRATION_ENVELOPE_UNSUPPORTED_ENVELOPE_VERSION";
    case INTEGRATION_ENVELOPE_UNSUPPORTED_SCHEMA_VERSION:
        return "INTEGRATION_ENVELOPE_UNSUPPORTED_SCHEMA_VERSION";
    case INTEGRATION_ENVELOPE_INVALID_PURPOSE:
        return "INTEGRATION_ENVELOPE_INVALID_PURPOSE";
    case INTEGRATION_ENVELOPE_INVALID_CORRELATION_ID:
        return "INTEGRATION_ENVELOPE_INVALID_CORRELATION_ID";
    case INTEGRATION_ENVELOPE_INVALID_PAYLOAD_LENGTH:
        return "INTEGRATION_ENVELOPE_INVALID_PAYLOAD_LENGTH";
    case INTEGRATION_ENVELOPE_CORRUPT_RESERVED:
        return "INTEGRATION_ENVELOPE_CORRUPT_RESERVED";
    case INTEGRATION_ENVELOPE_BAD_DIGEST:
        return "INTEGRATION_ENVELOPE_BAD_DIGEST";
    case INTEGRATION_ENVELOPE_BUFFER_TOO_SMALL:
        return "INTEGRATION_ENVELOPE_BUFFER_TOO_SMALL";
    case INTEGRATION_ENVELOPE_BAD_STATE:
        return "INTEGRATION_ENVELOPE_BAD_STATE";
    default:
        return NULL;
    }
}

static void test_status_strings(void)
{
    enum integration_envelope_status status;

    for (status = INTEGRATION_ENVELOPE_OK;
         status <= INTEGRATION_ENVELOPE_BAD_STATE; ++status) {
        const char *string = integration_envelope_status_string(status);
        const char *expected = expected_status_string(status);

        CHECK(string != NULL && expected != NULL &&
                  strcmp(string, expected) == 0,
              "envelope status string is exact");
    }
    CHECK(integration_envelope_status_string(
              (enum integration_envelope_status)14U) == NULL,
          "undefined envelope status is rejected");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "envelope status handling performs no allocation");
}

int main(void)
{
    test_exact_constants();
    test_seal_failures();
    test_seal_and_validate_success();
    test_malformed_envelope_rejection();
    test_status_strings();

    if (failures != 0) {
        fprintf(stderr, "Integration envelope host tests: FAILED (%d)\n",
                failures);
        return 1;
    }
    printf("Integration envelope host tests: ALL OK\n");
    return 0;
}

#endif
