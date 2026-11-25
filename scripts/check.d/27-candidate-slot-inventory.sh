#!/usr/bin/env bash
# ITEM-387: contracted slice of the HFOC OS check suite. Sourced in pinned
#           order by scripts/check.sh; lint it on its own with shellcheck.

# Candidate slot inventory contracts.
grep -Fq 'test-candidate-slot-inventory-host' Makefile || {
    echo "Makefile не содержит test-candidate-slot-inventory-host" >&2
    exit 1
}
# shellcheck disable=SC2016
grep -Fq '$(MAKE) test-candidate-slot-inventory-host' Makefile || {
    echo "test: target не содержит test-candidate-slot-inventory-host" >&2
    exit 1
}
grep -Fq 'CANDIDATE_SLOT_INVENTORY_VERSION' \
    kernel/include/candidate_slot_inventory.h || {
    echo "candidate_slot_inventory.h не содержит CANDIDATE_SLOT_INVENTORY_VERSION" >&2
    exit 1
}
grep -Fq 'struct candidate_slot_inventory' \
    kernel/include/candidate_slot_inventory.h || {
    echo "candidate_slot_inventory.h не содержит struct candidate_slot_inventory" >&2
    exit 1
}
grep -Fq 'struct candidate_slot_inventory_entry' \
    kernel/include/candidate_slot_inventory.h || {
    echo "candidate_slot_inventory.h не содержит struct candidate_slot_inventory_entry" >&2
    exit 1
}
grep -Fq 'candidate_slot_inventory_publish' \
    kernel/include/candidate_slot_inventory.h || {
    echo "candidate_slot_inventory.h не содержит candidate_slot_inventory_publish" >&2
    exit 1
}
for candidate_slot_inventory_symbol in candidate_slot_inventory_init \
                                      candidate_slot_inventory_publish \
                                      candidate_slot_inventory_lookup \
                                      candidate_slot_inventory_generation \
                                      candidate_slot_inventory_slot_count \
                                      candidate_slot_inventory_active \
                                      candidate_slot_inventory_status_string; do
    nm build/hfoc-kernel.elf | grep -F " $candidate_slot_inventory_symbol" \
        >/dev/null || {
        echo "Production kernel не содержит $candidate_slot_inventory_symbol" >&2
        exit 1
    }
done
if grep -nE '\b(kmalloc|kfree|pmm_alloc_contiguous_pages|pmm_free_contiguous_pages)\b|\b(serial_printf|printf|fprintf|serial_write|console_write|panic)\b|\b(sleep|delay|hlt|sti|cli|pic_|lapic_|ioapic_|smp_)\b|\b(block_device_read|block_device_write)\b|\b(event_ring_|audit_)\b' \
        kernel/src/candidate_slot_inventory.c; then
    echo "candidate_slot_inventory.c выполняет запрещённое выделение, диагностику, аудит или блокирование" >&2
    exit 1
fi
if nm -g build/hfoc-kernel.elf |
        grep 'candidate_slot_inventory_host_test_'; then
    echo "candidate_slot_inventory_host_test_ символы попали в kernel ELF" >&2
    exit 1
fi
if bash scripts/hfoc-file-marker.sh build/hfoc-kernel.elf \
        'Candidate slot inventory host tests: ALL OK'; then
    echo "Production kernel содержит candidate slot inventory host marker" >&2
    exit 1
fi
for candidate_slot_inventory_doc in README.md docs/ARCHITECTURE.md docs/ROADMAP.md; do
    if ! grep -Eiq 'candidate slot inventory|candidate-slot-inventory' \
            "$candidate_slot_inventory_doc"; then
        echo "$candidate_slot_inventory_doc не документирует candidate slot inventory" >&2
        exit 1
    fi
done
grep -Fq -- '- [x] **152. Candidate-slot inventory' \
    docs/superpowers/plans/2026-08-17-appliance-v1-next-100-pr-queue-execution.md || {
    echo "appliance queue plan не помечает item 152 завершённым" >&2
    exit 1
}

# Candidate preflight contracts.
grep -Fq 'test-candidate-preflight-host' Makefile || {
    echo "Makefile не содержит test-candidate-preflight-host" >&2
    exit 1
}
# shellcheck disable=SC2016
grep -Fq '$(MAKE) test-candidate-preflight-host' Makefile || {
    echo "test: target не содержит test-candidate-preflight-host" >&2
    exit 1
}
grep -Fq 'CANDIDATE_PREFLIGHT_VERSION' \
    kernel/include/candidate_preflight.h || {
    echo "candidate_preflight.h не содержит CANDIDATE_PREFLIGHT_VERSION" >&2
    exit 1
}
grep -Fq 'struct candidate_preflight_request' \
    kernel/include/candidate_preflight.h || {
    echo "candidate_preflight.h не содержит struct candidate_preflight_request" >&2
    exit 1
}
grep -Fq 'struct candidate_preflight_gate' \
    kernel/include/candidate_preflight.h || {
    echo "candidate_preflight.h не содержит struct candidate_preflight_gate" >&2
    exit 1
}
grep -Fq 'candidate_preflight_run' \
    kernel/include/candidate_preflight.h || {
    echo "candidate_preflight.h не содержит candidate_preflight_run" >&2
    exit 1
}
for candidate_preflight_symbol in candidate_preflight_init \
                                  candidate_preflight_run \
                                  candidate_preflight_status_string; do
    nm build/hfoc-kernel.elf | grep -F " $candidate_preflight_symbol" \
        >/dev/null || {
        echo "Production kernel не содержит $candidate_preflight_symbol" >&2
        exit 1
    }
done
if grep -nE '\b(kmalloc|kfree|pmm_alloc_contiguous_pages|pmm_free_contiguous_pages)\b|\b(serial_printf|printf|fprintf|serial_write|console_write|panic)\b|\b(sleep|delay|hlt|sti|cli|pic_|lapic_|ioapic_|smp_)\b|\b(block_device_read|block_device_write)\b|\b(event_ring_|audit_)\b' \
        kernel/src/candidate_preflight.c; then
    echo "candidate_preflight.c выполняет запрещённое выделение, диагностику, аудит или блокирование" >&2
    exit 1
fi
if nm -g build/hfoc-kernel.elf |
        grep 'candidate_preflight_host_test_'; then
    echo "candidate_preflight_host_test_ символы попали в kernel ELF" >&2
    exit 1
fi
if bash scripts/hfoc-file-marker.sh build/hfoc-kernel.elf \
        'Candidate preflight host tests: ALL OK'; then
    echo "Production kernel содержит candidate preflight host marker" >&2
    exit 1
fi
for candidate_preflight_doc in README.md docs/ARCHITECTURE.md docs/ROADMAP.md; do
    if ! grep -Eiq 'candidate preflight|candidate-preflight' \
            "$candidate_preflight_doc"; then
        echo "$candidate_preflight_doc не документирует candidate preflight" >&2
        exit 1
    fi
done
grep -Fq -- '- [x] **153. Candidate preflight' \
    docs/superpowers/plans/2026-08-17-appliance-v1-next-100-pr-queue-execution.md || {
    echo "appliance queue plan не помечает item 153 завершённым" >&2
    exit 1
}

# Candidate staged write verification contracts.
grep -Fq 'test-candidate-staged-write-host' Makefile || {
    echo "Makefile не содержит test-candidate-staged-write-host" >&2
    exit 1
}
# shellcheck disable=SC2016
grep -Fq '$(MAKE) test-candidate-staged-write-host' Makefile || {
    echo "test: target не содержит test-candidate-staged-write-host" >&2
    exit 1
}
grep -Fq 'CANDIDATE_STAGED_WRITE_VERSION' \
    kernel/include/candidate_staged_write.h || {
    echo "candidate_staged_write.h не содержит CANDIDATE_STAGED_WRITE_VERSION" >&2
    exit 1
}
grep -Fq 'struct candidate_staged_write_gate' \
    kernel/include/candidate_staged_write.h || {
    echo "candidate_staged_write.h не содержит struct candidate_staged_write_gate" >&2
    exit 1
}
grep -Fq 'candidate_staged_write_run' \
    kernel/include/candidate_staged_write.h || {
    echo "candidate_staged_write.h не содержит candidate_staged_write_run" >&2
    exit 1
}
for candidate_staged_write_symbol in candidate_staged_write_init \
                                    candidate_staged_write_run \
                                    candidate_staged_write_status_string; do
    nm build/hfoc-kernel.elf | grep -F " $candidate_staged_write_symbol" \
        >/dev/null || {
        echo "Production kernel не содержит $candidate_staged_write_symbol" >&2
        exit 1
    }
done
if grep -nE '\b(kmalloc|kfree|pmm_alloc_contiguous_pages|pmm_free_contiguous_pages)\b|\b(serial_printf|printf|fprintf|serial_write|console_write|panic)\b|\b(sleep|delay|hlt|sti|cli|pic_|lapic_|ioapic_|smp_)\b|\b(block_device_read|block_device_write)\b|\b(event_ring_|audit_)\b' \
        kernel/src/candidate_staged_write.c; then
    echo "candidate_staged_write.c выполняет запрещённое выделение, диагностику, аудит или блокирование" >&2
    exit 1
fi
if nm -g build/hfoc-kernel.elf |
        grep 'candidate_staged_write_host_test_'; then
    echo "candidate_staged_write_host_test_ символы попали в kernel ELF" >&2
    exit 1
fi
if bash scripts/hfoc-file-marker.sh build/hfoc-kernel.elf \
        'Candidate staged write host tests: ALL OK'; then
    echo "Production kernel содержит candidate staged write host marker" >&2
    exit 1
fi
for candidate_staged_write_doc in README.md docs/ARCHITECTURE.md docs/ROADMAP.md; do
    if ! grep -Eiq 'candidate staged write|candidate-staged-write' \
            "$candidate_staged_write_doc"; then
        echo "$candidate_staged_write_doc не документирует candidate staged write" >&2
        exit 1
    fi
done
grep -Fq -- '- [x] **154. Staged write verification' \
    docs/superpowers/plans/2026-08-17-appliance-v1-next-100-pr-queue-execution.md || {
    echo "appliance queue plan не помечает item 154 завершённым" >&2
    exit 1
}

# Activation intent record contracts.
grep -Fq 'test-activation-intent-host' Makefile || {
    echo "Makefile не содержит test-activation-intent-host" >&2
    exit 1
}
# shellcheck disable=SC2016
grep -Fq '$(MAKE) test-activation-intent-host' Makefile || {
    echo "test: target не содержит test-activation-intent-host" >&2
    exit 1
}
grep -Fq 'ACTIVATION_INTENT_VERSION' \
    kernel/include/activation_intent.h || {
    echo "activation_intent.h не содержит ACTIVATION_INTENT_VERSION" >&2
    exit 1
}
grep -Fq 'struct activation_intent_store' \
    kernel/include/activation_intent.h || {
    echo "activation_intent.h не содержит struct activation_intent_store" >&2
    exit 1
}
grep -Fq 'activation_intent_record' \
    kernel/include/activation_intent.h || {
    echo "activation_intent.h не содержит activation_intent_record" >&2
    exit 1
}
for activation_intent_symbol in activation_intent_init \
                               activation_intent_record \
                               activation_intent_pending \
                               activation_intent_known_good \
                               activation_intent_resolve \
                               activation_intent_status_string; do
    nm build/hfoc-kernel.elf | grep -F " $activation_intent_symbol" \
        >/dev/null || {
        echo "Production kernel не содержит $activation_intent_symbol" >&2
        exit 1
    }
done
if grep -nE '\b(kmalloc|kfree|pmm_alloc_contiguous_pages|pmm_free_contiguous_pages)\b|\b(serial_printf|printf|fprintf|serial_write|console_write|panic)\b|\b(sleep|delay|hlt|sti|cli|pic_|lapic_|ioapic_|smp_)\b|\b(block_device_read|block_device_write)\b|\b(event_ring_|audit_)\b' \
        kernel/src/activation_intent.c; then
    echo "activation_intent.c выполняет запрещённое выделение, диагностику, аудит или блокирование" >&2
    exit 1
fi
if nm -g build/hfoc-kernel.elf |
        grep 'activation_intent_host_test_'; then
    echo "activation_intent_host_test_ символы попали в kernel ELF" >&2
    exit 1
fi
if bash scripts/hfoc-file-marker.sh build/hfoc-kernel.elf \
        'Activation intent host tests: ALL OK'; then
    echo "Production kernel содержит activation intent host marker" >&2
    exit 1
fi
for activation_intent_doc in README.md docs/ARCHITECTURE.md docs/ROADMAP.md; do
    if ! grep -Eiq 'activation intent|activation-intent' \
            "$activation_intent_doc"; then
        echo "$activation_intent_doc не документирует activation intent" >&2
        exit 1
    fi
done
grep -Fq -- '- [x] **155. Activation intent record' \
    docs/superpowers/plans/2026-08-17-appliance-v1-next-100-pr-queue-execution.md || {
    echo "appliance queue plan не помечает item 155 завершённым" >&2
    exit 1
}

# First-boot health contract contracts.
grep -Fq 'test-first-boot-health-host' Makefile || {
    echo "Makefile не содержит test-first-boot-health-host" >&2
    exit 1
}
# shellcheck disable=SC2016
grep -Fq '$(MAKE) test-first-boot-health-host' Makefile || {
    echo "test: target не содержит test-first-boot-health-host" >&2
    exit 1
}
grep -Fq 'FIRST_BOOT_HEALTH_VERSION' \
    kernel/include/first_boot_health.h || {
    echo "first_boot_health.h не содержит FIRST_BOOT_HEALTH_VERSION" >&2
    exit 1
}
grep -Fq 'struct first_boot_health_store' \
    kernel/include/first_boot_health.h || {
    echo "first_boot_health.h не содержит struct first_boot_health_store" >&2
    exit 1
}
grep -Fq 'first_boot_health_report' \
    kernel/include/first_boot_health.h || {
    echo "first_boot_health.h не содержит first_boot_health_report" >&2
    exit 1
}
for first_boot_health_symbol in first_boot_health_init \
                                first_boot_health_define \
                                first_boot_health_report \
                                first_boot_health_evaluate \
                                first_boot_health_status_string; do
    nm build/hfoc-kernel.elf | grep -F " $first_boot_health_symbol" \
        >/dev/null || {
        echo "Production kernel не содержит $first_boot_health_symbol" >&2
        exit 1
    }
done
if grep -nE '\b(kmalloc|kfree|pmm_alloc_contiguous_pages|pmm_free_contiguous_pages)\b|\b(serial_printf|printf|fprintf|serial_write|console_write|panic)\b|\b(sleep|delay|hlt|sti|cli|pic_|lapic_|ioapic_|smp_)\b|\b(block_device_read|block_device_write)\b|\b(event_ring_|audit_)\b' \
        kernel/src/first_boot_health.c; then
    echo "first_boot_health.c выполняет запрещённое выделение, диагностику, аудит или блокирование" >&2
    exit 1
fi
if nm -g build/hfoc-kernel.elf |
        grep 'first_boot_health_host_test_'; then
    echo "first_boot_health_host_test_ символы попали в kernel ELF" >&2
    exit 1
fi
if bash scripts/hfoc-file-marker.sh build/hfoc-kernel.elf \
        'First-boot health host tests: ALL OK'; then
    echo "Production kernel содержит first-boot health host marker" >&2
    exit 1
fi
for first_boot_health_doc in README.md docs/ARCHITECTURE.md docs/ROADMAP.md; do
    if ! grep -Eiq 'first-boot health|first boot health' \
            "$first_boot_health_doc"; then
        echo "$first_boot_health_doc не документирует first-boot health" >&2
        exit 1
    fi
done
grep -Fq -- '- [x] **156. First-boot health contract' \
    docs/superpowers/plans/2026-08-17-appliance-v1-next-100-pr-queue-execution.md || {
    echo "appliance queue plan не помечает item 156 завершённым" >&2
    exit 1
}

# Automatic rollback selection contracts.
grep -Fq 'test-automatic-rollback-host' Makefile || {
    echo "Makefile не содержит test-automatic-rollback-host" >&2
    exit 1
}
# shellcheck disable=SC2016
grep -Fq '$(MAKE) test-automatic-rollback-host' Makefile || {
    echo "test: target не содержит test-automatic-rollback-host" >&2
    exit 1
}
grep -Fq 'AUTOMATIC_ROLLBACK_VERSION' \
    kernel/include/automatic_rollback.h || {
    echo "automatic_rollback.h не содержит AUTOMATIC_ROLLBACK_VERSION" >&2
    exit 1
}
grep -Fq 'struct automatic_rollback_store' \
    kernel/include/automatic_rollback.h || {
    echo "automatic_rollback.h не содержит struct automatic_rollback_store" >&2
    exit 1
}
grep -Fq 'automatic_rollback_run' \
    kernel/include/automatic_rollback.h || {
    echo "automatic_rollback.h не содержит automatic_rollback_run" >&2
    exit 1
}
for automatic_rollback_symbol in automatic_rollback_init \
                                 automatic_rollback_run \
                                 automatic_rollback_evidence \
                                 automatic_rollback_selected \
                                 automatic_rollback_status_string \
                                 automatic_rollback_reason_string; do
    nm build/hfoc-kernel.elf | grep -F " $automatic_rollback_symbol" \
        >/dev/null || {
        echo "Production kernel не содержит $automatic_rollback_symbol" >&2
        exit 1
    }
done
if grep -nE '\b(kmalloc|kfree|pmm_alloc_contiguous_pages|pmm_free_contiguous_pages)\b|\b(serial_printf|printf|fprintf|serial_write|console_write|panic)\b|\b(sleep|delay|hlt|sti|cli|pic_|lapic_|ioapic_|smp_)\b|\b(block_device_read|block_device_write)\b|\b(event_ring_|audit_)\b' \
        kernel/src/automatic_rollback.c; then
    echo "automatic_rollback.c выполняет запрещённое выделение, диагностику, аудит или блокирование" >&2
    exit 1
fi
if nm -g build/hfoc-kernel.elf |
        grep 'automatic_rollback_host_test_'; then
    echo "automatic_rollback_host_test_ символы попали в kernel ELF" >&2
    exit 1
fi
if bash scripts/hfoc-file-marker.sh build/hfoc-kernel.elf \
        'Automatic rollback host tests: ALL OK'; then
    echo "Production kernel содержит automatic rollback host marker" >&2
    exit 1
fi
for automatic_rollback_doc in README.md docs/ARCHITECTURE.md docs/ROADMAP.md; do
    if ! grep -Eiq 'automatic rollback|automatic-rollback' \
            "$automatic_rollback_doc"; then
        echo "$automatic_rollback_doc не документирует automatic rollback selection" >&2
        exit 1
    fi
done
grep -Fq -- '- [x] **157. Automatic rollback selection' \
    docs/superpowers/plans/2026-08-17-appliance-v1-next-100-pr-queue-execution.md || {
    echo "appliance queue plan не помечает item 157 завершённым" >&2
    exit 1
}

# Update acceptance commit contracts.
grep -Fq 'test-update-acceptance-host' Makefile || {
    echo "Makefile не содержит test-update-acceptance-host" >&2
    exit 1
}
# shellcheck disable=SC2016
grep -Fq '$(MAKE) test-update-acceptance-host' Makefile || {
    echo "test: target не содержит test-update-acceptance-host" >&2
    exit 1
}
grep -Fq 'UPDATE_ACCEPTANCE_VERSION' \
    kernel/include/update_acceptance.h || {
    echo "update_acceptance.h не содержит UPDATE_ACCEPTANCE_VERSION" >&2
    exit 1
}
grep -Fq 'struct update_acceptance_store' \
    kernel/include/update_acceptance.h || {
    echo "update_acceptance.h не содержит struct update_acceptance_store" >&2
    exit 1
}
grep -Fq 'update_acceptance_commit' \
    kernel/include/update_acceptance.h || {
    echo "update_acceptance.h не содержит update_acceptance_commit" >&2
    exit 1
}
grep -Fq 'activation_intent_commit_known_good' \
    kernel/include/activation_intent.h || {
    echo "activation_intent.h не содержит activation_intent_commit_known_good" >&2
    exit 1
}
for update_acceptance_symbol in update_acceptance_init \
                                update_acceptance_commit \
                                update_acceptance_evidence \
                                update_acceptance_status_string \
                                activation_intent_commit_known_good; do
    nm build/hfoc-kernel.elf | grep -F " $update_acceptance_symbol" \
        >/dev/null || {
        echo "Production kernel не содержит $update_acceptance_symbol" >&2
        exit 1
    }
done
if grep -nE '\b(kmalloc|kfree|pmm_alloc_contiguous_pages|pmm_free_contiguous_pages)\b|\b(serial_printf|printf|fprintf|serial_write|console_write|panic)\b|\b(sleep|delay|hlt|sti|cli|pic_|lapic_|ioapic_|smp_)\b|\b(block_device_read|block_device_write)\b|\b(event_ring_|audit_)\b' \
        kernel/src/update_acceptance.c; then
    echo "update_acceptance.c выполняет запрещённое выделение, диагностику, аудит или блокирование" >&2
    exit 1
fi
if nm -g build/hfoc-kernel.elf |
        grep 'update_acceptance_host_test_'; then
    echo "update_acceptance_host_test_ символы попали в kernel ELF" >&2
    exit 1
fi
if bash scripts/hfoc-file-marker.sh build/hfoc-kernel.elf \
        'Update acceptance host tests: ALL OK'; then
    echo "Production kernel содержит update acceptance host marker" >&2
    exit 1
fi
for update_acceptance_doc in README.md docs/ARCHITECTURE.md docs/ROADMAP.md; do
    if ! grep -Eiq 'update acceptance|update-acceptance' \
            "$update_acceptance_doc"; then
        echo "$update_acceptance_doc не документирует update acceptance commit" >&2
        exit 1
    fi
done
grep -Fq -- '- [x] **158. Update acceptance commit' \
    docs/superpowers/plans/2026-08-17-appliance-v1-next-100-pr-queue-execution.md || {
    echo "appliance queue plan не помечает item 158 завершённым" >&2
    exit 1
}

# Update retention policy contracts.
grep -Fq 'test-update-retention-host' Makefile || {
    echo "Makefile не содержит test-update-retention-host" >&2
    exit 1
}
# shellcheck disable=SC2016
grep -Fq '$(MAKE) test-update-retention-host' Makefile || {
    echo "test: target не содержит test-update-retention-host" >&2
    exit 1
}
grep -Fq 'UPDATE_RETENTION_VERSION' \
    kernel/include/update_retention.h || {
    echo "update_retention.h не содержит UPDATE_RETENTION_VERSION" >&2
    exit 1
}
grep -Fq 'struct update_retention_store' \
    kernel/include/update_retention.h || {
    echo "update_retention.h не содержит struct update_retention_store" >&2
    exit 1
}
grep -Fq 'update_retention_run' \
    kernel/include/update_retention.h || {
    echo "update_retention.h не содержит update_retention_run" >&2
    exit 1
}
for update_retention_symbol in update_retention_init \
                                update_retention_run \
                                update_retention_evidence \
                                update_retention_status_string; do
    nm build/hfoc-kernel.elf | grep -F " $update_retention_symbol" \
        >/dev/null || {
        echo "Production kernel не содержит $update_retention_symbol" >&2
        exit 1
    }
done
if grep -nE '\b(kmalloc|kfree|pmm_alloc_contiguous_pages|pmm_free_contiguous_pages)\b|\b(serial_printf|printf|fprintf|serial_write|console_write|panic)\b|\b(sleep|delay|hlt|sti|cli|pic_|lapic_|ioapic_|smp_)\b|\b(block_device_read|block_device_write)\b|\b(event_ring_|audit_)\b' \
        kernel/src/update_retention.c; then
    echo "update_retention.c выполняет запрещённое выделение, диагностику, аудит или блокирование" >&2
    exit 1
fi
if nm -g build/hfoc-kernel.elf |
        grep 'update_retention_host_test_'; then
    echo "update_retention_host_test_ символы попали в kernel ELF" >&2
    exit 1
fi
if bash scripts/hfoc-file-marker.sh build/hfoc-kernel.elf \
        'Update retention host tests: ALL OK'; then
    echo "Production kernel содержит update retention host marker" >&2
    exit 1
fi
for update_retention_doc in README.md docs/ARCHITECTURE.md docs/ROADMAP.md; do
    if ! grep -Eiq 'update retention|retention policy' \
            "$update_retention_doc"; then
        echo "$update_retention_doc не документирует update retention policy" >&2
        exit 1
    fi
done
grep -Fq -- '- [x] **159. Update retention policy' \
    docs/superpowers/plans/2026-08-17-appliance-v1-next-100-pr-queue-execution.md || {
    echo "appliance queue plan не помечает item 159 завершённым" >&2
    exit 1
}

# Update lifecycle QEMU proof contracts.
grep -Fq 'test-update-lifecycle-qemu' Makefile || {
    echo "Makefile не содержит test-update-lifecycle-qemu" >&2
    exit 1
}
# shellcheck disable=SC2016
grep -Fq '$(MAKE) test-update-lifecycle-qemu' Makefile || {
    echo "test: target не содержит test-update-lifecycle-qemu" >&2
    exit 1
}
grep -Fq 'scripts/test-qemu-update-lifecycle.sh' Makefile || {
    echo "Makefile не содержит update-lifecycle QEMU script" >&2
    exit 1
}
grep -Fq 'HFOC_UPDATE_LIFECYCLE_QEMU_TEST' \
    kernel/src/update_lifecycle_qemu.c || {
    echo "update_lifecycle_qemu.c не имеет proof guard" >&2
    exit 1
}
grep -Fq '[UPDATE-LIFECYCLE] proof init' \
    kernel/src/update_lifecycle_qemu.c || {
    echo "update_lifecycle_qemu.c не содержит proof init marker" >&2
    exit 1
}
grep -Fq 'update_lifecycle_qemu_test_init' kernel/src/kernel.c || {
    echo "kernel.c не вызывает update-lifecycle proof" >&2
    exit 1
}
if bash scripts/hfoc-file-marker.sh build/hfoc-kernel.elf '[UPDATE-LIFECYCLE]'; then
    echo "Production kernel содержит update-lifecycle marker" >&2
    exit 1
fi
if grep -Fq 'qemu-update-lifecycle' .github/workflows/ci.yml; then
    :
else
    echo "CI pgrep guard не содержит update-lifecycle script" >&2
    exit 1
fi
grep -Fq 'QEMU update-lifecycle baseline: machine=q35 smp=1 cpu=default' \
    scripts/test-qemu-update-lifecycle.sh || {
    echo "test-qemu-update-lifecycle.sh не фиксирует portable baseline" >&2
    exit 1
}
grep -Fq 'QEMU update-lifecycle test: OK' \
    scripts/test-qemu-update-lifecycle.sh || {
    echo "test-qemu-update-lifecycle.sh не содержит success marker" >&2
    exit 1
}
for update_lifecycle_doc in README.md docs/ARCHITECTURE.md docs/ROADMAP.md; do
    if ! grep -Eiq 'update lifecycle QEMU proof|update-lifecycle QEMU proof' \
            "$update_lifecycle_doc"; then
        echo "$update_lifecycle_doc не документирует update-lifecycle QEMU proof" >&2
        exit 1
    fi
done
grep -Fq -- '- [x] **160. Update lifecycle QEMU proof' \
    docs/superpowers/plans/2026-08-17-appliance-v1-next-100-pr-queue-execution.md || {
    echo "appliance queue plan не помечает item 160 завершённым" >&2
    exit 1
}
