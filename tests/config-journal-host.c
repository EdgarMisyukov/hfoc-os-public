#include "block_device.h"
#include "config_journal.h"
#include "sha256.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifdef HFOC_CONFIG_JOURNAL_HOST_TEST

#define TEST_BLOCK_SIZE 512U
#define TEST_BLOCK_COUNT 8U
#define TEST_CAPACITY (TEST_BLOCK_SIZE * TEST_BLOCK_COUNT)
#define TEST_REGION_OFFSET 1U
#define TEST_REGION_COUNT 7U

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

struct fixture_device {
    unsigned char storage[TEST_CAPACITY];
    bool fail_read;
    bool fail_write;
    bool fail_write_partial;
    size_t read_calls;
    size_t write_calls;
};

static enum block_device_status fixture_read(
    struct block_device *device,
    uint64_t offset,
    void *buffer,
    size_t length)
{
    struct fixture_device *fixture =
        (struct fixture_device *)device->context;

    ++fixture->read_calls;
    if (fixture->fail_read) {
        fixture->fail_read = false;
        return BLOCK_DEVICE_IO_ERROR;
    }
    memcpy(buffer, fixture->storage + (size_t)offset, length);
    return BLOCK_DEVICE_OK;
}

static enum block_device_status fixture_write(
    struct block_device *device,
    uint64_t offset,
    const void *buffer,
    size_t length)
{
    struct fixture_device *fixture =
        (struct fixture_device *)device->context;

    ++fixture->write_calls;
    if (fixture->fail_write) {
        fixture->fail_write = false;
        return BLOCK_DEVICE_IO_ERROR;
    }
    if (fixture->fail_write_partial) {
        fixture->fail_write_partial = false;
        memcpy(fixture->storage + (size_t)offset, buffer,
               CONFIG_JOURNAL_COMMIT_OFFSET);
        return BLOCK_DEVICE_IO_ERROR;
    }
    memcpy(fixture->storage + (size_t)offset, buffer, length);
    return BLOCK_DEVICE_OK;
}

static void init_fixture(struct fixture_device *fixture) {
    memset(fixture, 0, sizeof(*fixture));
}

static bool all_bytes(const unsigned char *buffer,
                      size_t length,
                      unsigned char value)
{
    size_t index;

    for (index = 0U; index < length; ++index) {
        if (buffer[index] != value) {
            return false;
        }
    }
    return true;
}

static struct block_device make_device(struct fixture_device *fixture) {
    struct block_device device;

    memset(&device, 0, sizeof(device));
    device.block_size = TEST_BLOCK_SIZE;
    device.block_count = TEST_BLOCK_COUNT;
    device.context = fixture;
    device.read_fn = fixture_read;
    device.write_fn = fixture_write;
    return device;
}

static void store_le32(unsigned char *bytes, uint32_t value) {
    bytes[0] = (unsigned char)(value);
    bytes[1] = (unsigned char)(value >> 8U);
    bytes[2] = (unsigned char)(value >> 16U);
    bytes[3] = (unsigned char)(value >> 24U);
}

static void store_le64(unsigned char *bytes, uint64_t value) {
    size_t index;

    for (index = 0U; index < 8U; ++index) {
        bytes[index] = (unsigned char)(value >> (8U * index));
    }
}

static void write_valid_record(struct fixture_device *fixture,
                               uint64_t slot,
                               uint64_t sequence,
                               const char *data,
                               size_t data_length)
{
    static const unsigned char magic[CONFIG_JOURNAL_MAGIC_SIZE] =
        CONFIG_JOURNAL_MAGIC_BYTES;
    static const unsigned char commit_magic[
        CONFIG_JOURNAL_COMMIT_MAGIC_SIZE] =
        CONFIG_JOURNAL_COMMIT_MAGIC_BYTES;
    unsigned char block[TEST_BLOCK_SIZE];
    unsigned char digest[SHA256_DIGEST_SIZE];
    struct sha256 context;

    memset(block, 0, sizeof(block));
    memcpy(block, magic, sizeof(magic));
    block[8U] = CONFIG_JOURNAL_FORMAT_VERSION;
    store_le64(block + 16U, sequence);
    store_le32(block + 24U, (uint32_t)data_length);

    CHECK(sha256_init(&context) == SHA256_OK,
          "valid-record helper initializes sha256");
    CHECK(sha256_update(&context, block, 32U) == SHA256_OK,
          "valid-record helper updates header");
    CHECK(sha256_update(&context, data, data_length) == SHA256_OK,
          "valid-record helper updates data");
    memcpy(block + CONFIG_JOURNAL_COMMIT_OFFSET, commit_magic,
           sizeof(commit_magic));
    store_le64(block + CONFIG_JOURNAL_COMMIT_SEQUENCE_OFFSET, sequence);
    CHECK(sha256_update(&context,
                        block + CONFIG_JOURNAL_COMMIT_OFFSET,
                        CONFIG_JOURNAL_COMMIT_SIZE) == SHA256_OK,
          "valid-record helper updates commit marker");
    CHECK(sha256_final(&context, digest) == SHA256_OK,
          "valid-record helper finalizes checksum");
    memcpy(block + CONFIG_JOURNAL_CHECKSUM_OFFSET, digest,
           SHA256_DIGEST_SIZE);
    memcpy(block + CONFIG_JOURNAL_DATA_OFFSET, data, data_length);
    memcpy(fixture->storage +
               (size_t)((TEST_REGION_OFFSET + slot) * TEST_BLOCK_SIZE),
           block, sizeof(block));
}

static void write_markerless_record(struct fixture_device *fixture,
                                    uint64_t slot,
                                    uint64_t sequence,
                                    const char *data,
                                    size_t data_length)
{
    static const unsigned char magic[CONFIG_JOURNAL_MAGIC_SIZE] =
        CONFIG_JOURNAL_MAGIC_BYTES;
    unsigned char block[TEST_BLOCK_SIZE];
    unsigned char digest[SHA256_DIGEST_SIZE];
    struct sha256 context;

    memset(block, 0, sizeof(block));
    memcpy(block, magic, sizeof(magic));
    block[8U] = CONFIG_JOURNAL_FORMAT_VERSION;
    store_le64(block + 16U, sequence);
    store_le32(block + 24U, (uint32_t)data_length);
    memcpy(block + CONFIG_JOURNAL_DATA_OFFSET, data, data_length);

    CHECK(sha256_init(&context) == SHA256_OK,
          "markerless-record helper initializes sha256");
    CHECK(sha256_update(&context, block, 32U) == SHA256_OK,
          "markerless-record helper updates header");
    CHECK(sha256_update(&context, data, data_length) == SHA256_OK,
          "markerless-record helper updates data");
    CHECK(sha256_final(&context, digest) == SHA256_OK,
          "markerless-record helper finalizes checksum");
    memcpy(block + CONFIG_JOURNAL_CHECKSUM_OFFSET, digest,
           SHA256_DIGEST_SIZE);
    memcpy(fixture->storage +
               (size_t)((TEST_REGION_OFFSET + slot) * TEST_BLOCK_SIZE),
           block, sizeof(block));
}

static enum config_journal_status init_journal(
    struct config_journal *journal,
    struct block_device *device,
    uint64_t block_count)
{
    memset(journal, 0, sizeof(*journal));
    return config_journal_init(journal, device, TEST_REGION_OFFSET,
                               block_count);
}

static void test_validation_and_status_strings(void) {
    static const enum config_journal_status statuses[] = {
        CONFIG_JOURNAL_OK,
        CONFIG_JOURNAL_NULL,
        CONFIG_JOURNAL_BAD_DEVICE,
        CONFIG_JOURNAL_BAD_REGION,
        CONFIG_JOURNAL_IO_ERROR,
        CONFIG_JOURNAL_CORRUPT,
        CONFIG_JOURNAL_BAD_STATE,
        CONFIG_JOURNAL_BAD_RECORD,
        CONFIG_JOURNAL_TOO_SMALL,
        CONFIG_JOURNAL_EMPTY,
        CONFIG_JOURNAL_FULL,
    };
    struct fixture_device fixture;
    struct block_device device;
    struct config_journal journal;
    unsigned char buffer[CONFIG_JOURNAL_DATA_MAX];
    size_t length = 0U;
    size_t index;

    init_fixture(&fixture);
    device = make_device(&fixture);
    memset(buffer, 0, sizeof(buffer));

    CHECK(config_journal_init(NULL, &device, TEST_REGION_OFFSET,
                              TEST_REGION_COUNT) == CONFIG_JOURNAL_NULL &&
              fixture.read_calls == 0U,
          "journal init rejects null journal without I/O");
    CHECK(config_journal_init(&journal, NULL, TEST_REGION_OFFSET,
                              TEST_REGION_COUNT) == CONFIG_JOURNAL_NULL &&
              fixture.read_calls == 0U,
          "journal init rejects null device without I/O");

    device.read_fn = NULL;
    CHECK(config_journal_init(&journal, &device, TEST_REGION_OFFSET,
                              TEST_REGION_COUNT) ==
              CONFIG_JOURNAL_BAD_DEVICE &&
              fixture.read_calls == 0U,
          "journal init rejects invalid device without I/O");
    device = make_device(&fixture);

    device.block_size = 4096U;
    CHECK(config_journal_init(&journal, &device, TEST_REGION_OFFSET,
                              TEST_REGION_COUNT) ==
              CONFIG_JOURNAL_BAD_DEVICE &&
              fixture.read_calls == 0U,
          "journal init rejects unsupported block size without I/O");
    device = make_device(&fixture);

    CHECK(config_journal_init(&journal, &device, TEST_REGION_OFFSET,
                              0U) == CONFIG_JOURNAL_BAD_REGION &&
              fixture.read_calls == 0U,
          "journal init rejects empty region without I/O");
    CHECK(config_journal_init(&journal, &device, TEST_REGION_OFFSET,
                              CONFIG_JOURNAL_REGION_BLOCK_COUNT_MAX + 1U) ==
              CONFIG_JOURNAL_BAD_REGION &&
              fixture.read_calls == 0U,
          "journal init rejects over-limit region without I/O");
    CHECK(config_journal_init(&journal, &device, TEST_BLOCK_COUNT, 1U) ==
              CONFIG_JOURNAL_BAD_REGION &&
              fixture.read_calls == 0U,
          "journal init rejects out-of-range region without I/O");

    CHECK(init_journal(&journal, &device, TEST_REGION_COUNT) ==
              CONFIG_JOURNAL_OK &&
              journal.initialized &&
              journal.next_block == 0U &&
              journal.next_sequence == 1U &&
              fixture.read_calls == TEST_REGION_COUNT,
          "empty region recovery publishes append position zero");

    CHECK(config_journal_append(&journal, NULL, 1U) ==
              CONFIG_JOURNAL_NULL,
          "journal append rejects null data");
    CHECK(config_journal_append(&journal, "x", 0U) ==
              CONFIG_JOURNAL_BAD_RECORD,
          "journal append rejects zero length");
    CHECK(config_journal_append(&journal, buffer,
                                CONFIG_JOURNAL_DATA_MAX + 1U) ==
              CONFIG_JOURNAL_BAD_RECORD,
          "journal append rejects over-limit data");
    memset(&journal, 0, sizeof(journal));
    CHECK(config_journal_append(&journal, "x", 1U) ==
              CONFIG_JOURNAL_BAD_STATE &&
              fixture.write_calls == 0U,
          "uninitialized journal rejects append");
    CHECK(config_journal_head(&journal, buffer, sizeof(buffer), &length) ==
              CONFIG_JOURNAL_BAD_STATE,
          "uninitialized journal rejects head read");
    CHECK(config_journal_head(&journal, NULL, sizeof(buffer), &length) ==
              CONFIG_JOURNAL_NULL,
          "journal head rejects null buffer");
    CHECK(config_journal_head(&journal, buffer, sizeof(buffer), NULL) ==
              CONFIG_JOURNAL_NULL,
          "journal head rejects null length");

    for (index = 0U; index < sizeof(statuses) / sizeof(statuses[0]);
         ++index) {
        CHECK(config_journal_status_string(statuses[index]) != NULL &&
                  strcmp(config_journal_status_string(statuses[index]),
                         statuses[index] == CONFIG_JOURNAL_OK ?
                             "CONFIG_JOURNAL_OK" :
                         statuses[index] == CONFIG_JOURNAL_NULL ?
                             "CONFIG_JOURNAL_NULL" :
                         statuses[index] == CONFIG_JOURNAL_BAD_DEVICE ?
                             "CONFIG_JOURNAL_BAD_DEVICE" :
                         statuses[index] == CONFIG_JOURNAL_BAD_REGION ?
                             "CONFIG_JOURNAL_BAD_REGION" :
                         statuses[index] == CONFIG_JOURNAL_IO_ERROR ?
                             "CONFIG_JOURNAL_IO_ERROR" :
                         statuses[index] == CONFIG_JOURNAL_CORRUPT ?
                             "CONFIG_JOURNAL_CORRUPT" :
                         statuses[index] == CONFIG_JOURNAL_BAD_STATE ?
                             "CONFIG_JOURNAL_BAD_STATE" :
                         statuses[index] == CONFIG_JOURNAL_BAD_RECORD ?
                             "CONFIG_JOURNAL_BAD_RECORD" :
                         statuses[index] == CONFIG_JOURNAL_TOO_SMALL ?
                             "CONFIG_JOURNAL_TOO_SMALL" :
                         statuses[index] == CONFIG_JOURNAL_EMPTY ?
                             "CONFIG_JOURNAL_EMPTY" :
                             "CONFIG_JOURNAL_FULL") == 0,
              "config-journal status string is exact");
    }
    CHECK(strcmp(config_journal_status_string(
                     (enum config_journal_status)99U),
                 "CONFIG_JOURNAL_UNKNOWN") == 0,
          "config-journal undefined status string is exact");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "journal validation performs no allocation");
}

static void test_append_head_and_recovery(void) {
    struct fixture_device fixture;
    struct block_device device;
    struct config_journal journal;
    unsigned char buffer[CONFIG_JOURNAL_DATA_MAX];
    size_t length = 0U;

    init_fixture(&fixture);
    device = make_device(&fixture);
    CHECK(init_journal(&journal, &device, TEST_REGION_COUNT) ==
              CONFIG_JOURNAL_OK,
          "append test journal init succeeds");

    CHECK(config_journal_append(&journal, "one", 3U) ==
              CONFIG_JOURNAL_OK &&
              fixture.write_calls == 1U &&
              journal.next_block == 1U &&
              journal.next_sequence == 2U,
          "first append publishes sequence one");
    CHECK(config_journal_head(&journal, buffer, sizeof(buffer), &length) ==
              CONFIG_JOURNAL_OK &&
              length == 3U &&
              memcmp(buffer, "one", 3U) == 0,
          "first appended record is head");

    CHECK(config_journal_append(&journal, "two", 3U) ==
              CONFIG_JOURNAL_OK &&
              fixture.write_calls == 2U &&
              journal.next_block == 2U &&
              journal.next_sequence == 3U,
          "second append publishes sequence two");
    CHECK(config_journal_head(&journal, buffer, sizeof(buffer), &length) ==
              CONFIG_JOURNAL_OK &&
              length == 3U &&
              memcmp(buffer, "two", 3U) == 0,
          "second appended record is head");

    init_journal(&journal, &device, TEST_REGION_COUNT);
    CHECK(journal.next_block == 2U &&
              journal.next_sequence == 3U &&
              fixture.read_calls == 2U * TEST_REGION_COUNT + 2U,
          "recovery restores append position after committed records");
    CHECK(config_journal_head(&journal, buffer, sizeof(buffer), &length) ==
              CONFIG_JOURNAL_OK &&
              length == 3U &&
              memcmp(buffer, "two", 3U) == 0,
          "recovery restores committed head");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "append and recovery perform no allocation");
}

static void test_interrupted_tail_recovery_and_overwrite(void) {
    struct fixture_device fixture;
    struct block_device device;
    struct config_journal journal;
    unsigned char buffer[CONFIG_JOURNAL_DATA_MAX];
    size_t length = 0U;

    init_fixture(&fixture);
    device = make_device(&fixture);
    CHECK(init_journal(&journal, &device, TEST_REGION_COUNT) ==
              CONFIG_JOURNAL_OK &&
              config_journal_append(&journal, "one", 3U) ==
                  CONFIG_JOURNAL_OK &&
              config_journal_append(&journal, "two", 3U) ==
                  CONFIG_JOURNAL_OK,
          "interrupted-tail setup appends two records");

    memset(fixture.storage +
               (size_t)((TEST_REGION_OFFSET + 2U) * TEST_BLOCK_SIZE),
           0xA5, TEST_BLOCK_SIZE);
    CHECK(init_journal(&journal, &device, TEST_REGION_COUNT) ==
              CONFIG_JOURNAL_OK &&
              journal.next_block == 2U &&
              journal.next_sequence == 3U,
          "interrupted tail is ignored during reverse-order recovery");
    CHECK(config_journal_head(&journal, buffer, sizeof(buffer), &length) ==
              CONFIG_JOURNAL_OK &&
              length == 3U &&
              memcmp(buffer, "two", 3U) == 0,
          "interrupted tail is not published as live head");

    CHECK(config_journal_append(&journal, "three", 5U) ==
              CONFIG_JOURNAL_OK &&
              journal.next_block == 3U &&
              journal.next_sequence == 4U,
          "append overwrites interrupted tail at recovered position");
    CHECK(init_journal(&journal, &device, TEST_REGION_COUNT) ==
              CONFIG_JOURNAL_OK &&
              config_journal_head(&journal, buffer, sizeof(buffer),
                                  &length) == CONFIG_JOURNAL_OK &&
              length == 5U &&
              memcmp(buffer, "three", 5U) == 0,
          "recovery confirms interrupted tail was replaced");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "interrupted-tail recovery performs no allocation");
}

static void test_interrupted_initial_record_resets(void) {
    struct fixture_device fixture;
    struct block_device device;
    struct config_journal journal;
    unsigned char buffer[CONFIG_JOURNAL_DATA_MAX];
    size_t length = 0U;

    init_fixture(&fixture);
    device = make_device(&fixture);
    memset(fixture.storage +
               (size_t)(TEST_REGION_OFFSET * TEST_BLOCK_SIZE),
           0xA5, TEST_BLOCK_SIZE);

    CHECK(init_journal(&journal, &device, TEST_REGION_COUNT) ==
              CONFIG_JOURNAL_OK &&
              journal.next_block == 0U &&
              journal.next_sequence == 1U,
          "interrupted first record resets journal to empty");
    CHECK(config_journal_append(&journal, "first", 5U) ==
              CONFIG_JOURNAL_OK,
          "empty recovery can append first record");
    CHECK(init_journal(&journal, &device, TEST_REGION_COUNT) ==
              CONFIG_JOURNAL_OK &&
              config_journal_head(&journal, buffer, sizeof(buffer),
                                  &length) == CONFIG_JOURNAL_OK &&
              length == 5U &&
              memcmp(buffer, "first", 5U) == 0,
          "first record replaces interrupted initial block");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "interrupted-initial recovery performs no allocation");
}

static void test_corrupt_chain_and_sequence_gap(void) {
    struct fixture_device fixture;
    struct block_device device;
    struct config_journal journal;

    init_fixture(&fixture);
    device = make_device(&fixture);
    CHECK(init_journal(&journal, &device, TEST_REGION_COUNT) ==
              CONFIG_JOURNAL_OK &&
              config_journal_append(&journal, "one", 3U) ==
                  CONFIG_JOURNAL_OK,
          "gap test appends first committed record");
    memset(fixture.storage +
               (size_t)((TEST_REGION_OFFSET + 1U) * TEST_BLOCK_SIZE),
           0xA5, TEST_BLOCK_SIZE);
    write_valid_record(&fixture, 2U, 3U, "three", 5U);

    CHECK(init_journal(&journal, &device, TEST_REGION_COUNT) ==
              CONFIG_JOURNAL_CORRUPT,
          "missing committed record returns corrupt chain");

    init_fixture(&fixture);
    device = make_device(&fixture);
    write_valid_record(&fixture, 0U, 5U, "bad", 3U);
    CHECK(init_journal(&journal, &device, TEST_REGION_COUNT) ==
              CONFIG_JOURNAL_CORRUPT,
          "sequence not matching slot returns corrupt chain");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "corrupt-chain recovery performs no allocation");
}

static void test_append_failure_and_init_read_failure(void) {
    struct fixture_device fixture;
    struct block_device device;
    struct config_journal journal;
    unsigned char buffer[CONFIG_JOURNAL_DATA_MAX];
    size_t length = 0U;

    init_fixture(&fixture);
    device = make_device(&fixture);
    CHECK(init_journal(&journal, &device, TEST_REGION_COUNT) ==
              CONFIG_JOURNAL_OK,
          "append-failure test init succeeds");
    fixture.fail_write = true;
    CHECK(config_journal_append(&journal, "x", 1U) ==
              CONFIG_JOURNAL_IO_ERROR &&
              fixture.write_calls == 1U &&
              !fixture.fail_write &&
              journal.next_block == 0U &&
              journal.next_sequence == 1U &&
              all_bytes(fixture.storage +
                            (size_t)(TEST_REGION_OFFSET * TEST_BLOCK_SIZE),
                        TEST_BLOCK_SIZE, 0U),
          "failed append never advances journal or publishes data");
    CHECK(config_journal_append(&journal, "x", 1U) ==
              CONFIG_JOURNAL_OK &&
              journal.next_block == 1U &&
              journal.next_sequence == 2U,
          "append retry succeeds after injected failure");
    CHECK(config_journal_head(&journal, buffer, sizeof(buffer), &length) ==
              CONFIG_JOURNAL_OK &&
              length == 1U &&
              buffer[0] == 'x',
          "retried append publishes exact record");

    init_fixture(&fixture);
    device = make_device(&fixture);
    fixture.fail_read = true;
    CHECK(init_journal(&journal, &device, TEST_REGION_COUNT) ==
              CONFIG_JOURNAL_IO_ERROR &&
              !journal.initialized &&
              journal.next_block == 0U &&
              journal.next_sequence == 1U,
          "failed recovery read leaves journal unpublished");
    CHECK(init_journal(&journal, &device, TEST_REGION_COUNT) ==
              CONFIG_JOURNAL_OK &&
              journal.initialized,
          "recovery retry succeeds after injected read failure");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "failure paths perform no allocation");
}

static void test_atomic_commit_failures(void) {
    struct fixture_device fixture;
    struct block_device device;
    struct config_journal journal;
    unsigned char buffer[CONFIG_JOURNAL_DATA_MAX];
    size_t length = 0U;

    init_fixture(&fixture);
    device = make_device(&fixture);
    CHECK(init_journal(&journal, &device, TEST_REGION_COUNT) ==
              CONFIG_JOURNAL_OK &&
              config_journal_append(&journal, "one", 3U) ==
                  CONFIG_JOURNAL_OK,
          "atomic failure test commits first record");

    fixture.fail_write_partial = true;
    CHECK(config_journal_append(&journal, "two", 3U) ==
              CONFIG_JOURNAL_IO_ERROR &&
              journal.next_block == 1U &&
              journal.next_sequence == 2U,
          "torn append returns I/O error without advancing");
    CHECK(config_journal_head(&journal, buffer, sizeof(buffer), &length) ==
              CONFIG_JOURNAL_OK &&
              length == 3U &&
              memcmp(buffer, "one", 3U) == 0,
          "torn append leaves previous head readable in memory");
    CHECK(init_journal(&journal, &device, TEST_REGION_COUNT) ==
              CONFIG_JOURNAL_OK &&
              journal.next_block == 1U &&
              journal.next_sequence == 2U,
          "recovery ignores torn tail and restores append position");
    CHECK(config_journal_head(&journal, buffer, sizeof(buffer), &length) ==
              CONFIG_JOURNAL_OK &&
              length == 3U &&
              memcmp(buffer, "one", 3U) == 0,
          "recovery keeps previous head after torn write");

    fixture.fail_write = true;
    CHECK(config_journal_append(&journal, "two", 3U) ==
              CONFIG_JOURNAL_IO_ERROR &&
              journal.next_block == 1U &&
              journal.next_sequence == 2U,
          "failed flush returns I/O error without advancing");
    CHECK(config_journal_head(&journal, buffer, sizeof(buffer), &length) ==
              CONFIG_JOURNAL_OK &&
              length == 3U &&
              memcmp(buffer, "one", 3U) == 0,
          "failed flush leaves previous head readable");
    CHECK(init_journal(&journal, &device, TEST_REGION_COUNT) ==
              CONFIG_JOURNAL_OK &&
              journal.next_block == 1U &&
              journal.next_sequence == 2U,
          "recovery after failed flush preserves previous state");

    write_markerless_record(&fixture, 1U, 2U, "two", 3U);
    CHECK(init_journal(&journal, &device, TEST_REGION_COUNT) ==
              CONFIG_JOURNAL_OK &&
              journal.next_block == 1U &&
              journal.next_sequence == 2U,
          "markerless interrupted commit is ignored by recovery");
    CHECK(config_journal_head(&journal, buffer, sizeof(buffer), &length) ==
              CONFIG_JOURNAL_OK &&
              length == 3U &&
              memcmp(buffer, "one", 3U) == 0,
          "markerless interrupted commit never becomes head");

    CHECK(config_journal_append(&journal, "two", 3U) ==
              CONFIG_JOURNAL_OK &&
              journal.next_block == 2U &&
              journal.next_sequence == 3U,
          "retry overwrites uncommitted slot and commits");
    CHECK(config_journal_head(&journal, buffer, sizeof(buffer), &length) ==
              CONFIG_JOURNAL_OK &&
              length == 3U &&
              memcmp(buffer, "two", 3U) == 0,
          "retry becomes committed head");
    CHECK(init_journal(&journal, &device, TEST_REGION_COUNT) ==
              CONFIG_JOURNAL_OK &&
              config_journal_head(&journal, buffer, sizeof(buffer),
                                  &length) == CONFIG_JOURNAL_OK &&
              length == 3U &&
              memcmp(buffer, "two", 3U) == 0,
          "atomic retry survives reinitialization");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "atomic commit failure paths perform no allocation");
}

static void test_full_and_too_small(void) {
    struct fixture_device fixture;
    struct block_device device;
    struct config_journal journal;
    unsigned char buffer[8U];
    size_t length = 0U;

    init_fixture(&fixture);
    device = make_device(&fixture);
    CHECK(init_journal(&journal, &device, 2U) == CONFIG_JOURNAL_OK &&
              config_journal_append(&journal, "a", 1U) ==
                  CONFIG_JOURNAL_OK &&
              config_journal_append(&journal, "bb", 2U) ==
                  CONFIG_JOURNAL_OK,
          "two-block region accepts two records");
    CHECK(config_journal_append(&journal, "c", 1U) ==
              CONFIG_JOURNAL_FULL &&
              fixture.write_calls == 2U &&
              journal.next_block == 2U &&
              journal.next_sequence == 3U,
          "full journal rejects append without device write");

    memset(buffer, 'z', sizeof(buffer));
    CHECK(config_journal_head(&journal, buffer, 1U, &length) ==
              CONFIG_JOURNAL_TOO_SMALL &&
              length == 2U &&
              buffer[0] == 'z',
          "head read with small capacity never copies partial record");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "full and too-small paths perform no allocation");
}

int main(void) {
    test_validation_and_status_strings();
    test_append_head_and_recovery();
    test_interrupted_tail_recovery_and_overwrite();
    test_interrupted_initial_record_resets();
    test_corrupt_chain_and_sequence_gap();
    test_append_failure_and_init_read_failure();
    test_atomic_commit_failures();
    test_full_and_too_small();

    if (failures != 0) {
        fprintf(stderr, "Config-journal host tests: %d FAILURES\n", failures);
        return 1;
    }
    puts("Config-journal host tests: ALL OK");
    return 0;
}

#endif
