#include "restore_validator.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define TEST_CAPACITY 6U
#define TEST_OVERFLOW_CAPACITY 8U
#define TEST_STREAM_CAPACITY 2048U

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

struct stage_fixture {
    size_t calls;
    bool fail_phase0;
    bool fail_phase1;
};

static bool stage_write(void *context,
                        uint8_t phase,
                        size_t slot_index,
                        const struct restore_plan *plan)
{
    struct stage_fixture *fixture = (struct stage_fixture *)context;

    (void)slot_index;
    (void)plan;
    ++fixture->calls;
    if (phase == 0U && fixture->fail_phase0) {
        return false;
    }
    if (phase == 1U && fixture->fail_phase1) {
        return false;
    }
    return true;
}

static void populate_state(struct config_kv *kv,
                           struct config_kv_entry *entries,
                           bool destructive)
{
    bool replaced = false;
    size_t index;

    memset(entries, 0, sizeof(entries[0]) * TEST_CAPACITY);
    CHECK(config_kv_init(kv, entries, TEST_CAPACITY) == CONFIG_KV_OK,
          "restore activation fixture initializes kv");
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
          "restore activation fixture populates exact state");

    if (destructive) {
        CHECK(config_kv_remove(kv, CONFIG_KV_KIND_CONFIG,
                               "alpha", 5U, &replaced) == CONFIG_KV_OK &&
                  config_kv_put(kv, CONFIG_KV_KIND_CONFIG,
                                "alpha", 5U, "changed", 7U, &replaced) ==
                      CONFIG_KV_OK,
              "restore activation fixture changes active config");
        CHECK(config_kv_remove(kv, CONFIG_KV_KIND_AUDIT,
                               "beta", 4U, &replaced) == CONFIG_KV_OK,
              "restore activation fixture changes active audit");
        for (index = 0U; index < kv->capacity; ++index) {
            if (kv->entries[index].occupied) {
                kv->entries[index].revision = 0x1111111122222222ULL;
            }
        }
    } else {
        for (index = 0U; index < kv->capacity; ++index) {
            if (kv->entries[index].occupied) {
                kv->entries[index].revision = 0x3333333344444444ULL;
            }
        }
    }
}

static void populate_overflow_source(struct config_kv *kv,
                                     struct config_kv_entry *entries)
{
    bool replaced = false;
    size_t index;

    memset(entries, 0, sizeof(entries[0]) * TEST_OVERFLOW_CAPACITY);
    CHECK(config_kv_init(kv, entries, TEST_OVERFLOW_CAPACITY) ==
              CONFIG_KV_OK,
          "overflow fixture initializes kv");
    for (index = 0U; index < 7U; ++index) {
        char key[3] = {'k', (char)('0' + index), '\0'};
        char value[3] = {'v', (char)('0' + index), '\0'};

        CHECK(config_kv_put(kv, CONFIG_KV_KIND_CONFIG,
                            key, 2U, value, 2U, &replaced) ==
                  CONFIG_KV_OK,
              "overflow fixture populates seven entries");
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
          "restore activation source exports a valid stream");
    memset(plan, 0x5A, sizeof(*plan));
    CHECK(restore_validate(stream, *stream_length, plan) == RESTORE_OK &&
              plan->valid &&
              plan->count == 4U &&
              plan->counts.config_count == 2U &&
              plan->counts.audit_count == 2U,
          "restore validation publishes exact activation plan counts");
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

static void test_init_and_status_strings(void)
{
    struct restore_activation activation;
    struct config_kv_entry active_entries[TEST_CAPACITY];
    struct config_kv_entry staged_entries[TEST_CAPACITY];
    const struct config_kv *active;

    CHECK(restore_activation_init(NULL, active_entries, staged_entries,
                                  TEST_CAPACITY) == RESTORE_NULL,
          "activation init rejects null activation");
    CHECK(restore_activation_init(&activation, NULL, staged_entries,
                                  TEST_CAPACITY) == RESTORE_NULL,
          "activation init rejects null active entries");
    CHECK(restore_activation_init(&activation, active_entries, NULL,
                                  TEST_CAPACITY) == RESTORE_NULL,
          "activation init rejects null staged entries");
    CHECK(restore_activation_init(&activation, active_entries, staged_entries,
                                  0U) == RESTORE_BAD_STAGE,
          "activation init rejects zero capacity");
    CHECK(restore_activation_init(&activation, active_entries, staged_entries,
                                  CONFIG_KV_CAPACITY_MAX + 1U) ==
              RESTORE_BAD_STAGE,
          "activation init rejects excessive capacity");

    memset(&activation, 0, sizeof(activation));
    CHECK(restore_activation_init(&activation, active_entries, staged_entries,
                                  TEST_CAPACITY) == RESTORE_OK &&
              activation.initialized &&
              activation.active_slot == 0U,
          "activation init publishes initialized slot zero");
    CHECK(restore_activation_init(&activation, active_entries, staged_entries,
                                  TEST_CAPACITY) == RESTORE_BAD_STAGE,
          "activation init rejects reinitialization");
    active = restore_activation_active(&activation);
    CHECK(active != NULL &&
              active->initialized &&
              active->capacity == TEST_CAPACITY &&
              active->count == 0U,
          "active getter returns initialized slot zero");
    CHECK(restore_activation_active(NULL) == NULL,
          "active getter rejects null activation");

    CHECK(strcmp(restore_status_string(RESTORE_BAD_STAGE),
                 "RESTORE_BAD_STAGE") == 0,
          "bad-stage status string is exact");
    CHECK(strcmp(restore_status_string((enum restore_status)99U),
                 "RESTORE_UNKNOWN") == 0,
          "undefined restore status string is exact");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "activation init performs no allocation");
}

static void test_validation_and_no_change(void)
{
    struct restore_activation activation;
    struct config_kv_entry active_entries[TEST_CAPACITY];
    struct config_kv_entry staged_entries[TEST_CAPACITY];
    struct config_kv_entry source_entries[TEST_CAPACITY];
    struct config_kv source;
    unsigned char stream[TEST_STREAM_CAPACITY];
    size_t stream_length = 0U;
    struct restore_plan plan;
    struct restore_plan dry_run;
    struct restore_plan invalid;
    struct stage_fixture fixture;
    struct restore_stage_io io;
    unsigned char active_before[sizeof(active_entries)];

    export_plan(&source, source_entries, stream, &stream_length, &plan);
    memset(&activation, 0, sizeof(activation));
    CHECK(restore_activation_init(&activation, active_entries, staged_entries,
                                  TEST_CAPACITY) == RESTORE_OK,
          "validation fixture initializes activation");
    populate_state(&activation.slots[activation.active_slot],
                   active_entries, true);
    memcpy(active_before, active_entries, sizeof(active_entries));

    memset(&fixture, 0, sizeof(fixture));
    io.context = &fixture;
    io.write = stage_write;

    CHECK(restore_stage_apply(NULL, &plan, &io) == RESTORE_NULL,
          "stage apply rejects null activation");
    CHECK(restore_stage_apply(&activation, NULL, &io) == RESTORE_NULL,
          "stage apply rejects null plan");
    CHECK(restore_stage_apply(&activation, &plan, NULL) == RESTORE_NULL,
          "stage apply rejects null io");

    memset(&invalid, 0, sizeof(invalid));
    CHECK(restore_stage_apply(&activation, &invalid, &io) ==
              RESTORE_BAD_STATE,
          "stage apply rejects invalid plan");
    CHECK(restore_dry_run_plan(stream, stream_length, &dry_run) ==
              RESTORE_OK &&
              restore_stage_apply(&activation, &dry_run, &io) ==
                  RESTORE_DRY_RUN,
          "stage apply rejects dry-run plan");
    CHECK(fixture.calls == 0U,
          "invalid and dry-run rejection invokes no stage callback");
    CHECK(memcmp(active_entries, active_before, sizeof(active_entries)) == 0,
          "invalid and dry-run rejection preserves active bytes");

    memset(&fixture, 0, sizeof(fixture));
    populate_state(&activation.slots[activation.active_slot],
                   active_entries, false);
    CHECK(restore_stage_apply(&activation, &plan, &io) == RESTORE_NO_CHANGE &&
              fixture.calls == 0U,
          "equal active state returns no-change without callback");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "stage validation performs no allocation");
}

static void test_overflow_rejected_before_mutation(void)
{
    struct restore_activation activation;
    struct config_kv_entry active_entries[TEST_CAPACITY];
    struct config_kv_entry staged_entries[TEST_CAPACITY];
    struct config_kv_entry source_entries[TEST_OVERFLOW_CAPACITY];
    struct config_kv source;
    unsigned char stream[TEST_STREAM_CAPACITY];
    size_t stream_length = 0U;
    struct restore_plan plan;
    struct stage_fixture fixture;
    struct restore_stage_io io;

    populate_overflow_source(&source, source_entries);
    CHECK(backup_stream_export(&source, stream, TEST_STREAM_CAPACITY,
                               &stream_length) == BACKUP_STREAM_OK &&
              restore_validate(stream, stream_length, &plan) == RESTORE_OK &&
              plan.count == 7U,
          "overflow fixture validates seven-entry plan");
    memset(&activation, 0, sizeof(activation));
    CHECK(restore_activation_init(&activation, active_entries, staged_entries,
                                  TEST_CAPACITY) == RESTORE_OK,
          "overflow fixture initializes six-slot activation");
    populate_state(&activation.slots[activation.active_slot],
                   active_entries, false);
    memset(&fixture, 0, sizeof(fixture));
    io.context = &fixture;
    io.write = stage_write;

    CHECK(restore_stage_apply(&activation, &plan, &io) == RESTORE_OVERFLOW &&
              fixture.calls == 0U &&
              activation.slots[activation.active_slot].count == 4U,
          "stage apply rejects overflow before mutation");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "overflow rejection performs no allocation");
}

static void test_successful_staged_activation(void)
{
    struct restore_activation activation;
    struct config_kv_entry active_entries[TEST_CAPACITY];
    struct config_kv_entry staged_entries[TEST_CAPACITY];
    struct config_kv_entry source_entries[TEST_CAPACITY];
    struct config_kv source;
    unsigned char stream[TEST_STREAM_CAPACITY];
    size_t stream_length = 0U;
    struct restore_plan plan;
    struct stage_fixture fixture;
    struct restore_stage_io io;
    const struct config_kv *active;
    const struct config_kv *inactive;
    unsigned char active_before[sizeof(active_entries)];
    size_t active_count_before;

    export_plan(&source, source_entries, stream, &stream_length, &plan);
    memset(&activation, 0, sizeof(activation));
    CHECK(restore_activation_init(&activation, active_entries, staged_entries,
                                  TEST_CAPACITY) == RESTORE_OK,
          "success fixture initializes activation");
    populate_state(&activation.slots[activation.active_slot],
                   active_entries, true);
    active_count_before = activation.slots[activation.active_slot].count;
    memcpy(active_before, active_entries, sizeof(active_entries));
    memset(&fixture, 0, sizeof(fixture));
    io.context = &fixture;
    io.write = stage_write;

    CHECK(restore_stage_apply(&activation, &plan, &io) == RESTORE_OK &&
              fixture.calls == 2U,
          "stage apply completes phase zero and phase one");
    active = restore_activation_active(&activation);
    CHECK(active != NULL &&
              active_equal_to_plan(active, &plan),
          "successful stage apply publishes exact active plan");
    CHECK(activation.active_slot == 1U,
          "successful stage apply switches active slot");
    inactive = &activation.slots[1U - activation.active_slot];
    CHECK(inactive->count == active_count_before &&
              memcmp(inactive->entries, active_before,
                     sizeof(active_before)) == 0,
          "previous active bytes remain only in inactive slot");
    CHECK(active_equal_to_plan(active, &plan),
          "published active state is exact after activation");

    memset(&fixture, 0, sizeof(fixture));
    CHECK(restore_stage_apply(&activation, &plan, &io) == RESTORE_NO_CHANGE &&
              fixture.calls == 0U &&
              activation.active_slot == 1U,
          "repeat equal activation remains no-change");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "successful activation performs no allocation");
}

static void test_phase_faults_preserve_active(void)
{
    struct restore_activation activation;
    struct config_kv_entry active_entries[TEST_CAPACITY];
    struct config_kv_entry staged_entries[TEST_CAPACITY];
    struct config_kv_entry source_entries[TEST_CAPACITY];
    struct config_kv source;
    unsigned char stream[TEST_STREAM_CAPACITY];
    size_t stream_length = 0U;
    struct restore_plan plan;
    struct stage_fixture fixture;
    struct restore_stage_io io;
    const struct config_kv *active;
    unsigned char active_before[sizeof(active_entries)];
    size_t active_count_before;

    export_plan(&source, source_entries, stream, &stream_length, &plan);
    memset(&activation, 0, sizeof(activation));
    CHECK(restore_activation_init(&activation, active_entries, staged_entries,
                                  TEST_CAPACITY) == RESTORE_OK,
          "fault fixture initializes activation");
    populate_state(&activation.slots[activation.active_slot],
                   active_entries, true);
    active_count_before = activation.slots[activation.active_slot].count;
    memcpy(active_before, active_entries, sizeof(active_entries));
    memset(&fixture, 0, sizeof(fixture));
    fixture.fail_phase0 = true;
    io.context = &fixture;
    io.write = stage_write;

    CHECK(restore_stage_apply(&activation, &plan, &io) ==
              RESTORE_WRITE_FAULT &&
              fixture.calls == 1U,
          "phase zero fault returns write fault");
    active = restore_activation_active(&activation);
    CHECK(active != NULL &&
              active->count == active_count_before &&
              memcmp(active->entries, active_before,
                     sizeof(active_before)) == 0 &&
              activation.active_slot == 0U,
          "phase zero fault preserves active slot and bytes");
    CHECK(activation.slots[1U - activation.active_slot].count == 0U,
          "phase zero fault leaves no ready staged slot");

    memset(&fixture, 0, sizeof(fixture));
    CHECK(restore_stage_apply(&activation, &plan, &io) == RESTORE_OK &&
              restore_activation_active(&activation) != NULL &&
              active_equal_to_plan(restore_activation_active(&activation),
                                   &plan),
          "retry after phase zero fault publishes plan");

    memset(&activation, 0, sizeof(activation));
    CHECK(restore_activation_init(&activation, active_entries, staged_entries,
                                  TEST_CAPACITY) == RESTORE_OK,
          "phase one fixture reinitializes activation");
    populate_state(&activation.slots[activation.active_slot],
                   active_entries, true);
    active_count_before = activation.slots[activation.active_slot].count;
    memcpy(active_before, active_entries, sizeof(active_entries));
    memset(&fixture, 0, sizeof(fixture));
    fixture.fail_phase1 = true;

    CHECK(restore_stage_apply(&activation, &plan, &io) ==
              RESTORE_WRITE_FAULT &&
              fixture.calls == 2U,
          "phase one fault returns write fault");
    active = restore_activation_active(&activation);
    CHECK(active != NULL &&
              active->count == active_count_before &&
              memcmp(active->entries, active_before,
                     sizeof(active_before)) == 0 &&
              activation.active_slot == 0U,
          "phase one fault preserves active slot and bytes");
    CHECK(activation.slots[1U - activation.active_slot].count == 0U,
          "phase one fault leaves no ready staged slot");

    memset(&fixture, 0, sizeof(fixture));
    CHECK(restore_stage_apply(&activation, &plan, &io) == RESTORE_OK &&
              active_equal_to_plan(restore_activation_active(&activation),
                                   &plan),
          "retry after phase one fault publishes plan");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "fault and retry paths perform no allocation");
}

int main(void)
{
    test_init_and_status_strings();
    test_validation_and_no_change();
    test_overflow_rejected_before_mutation();
    test_successful_staged_activation();
    test_phase_faults_preserve_active();
    if (failures != 0) {
        fprintf(stderr, "Restore-activation host tests: FAILED (%d)\n",
                failures);
        return 1;
    }
    printf("Restore-activation host tests: ALL OK\n");
    return 0;
}
