#!/usr/bin/env bash
# ITEM-387: contracted slice of the HFOC OS check suite. Sourced in pinned
#           order by scripts/check.sh; lint it on its own with shellcheck.

# UEFI installer helper contracts (ITEM-361).
grep -Fq 'test-hfoc-efi-installer-host' Makefile || {
    echo "Makefile не содержит test-hfoc-efi-installer-host" >&2
    exit 1
}
# shellcheck disable=SC2016
grep -Fq '$(MAKE) test-hfoc-efi-installer-host' Makefile || {
    echo "test-host-suite не вызывает test-hfoc-efi-installer-host" >&2
    exit 1
}
grep -Fq 'test-hfoc-efi-installer-qemu' Makefile || {
    echo "Makefile не содержит test-hfoc-efi-installer-qemu" >&2
    exit 1
}
if ! awk '
    /^QEMU_PARALLEL_PHASE_1 :=/ { in_phase = 1; next }
    in_phase && /^[^[:space:]#]/ { exit }
    in_phase && /test-hfoc-efi-installer-qemu/ { found = 1 }
    END { exit found ? 0 : 1 }
' Makefile; then
    echo "QEMU_PARALLEL_PHASE_1 не содержит test-hfoc-efi-installer-qemu" >&2
    exit 1
fi
for efi_installer_file in \
    boot/efi/hfoc_efi_installer.h \
    boot/efi/hfoc_efi_installer.c \
    boot/efi/hfoc_efi_installer_uefi.c \
    boot/efi/hfoc_efi_installer_uefi.lds \
    boot/efi/uefi_min.h \
    boot/efi/limine-helper.conf \
    tests/hfoc-efi-installer-host.c \
    scripts/test-qemu-hfoc-efi-installer.sh \
    docs/hfoc-uefi-installer-helper.md; do
    test -f "$efi_installer_file" || {
        echo "Отсутствует ITEM-361 file: $efi_installer_file" >&2
        exit 1
    }
done
grep -Fq 'HFOC_EFI_INSTALLER_AMBIGUOUS_TARGET' \
    boot/efi/hfoc_efi_installer.c || {
    echo "EFI installer helper не выполняет fail-closed target selection" >&2
    exit 1
}
grep -Fq 'efi_boot_environment_set_device_path' \
    boot/efi/hfoc_efi_installer_uefi.c kernel/src/efi_boot_variables.c || {
    echo "EFI installer helper не использует real UEFI device path" >&2
    exit 1
}
grep -Fq 'chainloads the helper twice' docs/hfoc-uefi-installer-helper.md || {
    echo "ITEM-361 doc не документирует repeated OVMF proof" >&2
    exit 1
}
grep -Fq 'Physical UEFI' docs/hfoc-uefi-installer-helper.md || {
    echo "ITEM-361 doc не сохраняет physical UEFI boundary" >&2
    exit 1
}
for efi_installer_metadata_doc in README.md docs/ARCHITECTURE.md docs/ROADMAP.md; do
    grep -Eiq 'UEFI installer helper|pre-kernel UEFI installer helper' \
        "$efi_installer_metadata_doc" || {
        echo "$efi_installer_metadata_doc не документирует UEFI installer helper" >&2
        exit 1
    }
done
bash -n scripts/test-qemu-hfoc-efi-installer.sh || {
    echo "Синтаксис scripts/test-qemu-hfoc-efi-installer.sh некорректен" >&2
    exit 1
}

# Serial marker atomicity: a printed line assembled from more than one
# serial_write*() call must sit inside serial_sequence_begin()/end(). Each call
# is IRQ-safe on its own, but an IRQ-context print between two calls splits the
# marker, and the QEMU proofs then report a missing marker as a timeout
# (2026-09-11, run 34547418325).
serial_sequence_fixture_dir=$(mktemp -d "${TMPDIR:-/tmp}/hfoc-serial-seq.XXXXXX")
trap 'rm -rf "$serial_sequence_fixture_dir"' EXIT
cat >"$serial_sequence_fixture_dir/split.c" <<'EOF'
static void emit(void) {
    serial_write("[FIXTURE] value=");
    serial_write_u64(1U);
    serial_write("\n");
}
EOF
if python3 scripts/check-serial-sequences.py \
    "$serial_sequence_fixture_dir/split.c" 2>/dev/null; then
    echo "serial sequence fixture: split marker incorrectly accepted" >&2
    exit 1
fi
cat >"$serial_sequence_fixture_dir/atomic.c" <<'EOF'
static void emit(void) {
    const uint64_t serial_flags = serial_sequence_begin();
    serial_write("[FIXTURE] value=");
    serial_write_u64(1U);
    serial_write("\n");
    serial_sequence_end(serial_flags);
}
EOF
python3 scripts/check-serial-sequences.py \
    "$serial_sequence_fixture_dir/atomic.c" || {
    echo "serial sequence fixture: atomic marker rejected" >&2
    exit 1
}
rm -rf "$serial_sequence_fixture_dir"
trap - EXIT
python3 scripts/check-serial-sequences.py kernel/src/*.c || {
    echo "маркеры serial собираются без serial_sequence_begin/end" >&2
    exit 1
}

# Disk and hardware-acceptance policy (AGENTS.md 3a): /dev/nvme0n1 is never
# touched, /dev/sda is the dedicated acceptance disk, and the standing
# permission to write it is safe only while the acceptance script verifies the
# disk identity before the first write. Contract the policy and the guard.
# shellcheck disable=SC2016
for disk_policy_fragment in \
    '`/dev/sda` is the dedicated hardware-acceptance disk' \
    '`/dev/nvme0n1` is the host system disk'; do
    grep -Fq -- "$disk_policy_fragment" AGENTS.md || {
        echo "AGENTS.md не содержит disk policy: $disk_policy_fragment" >&2
        exit 1
    }
done
for acceptance_guard_fragment in \
    'HFOC_PHYSICAL_SATA_DISK=/dev/sda' \
    "HFOC_PHYSICAL_SATA_EXPECTED_SERIAL='DEADBEEF00000001'" \
    "HFOC_PHYSICAL_SATA_EXPECTED_ROOT='/dev/nvme0n1p2'" \
    'exit 90'; do
    grep -Fq -- "$acceptance_guard_fragment" \
        scripts/hfoc-physical-sata-continue.sh || {
        echo "physical acceptance script не содержит identity guard: $acceptance_guard_fragment" >&2
        exit 1
    }
done
if grep -rFq '/dev/sda and /dev/nvme0n1 must never be' \
    docs/superpowers/plans/; then
    echo "queue docs всё ещё содержат снятую disk policy" >&2
    exit 1
fi
# The permission to write the acceptance disk is exercised under execution
# rules: an audit line before the first write, a hardware-acceptance queue item,
# and a human trigger until the first complete successful hardware run.
for hardware_run_fragment in \
    'Before the first write of a hardware-acceptance run, the run appends an audit' \
    'stays human-triggered until the first complete successful hardware run on the'; do
    grep -Fq -- "$hardware_run_fragment" AGENTS.md || {
        echo "AGENTS.md не содержит правила запуска hardware acceptance: $hardware_run_fragment" >&2
        exit 1
    }
done
for hardware_queue_fragment in \
    '## Hardware acceptance execution' \
    '| 381 | Hardware acceptance on the dedicated acceptance disk |'; do
    grep -Fq -- "$hardware_queue_fragment" \
        docs/superpowers/plans/2026-09-11-appliance-productization-queue.md || {
        echo "productization queue не содержит ITEM-381 или правила запуска: $hardware_queue_fragment" >&2
        exit 1
    }
done

# Product release gate contracts (ITEM-380): SBOM, reproducible ISO digest,
# CVE runbook and a gate that fails closed when either is missing.
for release_gate_file in \
    scripts/generate-release-sbom.sh \
    scripts/verify-release-sbom.sh \
    scripts/verify-cve-runbook.sh \
    scripts/verify-product-release-gate.sh \
    tests/test-release-gate-host.sh \
    docs/hfoc-security-response-runbook.md; do
    test -f "$release_gate_file" || {
        echo "Отсутствует ITEM-380 file: $release_gate_file" >&2
        exit 1
    }
done
for release_gate_script in \
    scripts/generate-release-sbom.sh \
    scripts/verify-release-sbom.sh \
    scripts/verify-cve-runbook.sh \
    scripts/verify-product-release-gate.sh; do
    test -x "$release_gate_script" || {
        echo "${release_gate_script} не исполняемый" >&2
        exit 1
    }
    bash -n "$release_gate_script" || {
        echo "Синтаксис ${release_gate_script} некорректен" >&2
        exit 1
    }
done
grep -Fq 'test-product-release-gate-host:' Makefile || {
    echo "Makefile не содержит test-product-release-gate-host" >&2
    exit 1
}
# shellcheck disable=SC2016
grep -Fq '$(MAKE) test-product-release-gate-host' Makefile || {
    echo "test: target не содержит test-product-release-gate-host" >&2
    exit 1
}
grep -Fq 'test-product-release-gate:' Makefile || {
    echo "Makefile не содержит release-time gate test-product-release-gate" >&2
    exit 1
}
if sed -n '/^test:/,/^[^[:space:]]/p' Makefile |
        grep -Fq 'test-product-release-gate '; then
    echo "test: target не должен запускать release-time gate" >&2
    exit 1
fi
test -x scripts/normalize-iso-gpt.py || {
    echo "scripts/normalize-iso-gpt.py не исполняемый" >&2
    exit 1
}
python3 -c 'import ast,sys; ast.parse(open(sys.argv[1]).read())' \
    scripts/normalize-iso-gpt.py || {
    echo "Синтаксис normalize-iso-gpt.py некорректен" >&2
    exit 1
}
for reproducible_iso_fragment in \
    'artifact_iso=' \
    'ISO_REPRODUCIBLE_FLAGS' \
    'ISO_GPT_SEED'; do
    grep -Fq -- "$reproducible_iso_fragment" \
        scripts/capture-reproducible-image-digests.sh Makefile || {
        echo "reproducible ISO evidence не содержит ${reproducible_iso_fragment}" >&2
        exit 1
    }
done
grep -Fq 'normalize-iso-gpt.py' Makefile || {
    echo "Makefile не нормализует GPT GUID для воспроизводимого ISO" >&2
    exit 1
}
for reproducible_report_fragment in \
    'artifact_iso_first=' \
    'artifact_iso_second=' \
    'reproducibility_report_version=1'; do
    grep -Fq -- "$reproducible_report_fragment" \
        scripts/compare-reproducible-image-digests.sh || {
        echo "compare-reproducible-image-digests.sh не содержит ${reproducible_report_fragment}" >&2
            exit 1
        }
done
for release_gate_fragment in \
    'Product release gate: ACCEPT' \
    'verify-release-sbom.sh' \
    'verify-cve-runbook.sh' \
    'artifact_iso_first'; do
    grep -Fq -- "$release_gate_fragment" \
        scripts/verify-product-release-gate.sh || {
        echo "verify-product-release-gate.sh не содержит: ${release_gate_fragment}" >&2
        exit 1
    }
done
release_gate_tmp=$(mktemp -d)
bash scripts/generate-release-sbom.sh "$release_gate_tmp/sbom.spdx" >/dev/null
bash scripts/verify-release-sbom.sh "$release_gate_tmp/sbom.spdx" >/dev/null
grep -Fq 'election=Apache-2.0' "$release_gate_tmp/sbom.spdx" || {
    echo "SBOM не фиксирует mbedTLS Apache-2.0 election" >&2
    rm -rf "$release_gate_tmp"
    exit 1
}
rm -rf "$release_gate_tmp"
bash scripts/verify-cve-runbook.sh docs/hfoc-security-response-runbook.md >/dev/null
bash tests/test-release-gate-host.sh >/dev/null
for release_gate_doc in README.md docs/ARCHITECTURE.md docs/ROADMAP.md; do
    if ! grep -Eiq 'release gate|SBOM' "$release_gate_doc"; then
        echo "$release_gate_doc не документирует release gate или SBOM" >&2
        exit 1
    fi
done

# Hardware acceptance harness, matrix and report contracts (ITEM-381).
for hardware_acceptance_file in \
    scripts/hfoc-hardware-acceptance.sh \
    scripts/verify-hardware-acceptance-matrix.sh \
    scripts/verify-hardware-acceptance-report.sh \
    tests/test-hardware-acceptance-host.sh \
    release/hfoc-hardware-acceptance-matrix.txt \
    docs/hfoc-hardware-acceptance.md; do
    test -f "$hardware_acceptance_file" || {
        echo "Отсутствует ITEM-381 file: $hardware_acceptance_file" >&2
        exit 1
    }
done
test -x scripts/hfoc-hardware-acceptance.sh || {
    echo "scripts/hfoc-hardware-acceptance.sh не исполняемый" >&2
    exit 1
}
test -x scripts/verify-hardware-acceptance-matrix.sh || {
    echo "scripts/verify-hardware-acceptance-matrix.sh не исполняемый" >&2
    exit 1
}
test -x scripts/verify-hardware-acceptance-report.sh || {
    echo "scripts/verify-hardware-acceptance-report.sh не исполняемый" >&2
    exit 1
}
test -x tests/test-hardware-acceptance-host.sh || {
    echo "tests/test-hardware-acceptance-host.sh не исполняемый" >&2
    exit 1
}
for hardware_acceptance_script in \
    scripts/hfoc-hardware-acceptance.sh \
    scripts/verify-hardware-acceptance-matrix.sh \
    scripts/verify-hardware-acceptance-report.sh \
    tests/test-hardware-acceptance-host.sh; do
    bash -n "$hardware_acceptance_script" || {
        echo "Синтаксис ${hardware_acceptance_script} некорректен" >&2
        exit 1
    }
done
grep -Fq 'test-hardware-acceptance-host:' Makefile || {
    echo "Makefile не содержит test-hardware-acceptance-host" >&2
    exit 1
}
# shellcheck disable=SC2016
grep -Fq '$(MAKE) test-hardware-acceptance-host' Makefile || {
    echo "test: target не содержит test-hardware-acceptance-host" >&2
    exit 1
}
# The physical run never starts from CI or from another test: the harness only
# arms behind the human confirmation token, and CI must not reference it.
if grep -Fq 'hfoc-hardware-acceptance.sh' .github/workflows/ci.yml; then
    echo "CI не должен запускать hardware acceptance" >&2
    exit 1
fi
if sed -n '/^test:/,/^[^[:space:]]/p' Makefile |
        grep -Fq 'hfoc-hardware-acceptance.sh'; then
    echo "test: target не должен запускать физическую приёмку" >&2
    exit 1
fi
for hardware_acceptance_fragment in \
    'HFOC_HARDWARE_ACCEPTANCE_CONFIRM' \
    'hardware acceptance never arms in CI' \
    'audit initiator=' \
    'image_sha256=' \
    'exit 92'; do
    grep -Fq -- "$hardware_acceptance_fragment" \
        scripts/hfoc-hardware-acceptance.sh || {
        echo "hardware acceptance harness не содержит: $hardware_acceptance_fragment" >&2
        exit 1
    }
done
bash scripts/verify-hardware-acceptance-matrix.sh \
    release/hfoc-hardware-acceptance-matrix.txt >/dev/null
bash tests/test-hardware-acceptance-host.sh >/dev/null
grep -Fq '**Status:** NOT TESTED' release/hfoc-hardware-acceptance-report.md || {
    echo "published hardware report не содержит NOT TESTED" >&2
    exit 1
}
if grep -Eq '^\| [A-Za-z0-9 /_-]+ \| PASS \|' \
        release/hfoc-hardware-acceptance-report.md; then
    echo "published hardware report фабрикует PASS" >&2
    exit 1
fi
for hardware_acceptance_doc in README.md docs/ARCHITECTURE.md docs/ROADMAP.md; do
    if ! grep -Eiq 'hardware acceptance' "$hardware_acceptance_doc"; then
        echo "$hardware_acceptance_doc не документирует hardware acceptance" >&2
        exit 1
    fi
done

# Signed media and installer hardening contracts (ITEM-371).
for signed_media_file in \
    kernel/include/installer_media_verification.h \
    kernel/src/installer_media_verification.c \
    kernel/include/installer_supported_hardware.h \
    kernel/src/installer_supported_hardware.c \
    kernel/include/installer_uninstall.h \
    kernel/src/installer_uninstall.c \
    tests/installer-hardening-host.c \
    docs/hfoc-installer-media-hardening.md; do
    test -f "$signed_media_file" || {
        echo "Отсутствует ITEM-371 file: $signed_media_file" >&2
        exit 1
    }
done
grep -Fq 'test-installer-hardening-host' Makefile || {
    echo "Makefile не содержит test-installer-hardening-host" >&2
    exit 1
}
# shellcheck disable=SC2016
grep -Fq '$(MAKE) test-installer-hardening-host' Makefile || {
    echo "test-host-suite не вызывает test-installer-hardening-host" >&2
    exit 1
}
grep -Fq 'installer_media_verification_authorizes_write' \
    kernel/src/installer_media_verification.c || {
    echo "media gate не публикует write_authorized" >&2
    exit 1
}
grep -Fq 'INSTALLER_MEDIA_VERIFIER_UNAVAILABLE' \
    kernel/include/installer_media_verification.h || {
    echo "media gate не фиксирует fail-closed verifier boundary" >&2
    exit 1
}
grep -Fq 'installer_media_source_digest' \
    kernel/src/installer_media_verification.c || {
    echo "media gate не связывает подписанный digest с устанавливаемыми байтами" >&2
    exit 1
}
if [[ $(grep -c 'installer->media_verified' \
        kernel/src/production_installer.c) -lt 2 ]]; then
    echo "installer не перепроверяет media gate на первой записи" >&2
    exit 1
fi
if [[ $(grep -c 'installer->hardware_supported' \
        kernel/src/production_installer.c) -lt 2 ]]; then
    echo "installer не перепроверяет hardware gate на первой записи" >&2
    exit 1
fi
grep -Fq 'INSTALLER_HARDWARE_REASON_CONTROLLER_UNSUPPORTED' \
    kernel/src/installer_supported_hardware.c || {
    echo "supported-hardware gate не перечисляет причины отказа" >&2
    exit 1
}
for installer_hardening_status in \
    PRODUCTION_INSTALLER_MEDIA_UNVERIFIED \
    PRODUCTION_INSTALLER_UNSUPPORTED_HARDWARE; do
    grep -Fq "$installer_hardening_status" kernel/src/production_installer.c || {
        echo "production installer не применяет gate: $installer_hardening_status" >&2
        exit 1
    }
done
grep -Fq 'installer_uninstall_verify_outcome' \
    kernel/src/installer_uninstall.c || {
    echo "uninstall contract не проверяет outcome" >&2
    exit 1
}
grep -Fq 'unverified media no-write proof OK' \
    kernel/src/production_installer_qemu.c || {
    echo "QEMU proof не доказывает отказ до первой записи" >&2
    exit 1
}
grep -Fq 'uninstall previous-OS restoration proof OK' \
    kernel/src/production_installer_qemu.c || {
    echo "QEMU proof не доказывает возврат к предыдущей ОС" >&2
    exit 1
}
grep -Fq 'stage_15_state=DEGRADED' \
    release/hfoc-production-end-to-end-acceptance.txt || {
    echo "SIGNED_MEDIA stage не помечен DEGRADED" >&2
    exit 1
}
grep -Fq 'stage_15_target=test-installer-hardening-host' \
    release/hfoc-production-end-to-end-acceptance.txt || {
    echo "SIGNED_MEDIA stage не называет focused proof" >&2
    exit 1
}
for signed_media_doc_fragment in \
    'test-installer-hardening-host' \
    'Installer media and hardening host tests: ALL OK' \
    'fails closed'; do
    grep -Fq -- "$signed_media_doc_fragment" \
        docs/hfoc-installer-media-hardening.md || {
        echo "ITEM-371 doc не содержит: $signed_media_doc_fragment" >&2
        exit 1
    }
done
for signed_media_metadata_doc in README.md docs/ARCHITECTURE.md docs/ROADMAP.md; do
    grep -Eiq 'signed media|installer hardening|installer-media-hardening' \
        "$signed_media_metadata_doc" || {
        echo "$signed_media_metadata_doc не документирует signed media hardening" >&2
        exit 1
    }
done
bash -n scripts/test-qemu-production-installer.sh || {
    echo "Синтаксис scripts/test-qemu-production-installer.sh некорректен" >&2
    exit 1
}

# Signed boot chain and boot attestation contracts (ITEM-372).
for boot_chain_file in \
    kernel/include/image_signature.h \
    kernel/src/image_signature.c \
    kernel/src/image_signature_ecdsa.c \
    kernel/include/hfoc_verify_config.h \
    kernel/include/hfoc_verify_alloc.h \
    kernel/include/boot_chain.h \
    kernel/src/boot_chain.c \
    kernel/include/boot_measurements.h \
    kernel/src/boot_measurements.c \
    kernel/include/boot_attestation.h \
    kernel/src/boot_attestation.c \
    kernel/include/boot_chain_boot.h \
    kernel/src/boot_chain_boot.c \
    kernel/src/boot_chain_qemu.c \
    tests/image-signature-host.c \
    tests/boot-chain-host.c \
    tests/boot-chain-vector.c \
    tests/boot-attestation-host.c \
    tests/boot-measurements-host.c \
    scripts/hfoc-boot-chain-manifest.py \
    boot/keys/hfoc-development-signing-key.pem \
    docs/hfoc-signed-boot-chain.md; do
    test -f "$boot_chain_file" || {
        echo "Отсутствует ITEM-372 file: $boot_chain_file" >&2
        exit 1
    }
done
for boot_chain_target in \
    test-image-signature-host \
    test-boot-measurements-host \
    test-boot-chain-host \
    test-boot-attestation-host; do
    grep -Fq "$boot_chain_target" Makefile || {
        echo "Makefile не содержит $boot_chain_target" >&2
        exit 1
    }
    # shellcheck disable=SC2016
    grep -Fq "\$(MAKE) $boot_chain_target" Makefile || {
        echo "test-host-suite не вызывает $boot_chain_target" >&2
        exit 1
    }
done
grep -Fq 'test-production-boot-chain-qemu' Makefile || {
    echo "Makefile не содержит test-production-boot-chain-qemu" >&2
    exit 1
}
grep -Fq 'scripts/test-qemu-boot-chain.sh' Makefile || {
    echo "Makefile не вызывает scripts/test-qemu-boot-chain.sh" >&2
    exit 1
}
awk '/^QEMU_PARALLEL_PHASE_1 :=/,/^$/' Makefile | \
    grep -Fq 'test-production-boot-chain-qemu' || {
    echo "QEMU_PARALLEL_PHASE_1 не содержит test-production-boot-chain-qemu" >&2
    exit 1
}
# shellcheck disable=SC2016
awk '/^QEMU_TEST_IMAGES :=/,/^$/' Makefile | \
    grep -Fq '$(BOOT_CHAIN_ISO)' || {
    echo "QEMU_TEST_IMAGES не содержит BOOT_CHAIN_ISO" >&2
    exit 1
}
grep -Fq 'image_signature_ecdsa_p256_verify' \
    kernel/src/image_signature.c || {
    echo "verifier не публикует fail-closed backend boundary" >&2
    exit 1
}
grep -Fq 'IMAGE_SIGNATURE_UNAVAILABLE' kernel/src/image_signature.c || {
    echo "verifier не фиксирует UNAVAILABLE по умолчанию" >&2
    exit 1
}
grep -Fq 'HFOC_RELEASE_BUILD' kernel/src/image_signature.c || {
    echo "verifier не разделяет development и release anchors" >&2
    exit 1
}
grep -Fq 'boot_chain_report_is_trusted' kernel/src/boot_chain.c || {
    echo "boot chain не публикует trusted report" >&2
    exit 1
}
grep -Fq 'BOOT_CHAIN_COMPONENT_DIGEST_MISMATCH' \
    kernel/src/boot_chain.c || {
    echo "boot chain не проверяет digest компонентов" >&2
    exit 1
}
grep -Fq 'boot_measurements_publish' kernel/src/boot_measurements.c || {
    echo "measured boot registers не публикуются" >&2
    exit 1
}
grep -Fq 'boot_attestation_blocks_production_boot' \
    kernel/src/boot_attestation.c || {
    echo "attestation не блокирует production boot" >&2
    exit 1
}
grep -Fq 'APPLIANCE_PRODUCTION_BOOT_UNATTESTED' \
    kernel/src/appliance_production_boot.c || {
    echo "production boot coordinator не применяет attestation gate" >&2
    exit 1
}
grep -Fq 'attestation=' kernel/src/appliance_production_management.c || {
    echo "management plane не публикует attestation" >&2
    exit 1
}
if nm build/hfoc-kernel.elf | grep -F ' image_signature_ecdsa_p256_verify' >/dev/null; then
    :
else
    echo "production kernel не содержит ECDSA backend" >&2
    exit 1
fi
if bash scripts/hfoc-file-marker.sh build/hfoc-kernel.elf '[BOOT-CHAIN] proof init'; then
    echo "Production kernel содержит boot-chain proof marker" >&2
    exit 1
fi
if bash scripts/hfoc-file-marker.sh build/hfoc-kernel.elf '[BOOT-CHAIN] proof OK'; then
    echo "Production kernel содержит boot-chain proof OK marker" >&2
    exit 1
fi
grep -Fq 'stage_16_state=PROVEN' \
    release/hfoc-production-end-to-end-acceptance.txt || {
    echo "acceptance matrix не фиксирует SIGNED_BOOT как PROVEN" >&2
    exit 1
}
grep -Fq 'stage_16_target=test-production-boot-chain-qemu' \
    release/hfoc-production-end-to-end-acceptance.txt || {
    echo "acceptance matrix не связывает SIGNED_BOOT с QEMU proof" >&2
    exit 1
}
for boot_chain_doc in \
    docs/hfoc-signed-boot-chain.md \
    docs/hfoc-production-end-to-end-acceptance.md; do
    grep -Fq 'test-production-boot-chain-qemu' "$boot_chain_doc" || {
        echo "$boot_chain_doc не документирует QEMU proof" >&2
        exit 1
    }
done
grep -Fq 'QEMU production boot chain test: OK' \
    docs/hfoc-signed-boot-chain.md || {
    echo "ITEM-372 doc не фиксирует proof marker" >&2
    exit 1
}
grep -Eiq 'signed boot chain|boot attestation|boot-chain' \
    README.md docs/ARCHITECTURE.md docs/ROADMAP.md || {
    echo "ITEM-372 не отражён в README/ARCHITECTURE/ROADMAP" >&2
    exit 1
}
bash -n scripts/test-qemu-boot-chain.sh || {
    echo "Синтаксис scripts/test-qemu-boot-chain.sh некорректен" >&2
    exit 1
}
python3 -m py_compile scripts/hfoc-boot-chain-manifest.py || {
    echo "Синтаксис scripts/hfoc-boot-chain-manifest.py некорректен" >&2
    exit 1
}
