#ifdef HFOC_STORAGE_INTERRUPTION_QEMU_TEST

#include "backup_producer.h"
#include "backup_stream.h"
#include "config_kv.h"
#include "fault_injection.h"
#include "journal_compaction.h"
#include "restore_validator.h"
#include "serial.h"
#include "sha256.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define STORAGE_INTERRUPTION_CAPACITY 6U
#define STORAGE_INTERRUPTION_STREAM_CAPACITY 1024U

static void storage_interruption_failure(const char *message)
    __attribute__((noreturn));

static void storage_interruption_failure(const char *message)
{
    {
        const uint64_t serial_flags = serial_sequence_begin();
        serial_write("[STORAGE-INTERRUPTION] failed: ");
        serial_write(message);
        serial_write("\n");
        serial_sequence_end(serial_flags);
    }
    for (;;) {
        __asm__ volatile ("hlt" : : : "memory");
    }
}

static void storage_interruption_require(bool condition,
                                         const char *message)
{
    if (!condition) {
        storage_interruption_failure(message);
    }
}

static bool storage_interruption_equal(const unsigned char *left,
                                       const unsigned char *right,
                                       size_t length)
{
    size_t index;

    for (index = 0U; index < length; ++index) {
        if (left[index] != right[index]) {
            return false;
        }
    }
    return true;
}

static void storage_interruption_populate(
    struct config_kv *kv,
    struct config_kv_entry *entries,
    bool destructive)
{
    bool replaced = false;

    memset(entries, 0, sizeof(entries[0]) *
           STORAGE_INTERRUPTION_CAPACITY);
    storage_interruption_require(
        config_kv_init(kv, entries,
                       STORAGE_INTERRUPTION_CAPACITY) == CONFIG_KV_OK,
        "drill kv initialization");
    storage_interruption_require(
        config_kv_put(kv, CONFIG_KV_KIND_CONFIG,
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
        "drill kv population");
    if (destructive) {
        storage_interruption_require(
            config_kv_remove(kv, CONFIG_KV_KIND_CONFIG,
                             "alpha", 5U, &replaced) == CONFIG_KV_OK &&
                config_kv_put(kv, CONFIG_KV_KIND_CONFIG,
                              "alpha", 5U, "changed", 7U, &replaced) ==
                    CONFIG_KV_OK &&
                config_kv_remove(kv, CONFIG_KV_KIND_AUDIT,
                                 "beta", 4U, &replaced) == CONFIG_KV_OK,
            "drill destructive state");
    }
}

static bool storage_interruption_active_equal_plan(
    const struct config_kv *active,
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

static void storage_interruption_make_candidate(
    struct journal_compaction_candidate *candidate,
    struct config_kv_entry *entries,
    uint64_t sequence)
{
    struct config_kv kv;
    struct journal_compaction_checkpoint checkpoints[1];

    storage_interruption_populate(&kv, entries, false);
    checkpoints[0].version = JOURNAL_COMPACTION_CHECKPOINT_VERSION;
    checkpoints[0].sequence = sequence;
    checkpoints[0].kv = &kv;
    storage_interruption_require(
        journal_compaction_select(candidate, checkpoints, 1U) ==
            JOURNAL_COMPACTION_OK &&
        candidate->source_sequence == sequence,
        "compaction candidate selection");
}

static bool storage_interruption_active_generation_is(
    const struct journal_compaction_commit *commit,
    uint64_t generation)
{
    const struct journal_compaction_generation *active;
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
    active = journal_compaction_active(commit);
    return active_count == 1U &&
           active != NULL &&
           active->generation_id == generation;
}

static bool storage_interruption_has_no_interrupted(
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

struct storage_compaction_fault {
    uint8_t fail_phase;
    bool enabled;
    bool injected;
};

static bool storage_compaction_write(
    void *context,
    uint8_t phase,
    size_t slot_index,
    const struct journal_compaction_generation *generation)
{
    struct storage_compaction_fault *fault =
        (struct storage_compaction_fault *)context;

    (void)slot_index;
    (void)generation;
    if (fault->enabled &&
        phase == fault->fail_phase &&
        !fault->injected) {
        fault->injected = true;
        return false;
    }
    return true;
}

static struct journal_compaction_io storage_compaction_io(
    struct storage_compaction_fault *fault)
{
    struct journal_compaction_io io;

    io.context = fault;
    io.write = storage_compaction_write;
    return io;
}

static void storage_interruption_drain_producer(
    const struct config_kv *kv,
    unsigned char *stream,
    size_t capacity,
    size_t *length_out)
{
    struct backup_producer producer;
    unsigned char chunk[BACKUP_PRODUCER_MIN_CHUNK_SIZE];
    size_t chunk_length = 0U;
    size_t offset = 0U;

    storage_interruption_require(
        backup_producer_init(&producer, kv) == BACKUP_PRODUCER_OK,
        "backup producer reinit");
    for (;;) {
        enum backup_producer_status status;

        status = backup_producer_next(&producer, chunk, sizeof(chunk),
                                      &chunk_length);
        if (status == BACKUP_PRODUCER_DONE) {
            break;
        }
        storage_interruption_require(
            status == BACKUP_PRODUCER_OK &&
                offset + chunk_length <= capacity,
            "backup producer atomic frame");
        memcpy(stream + offset, chunk, chunk_length);
        offset += chunk_length;
    }
    *length_out = offset;
}

struct storage_stage_fault {
    uint8_t fail_phase;
    bool enabled;
    bool injected;
};

static bool storage_stage_write(
    void *context,
    uint8_t phase,
    size_t slot_index,
    const struct restore_plan *plan)
{
    struct storage_stage_fault *fault =
        (struct storage_stage_fault *)context;

    (void)slot_index;
    (void)plan;
    if (fault->enabled &&
        phase == fault->fail_phase &&
        !fault->injected) {
        fault->injected = true;
        return false;
    }
    return true;
}

static void storage_interruption_check_restore_fault(
    struct restore_activation *activation,
    const struct restore_plan *plan,
    struct storage_stage_fault *fault,
    unsigned char *active_before)
{
    struct restore_stage_io io;
    const struct config_kv *active;
    size_t active_count_before;

    active = restore_activation_active(activation);
    storage_interruption_require(active != NULL, "restore active access");
    active_count_before = active->count;
    memcpy(active_before, active->entries,
           sizeof(active->entries[0]) * active->capacity);
    io.context = fault;
    io.write = storage_stage_write;
    storage_interruption_require(
        restore_stage_apply(activation, plan, &io) == RESTORE_WRITE_FAULT,
        "restore stage fault status");
    active = restore_activation_active(activation);
    storage_interruption_require(
        active != NULL &&
        active->count == active_count_before &&
        memcmp(active->entries, active_before,
               sizeof(active->entries[0]) * active->capacity) == 0,
        "restore stage active preservation");
    storage_interruption_require(
        activation->slots[1U - activation->active_slot].count == 0U,
        "restore stage discard");
}

void storage_interruption_qemu_test_init(void)
{
    /*
     * ITEM-393: the fixtures live in .bss, not on the boot stack.
     *
     * This proof is called once from kmain, before the scheduler exists, so it
     * runs on the stack the bootloader handed over. Its fixtures add up to
     * more than 60 KiB, which used to be one stack frame of exactly that size -
     * within touching distance of the page tables the bootloader keeps just
     * below the stack. On the acceptance-qemu run the storage-interruption
     * target died with an instruction-fetch page fault inside
     * restore_activation_init (CR2 = RIP inside kernel text, CR3 pointing at
     * the bootloader's PML4 whose page the frame had overwritten), then a
     * double-fault handler that faulted the same way, then a triple fault and
     * a silent reboot. A proof may not spend the boot stack, so every fixture
     * and snapshot below is file-scope storage: the function stays
     * single-shot and non-reentrant exactly as before, and the frame becomes
     * a few bytes.
     */
    static struct config_kv_entry first_entries[STORAGE_INTERRUPTION_CAPACITY];
    static struct config_kv_entry second_entries[STORAGE_INTERRUPTION_CAPACITY];
    static struct journal_compaction_candidate first;
    static struct journal_compaction_candidate second;
    static struct journal_compaction_commit commit;
    static struct storage_compaction_fault compaction_fault;
    static struct journal_compaction_io compaction_io;
    static struct config_kv kv;
    static struct config_kv_entry entries[STORAGE_INTERRUPTION_CAPACITY];
    static unsigned char entries_before[sizeof(entries)];
    static struct backup_producer producer;
    static unsigned char chunk[BACKUP_PRODUCER_MIN_CHUNK_SIZE];
    static unsigned char stream[STORAGE_INTERRUPTION_STREAM_CAPACITY];
    static unsigned char expected[STORAGE_INTERRUPTION_STREAM_CAPACITY];
    static struct backup_stream_layout layout;
    static struct config_kv_entry source_entries[STORAGE_INTERRUPTION_CAPACITY];
    static struct config_kv_entry active_entries[STORAGE_INTERRUPTION_CAPACITY];
    static struct config_kv_entry staged_entries[STORAGE_INTERRUPTION_CAPACITY];
    static struct restore_activation activation;
    static struct restore_plan plan;
    static struct storage_stage_fault stage_fault;
    static struct restore_stage_io stage_io;
    static unsigned char active_before[sizeof(active_entries)];
    static struct config_kv source;
    const struct fault_injection_site *site = NULL;
    size_t chunk_length = 0U;
    size_t stream_length = 0U;
    size_t expected_length = 0U;
    size_t plan_length = 0U;

    serial_write("[STORAGE-INTERRUPTION] proof init\n");

    storage_interruption_require(
        fault_injection_site_by_id(FAULT_INJECTION_CONFIG_JOURNAL_WRITE,
                                   &site) == FAULT_INJECTION_OK &&
            site->subsystem == FAULT_INJECTION_SUBSYSTEM_STORAGE &&
            site->bounded && site->test_only &&
        fault_injection_site_by_id(FAULT_INJECTION_CONFIG_JOURNAL_READBACK,
                                   &site) == FAULT_INJECTION_OK &&
            site->subsystem == FAULT_INJECTION_SUBSYSTEM_STORAGE &&
            site->bounded && site->test_only &&
        fault_injection_site_by_id(FAULT_INJECTION_BLOCK_DEVICE_WRITE,
                                   &site) == FAULT_INJECTION_OK &&
            site->subsystem == FAULT_INJECTION_SUBSYSTEM_STORAGE &&
            site->bounded && site->test_only &&
        fault_injection_site_by_id(FAULT_INJECTION_RESTORE_WRITE,
                                   &site) == FAULT_INJECTION_OK &&
            site->subsystem == FAULT_INJECTION_SUBSYSTEM_STORAGE &&
            site->bounded && site->test_only,
        "catalogue storage fault points");
    serial_write("[STORAGE-INTERRUPTION] catalogue storage faults exact\n");

    storage_interruption_make_candidate(&first, first_entries, 1U);
    storage_interruption_make_candidate(&second, second_entries, 2U);
    memset(&commit, 0, sizeof(commit));
    storage_interruption_require(
        journal_compaction_commit_init(&commit, &first) ==
            JOURNAL_COMPACTION_OK,
        "compaction initial generation");
    memset(&compaction_fault, 0, sizeof(compaction_fault));
    compaction_fault.fail_phase = 0U;
    compaction_fault.enabled = true;
    compaction_io = storage_compaction_io(&compaction_fault);
    storage_interruption_require(
        journal_compaction_publish(&commit, &second, &compaction_io) ==
            JOURNAL_COMPACTION_WRITE_FAULT &&
        storage_interruption_active_generation_is(&commit, 1U),
        "phase zero active preservation");
    memset(&compaction_fault, 0, sizeof(compaction_fault));
    compaction_io = storage_compaction_io(&compaction_fault);
    storage_interruption_require(
        journal_compaction_recover(&commit, &compaction_io) ==
            JOURNAL_COMPACTION_OK &&
        storage_interruption_has_no_interrupted(&commit),
        "phase zero recovery");
    storage_interruption_require(
        journal_compaction_publish(&commit, &second, &compaction_io) ==
            JOURNAL_COMPACTION_OK &&
        storage_interruption_active_generation_is(&commit, 2U),
        "clean compaction re-publish");
    serial_write("[STORAGE-INTERRUPTION] compaction phase0 recovered\n");

    memset(&commit, 0, sizeof(commit));
    storage_interruption_require(
        journal_compaction_commit_init(&commit, &first) ==
            JOURNAL_COMPACTION_OK,
        "compaction phase one reinit");
    memset(&compaction_fault, 0, sizeof(compaction_fault));
    compaction_fault.fail_phase = 1U;
    compaction_fault.enabled = true;
    compaction_io = storage_compaction_io(&compaction_fault);
    storage_interruption_require(
        journal_compaction_publish(&commit, &second, &compaction_io) ==
            JOURNAL_COMPACTION_WRITE_FAULT &&
        storage_interruption_active_generation_is(&commit, 1U) &&
        commit.slots[1].staged && commit.slots[1].interrupted,
        "phase one active and staged preservation");
    memset(&compaction_fault, 0, sizeof(compaction_fault));
    compaction_io = storage_compaction_io(&compaction_fault);
    storage_interruption_require(
        journal_compaction_recover(&commit, &compaction_io) ==
            JOURNAL_COMPACTION_OK &&
        storage_interruption_active_generation_is(&commit, 2U) &&
        storage_interruption_has_no_interrupted(&commit),
        "phase one recovery commit");
    serial_write("[STORAGE-INTERRUPTION] compaction phase1 recovered\n");

    storage_interruption_populate(&kv, entries, false);
    memcpy(entries_before, entries, sizeof(entries));
    storage_interruption_require(
        backup_producer_init(&producer, &kv) == BACKUP_PRODUCER_OK,
        "backup producer init");
    storage_interruption_require(
        backup_producer_next(&producer, chunk, sizeof(chunk),
                             &chunk_length) == BACKUP_PRODUCER_OK &&
            chunk_length == BACKUP_PRODUCER_HEADER_SIZE,
        "backup header before interruption");
    storage_interruption_require(
        backup_producer_close(&producer) == BACKUP_PRODUCER_OK &&
        memcmp(entries, entries_before, sizeof(entries)) == 0,
        "backup source unchanged after interruption");
    serial_write("[STORAGE-INTERRUPTION] backup source intact\n");
    storage_interruption_require(
        backup_stream_export(&kv, expected, sizeof(expected),
                             &expected_length) == BACKUP_STREAM_OK,
        "backup expected export");
    storage_interruption_drain_producer(
        &kv, stream, sizeof(stream), &stream_length);
    storage_interruption_require(
        stream_length == expected_length &&
        storage_interruption_equal(stream, expected, stream_length) &&
        backup_stream_frame_validate(stream, stream_length,
                                     &layout) == BACKUP_STREAM_OK,
        "backup completed stream validation");
    serial_write("[STORAGE-INTERRUPTION] backup stream valid\n");

    storage_interruption_populate(&source, source_entries, false);
    storage_interruption_require(
        backup_stream_export(&source, stream, sizeof(stream),
                             &plan_length) == BACKUP_STREAM_OK,
        "restore source export");
    memset(&plan, 0, sizeof(plan));
    storage_interruption_require(
        restore_validate(stream, plan_length, &plan) == RESTORE_OK &&
        plan.valid && plan.count == 4U,
        "restore plan validation");
    memset(&activation, 0, sizeof(activation));
    storage_interruption_require(
        restore_activation_init(&activation, active_entries, staged_entries,
                                STORAGE_INTERRUPTION_CAPACITY) == RESTORE_OK,
        "restore activation init");
    storage_interruption_populate(
        &activation.slots[activation.active_slot], active_entries, true);
    memset(&stage_fault, 0, sizeof(stage_fault));
    stage_fault.fail_phase = 0U;
    stage_fault.enabled = true;
    storage_interruption_check_restore_fault(
        &activation, &plan, &stage_fault, active_before);
    memset(&stage_fault, 0, sizeof(stage_fault));
    stage_io.context = &stage_fault;
    stage_io.write = storage_stage_write;
    storage_interruption_require(
        restore_stage_apply(&activation, &plan, &stage_io) == RESTORE_OK &&
        restore_activation_active(&activation) != NULL &&
        storage_interruption_active_equal_plan(
            restore_activation_active(&activation), &plan),
        "restore phase zero retry");
    serial_write("[STORAGE-INTERRUPTION] restore stage preserved\n");

    memset(&activation, 0, sizeof(activation));
    storage_interruption_require(
        restore_activation_init(&activation, active_entries, staged_entries,
                                STORAGE_INTERRUPTION_CAPACITY) == RESTORE_OK,
        "restore phase one reinit");
    storage_interruption_populate(
        &activation.slots[activation.active_slot], active_entries, true);
    memset(&stage_fault, 0, sizeof(stage_fault));
    stage_fault.fail_phase = 1U;
    stage_fault.enabled = true;
    storage_interruption_check_restore_fault(
        &activation, &plan, &stage_fault, active_before);
    memset(&stage_fault, 0, sizeof(stage_fault));
    stage_io.context = &stage_fault;
    stage_io.write = storage_stage_write;
    storage_interruption_require(
        restore_stage_apply(&activation, &plan, &stage_io) == RESTORE_OK &&
        storage_interruption_active_equal_plan(
            restore_activation_active(&activation), &plan),
        "restore phase one retry");
    serial_write("[STORAGE-INTERRUPTION] restore retry committed\n");

    serial_write("[STORAGE-INTERRUPTION] clean shutdown\n");
    serial_write("[STORAGE-INTERRUPTION] proof OK\n");
}

#else
typedef int storage_interruption_qemu_translation_unit_guard;
#endif
