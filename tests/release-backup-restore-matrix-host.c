#include "restore_validator.h"

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

static void populate_source(struct config_kv *kv,
                            struct config_kv_entry *entries)
{
    bool replaced = false;
    size_t index;

    memset(entries, 0, sizeof(entries[0]) * TEST_CAPACITY);
    CHECK(config_kv_init(kv, entries, TEST_CAPACITY) == CONFIG_KV_OK,
          "release matrix fixture initializes source kv");
    CHECK(config_kv_put(kv, CONFIG_KV_KIND_CONFIG,
                        "alpha", 5U, "one", 3U, &replaced) ==
                  CONFIG_KV_OK &&
              config_kv_put(kv, CONFIG_KV_KIND_CONFIG,
                            "beta", 4U, "two", 3U, &replaced) ==
                  CONFIG_KV_OK &&
              config_kv_put(kv, CONFIG_KV_KIND_AUDIT,
                            "alpha", 5U, "audit-one", 9U, &replaced) ==
                  CONFIG_KV_OK &&
              config_kv_put(kv, CONFIG_KV_KIND_AUDIT,
                            "beta", 4U, "audit-two", 9U, &replaced) ==
                  CONFIG_KV_OK,
          "release matrix fixture populates source state");
    for (index = 0U; index < kv->capacity; ++index) {
        if (kv->entries[index].occupied) {
            kv->entries[index].revision = 0x1111222233334444ULL;
        }
    }
}

static void populate_active(struct config_kv *kv,
                            struct config_kv_entry *entries)
{
    bool replaced = false;
    size_t index;

    memset(entries, 0, sizeof(entries[0]) * TEST_CAPACITY);
    CHECK(config_kv_init(kv, entries, TEST_CAPACITY) == CONFIG_KV_OK,
          "release matrix fixture initializes active kv");
    CHECK(config_kv_put(kv, CONFIG_KV_KIND_CONFIG,
                        "alpha", 5U, "changed", 7U, &replaced) ==
                  CONFIG_KV_OK &&
              config_kv_put(kv, CONFIG_KV_KIND_CONFIG,
                            "beta", 4U, "two", 3U, &replaced) ==
                  CONFIG_KV_OK &&
              config_kv_put(kv, CONFIG_KV_KIND_AUDIT,
                            "alpha", 5U, "audit-old", 9U, &replaced) ==
                  CONFIG_KV_OK &&
              config_kv_put(kv, CONFIG_KV_KIND_AUDIT,
                            "beta", 4U, "audit-two", 9U, &replaced) ==
                  CONFIG_KV_OK,
          "release matrix fixture populates active state");
    for (index = 0U; index < kv->capacity; ++index) {
        if (kv->entries[index].occupied) {
            kv->entries[index].revision = 0xAABBCCDDEEFF0011ULL;
        }
    }
}

static bool fixture_write(void *context, size_t write_index)
{
    (void)context;
    (void)write_index;
    return true;
}

static void test_incompatible_restore_no_change(void)
{
    struct config_kv source;
    struct config_kv_entry source_entries[TEST_CAPACITY];
    struct config_kv active;
    struct config_kv_entry active_entries[TEST_CAPACITY];
    struct config_kv_entry rollback[TEST_CAPACITY];
    unsigned char stream[TEST_STREAM_CAPACITY];
    unsigned char incompatible[TEST_STREAM_CAPACITY];
    unsigned char active_before[sizeof(active_entries)];
    unsigned char plan_before[sizeof(struct restore_plan)];
    struct restore_plan plan;
    struct backup_stream_layout layout;
    struct restore_io io;
    size_t stream_length = 0U;

    populate_source(&source, source_entries);
    CHECK(backup_stream_export(&source, stream, sizeof(stream),
                               &stream_length) == BACKUP_STREAM_OK &&
              stream_length > BACKUP_STREAM_HEADER_SIZE +
                  BACKUP_STREAM_DIGEST_SIZE,
          "release matrix fixture exports a non-empty valid stream");

    populate_active(&active, active_entries);
    memcpy(active_before, active_entries, sizeof(active_entries));
    memcpy(incompatible, stream, stream_length);
    incompatible[BACKUP_STREAM_MAGIC_SIZE] =
        (unsigned char)(BACKUP_STREAM_FORMAT_VERSION + 1U);

    CHECK(backup_stream_frame_validate(
              incompatible, stream_length, &layout) ==
              BACKUP_STREAM_UNSUPPORTED,
          "incompatible backup stream format is rejected by framing");

    memset(&plan, 0x5A, sizeof(plan));
    memcpy(plan_before, &plan, sizeof(plan));
    CHECK(restore_validate(incompatible, stream_length, &plan) ==
              RESTORE_CORRUPT,
          "incompatible backup stream is rejected before restore planning");
    CHECK(memcmp(&plan, plan_before, sizeof(plan)) == 0,
          "incompatible restore leaves restore plan bytes unchanged");
    CHECK(memcmp(active_entries, active_before,
                 sizeof(active_entries)) == 0,
          "incompatible restore leaves active checkpoint unchanged");

    memset(&io, 0, sizeof(io));
    io.write = fixture_write;
    io.rollback = rollback;
    io.rollback_capacity = TEST_CAPACITY;
    CHECK(restore_apply(incompatible, stream_length, &active, &io) ==
              RESTORE_CORRUPT,
          "incompatible restore apply is rejected before any write");
    CHECK(memcmp(active_entries, active_before,
                 sizeof(active_entries)) == 0,
          "incompatible restore apply preserves no-change rollback");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "incompatible restore proof performs no allocation");
}

int main(void)
{
    test_incompatible_restore_no_change();
    if (failures != 0) {
        fprintf(stderr, "Release backup/restore matrix no-change host test: "
                        "FAILED (%d)\n", failures);
        return 1;
    }
    printf("Release backup/restore matrix no-change host test: OK\n");
    return 0;
}
