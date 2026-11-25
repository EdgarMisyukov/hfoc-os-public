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

static void populate_state(struct config_kv *kv,
                           struct config_kv_entry *entries,
                           bool reverse_slots);

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

static void store_le32(unsigned char *bytes, uint32_t value) {
    bytes[0] = (unsigned char)(value);
    bytes[1] = (unsigned char)(value >> 8U);
    bytes[2] = (unsigned char)(value >> 16U);
    bytes[3] = (unsigned char)(value >> 24U);
}

static void store_le16(unsigned char *bytes, uint16_t value) {
    bytes[0] = (unsigned char)(value);
    bytes[1] = (unsigned char)(value >> 8U);
}

static void store_le64(unsigned char *bytes, uint64_t value) {
    size_t index;

    for (index = 0U; index < 8U; ++index) {
        bytes[index] = (unsigned char)(value >> (8U * index));
    }
}

static bool bytes_equal(const unsigned char *left,
                        const unsigned char *right,
                        size_t length)
{
    return memcmp(left, right, length) == 0;
}

static bool entry_equal(const struct config_kv_entry *entry,
                        uint8_t kind,
                        const unsigned char *key,
                        size_t key_length,
                        const unsigned char *value,
                        size_t value_length,
                        uint64_t revision)
{
    return entry != NULL &&
           entry->occupied &&
           entry->kind == kind &&
           entry->key_length == key_length &&
           bytes_equal(entry->key, key, key_length) &&
           entry->value_length == value_length &&
           bytes_equal(entry->value, value, value_length) &&
           entry->revision == revision;
}

static void build_duplicate_stream(unsigned char *stream, size_t *length) {
    const unsigned char magic[BACKUP_STREAM_MAGIC_SIZE] =
        BACKUP_STREAM_MAGIC_BYTES;
    size_t frame_one = BACKUP_STREAM_HEADER_SIZE;
    size_t frame_two = frame_one + BACKUP_STREAM_FRAME_HEADER_SIZE + 4U + 4U;
    size_t digest_offset = frame_two + BACKUP_STREAM_FRAME_HEADER_SIZE +
                           4U + 4U;
    unsigned char digest[SHA256_DIGEST_SIZE];

    memset(stream, 0, BACKUP_STREAM_HEADER_SIZE);
    memcpy(stream, magic, sizeof(magic));
    stream[8U] = BACKUP_STREAM_FORMAT_VERSION;
    store_le32(stream + 16U, 2U);
    store_le32(stream + 20U, 0U);

    memset(stream + frame_one, 0, BACKUP_STREAM_FRAME_HEADER_SIZE);
    stream[frame_one] = CONFIG_KV_KIND_CONFIG;
    stream[frame_one + 1U] = 4U;
    store_le16(stream + frame_one + 2U, 4U);
    store_le64(stream + frame_one + 4U, 1U);
    memcpy(stream + frame_one + BACKUP_STREAM_FRAME_HEADER_SIZE,
           "aaaa", 4U);
    memcpy(stream + frame_one + BACKUP_STREAM_FRAME_HEADER_SIZE + 4U,
           "v1\0\0", 4U);

    memset(stream + frame_two, 0, BACKUP_STREAM_FRAME_HEADER_SIZE);
    stream[frame_two] = CONFIG_KV_KIND_CONFIG;
    stream[frame_two + 1U] = 4U;
    store_le16(stream + frame_two + 2U, 4U);
    store_le64(stream + frame_two + 4U, 1U);
    memcpy(stream + frame_two + BACKUP_STREAM_FRAME_HEADER_SIZE,
           "bbbb", 4U);
    memcpy(stream + frame_two + BACKUP_STREAM_FRAME_HEADER_SIZE + 4U,
           "v2\0\0", 4U);

    CHECK(sha256_compute(stream, digest_offset, digest) == SHA256_OK,
          "duplicate fixture digest computes");
    memcpy(stream + digest_offset, digest, SHA256_DIGEST_SIZE);
    *length = digest_offset + SHA256_DIGEST_SIZE;
}

static void check_import_failure(const unsigned char *stream,
                                 size_t length,
                                 enum backup_stream_status expected,
                                 const char *message)
{
    struct config_kv_entry output[TEST_CAPACITY];
    unsigned char before[sizeof(output)];
    size_t entry_count = 0x5AU;
    struct backup_stream_counts counts;

    memset(&counts, 0x5A, sizeof(counts));
    memset(output, 0xCD, sizeof(output));
    memcpy(before, output, sizeof(before));
    CHECK(backup_stream_import(
              stream, length, output, TEST_CAPACITY,
              &entry_count, &counts) == expected,
          message);
    CHECK(memcmp(output, before, sizeof(output)) == 0,
          "failed import leaves output entries unchanged");
    CHECK(entry_count == 0x5AU && counts.config_count == 0x5A5A5A5AU &&
              counts.audit_count == 0x5A5A5A5AU,
          "failed import leaves count outputs unchanged");
}

static void test_validation_and_status_strings(void) {
    static const enum backup_stream_status statuses[] = {
        BACKUP_STREAM_OK,
        BACKUP_STREAM_NULL,
        BACKUP_STREAM_BAD_STATE,
        BACKUP_STREAM_TOO_SMALL,
        BACKUP_STREAM_OVERFLOW,
        BACKUP_STREAM_UNSUPPORTED,
        BACKUP_STREAM_CORRUPT,
    };
    struct config_kv_entry entries[TEST_CAPACITY];
    struct config_kv kv;
    struct config_kv_entry output[TEST_CAPACITY];
    struct backup_stream_counts counts;
    unsigned char stream[TEST_STREAM_CAPACITY];
    size_t stream_length = 0U;
    size_t entry_count = 0U;
    size_t index;

    memset(entries, 0, sizeof(entries));
    memset(output, 0, sizeof(output));
    memset(stream, 0, sizeof(stream));
    CHECK(config_kv_init(&kv, entries, TEST_CAPACITY) == CONFIG_KV_OK,
          "backup fixture initializes kv");

    CHECK(backup_stream_export(NULL, stream, sizeof(stream),
                               &stream_length) == BACKUP_STREAM_NULL,
          "export rejects null kv");
    CHECK(backup_stream_export(&kv, NULL, sizeof(stream),
                               &stream_length) == BACKUP_STREAM_NULL,
          "export rejects null buffer");
    CHECK(backup_stream_export(&kv, stream, sizeof(stream),
                               NULL) == BACKUP_STREAM_NULL,
          "export rejects null length output");
    CHECK(backup_stream_import(NULL, 0U, output, TEST_CAPACITY,
                               &entry_count, &counts) == BACKUP_STREAM_NULL,
          "import rejects null stream");
    CHECK(backup_stream_import(stream, 0U, NULL, 0U,
                               NULL, &counts) == BACKUP_STREAM_NULL,
          "import rejects null entry count output");
    CHECK(backup_stream_import(stream, 0U, output, TEST_CAPACITY,
                               NULL, &counts) == BACKUP_STREAM_NULL,
          "import rejects null count output");
    CHECK(backup_stream_import(stream, 0U, output, TEST_CAPACITY,
                               &entry_count, NULL) == BACKUP_STREAM_NULL,
          "import rejects null counts output");
    CHECK(backup_stream_import(stream, 0U, NULL, TEST_CAPACITY,
                               &entry_count, &counts) == BACKUP_STREAM_NULL,
          "import rejects non-null entries with zero capacity");
    memset(&kv, 0, sizeof(kv));
    CHECK(backup_stream_export(&kv, stream, sizeof(stream),
                               &stream_length) == BACKUP_STREAM_BAD_STATE,
          "export rejects uninitialized kv");
    config_kv_init(&kv, entries, TEST_CAPACITY);

    CHECK(backup_stream_export(&kv, stream, sizeof(stream),
                               &stream_length) == BACKUP_STREAM_OK &&
              stream_length == BACKUP_STREAM_HEADER_SIZE +
                  BACKUP_STREAM_DIGEST_SIZE,
          "empty checkpoint exports a minimal valid stream");
    CHECK(backup_stream_export(&kv, stream, BACKUP_STREAM_HEADER_SIZE +
                               BACKUP_STREAM_DIGEST_SIZE - 1U,
                               &stream_length) == BACKUP_STREAM_TOO_SMALL,
          "short export buffer returns too-small");

    check_import_failure(stream, 0U, BACKUP_STREAM_CORRUPT,
                         "empty stream is corrupt");
    check_import_failure(stream, 1U, BACKUP_STREAM_CORRUPT,
                         "truncated stream is corrupt");
    check_import_failure(stream, stream_length + 1U, BACKUP_STREAM_CORRUPT,
                         "trailing byte is corrupt");

    for (index = 0U; index < sizeof(statuses) / sizeof(statuses[0]);
         ++index) {
        CHECK(backup_stream_status_string(statuses[index]) != NULL &&
                  strcmp(backup_stream_status_string(statuses[index]),
                         statuses[index] == BACKUP_STREAM_OK ?
                             "BACKUP_STREAM_OK" :
                         statuses[index] == BACKUP_STREAM_NULL ?
                             "BACKUP_STREAM_NULL" :
                         statuses[index] == BACKUP_STREAM_BAD_STATE ?
                             "BACKUP_STREAM_BAD_STATE" :
                         statuses[index] == BACKUP_STREAM_TOO_SMALL ?
                             "BACKUP_STREAM_TOO_SMALL" :
                         statuses[index] == BACKUP_STREAM_OVERFLOW ?
                             "BACKUP_STREAM_OVERFLOW" :
                         statuses[index] == BACKUP_STREAM_UNSUPPORTED ?
                             "BACKUP_STREAM_UNSUPPORTED" :
                             "BACKUP_STREAM_CORRUPT") == 0,
              "backup-stream status string is exact");
    }
    CHECK(strcmp(backup_stream_status_string(
                     (enum backup_stream_status)99U),
                 "BACKUP_STREAM_UNKNOWN") == 0,
          "backup-stream undefined status string is exact");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "backup-stream validation performs no allocation");
}

static void check_frame_failure(const unsigned char *stream,
                                size_t length,
                                enum backup_stream_status expected,
                                const char *message)
{
    struct backup_stream_layout before;
    struct backup_stream_layout layout;

    memset(&layout, 0x5A, sizeof(layout));
    memcpy(&before, &layout, sizeof(before));
    CHECK(backup_stream_frame_validate(stream, length, &layout) == expected,
          message);
    CHECK(memcmp(&layout, &before, sizeof(layout)) == 0,
          "failed frame validation leaves layout unchanged");
}

static void test_framing_validation_and_layout(void) {
    struct config_kv_entry entries[TEST_CAPACITY];
    struct config_kv_entry source_entries[TEST_CAPACITY];
    struct config_kv kv;
    struct config_kv source;
    struct backup_stream_layout layout;
    unsigned char stream[TEST_STREAM_CAPACITY];
    unsigned char copy[TEST_STREAM_CAPACITY];
    unsigned char oversized[BACKUP_STREAM_MAX_FRAMED_LENGTH + 1U];
    size_t stream_length = 0U;
    size_t populated_length = 0U;
    size_t digest_offset;
    size_t frame_offset;

    memset(entries, 0, sizeof(entries));
    CHECK(config_kv_init(&kv, entries, TEST_CAPACITY) == CONFIG_KV_OK,
          "framing fixture initializes empty kv");
    CHECK(backup_stream_export(&kv, stream, sizeof(stream),
                               &stream_length) == BACKUP_STREAM_OK,
          "framing fixture exports empty stream");

    memset(&layout, 0x5A, sizeof(layout));
    CHECK(backup_stream_frame_validate(NULL, stream_length,
                                       &layout) == BACKUP_STREAM_NULL,
          "frame validation rejects null stream");
    CHECK(backup_stream_frame_validate(stream, stream_length,
                                       NULL) == BACKUP_STREAM_NULL,
          "frame validation rejects null layout");

    memset(&layout, 0, sizeof(layout));
    CHECK(backup_stream_frame_validate(stream, stream_length, &layout) ==
              BACKUP_STREAM_OK &&
              layout.valid &&
              layout.header.valid &&
              layout.header.version == BACKUP_STREAM_FORMAT_VERSION &&
              layout.header.config_count == 0U &&
              layout.header.audit_count == 0U &&
              layout.entry_count == 0U &&
              layout.payload_length == 0U &&
              layout.digest_offset == stream_length -
                  BACKUP_STREAM_DIGEST_SIZE &&
              layout.stream_length == stream_length,
          "empty stream framing publishes exact layout");

    check_frame_failure(
        stream, BACKUP_STREAM_HEADER_SIZE + BACKUP_STREAM_DIGEST_SIZE - 1U,
        BACKUP_STREAM_CORRUPT, "truncated header framing is corrupt");

    memset(oversized, 0, sizeof(oversized));
    check_frame_failure(
        oversized, sizeof(oversized), BACKUP_STREAM_CORRUPT,
        "over-limit stream framing is corrupt");

    populate_state(&source, source_entries, false);
    CHECK(backup_stream_export(&source, stream, sizeof(stream),
                               &populated_length) == BACKUP_STREAM_OK,
          "framing fixture exports populated stream");
    memset(&layout, 0, sizeof(layout));
    CHECK(backup_stream_frame_validate(stream, populated_length, &layout) ==
              BACKUP_STREAM_OK &&
              layout.valid &&
              layout.header.valid &&
              layout.header.version == BACKUP_STREAM_FORMAT_VERSION &&
              layout.header.config_count == 2U &&
              layout.header.audit_count == 2U &&
              layout.entry_count == 4U &&
              layout.payload_length == populated_length -
                  BACKUP_STREAM_HEADER_SIZE - BACKUP_STREAM_DIGEST_SIZE &&
              layout.digest_offset == populated_length -
                  BACKUP_STREAM_DIGEST_SIZE &&
              layout.stream_length == populated_length,
          "populated stream framing publishes exact layout");

    digest_offset = populated_length - BACKUP_STREAM_DIGEST_SIZE;
    frame_offset = BACKUP_STREAM_HEADER_SIZE;
    memcpy(copy, stream, populated_length);
    check_frame_failure(copy, digest_offset - 1U, BACKUP_STREAM_CORRUPT,
                        "truncated entry frame is corrupt");

    memcpy(copy, stream, populated_length);
    copy[0U] ^= 0xFFU;
    check_frame_failure(copy, populated_length, BACKUP_STREAM_CORRUPT,
                        "bad framing magic is corrupt");

    memcpy(copy, stream, populated_length);
    copy[BACKUP_STREAM_MAGIC_SIZE] = BACKUP_STREAM_FORMAT_VERSION + 1U;
    check_frame_failure(copy, populated_length, BACKUP_STREAM_UNSUPPORTED,
                        "unsupported framing version is rejected");

    memcpy(copy, stream, populated_length);
    copy[BACKUP_STREAM_MAGIC_SIZE + 1U] = 1U;
    check_frame_failure(copy, populated_length, BACKUP_STREAM_CORRUPT,
                        "nonzero header reserved byte is corrupt");

    memcpy(copy, stream, populated_length);
    copy[frame_offset] = CONFIG_KV_KIND_COUNT;
    check_frame_failure(copy, populated_length, BACKUP_STREAM_CORRUPT,
                        "bad entry kind framing is corrupt");

    memcpy(copy, stream, populated_length);
    copy[frame_offset + 1U] = 0U;
    check_frame_failure(copy, populated_length, BACKUP_STREAM_CORRUPT,
                        "empty key length framing is corrupt");

    memcpy(copy, stream, populated_length);
    copy[frame_offset + 1U] = CONFIG_KV_KEY_MAX + 1U;
    check_frame_failure(copy, populated_length, BACKUP_STREAM_CORRUPT,
                        "over-limit key length framing is corrupt");

    memcpy(copy, stream, populated_length);
    store_le16(copy + frame_offset + 2U, CONFIG_KV_VALUE_MAX + 1U);
    check_frame_failure(copy, populated_length, BACKUP_STREAM_CORRUPT,
                        "over-limit value length framing is corrupt");

    memcpy(copy, stream, populated_length);
    store_le64(copy + frame_offset + 4U, 0U);
    check_frame_failure(copy, populated_length, BACKUP_STREAM_CORRUPT,
                        "zero revision framing is corrupt");

    memcpy(copy, stream, populated_length);
    copy[populated_length - 1U] ^= 0xFFU;
    check_frame_failure(copy, populated_length, BACKUP_STREAM_CORRUPT,
                        "digest mismatch framing is corrupt");

    memcpy(copy, stream, populated_length);
    check_frame_failure(copy, populated_length + 1U, BACKUP_STREAM_CORRUPT,
                        "trailing byte framing is corrupt");

    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "backup-stream framing performs no allocation");
}

static void populate_state(struct config_kv *kv,
                           struct config_kv_entry *entries,
                           bool reverse_slots)
{
    bool replaced = false;

    memset(entries, 0, sizeof(entries[0]) * TEST_CAPACITY);
    CHECK(config_kv_init(kv, entries, TEST_CAPACITY) == CONFIG_KV_OK,
          reverse_slots ?
              "reverse-slot kv fixture initializes" :
              "forward-slot kv fixture initializes");

    if (!reverse_slots) {
        CHECK(config_kv_put(kv, CONFIG_KV_KIND_CONFIG,
                            "alpha", 5U, "one", 3U, &replaced) ==
                  CONFIG_KV_OK &&
                  config_kv_put(kv, CONFIG_KV_KIND_AUDIT,
                                "alpha", 5U, "audit-one", 9U, &replaced) ==
                      CONFIG_KV_OK &&
                  config_kv_put(kv, CONFIG_KV_KIND_CONFIG,
                                "beta", 4U, "two", 3U, &replaced) ==
                      CONFIG_KV_OK &&
                  config_kv_put(kv, CONFIG_KV_KIND_AUDIT,
                                "beta", 4U, "audit-two", 9U, &replaced) ==
                      CONFIG_KV_OK,
              "forward-slot state populates");
        CHECK(config_kv_remove(kv, CONFIG_KV_KIND_CONFIG,
                               "beta", 4U, &replaced) == CONFIG_KV_OK &&
                  config_kv_put(kv, CONFIG_KV_KIND_CONFIG,
                                "beta", 4U, "two", 3U, &replaced) ==
                      CONFIG_KV_OK,
              "forward-slot state reuses a config hole");
        CHECK(config_kv_remove(kv, CONFIG_KV_KIND_AUDIT,
                               "beta", 4U, &replaced) == CONFIG_KV_OK &&
                  config_kv_put(kv, CONFIG_KV_KIND_AUDIT,
                                "beta", 4U, "audit-two", 9U, &replaced) ==
                      CONFIG_KV_OK,
              "forward-slot state reuses an audit hole");
    } else {
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
              "reverse-slot state populates");
        CHECK(config_kv_remove(kv, CONFIG_KV_KIND_CONFIG,
                               "beta", 4U, &replaced) == CONFIG_KV_OK &&
                  config_kv_put(kv, CONFIG_KV_KIND_CONFIG,
                                "beta", 4U, "two", 3U, &replaced) ==
                      CONFIG_KV_OK,
              "reverse-slot state reuses a config hole");
        CHECK(config_kv_remove(kv, CONFIG_KV_KIND_AUDIT,
                               "beta", 4U, &replaced) == CONFIG_KV_OK &&
                  config_kv_put(kv, CONFIG_KV_KIND_AUDIT,
                                "beta", 4U, "audit-two", 9U, &replaced) ==
                      CONFIG_KV_OK,
              "reverse-slot state reuses an audit hole");
    }
}

static void test_byte_stable_export_and_import(void) {
    struct config_kv_entry entries_a[TEST_CAPACITY];
    struct config_kv_entry entries_b[TEST_CAPACITY];
    struct config_kv_entry import_entries[TEST_CAPACITY];
    struct config_kv kv_a;
    struct config_kv kv_b;
    struct backup_stream_counts counts;
    unsigned char stream_a[TEST_STREAM_CAPACITY];
    unsigned char stream_b[TEST_STREAM_CAPACITY];
    unsigned char stream_c[TEST_STREAM_CAPACITY];
    size_t length_a = 0U;
    size_t length_b = 0U;
    size_t length_c = 0U;
    size_t entry_count = 0U;
    unsigned char before_import[sizeof(import_entries)];
    size_t index;

    populate_state(&kv_a, entries_a, false);
    populate_state(&kv_b, entries_b, true);

    CHECK(backup_stream_export(&kv_a, stream_a, sizeof(stream_a),
                               &length_a) == BACKUP_STREAM_OK &&
              backup_stream_export(&kv_b, stream_b, sizeof(stream_b),
                                   &length_b) == BACKUP_STREAM_OK &&
              length_a == length_b &&
              bytes_equal(stream_a, stream_b, length_a),
          "equal checkpoint state exports byte-identically across slot reuse");

    memset(stream_c, 0xA5, sizeof(stream_c));
    CHECK(backup_stream_export(&kv_a, stream_c, sizeof(stream_c),
                               &length_c) == BACKUP_STREAM_OK &&
              length_c == length_a &&
              bytes_equal(stream_a, stream_c, length_a),
          "repeated export is byte-identical");

    memset(import_entries, 0xCD, sizeof(import_entries));
    CHECK(backup_stream_import(stream_a, length_a,
                               import_entries, TEST_CAPACITY,
                               &entry_count, &counts) == BACKUP_STREAM_OK &&
              entry_count == 4U &&
              counts.config_count == 2U &&
              counts.audit_count == 2U,
          "valid backup stream imports with exact counts");

    index = 0U;
    CHECK(entry_equal(&import_entries[index],
                      CONFIG_KV_KIND_CONFIG,
                      (const unsigned char *)"beta", 4U,
                      (const unsigned char *)"two", 3U, 1U),
          "import publishes shorter config key first");
    ++index;
    CHECK(entry_equal(&import_entries[index],
                      CONFIG_KV_KIND_CONFIG,
                      (const unsigned char *)"alpha", 5U,
                      (const unsigned char *)"one", 3U, 1U),
          "import publishes longer config key second");
    ++index;
    CHECK(entry_equal(&import_entries[index],
                      CONFIG_KV_KIND_AUDIT,
                      (const unsigned char *)"beta", 4U,
                      (const unsigned char *)"audit-two", 9U, 1U),
          "import publishes shorter audit key third");
    ++index;
    CHECK(entry_equal(&import_entries[index],
                      CONFIG_KV_KIND_AUDIT,
                      (const unsigned char *)"alpha", 5U,
                      (const unsigned char *)"audit-one", 9U, 1U),
          "import publishes longer audit key fourth");

    memset(import_entries, 0xCD, sizeof(import_entries));
    memset(&counts, 0, sizeof(counts));
    CHECK(backup_stream_import(stream_a, length_a,
                               NULL, 0U, &entry_count, &counts) ==
              BACKUP_STREAM_OK &&
              entry_count == 4U &&
              counts.config_count == 2U &&
              counts.audit_count == 2U,
          "query import validates without output");

    memset(import_entries, 0xCD, sizeof(import_entries));
    memcpy(before_import, import_entries, sizeof(before_import));
    entry_count = 0x5AU;
    CHECK(backup_stream_import(stream_a, length_a,
                               import_entries, 2U,
                               &entry_count, &counts) ==
              BACKUP_STREAM_OVERFLOW,
          "undersized import returns overflow");
    CHECK(entry_count == 0x5AU,
          "overflow import leaves entry count unchanged");
    CHECK(memcmp(import_entries, before_import, sizeof(before_import)) == 0,
          "overflow import leaves destination bytes unchanged");
}

static void test_corrupt_stream_rejection(void) {
    struct config_kv_entry entries[TEST_CAPACITY];
    struct config_kv kv;
    unsigned char stream[TEST_STREAM_CAPACITY];
    unsigned char copy[TEST_STREAM_CAPACITY];
    unsigned char duplicate[128U];
    size_t stream_length = 0U;
    size_t copy_length = 0U;
    size_t duplicate_length = 0U;
    size_t frame_offset;

    populate_state(&kv, entries, false);
    CHECK(backup_stream_export(&kv, stream, sizeof(stream),
                               &stream_length) == BACKUP_STREAM_OK,
          "corrupt fixture exports");

    memcpy(copy, stream, stream_length);
    copy_length = stream_length;
    copy[0U] ^= 0xFFU;
    check_import_failure(copy, copy_length, BACKUP_STREAM_CORRUPT,
                         "bad magic is corrupt");

    memcpy(copy, stream, stream_length);
    copy[8U] = BACKUP_STREAM_FORMAT_VERSION + 1U;
    check_import_failure(copy, copy_length, BACKUP_STREAM_UNSUPPORTED,
                         "unsupported version is rejected");

    memcpy(copy, stream, stream_length);
    copy[9U] = 1U;
    check_import_failure(copy, copy_length, BACKUP_STREAM_CORRUPT,
                         "nonzero header reserved byte is corrupt");

    memcpy(copy, stream, stream_length);
    store_le32(copy + 16U, 99U);
    check_import_failure(copy, copy_length, BACKUP_STREAM_CORRUPT,
                         "config count mismatch is corrupt");

    memcpy(copy, stream, stream_length);
    store_le32(copy + 20U, 99U);
    check_import_failure(copy, copy_length, BACKUP_STREAM_CORRUPT,
                         "audit count mismatch is corrupt");

    frame_offset = BACKUP_STREAM_HEADER_SIZE;
    memcpy(copy, stream, stream_length);
    copy[frame_offset] = CONFIG_KV_KIND_COUNT;
    check_import_failure(copy, copy_length, BACKUP_STREAM_CORRUPT,
                         "bad entry kind is corrupt");

    memcpy(copy, stream, stream_length);
    copy[frame_offset + 1U] = 0U;
    check_import_failure(copy, copy_length, BACKUP_STREAM_CORRUPT,
                         "empty key length is corrupt");

    memcpy(copy, stream, stream_length);
    copy[frame_offset + 1U] = CONFIG_KV_KEY_MAX + 1U;
    check_import_failure(copy, copy_length, BACKUP_STREAM_CORRUPT,
                         "over-limit key length is corrupt");

    memcpy(copy, stream, stream_length);
    store_le16(copy + frame_offset + 2U, CONFIG_KV_VALUE_MAX + 1U);
    check_import_failure(copy, copy_length, BACKUP_STREAM_CORRUPT,
                         "over-limit value length is corrupt");

    memcpy(copy, stream, stream_length);
    store_le64(copy + frame_offset + 4U, 0U);
    check_import_failure(copy, copy_length, BACKUP_STREAM_CORRUPT,
                         "zero revision is corrupt");

    memcpy(copy, stream, stream_length);
    copy[copy_length - 1U] ^= 0xFFU;
    check_import_failure(copy, copy_length, BACKUP_STREAM_CORRUPT,
                         "digest mismatch is corrupt");

    build_duplicate_stream(duplicate, &duplicate_length);
    memset(copy, 0xCD, sizeof(copy));
    memcpy(copy, duplicate, duplicate_length);
    memcpy(copy + BACKUP_STREAM_HEADER_SIZE +
               BACKUP_STREAM_FRAME_HEADER_SIZE,
           "bbbb", 4U);
    check_import_failure(copy, duplicate_length, BACKUP_STREAM_CORRUPT,
                         "duplicate key is corrupt");

    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "backup-stream lifecycle performs no allocation");
}

int main(void) {
    test_validation_and_status_strings();
    test_framing_validation_and_layout();
    test_byte_stable_export_and_import();
    test_corrupt_stream_rejection();
    if (failures != 0) {
        fprintf(stderr, "Backup-stream host tests: FAILED (%d)\n", failures);
        return 1;
    }
    printf("Backup-stream host tests: ALL OK\n");
    return 0;
}
