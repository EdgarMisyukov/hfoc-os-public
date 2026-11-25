#include "backup_producer.h"
#include "backup_stream.h"
#include "sha256.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define TEST_CAPACITY 6U
#define TEST_STREAM_CAPACITY 1024U

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

static void init_empty_kv(struct config_kv *kv,
                          struct config_kv_entry *entries,
                          size_t capacity)
{
    memset(entries, 0, sizeof(entries[0]) * capacity);
    CHECK(config_kv_init(kv, entries, capacity) == CONFIG_KV_OK,
          "producer fixture initializes kv");
}

static void populate_state(struct config_kv *kv,
                           struct config_kv_entry *entries,
                           bool reverse_slots)
{
    bool replaced = false;

    init_empty_kv(kv, entries, TEST_CAPACITY);
    if (!reverse_slots) {
        CHECK(config_kv_put(kv, CONFIG_KV_KIND_CONFIG,
                            "beta", 4U, "two", 3U, &replaced) ==
                  CONFIG_KV_OK &&
                  config_kv_put(kv, CONFIG_KV_KIND_AUDIT,
                                "beta", 4U, "audit-two", 9U, &replaced) ==
                      CONFIG_KV_OK &&
                  config_kv_put(kv, CONFIG_KV_KIND_CONFIG,
                                "alpha", 5U, "one", 3U, &replaced) ==
                      CONFIG_KV_OK &&
                  config_kv_put(kv, CONFIG_KV_KIND_AUDIT,
                                "alpha", 5U, "audit-one", 9U, &replaced) ==
                      CONFIG_KV_OK,
              "producer forward checkpoint populates exact entries");
    } else {
        CHECK(config_kv_put(kv, CONFIG_KV_KIND_AUDIT,
                            "alpha", 5U, "audit-one", 9U, &replaced) ==
                  CONFIG_KV_OK &&
                  config_kv_put(kv, CONFIG_KV_KIND_CONFIG,
                                "alpha", 5U, "one", 3U, &replaced) ==
                      CONFIG_KV_OK &&
                  config_kv_put(kv, CONFIG_KV_KIND_AUDIT,
                                "beta", 4U, "audit-two", 9U, &replaced) ==
                      CONFIG_KV_OK &&
                  config_kv_put(kv, CONFIG_KV_KIND_CONFIG,
                                "beta", 4U, "two", 3U, &replaced) ==
                      CONFIG_KV_OK,
              "producer reverse checkpoint populates exact entries");
    }
}

static void drain_producer(const struct config_kv *kv,
                           unsigned char *stream,
                           size_t stream_capacity,
                           size_t *stream_length_out)
{
    struct backup_producer producer;
    unsigned char chunk[BACKUP_PRODUCER_MIN_CHUNK_SIZE];
    size_t chunk_length = 0U;
    size_t offset = 0U;

    CHECK(backup_producer_init(&producer, kv) == BACKUP_PRODUCER_OK,
          "producer init publishes an active iterator");
    for (;;) {
        enum backup_producer_status status;

        status = backup_producer_next(&producer, chunk, sizeof(chunk),
                                      &chunk_length);
        if (status == BACKUP_PRODUCER_DONE) {
            break;
        }
        CHECK(status == BACKUP_PRODUCER_OK,
              "producer next emits a complete atomic unit");
        CHECK(offset + chunk_length <= stream_capacity,
              "producer stream remains within fixture capacity");
        memcpy(stream + offset, chunk, chunk_length);
        offset += chunk_length;
    }
    *stream_length_out = offset;
}

static void test_validation_and_status_strings(void)
{
    static const enum backup_producer_status statuses[] = {
        BACKUP_PRODUCER_OK,
        BACKUP_PRODUCER_NULL,
        BACKUP_PRODUCER_BAD_STATE,
        BACKUP_PRODUCER_TOO_SMALL,
        BACKUP_PRODUCER_DONE,
    };
    struct backup_producer producer;
    struct config_kv kv;
    struct config_kv_entry entries[TEST_CAPACITY];
    struct config_kv bad_kv;
    unsigned char chunk[BACKUP_PRODUCER_MIN_CHUNK_SIZE];
    size_t chunk_length = 0U;
    size_t index;

    memset(&producer, 0, sizeof(producer));
    memset(&bad_kv, 0, sizeof(bad_kv));
    populate_state(&kv, entries, false);

    CHECK(backup_producer_init(NULL, &kv) == BACKUP_PRODUCER_NULL,
          "producer init rejects null producer");
    CHECK(backup_producer_init(&producer, NULL) == BACKUP_PRODUCER_NULL,
          "producer init rejects null kv");
    CHECK(backup_producer_init(&producer, &bad_kv) ==
              BACKUP_PRODUCER_BAD_STATE,
          "producer init rejects uninitialized kv");

    CHECK(backup_producer_init(&producer, &kv) == BACKUP_PRODUCER_OK &&
              producer.active &&
              !producer.done &&
              !producer.closed &&
              producer.kv == &kv,
          "producer init publishes exact active source");

    CHECK(backup_producer_next(NULL, chunk, sizeof(chunk),
                               &chunk_length) == BACKUP_PRODUCER_NULL,
          "producer next rejects null producer");
    CHECK(backup_producer_next(&producer, NULL, sizeof(chunk),
                               &chunk_length) == BACKUP_PRODUCER_NULL,
          "producer next rejects null buffer");
    CHECK(backup_producer_next(&producer, chunk, sizeof(chunk),
                               NULL) == BACKUP_PRODUCER_NULL,
          "producer next rejects null length output");
    CHECK(backup_producer_close(NULL) == BACKUP_PRODUCER_NULL,
          "producer close rejects null producer");

    for (index = 0U; index < sizeof(statuses) / sizeof(statuses[0]);
         ++index) {
        CHECK(backup_producer_status_string(statuses[index]) != NULL &&
                  strcmp(backup_producer_status_string(statuses[index]),
                         statuses[index] == BACKUP_PRODUCER_OK ?
                             "BACKUP_PRODUCER_OK" :
                         statuses[index] == BACKUP_PRODUCER_NULL ?
                             "BACKUP_PRODUCER_NULL" :
                         statuses[index] == BACKUP_PRODUCER_BAD_STATE ?
                             "BACKUP_PRODUCER_BAD_STATE" :
                         statuses[index] == BACKUP_PRODUCER_TOO_SMALL ?
                             "BACKUP_PRODUCER_TOO_SMALL" :
                             "BACKUP_PRODUCER_DONE") == 0,
              "producer status string is exact");
    }
    CHECK(strcmp(backup_producer_status_string(
                     (enum backup_producer_status)99U),
                 "BACKUP_PRODUCER_UNKNOWN") == 0,
          "producer undefined status string is exact");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "producer validation performs no allocation");
}

static void check_empty_sequence(void)
{
    struct backup_producer producer;
    struct config_kv kv;
    struct config_kv_entry entries[TEST_CAPACITY];
    unsigned char stream[TEST_STREAM_CAPACITY];
    unsigned char expected[TEST_STREAM_CAPACITY];
    unsigned char chunk[BACKUP_PRODUCER_MIN_CHUNK_SIZE];
    struct backup_stream_layout layout;
    size_t stream_length = 0U;
    size_t expected_length = 0U;
    size_t chunk_length = 0U;
    size_t offset = 0U;

    init_empty_kv(&kv, entries, TEST_CAPACITY);
    CHECK(backup_stream_export(&kv, expected, sizeof(expected),
                               &expected_length) == BACKUP_STREAM_OK,
          "empty one-shot export fixture succeeds");

    CHECK(backup_producer_init(&producer, &kv) == BACKUP_PRODUCER_OK,
          "empty producer init succeeds");
    CHECK(backup_producer_next(&producer, chunk, sizeof(chunk),
                               &chunk_length) == BACKUP_PRODUCER_OK &&
              chunk_length == BACKUP_PRODUCER_HEADER_SIZE,
          "empty producer emits header unit");
    memcpy(stream + offset, chunk, chunk_length);
    offset += chunk_length;

    chunk_length = 0U;
    CHECK(backup_producer_next(&producer, chunk, sizeof(chunk),
                               &chunk_length) == BACKUP_PRODUCER_OK &&
              chunk_length == BACKUP_PRODUCER_DIGEST_SIZE,
          "empty producer emits terminal digest unit");
    memcpy(stream + offset, chunk, chunk_length);
    offset += chunk_length;

    CHECK(backup_producer_next(&producer, chunk, sizeof(chunk),
                               &chunk_length) == BACKUP_PRODUCER_DONE,
          "empty producer exhausts after digest");
    stream_length = offset;
    CHECK(stream_length == expected_length &&
              memcmp(stream, expected, stream_length) == 0,
          "empty producer stream equals one-shot export");
    CHECK(backup_stream_frame_validate(stream, stream_length,
                                       &layout) == BACKUP_STREAM_OK &&
              layout.entry_count == 0U,
          "empty producer stream passes framing v1");
}

static void check_populated_sequence(bool reverse_slots,
                                     const char *message)
{
    struct config_kv kv;
    struct config_kv_entry entries[TEST_CAPACITY];
    unsigned char stream[TEST_STREAM_CAPACITY];
    unsigned char expected[TEST_STREAM_CAPACITY];
    struct backup_stream_layout layout;
    size_t stream_length = 0U;
    size_t expected_length = 0U;

    populate_state(&kv, entries, reverse_slots);
    CHECK(backup_stream_export(&kv, expected, sizeof(expected),
                               &expected_length) == BACKUP_STREAM_OK,
          "populated one-shot export fixture succeeds");
    drain_producer(&kv, stream, sizeof(stream), &stream_length);
    CHECK(stream_length == expected_length &&
              memcmp(stream, expected, stream_length) == 0,
          message);
    CHECK(backup_stream_frame_validate(stream, stream_length,
                                       &layout) == BACKUP_STREAM_OK &&
              layout.header.config_count == 2U &&
              layout.header.audit_count == 2U &&
              layout.entry_count == 4U,
          "populated producer stream passes framing v1");
}

static void test_byte_identical_sequences(void)
{
    struct config_kv forward_kv;
    struct config_kv reverse_kv;
    struct config_kv_entry forward_entries[TEST_CAPACITY];
    struct config_kv_entry reverse_entries[TEST_CAPACITY];
    unsigned char forward[TEST_STREAM_CAPACITY];
    unsigned char reverse[TEST_STREAM_CAPACITY];
    size_t forward_length = 0U;
    size_t reverse_length = 0U;

    check_empty_sequence();
    check_populated_sequence(false,
                             "populated producer stream equals one-shot export");
    check_populated_sequence(true,
                             "reverse-slot producer stream equals one-shot export");

    populate_state(&forward_kv, forward_entries, false);
    populate_state(&reverse_kv, reverse_entries, true);
    drain_producer(&forward_kv, forward, sizeof(forward), &forward_length);
    drain_producer(&reverse_kv, reverse, sizeof(reverse), &reverse_length);
    CHECK(forward_length == reverse_length &&
              memcmp(forward, reverse, forward_length) == 0,
          "equal logical state produces byte-identical sequences");
}

static void check_short_output_retry(void)
{
    struct backup_producer producer;
    struct backup_producer before;
    struct config_kv kv;
    struct config_kv_entry entries[TEST_CAPACITY];
    unsigned char chunk[BACKUP_PRODUCER_MIN_CHUNK_SIZE];
    unsigned char before_bytes[sizeof(chunk)];
    size_t chunk_length = 0x5A5A5A5AU;

    populate_state(&kv, entries, false);
    CHECK(backup_producer_init(&producer, &kv) == BACKUP_PRODUCER_OK,
          "short-output fixture initializes producer");
    memcpy(&before, &producer, sizeof(before));
    memset(chunk, 0xA5, sizeof(chunk));
    memcpy(before_bytes, chunk, sizeof(chunk));

    CHECK(backup_producer_next(&producer, chunk,
                               BACKUP_PRODUCER_HEADER_SIZE - 1U,
                               &chunk_length) == BACKUP_PRODUCER_TOO_SMALL,
          "short header buffer returns too-small");
    CHECK(memcmp(&producer, &before, sizeof(producer)) == 0 &&
              memcmp(chunk, before_bytes, sizeof(chunk)) == 0 &&
              chunk_length == 0x5A5A5A5AU,
          "short header buffer leaves producer, output, and length unchanged");

    chunk_length = 0U;
    CHECK(backup_producer_next(&producer, chunk, sizeof(chunk),
                               &chunk_length) == BACKUP_PRODUCER_OK &&
              chunk_length == BACKUP_PRODUCER_HEADER_SIZE,
          "header retry with enough capacity emits exact header");

    memcpy(&before, &producer, sizeof(before));
    memset(chunk, 0x5A, sizeof(chunk));
    memcpy(before_bytes, chunk, sizeof(chunk));
    chunk_length = 0x5A5A5A5AU;
    CHECK(backup_producer_next(&producer, chunk,
                               BACKUP_STREAM_FRAME_HEADER_SIZE,
                               &chunk_length) == BACKUP_PRODUCER_TOO_SMALL,
          "short frame buffer returns too-small");
    CHECK(memcmp(&producer, &before, sizeof(producer)) == 0 &&
              memcmp(chunk, before_bytes, sizeof(chunk)) == 0 &&
              chunk_length == 0x5A5A5A5AU,
          "short frame buffer leaves producer, output, and length unchanged");

    chunk_length = 0U;
    CHECK(backup_producer_next(&producer, chunk, sizeof(chunk),
                               &chunk_length) == BACKUP_PRODUCER_OK &&
              chunk_length > BACKUP_STREAM_FRAME_HEADER_SIZE,
          "frame retry with enough capacity emits one complete frame");
    CHECK(backup_producer_close(&producer) == BACKUP_PRODUCER_OK,
          "short-output retry fixture closes cleanly");
}

static void check_close_and_exhaustion(void)
{
    struct config_kv kv;
    struct config_kv_entry entries[TEST_CAPACITY];
    struct config_kv kv_before;
    struct config_kv_entry entries_before[TEST_CAPACITY];
    struct backup_producer producer;
    struct backup_stream_layout layout;
    unsigned char chunk[BACKUP_PRODUCER_MIN_CHUNK_SIZE];
    unsigned char stream[TEST_STREAM_CAPACITY];
    size_t stream_length = 0U;
    size_t chunk_length = 0U;

    populate_state(&kv, entries, false);
    memcpy(&kv_before, &kv, sizeof(kv));
    memcpy(entries_before, entries, sizeof(entries));
    CHECK(backup_producer_init(&producer, &kv) == BACKUP_PRODUCER_OK,
          "close fixture initializes producer");
    CHECK(backup_producer_next(&producer, chunk, sizeof(chunk),
                               &chunk_length) == BACKUP_PRODUCER_OK,
          "close fixture emits header before close");
    CHECK(backup_producer_close(&producer) == BACKUP_PRODUCER_OK,
          "close before exhaustion succeeds");
    CHECK(!producer.active && producer.closed,
          "close marks producer inactive and closed");
    CHECK(backup_producer_next(&producer, chunk, sizeof(chunk),
                               &chunk_length) == BACKUP_PRODUCER_BAD_STATE,
          "next after close before exhaustion is bad state");
    CHECK(backup_producer_close(&producer) == BACKUP_PRODUCER_OK,
          "close is idempotent");
    CHECK(memcmp(&kv, &kv_before, sizeof(kv)) == 0 &&
              memcmp(entries, entries_before, sizeof(entries)) == 0,
          "close leaves source state unchanged");

    populate_state(&kv, entries, false);
    memcpy(&kv_before, &kv, sizeof(kv));
    memcpy(entries_before, entries, sizeof(entries));
    drain_producer(&kv, stream, sizeof(stream), &stream_length);
    CHECK(stream_length > 0U &&
              backup_stream_frame_validate(stream, stream_length,
                                           &layout) == BACKUP_STREAM_OK,
          "drained stream passes framing v1");
    CHECK(memcmp(&kv, &kv_before, sizeof(kv)) == 0 &&
              memcmp(entries, entries_before, sizeof(entries)) == 0,
          "exhaustion leaves source state unchanged");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "producer iteration performs no allocation");
}

int main(void)
{
    test_validation_and_status_strings();
    test_byte_identical_sequences();
    check_short_output_retry();
    check_close_and_exhaustion();

    if (failures != 0) {
        fprintf(stderr, "Backup producer host tests: %d FAILURES\n",
                failures);
        return 1;
    }
    printf("Backup producer host tests: ALL OK\n");
    return 0;
}
