#include "restore_validator.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define TEST_CAPACITY 6U
#define TEST_STREAM_CAPACITY 1024U
#define TEST_DEVICE_REGION_SIZE 512U
#define TEST_JOURNAL_REGION_SIZE 512U
#define TEST_SERVICE_REGION_SIZE 64U

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

struct restore_guard {
    struct config_kv active;
    struct config_kv_entry active_entries[TEST_CAPACITY];
    unsigned char device_region[TEST_DEVICE_REGION_SIZE];
    unsigned char journal_region[TEST_JOURNAL_REGION_SIZE];
    unsigned char service_region[TEST_SERVICE_REGION_SIZE];
};

struct guard_snapshot {
    unsigned char active_entries[sizeof(struct config_kv_entry) *
                                 TEST_CAPACITY];
    unsigned char device_region[TEST_DEVICE_REGION_SIZE];
    unsigned char journal_region[TEST_JOURNAL_REGION_SIZE];
    unsigned char service_region[TEST_SERVICE_REGION_SIZE];
};

struct write_fixture {
    size_t calls;
};

static void populate_source(struct config_kv *kv,
                            struct config_kv_entry *entries)
{
    bool replaced = false;
    size_t index;

    memset(entries, 0, sizeof(entries[0]) * TEST_CAPACITY);
    CHECK(config_kv_init(kv, entries, TEST_CAPACITY) == CONFIG_KV_OK,
          "dry-run source initializes kv");
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
          "dry-run source populates exact entries");
    for (index = 0U; index < kv->capacity; ++index) {
        if (kv->entries[index].occupied) {
            kv->entries[index].revision = 0x1111222233334444ULL;
        }
    }
}

static void populate_guard(struct restore_guard *guard)
{
    bool replaced = false;
    size_t index;

    memset(guard, 0, sizeof(*guard));
    CHECK(config_kv_init(&guard->active, guard->active_entries,
                         TEST_CAPACITY) == CONFIG_KV_OK,
          "dry-run guard initializes active kv");
    CHECK(config_kv_put(&guard->active, CONFIG_KV_KIND_CONFIG,
                        "alpha", 5U, "old-alpha", 9U, &replaced) ==
                  CONFIG_KV_OK &&
              config_kv_put(&guard->active, CONFIG_KV_KIND_CONFIG,
                            "beta", 4U, "old-beta", 8U, &replaced) ==
                  CONFIG_KV_OK &&
              config_kv_put(&guard->active, CONFIG_KV_KIND_AUDIT,
                            "alpha", 5U, "old-audit", 9U, &replaced) ==
                  CONFIG_KV_OK,
          "dry-run guard populates active state");

    for (index = 0U; index < sizeof(guard->device_region); ++index) {
        guard->device_region[index] = (unsigned char)(0xA0U + index);
    }
    memcpy(guard->journal_region, "HFOCJRNL", 8U);
    for (index = 8U; index < sizeof(guard->journal_region); ++index) {
        guard->journal_region[index] = (unsigned char)(0x40U + index);
    }
    memcpy(guard->service_region, "SERVICE-GUARD", 13U);
    for (index = 13U; index < sizeof(guard->service_region); ++index) {
        guard->service_region[index] = (unsigned char)(0x20U + index);
    }
}

static void snapshot_guard(const struct restore_guard *guard,
                           struct guard_snapshot *snapshot)
{
    memcpy(snapshot->active_entries, guard->active_entries,
           sizeof(snapshot->active_entries));
    memcpy(snapshot->device_region, guard->device_region,
           sizeof(snapshot->device_region));
    memcpy(snapshot->journal_region, guard->journal_region,
           sizeof(snapshot->journal_region));
    memcpy(snapshot->service_region, guard->service_region,
           sizeof(snapshot->service_region));
}

static void check_guard_unchanged(const struct restore_guard *guard,
                                  const struct guard_snapshot *snapshot,
                                  const char *message)
{
    CHECK(memcmp(guard->active_entries, snapshot->active_entries,
                 sizeof(snapshot->active_entries)) == 0,
          message);
    CHECK(memcmp(guard->device_region, snapshot->device_region,
                 sizeof(snapshot->device_region)) == 0,
          "dry run leaves device guard bytes unchanged");
    CHECK(memcmp(guard->journal_region, snapshot->journal_region,
                 sizeof(snapshot->journal_region)) == 0,
          "dry run leaves journal guard bytes unchanged");
    CHECK(memcmp(guard->service_region, snapshot->service_region,
                 sizeof(snapshot->service_region)) == 0,
          "dry run leaves live-service guard bytes unchanged");
}

static bool fixture_write(void *context, size_t write_index)
{
    struct write_fixture *fixture = (struct write_fixture *)context;

    (void)write_index;
    ++fixture->calls;
    return true;
}

static void export_stream(struct config_kv *source,
                          struct config_kv_entry *source_entries,
                          unsigned char *stream,
                          size_t *stream_length)
{
    populate_source(source, source_entries);
    CHECK(backup_stream_export(source, stream, TEST_STREAM_CAPACITY,
                               stream_length) == BACKUP_STREAM_OK,
          "dry-run source exports a valid stream");
}

static void test_validation_and_status_strings(void)
{
    struct restore_plan dry_run;
    struct restore_plan normal;
    struct config_kv source;
    struct config_kv_entry source_entries[TEST_CAPACITY];
    unsigned char stream[TEST_STREAM_CAPACITY];
    size_t stream_length = 0U;
    size_t index;

    export_stream(&source, source_entries, stream, &stream_length);

    CHECK(restore_dry_run_plan(NULL, stream_length, &dry_run) ==
              RESTORE_NULL,
          "dry-run planner rejects null stream");
    CHECK(restore_dry_run_plan(stream, stream_length, NULL) ==
              RESTORE_NULL,
          "dry-run planner rejects null plan");

    memset(&dry_run, 0x5A, sizeof(dry_run));
    CHECK(restore_dry_run_plan(stream, stream_length, &dry_run) ==
              RESTORE_OK &&
              dry_run.valid &&
              dry_run.dry_run &&
              dry_run.count == 4U &&
              dry_run.counts.config_count == 2U &&
              dry_run.counts.audit_count == 2U,
          "dry-run planner publishes exact unpublished plan");
    for (index = 0U; index < dry_run.count; ++index) {
        if (dry_run.entries[index].occupied &&
            dry_run.entries[index].kind == CONFIG_KV_KIND_CONFIG &&
            dry_run.entries[index].key_length == 5U &&
            memcmp(dry_run.entries[index].key, "alpha", 5U) == 0) {
            CHECK(dry_run.entries[index].revision ==
                      0x1111222233334444ULL,
                  "dry-run plan preserves exact revisions");
            break;
        }
    }
    CHECK(index < dry_run.count,
          "dry-run plan contains exact config key");

    memset(&normal, 0x5A, sizeof(normal));
    CHECK(restore_validate(stream, stream_length, &normal) == RESTORE_OK &&
              normal.valid &&
              !normal.dry_run,
          "normal restore validation publishes non-dry-run plan");

    CHECK(restore_status_string(RESTORE_DRY_RUN) != NULL &&
              strcmp(restore_status_string(RESTORE_DRY_RUN),
                     "RESTORE_DRY_RUN") == 0,
          "dry-run status string is exact");
    CHECK(strcmp(restore_status_string((enum restore_status)99U),
                 "RESTORE_UNKNOWN") == 0,
          "undefined restore status string is exact");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "dry-run planning performs no allocation");
}

static void test_failed_dry_run_leaves_plan_and_guards_unchanged(void)
{
    struct restore_guard guard;
    struct guard_snapshot snapshot;
    struct restore_plan before;
    struct restore_plan plan;
    struct config_kv source;
    struct config_kv_entry source_entries[TEST_CAPACITY];
    unsigned char stream[TEST_STREAM_CAPACITY];
    unsigned char bad[TEST_STREAM_CAPACITY];
    size_t stream_length = 0U;
    size_t bad_length;

    export_stream(&source, source_entries, stream, &stream_length);
    populate_guard(&guard);
    snapshot_guard(&guard, &snapshot);

    memset(&plan, 0x5A, sizeof(plan));
    memcpy(&before, &plan, sizeof(before));
    memcpy(bad, stream, stream_length);
    bad_length = stream_length;
    bad[bad_length - 1U] ^= 0xFFU;
    CHECK(restore_dry_run_plan(bad, bad_length, &plan) == RESTORE_CORRUPT,
          "digest-failed stream rejects dry run");
    CHECK(memcmp(&plan, &before, sizeof(plan)) == 0,
          "failed dry run leaves plan bytes unchanged");

    memset(&plan, 0x5A, sizeof(plan));
    memcpy(&before, &plan, sizeof(before));
    CHECK(restore_dry_run_plan(stream, stream_length - 1U, &plan) ==
              RESTORE_CORRUPT,
          "truncated stream rejects dry run");
    CHECK(memcmp(&plan, &before, sizeof(plan)) == 0,
          "truncated dry run leaves plan bytes unchanged");

    check_guard_unchanged(&guard, &snapshot,
                          "failed dry run leaves active checkpoint unchanged");
    CHECK(guard.active.count == 3U,
          "failed dry run leaves active count unchanged");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "failed dry-run planning performs no allocation");
}

static void test_dry_run_plan_is_unpublishable(void)
{
    struct restore_plan plan;
    struct restore_guard guard;
    struct guard_snapshot snapshot;
    struct config_kv_entry rollback[TEST_CAPACITY];
    struct config_kv source;
    struct config_kv_entry source_entries[TEST_CAPACITY];
    unsigned char stream[TEST_STREAM_CAPACITY];
    size_t stream_length = 0U;
    struct write_fixture fixture;
    struct restore_io io;

    export_stream(&source, source_entries, stream, &stream_length);
    CHECK(restore_dry_run_plan(stream, stream_length, &plan) == RESTORE_OK &&
              plan.valid &&
              plan.dry_run,
          "unpublishable fixture builds a valid dry-run plan");
    populate_guard(&guard);
    snapshot_guard(&guard, &snapshot);
    memset(&fixture, 0, sizeof(fixture));
    io.context = &fixture;
    io.write = fixture_write;
    io.rollback = rollback;
    io.rollback_capacity = TEST_CAPACITY;

    CHECK(restore_commit(&plan, &guard.active, &io) == RESTORE_DRY_RUN,
          "commit rejects dry-run plan");
    CHECK(fixture.calls == 0U,
          "dry-run rejection invokes no write callback");
    check_guard_unchanged(&guard, &snapshot,
                          "dry-run commit leaves active checkpoint unchanged");
    CHECK(guard.active.count == 3U,
          "dry-run commit leaves active count unchanged");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "dry-run commit rejection performs no allocation");
}

int main(void)
{
    test_validation_and_status_strings();
    test_failed_dry_run_leaves_plan_and_guards_unchanged();
    test_dry_run_plan_is_unpublishable();
    if (failures != 0) {
        fprintf(stderr, "Restore-planner host tests: FAILED (%d)\n",
                failures);
        return 1;
    }
    printf("Restore-planner host tests: ALL OK\n");
    return 0;
}
