#!/usr/bin/env bash
# ITEM-387: contracted slice of the HFOC OS check suite. Sourced in pinned
#           order by scripts/check.sh; lint it on its own with shellcheck.

# Deterministic backup-stream contracts.
grep -Fq 'test-backup-stream-host' Makefile || {
    echo "Makefile не содержит test-backup-stream-host" >&2
    exit 1
}
# shellcheck disable=SC2016
grep -Fq '$(MAKE) test-backup-stream-host' Makefile || {
    echo "test: target не содержит test-backup-stream-host" >&2
    exit 1
}
grep -Fq 'BACKUP_STREAM_FORMAT_VERSION' kernel/include/backup_stream.h || {
    echo "backup_stream.h не содержит BACKUP_STREAM_FORMAT_VERSION" >&2
    exit 1
}
grep -Fq 'BACKUP_STREAM_HEADER_SIZE' kernel/include/backup_stream.h || {
    echo "backup_stream.h не содержит BACKUP_STREAM_HEADER_SIZE" >&2
    exit 1
}
grep -Fq 'BACKUP_STREAM_DIGEST_SIZE' kernel/include/backup_stream.h || {
    echo "backup_stream.h не содержит BACKUP_STREAM_DIGEST_SIZE" >&2
    exit 1
}
grep -Fq 'BACKUP_STREAM_MAX_ENTRIES' kernel/include/backup_stream.h || {
    echo "backup_stream.h не содержит BACKUP_STREAM_MAX_ENTRIES" >&2
    exit 1
}
grep -Fq 'BACKUP_STREAM_MAX_FRAMED_LENGTH' kernel/include/backup_stream.h || {
    echo "backup_stream.h не содержит BACKUP_STREAM_MAX_FRAMED_LENGTH" >&2
    exit 1
}
grep -Fq 'backup_stream_frame_validate' kernel/include/backup_stream.h || {
    echo "backup_stream.h не объявляет backup_stream_frame_validate" >&2
    exit 1
}
grep -Fq 'backup_stream_frame_validate' kernel/src/restore_validator.c || {
    echo "restore_validator.c не вызывает backup_stream_frame_validate" >&2
    exit 1
}
if ! awk '
    /backup_stream_frame_validate\(stream/ { frame_seen = 1 }
    /restore_zero_plan\(plan\);/ {
        if (!frame_seen) { exit 1 }
    }
    END { if (!frame_seen) exit 1 }
' kernel/src/restore_validator.c; then
    echo "restore_validate должен проверять framing до записи плана" >&2
    exit 1
fi
for backup_stream_symbol in backup_stream_export backup_stream_import \
                            backup_stream_frame_validate \
                            backup_stream_status_string; do
    nm build/hfoc-kernel.elf | grep -F " $backup_stream_symbol" \
        >/dev/null || {
        echo "Production kernel не содержит $backup_stream_symbol" >&2
        exit 1
    }
done
if grep -nE '\b(kmalloc|kfree|malloc|free)\b' \
        kernel/src/backup_stream.c; then
    echo "backup_stream.c выполняет запрещённое выделение" >&2
    exit 1
fi
grep -Fq 'Backup-stream host tests: ALL OK' tests/backup-stream-host.c || {
    echo "backup-stream host harness не содержит success marker" >&2
    exit 1
}
if bash scripts/hfoc-file-marker.sh build/hfoc-kernel.elf \
        'Backup-stream host tests: ALL OK'; then
    echo "Production kernel содержит backup-stream host marker" >&2
    exit 1
fi
for backup_stream_doc in README.md docs/ARCHITECTURE.md docs/ROADMAP.md; do
    if ! grep -Eiq 'backup.?stream|deterministic.*backup|backup.*format' \
            "$backup_stream_doc"; then
        echo "$backup_stream_doc не документирует backup-stream boundary" >&2
        exit 1
    fi
    if ! grep -Eiq 'framing v1|frame.?validate|framing gate' \
            "$backup_stream_doc"; then
        echo "$backup_stream_doc не документирует backup-stream framing v1" >&2
        exit 1
    fi
done

# Backup manifest inventory contracts.
grep -Fq 'test-backup-manifest-host' Makefile || {
    echo "Makefile не содержит test-backup-manifest-host" >&2
    exit 1
}
# shellcheck disable=SC2016
grep -Fq '$(MAKE) test-backup-manifest-host' Makefile || {
    echo "test: target не содержит test-backup-manifest-host" >&2
    exit 1
}
grep -Fq 'BACKUP_MANIFEST_FORMAT_VERSION' \
    kernel/include/backup_manifest.h || {
    echo "backup_manifest.h не содержит BACKUP_MANIFEST_FORMAT_VERSION" >&2
    exit 1
}
grep -Fq 'BACKUP_MANIFEST_SIZE' kernel/include/backup_manifest.h || {
    echo "backup_manifest.h не содержит BACKUP_MANIFEST_SIZE" >&2
    exit 1
}
grep -Fq 'struct backup_manifest_source' \
    kernel/include/backup_manifest.h || {
    echo "backup_manifest.h не содержит struct backup_manifest_source" >&2
    exit 1
}
grep -Fq 'backup_manifest_export' kernel/include/backup_manifest.h || {
    echo "backup_manifest.h не объявляет backup_manifest_export" >&2
    exit 1
}
for backup_manifest_symbol in backup_manifest_export \
                              backup_manifest_status_string; do
    nm build/hfoc-kernel.elf | grep -F " $backup_manifest_symbol" \
        >/dev/null || {
        echo "Production kernel не содержит $backup_manifest_symbol" >&2
        exit 1
    }
done
if grep -nE '\b(kmalloc|kfree|malloc|free)\b' \
        kernel/src/backup_manifest.c; then
    echo "backup_manifest.c выполняет запрещённое выделение" >&2
    exit 1
fi
if grep -nE 'config_journal_|block_device_' \
        kernel/src/backup_manifest.c; then
    echo "backup_manifest.c выполняет прямое storage I/O" >&2
    exit 1
fi
if grep -nE 'backup_manifest_export\(' \
        kernel/src/arch/x86_64/interrupts.c; then
    echo "Backup-manifest export вызывается из interrupt path" >&2
    exit 1
fi
grep -Fq 'Backup manifest host tests: ALL OK' \
    tests/backup-manifest-host.c || {
    echo "backup-manifest host harness не содержит success marker" >&2
    exit 1
}
if bash scripts/hfoc-file-marker.sh build/hfoc-kernel.elf \
        'Backup manifest host tests: ALL OK'; then
    echo "Production kernel содержит backup-manifest host marker" >&2
    exit 1
fi
for backup_manifest_doc in README.md docs/ARCHITECTURE.md docs/ROADMAP.md; do
    if ! grep -Eiq 'backup manifest|manifest inventory' \
            "$backup_manifest_doc"; then
        echo "$backup_manifest_doc не документирует backup manifest" >&2
        exit 1
    fi
done
grep -Fq -- '- [x] **106. Backup manifest inventory' \
    docs/superpowers/plans/2026-08-17-appliance-v1-next-100-pr-queue-execution.md || {
    echo "appliance queue plan не помечает item 106 завершённым" >&2
    exit 1
}

# Bounded backup-producer contracts.
grep -Fq 'test-backup-producer-host' Makefile || {
    echo "Makefile не содержит test-backup-producer-host" >&2
    exit 1
}
# shellcheck disable=SC2016
grep -Fq '$(MAKE) test-backup-producer-host' Makefile || {
    echo "test: target не содержит test-backup-producer-host" >&2
    exit 1
}
grep -Fq 'BACKUP_PRODUCER_MAX_ENTRIES' \
    kernel/include/backup_producer.h || {
    echo "backup_producer.h не содержит BACKUP_PRODUCER_MAX_ENTRIES" >&2
    exit 1
}
grep -Fq 'BACKUP_PRODUCER_MIN_CHUNK_SIZE' \
    kernel/include/backup_producer.h || {
    echo "backup_producer.h не содержит BACKUP_PRODUCER_MIN_CHUNK_SIZE" >&2
    exit 1
}
grep -Fq 'struct backup_producer' kernel/include/backup_producer.h || {
    echo "backup_producer.h не содержит struct backup_producer" >&2
    exit 1
}
grep -Fq 'backup_producer_init' kernel/include/backup_producer.h || {
    echo "backup_producer.h не объявляет backup_producer_init" >&2
    exit 1
}
grep -Fq 'backup_producer_next' kernel/include/backup_producer.h || {
    echo "backup_producer.h не объявляет backup_producer_next" >&2
    exit 1
}
grep -Fq 'backup_producer_close' kernel/include/backup_producer.h || {
    echo "backup_producer.h не объявляет backup_producer_close" >&2
    exit 1
}
for backup_producer_symbol in backup_producer_init backup_producer_next \
                              backup_producer_close \
                              backup_producer_status_string; do
    nm build/hfoc-kernel.elf | grep -F " $backup_producer_symbol" \
        >/dev/null || {
        echo "Production kernel не содержит $backup_producer_symbol" >&2
        exit 1
    }
done
if grep -nE '\b(kmalloc|kfree|malloc|free)\b' \
        kernel/src/backup_producer.c; then
    echo "backup_producer.c выполняет запрещённое выделение" >&2
    exit 1
fi
if grep -nE 'config_journal_|block_device_' \
        kernel/src/backup_producer.c; then
    echo "backup_producer.c выполняет прямое storage I/O" >&2
    exit 1
fi
if grep -nE 'backup_producer_init\(' \
        kernel/src/arch/x86_64/interrupts.c; then
    echo "Backup-producer init вызывается из interrupt path" >&2
    exit 1
fi
grep -Fq 'Backup producer host tests: ALL OK' \
    tests/backup-producer-host.c || {
    echo "backup-producer host harness не содержит success marker" >&2
    exit 1
}
if bash scripts/hfoc-file-marker.sh build/hfoc-kernel.elf \
        'Backup producer host tests: ALL OK'; then
    echo "Production kernel содержит backup-producer host marker" >&2
    exit 1
fi
for backup_producer_doc in README.md docs/ARCHITECTURE.md docs/ROADMAP.md; do
    if ! grep -Eiq 'bounded backup producer|backup producer' \
            "$backup_producer_doc"; then
        echo "$backup_producer_doc не документирует bounded backup producer" >&2
        exit 1
    fi
done
grep -Fq -- '- [x] **107. Bounded backup producer' \
    docs/superpowers/plans/2026-08-17-appliance-v1-next-100-pr-queue-execution.md || {
    echo "appliance queue plan не помечает item 107 завершённым" >&2
    exit 1
}
grep -Fq -- '- [x] **108. Restore dry-run planner' \
    docs/superpowers/plans/2026-08-17-appliance-v1-next-100-pr-queue-execution.md || {
    echo "appliance queue plan не помечает item 108 завершённым" >&2
    exit 1
}

# Restore-validation and no-change rollback contracts.
grep -Fq 'test-restore-validator-host' Makefile || {
    echo "Makefile не содержит test-restore-validator-host" >&2
    exit 1
}
# shellcheck disable=SC2016
grep -Fq '$(MAKE) test-restore-validator-host' Makefile || {
    echo "test: target не содержит test-restore-validator-host" >&2
    exit 1
}
grep -Fq 'RESTORE_PLAN_MAX_ENTRIES' kernel/include/restore_validator.h || {
    echo "restore_validator.h не содержит RESTORE_PLAN_MAX_ENTRIES" >&2
    exit 1
}
grep -Fq 'RESTORE_DRY_RUN' kernel/include/restore_validator.h || {
    echo "restore_validator.h не содержит RESTORE_DRY_RUN" >&2
    exit 1
}
grep -Fq 'bool dry_run;' kernel/include/restore_validator.h || {
    echo "restore_validator.h не содержит dry_run plan flag" >&2
    exit 1
}
grep -Fq 'restore_dry_run_plan' kernel/include/restore_validator.h || {
    echo "restore_validator.h не объявляет restore_dry_run_plan" >&2
    exit 1
}
for restore_symbol in restore_validate restore_commit restore_apply \
                      restore_dry_run_plan restore_status_string; do
    nm build/hfoc-kernel.elf | grep -F " $restore_symbol" \
        >/dev/null || {
        echo "Production kernel не содержит $restore_symbol" >&2
        exit 1
    }
done
if grep -nE '\b(kmalloc|kfree|malloc|free)\b' \
        kernel/src/restore_validator.c; then
    echo "restore_validator.c выполняет запрещённое выделение" >&2
    exit 1
fi
if grep -nE 'block_device_|config_journal_|service_supervisor_|config_kv_put|config_kv_remove' \
        kernel/src/restore_validator.c; then
    echo "restore_validator.c выполняет запрещённое storage/service mutation" >&2
    exit 1
fi
grep -Fq 'Restore-validator host tests: ALL OK' \
    tests/restore-validator-host.c || {
    echo "restore-validator host harness не содержит success marker" >&2
    exit 1
}
if bash scripts/hfoc-file-marker.sh build/hfoc-kernel.elf \
        'Restore-validator host tests: ALL OK'; then
    echo "Production kernel содержит restore-validator host marker" >&2
    exit 1
fi
grep -Fq 'test-restore-planner-host' Makefile || {
    echo "Makefile не содержит test-restore-planner-host" >&2
    exit 1
}
# shellcheck disable=SC2016
grep -Fq '$(MAKE) test-restore-planner-host' Makefile || {
    echo "test: target не содержит test-restore-planner-host" >&2
    exit 1
}
grep -Fq 'restore_dry_run_plan' kernel/src/restore_validator.c || {
    echo "restore_validator.c не реализует restore_dry_run_plan" >&2
    exit 1
}
grep -Fq 'plan->dry_run = true;' kernel/src/restore_validator.c || {
    echo "restore_validator.c не публикует dry-run plan flag" >&2
    exit 1
}
grep -Fq 'Restore-planner host tests: ALL OK' \
    tests/restore-planner-host.c || {
    echo "restore-planner host harness не содержит success marker" >&2
    exit 1
}
if bash scripts/hfoc-file-marker.sh build/hfoc-kernel.elf \
        'Restore-planner host tests: ALL OK'; then
    echo "Production kernel содержит restore-planner host marker" >&2
    exit 1
fi
for restore_doc in README.md docs/ARCHITECTURE.md docs/ROADMAP.md; do
    if ! grep -Eiq 'restore.*valid|restore.*rollback|no-change.*restore' \
            "$restore_doc"; then
        echo "$restore_doc не документирует restore-validation boundary" >&2
        exit 1
    fi
    if ! grep -Eiq 'restore dry-run|dry-run planner' "$restore_doc"; then
        echo "$restore_doc не документирует restore dry-run planner" >&2
        exit 1
    fi
done

# Staged restore activation contracts.
grep -Fq 'test-restore-activation-host' Makefile || {
    echo "Makefile не содержит test-restore-activation-host" >&2
    exit 1
}
# shellcheck disable=SC2016
grep -Fq '$(MAKE) test-restore-activation-host' Makefile || {
    echo "test: target не содержит test-restore-activation-host" >&2
    exit 1
}
grep -Fq 'RESTORE_STAGE_SLOT_COUNT' kernel/include/restore_validator.h || {
    echo "restore_validator.h не содержит RESTORE_STAGE_SLOT_COUNT" >&2
    exit 1
}
grep -Fq 'RESTORE_BAD_STAGE' kernel/include/restore_validator.h || {
    echo "restore_validator.h не содержит RESTORE_BAD_STAGE" >&2
    exit 1
}
grep -Fq 'struct restore_activation' kernel/include/restore_validator.h || {
    echo "restore_validator.h не содержит struct restore_activation" >&2
    exit 1
}
for restore_activation_api in restore_activation_init restore_stage_apply \
                              restore_activation_active; do
    grep -Fq "$restore_activation_api" kernel/include/restore_validator.h || {
        echo "restore_validator.h не объявляет $restore_activation_api" >&2
        exit 1
    }
    nm build/hfoc-kernel.elf | grep -F " $restore_activation_api" \
        >/dev/null || {
        echo "Production kernel не содержит $restore_activation_api" >&2
        exit 1
    }
done
if grep -nE '\b(kmalloc|kfree|malloc|free)\b' \
        kernel/src/restore_validator.c; then
    echo "restore_validator.c выполняет запрещённое выделение" >&2
    exit 1
fi
if grep -nE 'block_device_|config_journal_|service_supervisor_|config_kv_put|config_kv_remove' \
        kernel/src/restore_validator.c; then
    echo "restore_validator.c выполняет запрещённое storage/service mutation" >&2
    exit 1
fi
if grep -nE 'restore_stage_apply\(' \
        kernel/src/arch/x86_64/interrupts.c; then
    echo "Staged restore activation вызывается из interrupt path" >&2
    exit 1
fi
grep -Fq 'Restore-activation host tests: ALL OK' \
    tests/restore-activation-host.c || {
    echo "restore-activation host harness не содержит success marker" >&2
    exit 1
}
if bash scripts/hfoc-file-marker.sh build/hfoc-kernel.elf \
        'Restore-activation host tests: ALL OK'; then
    echo "Production kernel содержит restore-activation host marker" >&2
    exit 1
fi
for restore_activation_doc in README.md docs/ARCHITECTURE.md \
                              docs/ROADMAP.md; do
    if ! grep -Eiq 'staged restore activation' \
            "$restore_activation_doc"; then
        echo "$restore_activation_doc не документирует staged restore" >&2
        exit 1
    fi
done
grep -Fq -- '- [x] **109. Staged restore activation' \
    docs/superpowers/plans/2026-08-17-appliance-v1-next-100-pr-queue-execution.md || {
    echo "appliance queue plan не помечает item 109 завершённым" >&2
    exit 1
}

# Backup/restore QEMU acceptance.
grep -Fq 'test-backup-restore-qemu' Makefile || {
    echo "Makefile не содержит test-backup-restore-qemu" >&2
    exit 1
}
# shellcheck disable=SC2016
grep -Fq '$(MAKE) test-backup-restore-qemu' Makefile || {
    echo "test: target не содержит test-backup-restore-qemu" >&2
    exit 1
}
grep -Fq 'backup_restore_qemu_test_init' kernel/src/kernel.c || {
    echo "kernel.c не подключает backup-restore QEMU proof" >&2
    exit 1
}
grep -Fq 'HFOC_BACKUP_RESTORE_QEMU_TEST' Makefile || {
    echo "Makefile не содержит HFOC_BACKUP_RESTORE_QEMU_TEST" >&2
    exit 1
}
for backup_restore_marker in \
    '[BACKUP-RESTORE] proof init' \
    '[BACKUP-RESTORE] active state initialized' \
    '[BACKUP-RESTORE] stream exported' \
    '[BACKUP-RESTORE] dry-run plan validated' \
    '[BACKUP-RESTORE] active state changed' \
    '[BACKUP-RESTORE] values restored' \
    '[BACKUP-RESTORE] audit continuity preserved' \
    '[BACKUP-RESTORE] clean reboot' \
    '[BACKUP-RESTORE] proof OK'; do
    grep -Fq "$backup_restore_marker" \
        scripts/test-qemu-backup-restore.sh || {
        echo "QEMU backup-restore script не требует $backup_restore_marker" >&2
        exit 1
    }
done
grep -Fq 'system_reset' \
    scripts/test-qemu-backup-restore.sh || {
    echo "QEMU backup-restore script не выполняет HMP system_reset" >&2
    exit 1
}
grep -Fq 'Backup-restore proof 2: OK' \
    scripts/test-qemu-backup-restore.sh || {
    echo "QEMU backup-restore script не проверяет второй proof после reset" >&2
    exit 1
}
grep -Fq 'QEMU did not exit after monitor quit' \
    scripts/test-qemu-backup-restore.sh || {
    echo "QEMU backup-restore script не требует строгого shutdown" >&2
    exit 1
}
if grep -Fq -- '-no-reboot' \
        scripts/test-qemu-backup-restore.sh; then
    echo "QEMU backup-restore script не должен использовать -no-reboot" >&2
    exit 1
fi
grep -Fq 'QEMU backup-restore test: OK' \
    scripts/test-qemu-backup-restore.sh || {
    echo "QEMU backup-restore script не содержит success marker" >&2
    exit 1
}
if bash scripts/hfoc-file-marker.sh build/hfoc-kernel.elf \
        '[BACKUP-RESTORE]'; then
    echo "Production kernel содержит backup-restore proof marker" >&2
    exit 1
fi
grep -Fq '[t]est-qemu-backup-restore' \
    .github/workflows/ci.yml || {
    echo "CI не проверяет утечку backup-restore QEMU процесса" >&2
    exit 1
}
for backup_restore_doc in README.md docs/ARCHITECTURE.md \
                          docs/ROADMAP.md; do
    if ! grep -Eiq 'backup/restore QEMU acceptance|backup-restore QEMU acceptance' \
            "$backup_restore_doc"; then
        echo "$backup_restore_doc не документирует backup/restore QEMU acceptance" >&2
        exit 1
    fi
done
grep -Fq -- '- [x] **110. Backup/restore QEMU acceptance' \
    docs/superpowers/plans/2026-08-17-appliance-v1-next-100-pr-queue-execution.md || {
    echo "appliance queue plan не помечает item 110 завершённым" >&2
    exit 1
}

# PMM bitmap allocator contracts.
grep -Fq 'HFOC_PMM_MAX_PAGES' kernel/include/pmm.h || {
    echo "pmm.h не содержит HFOC_PMM_MAX_PAGES" >&2; exit 1; }
# ITEM-390: the shipped page bitmap cap must stay at 1 TiB and the kernel build
# must never override it. A host proof may lower the cap in its own compile
# rule; the production value is pinned here.
grep -Fq '#define HFOC_PMM_MAX_PAGES (1U << 28U)' kernel/include/pmm.h || {
    echo "pmm.h не фиксирует производный предел страниц (1 TiB)" >&2; exit 1; }
kernel_cflags_block=$(sed -n '/^CFLAGS :=/,/^[^[:space:]]/p' Makefile)
if grep -Fq 'HFOC_PMM_MAX_PAGES' <<<"$kernel_cflags_block"; then
    echo "CFLAGS ядра не должны переопределять HFOC_PMM_MAX_PAGES" >&2
    exit 1
fi
for pmm_proof_target in test-pmm-limited-host test-pmm-highmem-host test-pmm-pci-hole-host; do
    grep -Fq "${pmm_proof_target}:" Makefile || {
        echo "Makefile не содержит ${pmm_proof_target}" >&2
        exit 1
    }
    # shellcheck disable=SC2016
    grep -Fq "\$(MAKE) ${pmm_proof_target}" Makefile || {
        echo "test-host-suite-serial не вызывает ${pmm_proof_target}" >&2
        exit 1
    }
done
for pmm_api in \
    'bool pmm_init(struct limine_memmap_response *memory_map, uint64_t hhdm_offset);' \
    'uint64_t pmm_total_usable_bytes(void);' \
    'uint64_t pmm_free_page_count(void);' \
    'uint64_t pmm_alloc_page(void);' \
    'bool pmm_free_page(uint64_t physical_address);' \
    'uint64_t pmm_alloc_contiguous_pages(uint32_t count);' \
    'bool pmm_free_contiguous_pages(uint64_t physical_address, uint32_t count);'; do
    grep -Fq "$pmm_api" kernel/include/pmm.h || {
        echo "pmm.h не содержит API: $pmm_api" >&2
        exit 1
    }
done
for pmm_symbol in pmm_init pmm_total_usable_bytes pmm_free_page_count \
                  pmm_alloc_page pmm_free_page pmm_alloc_contiguous_pages \
                  pmm_free_contiguous_pages; do
    nm build/hfoc-kernel.elf | grep -F " $pmm_symbol" >/dev/null || {
        echo "В ELF отсутствует символ: $pmm_symbol" >&2
        exit 1
    }
done
if nm -g build/hfoc-kernel.elf | grep -E 'pmm_test_|mock_'; then
    echo "PMM host test symbols попали в kernel ELF" >&2
    exit 1
fi
grep -Fq 'pmm_alloc_contiguous_pages(HFOC_AP_STACK_PAGES)' \
    kernel/src/smp.c || {
    echo "smp.c не использует pmm_alloc_contiguous_pages для AP stack" >&2
    exit 1
}
grep -Fq 'console_command_is(input_line, "pmm")' kernel/src/console.c || {
    echo "console.c не содержит команду pmm" >&2
    exit 1
}
grep -Fq '[PMM ] usable=' kernel/src/console.c || {
    echo "console.c не содержит PMM диагностику usable" >&2
    exit 1
}
grep -Fq 'free_pages=' kernel/src/console.c || {
    echo "console.c не содержит PMM диагностику free_pages" >&2
    exit 1
}
# ITEM-393: a page fault on a machine without a serial console is diagnosed
# from the panic screen alone, so the record that turns CR2 into the physical
# address behind it and classifies it against the tracked regions must stay.
grep -Fq 'pmm_fault_line(' kernel/src/panic.c || {
    echo "panic.c не публикует физический адрес ошибки страницы" >&2
    exit 1
}
grep -Fq 'HFOC_PMM_FAULT_LINE_MAX' kernel/include/pmm.h || {
    echo "pmm.h не ограничивает запись об ошибке страницы" >&2
    exit 1
}
grep -Fq 'test-pmm-host' Makefile || {
    echo "Makefile не содержит test-pmm-host" >&2; exit 1; }
# shellcheck disable=SC2016
grep -Fq '$(MAKE) test-pmm-host' Makefile || {
    echo "test: target не содержит test-pmm-host" >&2; exit 1; }
grep -Fq 'test-pmm' Makefile || {
    echo "Makefile не содержит test-pmm" >&2; exit 1; }
# shellcheck disable=SC2016
grep -Fq '$(MAKE) test-pmm' Makefile || {
    echo "test: target не содержит test-pmm" >&2; exit 1; }
if [[ "$(grep -c '^test-pmm:' Makefile || true)" -ne 1 ]]; then
    echo "Ожидался ровно один test-pmm target" >&2
    exit 1
fi
pmm_target_block=$(sed -n '/^test-pmm:/,/^[^[:space:]]/p' Makefile)
grep -Fq "QEMU_MACHINE='q35'" <<<"$pmm_target_block" || {
    echo "test-pmm должен использовать QEMU_MACHINE='q35'" >&2
    exit 1
}
grep -Fq "QEMU_SMP='1'" <<<"$pmm_target_block" || {
    echo "test-pmm должен использовать QEMU_SMP='1'" >&2
    exit 1
}
if grep -Eq 'QEMU_CPU=.*(max|-apic|-no-apic)|-cpu[[:space:]]+(max|-apic|-no-apic)' \
    <<<"$pmm_target_block"; then
    echo "test-pmm должен не задавать QEMU_CPU" >&2
    exit 1
fi
bash -n scripts/test-qemu-pmm.sh || {
    echo "Синтаксис scripts/test-qemu-pmm.sh некорректен" >&2
    exit 1
}
grep -Fq 'QEMU APIC baseline: machine=q35 smp=1 cpu=default' \
    scripts/test-qemu-pmm.sh || {
    echo "test-qemu-pmm.sh не фиксирует portable baseline" >&2
    exit 1
}
grep -Fq 'QEMU PMM test: OK' scripts/test-qemu-pmm.sh || {
    echo "test-qemu-pmm.sh не содержит success marker" >&2
    exit 1
}
for pmm_strict_symbol in mktemp timeout send_monitor_quit stop_qemu cleanup \
                         no-reboot no-shutdown; do
    grep -Fq "$pmm_strict_symbol" scripts/test-qemu-pmm.sh || {
        echo "test-qemu-pmm.sh не содержит $pmm_strict_symbol" >&2
        exit 1
    }
done
