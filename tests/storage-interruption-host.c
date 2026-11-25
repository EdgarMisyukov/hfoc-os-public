#include "backup_producer.h"
#include "backup_stream.h"
#include "config_kv.h"
#include "fault_injection.h"
#include "journal_compaction.h"
#include "restore_validator.h"
#include "sha256.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifdef HFOC_STORAGE_INTERRUPTION_HOST_TEST

#define DRILL_CAPACITY 6U
#define DRILL_STREAM_CAPACITY 1024U

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

static void populate_state(struct config_kv *kv,
                           struct config_kv_entry *entries,
                           bool destructive)
{
    bool replaced = false;

    memset(entries, 0, sizeof(entries[0]) * DRILL_CAPACITY);
    CHECK(config_kv_init(kv, entries, DRILL_CAPACITY) == CONFIG_KV_OK,
          destructive ?
              "drill active fixture initializes kv" :
              "drill source fixture initializes kv");
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
          destructive ?
              "drill active fixture populates exact state" :
              "drill source fixture populates exact state");

    if (destructive) {
        CHECK(config_kv_remove(kv, CONFIG_KV_KIND_CONFIG,
                               "alpha", 5U, &replaced) == CONFIG_KV_OK &&
                  config_kv_put(kv, CONFIG_KV_KIND_CONFIG,
                                "alpha", 5U, "changed", 7U, &replaced) ==
                      CONFIG_KV_OK,
              "drill active fixture changes one config entry");
        CHECK(config_kv_remove(kv, CONFIG_KV_KIND_AUDIT,
                               "beta", 4U, &replaced) == CONFIG_KV_OK,
              "drill active fixture removes one audit entry");
    }
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
                memcmp(candidate->key, entry->key,
                       entry->key_length) == 0 &&
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

static void make_candidate(struct journal_compaction_candidate *candidate,
                           struct config_kv_entry *entries,
                           uint64_t sequence)
{
    struct config_kv kv;
    struct journal_compaction_checkpoint checkpoints[1];

    populate_state(&kv, entries, false);
    checkpoints[0].version = JOURNAL_COMPACTION_CHECKPOINT_VERSION;
    checkpoints[0].sequence = sequence;
    checkpoints[0].kv = &kv;
    CHECK(journal_compaction_select(candidate, checkpoints, 1U) ==
              JOURNAL_COMPACTION_OK &&
              candidate->source_sequence == sequence,
          "compaction drill selects exact candidate");
}

static bool exactly_one_active_generation(
    const struct journal_compaction_commit *commit)
{
    size_t active_count = 0U;
    size_t index;

    if (commit == NULL) {
        return false;
    }
    for (index = 0U;
         index < JOURNAL_COMPACTION_GENERATION_SLOT_COUNT;
         ++index) {
        if (commit->slots[index].active) {
            ++active_count;
        }
    }
    return active_count == 1U &&
           commit->active_slot <
               JOURNAL_COMPACTION_GENERATION_SLOT_COUNT &&
           commit->slots[commit->active_slot].active &&
           commit->slots[commit->active_slot].valid;
}

static bool no_interrupted_generation(
    const struct journal_compaction_commit *commit)
{
    size_t index;

    if (commit == NULL) {
        return false;
    }
    for (index = 0U;
         index < JOURNAL_COMPACTION_GENERATION_SLOT_COUNT;
         ++index) {
        if (commit->slots[index].interrupted) {
            return false;
        }
    }
    return true;
}

struct compaction_fault {
    uint8_t fail_phase;
    bool fail_once;
    bool injected;
    uint32_t writes;
};

static bool compaction_write(
    void *context,
    uint8_t phase,
    size_t slot_index,
    const struct journal_compaction_generation *generation)
{
    struct compaction_fault *fault = (struct compaction_fault *)context;

    (void)slot_index;
    (void)generation;
    ++fault->writes;
    if (fault->fail_once &&
        phase == fault->fail_phase &&
        !fault->injected) {
        fault->injected = true;
        return false;
    }
    return true;
}

static struct journal_compaction_io make_compaction_io(
    struct compaction_fault *fault)
{
    struct journal_compaction_io io;

    io.context = fault;
    io.write = compaction_write;
    return io;
}

static void test_catalogue_storage_faults(void)
{
    static const struct {
        enum fault_injection_id id;
        const char *name;
    } storage_faults[] = {
        {FAULT_INJECTION_CONFIG_JOURNAL_WRITE,
         "FAULT_INJECTION_CONFIG_JOURNAL_WRITE"},
        {FAULT_INJECTION_CONFIG_JOURNAL_READBACK,
         "FAULT_INJECTION_CONFIG_JOURNAL_READBACK"},
        {FAULT_INJECTION_BLOCK_DEVICE_WRITE,
         "FAULT_INJECTION_BLOCK_DEVICE_WRITE"},
        {FAULT_INJECTION_RESTORE_WRITE,
         "FAULT_INJECTION_RESTORE_WRITE"},
    };
    size_t index;

    alloc_attempts = 0U;
    free_attempts = 0U;
    for (index = 0U;
         index < sizeof(storage_faults) / sizeof(storage_faults[0]);
         ++index) {
        const struct fault_injection_site *site = NULL;

        CHECK(fault_injection_site_by_id(storage_faults[index].id,
                                         &site) == FAULT_INJECTION_OK &&
                  site != NULL &&
                  site->subsystem == FAULT_INJECTION_SUBSYSTEM_STORAGE &&
                  site->bounded &&
                  site->test_only &&
                  strcmp(site->name, storage_faults[index].name) == 0,
              "drill consumes exact storage fault point");
    }
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "catalogue drill performs no allocation");
}

static void test_compaction_interruption(void)
{
    struct config_kv_entry first_entries[DRILL_CAPACITY];
    struct config_kv_entry second_entries[DRILL_CAPACITY];
    struct journal_compaction_candidate first;
    struct journal_compaction_candidate second;
    struct journal_compaction_commit commit;
    struct compaction_fault fault;
    struct journal_compaction_io io;

    alloc_attempts = 0U;
    free_attempts = 0U;
    make_candidate(&first, first_entries, 1U);
    make_candidate(&second, second_entries, 2U);
    memset(&commit, 0, sizeof(commit));
    CHECK(journal_compaction_commit_init(&commit, &first) ==
              JOURNAL_COMPACTION_OK &&
              exactly_one_active_generation(&commit),
          "compaction drill initializes generation one");

    memset(&fault, 0, sizeof(fault));
    fault.fail_phase = 0U;
    fault.fail_once = true;
    fault.injected = false;
    io = make_compaction_io(&fault);
    CHECK(journal_compaction_publish(&commit, &second, &io) ==
              JOURNAL_COMPACTION_WRITE_FAULT,
          "phase zero interruption returns explicit write fault");
    CHECK(exactly_one_active_generation(&commit) &&
              journal_compaction_active(&commit) != NULL &&
              journal_compaction_active(&commit)->generation_id == 1U,
          "phase zero interruption keeps previous active generation");
    CHECK(commit.slots[1].interrupted && !commit.slots[1].staged,
          "phase zero interruption marks unstaged interrupted slot");

    memset(&fault, 0, sizeof(fault));
    io = make_compaction_io(&fault);
    CHECK(journal_compaction_recover(&commit, &io) ==
              JOURNAL_COMPACTION_OK &&
              exactly_one_active_generation(&commit) &&
              no_interrupted_generation(&commit),
          "phase zero recovery discards interrupted slot");
    CHECK(journal_compaction_publish(&commit, &second, &io) ==
              JOURNAL_COMPACTION_OK &&
              journal_compaction_active(&commit) != NULL &&
              journal_compaction_active(&commit)->generation_id == 2U &&
              exactly_one_active_generation(&commit) &&
              no_interrupted_generation(&commit),
          "clean re-publish commits generation two");

    memset(&commit, 0, sizeof(commit));
    CHECK(journal_compaction_commit_init(&commit, &first) ==
              JOURNAL_COMPACTION_OK,
          "phase one drill reinitializes generation one");
    memset(&fault, 0, sizeof(fault));
    fault.fail_phase = 1U;
    fault.fail_once = true;
    fault.injected = false;
    io = make_compaction_io(&fault);
    CHECK(journal_compaction_publish(&commit, &second, &io) ==
              JOURNAL_COMPACTION_WRITE_FAULT,
          "phase one interruption returns explicit write fault");
    CHECK(journal_compaction_active(&commit) != NULL &&
              journal_compaction_active(&commit)->generation_id == 1U &&
              exactly_one_active_generation(&commit),
          "phase one interruption keeps previous active generation");
    CHECK(commit.slots[1].interrupted && commit.slots[1].staged &&
              commit.slots[1].source_sequence == 2U,
          "phase one interruption retains staged recoverable generation");

    memset(&fault, 0, sizeof(fault));
    io = make_compaction_io(&fault);
    CHECK(journal_compaction_recover(&commit, &io) ==
              JOURNAL_COMPACTION_OK &&
              journal_compaction_active(&commit) != NULL &&
              journal_compaction_active(&commit)->generation_id == 2U &&
              exactly_one_active_generation(&commit) &&
              no_interrupted_generation(&commit),
          "phase one recovery commits staged generation");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "compaction drill performs no allocation");
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
          "backup drill reinitializes producer");
    for (;;) {
        enum backup_producer_status status;

        status = backup_producer_next(&producer, chunk, sizeof(chunk),
                                      &chunk_length);
        if (status == BACKUP_PRODUCER_DONE) {
            break;
        }
        CHECK(status == BACKUP_PRODUCER_OK,
              "backup drill producer emits atomic frame");
        CHECK(offset + chunk_length <= stream_capacity,
              "backup drill stream remains bounded");
        memcpy(stream + offset, chunk, chunk_length);
        offset += chunk_length;
    }
    *stream_length_out = offset;
}

static void test_backup_export_interruption(void)
{
    struct config_kv kv;
    struct config_kv_entry entries[DRILL_CAPACITY];
    unsigned char entries_before[sizeof(entries)];
    struct backup_producer producer;
    unsigned char chunk[BACKUP_PRODUCER_MIN_CHUNK_SIZE];
    unsigned char stream[DRILL_STREAM_CAPACITY];
    unsigned char expected[DRILL_STREAM_CAPACITY];
    struct backup_stream_layout layout;
    size_t chunk_length = 0U;
    size_t stream_length = 0U;
    size_t expected_length = 0U;

    alloc_attempts = 0U;
    free_attempts = 0U;
    populate_state(&kv, entries, false);
    memcpy(entries_before, entries, sizeof(entries));

    CHECK(backup_producer_init(&producer, &kv) == BACKUP_PRODUCER_OK,
          "backup drill initializes producer");
    CHECK(backup_producer_next(&producer, chunk, sizeof(chunk),
                               &chunk_length) == BACKUP_PRODUCER_OK &&
              chunk_length == BACKUP_PRODUCER_HEADER_SIZE,
          "backup drill emits header before interruption");
    CHECK(backup_producer_close(&producer) == BACKUP_PRODUCER_OK &&
              producer.closed,
          "backup drill closes interrupted export");
    CHECK(memcmp(entries, entries_before, sizeof(entries)) == 0,
          "interrupted backup export leaves source bytes unchanged");

    CHECK(backup_stream_export(&kv, expected, sizeof(expected),
                               &expected_length) == BACKUP_STREAM_OK,
          "backup drill exports expected one-shot stream");
    drain_producer(&kv, stream, sizeof(stream), &stream_length);
    CHECK(stream_length == expected_length &&
              memcmp(stream, expected, stream_length) == 0,
          "fresh backup producer completes byte-identical stream");
    CHECK(backup_stream_frame_validate(stream, stream_length,
                                       &layout) == BACKUP_STREAM_OK,
          "completed backup stream passes framing v1");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "backup interruption drill performs no allocation");
}

struct stage_fault {
    uint8_t fail_phase;
    bool fail_once;
    bool injected;
    uint32_t calls;
};

static bool stage_write(void *context,
                        uint8_t phase,
                        size_t slot_index,
                        const struct restore_plan *plan)
{
    struct stage_fault *fault = (struct stage_fault *)context;

    (void)slot_index;
    (void)plan;
    ++fault->calls;
    if (fault->fail_once &&
        phase == fault->fail_phase &&
        !fault->injected) {
        fault->injected = true;
        return false;
    }
    return true;
}

static void prepare_restore_plan(
    struct config_kv_entry *source_entries,
    unsigned char *stream,
    struct restore_plan *plan,
    size_t *stream_length)
{
    struct config_kv source;

    populate_state(&source, source_entries, false);
    CHECK(backup_stream_export(&source, stream, DRILL_STREAM_CAPACITY,
                               stream_length) == BACKUP_STREAM_OK,
          "restore drill exports source stream");
    memset(plan, 0, sizeof(*plan));
    CHECK(restore_validate(stream, *stream_length, plan) == RESTORE_OK &&
              plan->valid &&
              plan->count == 4U,
          "restore drill validates exact plan");
}

static void test_staged_restore_interruption(void)
{
    struct config_kv_entry source_entries[DRILL_CAPACITY];
    struct config_kv_entry active_entries[DRILL_CAPACITY];
    struct config_kv_entry staged_entries[DRILL_CAPACITY];
    struct restore_activation activation;
    struct restore_plan plan;
    unsigned char stream[DRILL_STREAM_CAPACITY];
    size_t stream_length = 0U;
    struct stage_fault fault;
    struct restore_stage_io io;
    unsigned char active_before[sizeof(active_entries)];
    size_t active_count_before;

    alloc_attempts = 0U;
    free_attempts = 0U;
    prepare_restore_plan(source_entries, stream, &plan, &stream_length);

    memset(&activation, 0, sizeof(activation));
    CHECK(restore_activation_init(&activation, active_entries,
                                  staged_entries,
                                  DRILL_CAPACITY) == RESTORE_OK,
          "restore drill initializes two-slot activation");
    populate_state(&activation.slots[activation.active_slot],
                   active_entries, true);
    active_count_before = activation.slots[activation.active_slot].count;
    memcpy(active_before, active_entries, sizeof(active_entries));

    memset(&fault, 0, sizeof(fault));
    fault.fail_phase = 0U;
    fault.fail_once = true;
    fault.injected = false;
    io.context = &fault;
    io.write = stage_write;
    CHECK(restore_stage_apply(&activation, &plan, &io) ==
              RESTORE_WRITE_FAULT &&
              fault.calls == 1U,
          "restore phase zero interruption returns explicit fault");
    CHECK(activation.active_slot == 0U &&
              activation.slots[activation.active_slot].count ==
                  active_count_before &&
              memcmp(activation.slots[activation.active_slot].entries,
                     active_before, sizeof(active_before)) == 0,
          "restore phase zero interruption preserves active state");
    CHECK(activation.slots[1U].count == 0U,
          "restore phase zero interruption leaves no ready staged slot");

    memset(&fault, 0, sizeof(fault));
    io.write = stage_write;
    CHECK(restore_stage_apply(&activation, &plan, &io) == RESTORE_OK &&
              restore_activation_active(&activation) != NULL &&
              active_equal_to_plan(restore_activation_active(&activation),
                                   &plan),
          "restore retry after phase zero commits plan");

    memset(&activation, 0, sizeof(activation));
    CHECK(restore_activation_init(&activation, active_entries,
                                  staged_entries,
                                  DRILL_CAPACITY) == RESTORE_OK,
          "restore phase one drill reinitializes activation");
    populate_state(&activation.slots[activation.active_slot],
                   active_entries, true);
    active_count_before = activation.slots[activation.active_slot].count;
    memcpy(active_before, active_entries, sizeof(active_entries));
    memset(&fault, 0, sizeof(fault));
    fault.fail_phase = 1U;
    fault.fail_once = true;
    fault.injected = false;
    io.write = stage_write;
    CHECK(restore_stage_apply(&activation, &plan, &io) ==
              RESTORE_WRITE_FAULT &&
              fault.calls == 2U,
          "restore phase one interruption returns explicit fault");
    CHECK(activation.active_slot == 0U &&
              activation.slots[activation.active_slot].count ==
                  active_count_before &&
              memcmp(activation.slots[activation.active_slot].entries,
                     active_before, sizeof(active_before)) == 0,
          "restore phase one interruption preserves active state");
    CHECK(activation.slots[1U].count == 0U,
          "restore phase one interruption leaves no ready staged slot");

    memset(&fault, 0, sizeof(fault));
    io.write = stage_write;
    CHECK(restore_stage_apply(&activation, &plan, &io) == RESTORE_OK &&
              restore_activation_active(&activation) != NULL &&
              active_equal_to_plan(restore_activation_active(&activation),
                                   &plan),
          "restore retry after phase one commits plan");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "restore interruption drill performs no allocation");
}

int main(void)
{
    test_catalogue_storage_faults();
    test_compaction_interruption();
    test_backup_export_interruption();
    test_staged_restore_interruption();

    if (failures != 0) {
        fprintf(stderr,
                "Storage interruption drill host tests: %d FAILURES\n",
                failures);
        return 1;
    }
    printf("Storage interruption drill host tests: ALL OK\n");
    return 0;
}

#else
#error "storage-interruption-host.c must be compiled with HFOC_STORAGE_INTERRUPTION_HOST_TEST"
#endif
