#include "pem_decode.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static int failures;

#define CHECK(condition, message) do { \
    if (!(condition)) { \
        fprintf(stderr, "FAIL: %s\n", message); \
        ++failures; \
    } else { \
        printf("PASS: %s\n", message); \
    } \
} while (0)

/* base64("hello") */
static const char hello_pem[] =
    "-----BEGIN CERTIFICATE-----\n"
    "aGVsbG8=\n"
    "-----END CERTIFICATE-----\n";

static void test_decodes_a_single_block(void)
{
    unsigned char output[32U];
    size_t length = 0U;

    CHECK(pem_decode_block(
              hello_pem, sizeof(hello_pem) - 1U, "CERTIFICATE",
              output, sizeof(output), &length) == PEM_DECODE_OK &&
              length == 5U &&
              memcmp(output, "hello", 5U) == 0,
          "a single PEM block decodes to its bytes");
}

static void test_decodes_a_multi_line_block(void)
{
    /* base64("The quick brown fox") split over three lines. */
    static const char text[] =
        "junk before\n"
        "-----BEGIN PRIVATE KEY-----\n"
        "VGhlIHF1aWNr\n"
        "IGJyb3duIGZv\n"
        "eA==\n"
        "-----END PRIVATE KEY-----\n"
        "junk after\n";
    unsigned char output[64U];
    size_t length = 0U;

    CHECK(pem_decode_block(
              text, sizeof(text) - 1U, "PRIVATE KEY",
              output, sizeof(output), &length) == PEM_DECODE_OK &&
              length == 19U &&
              memcmp(output, "The quick brown fox", 19U) == 0,
          "a multi-line block decodes and surrounding text is ignored");
}

static void test_selects_the_named_block(void)
{
    static const char text[] =
        "-----BEGIN CERTIFICATE-----\n"
        "aGVsbG8=\n"
        "-----END CERTIFICATE-----\n"
        "-----BEGIN PRIVATE KEY-----\n"
        "d29ybGQ=\n"
        "-----END PRIVATE KEY-----\n";
    unsigned char output[32U];
    size_t length = 0U;

    CHECK(pem_decode_block(
              text, sizeof(text) - 1U, "PRIVATE KEY",
              output, sizeof(output), &length) == PEM_DECODE_OK &&
              length == 5U &&
              memcmp(output, "world", 5U) == 0,
          "the block with the requested label is the one decoded");
}

static void test_decodes_a_crlf_block_and_a_full_length_label(void)
{
    /* base64("hello") with CRLF line endings, as a file written elsewhere. */
    static const char crlf[] =
        "-----BEGIN CERTIFICATE-----\r\n"
        "aGVsbG8=\r\n"
        "-----END CERTIFICATE-----\r\n";
    /* ITEM-503 follow-up: a 47-byte label is legal and exactly fills the
     * marker buffer; the bound check must not refuse it. */
    static const char label[48U] =
        "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA";
    char text[256U];
    unsigned char output[32U];
    size_t length = 0U;
    int written;

    CHECK(pem_decode_block(
              crlf, sizeof(crlf) - 1U, "CERTIFICATE",
              output, sizeof(output), &length) == PEM_DECODE_OK &&
              length == 5U &&
              memcmp(output, "hello", 5U) == 0,
          "a CRLF document decodes like an LF one");
    written = snprintf(text, sizeof(text),
                       "-----BEGIN %s-----\n"
                       "aGVsbG8=\n"
                       "-----END %s-----\n",
                       label, label);
    CHECK(written > 0 && (size_t)written < sizeof(text) &&
              pem_decode_block(
                  text, (size_t)written, label,
                  output, sizeof(output), &length) == PEM_DECODE_OK &&
              length == 5U &&
              memcmp(output, "hello", 5U) == 0,
          "a label of the maximum legal length is accepted");
}

static void test_refuses_malformed_documents(void)
{
    unsigned char output[32U];
    size_t length = 0U;

    CHECK(pem_decode_block(
              "-----BEGIN CERTIFICATE-----\n"
              "aGVsbG8=\n",
              sizeof("-----BEGIN CERTIFICATE-----\n"
                     "aGVsbG8=\n") - 1U,
              "CERTIFICATE", output, sizeof(output), &length) ==
              PEM_DECODE_BAD_ARMOUR,
          "a block that never closes is refused");
    CHECK(pem_decode_block(
              "-----BEGIN CERTIFICATE-----\n"
              "aGVsbG8\n"
              "-----END CERTIFICATE-----\n",
              sizeof("-----BEGIN CERTIFICATE-----\n"
                     "aGVsbG8\n"
                     "-----END CERTIFICATE-----\n") - 1U,
              "CERTIFICATE", output, sizeof(output), &length) ==
              PEM_DECODE_BAD_BASE64,
          "a body that is not a multiple of four is refused");
    CHECK(pem_decode_block(
              "-----BEGIN CERTIFICATE-----\n"
              "aG=sbG8=\n"
              "-----END CERTIFICATE-----\n",
              sizeof("-----BEGIN CERTIFICATE-----\n"
                     "aG=sbG8=\n"
                     "-----END CERTIFICATE-----\n") - 1U,
              "CERTIFICATE", output, sizeof(output), &length) ==
              PEM_DECODE_BAD_BASE64,
          "padding in the middle of the body is refused");
    CHECK(pem_decode_block(
              hello_pem, sizeof(hello_pem) - 1U, "PRIVATE KEY",
              output, sizeof(output), &length) == PEM_DECODE_NO_BLOCK,
          "a missing label is no block at all");
    CHECK(pem_decode_block(
              hello_pem, sizeof(hello_pem) - 1U, "CERTIFICATE\\nX",
              output, sizeof(output), &length) == PEM_DECODE_BAD_ARMOUR,
          "a label with illegal characters is refused");
    CHECK(pem_decode_block(
              NULL, 0U, "CERTIFICATE", output, sizeof(output),
              &length) == PEM_DECODE_NULL &&
              pem_decode_block(
                  hello_pem, sizeof(hello_pem) - 1U, "CERTIFICATE",
                  NULL, sizeof(output), &length) == PEM_DECODE_NULL,
          "null inputs are refused");
    CHECK(pem_decode_block(
              hello_pem, sizeof(hello_pem) - 1U, "CERTIFICATE",
              NULL, 0U, &length) == PEM_DECODE_TOO_LARGE,
          "a non-empty block with no room is too large");
    CHECK(pem_decode_block(
              "-----BEGIN CERTIFICATE-----\n"
              "-----END CERTIFICATE-----\n",
              sizeof("-----BEGIN CERTIFICATE-----\n"
                     "-----END CERTIFICATE-----\n") - 1U,
              "CERTIFICATE", output, sizeof(output), &length) ==
              PEM_DECODE_BAD_BASE64,
          "an empty body is refused, not decoded to zero bytes");
    CHECK(pem_decode_block(
              "-----BEGIN CERTIFICATE-----\n"
              "-----BEGIN CERTIFICATE-----\n"
              "aGVsbG8=\n"
              "-----END CERTIFICATE-----\n",
              sizeof("-----BEGIN CERTIFICATE-----\n"
                     "-----BEGIN CERTIFICATE-----\n"
                     "aGVsbG8=\n"
                     "-----END CERTIFICATE-----\n") - 1U,
              "CERTIFICATE", output, sizeof(output), &length) ==
              PEM_DECODE_BAD_ARMOUR,
          "a second armour line inside the block is refused");
}

static void test_refuses_an_output_that_does_not_fit(void)
{
    unsigned char output[4U];
    size_t length = 0U;

    CHECK(pem_decode_block(
              hello_pem, sizeof(hello_pem) - 1U, "CERTIFICATE",
              output, sizeof(output), &length) ==
              PEM_DECODE_TOO_LARGE &&
              length == 0U,
          "an output larger than the buffer is refused, never truncated");
}

static void test_round_trips_a_long_body(void)
{
    /* 96 bytes encoded as 128 base64 characters over two lines. */
    static const char text[] =
        "-----BEGIN CERTIFICATE-----\n"
        "AAECAwQFBgcICQoLDA0ODxAREhMUFRYXGBkaGxwdHh8gISIjJCUmJygpKissLS4v\n"
        "MDEyMzQ1Njc4OTo7PD0+P0BBQkNERUZHSElKS0xNTk9QUVJTVFVWV1hZWltcXV5f\n"
        "-----END CERTIFICATE-----\n";
    unsigned char output[128U];
    size_t length = 0U;
    size_t index;
    bool exact = true;

    CHECK(pem_decode_block(
              text, sizeof(text) - 1U, "CERTIFICATE",
              output, sizeof(output), &length) == PEM_DECODE_OK &&
              length == 96U,
          "a 96-byte body decodes to 96 bytes");
    for (index = 0U; index < length && index < 96U; ++index) {
        if (output[index] != (unsigned char)index) {
            exact = false;
        }
    }
    CHECK(exact, "the decoded bytes are the ones that were encoded");
}

int main(void)
{
    test_decodes_a_single_block();
    test_decodes_a_multi_line_block();
    test_selects_the_named_block();
    test_decodes_a_crlf_block_and_a_full_length_label();
    test_refuses_malformed_documents();
    test_refuses_an_output_that_does_not_fit();
    test_round_trips_a_long_body();

    if (failures != 0) {
        fprintf(stderr, "PEM decode host tests: %d FAILURES\n", failures);
        return 1;
    }
    puts("PEM decode host tests: ALL OK");
    return 0;
}
