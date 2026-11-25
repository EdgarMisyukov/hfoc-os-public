#!/usr/bin/env bash
# ITEM-387: contracted slice of the HFOC OS check suite. Sourced in pinned
#           order by scripts/check.sh; lint it on its own with shellcheck.

# Virtio-rng queue composition contracts.
grep -Fq 'test-virtio-rng-queue-composition-host' Makefile || {
    echo "Makefile не содержит test-virtio-rng-queue-composition-host" >&2
    exit 1
}
# shellcheck disable=SC2016
grep -Fq '$(MAKE) test-virtio-rng-queue-composition-host' Makefile || {
    echo "test-host-suite не вызывает test-virtio-rng-queue-composition-host" >&2
    exit 1
}
grep -Fq 'VIRTIO_RNG_QUEUE_COMPOSITION_VERSION' \
    kernel/include/virtio_rng_queue_composition.h || {
    echo "virtio_rng_queue_composition.h не содержит version" >&2
    exit 1
}
grep -Fq 'VIRTIO_RNG_QUEUE_COMPOSITION_QUEUE_SIZE_MAX' \
    kernel/include/virtio_rng_queue_composition.h || {
    echo "virtio_rng_queue_composition.h не содержит queue size max" >&2
    exit 1
}
grep -Fq 'struct virtio_rng_queue_config' \
    kernel/include/virtio_rng_queue_composition.h || {
    echo "virtio_rng_queue_composition.h не содержит queue config" >&2
    exit 1
}
grep -Fq 'struct virtio_rng_queue_composition' \
    kernel/include/virtio_rng_queue_composition.h || {
    echo "virtio_rng_queue_composition.h не содержит composition" >&2
    exit 1
}
for virtio_rng_queue_symbol in virtio_rng_queue_composition_activate \
                               virtio_rng_queue_composition_teardown \
                               virtio_rng_queue_composition_published \
                               virtio_rng_queue_composition_generation \
                               virtio_rng_queue_composition_queue \
                               virtio_rng_queue_composition_status_string; do
    nm build/hfoc-kernel.elf | grep -F " $virtio_rng_queue_symbol" \
        >/dev/null || {
        echo "Production kernel не содержит $virtio_rng_queue_symbol" >&2
        exit 1
    }
done
if grep -nE '\b(kmalloc|kfree|pmm_alloc_contiguous_pages|pmm_free_contiguous_pages)\b|\b(serial_printf|printf|fprintf|audit_|audit_record|panic)\b|\b(sleep|delay|hlt|sti|cli|pic_|lapic_|ioapic_|smp_)\b' \
        kernel/src/virtio_rng_queue_composition.c; then
    echo "virtio_rng_queue_composition.c выполняет запрещённое выделение, диагностику или блокировку" >&2
    exit 1
fi
if grep -nE 'virtio_rng_queue_composition_(activate|teardown)\(' \
        kernel/src/arch/x86_64/interrupts.c; then
    echo "Virtio-rng queue composition вызывается из interrupt path" >&2
    exit 1
fi
if nm -g build/hfoc-kernel.elf | grep 'virtio_rng_queue_composition_host_test_'; then
    echo "virtio_rng_queue_composition_host_test_ символы попали в kernel ELF" >&2
    exit 1
fi
if bash scripts/hfoc-file-marker.sh build/hfoc-kernel.elf \
        'Virtio rng queue composition host tests: ALL OK'; then
    echo "Production kernel содержит virtio rng queue composition host marker" >&2
    exit 1
fi
for virtio_rng_queue_doc in README.md docs/ARCHITECTURE.md docs/ROADMAP.md; do
    if ! grep -Eiq 'virtio[- ].*rng[- ].*queue[- ].*composition' \
            "$virtio_rng_queue_doc"; then
        echo "$virtio_rng_queue_doc не документирует virtio-rng queue composition" >&2
        exit 1
    fi
done
grep -Fq -- '- [x] **252. Virtio-rng queue composition' \
    docs/superpowers/plans/2026-08-22-https-vertical-slice-items-201-300.md || {
    echo "HTTPS queue plan не помечает item 252 завершённым" >&2
    exit 1
}

# Virtio-rng bounded random request API contracts (item 253).
grep -Fq 'test-virtio-rng-request-host' Makefile || {
    echo "Makefile не содержит test-virtio-rng-request-host" >&2
    exit 1
}
# shellcheck disable=SC2016
grep -Fq '$(MAKE) test-virtio-rng-request-host' Makefile || {
    echo "test-host-suite не вызывает test-virtio-rng-request-host" >&2
    exit 1
}
grep -Fq 'VIRTIO_RNG_REQUEST_VERSION' \
    kernel/include/virtio_rng_request.h || {
    echo "virtio_rng_request.h не содержит version" >&2
    exit 1
}
grep -Fq 'VIRTIO_RNG_REQUEST_LENGTH_MAX' \
    kernel/include/virtio_rng_request.h || {
    echo "virtio_rng_request.h не содержит maximum request length" >&2
    exit 1
}
grep -Fq 'VIRTIO_RNG_CALLER_CONTEXT_INTERRUPT' \
    kernel/include/virtio_rng_request.h || {
    echo "virtio_rng_request.h не содержит interrupt caller context" >&2
    exit 1
}
grep -Fq 'VIRTIO_RNG_REQUEST_RESULT_PENDING' \
    kernel/include/virtio_rng_request.h || {
    echo "virtio_rng_request.h не содержит pending completion result" >&2
    exit 1
}
grep -Fq 'struct virtio_rng_request' \
    kernel/include/virtio_rng_request.h || {
    echo "virtio_rng_request.h не содержит request descriptor" >&2
    exit 1
}
grep -Fq 'struct virtio_rng_request_api' \
    kernel/include/virtio_rng_request.h || {
    echo "virtio_rng_request.h не содержит request API state" >&2
    exit 1
}
for virtio_rng_request_symbol in virtio_rng_request_api_init \
                                 virtio_rng_request_api_submit \
                                 virtio_rng_request_api_complete \
                                 virtio_rng_request_api_initialized \
                                 virtio_rng_request_api_pending \
                                 virtio_rng_request_api_generation \
                                 virtio_rng_request_api_current \
                                 virtio_rng_request_api_status_string; do
    nm build/hfoc-kernel.elf | grep -F " $virtio_rng_request_symbol" \
        >/dev/null || {
        echo "Production kernel не содержит $virtio_rng_request_symbol" >&2
        exit 1
    }
done
if grep -nE '\b(kmalloc|kfree|pmm_alloc_contiguous_pages|pmm_free_contiguous_pages)\b|\b(serial_printf|printf|fprintf|audit_|audit_record|panic)\b|\b(sleep|delay|hlt|sti|cli|pic_|lapic_|ioapic_|smp_)\b' \
        kernel/src/virtio_rng_request.c; then
    echo "virtio_rng_request.c выполняет запрещённое выделение, диагностику или блокировку" >&2
    exit 1
fi
if grep -nE 'virtio_rng_request_api_(init|submit|complete)\(' \
        kernel/src/arch/x86_64/interrupts.c; then
    echo "Virtio-rng request API вызывается из interrupt path" >&2
    exit 1
fi
if nm -g build/hfoc-kernel.elf | grep 'virtio_rng_request_host_test_'; then
    echo "virtio_rng_request_host_test_ символы попали в kernel ELF" >&2
    exit 1
fi
if bash scripts/hfoc-file-marker.sh build/hfoc-kernel.elf \
        'Virtio rng request host tests: ALL OK'; then
    echo "Production kernel содержит virtio rng request host marker" >&2
    exit 1
fi
for virtio_rng_request_doc in README.md docs/ARCHITECTURE.md docs/ROADMAP.md; do
    if ! grep -Eiq 'virtio[- ].*rng[- ].*request[- ].*api|random request api' \
            "$virtio_rng_request_doc"; then
        echo "$virtio_rng_request_doc не документирует virtio-rng request API" >&2
        exit 1
    fi
done
grep -Fq -- '- [x] **253. Bounded random request API' \
    docs/superpowers/plans/2026-08-22-https-vertical-slice-items-201-300.md || {
    echo "HTTPS queue plan не помечает item 253 завершённым" >&2
    exit 1
}

# Virtio-rng completion path contracts (item 254).
grep -Fq 'test-virtio-rng-completion-host' Makefile || {
    echo "Makefile не содержит test-virtio-rng-completion-host" >&2
    exit 1
}
# shellcheck disable=SC2016
grep -Fq '$(MAKE) test-virtio-rng-completion-host' Makefile || {
    echo "test-host-suite не вызывает test-virtio-rng-completion-host" >&2
    exit 1
}
grep -Fq 'VIRTIO_RNG_COMPLETION_VERSION' \
    kernel/include/virtio_rng_completion.h || {
    echo "virtio_rng_completion.h не содержит version" >&2
    exit 1
}
grep -Fq 'struct virtio_rng_completion_result' \
    kernel/include/virtio_rng_completion.h || {
    echo "virtio_rng_completion.h не содержит completion result" >&2
    exit 1
}
grep -Fq 'VIRTIO_RNG_COMPLETION_DUPLICATE_COMPLETION' \
    kernel/include/virtio_rng_completion.h || {
    echo "virtio_rng_completion.h не содержит duplicate status" >&2
    exit 1
}
grep -Fq 'VIRTIO_RNG_COMPLETION_SHORT_COMPLETION' \
    kernel/include/virtio_rng_completion.h || {
    echo "virtio_rng_completion.h не содержит short status" >&2
    exit 1
}
grep -Fq 'VIRTIO_RNG_COMPLETION_OVERSIZED_COMPLETION' \
    kernel/include/virtio_rng_completion.h || {
    echo "virtio_rng_completion.h не содержит oversized status" >&2
    exit 1
}
grep -Fq 'VIRTIO_RNG_COMPLETION_STALE_COMPLETION' \
    kernel/include/virtio_rng_completion.h || {
    echo "virtio_rng_completion.h не содержит stale status" >&2
    exit 1
}
for virtio_rng_completion_symbol in virtio_rng_completion_complete \
                                    virtio_rng_completion_status_string; do
    nm build/hfoc-kernel.elf | grep -F " $virtio_rng_completion_symbol" \
        >/dev/null || {
        echo "Production kernel не содержит $virtio_rng_completion_symbol" >&2
        exit 1
    }
done
if grep -nE '\b(kmalloc|kfree|pmm_alloc_contiguous_pages|pmm_free_contiguous_pages)\b|\b(serial_printf|printf|fprintf|audit_|audit_record|panic)\b|\b(sleep|delay|hlt|sti|cli|pic_|lapic_|ioapic_|smp_)\b' \
        kernel/src/virtio_rng_completion.c; then
    echo "virtio_rng_completion.c выполняет запрещённое выделение, диагностику или блокировку" >&2
    exit 1
fi
if grep -nE 'virtio_rng_completion_complete\(' \
        kernel/src/arch/x86_64/interrupts.c; then
    echo "Virtio-rng completion path вызывается из interrupt path" >&2
    exit 1
fi
if nm -g build/hfoc-kernel.elf | grep 'virtio_rng_completion_host_test_'; then
    echo "virtio_rng_completion_host_test_ символы попали в kernel ELF" >&2
    exit 1
fi
if bash scripts/hfoc-file-marker.sh build/hfoc-kernel.elf \
        'Virtio rng completion host tests: ALL OK'; then
    echo "Production kernel содержит virtio rng completion host marker" >&2
    exit 1
fi
for virtio_rng_completion_doc in README.md docs/ARCHITECTURE.md docs/ROADMAP.md; do
    if ! grep -Eiq 'virtio[- ].*rng[- ].*completion[- ].*path|completion path' \
            "$virtio_rng_completion_doc"; then
        echo "$virtio_rng_completion_doc не документирует virtio-rng completion path" >&2
        exit 1
    fi
done
grep -Fq -- '- [x] **254. Virtio-rng completion path' \
    docs/superpowers/plans/2026-08-22-https-vertical-slice-items-201-300.md || {
    echo "HTTPS queue plan не помечает item 254 завершённым" >&2
    exit 1
}

# Entropy continuous health contracts (item 255).
grep -Fq 'test-entropy-health-host' Makefile || {
    echo "Makefile не содержит test-entropy-health-host" >&2
    exit 1
}
# shellcheck disable=SC2016
grep -Fq '$(MAKE) test-entropy-health-host' Makefile || {
    echo "test-host-suite не вызывает test-entropy-health-host" >&2
    exit 1
}
grep -Fq 'ENTROPY_HEALTH_VERSION' \
    kernel/include/entropy_health.h || {
    echo "entropy_health.h не содержит version" >&2
    exit 1
}
grep -Fq 'ENTROPY_HEALTH_GENERATION_MIN' \
    kernel/include/entropy_health.h || {
    echo "entropy_health.h не содержит generation min" >&2
    exit 1
}
grep -Fq 'ENTROPY_HEALTH_BLOCK_LENGTH_MAX' \
    kernel/include/entropy_health.h || {
    echo "entropy_health.h не содержит block length max" >&2
    exit 1
}
grep -Fq 'ENTROPY_HEALTH_STALL_LIMIT_MAX' \
    kernel/include/entropy_health.h || {
    echo "entropy_health.h не содержит stall limit max" >&2
    exit 1
}
grep -Fq 'ENTROPY_HEALTH_REPEATED_BLOCK' \
    kernel/include/entropy_health.h || {
    echo "entropy_health.h не содержит repeated-block status" >&2
    exit 1
}
grep -Fq 'ENTROPY_HEALTH_IMPOSSIBLE_LENGTH' \
    kernel/include/entropy_health.h || {
    echo "entropy_health.h не содержит impossible-length status" >&2
    exit 1
}
grep -Fq 'ENTROPY_HEALTH_STALLED' \
    kernel/include/entropy_health.h || {
    echo "entropy_health.h не содержит stalled status" >&2
    exit 1
}
grep -Fq 'ENTROPY_HEALTH_DEVICE_RESET' \
    kernel/include/entropy_health.h || {
    echo "entropy_health.h не содержит device-reset status" >&2
    exit 1
}
grep -Fq 'struct entropy_health_config' \
    kernel/include/entropy_health.h || {
    echo "entropy_health.h не содержит config" >&2
    exit 1
}
grep -Fq 'struct entropy_health' \
    kernel/include/entropy_health.h || {
    echo "entropy_health.h не содержит health state" >&2
    exit 1
}
grep -Fq 'enum entropy_health_state' \
    kernel/include/entropy_health.h || {
    echo "entropy_health.h не содержит state enum" >&2
    exit 1
}
grep -Fq 'enum entropy_health_fault' \
    kernel/include/entropy_health.h || {
    echo "entropy_health.h не содержит fault enum" >&2
    exit 1
}
for entropy_health_symbol in entropy_health_init \
                             entropy_health_observe \
                             entropy_health_progress \
                             entropy_health_state \
                             entropy_health_fault \
                             entropy_health_generation \
                             entropy_health_ready \
                             entropy_health_status_string \
                             entropy_health_state_string \
                             entropy_health_fault_string; do
    nm build/hfoc-kernel.elf | grep -F " $entropy_health_symbol" \
        >/dev/null || {
        echo "Production kernel не содержит $entropy_health_symbol" >&2
        exit 1
    }
done
if grep -nE '\b(kmalloc|kfree|pmm_alloc_contiguous_pages|pmm_free_contiguous_pages)\b|\b(serial_printf|printf|fprintf|audit_|audit_record|panic)\b|\b(sleep|delay|hlt|sti|cli|pic_|lapic_|ioapic_|smp_)\b' \
        kernel/src/entropy_health.c; then
    echo "entropy_health.c выполняет запрещённое выделение, диагностику или блокировку" >&2
    exit 1
fi
if grep -nE 'entropy_health_(init|observe|progress)\(' \
        kernel/src/arch/x86_64/interrupts.c; then
    echo "Entropy health вызывается из interrupt path" >&2
    exit 1
fi
if nm -g build/hfoc-kernel.elf | grep 'entropy_health_host_test_'; then
    echo "entropy_health_host_test_ символы попали в kernel ELF" >&2
    exit 1
fi
if bash scripts/hfoc-file-marker.sh build/hfoc-kernel.elf \
        'Entropy health host tests: ALL OK'; then
    echo "Production kernel содержит entropy health host marker" >&2
    exit 1
fi
for entropy_health_doc in README.md docs/ARCHITECTURE.md docs/ROADMAP.md; do
    if ! grep -Eiq 'entropy[- ].*health|continuous[- ].*entropy[- ].*health' \
            "$entropy_health_doc"; then
        echo "$entropy_health_doc не документирует entropy continuous health" >&2
        exit 1
    fi
done
grep -Fq -- '- [x] **255. Entropy continuous health checks' \
    docs/superpowers/plans/2026-08-22-https-vertical-slice-items-201-300.md || {
    echo "HTTPS queue plan не помечает item 255 завершённым" >&2
    exit 1
}

# Entropy accumulator lifecycle contracts (item 256).
grep -Fq 'test-entropy-accumulator-host' Makefile || {
    echo "Makefile не содержит test-entropy-accumulator-host" >&2
    exit 1
}
# shellcheck disable=SC2016
grep -Fq '$(MAKE) test-entropy-accumulator-host' Makefile || {
    echo "test-host-suite не вызывает test-entropy-accumulator-host" >&2
    exit 1
}
grep -Fq 'ENTROPY_ACCUMULATOR_VERSION' \
    kernel/include/entropy_accumulator.h || {
    echo "entropy_accumulator.h не содержит version" >&2
    exit 1
}
grep -Fq 'ENTROPY_ACCUMULATOR_CAPACITY' \
    kernel/include/entropy_accumulator.h || {
    echo "entropy_accumulator.h не содержит capacity" >&2
    exit 1
}
grep -Fq 'ENTROPY_ACCUMULATOR_GENERATION_MIN' \
    kernel/include/entropy_accumulator.h || {
    echo "entropy_accumulator.h не содержит generation min" >&2
    exit 1
}
grep -Fq 'ENTROPY_ACCUMULATOR_SOURCE_ID_MIN' \
    kernel/include/entropy_accumulator.h || {
    echo "entropy_accumulator.h не содержит source identity min" >&2
    exit 1
}
grep -Fq 'ENTROPY_ACCUMULATOR_BLOCK_LENGTH_MAX' \
    kernel/include/entropy_accumulator.h || {
    echo "entropy_accumulator.h не содержит block length max" >&2
    exit 1
}
grep -Fq 'ENTROPY_ACCUMULATOR_STALL_LIMIT_MAX' \
    kernel/include/entropy_accumulator.h || {
    echo "entropy_accumulator.h не содержит stall limit max" >&2
    exit 1
}
grep -Fq 'struct entropy_accumulator_source_state' \
    kernel/include/entropy_accumulator.h || {
    echo "entropy_accumulator.h не содержит source state" >&2
    exit 1
}
grep -Fq 'struct entropy_accumulator' \
    kernel/include/entropy_accumulator.h || {
    echo "entropy_accumulator.h не содержит accumulator state" >&2
    exit 1
}
grep -Fq 'enum entropy_accumulator_state' \
    kernel/include/entropy_accumulator.h || {
    echo "entropy_accumulator.h не содержит state enum" >&2
    exit 1
}
grep -Fq 'enum entropy_accumulator_fault' \
    kernel/include/entropy_accumulator.h || {
    echo "entropy_accumulator.h не содержит fault enum" >&2
    exit 1
}
grep -Fq 'ENTROPY_ACCUMULATOR_CREDIT_OVERFLOW' \
    kernel/include/entropy_accumulator.h || {
    echo "entropy_accumulator.h не содержит credit-overflow status" >&2
    exit 1
}
if grep -nE 'entropy_accumulator_(read|extract|seed|fill|snapshot)\(' \
        kernel/include/entropy_accumulator.h; then
    echo "entropy_accumulator.h предоставляет публичное чтение seed/bytes" >&2
    exit 1
fi
for entropy_accumulator_symbol in entropy_accumulator_init \
                                  entropy_accumulator_mix \
                                  entropy_accumulator_progress \
                                  entropy_accumulator_state \
                                  entropy_accumulator_fault \
                                  entropy_accumulator_initialized \
                                  entropy_accumulator_accumulating \
                                  entropy_accumulator_generation \
                                  entropy_accumulator_source_id \
                                  entropy_accumulator_credited_bytes \
                                  entropy_accumulator_credited_blocks \
                                  entropy_accumulator_last_accept_tick \
                                  entropy_accumulator_status_string \
                                  entropy_accumulator_state_string \
                                  entropy_accumulator_fault_string; do
    nm build/hfoc-kernel.elf | grep -F " $entropy_accumulator_symbol" \
        >/dev/null || {
        echo "Production kernel не содержит $entropy_accumulator_symbol" >&2
        exit 1
    }
done
if grep -nE '\b(kmalloc|kfree|pmm_alloc_contiguous_pages|pmm_free_contiguous_pages)\b|\b(serial_printf|printf|fprintf|audit_|audit_record|panic)\b|\b(sleep|delay|hlt|sti|cli|pic_|lapic_|ioapic_|smp_)\b' \
        kernel/src/entropy_accumulator.c; then
    echo "entropy_accumulator.c выполняет запрещённое выделение, диагностику или блокировку" >&2
    exit 1
fi
if grep -nE 'entropy_accumulator_(init|mix|progress)\(' \
        kernel/src/arch/x86_64/interrupts.c; then
    echo "Entropy accumulator вызывается из interrupt path" >&2
    exit 1
fi
if nm -g build/hfoc-kernel.elf | grep 'entropy_accumulator_host_test_'; then
    echo "entropy_accumulator_host_test_ символы попали в kernel ELF" >&2
    exit 1
fi
if bash scripts/hfoc-file-marker.sh build/hfoc-kernel.elf \
        'Entropy accumulator host tests: ALL OK'; then
    echo "Production kernel содержит entropy accumulator host marker" >&2
    exit 1
fi
for entropy_accumulator_doc in README.md docs/ARCHITECTURE.md docs/ROADMAP.md; do
    if ! grep -Eiq 'entropy[- ].*accumulator|accumulator[- ].*entropy' \
            "$entropy_accumulator_doc"; then
        echo "$entropy_accumulator_doc не документирует entropy accumulator" >&2
        exit 1
    fi
done
grep -Fq -- '- [x] **256. Entropy accumulator lifecycle' \
    docs/superpowers/plans/2026-08-22-https-vertical-slice-items-201-300.md || {
    echo "HTTPS queue plan не помечает item 256 завершённым" >&2
    exit 1
}

# TLS seed/fill contract contracts (item 257).
grep -Fq 'test-entropy-seed-fill-host' Makefile || {
    echo "Makefile не содержит test-entropy-seed-fill-host" >&2
    exit 1
}
# shellcheck disable=SC2016
grep -Fq '$(MAKE) test-entropy-seed-fill-host' Makefile || {
    echo "test-host-suite не вызывает test-entropy-seed-fill-host" >&2
    exit 1
}
grep -Fq 'ENTROPY_SEED_FILL_VERSION' \
    kernel/include/entropy_seed_fill.h || {
    echo "entropy_seed_fill.h не содержит version" >&2
    exit 1
}
grep -Fq 'ENTROPY_SEED_FILL_STATE_UNAVAILABLE' \
    kernel/include/entropy_seed_fill.h || {
    echo "entropy_seed_fill.h не содержит unavailable state" >&2
    exit 1
}
grep -Fq 'ENTROPY_SEED_FILL_STATE_DEGRADED' \
    kernel/include/entropy_seed_fill.h || {
    echo "entropy_seed_fill.h не содержит degraded state" >&2
    exit 1
}
grep -Fq 'ENTROPY_SEED_FILL_STATE_READY' \
    kernel/include/entropy_seed_fill.h || {
    echo "entropy_seed_fill.h не содержит ready state" >&2
    exit 1
}
grep -Fq 'ENTROPY_SEED_FILL_OUTPUT_OVERLAP' \
    kernel/include/entropy_seed_fill.h || {
    echo "entropy_seed_fill.h не содержит output-overlap status" >&2
    exit 1
}
grep -Fq 'ENTROPY_SEED_FILL_SEED_ALREADY_PUBLISHED' \
    kernel/include/entropy_seed_fill.h || {
    echo "entropy_seed_fill.h не содержит seed-published status" >&2
    exit 1
}
grep -Fq 'struct entropy_seed_fill_config' \
    kernel/include/entropy_seed_fill.h || {
    echo "entropy_seed_fill.h не содержит config" >&2
    exit 1
}
grep -Fq 'struct entropy_seed_fill' \
    kernel/include/entropy_seed_fill.h || {
    echo "entropy_seed_fill.h не содержит controller" >&2
    exit 1
}
grep -Fq 'enum entropy_seed_fill_state' \
    kernel/include/entropy_seed_fill.h || {
    echo "entropy_seed_fill.h не содержит state enum" >&2
    exit 1
}
grep -Fq 'enum entropy_seed_fill_fault' \
    kernel/include/entropy_seed_fill.h || {
    echo "entropy_seed_fill.h не содержит fault enum" >&2
    exit 1
}
if grep -nE 'entropy_seed_fill_(read|peek|extract|snapshot|bytes|accumulator)\(' \
        kernel/include/entropy_seed_fill.h; then
    echo "entropy_seed_fill.h предоставляет публичное чтение seed/bytes" >&2
    exit 1
fi
for entropy_seed_fill_symbol in entropy_seed_fill_init \
                                entropy_seed_fill_seed \
                                entropy_seed_fill_fill \
                                entropy_seed_fill_state \
                                entropy_seed_fill_fault \
                                entropy_seed_fill_initialized \
                                entropy_seed_fill_ready \
                                entropy_seed_fill_seed_published \
                                entropy_seed_fill_generation \
                                entropy_seed_fill_source_id \
                                entropy_seed_fill_seed_material_bytes \
                                entropy_seed_fill_fill_count \
                                entropy_seed_fill_status_string \
                                entropy_seed_fill_state_string \
                                entropy_seed_fill_fault_string; do
    nm build/hfoc-kernel.elf | grep -F " $entropy_seed_fill_symbol" \
        >/dev/null || {
        echo "Production kernel не содержит $entropy_seed_fill_symbol" >&2
        exit 1
    }
done
if grep -nE '\b(kmalloc|kfree|pmm_alloc_contiguous_pages|pmm_free_contiguous_pages)\b|\b(serial_printf|printf|fprintf|audit_|audit_record|panic)\b|\b(sleep|delay|hlt|sti|cli|pic_|lapic_|ioapic_|smp_)\b' \
        kernel/src/entropy_seed_fill.c; then
    echo "entropy_seed_fill.c выполняет запрещённое выделение, диагностику или блокировку" >&2
    exit 1
fi
if grep -nE 'entropy_seed_fill_(init|seed|fill)\(' \
        kernel/src/arch/x86_64/interrupts.c; then
    echo "Entropy seed/fill contract вызывается из interrupt path" >&2
    exit 1
fi
if nm -g build/hfoc-kernel.elf | grep 'entropy_seed_fill_host_test_'; then
    echo "entropy_seed_fill_host_test_ символы попали в kernel ELF" >&2
    exit 1
fi
if bash scripts/hfoc-file-marker.sh build/hfoc-kernel.elf \
        'Entropy seed/fill host tests: ALL OK'; then
    echo "Production kernel содержит entropy seed/fill host marker" >&2
    exit 1
fi
for entropy_seed_fill_doc in README.md docs/ARCHITECTURE.md docs/ROADMAP.md; do
    if ! grep -Eiq 'seed.?fill contract|seed.?fill' \
            "$entropy_seed_fill_doc"; then
        echo "$entropy_seed_fill_doc не документирует TLS seed/fill contract" >&2
        exit 1
    fi
done
grep -Fq -- '- [x] **257. TLS seed/fill contract' \
    docs/superpowers/plans/2026-08-22-https-vertical-slice-items-201-300.md || {
    echo "HTTPS queue plan не помечает item 257 завершённым" >&2
    exit 1
}
