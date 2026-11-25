#!/usr/bin/env bash
# ITEM-487: contracted slice of the HFOC OS check suite. Sourced in pinned
#           order by scripts/check.sh; lint it on its own with shellcheck.

# The durable boot log's flush: one range write per run of consecutive slots,
# the records retired only after the bytes are on the device, and the reading
# that names the cost.
#
# `hw15` measured a 64-record flush at 2.7-3.2 s of blocked main loop and about
# 42 ms for every 512-byte record, because the flush wrote one slot per range
# call and every call resolved the path again and walked the file's whole FAT
# chain.  This slice pins the fix and the reading that proves it:
#
#   * `appliance_boot_log_ring_peek()` reads the `index`-th pending record
#     without retiring anything, so a flush can render a run before writing it;
#   * the flush collects consecutive slots into a bounded buffer, hands them to
#     the range writer once, and retires them only after the write returned, so
#     a refused write leaves the whole run staged;
#   * the run ends where the extent's own ring wraps and never spans it;
#   * `[BOOTLOG-FLUSH]` and `[BOOTLOG-FLUSH-TAIL]` name the records, the slot
#     bytes, the measured write, commit and total spans, the two spans inside
#     the writes and the number of range writes, and both halves fit the bound
#     the journal carries;
#   * the ESP writer's probe is a reading, not a policy.

for item487_file in \
    kernel/include/appliance_boot_log.h \
    kernel/include/appliance_boot_log_store.h \
    kernel/include/efi_fat32_esp.h \
    kernel/src/appliance_boot_log.c \
    kernel/src/appliance_boot_log_store.c \
    kernel/src/efi_fat32_esp.c \
    kernel/src/kernel.c \
    tests/appliance-boot-log-store-host.c \
    tests/efi-fat32-esp-host.c \
    docs/hfoc-appliance-boot-console.md; do
    test -f "$item487_file" || {
        echo "ITEM-487: отсутствует файл: $item487_file" >&2
        exit 1
    }
done

# The reading without retiring is a public, pure reading of the ring.
grep -Fq 'appliance_boot_log_ring_peek' \
    kernel/include/appliance_boot_log.h || {
    echo "ITEM-487: ring_peek не объявлен в заголовке кольца" >&2
    exit 1
}
grep -Fq 'appliance_boot_log_ring_peek(' kernel/src/appliance_boot_log.c || {
    echo "ITEM-487: ring_peek не реализован" >&2
    exit 1
}
# A peek that retired a record would move `written_sequence`, and the flush
# would retire records whose bytes never reached the extent.  The body is
# bounded by the function's own closing brace at column zero.
awk '/^enum appliance_boot_log_status appliance_boot_log_ring_peek\(/{inside=1}
     inside && /^}/{inside=0}
     inside && /written_sequence[[:space:]]*(\+=|=)/{found=1}
     END{exit found ? 1 : 0}' kernel/src/appliance_boot_log.c || {
    echo "ITEM-487: ring_peek двигает written_sequence, то есть не является чтением" >&2
    exit 1
}

# One run, one range write, and the records retired only after it.
grep -Fq 'APPLIANCE_BOOT_LOG_PENDING_CAPACITY * APPLIANCE_BOOT_LOG_SLOT_SIZE' \
    kernel/src/appliance_boot_log_store.c || {
    echo "ITEM-487: буфер пачки не ограничен кольцом pending" >&2
    exit 1
}
grep -Fq 'store_flush_batch' kernel/src/appliance_boot_log_store.c || {
    echo "ITEM-487: пачка слотов не собирается в буфер" >&2
    exit 1
}
grep -Fq 'plan.slot_index !=' kernel/src/appliance_boot_log_store.c || {
    echo "ITEM-487: пачка не прекращается на перехода через кольцо файла" >&2
    exit 1
}
item487_write_line=$(grep -n 'store_flush_batch, batch_bytes' \
    kernel/src/appliance_boot_log_store.c | head -1 | cut -d: -f1)
item487_retire_line=$(grep -n 'appliance_boot_log_ring_mark_written(' \
    kernel/src/appliance_boot_log_store.c | head -1 | cut -d: -f1)
[[ -n $item487_write_line && -n $item487_retire_line ]] || {
    echo "ITEM-487: запись пачки или retirement не найдены" >&2
    exit 1
}
(( item487_write_line < item487_retire_line )) || {
    echo "ITEM-487: запись журнала ретирует записи до записи байтов" >&2
    exit 1
}
grep -Fq 'APPLIANCE_BOOT_LOG_PATH' kernel/src/appliance_boot_log_store.c || {
    echo "ITEM-487: пачка пишется не в тот же путь журнала" >&2
    exit 1
}

# The ESP writer's probe, and the spans the record reads from it.
for item487_probe in \
    'struct efi_fat32_esp_io_stats' \
    'uint64_t transfers;' \
    'uint64_t lookups;' \
    'uint64_t fat_entries_read;' \
    'uint64_t sector_reads;' \
    'uint64_t sector_writes;' \
    'uint64_t lookup_us;' \
    'uint64_t traversal_us;' \
    'efi_fat32_esp_io_stats_reset' \
    'efi_fat32_esp_io_stats_read'; do
    grep -Fq "$item487_probe" kernel/include/efi_fat32_esp.h || {
        echo "ITEM-487: проба писателя не публикует: $item487_probe" >&2
        exit 1
    }
done
# The cycle counter is reached weakly: a host proof without a timer must link.
grep -Fq '__attribute__((weak))' kernel/src/efi_fat32_esp.c || {
    echo "ITEM-487: проба требует счётчик циклов жёстко" >&2
    exit 1
}

# The record: the fields of the reading and the bound that carries them.
for item487_record in \
    'APPLIANCE_BOOT_LOG_FLUSH_RECORD_BYTES 320U' \
    'appliance_boot_log_store_format_flush_record' \
    'appliance_boot_log_store_format_flush_tail_record' \
    'appliance_boot_log_store_last_flush'; do
    grep -Fq "$item487_record" kernel/include/appliance_boot_log_store.h || {
        echo "ITEM-487: чтение флаша не публикует: $item487_record" >&2
        exit 1
    }
done
for item487_field in \
    '"[BOOTLOG-FLUSH] records="' \
    '" esp_write_us="' \
    '" commit_us="' \
    '" total_us="' \
    '"[BOOTLOG-FLUSH-TAIL] publish_us="' \
    '" publish=derived lookup_us="' \
    '" fat_traversal_us="' \
    '" batch_count="' \
    '" unavailable=esp_blocks,clusters"'; do
    grep -Fq -- "$item487_field" kernel/src/appliance_boot_log_store.c || {
        echo "ITEM-487: запись флаша не называет: $item487_field" >&2
        exit 1
    }
done
grep -Fq 'stats.count != kernel_bootlog_flush_seen' kernel/src/kernel.c || {
    echo "ITEM-487: флаш публикуется не один раз на новый" >&2
    exit 1
}

# The behaviour the fix must not have changed: the record still names the
# moment it flushed for, and no diagnostic record became a flush marker.
grep -Fq 'appliance_boot_log_store_flush("readiness");' kernel/src/kernel.c || {
    echo "ITEM-487: readiness-флаш исчез" >&2
    exit 1
}
if grep -Fq '"[BOOTLOG-FLUSH]"' kernel/src/appliance_boot_log.c; then
    echo "ITEM-487: диагностическая запись стала flush-маркером" >&2
    exit 1
fi

# The evidence: the host proofs that read the accounting and the writer.
for item487_case in \
    'test_flush_readings_for_batch_sizes' \
    'test_failed_run_retires_nothing' \
    'test_flush_reading_is_bounded'; do
    grep -Fq "$item487_case" tests/appliance-boot-log-store-host.c || {
        echo "ITEM-487: host-проба не доказывает: $item487_case" >&2
        exit 1
    }
done
grep -Fq 'test_range_probe_counts_the_work' tests/efi-fat32-esp-host.c || {
    echo "ITEM-487: host-проба писателя не доказывает пробу" >&2
    exit 1
}

# ------------------------------------------------------------- the documents --
for item487_doc_word in ITEM-487 BOOTLOG-FLUSH batch_count lookup_us \
    fat_traversal_us 'appliance_boot_log_ring_peek'; do
    grep -Fq -- "$item487_doc_word" docs/hfoc-appliance-boot-console.md || {
        echo "docs/hfoc-appliance-boot-console.md не фиксирует ITEM-487:" \
            "$item487_doc_word" >&2
        exit 1
    }
done
