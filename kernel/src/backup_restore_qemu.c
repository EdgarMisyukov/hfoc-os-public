#ifdef HFOC_BACKUP_RESTORE_QEMU_TEST

#include "backup_stream.h"
#include "config_kv.h"
#include "restore_validator.h"
#include "serial.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define BACKUP_RESTORE_QEMU_CAPACITY 8U
#define BACKUP_RESTORE_QEMU_STREAM_CAPACITY 4096U

static void backup_restore_qemu_zero(unsigned char *bytes, size_t length)
{
    size_t index;

    for (index = 0U; index < length; ++index) {
        bytes[index] = 0U;
    }
}

static bool backup_restore_qemu_equal(const unsigned char *left,
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

static void backup_restore_qemu_failure(const char *message)
    __attribute__((noreturn));

static void backup_restore_qemu_failure(const char *message)
{
    {
        const uint64_t serial_flags = serial_sequence_begin();
        serial_write("[BACKUP-RESTORE] failed: ");
        serial_write(message);
        serial_write("\n");
        serial_sequence_end(serial_flags);
    }
    for (;;) {
        __asm__ volatile ("hlt" : : : "memory");
    }
}

static void backup_restore_qemu_require(bool condition,
                                        const char *message)
{
    if (!condition) {
        backup_restore_qemu_failure(message);
    }
}

static void backup_restore_qemu_populate(struct config_kv *kv,
                                         struct config_kv_entry *entries)
{
    bool replaced = false;

    backup_restore_qemu_require(
        config_kv_init(kv, entries, BACKUP_RESTORE_QEMU_CAPACITY) ==
            CONFIG_KV_OK,
        "active kv initialization");
    backup_restore_qemu_require(
        config_kv_put(kv, CONFIG_KV_KIND_CONFIG,
                      "region", 6U, "golden", 6U, &replaced) ==
            CONFIG_KV_OK &&
        config_kv_put(kv, CONFIG_KV_KIND_CONFIG,
                      "service", 7U, "ready", 5U, &replaced) ==
            CONFIG_KV_OK &&
        config_kv_put(kv, CONFIG_KV_KIND_AUDIT,
                      "last_backup", 11U, "complete", 8U, &replaced) ==
            CONFIG_KV_OK &&
        config_kv_put(kv, CONFIG_KV_KIND_AUDIT,
                      "generation", 10U, "7", 1U, &replaced) ==
            CONFIG_KV_OK,
        "known state population");
}

static bool backup_restore_qemu_is_golden(const struct config_kv *kv)
{
    unsigned char value[CONFIG_KV_VALUE_MAX];
    size_t length = 0U;
    uint64_t revision = 0U;

    return config_kv_get(kv, CONFIG_KV_KIND_CONFIG,
                         "region", 6U, value, sizeof(value),
                         &length, &revision) == CONFIG_KV_OK &&
           length == 6U &&
           backup_restore_qemu_equal(value,
                                     (const unsigned char *)"golden", 6U) &&
           config_kv_get(kv, CONFIG_KV_KIND_CONFIG,
                         "service", 7U, value, sizeof(value),
                         &length, &revision) == CONFIG_KV_OK &&
           length == 5U &&
           backup_restore_qemu_equal(value,
                                     (const unsigned char *)"ready", 5U) &&
           config_kv_get(kv, CONFIG_KV_KIND_AUDIT,
                         "last_backup", 11U, value, sizeof(value),
                         &length, &revision) == CONFIG_KV_OK &&
           length == 8U &&
           backup_restore_qemu_equal(
               value, (const unsigned char *)"complete", 8U) &&
           config_kv_get(kv, CONFIG_KV_KIND_AUDIT,
                         "generation", 10U, value, sizeof(value),
                         &length, &revision) == CONFIG_KV_OK &&
           length == 1U &&
           backup_restore_qemu_equal(value,
                                     (const unsigned char *)"7", 1U);
}

static void backup_restore_qemu_mutate(struct config_kv *kv)
{
    bool removed = false;
    bool replaced = false;

    backup_restore_qemu_require(
        config_kv_remove(kv, CONFIG_KV_KIND_CONFIG,
                         "region", 6U, &removed) == CONFIG_KV_OK &&
            removed &&
        config_kv_put(kv, CONFIG_KV_KIND_CONFIG,
                      "region", 6U, "broken", 6U, &replaced) ==
            CONFIG_KV_OK &&
        config_kv_remove(kv, CONFIG_KV_KIND_AUDIT,
                         "generation", 10U, &removed) == CONFIG_KV_OK &&
            removed &&
        config_kv_put(kv, CONFIG_KV_KIND_AUDIT,
                      "generation", 10U, "corrupt", 7U, &replaced) ==
            CONFIG_KV_OK,
        "destructive state mutation");
}

void backup_restore_qemu_test_init(void)
{
    static struct config_kv_entry active_entries[
        BACKUP_RESTORE_QEMU_CAPACITY];
    static struct config_kv_entry rollback_entries[
        BACKUP_RESTORE_QEMU_CAPACITY];
    static struct config_kv active;
    static struct restore_plan plan;
    static struct restore_io io;
    static unsigned char stream[BACKUP_RESTORE_QEMU_STREAM_CAPACITY];
    static unsigned char restored_stream[
        BACKUP_RESTORE_QEMU_STREAM_CAPACITY];
    static size_t stream_length = 0U;
    static size_t restored_length = 0U;

    backup_restore_qemu_zero((unsigned char *)&active,
                             sizeof(active));
    backup_restore_qemu_zero((unsigned char *)&plan, sizeof(plan));
    backup_restore_qemu_zero((unsigned char *)&io, sizeof(io));
    backup_restore_qemu_zero((unsigned char *)stream, sizeof(stream));
    backup_restore_qemu_zero((unsigned char *)restored_stream,
                             sizeof(restored_stream));
    backup_restore_qemu_zero((unsigned char *)rollback_entries,
                             sizeof(rollback_entries));

    serial_write("[BACKUP-RESTORE] proof init\n");

    backup_restore_qemu_populate(&active, active_entries);
    backup_restore_qemu_require(
        backup_restore_qemu_is_golden(&active),
        "known active state verification");
    serial_write("[BACKUP-RESTORE] active state initialized\n");

    backup_restore_qemu_require(
        backup_stream_export(&active, stream, sizeof(stream),
                             &stream_length) == BACKUP_STREAM_OK &&
            stream_length > 0U,
        "backup stream export");
    serial_write("[BACKUP-RESTORE] stream exported\n");

    backup_restore_qemu_require(
        restore_dry_run_plan(stream, stream_length, &plan) ==
            RESTORE_OK &&
            plan.valid && plan.dry_run && plan.count == 4U,
        "dry-run restore plan");
    serial_write("[BACKUP-RESTORE] dry-run plan validated\n");

    backup_restore_qemu_mutate(&active);
    backup_restore_qemu_require(
        !backup_restore_qemu_is_golden(&active),
        "destructive change visibility");
    serial_write("[BACKUP-RESTORE] active state changed\n");

    io.rollback = rollback_entries;
    io.rollback_capacity = BACKUP_RESTORE_QEMU_CAPACITY;
    backup_restore_qemu_require(
        restore_apply(stream, stream_length, &active, &io) ==
            RESTORE_OK &&
            backup_restore_qemu_is_golden(&active),
        "restore apply restores golden state");
    serial_write("[BACKUP-RESTORE] values restored\n");

    backup_restore_qemu_require(
        backup_stream_export(&active, restored_stream,
                             sizeof(restored_stream),
                             &restored_length) == BACKUP_STREAM_OK &&
            restored_length == stream_length &&
            backup_restore_qemu_equal(stream, restored_stream,
                                      stream_length),
        "audit continuity after restore");
    serial_write("[BACKUP-RESTORE] audit continuity preserved\n");
    serial_write("[BACKUP-RESTORE] clean reboot\n");
    serial_write("[BACKUP-RESTORE] proof OK\n");
}

#else
typedef int backup_restore_qemu_translation_unit_guard;
#endif
