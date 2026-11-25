#!/usr/bin/env bash
# ITEM-387: contracted slice of the HFOC OS check suite. Sourced in pinned
#           order by scripts/check.sh; lint it on its own with shellcheck.

# Fixed-size block cache ownership contracts.
grep -Fq 'test-block-cache-host' Makefile || {
    echo "Makefile не содержит test-block-cache-host" >&2
    exit 1
}
# shellcheck disable=SC2016
grep -Fq '$(MAKE) test-block-cache-host' Makefile || {
    echo "test: target не содержит test-block-cache-host" >&2
    exit 1
}
grep -Fq 'test-block-cache' Makefile || {
    echo "Makefile не содержит test-block-cache" >&2
    exit 1
}
# shellcheck disable=SC2016
grep -Fq '$(MAKE) test-block-cache' Makefile || {
    echo "test: target не содержит test-block-cache" >&2
    exit 1
}
grep -Fq 'BLOCK_CACHE_CAPACITY_MIN' kernel/include/block_cache.h || {
    echo "block_cache.h не содержит BLOCK_CACHE_CAPACITY_MIN" >&2
    exit 1
}
grep -Fq 'BLOCK_CACHE_CAPACITY_MAX' kernel/include/block_cache.h || {
    echo "block_cache.h не содержит BLOCK_CACHE_CAPACITY_MAX" >&2
    exit 1
}
grep -Fq 'struct block_cache' kernel/include/block_cache.h || {
    echo "block_cache.h не содержит struct block_cache" >&2
    exit 1
}
for block_cache_symbol in block_cache_init block_cache_read block_cache_write \
                          block_cache_flush block_cache_flush_all \
                          block_cache_status_string; do
    nm build/hfoc-kernel.elf | grep -F " $block_cache_symbol" \
        >/dev/null || {
        echo "Production kernel не содержит $block_cache_symbol" >&2
        exit 1
    }
done
if grep -nE '\b(kmalloc|kfree|pmm_alloc_contiguous_pages|pmm_free_contiguous_pages)\b' \
        kernel/src/block_cache.c; then
    echo "block_cache.c выполняет запрещённое выделение" >&2
    exit 1
fi
if grep -nE 'block_cache_(read|write|flush|flush_all)\(' \
        kernel/src/arch/x86_64/interrupts.c; then
    echo "Block-cache I/O вызывается из interrupt path" >&2
    exit 1
fi
grep -Fq 'block_cache_qemu_test_init();' kernel/src/kernel.c || {
    echo "kernel.c не вызывает block_cache_qemu_test_init" >&2
    exit 1
}
for block_cache_marker in \
    '[BLOCK-CACHE] proof init' \
    '[BLOCK-CACHE] cache initialized' \
    '[BLOCK-CACHE] cache hit preserved' \
    '[BLOCK-CACHE] dirty block cached' \
    '[BLOCK-CACHE] dirty flush persisted' \
    '[BLOCK-CACHE] flush failure kept dirty' \
    '[BLOCK-CACHE] dirty eviction flushed' \
    '[BLOCK-CACHE] clean eviction skipped write' \
    '[BLOCK-CACHE] read failure left no duplicate' \
    '[BLOCK-CACHE] clean shutdown' \
    '[BLOCK-CACHE] proof OK'; do
    grep -Fq "$block_cache_marker" \
        kernel/src/block_cache_qemu.c || {
        echo "block_cache_qemu.c не содержит $block_cache_marker" >&2
        exit 1
    }
    grep -Fq "$block_cache_marker" \
        scripts/test-qemu-block-cache.sh || {
        echo "QEMU block-cache script не требует $block_cache_marker" >&2
        exit 1
    }
done
grep -Fq 'QEMU block-cache test: OK' \
    scripts/test-qemu-block-cache.sh || {
    echo "QEMU block-cache script не содержит success marker" >&2
    exit 1
}
if nm -g build/hfoc-kernel.elf | grep 'block_cache_qemu_test_'; then
    echo "Block-cache QEMU test symbols попали в kernel ELF" >&2
    exit 1
fi
if bash scripts/hfoc-file-marker.sh build/hfoc-kernel.elf \
        'Block-cache host tests: ALL OK'; then
    echo "Production kernel содержит block-cache host marker" >&2
    exit 1
fi
if bash scripts/hfoc-file-marker.sh build/hfoc-kernel.elf '[BLOCK-CACHE]'; then
    echo "Production kernel содержит block-cache proof marker" >&2
    exit 1
fi
grep -Fq '[t]est-qemu-block-cache' \
    .github/workflows/ci.yml || {
    echo "CI не проверяет утечку block-cache QEMU процесса" >&2
    exit 1
}
for block_cache_doc in README.md docs/ARCHITECTURE.md docs/ROADMAP.md; do
    if ! grep -Eiq 'block.?cache' \
            "$block_cache_doc"; then
        echo "$block_cache_doc не документирует block-cache boundary" >&2
        exit 1
    fi
done

# Checksummed configuration journal contracts.
grep -Fq 'test-config-journal-host' Makefile || {
    echo "Makefile не содержит test-config-journal-host" >&2
    exit 1
}
# shellcheck disable=SC2016
grep -Fq '$(MAKE) test-config-journal-host' Makefile || {
    echo "test: target не содержит test-config-journal-host" >&2
    exit 1
}
grep -Fq 'test-config-journal' Makefile || {
    echo "Makefile не содержит test-config-journal" >&2
    exit 1
}
# shellcheck disable=SC2016
grep -Fq '$(MAKE) test-config-journal' Makefile || {
    echo "test: target не содержит test-config-journal" >&2
    exit 1
}
grep -Fq 'CONFIG_JOURNAL_BLOCK_SIZE' kernel/include/config_journal.h || {
    echo "config_journal.h не содержит CONFIG_JOURNAL_BLOCK_SIZE" >&2
    exit 1
}
grep -Fq 'CONFIG_JOURNAL_DATA_MAX' kernel/include/config_journal.h || {
    echo "config_journal.h не содержит CONFIG_JOURNAL_DATA_MAX" >&2
    exit 1
}
grep -Fq 'CONFIG_JOURNAL_REGION_BLOCK_COUNT_MAX' \
    kernel/include/config_journal.h || {
    echo "config_journal.h не содержит CONFIG_JOURNAL_REGION_BLOCK_COUNT_MAX" >&2
    exit 1
}
grep -Fq 'CONFIG_JOURNAL_COMMIT_OFFSET' kernel/include/config_journal.h || {
    echo "config_journal.h не содержит CONFIG_JOURNAL_COMMIT_OFFSET" >&2
    exit 1
}
grep -Fq 'CONFIG_JOURNAL_COMMIT_MAGIC_BYTES' \
    kernel/include/config_journal.h || {
    echo "config_journal.h не содержит CONFIG_JOURNAL_COMMIT_MAGIC_BYTES" >&2
    exit 1
}
grep -Fq 'struct config_journal' kernel/include/config_journal.h || {
    echo "config_journal.h не содержит struct config_journal" >&2
    exit 1
}
for config_journal_symbol in config_journal_init config_journal_append \
                            config_journal_head \
                            config_journal_status_string; do
    nm build/hfoc-kernel.elf | grep -F " $config_journal_symbol" \
        >/dev/null || {
        echo "Production kernel не содержит $config_journal_symbol" >&2
        exit 1
    }
done
if grep -nE '\b(kmalloc|kfree|pmm_alloc_contiguous_pages|pmm_free_contiguous_pages)\b' \
        kernel/src/config_journal.c; then
    echo "config_journal.c выполняет запрещённое выделение" >&2
    exit 1
fi
if grep -nE 'config_journal_(append|head|init)\(' \
        kernel/src/arch/x86_64/interrupts.c; then
    echo "Config-journal I/O вызывается из interrupt path" >&2
    exit 1
fi
grep -Fq 'config_journal_qemu_test_init();' kernel/src/kernel.c || {
    echo "kernel.c не вызывает config_journal_qemu_test_init" >&2
    exit 1
}
for config_journal_marker in \
    '[CONFIG-JOURNAL] proof init' \
    '[CONFIG-JOURNAL] empty journal initialized' \
    '[CONFIG-JOURNAL] record 1 appended' \
    '[CONFIG-JOURNAL] record 2 appended' \
    '[CONFIG-JOURNAL] interrupted tail rejected' \
    '[CONFIG-JOURNAL] head recovered' \
    '[CONFIG-JOURNAL] interrupted tail overwritten' \
    '[CONFIG-JOURNAL] atomic torn write rejected' \
    '[CONFIG-JOURNAL] previous head readable after torn write' \
    '[CONFIG-JOURNAL] atomic failed flush rejected' \
    '[CONFIG-JOURNAL] previous head readable after failed flush' \
    '[CONFIG-JOURNAL] interrupted commit marker ignored' \
    '[CONFIG-JOURNAL] atomic retry committed' \
    '[CONFIG-JOURNAL] clean shutdown' \
    '[CONFIG-JOURNAL] proof OK'; do
    grep -Fq "$config_journal_marker" \
        kernel/src/config_journal_qemu.c || {
        echo "config_journal_qemu.c не содержит $config_journal_marker" >&2
        exit 1
    }
    grep -Fq "$config_journal_marker" \
        scripts/test-qemu-config-journal.sh || {
        echo "QEMU config-journal script не требует $config_journal_marker" >&2
        exit 1
    }
done
grep -Fq 'QEMU config-journal test: OK' \
    scripts/test-qemu-config-journal.sh || {
    echo "QEMU config-journal script не содержит success marker" >&2
    exit 1
}
if nm -g build/hfoc-kernel.elf | grep 'config_journal_qemu_test_'; then
    echo "Config-journal QEMU test symbols попали в kernel ELF" >&2
    exit 1
fi
if bash scripts/hfoc-file-marker.sh build/hfoc-kernel.elf \
        'Config-journal host tests: ALL OK'; then
    echo "Production kernel содержит config-journal host marker" >&2
    exit 1
fi
if bash scripts/hfoc-file-marker.sh build/hfoc-kernel.elf '[CONFIG-JOURNAL]'; then
    echo "Production kernel содержит config-journal proof marker" >&2
    exit 1
fi
grep -Fq '[t]est-qemu-config-journal' \
    .github/workflows/ci.yml || {
    echo "CI не проверяет утечку config-journal QEMU процесса" >&2
    exit 1
}
for config_journal_doc in README.md docs/ARCHITECTURE.md docs/ROADMAP.md; do
    if ! grep -Eiq 'config.?journal' \
            "$config_journal_doc"; then
        echo "$config_journal_doc не документирует config-journal boundary" >&2
        exit 1
    fi
done

# Bounded configuration/audit checkpoint KV contracts.
grep -Fq 'test-config-kv-host' Makefile || {
    echo "Makefile не содержит test-config-kv-host" >&2
    exit 1
}
# shellcheck disable=SC2016
grep -Fq '$(MAKE) test-config-kv-host' Makefile || {
    echo "test: target не содержит test-config-kv-host" >&2
    exit 1
}
grep -Fq 'CONFIG_KV_KEY_MAX' kernel/include/config_kv.h || {
    echo "config_kv.h не содержит CONFIG_KV_KEY_MAX" >&2
    exit 1
}
grep -Fq 'CONFIG_KV_VALUE_MAX' kernel/include/config_kv.h || {
    echo "config_kv.h не содержит CONFIG_KV_VALUE_MAX" >&2
    exit 1
}
grep -Fq 'CONFIG_KV_CAPACITY_MAX' kernel/include/config_kv.h || {
    echo "config_kv.h не содержит CONFIG_KV_CAPACITY_MAX" >&2
    exit 1
}
grep -Fq 'CONFIG_KV_KIND_CONFIG' kernel/include/config_kv.h || {
    echo "config_kv.h не содержит CONFIG_KV_KIND_CONFIG" >&2
    exit 1
}
grep -Fq 'CONFIG_KV_KIND_AUDIT' kernel/include/config_kv.h || {
    echo "config_kv.h не содержит CONFIG_KV_KIND_AUDIT" >&2
    exit 1
}
for config_kv_symbol in config_kv_init config_kv_put config_kv_get \
                        config_kv_remove config_kv_count \
                        config_kv_status_string; do
    nm build/hfoc-kernel.elf | grep -F " $config_kv_symbol" \
        >/dev/null || {
        echo "Production kernel не содержит $config_kv_symbol" >&2
        exit 1
    }
done
if grep -nE '\b(kmalloc|kfree|malloc|free)\b' \
        kernel/src/config_kv.c; then
    echo "config_kv.c выполняет запрещённое выделение" >&2
    exit 1
fi
grep -Fq 'Config-KV host tests: ALL OK' tests/config-kv-host.c || {
    echo "config-kv host harness не содержит success marker" >&2
    exit 1
}
if bash scripts/hfoc-file-marker.sh build/hfoc-kernel.elf \
        'Config-KV host tests: ALL OK'; then
    echo "Production kernel содержит config-kv host marker" >&2
    exit 1
fi
for config_kv_doc in README.md docs/ARCHITECTURE.md docs/ROADMAP.md; do
    if ! grep -Eiq 'config.?kv|checkpoint.*kv|key/value.*checkpoint' \
            "$config_kv_doc"; then
        echo "$config_kv_doc не документирует config-kv boundary" >&2
        exit 1
    fi
done

# Immutable state schema registry contracts.
grep -Fq 'test-state-schema-host' Makefile || {
    echo "Makefile не содержит test-state-schema-host" >&2
    exit 1
}
# shellcheck disable=SC2016
grep -Fq '$(MAKE) test-state-schema-host' Makefile || {
    echo "test: target не содержит test-state-schema-host" >&2
    exit 1
}
grep -Fq 'STATE_SCHEMA_ID_MAX' kernel/include/state_schema.h || {
    echo "state_schema.h не содержит STATE_SCHEMA_ID_MAX" >&2
    exit 1
}
grep -Fq 'STATE_SCHEMA_REGISTRY_CAPACITY_MAX' \
    kernel/include/state_schema.h || {
    echo "state_schema.h не содержит STATE_SCHEMA_REGISTRY_CAPACITY_MAX" >&2
    exit 1
}
grep -Fq 'STATE_SCHEMA_RECORD_LENGTH_MAX' \
    kernel/include/state_schema.h || {
    echo "state_schema.h не содержит STATE_SCHEMA_RECORD_LENGTH_MAX" >&2
    exit 1
}
grep -Fq 'STATE_SCHEMA_KIND_JOURNAL' kernel/include/state_schema.h || {
    echo "state_schema.h не содержит STATE_SCHEMA_KIND_JOURNAL" >&2
    exit 1
}
grep -Fq 'STATE_SCHEMA_KIND_KV' kernel/include/state_schema.h || {
    echo "state_schema.h не содержит STATE_SCHEMA_KIND_KV" >&2
    exit 1
}
for state_schema_symbol in state_schema_registry_init \
                           state_schema_validate_record \
                           state_schema_append_journal \
                           state_schema_kv_put \
                           state_schema_status_string; do
    nm build/hfoc-kernel.elf | grep -F " $state_schema_symbol" \
        >/dev/null || {
        echo "Production kernel не содержит $state_schema_symbol" >&2
        exit 1
    }
done
if grep -nE '\b(kmalloc|kfree|pmm_alloc_contiguous_pages|pmm_free_contiguous_pages)\b' \
        kernel/src/state_schema.c; then
    echo "state_schema.c выполняет запрещённое выделение" >&2
    exit 1
fi
if grep -nE 'state_schema_(append_journal|kv_put)\(' \
        kernel/src/arch/x86_64/interrupts.c; then
    echo "State-schema publication вызывается из interrupt path" >&2
    exit 1
fi
grep -Fq 'State schema host tests: ALL OK' tests/state-schema-host.c || {
    echo "state-schema host harness не содержит success marker" >&2
    exit 1
}
if bash scripts/hfoc-file-marker.sh build/hfoc-kernel.elf \
        'State schema host tests: ALL OK'; then
    echo "Production kernel содержит state-schema host marker" >&2
    exit 1
fi
for state_schema_doc in README.md docs/ARCHITECTURE.md docs/ROADMAP.md; do
    if ! grep -Eiq 'state.?schema' "$state_schema_doc"; then
        echo "$state_schema_doc не документирует state-schema boundary" >&2
        exit 1
    fi
done

# State migration transaction contracts.
grep -Fq 'test-state-migration-host' Makefile || {
    echo "Makefile не содержит test-state-migration-host" >&2
    exit 1
}
# shellcheck disable=SC2016
grep -Fq '$(MAKE) test-state-migration-host' Makefile || {
    echo "test: target не содержит test-state-migration-host" >&2
    exit 1
}
grep -Fq 'STATE_MIGRATION_MAX_STEPS' kernel/include/state_migration.h || {
    echo "state_migration.h не содержит STATE_MIGRATION_MAX_STEPS" >&2
    exit 1
}
grep -Fq '#include "state_schema.h"' kernel/include/state_migration.h || {
    echo "state_migration.h не включает state_schema.h" >&2
    exit 1
}
grep -Fq 'STATE_SCHEMA_KIND_KV' kernel/src/state_migration.c || {
    echo "state_migration.c не использует STATE_SCHEMA_KIND_KV" >&2
    exit 1
}
grep -Fq 'CONFIG_KV_KIND_COUNT' kernel/src/state_migration.c || {
    echo "state_migration.c не использует CONFIG_KV_KIND_COUNT" >&2
    exit 1
}
grep -Fq 'CONFIG_KV_KIND_CONFIG' tests/state-migration-host.c || {
    echo "state-migration harness не использует CONFIG_KV_KIND_CONFIG" >&2
    exit 1
}
grep -Fq 'CONFIG_KV_KIND_AUDIT' tests/state-migration-host.c || {
    echo "state-migration harness не использует CONFIG_KV_KIND_AUDIT" >&2
    exit 1
}
for state_migration_symbol in state_migration_plan_init \
                              state_migration_commit \
                              state_migration_apply \
                              state_migration_status_string; do
    nm build/hfoc-kernel.elf | grep -F " $state_migration_symbol" \
        >/dev/null || {
        echo "Production kernel не содержит $state_migration_symbol" >&2
        exit 1
    }
done
if grep -nE '\b(kmalloc|kfree|pmm_alloc_contiguous_pages|pmm_free_contiguous_pages)\b' \
        kernel/src/state_migration.c; then
    echo "state_migration.c выполняет запрещённое выделение" >&2
    exit 1
fi
if grep -nE 'state_migration_(commit|apply)\(' \
        kernel/src/arch/x86_64/interrupts.c; then
    echo "State-migration publication вызывается из interrupt path" >&2
    exit 1
fi
grep -Fq 'State migration host tests: ALL OK' \
    tests/state-migration-host.c || {
    echo "state-migration host harness не содержит success marker" >&2
    exit 1
}
if bash scripts/hfoc-file-marker.sh build/hfoc-kernel.elf \
        'State migration host tests: ALL OK'; then
    echo "Production kernel содержит state-migration host marker" >&2
    exit 1
fi
for state_migration_doc in README.md docs/ARCHITECTURE.md docs/ROADMAP.md; do
    if ! grep -Eiq 'state.?migration' "$state_migration_doc"; then
        echo "$state_migration_doc не документирует state-migration boundary" >&2
        exit 1
    fi
done

# Journal compaction candidate contracts.
grep -Fq 'test-journal-compaction-host' Makefile || {
    echo "Makefile не содержит test-journal-compaction-host" >&2
    exit 1
}
# shellcheck disable=SC2016
grep -Fq '$(MAKE) test-journal-compaction-host' Makefile || {
    echo "test: target не содержит test-journal-compaction-host" >&2
    exit 1
}
grep -Fq 'JOURNAL_COMPACTION_CHECKPOINT_VERSION' \
    kernel/include/journal_compaction.h || {
    echo "journal_compaction.h не содержит checkpoint version" >&2
    exit 1
}
grep -Fq 'JOURNAL_COMPACTION_MAX_CHECKPOINTS' \
    kernel/include/journal_compaction.h || {
    echo "journal_compaction.h не содержит max checkpoint count" >&2
    exit 1
}
grep -Fq 'JOURNAL_COMPACTION_MAX_ENTRIES' \
    kernel/include/journal_compaction.h || {
    echo "journal_compaction.h не содержит max candidate entries" >&2
    exit 1
}
grep -Fq 'JOURNAL_COMPACTION_GENERATION_SLOT_COUNT' \
    kernel/include/journal_compaction.h || {
    echo "journal_compaction.h не содержит generation slot count" >&2
    exit 1
}
grep -Fq 'JOURNAL_COMPACTION_INITIAL_GENERATION' \
    kernel/include/journal_compaction.h || {
    echo "journal_compaction.h не содержит initial generation" >&2
    exit 1
}
for journal_compaction_symbol in journal_compaction_select \
                                journal_compaction_status_string \
                                journal_compaction_commit_init \
                                journal_compaction_publish \
                                journal_compaction_recover \
                                journal_compaction_active; do
    nm build/hfoc-kernel.elf | grep -F " $journal_compaction_symbol" \
        >/dev/null || {
        echo "Production kernel не содержит $journal_compaction_symbol" >&2
        exit 1
    }
done
if grep -nE '\b(kmalloc|kfree|malloc|free)\b' \
        kernel/src/journal_compaction.c; then
    echo "journal_compaction.c выполняет запрещённое выделение" >&2
    exit 1
fi
if grep -nE 'config_kv_(put|remove)\(' kernel/src/journal_compaction.c; then
    echo "journal_compaction.c изменяет active checkpoint" >&2
    exit 1
fi
if grep -nE 'config_journal_|block_device_' \
        kernel/src/journal_compaction.c; then
    echo "journal_compaction.c выполняет прямое storage I/O" >&2
    exit 1
fi
if grep -nE 'journal_compaction_select\(' \
        kernel/src/arch/x86_64/interrupts.c; then
    echo "Journal-compaction selection вызывается из interrupt path" >&2
    exit 1
fi
if grep -nE 'journal_compaction_(commit_init|publish|recover)\(' \
        kernel/src/arch/x86_64/interrupts.c; then
    echo "Journal-compaction publication вызывается из interrupt path" >&2
    exit 1
fi
grep -Fq 'Journal compaction host tests: ALL OK' \
    tests/journal-compaction-host.c || {
    echo "journal-compaction host harness не содержит success marker" >&2
    exit 1
}
if bash scripts/hfoc-file-marker.sh build/hfoc-kernel.elf \
        'Journal compaction host tests: ALL OK'; then
    echo "Production kernel содержит journal-compaction host marker" >&2
    exit 1
fi
for journal_compaction_doc in README.md docs/ARCHITECTURE.md docs/ROADMAP.md; do
    if ! grep -Eiq 'journal.*compaction|compaction.*journal' \
            "$journal_compaction_doc"; then
        echo "$journal_compaction_doc не документирует journal-compaction boundary" >&2
        exit 1
    fi
done
