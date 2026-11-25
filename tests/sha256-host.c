#include "sha256.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifdef HFOC_SHA256_HOST_TEST

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

static void digest_to_hex(const uint8_t digest[SHA256_DIGEST_SIZE],
                          char hex[2U * SHA256_DIGEST_SIZE + 1U])
{
    uint32_t index;

    for (index = 0U; index < SHA256_DIGEST_SIZE; ++index) {
        snprintf(hex + 2U * index, 3U, "%02x", digest[index]);
    }
    hex[2U * SHA256_DIGEST_SIZE] = '\0';
}

static void check_digest(const uint8_t *data,
                         size_t data_length,
                         const char *expected_hex,
                         const char *label)
{
    uint8_t digest[SHA256_DIGEST_SIZE];
    char hex[2U * SHA256_DIGEST_SIZE + 1U];

    if (sha256_compute(data, data_length, digest) != SHA256_OK) {
        fprintf(stderr, "FAIL: %s\n", label);
        ++failures;
        return;
    }
    digest_to_hex(digest, hex);
    if (strcmp(hex, expected_hex) != 0) {
        fprintf(stderr, "FAIL: %s (got %s, expected %s)\n",
                label, hex, expected_hex);
        ++failures;
        return;
    }
    printf("PASS: %s\n", label);
}

static void check_streaming_equivalent(const uint8_t *data,
                                       size_t data_length,
                                       const size_t *chunks,
                                       size_t chunk_count,
                                       const char *label)
{
    struct sha256 context;
    uint8_t one_shot[SHA256_DIGEST_SIZE];
    uint8_t streamed[SHA256_DIGEST_SIZE];
    size_t offset = 0U;
    size_t chunk_index;

    if (sha256_compute(data, data_length, one_shot) != SHA256_OK ||
        sha256_init(&context) != SHA256_OK) {
        fprintf(stderr, "FAIL: %s (setup)\n", label);
        ++failures;
        return;
    }
    for (chunk_index = 0U; chunk_index < chunk_count; ++chunk_index) {
        size_t chunk_length = chunks[chunk_index];

        if (chunk_length > data_length - offset) {
            chunk_length = data_length - offset;
        }
        if (sha256_update(&context, data + offset, chunk_length) !=
            SHA256_OK) {
            fprintf(stderr, "FAIL: %s (update %zu)\n",
                    label, chunk_index);
            ++failures;
            return;
        }
        offset += chunk_length;
    }
    if (offset != data_length) {
        fprintf(stderr, "FAIL: %s (short stream)\n", label);
        ++failures;
        return;
    }
    if (sha256_final(&context, streamed) != SHA256_OK ||
        memcmp(one_shot, streamed, SHA256_DIGEST_SIZE) != 0) {
        fprintf(stderr, "FAIL: %s\n", label);
        ++failures;
        return;
    }
    printf("PASS: %s\n", label);
}

static void test_validation_and_state(void) {
    struct sha256 context;
    uint8_t digest[SHA256_DIGEST_SIZE];

    memset(&context, 0xA5, sizeof(context));
    CHECK(sha256_init(NULL) == SHA256_NULL,
          "sha256 init rejects null context");
    CHECK(sha256_update(NULL, "abc", 3U) == SHA256_NULL,
          "sha256 update rejects null context");
    CHECK(sha256_update(&context, NULL, 3U) == SHA256_NULL,
          "sha256 update rejects null data");
    CHECK(sha256_final(NULL, digest) == SHA256_NULL,
          "sha256 final rejects null context");
    CHECK(sha256_final(&context, NULL) == SHA256_NULL,
          "sha256 final rejects null digest");
    CHECK(sha256_compute(NULL, 0U, digest) == SHA256_NULL,
          "sha256 compute rejects null data");
    CHECK(sha256_compute("abc", 3U, NULL) == SHA256_NULL,
          "sha256 compute rejects null digest");
    CHECK(sha256_update(&context, "abc", 3U) == SHA256_BAD_STATE,
          "unpublished context rejects update");
    CHECK(sha256_final(&context, digest) == SHA256_BAD_STATE,
          "unpublished context rejects final");

    CHECK(sha256_init(&context) == SHA256_OK &&
              context.initialized &&
              context.total_bytes == 0U &&
              context.buffer_used == 0U,
          "sha256 init publishes empty context");
    CHECK(sha256_final(&context, digest) == SHA256_OK &&
              !context.initialized,
          "sha256 final publishes digest and invalidates context");
    CHECK(sha256_update(&context, "x", 1U) == SHA256_BAD_STATE,
          "finalized context rejects update");
    CHECK(sha256_final(&context, digest) == SHA256_BAD_STATE,
          "finalized context rejects second final");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "sha256 validation performs no allocation");
}

static void test_known_answers(void) {
    static char million_a[1000000U];
    static const char nist_56[] =
        "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
    uint32_t index;

    for (index = 0U; index < sizeof(million_a); ++index) {
        million_a[index] = 'a';
    }
    check_digest((const uint8_t *)"", 0U,
                 "e3b0c44298fc1c149afbf4c8996fb924"
                 "27ae41e4649b934ca495991b7852b855",
                 "sha256 known-answer empty input");
    check_digest((const uint8_t *)"abc", 3U,
                 "ba7816bf8f01cfea414140de5dae2223"
                 "b00361a396177a9cb410ff61f20015ad",
                 "sha256 known-answer abc");
    check_digest((const uint8_t *)nist_56, sizeof(nist_56) - 1U,
                 "248d6a61d20638b8e5c026930c3e6039"
                 "a33ce45964ff2167f6ecedd419db06c1",
                 "sha256 known-answer 56-byte NIST vector");
    check_digest((const uint8_t *)million_a, sizeof(million_a),
                 "cdc76e5c9914fb9281a1c7e284d73e67"
                 "f1809a48a497200e046d39ccc7112cd0",
                 "sha256 known-answer one million a bytes");
}

static void test_streaming_equivalence(void) {
    static const size_t byte_chunks[3U] = {1U, 1U, 1U};
    static const size_t varied_chunks[6U] = {1U, 63U, 1U, 64U, 127U, 1U};
    static const char nist_56[] =
        "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";

    check_streaming_equivalent((const uint8_t *)"abc", 3U,
                               byte_chunks, 3U,
                               "sha256 byte-at-a-time streaming matches one-shot");
    check_streaming_equivalent(
        (const uint8_t *)nist_56, sizeof(nist_56) - 1U,
        varied_chunks, 6U,
        "sha256 varied-size streaming matches one-shot");
}

/*
 * CI-SPEEDUP-4: the credential KDF keeps a prepared context between messages,
 * so it absorbs the pad block on purpose and copies the context - counter
 * included - per round.  The seam must be exactly equivalent to streaming the
 * same bytes, must refuse a buffer that is not one block, and must leave a
 * refused context untouched.
 */
static void test_absorb_block(void) {
    uint8_t stream[160U];
    struct sha256 context;
    struct sha256 copy;
    uint8_t one_shot[SHA256_DIGEST_SIZE];
    uint8_t absorbed[SHA256_DIGEST_SIZE];
    uint8_t rejected[SHA256_DIGEST_SIZE];
    size_t index;

    for (index = 0U; index < sizeof(stream); ++index) {
        stream[index] = (uint8_t)(index * 7U + 3U);
    }

    memset(&context, 0xA5, sizeof(context));
    CHECK(sha256_absorb_block(NULL) == SHA256_NULL,
          "absorb block rejects null context");
    CHECK(sha256_absorb_block(&context) == SHA256_BAD_STATE,
          "absorb block rejects a context that was never initialised");

    CHECK(sha256_init(&context) == SHA256_OK,
          "absorb block starts from an initialised context");
    CHECK(sha256_update(&context, stream, SHA256_BLOCK_SIZE) == SHA256_OK,
          "a full block is buffered");
    CHECK(context.buffer_used == SHA256_BLOCK_SIZE &&
              context.total_bytes == 0U,
          "update buffers a full block lazily, as before");
    CHECK(sha256_absorb_block(&context) == SHA256_OK &&
              context.buffer_used == 0U &&
              context.total_bytes == SHA256_BLOCK_SIZE,
          "absorb block compresses it and moves the byte counter");
    CHECK(sha256_update(&context, stream + SHA256_BLOCK_SIZE,
                        sizeof(stream) - SHA256_BLOCK_SIZE) == SHA256_OK,
          "the stream continues after the absorbed block");

    copy = context;
    CHECK(sha256_final(&context, absorbed) == SHA256_OK,
          "the absorbed stream finalises");
    CHECK(sha256_final(&copy, one_shot) == SHA256_OK,
          "a copied context finalises on its own");
    CHECK(memcmp(absorbed, one_shot, SHA256_DIGEST_SIZE) == 0,
          "a copied context carries the same counter and state");
    CHECK(sha256_compute(stream, sizeof(stream), one_shot) == SHA256_OK &&
              memcmp(absorbed, one_shot, SHA256_DIGEST_SIZE) == 0,
          "absorb-then-continue equals one-shot hashing");

    CHECK(sha256_init(&context) == SHA256_OK &&
              sha256_update(&context, stream, 10U) == SHA256_OK &&
              sha256_absorb_block(&context) == SHA256_BAD_STATE &&
              context.buffer_used == 10U && context.total_bytes == 0U,
          "a buffer that is not one block is refused and left untouched");
    CHECK(sha256_update(&context, stream + 10U, sizeof(stream) - 10U) ==
              SHA256_OK &&
          sha256_final(&context, rejected) == SHA256_OK &&
          sha256_compute(stream, sizeof(stream), one_shot) == SHA256_OK &&
          memcmp(rejected, one_shot, SHA256_DIGEST_SIZE) == 0,
          "a refused absorb changes nothing in the stream");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "absorb block performs no allocation");
}

static void test_streaming_million(void) {
    static char million_a[1000000U];
    static const size_t chunks[16U] = {
        1U, 2U, 4U, 8U, 16U, 32U, 64U, 128U,
        256U, 512U, 1024U, 2048U, 4096U, 8192U, 65536U, 982383U
    };
    uint32_t index;

    for (index = 0U; index < sizeof(million_a); ++index) {
        million_a[index] = 'a';
    }
    check_streaming_equivalent((const uint8_t *)million_a,
                               sizeof(million_a), chunks, 16U,
                               "sha256 million-byte streaming matches one-shot");
}

static void test_overflow_and_status(void) {
    struct sha256 context;
    uint8_t digest[SHA256_DIGEST_SIZE];
    enum sha256_status status;

    memset(&context, 0, sizeof(context));
    CHECK(sha256_init(&context) == SHA256_OK,
          "sha256 overflow setup initializes context");
    context.total_bytes = SHA256_MAX_INPUT_BYTES;
    CHECK(sha256_update(&context, "x", 1U) == SHA256_OVERFLOW &&
              context.total_bytes == SHA256_MAX_INPUT_BYTES &&
              context.buffer_used == 0U,
          "sha256 overflow is rejected without context mutation");

    for (status = SHA256_OK; status <= SHA256_OVERFLOW; ++status) {
        CHECK(sha256_status_string(status) != NULL &&
                  (status == SHA256_OK ?
                       strcmp(sha256_status_string(status),
                              "SHA256_OK") == 0 :
                       status == SHA256_NULL ?
                           strcmp(sha256_status_string(status),
                                  "SHA256_NULL") == 0 :
                           status == SHA256_BAD_STATE ?
                               strcmp(sha256_status_string(status),
                                      "SHA256_BAD_STATE") == 0 :
                               strcmp(sha256_status_string(status),
                                      "SHA256_OVERFLOW") == 0),
              "sha256 status string is exact for every defined status");
    }
    CHECK(strcmp(sha256_status_string((enum sha256_status)99U),
                 "SHA256_UNKNOWN") == 0,
          "sha256 status string handles undefined status");
    (void)digest;
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "sha256 known-answer and state tests perform no allocation");
}

int main(void) {
    test_validation_and_state();
    test_known_answers();
    test_streaming_equivalence();
    test_absorb_block();
    test_streaming_million();
    test_overflow_and_status();

    if (failures != 0) {
        fprintf(stderr, "SHA-256 host tests: %d FAILURES\n", failures);
        return 1;
    }
    puts("SHA-256 host tests: ALL OK");
    return 0;
}

#else
#error "sha256-host.c must be compiled with HFOC_SHA256_HOST_TEST"
#endif
