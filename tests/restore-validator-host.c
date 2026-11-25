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

static void populate_state(struct config_kv *kv,
                           struct config_kv_entry *entries,
                           bool destructive)
{
    bool replaced = false;
    size_t index;

    memset(entries, 0, sizeof(entries[0]) * TEST_CAPACITY);
    CHECK(config_kv_init(kv, entries, TEST_CAPACITY) == CONFIG_KV_OK,
          destructive ?
              "destructive restore fixture initializes kv" :
              "restore source fixture initializes kv");
    CHECK(config_kv_put(kv, CONFIG_KV_KIND_CONFIG,
                        "alpha", 5U, "one", 3U, &replaced) == CONFIG_KV_OK &&
              config_kv_put(kv, CONFIG_KV_KIND_CONFIG,
                            "beta", 4U, "two", 3U, &replaced) ==
                  CONFIG_KV_OK &&
              config_kv_put(kv, CONFIG_KV_KIND_AUDIT,
                            "alpha", 5U, "audit-one", 9U, &replaced) ==
                  CONFIG_KV_OK &&
              config_kv_put(kv, CONFIG_KV_KIND_AUDIT,
                            "beta", 4U, "audit-two", 9U, &replaced) ==
                  CONFIG_KV_OK,
          destructive ?
              "destructive restore fixture populates exact state" :
              "restore source fixture populates exact state");

    if (destructive) {
        CHECK(config_kv_remove(kv, CONFIG_KV_KIND_CONFIG,
                               "alpha", 5U, &replaced) == CONFIG_KV_OK &&
                  config_kv_put(kv, CONFIG_KV_KIND_CONFIG,
                                "alpha", 5U, "changed", 7U, &replaced) ==
                      CONFIG_KV_OK,
              "destructive restore fixture replaces one config entry");
        CHECK(config_kv_remove(kv, CONFIG_KV_KIND_AUDIT,
                               "beta", 4U, &replaced) == CONFIG_KV_OK,
              "destructive restore fixture removes one audit entry");
        for (index = 0U; index < kv->capacity; ++index) {
            if (kv->entries[index].occupied) {
                kv->entries[index].revision = 0xAAAAAAAABBBBBBBBULL;
            }
        }
    }
}

static void export_plan(struct config_kv *source,
                        struct config_kv_entry *source_entries,
                        unsigned char *stream,
                        size_t *stream_length,
                        struct restore_plan *plan)
{
    populate_state(source, source_entries, false);
    CHECK(backup_stream_export(source, stream, TEST_STREAM_CAPACITY,
                               stream_length) == BACKUP_STREAM_OK,
          "restore source exports a valid stream");
    memset(plan, 0x5A, sizeof(*plan));
    CHECK(restore_validate(stream, *stream_length, plan) == RESTORE_OK &&
              plan->valid &&
              plan->count == 4U &&
              plan->counts.config_count == 2U &&
              plan->counts.audit_count == 2U,
          "restore validation publishes exact plan counts");
}

struct write_fixture {
    size_t calls;
    size_t fail_index;
    bool never_fail;
};

static bool fixture_write(void *context, size_t write_index)
{
    struct write_fixture *fixture = (struct write_fixture *)context;

    ++fixture->calls;
    if (!fixture->never_fail && write_index == fixture->fail_index) {
        return false;
    }
    return true;
}

static bool active_equal_to_plan(const struct config_kv *active,
                                 const struct restore_plan *plan)
{
    size_t active_count = 0U;
    size_t matched = 0U;
    size_t index;
    size_t plan_index;

    if (active == NULL || plan == NULL || !plan->valid) {
        return false;
    }
    for (index = 0U; index < active->capacity; ++index) {
        if (active->entries[index].occupied) {
            ++active_count;
        }
    }
    if (active_count != plan->count) {
        return false;
    }
    for (index = 0U; index < active->capacity; ++index) {
        const struct config_kv_entry *entry = &active->entries[index];

        if (!entry->occupied) {
            continue;
        }
        for (plan_index = 0U; plan_index < plan->count; ++plan_index) {
            const struct config_kv_entry *candidate =
                &plan->entries[plan_index];

            if (candidate->occupied &&
                candidate->kind == entry->kind &&
                candidate->key_length == entry->key_length &&
                memcmp(candidate->key, entry->key, entry->key_length) == 0 &&
                candidate->value_length == entry->value_length &&
                memcmp(candidate->value, entry->value,
                       entry->value_length) == 0 &&
                candidate->revision == entry->revision) {
                ++matched;
                break;
            }
        }
    }
    return matched == plan->count;
}

static void test_validation_and_status_strings(void) {
    struct restore_plan plan;
    struct config_kv active;
    struct config_kv_entry active_entries[TEST_CAPACITY];
    struct config_kv_entry source_entries[TEST_CAPACITY];
    struct config_kv source;
    unsigned char stream[TEST_STREAM_CAPACITY];
    unsigned char bad[TEST_STREAM_CAPACITY];
    size_t stream_length = 0U;
    size_t bad_length = 0U;
    unsigned char active_before[sizeof(active_entries)];
    struct restore_plan plan_before;

    populate_state(&active, active_entries, true);
    memcpy(active_before, active_entries, sizeof(active_entries));
    export_plan(&source, source_entries, stream, &stream_length, &plan);

    CHECK(restore_validate(NULL, stream_length, &plan) == RESTORE_NULL,
          "restore validate rejects null stream");
    CHECK(restore_validate(stream, stream_length, NULL) == RESTORE_NULL,
          "restore validate rejects null plan");
    CHECK(restore_commit(NULL, &active, NULL) == RESTORE_NULL,
          "restore commit rejects null plan");
    CHECK(restore_commit(&plan, NULL, NULL) == RESTORE_NULL,
          "restore commit rejects null active kv");
    CHECK(restore_apply(NULL, stream_length, &active, NULL) == RESTORE_NULL,
          "restore apply rejects null stream");
    CHECK(restore_apply(stream, stream_length, NULL, NULL) == RESTORE_NULL,
          "restore apply rejects null active kv");
    CHECK(restore_status_string(RESTORE_OK) != NULL &&
              strcmp(restore_status_string(RESTORE_OK),
                     "RESTORE_OK") == 0 &&
              strcmp(restore_status_string(RESTORE_WRITE_FAULT),
                     "RESTORE_WRITE_FAULT") == 0,
          "restore status strings are exact");
    CHECK(strcmp(restore_status_string((enum restore_status)99U),
                 "RESTORE_UNKNOWN") == 0,
          "restore undefined status string is exact");

    memcpy(bad, stream, stream_length);
    bad_length = stream_length;
    bad[bad_length - 1U] ^= 0xFFU;
    memset(&plan, 0x5A, sizeof(plan));
    memcpy(&plan_before, &plan, sizeof(plan_before));
    CHECK(restore_validate(bad, bad_length, &plan) == RESTORE_CORRUPT &&
              memcmp(&plan, &plan_before, sizeof(plan)) == 0,
          "digest-failed stream leaves restore plan unchanged");
    CHECK(memcmp(active_entries, active_before, sizeof(active_entries)) == 0,
          "failed validation leaves active checkpoint unchanged");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "restore validation performs no allocation");
}

static void check_restore_failure_before_planning(
    const unsigned char *stream,
    size_t length,
    const char *message)
{
    struct restore_plan before;
    struct restore_plan plan;

    memset(&plan, 0x5A, sizeof(plan));
    memcpy(&before, &plan, sizeof(before));
    CHECK(restore_validate(stream, length, &plan) == RESTORE_CORRUPT,
          message);
    CHECK(memcmp(&plan, &before, sizeof(plan)) == 0,
          "malformed stream leaves restore plan bytes unchanged");
}

static void test_framing_fails_before_restore_planning(void) {
    struct config_kv active;
    struct config_kv_entry active_entries[TEST_CAPACITY];
    struct config_kv_entry source_entries[TEST_CAPACITY];
    struct config_kv source;
    unsigned char stream[TEST_STREAM_CAPACITY];
    unsigned char copy[TEST_STREAM_CAPACITY];
    unsigned char active_before[sizeof(active_entries)];
    size_t stream_length = 0U;
    size_t digest_offset;
    size_t frame_offset;

    populate_state(&source, source_entries, false);
    CHECK(backup_stream_export(&source, stream, TEST_STREAM_CAPACITY,
                               &stream_length) == BACKUP_STREAM_OK,
          "restore framing fixture exports a valid stream");
    populate_state(&active, active_entries, true);
    memcpy(active_before, active_entries, sizeof(active_entries));

    memcpy(copy, stream, stream_length);
    check_restore_failure_before_planning(
        copy, stream_length - 1U, "truncated stream fails before planning");

    digest_offset = stream_length - BACKUP_STREAM_DIGEST_SIZE;
    memcpy(copy, stream, digest_offset - 1U);
    check_restore_failure_before_planning(
        copy, digest_offset - 1U,
        "truncated frame fails before planning");

    memcpy(copy, stream, stream_length);
    copy[0U] ^= 0xFFU;
    check_restore_failure_before_planning(
        copy, stream_length, "bad magic fails before planning");

    memcpy(copy, stream, stream_length);
    copy[BACKUP_STREAM_MAGIC_SIZE] = BACKUP_STREAM_FORMAT_VERSION + 1U;
    check_restore_failure_before_planning(
        copy, stream_length, "unsupported version fails before planning");

    memcpy(copy, stream, stream_length);
    copy[BACKUP_STREAM_MAGIC_SIZE + 1U] = 1U;
    check_restore_failure_before_planning(
        copy, stream_length, "reserved-field corruption fails before planning");

    frame_offset = BACKUP_STREAM_HEADER_SIZE;
    memcpy(copy, stream, stream_length);
    copy[frame_offset] = CONFIG_KV_KIND_COUNT;
    check_restore_failure_before_planning(
        copy, stream_length, "bad frame kind fails before planning");

    memcpy(copy, stream, stream_length);
    copy[stream_length - 1U] ^= 0xFFU;
    check_restore_failure_before_planning(
        copy, stream_length, "digest failure fails before planning");

    CHECK(memcmp(active_entries, active_before, sizeof(active_entries)) == 0,
          "failed framing leaves active checkpoint unchanged");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "restore framing validation performs no allocation");
}

static void test_no_change_detection(void) {
    struct restore_plan plan;
    struct config_kv active;
    struct config_kv_entry active_entries[TEST_CAPACITY];
    struct config_kv_entry rollback_entries[TEST_CAPACITY];
    struct config_kv_entry source_entries[TEST_CAPACITY];
    struct config_kv source;
    unsigned char stream[TEST_STREAM_CAPACITY];
    size_t stream_length = 0U;
    struct write_fixture fixture;

    export_plan(&source, source_entries, stream, &stream_length, &plan);
    populate_state(&active, active_entries, false);
    memset(&fixture, 0, sizeof(fixture));
    fixture.never_fail = true;
    {
        struct restore_io io;

        io.context = &fixture;
        io.write = fixture_write;
        io.rollback = rollback_entries;
        io.rollback_capacity = TEST_CAPACITY;
        CHECK(restore_commit(&plan, &active, &io) == RESTORE_NO_CHANGE &&
                  fixture.calls == 0U,
              "equal active state returns no-change without writes");
    }
    CHECK(active_equal_to_plan(&active, &plan),
          "no-change leaves active logical state intact");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "no-change restore performs no allocation");
}

static void test_exact_publication(void) {
    struct restore_plan plan;
    struct config_kv active;
    struct config_kv_entry active_entries[TEST_CAPACITY];
    struct config_kv_entry rollback_entries[TEST_CAPACITY];
    struct config_kv_entry source_entries[TEST_CAPACITY];
    struct config_kv source;
    unsigned char stream[TEST_STREAM_CAPACITY];
    size_t stream_length = 0U;
    struct write_fixture fixture;
    struct restore_io io;

    export_plan(&source, source_entries, stream, &stream_length, &plan);
    populate_state(&active, active_entries, true);
    memset(&fixture, 0, sizeof(fixture));
    fixture.never_fail = true;
    io.context = &fixture;
    io.write = fixture_write;
    io.rollback = rollback_entries;
    io.rollback_capacity = TEST_CAPACITY;

    CHECK(restore_commit(&plan, &active, &io) == RESTORE_OK &&
              fixture.calls > 0U,
          "valid restore publishes exact checkpoint state");
    CHECK(active_equal_to_plan(&active, &plan),
          "published active state matches exact plan values and revisions");
    CHECK(active.count == plan.count,
          "published active count matches restore plan");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "exact restore publication performs no allocation");
}

static void test_write_fault_rollback(void) {
    struct restore_plan plan;
    struct config_kv active;
    struct config_kv_entry active_entries[TEST_CAPACITY];
    struct config_kv_entry rollback_entries[TEST_CAPACITY];
    struct config_kv_entry source_entries[TEST_CAPACITY];
    struct config_kv source;
    unsigned char stream[TEST_STREAM_CAPACITY];
    unsigned char active_before[sizeof(active_entries)];
    size_t stream_length = 0U;
    struct write_fixture fixture;
    struct restore_io io;

    export_plan(&source, source_entries, stream, &stream_length, &plan);
    populate_state(&active, active_entries, true);
    memcpy(active_before, active_entries, sizeof(active_entries));

    memset(&fixture, 0, sizeof(fixture));
    fixture.fail_index = 1U;
    io.context = &fixture;
    io.write = fixture_write;
    io.rollback = rollback_entries;
    io.rollback_capacity = TEST_CAPACITY;
    CHECK(restore_commit(&plan, &active, &io) == RESTORE_WRITE_FAULT &&
              fixture.calls >= 2U,
          "injected mid-publication write fault returns write fault");
    CHECK(memcmp(active_entries, active_before, sizeof(active_entries)) == 0,
          "write-fault restore rolls back active table bytes exactly");
    CHECK(active.count == 3U,
          "write-fault restore rolls back active count exactly");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "write-fault rollback performs no allocation");
}

static void test_apply_convenience(void) {
    struct config_kv active;
    struct config_kv_entry active_entries[TEST_CAPACITY];
    struct config_kv_entry rollback_entries[TEST_CAPACITY];
    struct config_kv_entry source_entries[TEST_CAPACITY];
    struct config_kv source;
    unsigned char stream[TEST_STREAM_CAPACITY];
    size_t stream_length = 0U;
    struct write_fixture fixture;
    struct restore_io io;

    populate_state(&source, source_entries, false);
    CHECK(backup_stream_export(&source, stream, TEST_STREAM_CAPACITY,
                               &stream_length) == BACKUP_STREAM_OK,
          "apply convenience exports source stream");
    populate_state(&active, active_entries, true);
    memset(&fixture, 0, sizeof(fixture));
    fixture.never_fail = true;
    io.context = &fixture;
    io.write = fixture_write;
    io.rollback = rollback_entries;
    io.rollback_capacity = TEST_CAPACITY;
    CHECK(restore_apply(stream, stream_length, &active, &io) == RESTORE_OK,
          "apply convenience validates and restores in one call");
    CHECK(active.count == 4U,
          "apply convenience publishes restored count");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "apply convenience performs no allocation");
}

int main(void) {
    test_validation_and_status_strings();
    test_framing_fails_before_restore_planning();
    test_no_change_detection();
    test_exact_publication();
    test_write_fault_rollback();
    test_apply_convenience();
    if (failures != 0) {
        fprintf(stderr, "Restore-validator host tests: FAILED (%d)\n",
                failures);
        return 1;
    }
    printf("Restore-validator host tests: ALL OK\n");
    return 0;
}
