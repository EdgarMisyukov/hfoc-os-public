#!/usr/bin/env bash
# ITEM-387: contracted slice of the HFOC OS check suite. Sourced in pinned
#           order by scripts/check.sh; lint it on its own with shellcheck.

# Virtio-net frame QEMU proof contracts.
grep -Fq 'test-virtio-net-frame-qemu' Makefile || {
    echo "Makefile does not contain test-virtio-net-frame-qemu" >&2
    exit 1
}
# shellcheck disable=SC2016
grep -Fq '$(MAKE) test-virtio-net-frame-qemu' Makefile || {
    echo "test: target does not contain test-virtio-net-frame-qemu" >&2
    exit 1
}
grep -Fq 'scripts/test-qemu-virtio-net-frame.sh' Makefile || {
    echo "Makefile does not contain virtio-net-frame QEMU script" >&2
    exit 1
}
grep -Fq '[VIRTIO-NET-FRAME] proof init' \
    kernel/src/virtio_net_frame_qemu.c || {
    echo "virtio_net_frame_qemu.c does not contain proof init marker" >&2
    exit 1
}
grep -Fq 'virtio_net_frame_qemu_test_init' kernel/src/kernel.c || {
    echo "kernel.c does not call virtio-net-frame proof" >&2
    exit 1
}
if bash scripts/hfoc-file-marker.sh build/hfoc-kernel.elf '[VIRTIO-NET-FRAME]'; then
    echo "Production kernel contains virtio-net-frame marker" >&2
    exit 1
fi
if nm -g build/hfoc-kernel.elf |
    grep -F ' virtio_net_frame_qemu_test_init'; then
    echo "Production kernel contains virtio-net-frame proof symbol" >&2
    exit 1
fi
if grep -Fq 'qemu-virtio-net-frame' .github/workflows/ci.yml; then
    :
else
    echo "CI pgrep guard does not contain virtio-net-frame script" >&2
    exit 1
fi
grep -Fq 'QEMU virtio-net-frame baseline: machine=q35 smp=1 cpu=default' \
    scripts/test-qemu-virtio-net-frame.sh || {
    echo "test-qemu-virtio-net-frame.sh does not fix portable baseline" >&2
    exit 1
}
grep -Fq 'QEMU virtio-net frame test: OK' \
    scripts/test-qemu-virtio-net-frame.sh || {
    echo "test-qemu-virtio-net-frame.sh does not contain success marker" >&2
    exit 1
}
for virtio_net_frame_marker in \
    '[VIRTIO-NET-FRAME] proof init' \
    '[VIRTIO-NET-FRAME] device live' \
    '[VIRTIO-NET-FRAME] rx ready' \
    '[VIRTIO-NET-FRAME] rx ok' \
    '[VIRTIO-NET-FRAME] tx ready' \
    '[VIRTIO-NET-FRAME] tx ok' \
    '[VIRTIO-NET-FRAME] reset shutdown' \
    '[VIRTIO-NET-FRAME] balanced ownership' \
    '[VIRTIO-NET-FRAME] clean shutdown' \
    '[VIRTIO-NET-FRAME] proof OK'; do
    grep -Fq "$virtio_net_frame_marker" \
        kernel/src/virtio_net_frame_qemu.c || {
        echo "virtio_net_frame_qemu.c lacks $virtio_net_frame_marker" >&2
        exit 1
    }
    grep -Fq "$virtio_net_frame_marker" \
        scripts/test-qemu-virtio-net-frame.sh || {
        echo "virtio-net-frame script lacks $virtio_net_frame_marker" >&2
        exit 1
    }
done
for virtio_net_frame_doc in README.md docs/ARCHITECTURE.md docs/ROADMAP.md; do
    if ! grep -Eiq 'virtio-net frame qemu proof|virtio-net-frame-qemu' \
            "$virtio_net_frame_doc"; then
        echo "$virtio_net_frame_doc does not document virtio-net-frame proof" >&2
        exit 1
    fi
done
grep -Fq -- '- [x] **220. Virtio-net frame QEMU proof' \
    docs/superpowers/plans/2026-08-22-https-vertical-slice-items-201-300.md || {
    echo "HTTPS queue plan does not mark item 220 completed" >&2
    exit 1
}

# Packet-boundary QEMU proof contracts.
grep -Fq 'test-packet-boundary-qemu' Makefile || {
    echo "Makefile does not contain test-packet-boundary-qemu" >&2
    exit 1
}
# shellcheck disable=SC2016
grep -Fq '$(MAKE) test-packet-boundary-qemu' Makefile || {
    echo "test: target does not contain test-packet-boundary-qemu" >&2
    exit 1
}
grep -Fq 'scripts/test-qemu-packet-boundary.sh' Makefile || {
    echo "Makefile does not contain packet-boundary QEMU script" >&2
    exit 1
}
grep -Fq 'HFOC_PACKET_BOUNDARY_QEMU_TEST' \
    kernel/src/virtio_net_frame_qemu.c || {
    echo "virtio_net_frame_qemu.c does not contain packet-boundary gate" >&2
    exit 1
}
if bash scripts/hfoc-file-marker.sh build/hfoc-kernel.elf '[PACKET-BOUNDARY]'; then
    echo "Production kernel contains packet-boundary marker" >&2
    exit 1
fi
if nm -g build/hfoc-kernel.elf |
    grep -F ' vnf_packet_'; then
    echo "Production kernel contains packet-boundary proof symbol" >&2
    exit 1
fi
if grep -Fq 'qemu-packet-boundary' .github/workflows/ci.yml; then
    :
else
    echo "CI pgrep guard does not contain packet-boundary script" >&2
    exit 1
fi
grep -Fq 'QEMU packet-boundary baseline: machine=q35 smp=1 cpu=default' \
    scripts/test-qemu-packet-boundary.sh || {
    echo "test-qemu-packet-boundary.sh does not fix portable baseline" >&2
    exit 1
}
grep -Fq 'QEMU packet-boundary test: OK' \
    scripts/test-qemu-packet-boundary.sh || {
    echo "test-qemu-packet-boundary.sh does not contain success marker" >&2
    exit 1
}
for packet_boundary_marker in \
    '[PACKET-BOUNDARY] proof init' \
    '[PACKET-BOUNDARY] pool' \
    '[PACKET-BOUNDARY] identity/profile' \
    '[PACKET-BOUNDARY] rx ready' \
    '[PACKET-BOUNDARY] rx ok' \
    '[PACKET-BOUNDARY] tx ready' \
    '[PACKET-BOUNDARY] tx ok' \
    '[PACKET-BOUNDARY] pressure' \
    '[PACKET-BOUNDARY] reset shutdown' \
    '[PACKET-BOUNDARY] balanced pool' \
    '[PACKET-BOUNDARY] clean shutdown' \
    '[PACKET-BOUNDARY] proof OK'; do
    grep -Fq "$packet_boundary_marker" \
        kernel/src/virtio_net_frame_qemu.c || {
        echo "virtio_net_frame_qemu.c lacks $packet_boundary_marker" >&2
        exit 1
    }
    grep -Fq "$packet_boundary_marker" \
        scripts/test-qemu-packet-boundary.sh || {
        echo "packet-boundary script lacks $packet_boundary_marker" >&2
        exit 1
    }
done
for packet_boundary_doc in README.md docs/ARCHITECTURE.md docs/ROADMAP.md; do
    if ! grep -Eiq 'packet-boundary qemu proof|packet-boundary-qemu' \
            "$packet_boundary_doc"; then
        echo "$packet_boundary_doc does not document packet-boundary proof" >&2
        exit 1
    fi
done
grep -Fq -- '- [x] **230. Packet-boundary QEMU proof' \
    docs/superpowers/plans/2026-08-22-https-vertical-slice-items-201-300.md || {
    echo "HTTPS queue plan does not mark item 230 completed" >&2
    exit 1
}

# Trust-policy QEMU proof contracts.
grep -Fq 'test-trust-policy-qemu' Makefile || {
    echo "Makefile не содержит test-trust-policy-qemu" >&2
    exit 1
}
# shellcheck disable=SC2016
grep -Fq '$(MAKE) test-trust-policy-qemu' Makefile || {
    echo "test: target не содержит test-trust-policy-qemu" >&2
    exit 1
}
grep -Fq 'scripts/test-qemu-trust-policy.sh' Makefile || {
    echo "Makefile не содержит trust-policy QEMU script" >&2
    exit 1
}
grep -Fq 'HFOC_TRUST_POLICY_QEMU_TEST' \
    kernel/src/trust_policy_qemu.c || {
    echo "trust_policy_qemu.c не имеет proof guard" >&2
    exit 1
}
grep -Fq '[TRUST-POLICY] proof init' \
    kernel/src/trust_policy_qemu.c || {
    echo "trust_policy_qemu.c не содержит proof init marker" >&2
    exit 1
}
grep -Fq 'trust_policy_qemu_test_init' kernel/src/kernel.c || {
    echo "kernel.c не вызывает trust-policy proof" >&2
    exit 1
}
if bash scripts/hfoc-file-marker.sh build/hfoc-kernel.elf '[TRUST-POLICY]'; then
    echo "Production kernel содержит trust-policy marker" >&2
    exit 1
fi
if grep -Fq 'qemu-trust-policy' .github/workflows/ci.yml; then
    :
else
    echo "CI pgrep guard не содержит trust-policy script" >&2
    exit 1
fi
grep -Fq 'QEMU trust-policy baseline: machine=q35 smp=1 cpu=default' \
    scripts/test-qemu-trust-policy.sh || {
    echo "test-qemu-trust-policy.sh не фиксирует portable baseline" >&2
    exit 1
}
grep -Fq 'QEMU trust-policy test: OK' \
    scripts/test-qemu-trust-policy.sh || {
    echo "test-qemu-trust-policy.sh не содержит success marker" >&2
    exit 1
}
for trust_policy_doc in README.md docs/ARCHITECTURE.md docs/ROADMAP.md; do
    if ! grep -Eiq 'trust-policy QEMU proof|trust policy qemu|trust-policy qemu' \
            "$trust_policy_doc"; then
        echo "$trust_policy_doc не документирует trust-policy QEMU proof" >&2
        exit 1
    fi
done
grep -Fq -- '- [x] **150. Trust-policy QEMU proof' \
    docs/superpowers/plans/2026-08-17-appliance-v1-next-100-pr-queue-execution.md || {
    echo "appliance queue plan не помечает item 150 завершённым" >&2
    exit 1
}

# Trust anchor revocation contracts.
grep -Fq 'test-trust-revocation-host' Makefile || {
    echo "Makefile не содержит test-trust-revocation-host" >&2
    exit 1
}
# shellcheck disable=SC2016
grep -Fq '$(MAKE) test-trust-revocation-host' Makefile || {
    echo "test: target не содержит test-trust-revocation-host" >&2
    exit 1
}
grep -Fq 'AUDIT_RECORD_TRUST' kernel/include/audit.h || {
    echo "audit.h не содержит AUDIT_RECORD_TRUST" >&2
    exit 1
}
grep -Fq 'TRUST_REVOCATION_VERSION' \
    kernel/include/trust_revocation.h || {
    echo "trust_revocation.h не содержит TRUST_REVOCATION_VERSION" >&2
    exit 1
}
grep -Fq 'TRUST_REVOCATION_AUDIT_OPERATION_REVOKE' \
    kernel/include/trust_revocation.h || {
    echo "trust_revocation.h не содержит TRUST_REVOCATION_AUDIT_OPERATION_REVOKE" >&2
    exit 1
}
grep -Fq 'TRUST_REVOCATION_AUDIT_FAILED' \
    kernel/include/trust_revocation.h || {
    echo "trust_revocation.h не содержит TRUST_REVOCATION_AUDIT_FAILED" >&2
    exit 1
}
grep -Fq 'struct trust_revocation' \
    kernel/include/trust_revocation.h || {
    echo "trust_revocation.h не содержит struct trust_revocation" >&2
    exit 1
}
grep -Fq 'TRUST_ANCHOR_STATE_REVOKED' \
    kernel/src/verification_policy.c || {
    echo "verification_policy.c не допускает explicit revoked anchors" >&2
    exit 1
}
for revocation_symbol in trust_revocation_init \
                        trust_revocation_revoke \
                        trust_revocation_anchor_is_revoked \
                        trust_revocation_status_string; do
    nm build/hfoc-kernel.elf | grep -F " $revocation_symbol" \
        >/dev/null || {
        echo "Production kernel не содержит $revocation_symbol" >&2
        exit 1
    }
done
if grep -nE '\b(kmalloc|kfree|pmm_alloc_contiguous_pages|pmm_free_contiguous_pages)\b|\b(serial_printf|printf|fprintf|serial_write|console_write|panic)\b|\b(sleep|delay|hlt|sti|cli|pic_|lapic_|ioapic_|smp_)\b|\b(block_device_read|block_device_write)\b' \
        kernel/src/trust_revocation.c; then
    echo "trust_revocation.c выполняет запрещённое выделение, диагностику или блокировку" >&2
    exit 1
fi
if grep -nE 'trust_revocation_revoke\(' \
        kernel/src/arch/x86_64/interrupts.c; then
    echo "Trust revocation вызывается из interrupt path" >&2
    exit 1
fi
if nm -g build/hfoc-kernel.elf | grep 'trust_revocation_host_test_'; then
    echo "trust_revocation_host_test_ символы попали в kernel ELF" >&2
    exit 1
fi
if bash scripts/hfoc-file-marker.sh build/hfoc-kernel.elf \
        'Trust revocation host tests: ALL OK'; then
    echo "Production kernel содержит trust revocation host marker" >&2
    exit 1
fi
for revocation_doc in README.md docs/ARCHITECTURE.md docs/ROADMAP.md; do
    if ! grep -Eiq 'trust anchor revocation|trust-revocation' \
            "$revocation_doc"; then
        echo "$revocation_doc не документирует trust anchor revocation" >&2
        exit 1
    fi
done
grep -Fq -- '- [x] **146. Trust-anchor revocation' \
    docs/superpowers/plans/2026-08-17-appliance-v1-next-100-pr-queue-execution.md || {
    echo "appliance queue plan не помечает item 146 завершённым" >&2
    exit 1
}

# Image-policy binding contracts.
grep -Fq 'test-image-policy-host' Makefile || {
    echo "Makefile не содержит test-image-policy-host" >&2
    exit 1
}
# shellcheck disable=SC2016
grep -Fq '$(MAKE) test-image-policy-host' Makefile || {
    echo "test: target не содержит test-image-policy-host" >&2
    exit 1
}
grep -Fq 'IMAGE_POLICY_VERSION' \
    kernel/include/image_policy.h || {
    echo "image_policy.h не содержит IMAGE_POLICY_VERSION" >&2
    exit 1
}
grep -Fq 'IMAGE_POLICY_POLICY_MISMATCH' \
    kernel/include/image_policy.h || {
    echo "image_policy.h не содержит IMAGE_POLICY_POLICY_MISMATCH" >&2
    exit 1
}
grep -Fq 'struct image_policy_binding' \
    kernel/include/image_policy.h || {
    echo "image_policy.h не содержит struct image_policy_binding" >&2
    exit 1
}
for image_policy_symbol in image_policy_bind \
                          image_policy_activate \
                          image_policy_reset \
                          image_policy_is_published \
                          image_policy_manifest \
                          image_policy_status_string; do
    nm build/hfoc-kernel.elf | grep -F " $image_policy_symbol" \
        >/dev/null || {
        echo "Production kernel не содержит $image_policy_symbol" >&2
        exit 1
    }
done
if grep -nE '\b(kmalloc|kfree|pmm_alloc_contiguous_pages|pmm_free_contiguous_pages)\b|\b(serial_printf|printf|fprintf|serial_write|console_write|panic)\b|\b(sleep|delay|hlt|sti|cli|pic_|lapic_|ioapic_|smp_)\b|\b(block_device_read|block_device_write)\b|\b(event_ring_|audit_)\b' \
        kernel/src/image_policy.c; then
    echo "image_policy.c выполняет запрещённое выделение, диагностику, аудит или блокирование" >&2
    exit 1
fi
if grep -nE 'image_policy_bind\(' \
        kernel/src/arch/x86_64/interrupts.c; then
    echo "Image policy binding вызывается из interrupt path" >&2
    exit 1
fi
if nm -g build/hfoc-kernel.elf | grep 'image_policy_host_test_'; then
    echo "image_policy_host_test_ символы попали в kernel ELF" >&2
    exit 1
fi
if bash scripts/hfoc-file-marker.sh build/hfoc-kernel.elf \
        'image policy host tests: OK'; then
    echo "Production kernel содержит image policy host marker" >&2
    exit 1
fi
for image_policy_doc in README.md docs/ARCHITECTURE.md docs/ROADMAP.md; do
    if ! grep -Eiq 'image policy binding|image-policy' \
            "$image_policy_doc"; then
        echo "$image_policy_doc не документирует image policy binding" >&2
        exit 1
    fi
done
grep -Fq -- '- [x] **147. Image-policy binding' \
    docs/superpowers/plans/2026-08-17-appliance-v1-next-100-pr-queue-execution.md || {
    echo "appliance queue plan не помечает item 147 завершённым" >&2
    exit 1
}

# Measured boot evidence record contracts.
grep -Fq 'test-boot-evidence-host' Makefile || {
    echo "Makefile не содержит test-boot-evidence-host" >&2
    exit 1
}
# shellcheck disable=SC2016
grep -Fq '$(MAKE) test-boot-evidence-host' Makefile || {
    echo "test: target не содержит test-boot-evidence-host" >&2
    exit 1
}
grep -Fq 'BOOT_EVIDENCE_VERSION' \
    kernel/include/boot_evidence.h || {
    echo "boot_evidence.h не содержит BOOT_EVIDENCE_VERSION" >&2
    exit 1
}
grep -Fq 'BOOT_EVIDENCE_DIGEST_MISMATCH' \
    kernel/include/boot_evidence.h || {
    echo "boot_evidence.h не содержит BOOT_EVIDENCE_DIGEST_MISMATCH" >&2
    exit 1
}
grep -Fq 'struct boot_evidence' \
    kernel/include/boot_evidence.h || {
    echo "boot_evidence.h не содержит struct boot_evidence" >&2
    exit 1
}
grep -Fq 'BOOT_EVIDENCE_RECORD_SIZE' \
    kernel/include/boot_evidence.h || {
    echo "boot_evidence.h не содержит BOOT_EVIDENCE_RECORD_SIZE" >&2
    exit 1
}
for boot_evidence_symbol in boot_evidence_init \
                            boot_evidence_save \
                            boot_evidence_load \
                            boot_evidence_check_policy \
                            boot_evidence_is_published \
                            boot_evidence_status_string; do
    nm build/hfoc-kernel.elf | grep -F " $boot_evidence_symbol" \
        >/dev/null || {
        echo "Production kernel не содержит $boot_evidence_symbol" >&2
        exit 1
    }
done
if grep -nE '\b(kmalloc|kfree|pmm_alloc_contiguous_pages|pmm_free_contiguous_pages)\b|\b(serial_printf|printf|fprintf|serial_write|console_write|panic)\b|\b(sleep|delay|hlt|sti|cli|pic_|lapic_|ioapic_|smp_)\b|\b(block_device_read|block_device_write)\b|\b(event_ring_|audit_)\b' \
        kernel/src/boot_evidence.c; then
    echo "boot_evidence.c выполняет запрещённое выделение, диагностику, аудит или блокирование" >&2
    exit 1
fi
if grep -nE 'boot_evidence_init\(' \
        kernel/src/arch/x86_64/interrupts.c; then
    echo "Boot evidence init вызывается из interrupt path" >&2
    exit 1
fi
if nm -g build/hfoc-kernel.elf | grep 'boot_evidence_host_test_'; then
    echo "boot_evidence_host_test_ символы попали в kernel ELF" >&2
    exit 1
fi
if bash scripts/hfoc-file-marker.sh build/hfoc-kernel.elf \
        'boot evidence host tests: OK'; then
    echo "Production kernel содержит boot evidence host marker" >&2
    exit 1
fi
for boot_evidence_doc in README.md docs/ARCHITECTURE.md docs/ROADMAP.md; do
    if ! grep -Eiq 'measured boot evidence|boot evidence|boot-evidence' \
            "$boot_evidence_doc"; then
        echo "$boot_evidence_doc не документирует measured boot evidence" >&2
        exit 1
    fi
done
grep -Fq -- '- [x] **148. Measured boot evidence record' \
    docs/superpowers/plans/2026-08-17-appliance-v1-next-100-pr-queue-execution.md || {
    echo "appliance queue plan не помечает item 148 завершённым" >&2
    exit 1
}

# Recovery trust mode contracts.
grep -Fq 'test-recovery-trust-host' Makefile || {
    echo "Makefile не содержит test-recovery-trust-host" >&2
    exit 1
}
# shellcheck disable=SC2016
grep -Fq '$(MAKE) test-recovery-trust-host' Makefile || {
    echo "test: target не содержит test-recovery-trust-host" >&2
    exit 1
}
grep -Fq 'RECOVERY_TRUST_VERSION' \
    kernel/include/recovery_trust.h || {
    echo "recovery_trust.h не содержит RECOVERY_TRUST_VERSION" >&2
    exit 1
}
grep -Fq 'RECOVERY_TRUST_MODE_DENIED' \
    kernel/include/recovery_trust.h || {
    echo "recovery_trust.h не содержит RECOVERY_TRUST_MODE_DENIED" >&2
    exit 1
}
grep -Fq 'struct recovery_trust_state' \
    kernel/include/recovery_trust.h || {
    echo "recovery_trust.h не содержит struct recovery_trust_state" >&2
    exit 1
}
for recovery_trust_symbol in recovery_trust_init \
                             recovery_trust_enter \
                             recovery_trust_authorize \
                             recovery_trust_exit \
                             recovery_trust_is_recovery \
                             recovery_trust_status_string; do
    nm build/hfoc-kernel.elf | grep -F " $recovery_trust_symbol" \
        >/dev/null || {
        echo "Production kernel не содержит $recovery_trust_symbol" >&2
        exit 1
    }
done
if grep -nE '\b(kmalloc|kfree|pmm_alloc_contiguous_pages|pmm_free_contiguous_pages)\b|\b(serial_printf|printf|fprintf|serial_write|console_write|panic)\b|\b(sleep|delay|hlt|sti|cli|pic_|lapic_|ioapic_|smp_)\b|\b(block_device_read|block_device_write)\b|\b(event_ring_|audit_)\b' \
        kernel/src/recovery_trust.c; then
    echo "recovery_trust.c выполняет запрещённое выделение, диагностику, аудит или блокирование" >&2
    exit 1
fi
if grep -nE 'recovery_trust_enter\(' \
        kernel/src/arch/x86_64/interrupts.c; then
    echo "Recovery trust enter вызывается из interrupt path" >&2
    exit 1
fi
if nm -g build/hfoc-kernel.elf | grep 'recovery_trust_host_test_'; then
    echo "recovery_trust_host_test_ символы попали в kernel ELF" >&2
    exit 1
fi
if bash scripts/hfoc-file-marker.sh build/hfoc-kernel.elf \
        'recovery trust host tests: OK'; then
    echo "Production kernel содержит recovery trust host marker" >&2
    exit 1
fi
for recovery_trust_doc in README.md docs/ARCHITECTURE.md docs/ROADMAP.md; do
    if ! grep -Eiq 'recovery trust mode|recovery-trust' \
            "$recovery_trust_doc"; then
        echo "$recovery_trust_doc не документирует recovery trust mode" >&2
        exit 1
    fi
done
grep -Fq -- '- [x] **149. Recovery trust mode' \
    docs/superpowers/plans/2026-08-17-appliance-v1-next-100-pr-queue-execution.md || {
    echo "appliance queue plan не помечает item 149 завершённым" >&2
    exit 1
}

# Update compatibility manifest contracts.
grep -Fq 'test-update-compatibility-host' Makefile || {
    echo "Makefile не содержит test-update-compatibility-host" >&2
    exit 1
}
# shellcheck disable=SC2016
grep -Fq '$(MAKE) test-update-compatibility-host' Makefile || {
    echo "test: target не содержит test-update-compatibility-host" >&2
    exit 1
}
grep -Fq 'UPDATE_COMPATIBILITY_VERSION' \
    kernel/include/update_compatibility.h || {
    echo "update_compatibility.h не содержит UPDATE_COMPATIBILITY_VERSION" >&2
    exit 1
}
grep -Fq 'struct update_compatibility_manifest' \
    kernel/include/update_compatibility.h || {
    echo "update_compatibility.h не содержит struct update_compatibility_manifest" >&2
    exit 1
}
grep -Fq 'struct update_compatibility_gate' \
    kernel/include/update_compatibility.h || {
    echo "update_compatibility.h не содержит struct update_compatibility_gate" >&2
    exit 1
}
grep -Fq 'update_compatibility_stage' \
    kernel/include/update_compatibility.h || {
    echo "update_compatibility.h не содержит update_compatibility_stage" >&2
    exit 1
}
for update_compatibility_symbol in update_compatibility_manifest_parse \
                                   update_compatibility_manifest_validate \
                                   update_compatibility_check \
                                   update_compatibility_gate_init \
                                   update_compatibility_stage \
                                   update_compatibility_status_string; do
    nm build/hfoc-kernel.elf | grep -F " $update_compatibility_symbol" \
        >/dev/null || {
        echo "Production kernel не содержит $update_compatibility_symbol" >&2
        exit 1
    }
done
if grep -nE '\b(kmalloc|kfree|pmm_alloc_contiguous_pages|pmm_free_contiguous_pages)\b|\b(serial_printf|printf|fprintf|serial_write|console_write|panic)\b|\b(sleep|delay|hlt|sti|cli|pic_|lapic_|ioapic_|smp_)\b|\b(block_device_read|block_device_write)\b|\b(event_ring_|audit_)\b' \
        kernel/src/update_compatibility.c; then
    echo "update_compatibility.c выполняет запрещённое выделение, диагностику, аудит или блокирование" >&2
    exit 1
fi
if nm -g build/hfoc-kernel.elf |
        grep 'update_compatibility_host_test_'; then
    echo "update_compatibility_host_test_ символы попали в kernel ELF" >&2
    exit 1
fi
if bash scripts/hfoc-file-marker.sh build/hfoc-kernel.elf \
        'Update compatibility host tests: ALL OK'; then
    echo "Production kernel содержит update compatibility host marker" >&2
    exit 1
fi
for update_compatibility_doc in README.md docs/ARCHITECTURE.md docs/ROADMAP.md; do
    if ! grep -Eiq 'update compatibility manifest|update-compatibility' \
            "$update_compatibility_doc"; then
        echo "$update_compatibility_doc не документирует update compatibility manifest" >&2
        exit 1
    fi
done
grep -Fq -- '- [x] **151. Update compatibility manifest' \
    docs/superpowers/plans/2026-08-17-appliance-v1-next-100-pr-queue-execution.md || {
    echo "appliance queue plan не помечает item 151 завершённым" >&2
    exit 1
}
