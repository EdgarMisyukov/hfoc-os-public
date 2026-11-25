#!/usr/bin/env bash
# ITEM-387: contracted slice of the HFOC OS check suite. Sourced in pinned
#           order by scripts/check.sh; lint it on its own with shellcheck.

# Appliance update integrity verification contracts (ITEM-331).
grep -Fq 'test-appliance-update-integrity-host' Makefile || {
    echo "Makefile не содержит test-appliance-update-integrity-host" >&2
    exit 1
}
# shellcheck disable=SC2016
grep -Fq '$(MAKE) test-appliance-update-integrity-host' Makefile || {
    echo "test-host-suite не вызывает test-appliance-update-integrity-host" >&2
    exit 1
}
grep -Fq 'APPLIANCE_UPDATE_INTEGRITY_VERSION 1U' \
    kernel/include/appliance_update_integrity.h || {
    echo "appliance_update_integrity.h не содержит версию 1" >&2
    exit 1
}
for update_integrity_symbol in \
    appliance_update_integrity_init \
    appliance_update_integrity_verify \
    appliance_update_integrity_is_verified \
    appliance_update_integrity_status_string; do
    grep -Fq "$update_integrity_symbol" \
        kernel/include/appliance_update_integrity.h \
        kernel/src/appliance_update_integrity.c || {
        echo "appliance_update_integrity не объявляет $update_integrity_symbol" >&2
        exit 1
    }
done
if grep -nE '\b(errno|socket|malloc|calloc|free|kmalloc|kfree|pmm_alloc|pmm_free|serial_printf|printf|fprintf|snprintf|sprintf|sleep|delay|hlt|sti|cli|panic)\b|\b(serial_write|console_write|event_ring_|audit_|block_device_read|block_device_write)\b' \
        kernel/src/appliance_update_integrity.c; then
    echo "appliance_update_integrity.c использует запрещённое выделение, диагностику, аудит, I/O или блокирование" >&2
    exit 1
fi
for update_integrity_doc in README.md docs/ARCHITECTURE.md docs/ROADMAP.md; do
    if ! grep -Eiq 'update integrity verification|update integrity and signature verification' \
            "$update_integrity_doc"; then
        echo "$update_integrity_doc не документирует update integrity verification" >&2
        exit 1
    fi
done
grep -Fq -- '- [x] **ITEM-331**' \
    docs/superpowers/plans/2026-09-03-hfoc-appliance-platform.md || {
    echo "roadmap plan не помечает ITEM-331 завершённым" >&2
    exit 1
}
grep -Fq -- '- [x] **ITEM-332**' \
    docs/superpowers/plans/2026-09-03-hfoc-appliance-platform.md || {
    echo "roadmap plan не помечает ITEM-332 завершённым" >&2
    exit 1
}
grep -Fq -- '- [x] **ITEM-333**' \
    docs/superpowers/plans/2026-09-03-hfoc-appliance-platform.md || {
    echo "roadmap plan не помечает ITEM-333 завершённым" >&2
    exit 1
}
grep -Fq -- '- [x] **ITEM-334**' \
    docs/superpowers/plans/2026-09-03-hfoc-appliance-platform.md || {
    echo "roadmap plan не помечает ITEM-334 завершённым" >&2
    exit 1
}
grep -Fq -- '- [x] **ITEM-335**' \
    docs/superpowers/plans/2026-09-03-hfoc-appliance-platform.md || {
    echo "roadmap plan не помечает ITEM-335 завершённым" >&2
    exit 1
}
grep -Fq -- '- [x] **ITEM-336**' \
    docs/superpowers/plans/2026-09-03-hfoc-appliance-platform.md || {
    echo "roadmap plan не помечает ITEM-336 завершённым" >&2
    exit 1
}

# Appliance recovery mode contracts (ITEM-332).
grep -Fq 'test-appliance-recovery-mode-host' Makefile || {
    echo "Makefile не содержит test-appliance-recovery-mode-host" >&2
    exit 1
}
# shellcheck disable=SC2016
grep -Fq '$(MAKE) test-appliance-recovery-mode-host' Makefile || {
    echo "test-host-suite не вызывает test-appliance-recovery-mode-host" >&2
    exit 1
}
grep -Fq 'APPLIANCE_RECOVERY_MODE_VERSION 1U' \
    kernel/include/appliance_recovery_mode.h || {
    echo "appliance_recovery_mode.h не содержит версию 1" >&2
    exit 1
}
for recovery_mode_symbol in \
    appliance_recovery_mode_init \
    appliance_recovery_mode_evaluate \
    appliance_recovery_mode_is_evaluated \
    appliance_recovery_mode_status_string \
    appliance_recovery_mode_domain_string \
    appliance_recovery_mode_action_string; do
    grep -Fq "$recovery_mode_symbol" \
        kernel/include/appliance_recovery_mode.h \
        kernel/src/appliance_recovery_mode.c || {
        echo "appliance_recovery_mode не объявляет $recovery_mode_symbol" >&2
        exit 1
    }
done
if grep -nE '\b(errno|socket|malloc|calloc|free|kmalloc|kfree|pmm_alloc|pmm_free|serial_printf|printf|fprintf|snprintf|sprintf|sleep|delay|hlt|sti|cli|panic)\b|\b(serial_write|console_write|event_ring_|audit_|block_device_read|block_device_write)\b' \
        kernel/src/appliance_recovery_mode.c; then
    echo "appliance_recovery_mode.c использует запрещённое выделение, диагностику, аудит, I/O или блокирование" >&2
    exit 1
fi
for recovery_mode_doc in README.md docs/ARCHITECTURE.md docs/ROADMAP.md; do
    if ! grep -Eiq 'appliance recovery mode|recovery mode' \
            "$recovery_mode_doc"; then
        echo "$recovery_mode_doc не документирует appliance recovery mode" >&2
        exit 1
    fi
done
grep -Fq -- '- [x] **ITEM-332**' \
    docs/superpowers/plans/2026-09-03-hfoc-appliance-platform.md || {
    echo "roadmap plan не помечает ITEM-332 завершённым" >&2
    exit 1
}
grep -Fq -- '- [x] **ITEM-333**' \
    docs/superpowers/plans/2026-09-03-hfoc-appliance-platform.md || {
    echo "roadmap plan не помечает ITEM-333 завершённым" >&2
    exit 1
}
grep -Fq -- '- [x] **ITEM-334**' \
    docs/superpowers/plans/2026-09-03-hfoc-appliance-platform.md || {
    echo "roadmap plan не помечает ITEM-334 завершённым" >&2
    exit 1
}
grep -Fq -- '- [x] **ITEM-335**' \
    docs/superpowers/plans/2026-09-03-hfoc-appliance-platform.md || {
    echo "roadmap plan не помечает ITEM-335 завершённым" >&2
    exit 1
}
grep -Fq -- '- [x] **ITEM-336**' \
    docs/superpowers/plans/2026-09-03-hfoc-appliance-platform.md || {
    echo "roadmap plan не помечает ITEM-336 завершённым" >&2
    exit 1
}

# Appliance factory reset contracts (ITEM-333).
grep -Fq 'test-appliance-factory-reset-host' Makefile || {
    echo "Makefile не содержит test-appliance-factory-reset-host" >&2
    exit 1
}
# shellcheck disable=SC2016
grep -Fq '$(MAKE) test-appliance-factory-reset-host' Makefile || {
    echo "test-host-suite не вызывает test-appliance-factory-reset-host" >&2
    exit 1
}
grep -Fq 'APPLIANCE_FACTORY_RESET_VERSION 1U' \
    kernel/include/appliance_factory_reset.h || {
    echo "appliance_factory_reset.h не содержит версию 1" >&2
    exit 1
}
for factory_reset_symbol in \
    appliance_factory_reset_init \
    appliance_factory_reset_evaluate \
    appliance_factory_reset_is_published \
    appliance_factory_reset_status_string \
    appliance_factory_reset_action_string; do
    grep -Fq "$factory_reset_symbol" \
        kernel/include/appliance_factory_reset.h \
        kernel/src/appliance_factory_reset.c || {
        echo "appliance_factory_reset не объявляет $factory_reset_symbol" >&2
        exit 1
    }
done
if grep -nE '\b(errno|socket|malloc|calloc|free|kmalloc|kfree|pmm_alloc|pmm_free|serial_printf|printf|fprintf|snprintf|sprintf|sleep|delay|hlt|sti|cli|panic)\b|\b(serial_write|console_write|event_ring_|audit_|block_device_read|block_device_write)\b' \
        kernel/src/appliance_factory_reset.c; then
    echo "appliance_factory_reset.c использует запрещённое выделение, диагностику, аудит, I/O или блокирование" >&2
    exit 1
fi
for factory_reset_doc in README.md docs/ARCHITECTURE.md docs/ROADMAP.md; do
    if ! grep -Eiq 'appliance factory reset|factory reset' \
            "$factory_reset_doc"; then
        echo "$factory_reset_doc не документирует appliance factory reset" >&2
        exit 1
    fi
done
grep -Fq -- '- [x] **ITEM-333**' \
    docs/superpowers/plans/2026-09-03-hfoc-appliance-platform.md || {
    echo "roadmap plan не помечает ITEM-333 завершённым" >&2
    exit 1
}
grep -Fq -- '- [x] **ITEM-334**' \
    docs/superpowers/plans/2026-09-03-hfoc-appliance-platform.md || {
    echo "roadmap plan не помечает ITEM-334 завершённым" >&2
    exit 1
}
grep -Fq -- '- [x] **ITEM-335**' \
    docs/superpowers/plans/2026-09-03-hfoc-appliance-platform.md || {
    echo "roadmap plan не помечает ITEM-335 завершённым" >&2
    exit 1
}
grep -Fq -- '- [x] **ITEM-336**' \
    docs/superpowers/plans/2026-09-03-hfoc-appliance-platform.md || {
    echo "roadmap plan не помечает ITEM-336 завершённым" >&2
    exit 1
}

# Integrated appliance boot acceptance contracts (ITEM-334).
grep -Fq 'test-integrated-appliance-boot-acceptance-qemu' Makefile || {
    echo "Makefile не содержит test-integrated-appliance-boot-acceptance-qemu" >&2
    exit 1
}
# shellcheck disable=SC2016
grep -Fq '$(MAKE) test-integrated-appliance-boot-acceptance-qemu' Makefile || {
    echo "test-serial не вызывает test-integrated-appliance-boot-acceptance-qemu" >&2
    exit 1
}
grep -Fq 'serial_write("[PRODUCTION-BOOT] status=");' \
    kernel/src/kernel.c || {
    echo "kernel.c не содержит PRODUCTION-BOOT marker" >&2
    exit 1
}
grep -Fq 'serial_write("[APPLIANCE-BOOT] runtime=");' \
    kernel/src/kernel.c || {
    echo "kernel.c не содержит APPLIANCE-BOOT marker" >&2
    exit 1
}
grep -Fq 'appliance_production_boot_run(' kernel/src/kernel.c || {
    echo "kernel.c не запускает production boot coordinator" >&2
    exit 1
}
grep -Fq 'appliance_production_storage_probe' kernel/src/kernel.c || {
    echo "kernel.c не использует production storage backend" >&2
    exit 1
}
grep -Fq 'appliance_production_boot_probe_backends' \
    kernel/src/appliance_production_boot.c || {
    echo "coordinator не выполняет fail-closed backend selection" >&2
    exit 1
}
grep -Fq 'APPLIANCE_PRODUCTION_BOOT_AMBIGUOUS_BACKEND' \
    kernel/src/appliance_production_boot.c || {
    echo "coordinator не отклоняет ambiguous backend" >&2
    exit 1
}
test -f tests/appliance-production-boot-host.c || {
    echo "Отсутствует production boot coordinator host test" >&2
    exit 1
}
test -f scripts/test-qemu-production-storage-boot.sh || {
    echo "Отсутствует production storage QEMU proof" >&2
    exit 1
}
grep -Fq 'nvme_controller_bind' \
    kernel/src/appliance_production_storage.c || {
    echo "production storage adapter не использует existing NVMe seam" >&2
    exit 1
}
grep -Fq 'APPLIANCE_PRODUCTION_BOOT_BACKEND_NVME' \
    kernel/src/appliance_production_storage.c || {
    echo "production storage adapter не публикует NVMe backend" >&2
    exit 1
}
for nvme_marker in nvme_single nvme_reload; do
    grep -Fq "$nvme_marker" \
        scripts/test-qemu-production-storage-boot.sh || {
        echo "production storage QEMU proof не покрывает NVMe: $nvme_marker" >&2
        exit 1
    }
done
grep -Fq 'APPLIANCE_CONFIG_VALIDATION_SOURCE_DEFAULT_MISSING' \
    kernel/src/console.c || {
    echo "console.c не публикует safe-default источник конфигурации" >&2
    exit 1
}
grep -Fq 'APPLIANCE_CONFIG_VALIDATION_DEFAULT_HOSTNAME' \
    kernel/src/console.c || {
    echo "console.c не публикует safe-default hostname" >&2
    exit 1
}
for integrated_doc in README.md docs/ARCHITECTURE.md docs/ROADMAP.md; do
    if ! grep -Eiq 'integrated appliance boot acceptance|integrated boot acceptance' \
            "$integrated_doc"; then
        echo "$integrated_doc не документирует integrated appliance boot acceptance" >&2
        exit 1
    fi
done
grep -Fq -- '- [x] **ITEM-334**' \
    docs/superpowers/plans/2026-09-03-hfoc-appliance-platform.md || {
    echo "roadmap plan не помечает ITEM-334 завершённым" >&2
    exit 1
}
grep -Fq -- '- [x] **ITEM-335**' \
    docs/superpowers/plans/2026-09-03-hfoc-appliance-platform.md || {
    echo "roadmap plan не помечает ITEM-335 завершённым" >&2
    exit 1
}
grep -Fq -- '- [x] **ITEM-336**' \
    docs/superpowers/plans/2026-09-03-hfoc-appliance-platform.md || {
    echo "roadmap plan не помечает ITEM-336 завершённым" >&2
    exit 1
}

# Persistent configuration reboot acceptance contracts (ITEM-335).
grep -Fq 'test-persistent-config-reboot-acceptance-qemu' Makefile || {
    echo "Makefile не содержит test-persistent-config-reboot-acceptance-qemu" >&2
    exit 1
}
# shellcheck disable=SC2016
grep -Fq '$(MAKE) test-persistent-config-reboot-acceptance-qemu' Makefile || {
    echo "test-serial не вызывает test-persistent-config-reboot-acceptance-qemu" >&2
    exit 1
}
for reboot_marker in \
    '[REBOOT-CONFIG] phase1 committed' \
    '[REBOOT-CONFIG] phase2 invalid committed' \
    '[REBOOT-CONFIG] phase3 recovery OK'; do
    grep -Fq "$reboot_marker" \
        kernel/src/appliance_persistent_config_qemu.c \
        scripts/test-qemu-persistent-config-reboot-acceptance.sh || {
        echo "persistent config reboot acceptance не содержит marker: $reboot_marker" >&2
        exit 1
    }
done
grep -Fq 'HFOCREBT' \
    kernel/src/appliance_persistent_config_qemu.c \
    scripts/test-qemu-persistent-config-reboot-acceptance.sh || {
    echo "persistent config reboot acceptance не содержит storage magic" >&2
    exit 1
}
for reboot_doc in README.md docs/ARCHITECTURE.md docs/ROADMAP.md; do
    if ! grep -Eiq 'persistent configuration reboot acceptance|persistent config reboot acceptance' \
            "$reboot_doc"; then
        echo "$reboot_doc не документирует persistent config reboot acceptance" >&2
        exit 1
    fi
done

# Management plane QEMU acceptance contracts (ITEM-336).
grep -Fq 'test-management-plane-qemu' Makefile || {
    echo "Makefile не содержит test-management-plane-qemu" >&2
    exit 1
}
# shellcheck disable=SC2016
grep -Fq '$(MAKE) test-management-plane-qemu' Makefile || {
    echo "test-serial не вызывает test-management-plane-qemu" >&2
    exit 1
}
grep -Fq 'HFOC_APPLIANCE_MANAGEMENT_PLANE_QEMU_TEST' \
    Makefile \
    kernel/src/appliance_management_plane_qemu.c \
    kernel/src/virtio_net_frame_qemu.c || {
    echo "management plane QEMU proof не определяет macro guard" >&2
    exit 1
}
test -x scripts/test-qemu-management-plane-acceptance.sh || {
    echo "scripts/test-qemu-management-plane-acceptance.sh не исполняемый" >&2
    exit 1
}
bash -n scripts/test-qemu-management-plane-acceptance.sh || {
    echo "Синтаксис scripts/test-qemu-management-plane-acceptance.sh некорректен" >&2
    exit 1
}
for management_plane_marker in \
    '[MGMT-PLANE] proof init' \
    '[MGMT-PLANE] auth absent ok' \
    '[MGMT-PLANE] credential active ok' \
    '[MGMT-PLANE] wrong proof denied ok' \
    '[MGMT-PLANE] valid session ok' \
    '[MGMT-PLANE] valid bearer ok' \
    '[MGMT-PLANE] missing bearer denied ok' \
    '[MGMT-PLANE] malformed bearer denied ok' \
    '[MGMT-PLANE] unknown bearer denied ok' \
    '[MGMT-PLANE] disable revocation ok' \
    '[MGMT-PLANE] reset recovery ok' \
    '[MGMT-PLANE] throttle denial ok' \
    '[MGMT-PLANE] authorization policy ok' \
    '[MGMT-PLANE] discovery public ok' \
    '[MGMT-PLANE] protected deny closed ok' \
    '[MGMT-PLANE] protected read authorized ok' \
    '[MGMT-PLANE] method denied ok' \
    '[MGMT-PLANE] unknown denied ok' \
    '[MGMT-PLANE] production route boundary ok' \
    '[MGMT-PLANE] proof OK'; do
    grep -Fq "$management_plane_marker" \
        kernel/src/appliance_management_plane_qemu.c \
        scripts/test-qemu-management-plane-acceptance.sh || {
        echo "management plane QEMU acceptance не содержит marker: $management_plane_marker" >&2
        exit 1
    }
done
for management_plane_doc in README.md docs/ARCHITECTURE.md docs/ROADMAP.md; do
    if ! grep -Eiq 'management plane QEMU acceptance' "$management_plane_doc"; then
        echo "$management_plane_doc не документирует management plane QEMU acceptance" >&2
        exit 1
    fi
done
grep -Fq -- '- [x] **ITEM-336**' \
    docs/superpowers/plans/2026-09-03-hfoc-appliance-platform.md || {
    echo "roadmap plan не помечает ITEM-336 завершённым" >&2
    exit 1
}

# HFOC Appliance Platform release acceptance contracts (ITEM-337).
grep -Fq 'test-hfoc-appliance-platform-release-acceptance' Makefile || {
    echo "Makefile не содержит test-hfoc-appliance-platform-release-acceptance" >&2
    exit 1
}
# shellcheck disable=SC2016
grep -Fq '$(MAKE) test-hfoc-appliance-platform-release-acceptance' Makefile || {
    echo "test не вызывает test-hfoc-appliance-platform-release-acceptance" >&2
    exit 1
}
test -x scripts/verify-hfoc-appliance-platform-release-acceptance.sh || {
    echo "scripts/verify-hfoc-appliance-platform-release-acceptance.sh не исполняемый" >&2
    exit 1
}
test -x scripts/test-hfoc-appliance-platform-release-acceptance.sh || {
    echo "scripts/test-hfoc-appliance-platform-release-acceptance.sh не исполняемый" >&2
    exit 1
}
test -x tests/test-hfoc-appliance-platform-release-acceptance-host.sh || {
    echo "tests/test-hfoc-appliance-platform-release-acceptance-host.sh не исполняемый" >&2
    exit 1
}
bash -n scripts/verify-hfoc-appliance-platform-release-acceptance.sh || {
    echo "Синтаксис verify-hfoc-appliance-platform-release-acceptance.sh некорректен" >&2
    exit 1
}
bash -n scripts/test-hfoc-appliance-platform-release-acceptance.sh || {
    echo "Синтаксис test-hfoc-appliance-platform-release-acceptance.sh некорректен" >&2
    exit 1
}
bash -n tests/test-hfoc-appliance-platform-release-acceptance-host.sh || {
    echo "Синтаксис test-hfoc-appliance-platform-release-acceptance-host.sh некорректен" >&2
    exit 1
}
grep -Fq 'hfoc_appliance_platform_release_acceptance_version=1' \
    release/hfoc-appliance-platform-release-acceptance.txt || {
    echo "platform release acceptance matrix не содержит version 1" >&2
    exit 1
}
grep -Fq 'check_count=12' \
    release/hfoc-appliance-platform-release-acceptance.txt || {
    echo "platform release acceptance matrix не содержит 12 checks" >&2
    exit 1
}
bash scripts/verify-hfoc-appliance-platform-release-acceptance.sh \
    release/hfoc-appliance-platform-release-acceptance.txt >/dev/null
bash tests/test-hfoc-appliance-platform-release-acceptance-host.sh
for platform_release_doc in README.md docs/ARCHITECTURE.md docs/ROADMAP.md; do
    if ! grep -Eiq 'platform release acceptance|release acceptance' \
            "$platform_release_doc"; then
        echo "$platform_release_doc не документирует platform release acceptance" >&2
        exit 1
    fi
done
grep -Fq -- '- [x] **ITEM-337**' \
    docs/superpowers/plans/2026-09-03-hfoc-appliance-platform.md || {
    echo "roadmap plan не помечает ITEM-337 завершённым" >&2
    exit 1
}

# Production end-to-end acceptance contracts (ITEM-369).
grep -Fq 'test-production-end-to-end-acceptance' Makefile || {
    echo "Makefile не содержит test-production-end-to-end-acceptance" >&2
    exit 1
}
# shellcheck disable=SC2016
grep -Fq '$(MAKE) test-production-end-to-end-acceptance' Makefile || {
    echo "test не вызывает test-production-end-to-end-acceptance" >&2
    exit 1
}
test -x scripts/verify-production-end-to-end-acceptance.sh || {
    echo "scripts/verify-production-end-to-end-acceptance.sh не исполняемый" >&2
    exit 1
}
test -x scripts/test-production-end-to-end-acceptance.sh || {
    echo "scripts/test-production-end-to-end-acceptance.sh не исполняемый" >&2
    exit 1
}
test -x tests/test-production-end-to-end-acceptance-host.sh || {
    echo "tests/test-production-end-to-end-acceptance-host.sh не исполняемый" >&2
    exit 1
}
test -x scripts/verify-production-end-to-end-acceptance-report.sh || {
    echo "scripts/verify-production-end-to-end-acceptance-report.sh не исполняемый" >&2
    exit 1
}
bash -n scripts/verify-production-end-to-end-acceptance.sh || {
    echo "Синтаксис verify-production-end-to-end-acceptance.sh некорректен" >&2
    exit 1
}
bash -n scripts/verify-production-end-to-end-acceptance-report.sh || {
    echo "Синтаксис verify-production-end-to-end-acceptance-report.sh некорректен" >&2
    exit 1
}
bash -n scripts/test-production-end-to-end-acceptance.sh || {
    echo "Синтаксис test-production-end-to-end-acceptance.sh некорректен" >&2
    exit 1
}
bash -n tests/test-production-end-to-end-acceptance-host.sh || {
    echo "Синтаксис test-production-end-to-end-acceptance-host.sh некорректен" >&2
    exit 1
}
grep -Fq 'hfoc_production_end_to_end_acceptance_version=2' \
    release/hfoc-production-end-to-end-acceptance.txt || {
    echo "production end-to-end acceptance matrix не содержит version 2" >&2
    exit 1
}
# ITEM-379: an implemented stage may not be silently skipped, a degraded stage
# must publish its machine-readable reason, and the report is verified by its
# own contract before a release decision may read it.
grep -Fq 'required_executable_stages=(' \
    scripts/verify-production-end-to-end-acceptance.sh || {
    echo "verify-production-end-to-end-acceptance.sh не содержит no-skip контракт" >&2
    exit 1
}
grep -Fq 'may not be skipped as NOT_TESTED' \
    scripts/verify-production-end-to-end-acceptance.sh || {
    echo "verify-production-end-to-end-acceptance.sh не отвергает skip реализованной стадии" >&2
    exit 1
}
for degraded_stage in 15 17 18 19 20 21 22; do
    grep -Eq "^stage_${degraded_stage}_reason=.+" \
        release/hfoc-production-end-to-end-acceptance.txt || {
        echo "degraded stage ${degraded_stage} не содержит machine-readable reason" >&2
        exit 1
    }
done
# ITEM-386: the service/HTTPS stage is PROVEN.  The positive contract replaces
# the previous DEGRADED reason: the stage publishes PROVEN with no degraded
# reason, the production serving proof treats the real TLS 1.3 client phase as
# fatal instead of degraded, and the mbedTLS object partition that unblocked it
# is verified from the built objects and the linked artifact.
grep -Fq 'stage_7_state=PROVEN' \
    release/hfoc-production-end-to-end-acceptance.txt || {
    echo "HTTPS_SERVICE stage не PROVEN после ITEM-386" >&2
    exit 1
}
if grep -Eq '^stage_7_reason=.+' \
        release/hfoc-production-end-to-end-acceptance.txt; then
    echo "HTTPS_SERVICE stage несёт degraded reason при PROVEN" >&2
    exit 1
fi
grep -Fq 'Production serving live TLS 1.3 client: OK' \
    scripts/test-qemu-production-service-https.sh || {
    echo "production serving proof не публикует live TLS 1.3 client OK" >&2
    exit 1
}
grep -Fq 'Production serving live TLS 1.3 client phase failed' \
    scripts/test-qemu-production-service-https.sh || {
    echo "production serving proof не делает TLS 1.3 client phase fatal" >&2
    exit 1
}
if grep -Fq 'Production serving live TLS 1.3 client: DEGRADED' \
        scripts/test-qemu-production-service-https.sh; then
    echo "production serving proof всё ещё DEGRADED для live TLS 1.3 client" >&2
    exit 1
fi
grep -Fq 'scripts/verify-mbedtls-object-partition.sh' Makefile || {
    echo "Makefile не содержит verify-mbedtls-object-partition.sh" >&2
    exit 1
}
# shellcheck disable=SC2016  # the Makefile target text is matched literally
grep -Fq 'verify-mbedtls-object-partition: $(KERNEL)' Makefile || {
    echo "Makefile не связывает verify-mbedtls-object-partition с ядром" >&2
    exit 1
}
bash scripts/verify-mbedtls-object-partition.sh \
    --production build/hfoc-kernel.elf || {
    echo "mbedTLS object partition verification failed" >&2
    exit 1
}
# ITEM-386: the test-tls-qemu guard is re-pointed.  The pre-ITEM-386 blanket
# prohibition on `mbedtls_ssl_*`, `psa_crypto_*` and `mbedtls_psa_*` in the
# production kernel asserted the old production-isolation rule; the ordinary
# boot now terminates TLS 1.3 itself, so the Makefile asserts the serving
# closure and the ITEM-372 verification namespace positively and keeps the
# fixture-only surface forbidden.  These pins keep the re-pointing from
# regressing to the stale ban or to a bare deletion of the guard.
for tls_guard_contract in \
    'mbedtls_ssl_tls13_crypto_init' \
    'psa_crypto_init' \
    'hfoc_verify_mbedtls_asn1_get_len' \
    'FAIL: production kernel missing TLS-serving symbol' \
    'FAIL: production kernel missing ITEM-372 verification symbol' \
    'FAIL: production kernel contains fixture-only symbol'; do
    grep -Fq "$tls_guard_contract" Makefile || {
        echo "Makefile не содержит TLS-serving контракт: $tls_guard_contract" >&2
        exit 1
    }
done
if grep -Fq "' (mbedtls_ssl_|psa_crypto_|mbedtls_psa_)'" Makefile; then
    echo "Makefile всё ещё содержит stale mbedTLS SSL/PSA prohibition" >&2
    exit 1
fi
grep -Fq 'hfoc_production_end_to_end_acceptance_report_version=2' \
    scripts/test-production-end-to-end-acceptance.sh || {
    echo "test-production-end-to-end-acceptance.sh не публикует report version 2" >&2
    exit 1
}
grep -Fq 'verify-production-end-to-end-acceptance-report.sh' \
    scripts/test-production-end-to-end-acceptance.sh || {
    echo "runner не проверяет собственный report" >&2
    exit 1
}
grep -Fq 'stage_count=25' \
    release/hfoc-production-end-to-end-acceptance.txt || {
    echo "production end-to-end acceptance matrix не содержит 25 stages" >&2
    exit 1
}
grep -Fq 'stage_24_state=NOT_TESTED' \
    release/hfoc-production-end-to-end-acceptance.txt || {
    echo "production acceptance matrix не оставляет hardware NOT_TESTED" >&2
    exit 1
}
grep -Fq '**Status:** NOT TESTED' \
    release/hfoc-hardware-acceptance-report.md || {
    echo "hardware acceptance report не содержит NOT TESTED" >&2
    exit 1
}
bash scripts/verify-production-end-to-end-acceptance.sh \
    release/hfoc-production-end-to-end-acceptance.txt >/dev/null
bash tests/test-production-end-to-end-acceptance-host.sh
for production_acceptance_doc in README.md docs/ARCHITECTURE.md docs/ROADMAP.md; do
    if ! grep -Eiq 'production end-to-end acceptance' \
            "$production_acceptance_doc"; then
        echo "$production_acceptance_doc не документирует production end-to-end acceptance" >&2
        exit 1
    fi
done
