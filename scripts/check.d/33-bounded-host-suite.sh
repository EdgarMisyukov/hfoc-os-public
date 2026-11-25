#!/usr/bin/env bash
# ITEM-387: contracted slice of the HFOC OS check suite. Sourced in pinned
#           order by scripts/check.sh; lint it on its own with shellcheck.

# Bounded host suite contracts.
for host_suite_contract in \
    'scripts/run-host-suite.sh' \
    'tests/test-host-suite-runner.sh' \
    'tests/test-host-suite-manifest.sh' \
    'HOST_PARALLEL_PHASE_1' \
    'HFOC_HOST_JOBS' \
    'HFOC_HOST_SUITE_ARTIFACT_DIR' \
    'test-host-suite-serial:'; do
    grep -Fq "$host_suite_contract" Makefile scripts/run-host-suite.sh || {
        echo "Makefile или run-host-suite.sh не содержит host suite contract: $host_suite_contract" >&2
        exit 1
    }
done
bash tests/test-host-suite-runner.sh
bash tests/test-host-suite-manifest.sh

# ITEM-393: the boot-path proofs run on the stack the bootloader handed over.
# The frame budget check and its bound must stay wired into the canonical host
# recipe, so a proof cannot quietly start spending the boot stack again.
grep -Fq 'test-drill-stack-budget-host:' Makefile || {
    echo "Makefile не содержит test-drill-stack-budget-host" >&2
    exit 1
}
# shellcheck disable=SC2016
grep -Fq '$(MAKE) test-drill-stack-budget-host' Makefile || {
    echo "test-host-suite-serial не вызывает test-drill-stack-budget-host" >&2
    exit 1
}
grep -Fq 'DRILL_STACK_BUDGET_BYTES := 4096' Makefile || {
    echo "Makefile не фиксирует бюджет стека boot-path доказательств" >&2
    exit 1
}
grep -Fq 'tests/drill-stack-budget-host.sh' Makefile || {
    echo "Makefile не запускает tests/drill-stack-budget-host.sh" >&2
    exit 1
}
bash tests/drill-stack-budget-host.sh --makefile 4096 || {
    echo "boot-path доказательства выходят за бюджет стека" >&2
    exit 1
}

# ITEM-437: the budget proof measures a file, not a pipe.  Its reader stops at
# the frame instruction, and while the disassembly came straight out of
# objdump that closed the pipe on a writer still producing tens of kilobytes:
# SIGPIPE became the status of the assignment under `set -o pipefail`, and the
# check failed on an idle host while passing under CI load - backwards for a
# budget that must be deterministic.  The measurement is pinned to the file it
# now writes, and the race drill proves the shape: a disassembler with a tail
# far larger than a pipe buffer must not be able to kill the proof.
# shellcheck disable=SC2016  # the single-quoted text is what the script says
grep -Fq 'objdump -d "$object" >"$disassembly"' \
    tests/drill-stack-budget-host.sh || {
    echo "бюджет стека снова измеряется из канала, а не из файла" >&2
    exit 1
}
# shellcheck disable=SC2016  # the single-quoted text is what the script says
grep -Fq ' "$disassembly")' tests/drill-stack-budget-host.sh || {
    echo "измерение бюджета стека не читает файл дизассемблера" >&2
    exit 1
}
grep -Fq 'test-drill-stack-budget-race-host:' Makefile || {
    echo "Makefile не содержит test-drill-stack-budget-race-host" >&2
    exit 1
}
# shellcheck disable=SC2016  # the single-quoted text is what the Makefile says
grep -Fq '$(MAKE) test-drill-stack-budget-race-host' Makefile || {
    echo "test-host-suite-serial не вызывает test-drill-stack-budget-race-host" \
        >&2
    exit 1
}
grep -Fq 'test-drill-stack-budget-race-host' \
    tests/test-host-suite-manifest.sh || {
    echo "манифест host suite не содержит test-drill-stack-budget-race-host" >&2
    exit 1
}
bash tests/drill-stack-budget-race-host.sh || {
    echo "гонка бюджета стека не доказана" >&2
    exit 1
}

# Bounded acceptance suite contracts.
for acceptance_suite_contract in \
    'scripts/run-acceptance-suite.sh' \
    'tests/test-acceptance-suite-runner.sh' \
    'tests/test-acceptance-suite-manifest.sh' \
    'HFOC_ACCEPTANCE_JOBS' \
    'test-acceptance-suite-runner:' \
    'test-acceptance-suite-manifest:'; do
    grep -Fq "$acceptance_suite_contract" \
        Makefile scripts/run-acceptance-suite.sh \
        scripts/test-release-appliance-v1-acceptance.sh \
        scripts/test-https-vertical-slice-acceptance.sh || {
        echo "Acceptance suite contract is missing: $acceptance_suite_contract" >&2
        exit 1
    }
done
test -x scripts/run-acceptance-suite.sh || {
    echo 'scripts/run-acceptance-suite.sh is not executable' >&2
    exit 1
}
test -x tests/test-acceptance-suite-runner.sh || {
    echo 'tests/test-acceptance-suite-runner.sh is not executable' >&2
    exit 1
}
test -x tests/test-acceptance-suite-manifest.sh || {
    echo 'tests/test-acceptance-suite-manifest.sh is not executable' >&2
    exit 1
}
bash -n scripts/run-acceptance-suite.sh
bash -n tests/test-acceptance-suite-runner.sh
bash -n tests/test-acceptance-suite-manifest.sh
bash tests/test-acceptance-suite-runner.sh
bash tests/test-acceptance-suite-manifest.sh

# Release profile descriptor contracts.
grep -Fq 'test-release-profile-host' Makefile || {
    echo "Makefile не содержит test-release-profile-host" >&2
    exit 1
}
# shellcheck disable=SC2016
grep -Fq '$(MAKE) test-release-profile-host' Makefile || {
    echo "test-host-suite не вызывает test-release-profile-host" >&2
    exit 1
}
grep -Fq 'RELEASE_PROFILE_VERSION' \
    kernel/include/release_profile.h || {
    echo "release_profile.h не содержит RELEASE_PROFILE_VERSION" >&2
    exit 1
}
grep -Fq 'RELEASE_PROFILE_ID "HFOC-APPLIANCE-V1"' \
    kernel/include/release_profile.h || {
    echo "release_profile.h не содержит HFOC-APPLIANCE-V1" >&2
    exit 1
}
grep -Fq 'RELEASE_PROFILE_ARCH SYSTEM_IMAGE_ARCH_VALUE' \
    kernel/include/release_profile.h || {
    echo "release_profile.h не содержит supported architecture" >&2
    exit 1
}
grep -Fq 'RELEASE_PROFILE_IMAGE_GENERATION' \
    kernel/include/release_profile.h || {
    echo "release_profile.h не содержит image generation" >&2
    exit 1
}
grep -Fq 'RELEASE_PROFILE_INTEGRATION_SCHEMA_VERSION' \
    kernel/include/release_profile.h || {
    echo "release_profile.h не содержит integration schema version" >&2
    exit 1
}
grep -Fq 'struct release_profile' \
    kernel/include/release_profile.h || {
    echo "release_profile.h не содержит descriptor struct" >&2
    exit 1
}
grep -Fq 'struct release_profile_request' \
    kernel/include/release_profile.h || {
    echo "release_profile.h не содержит request struct" >&2
    exit 1
}
for release_profile_symbol in release_profile_init release_profile_validate \
                             release_profile_check \
                             release_profile_status_string; do
    nm build/hfoc-kernel.elf | grep -F " $release_profile_symbol" \
        >/dev/null || {
        echo "Production kernel не содержит $release_profile_symbol" >&2
        exit 1
    }
done
if grep -nE '\b(kmalloc|kfree|pmm_alloc_contiguous_pages|pmm_free_contiguous_pages)\b|\b(serial_printf|printf|fprintf|serial_write|console_write|panic)\b|\b(sleep|delay|hlt|sti|cli|pic_|lapic_|ioapic_|smp_)\b|\b(block_device_read|block_device_write)\b|\b(event_ring_|audit_)\b' \
        kernel/src/release_profile.c; then
    echo "release_profile.c выполняет запрещённое выделение, диагностику, аудит или блокирование" >&2
    exit 1
fi
if nm -g build/hfoc-kernel.elf |
        grep 'release_profile_host_test_'; then
    echo "release_profile_host_test_ символы попали в kernel ELF" >&2
    exit 1
fi
if bash scripts/hfoc-file-marker.sh build/hfoc-kernel.elf \
        'Release profile host tests: ALL OK'; then
    echo "Production kernel содержит release profile host marker" >&2
    exit 1
fi
if ! printf '#include "release_profile.h"\nvoid hfoc_release_profile_header(void) { (void)RELEASE_PROFILE_VERSION; }\n' |
    gcc -std=c11 -Wall -Wextra -Werror -Wpedantic -ffreestanding \
        -Ikernel/include -x c -c - -o /dev/null; then
    echo "release_profile.h не компилируется как self-contained header" >&2
    exit 1
fi
for release_profile_doc in README.md docs/ARCHITECTURE.md docs/ROADMAP.md; do
    if ! grep -Eiq 'release profile' "$release_profile_doc"; then
        echo "$release_profile_doc не документирует release profile" >&2
        exit 1
    fi
done
grep -Fq -- '- [x] **191. Release profile descriptor' \
    docs/superpowers/plans/2026-08-17-appliance-v1-next-100-pr-queue-execution.md || {
    echo "appliance queue plan не помечает item 191 завершённым" >&2
    exit 1
}

# Reproducible build input manifest contracts.
grep -Fq 'test-build-input-manifest-host' Makefile || {
    echo "Makefile не содержит test-build-input-manifest-host" >&2
    exit 1
}
# shellcheck disable=SC2016
grep -Fq '$(MAKE) test-build-input-manifest-host' Makefile || {
    echo "test-host-suite не вызывает test-build-input-manifest-host" >&2
    exit 1
}
# shellcheck disable=SC2016
grep -Fq 'kernel: build-input-manifest $(KERNEL)' Makefile || {
    echo "kernel target не зависит от build input manifest" >&2
    exit 1
}
[[ -f build/hfoc-build-input.txt ]] || {
    echo "make kernel не создал build input manifest" >&2
    exit 1
}
grep -Fq "source_revision=$(git rev-parse HEAD)" \
    build/hfoc-build-input.txt || {
    echo "build input manifest содержит неверную source revision" >&2
    exit 1
}
grep -Eq '^generated_input_.*=[0-9a-f]{64}$' \
    build/hfoc-build-input.txt || {
    echo "build input manifest не содержит generated input digest" >&2
    exit 1
}
bash tests/test-build-input-manifest-host.sh
for build_input_doc in README.md docs/ARCHITECTURE.md docs/ROADMAP.md; do
    if ! grep -Eiq 'build input manifest|build-input-manifest' \
        "$build_input_doc"; then
        echo "$build_input_doc не документирует build input manifest" >&2
        exit 1
    fi
done
grep -Fq -- '- [x] **192. Reproducible build input manifest' \
    docs/superpowers/plans/2026-08-17-appliance-v1-next-100-pr-queue-execution.md || {
    echo "appliance queue plan не помечает item 192 завершённым" >&2
    exit 1
}

# Reproducible image comparison contracts.
grep -Fq 'test-reproducible-image-comparison-host' Makefile || {
    echo "Makefile не содержит test-reproducible-image-comparison-host" >&2
    exit 1
}
# shellcheck disable=SC2016
grep -Fq '$(MAKE) test-reproducible-image-comparison-host' Makefile || {
    echo "test-host-suite не вызывает test-reproducible-image-comparison-host" >&2
    exit 1
}
grep -Fq 'test-reproducible-image-host' Makefile || {
    echo "Makefile не содержит test-reproducible-image-host" >&2
    exit 1
}
# shellcheck disable=SC2016
grep -Fq 'CCACHE_DISABLE=1 $(MAKE) -j$(HFOC_IMAGE_JOBS) iso' Makefile || {
    echo "test-reproducible-image-host не использует bounded parallel ISO build" >&2
    exit 1
}
# shellcheck disable=SC2016
grep -Fq 'check_3_target=test-reproducible-image-host' \
    release/release-appliance-v1-acceptance.txt || {
    echo "acceptance matrix не содержит test-reproducible-image-host" >&2
    exit 1
}
grep -Fq 'scripts/capture-reproducible-image-digests.sh' Makefile || {
    echo "Makefile не вызывает capture-reproducible-image-digests.sh" >&2
    exit 1
}
grep -Fq 'scripts/compare-reproducible-image-digests.sh' Makefile || {
    echo "Makefile не вызывает compare-reproducible-image-digests.sh" >&2
    exit 1
}
bash -n scripts/capture-reproducible-image-digests.sh || {
    echo "Синтаксис capture-reproducible-image-digests.sh некорректен" >&2
    exit 1
}
bash -n scripts/compare-reproducible-image-digests.sh || {
    echo "Синтаксис compare-reproducible-image-digests.sh некорректен" >&2
    exit 1
}
bash tests/test-reproducible-image-comparison-host.sh
for reproducible_image_doc in README.md docs/ARCHITECTURE.md docs/ROADMAP.md; do
    if ! grep -Eiq 'reproducible image comparison|reproducible-image' \
        "$reproducible_image_doc"; then
        echo "$reproducible_image_doc не документирует reproducible image comparison" >&2
        exit 1
    fi
done
grep -Fq -- '- [x] **193. Reproducible image comparison' \
    docs/superpowers/plans/2026-08-17-appliance-v1-next-100-pr-queue-execution.md || {
    echo "appliance queue plan не помечает item 193 завершённым" >&2
    exit 1
}

# Release artifact manifest contracts.
grep -Fq 'test-release-artifact-manifest-host' Makefile || {
    echo "Makefile не содержит test-release-artifact-manifest-host" >&2
    exit 1
}
# shellcheck disable=SC2016
grep -Fq '$(MAKE) test-release-artifact-manifest-host' Makefile || {
    echo "test-host-suite не вызывает test-release-artifact-manifest-host" >&2
    exit 1
}
grep -Fq 'test-release-artifact-host' Makefile || {
    echo "Makefile не содержит test-release-artifact-host" >&2
    exit 1
}
# shellcheck disable=SC2016
grep -Fq 'check_4_target=test-release-artifact-host' \
    release/release-appliance-v1-acceptance.txt || {
    echo "acceptance matrix не содержит test-release-artifact-host" >&2
    exit 1
}
grep -Fq 'scripts/generate-release-artifact-manifest.sh' Makefile || {
    echo "Makefile не вызывает generate-release-artifact-manifest.sh" >&2
    exit 1
}
grep -Fq 'scripts/verify-release-artifact-manifest.sh' Makefile || {
    echo "Makefile не вызывает verify-release-artifact-manifest.sh" >&2
    exit 1
}
grep -Fq 'scripts/make-release-update-bundle.sh' Makefile || {
    echo "Makefile не вызывает make-release-update-bundle.sh" >&2
    exit 1
}
bash -n scripts/generate-release-artifact-manifest.sh || {
    echo "Синтаксис generate-release-artifact-manifest.sh некорректен" >&2
    exit 1
}
bash -n scripts/verify-release-artifact-manifest.sh || {
    echo "Синтаксис verify-release-artifact-manifest.sh некорректен" >&2
    exit 1
}
bash -n scripts/make-release-update-bundle.sh || {
    echo "Синтаксис make-release-update-bundle.sh некорректен" >&2
    exit 1
}
bash tests/test-release-artifact-manifest-host.sh
for release_artifact_doc in README.md docs/ARCHITECTURE.md docs/ROADMAP.md; do
    if ! grep -Eiq 'release artifact manifest|release-artifact' \
        "$release_artifact_doc"; then
        echo "$release_artifact_doc не документирует release artifact manifest" >&2
        exit 1
    fi
done
grep -Fq -- '- [x] **194. Release artifact manifest' \
    docs/superpowers/plans/2026-08-17-appliance-v1-next-100-pr-queue-execution.md || {
    echo "appliance queue plan не помечает item 194 завершённым" >&2
    exit 1
}

# Release upgrade matrix contracts.
grep -Fq 'test-release-upgrade-matrix-host' Makefile || {
    echo "Makefile не содержит test-release-upgrade-matrix-host" >&2
    exit 1
}
# shellcheck disable=SC2016
grep -Fq '$(MAKE) test-release-upgrade-matrix-host' Makefile || {
    echo "test-host-suite не вызывает test-release-upgrade-matrix-host" >&2
    exit 1
}
grep -Fq 'test-release-upgrade-matrix:' Makefile || {
    echo "Makefile не содержит test-release-upgrade-matrix target" >&2
    exit 1
}
grep -Fq 'check_5_target=test-release-upgrade-matrix' \
    release/release-appliance-v1-acceptance.txt || {
    echo "acceptance matrix не содержит test-release-upgrade-matrix" >&2
    exit 1
}
grep -Fq 'scripts/verify-release-upgrade-matrix.sh' Makefile || {
    echo "Makefile не вызывает verify-release-upgrade-matrix.sh" >&2
    exit 1
}
bash -n scripts/verify-release-upgrade-matrix.sh || {
    echo "Синтаксис verify-release-upgrade-matrix.sh некорректен" >&2
    exit 1
}
bash tests/test-release-upgrade-matrix-host.sh
bash scripts/verify-release-upgrade-matrix.sh \
    release/release-upgrade-matrix.txt
for release_upgrade_doc in README.md docs/ARCHITECTURE.md docs/ROADMAP.md; do
    if ! grep -Eiq 'release upgrade matrix|release-upgrade-matrix' \
        "$release_upgrade_doc"; then
        echo "$release_upgrade_doc не документирует release upgrade matrix" >&2
        exit 1
    fi
done
grep -Fq -- '- [x] **195. Release upgrade matrix' \
    docs/superpowers/plans/2026-08-17-appliance-v1-next-100-pr-queue-execution.md || {
    echo "appliance queue plan не помечает item 195 завершённым" >&2
    exit 1
}

# Release backup/restore matrix contracts.
grep -Fq 'test-release-backup-restore-matrix-host' Makefile || {
    echo "Makefile не содержит test-release-backup-restore-matrix-host" >&2
    exit 1
}
# shellcheck disable=SC2016
grep -Fq '$(MAKE) test-release-backup-restore-matrix-host' Makefile || {
    echo "test-host-suite не вызывает test-release-backup-restore-matrix-host" >&2
    exit 1
}
grep -Fq 'test-release-backup-restore-matrix:' Makefile || {
    echo "Makefile не содержит test-release-backup-restore-matrix target" >&2
    exit 1
}
grep -Fq 'check_6_target=test-release-backup-restore-matrix' \
    release/release-appliance-v1-acceptance.txt || {
    echo "acceptance matrix не содержит test-release-backup-restore-matrix" >&2
    exit 1
}
grep -Fq 'scripts/verify-release-backup-restore-matrix.sh' Makefile || {
    echo "Makefile не вызывает verify-release-backup-restore-matrix.sh" >&2
    exit 1
}
bash -n scripts/verify-release-backup-restore-matrix.sh || {
    echo "Синтаксис verify-release-backup-restore-matrix.sh некорректен" >&2
    exit 1
}
bash tests/test-release-backup-restore-matrix-host.sh
bash scripts/verify-release-backup-restore-matrix.sh \
    release/release-backup-restore-matrix.txt
for release_backup_doc in README.md docs/ARCHITECTURE.md docs/ROADMAP.md; do
    if ! grep -Eiq 'release backup/restore matrix|release-backup-restore-matrix' \
        "$release_backup_doc"; then
        echo "$release_backup_doc не документирует release backup/restore matrix" >&2
        exit 1
    fi
done
grep -Fq -- '- [x] **196. Release backup/restore matrix' \
    docs/superpowers/plans/2026-08-17-appliance-v1-next-100-pr-queue-execution.md || {
    echo "appliance queue plan не помечает item 196 завершённым" >&2
    exit 1
}
if bash scripts/hfoc-file-marker.sh build/hfoc-kernel.elf \
        'Release backup/restore matrix no-change host test: OK'; then
    echo "Production kernel содержит release backup/restore matrix host marker" >&2
    exit 1
fi

# Release security baseline contracts.
grep -Fq 'test-release-security-baseline-host' Makefile || {
    echo "Makefile не содержит test-release-security-baseline-host" >&2
    exit 1
}
# shellcheck disable=SC2016
grep -Fq '$(MAKE) test-release-security-baseline-host' Makefile || {
    echo "test-host-suite не вызывает test-release-security-baseline-host" >&2
    exit 1
}
grep -Fq 'test-release-security-baseline:' Makefile || {
    echo "Makefile не содержит test-release-security-baseline target" >&2
    exit 1
}
grep -Fq 'check_7_target=test-release-security-baseline' \
    release/release-appliance-v1-acceptance.txt || {
    echo "acceptance matrix не содержит test-release-security-baseline" >&2
    exit 1
}
grep -Fq 'scripts/test-release-security-baseline.sh' Makefile || {
    echo "Makefile не вызывает test-release-security-baseline.sh" >&2
    exit 1
}
bash -n scripts/verify-release-security-baseline.sh || {
    echo "Синтаксис verify-release-security-baseline.sh некорректен" >&2
    exit 1
}
bash -n scripts/test-release-security-baseline.sh || {
    echo "Синтаксис test-release-security-baseline.sh некорректен" >&2
    exit 1
}
bash -n tests/test-release-security-baseline-host.sh || {
    echo "Синтаксис test-release-security-baseline-host.sh некорректен" >&2
    exit 1
}
bash tests/test-release-security-baseline-host.sh
bash scripts/verify-release-security-baseline.sh \
    release/release-security-baseline.txt
for security_target in \
    test-vmm-user-host \
    test-user-init-process-host \
    test-syscall-host \
    test-syscall-origin-host \
    test-syscall-gate-host \
    test-local-control-outage-drill-host \
    test-authorization-host \
    test-trust-failure-drill-host \
    test-network-admission-host \
    test-network-partition-drill-host \
    test-integration-principal-host \
    test-integration-config-intent-host \
    test-integration-replay-outage-drill-host; do
    grep -Fq "${security_target}:" Makefile || {
        echo "Makefile не содержит canonical security target ${security_target}" >&2
        exit 1
    }
done
for release_security_doc in README.md docs/ARCHITECTURE.md docs/ROADMAP.md; do
    if ! grep -Eiq 'release security baseline|release-security-baseline' \
        "$release_security_doc"; then
        echo "$release_security_doc не документирует release security baseline" >&2
        exit 1
    fi
done
grep -Fq -- '- [x] **197. Release security baseline' \
    docs/superpowers/plans/2026-08-17-appliance-v1-next-100-pr-queue-execution.md || {
    echo "appliance queue plan не помечает item 197 завершённым" >&2
    exit 1
}

# Release operational baseline contracts.
grep -Fq 'test-release-operational-baseline-host' Makefile || {
    echo "Makefile не содержит test-release-operational-baseline-host" >&2
    exit 1
}
# shellcheck disable=SC2016
grep -Fq '$(MAKE) test-release-operational-baseline-host' Makefile || {
    echo "test-host-suite не вызывает test-release-operational-baseline-host" >&2
    exit 1
}
grep -Fq 'test-release-operational-baseline:' Makefile || {
    echo "Makefile не содержит test-release-operational-baseline target" >&2
    exit 1
}
grep -Fq 'check_8_target=test-release-operational-baseline' \
    release/release-appliance-v1-acceptance.txt || {
    echo "acceptance matrix не содержит test-release-operational-baseline" >&2
    exit 1
}
grep -Fq 'scripts/test-release-operational-baseline.sh' Makefile || {
    echo "Makefile не вызывает test-release-operational-baseline.sh" >&2
    exit 1
}
grep -Fq 'test-release-operational-qemu-liveness' Makefile || {
    echo "Makefile не содержит test-release-operational-qemu-liveness" >&2
    exit 1
}
bash -n scripts/verify-release-operational-baseline.sh || {
    echo "Синтаксис verify-release-operational-baseline.sh некорректен" >&2
    exit 1
}
bash -n scripts/test-release-operational-baseline.sh || {
    echo "Синтаксис test-release-operational-baseline.sh некорректен" >&2
    exit 1
}
bash -n scripts/test-release-operational-qemu-liveness.sh || {
    echo "Синтаксис test-release-operational-qemu-liveness.sh некорректен" >&2
    exit 1
}
bash -n tests/test-release-operational-baseline-host.sh || {
    echo "Синтаксис test-release-operational-baseline-host.sh некорректен" >&2
    exit 1
}
bash tests/test-release-operational-baseline-host.sh
bash scripts/verify-release-operational-baseline.sh \
    release/release-operational-baseline.txt
for operational_target in \
    test-boot-policy-host \
    test-state-schema-host \
    test-state-migration-host \
    test-service-dependency-host \
    test-service-supervisor-host \
    test-storage-health-host \
    test-service-health-host \
    test-support-bundle-manifest-host \
    test-support-bundle-exporter-host \
    test-candidate-preflight-host \
    test-update-failure-drill-host \
    test-recovery-image-host \
    test-power-cycle-continuity-drill-host \
    test-release-operational-qemu-liveness; do
    grep -Fq "${operational_target}:" Makefile || {
        echo "Makefile не содержит canonical operational target ${operational_target}" >&2
        exit 1
    }
done
if bash scripts/hfoc-file-marker.sh build/hfoc-kernel.elf \
        'Release operational baseline host test: OK'; then
    echo "Production kernel содержит release operational baseline host marker" >&2
    exit 1
fi
for release_operational_doc in README.md docs/ARCHITECTURE.md docs/ROADMAP.md; do
    if ! grep -Eiq 'release operational baseline|release-operational-baseline' \
        "$release_operational_doc"; then
        echo "$release_operational_doc не документирует release operational baseline" >&2
        exit 1
    fi
done
grep -Fq -- '- [x] **198. Release operational baseline' \
    docs/superpowers/plans/2026-08-17-appliance-v1-next-100-pr-queue-execution.md || {
    echo "appliance queue plan не помечает item 198 завершённым" >&2
    exit 1
}

# Appliance v1 disaster-recovery rehearsal contracts.
grep -Fq 'test-release-disaster-recovery-rehearsal-host' Makefile || {
    echo "Makefile не содержит test-release-disaster-recovery-rehearsal-host" >&2
    exit 1
}
# shellcheck disable=SC2016
grep -Fq '$(MAKE) test-release-disaster-recovery-rehearsal-host' Makefile || {
    echo "test-host-suite не вызывает test-release-disaster-recovery-rehearsal-host" >&2
    exit 1
}
grep -Fq 'test-release-disaster-recovery-rehearsal:' Makefile || {
    echo "Makefile не содержит test-release-disaster-recovery-rehearsal target" >&2
    exit 1
}
grep -Fq 'check_9_target=test-release-disaster-recovery-rehearsal' \
    release/release-appliance-v1-acceptance.txt || {
    echo "acceptance matrix не содержит test-release-disaster-recovery-rehearsal" >&2
    exit 1
}
grep -Fq 'scripts/test-release-disaster-recovery-rehearsal.sh' Makefile || {
    echo "Makefile не вызывает test-release-disaster-recovery-rehearsal.sh" >&2
    exit 1
}
bash -n scripts/verify-release-disaster-recovery-rehearsal.sh || {
    echo "Синтаксис verify-release-disaster-recovery-rehearsal.sh некорректен" >&2
    exit 1
}
bash -n scripts/test-release-disaster-recovery-rehearsal.sh || {
    echo "Синтаксис test-release-disaster-recovery-rehearsal.sh некорректен" >&2
    exit 1
}
bash -n tests/test-release-disaster-recovery-rehearsal-host.sh || {
    echo "Синтаксис test-release-disaster-recovery-rehearsal-host.sh некорректен" >&2
    exit 1
}
bash tests/test-release-disaster-recovery-rehearsal-host.sh
bash scripts/verify-release-disaster-recovery-rehearsal.sh \
    release/release-disaster-recovery-rehearsal.txt
for rehearsal_target in \
    test-boot-policy-regression \
    test-backup-restore-qemu \
    test-service-cascade-drill-qemu \
    test-accounting-boundary-qemu \
    test-supportability-qemu; do
    grep -Fq "${rehearsal_target}:" Makefile || {
        echo "Makefile не содержит canonical disaster-recovery target ${rehearsal_target}" >&2
        exit 1
    }
done
for release_disaster_doc in README.md docs/ARCHITECTURE.md docs/ROADMAP.md; do
    if ! grep -Eiq 'disaster-recovery rehearsal|disaster recovery rehearsal' \
        "$release_disaster_doc"; then
        echo "$release_disaster_doc не документирует disaster-recovery rehearsal" >&2
        exit 1
    fi
done
grep -Fq -- '- [x] **199. Appliance v1 disaster-recovery rehearsal' \
    docs/superpowers/plans/2026-08-17-appliance-v1-next-100-pr-queue-execution.md || {
    echo "appliance queue plan не помечает item 199 завершённым" >&2
    exit 1
}
