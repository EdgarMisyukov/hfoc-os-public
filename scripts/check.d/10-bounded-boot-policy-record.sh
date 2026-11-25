#!/usr/bin/env bash
# ITEM-387: contracted slice of the HFOC OS check suite. Sourced in pinned
#           order by scripts/check.sh; lint it on its own with shellcheck.

# Bounded boot-policy record contracts.
grep -Fq 'test-boot-policy-host' Makefile || {
    echo "Makefile не содержит test-boot-policy-host" >&2; exit 1; }
# shellcheck disable=SC2016
grep -Fq '$(MAKE) test-boot-policy-host' Makefile || {
    echo "test: target не содержит test-boot-policy-host" >&2; exit 1; }
grep -Fq 'BOOT_POLICY_MODE_COUNT' kernel/include/boot_policy.h || {
    echo "boot_policy.h не содержит BOOT_POLICY_MODE_COUNT" >&2; exit 1; }
grep -Fq 'BOOT_POLICY_IMAGE_ID_MAX' kernel/include/boot_policy.h || {
    echo "boot_policy.h не содержит BOOT_POLICY_IMAGE_ID_MAX" >&2; exit 1; }
grep -Fq 'struct boot_policy_record' kernel/include/boot_policy.h || {
    echo "boot_policy.h не содержит struct boot_policy_record" >&2; exit 1; }
grep -Fq 'struct boot_policy' kernel/include/boot_policy.h || {
    echo "boot_policy.h не содержит struct boot_policy" >&2; exit 1; }
grep -Fq 'boot_policy_init' kernel/include/boot_policy.h || {
    echo "boot_policy.h не содержит boot_policy_init" >&2; exit 1; }
for boot_policy_symbol in boot_policy_init boot_policy_select \
                          boot_policy_destroy boot_policy_record \
                          boot_policy_mode_string \
                          boot_policy_status_string; do
    nm build/hfoc-kernel.elf | grep -F " $boot_policy_symbol" \
        >/dev/null || {
        echo "Production kernel не содержит $boot_policy_symbol" >&2
        exit 1
    }
done
if grep -nE '\b(kmalloc|kfree|pmm_alloc_contiguous_pages|pmm_free_contiguous_pages)\b' \
        kernel/src/boot_policy.c; then
    echo "boot_policy.c выполняет запрещённое выделение" >&2
    exit 1
fi
if nm -g build/hfoc-kernel.elf |
        grep -E 'boot_policy_host_test_|boot_policy_test_'; then
    echo "Boot policy host test symbols попали в kernel ELF" >&2
    exit 1
fi
if bash scripts/hfoc-file-marker.sh build/hfoc-kernel.elf \
        'Boot policy host tests: ALL OK'; then
    echo "Production kernel содержит boot policy host marker" >&2
    exit 1
fi
if bash scripts/hfoc-file-marker.sh build/hfoc-kernel.elf '[BOOT-POLICY]'; then
    echo "Production kernel содержит boot policy proof marker" >&2
    exit 1
fi
for boot_policy_doc in README.md docs/ARCHITECTURE.md docs/ROADMAP.md; do
    if ! grep -Eiq 'boot policy|boot-policy' "$boot_policy_doc"; then
        echo "$boot_policy_doc не документирует boot policy" >&2
        exit 1
    fi
done

# Static system-image manifest contracts.
grep -Fq 'test-system-image-host' Makefile || {
    echo "Makefile не содержит test-system-image-host" >&2; exit 1; }
# shellcheck disable=SC2016
grep -Fq '$(MAKE) test-system-image-host' Makefile || {
    echo "test: target не содержит test-system-image-host" >&2; exit 1; }
grep -Fq 'SYSTEM_IMAGE_MANIFEST_VERSION' kernel/include/system_image.h || {
    echo "system_image.h не содержит SYSTEM_IMAGE_MANIFEST_VERSION" >&2; exit 1; }
grep -Fq 'SYSTEM_IMAGE_INPUT_MAX' kernel/include/system_image.h || {
    echo "system_image.h не содержит SYSTEM_IMAGE_INPUT_MAX" >&2; exit 1; }
grep -Fq 'SYSTEM_IMAGE_COMPONENT_MAX' kernel/include/system_image.h || {
    echo "system_image.h не содержит SYSTEM_IMAGE_COMPONENT_MAX" >&2; exit 1; }
grep -Fq 'struct system_image_manifest' kernel/include/system_image.h || {
    echo "system_image.h не содержит struct system_image_manifest" >&2; exit 1; }
grep -Fq 'system_image_manifest_parse' kernel/include/system_image.h || {
    echo "system_image.h не содержит system_image_manifest_parse" >&2; exit 1; }
for system_image_symbol in system_image_manifest_parse \
                           system_image_status_string; do
    nm build/hfoc-kernel.elf | grep -F " $system_image_symbol" \
        >/dev/null || {
        echo "Production kernel не содержит $system_image_symbol" >&2
        exit 1
    }
done
if grep -nE '\b(kmalloc|kfree|pmm_alloc_contiguous_pages|pmm_free_contiguous_pages)\b' \
        kernel/src/system_image.c; then
    echo "system_image.c выполняет запрещённое выделение" >&2
    exit 1
fi
if nm -g build/hfoc-kernel.elf |
        grep -E 'system_image_host_test_|system_image_test_'; then
    echo "System image manifest host test symbols попали в kernel ELF" >&2
    exit 1
fi
if bash scripts/hfoc-file-marker.sh build/hfoc-kernel.elf \
        'System image manifest host tests: ALL OK'; then
    echo "Production kernel содержит system image manifest host marker" >&2
    exit 1
fi
if bash scripts/hfoc-file-marker.sh build/hfoc-kernel.elf '[SYSTEM-IMAGE]'; then
    echo "Production kernel содержит system image manifest proof marker" >&2
    exit 1
fi
for system_image_doc in README.md docs/ARCHITECTURE.md docs/ROADMAP.md; do
    if ! grep -Eiq 'system image manifest|system-image manifest' \
            "$system_image_doc"; then
        echo "$system_image_doc не документирует system image manifest" >&2
        exit 1
    fi
done

# SHA-256 measurement contracts.
grep -Fq 'test-sha256-host' Makefile || {
    echo "Makefile не содержит test-sha256-host" >&2; exit 1; }
# shellcheck disable=SC2016
grep -Fq '$(MAKE) test-sha256-host' Makefile || {
    echo "test: target не содержит test-sha256-host" >&2; exit 1; }
grep -Fq 'SHA256_DIGEST_SIZE' kernel/include/sha256.h || {
    echo "sha256.h не содержит SHA256_DIGEST_SIZE" >&2; exit 1; }
grep -Fq 'struct sha256' kernel/include/sha256.h || {
    echo "sha256.h не содержит struct sha256" >&2; exit 1; }
grep -Fq 'sha256_init' kernel/include/sha256.h || {
    echo "sha256.h не содержит sha256_init" >&2; exit 1; }
for sha256_symbol in sha256_init sha256_update sha256_final \
                     sha256_compute sha256_status_string; do
    nm build/hfoc-kernel.elf | grep -F " $sha256_symbol" \
        >/dev/null || {
        echo "Production kernel не содержит $sha256_symbol" >&2
        exit 1
    }
done
if grep -nE '\b(kmalloc|kfree|pmm_alloc_contiguous_pages|pmm_free_contiguous_pages)\b' \
        kernel/src/sha256.c; then
    echo "sha256.c выполняет запрещённое выделение" >&2
    exit 1
fi
if nm -g build/hfoc-kernel.elf |
        grep -E 'sha256_host_test_|sha256_test_'; then
    echo "SHA-256 host test symbols попали в kernel ELF" >&2
    exit 1
fi
if bash scripts/hfoc-file-marker.sh build/hfoc-kernel.elf \
        'SHA-256 host tests: ALL OK'; then
    echo "Production kernel содержит SHA-256 host marker" >&2
    exit 1
fi
if bash scripts/hfoc-file-marker.sh build/hfoc-kernel.elf '[SHA-256]'; then
    echo "Production kernel содержит SHA-256 proof marker" >&2
    exit 1
fi
for sha256_digest in \
        e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855 \
        ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad \
        248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1 \
        cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0; do
    if bash scripts/hfoc-file-marker.sh build/hfoc-kernel.elf "$sha256_digest"; then
        echo "Production kernel содержит SHA-256 known-answer constant" >&2
        exit 1
    fi
done
for sha256_doc in README.md docs/ARCHITECTURE.md docs/ROADMAP.md; do
    if ! grep -Eiq 'sha-256|sha256' "$sha256_doc"; then
        echo "$sha256_doc не документирует sha-256 measurement" >&2
        exit 1
    fi
done

# Pre-publication image integrity contracts.
grep -Fq 'test-image-integrity-host' Makefile || {
    echo "Makefile не содержит test-image-integrity-host" >&2; exit 1; }
# shellcheck disable=SC2016
grep -Fq '$(MAKE) test-image-integrity-host' Makefile || {
    echo "test: target не содержит test-image-integrity-host" >&2; exit 1; }
grep -Fq 'IMAGE_INTEGRITY_PAYLOAD_MAX' kernel/include/image_integrity.h || {
    echo "image_integrity.h не содержит IMAGE_INTEGRITY_PAYLOAD_MAX" >&2; exit 1; }
grep -Fq 'IMAGE_INTEGRITY_DATA_MAX' kernel/include/image_integrity.h || {
    echo "image_integrity.h не содержит IMAGE_INTEGRITY_DATA_MAX" >&2; exit 1; }
grep -Fq 'struct image_integrity_gate' kernel/include/image_integrity.h || {
    echo "image_integrity.h не содержит struct image_integrity_gate" >&2; exit 1; }
grep -Fq 'struct image_integrity_payload' kernel/include/image_integrity.h || {
    echo "image_integrity.h не содержит struct image_integrity_payload" >&2; exit 1; }
grep -Fq 'image_integrity_verify' kernel/include/image_integrity.h || {
    echo "image_integrity.h не содержит image_integrity_verify" >&2; exit 1; }
for image_integrity_symbol in image_integrity_verify \
                              image_integrity_is_published \
                              image_integrity_status_string; do
    nm build/hfoc-kernel.elf | grep -F " $image_integrity_symbol" \
        >/dev/null || {
        echo "Production kernel не содержит $image_integrity_symbol" >&2
        exit 1
    }
done
if grep -nE '\b(kmalloc|kfree|pmm_alloc_contiguous_pages|pmm_free_contiguous_pages)\b' \
        kernel/src/image_integrity.c; then
    echo "image_integrity.c выполняет запрещённое выделение" >&2
    exit 1
fi
if nm -g build/hfoc-kernel.elf |
        grep -E 'image_integrity_host_test_|image_integrity_test_'; then
    echo "Image integrity host test symbols попали в kernel ELF" >&2
    exit 1
fi
if bash scripts/hfoc-file-marker.sh build/hfoc-kernel.elf \
        'Image integrity host tests: ALL OK'; then
    echo "Production kernel содержит image integrity host marker" >&2
    exit 1
fi
if bash scripts/hfoc-file-marker.sh build/hfoc-kernel.elf '[IMAGE-INTEGRITY]'; then
    echo "Production kernel содержит image integrity proof marker" >&2
    exit 1
fi
grep -Fq 'IMAGE_INTEGRITY_HASH_MISMATCH' kernel/src/user_init_qemu.c || {
    echo "user_init_qemu.c не проверяет image integrity mismatch" >&2
    exit 1
}
grep -Fq '[IMAGE-INTEGRITY] verified initramfs before init launch' \
    kernel/src/user_init_qemu.c || {
    echo "user_init_qemu.c не публикует verified initramfs marker" >&2
    exit 1
}
grep -Fq '[IMAGE-INTEGRITY] mismatch rejected before init launch' \
    scripts/test-qemu-user-init.sh || {
    echo "QEMU user init proof не требует image integrity markers" >&2
    exit 1
}
for image_integrity_doc in README.md docs/ARCHITECTURE.md docs/ROADMAP.md; do
    if ! grep -Eiq 'image integrity|pre-publication.*image' \
            "$image_integrity_doc"; then
        echo "$image_integrity_doc не документирует image integrity" >&2
        exit 1
    fi
done

# Verified recovery-image selection contracts.
grep -Fq 'test-recovery-image-host' Makefile || {
    echo "Makefile не содержит test-recovery-image-host" >&2; exit 1; }
# shellcheck disable=SC2016
grep -Fq '$(MAKE) test-recovery-image-host' Makefile || {
    echo "test: target не содержит test-recovery-image-host" >&2; exit 1; }
grep -Fq 'RECOVERY_IMAGE_SELECTED_NORMAL' \
    kernel/include/recovery_image.h || {
    echo "recovery_image.h не содержит normal selection mode" >&2
    exit 1
}
grep -Fq 'RECOVERY_IMAGE_SELECTED_RECOVERY' \
    kernel/include/recovery_image.h || {
    echo "recovery_image.h не содержит recovery selection mode" >&2
    exit 1
}
grep -Fq 'struct recovery_image_selection' \
    kernel/include/recovery_image.h || {
    echo "recovery_image.h не содержит struct recovery_image_selection" >&2
    exit 1
}
grep -Fq 'struct recovery_image_candidate' \
    kernel/include/recovery_image.h || {
    echo "recovery_image.h не содержит struct recovery_image_candidate" >&2
    exit 1
}
grep -Fq 'recovery_image_select' kernel/include/recovery_image.h || {
    echo "recovery_image.h не содержит recovery_image_select" >&2
    exit 1
}
for recovery_image_symbol in recovery_image_select \
                             recovery_image_is_published \
                             recovery_image_manifest \
                             recovery_image_status_string; do
    nm build/hfoc-kernel.elf | grep -F " $recovery_image_symbol" \
        >/dev/null || {
        echo "Production kernel не содержит $recovery_image_symbol" >&2
        exit 1
    }
done
if grep -nE '\b(kmalloc|kfree|pmm_alloc_contiguous_pages|pmm_free_contiguous_pages)\b' \
        kernel/src/recovery_image.c; then
    echo "recovery_image.c выполняет запрещённое выделение" >&2
    exit 1
fi
if nm -g build/hfoc-kernel.elf |
        grep -E 'recovery_image_host_test_|recovery_image_test_'; then
    echo "Recovery image host test symbols попали в kernel ELF" >&2
    exit 1
fi
if bash scripts/hfoc-file-marker.sh build/hfoc-kernel.elf \
        'Recovery image host tests: ALL OK'; then
    echo "Production kernel содержит recovery image host marker" >&2
    exit 1
fi
if bash scripts/hfoc-file-marker.sh build/hfoc-kernel.elf '[RECOVERY-IMAGE]'; then
    echo "Production kernel содержит recovery image proof marker" >&2
    exit 1
fi
for recovery_image_doc in README.md docs/ARCHITECTURE.md docs/ROADMAP.md; do
    if ! grep -Eiq 'recovery image|recovery-image|verified recovery' \
            "$recovery_image_doc"; then
        echo "$recovery_image_doc не документирует recovery image selection" >&2
        exit 1
    fi
done

# Boot-policy QEMU regression harness contracts.
grep -Fq 'test-boot-policy-regression' Makefile || {
    echo "Makefile не содержит test-boot-policy-regression" >&2
    exit 1
}
# shellcheck disable=SC2016
grep -Fq '$(MAKE) test-boot-policy-regression' Makefile || {
    echo "test: target не содержит test-boot-policy-regression" >&2
    exit 1
}
grep -Fq '[BOOT-POLICY] proof init' kernel/src/boot_policy_qemu.c || {
    echo "boot_policy_qemu.c не содержит proof init marker" >&2
    exit 1
}
grep -Fq '[BOOT-POLICY] normal selected image=7' \
    kernel/src/boot_policy_qemu.c || {
    echo "boot_policy_qemu.c не содержит normal marker" >&2
    exit 1
}
grep -Fq '[BOOT-POLICY] malformed policy rejected image=0' \
    kernel/src/boot_policy_qemu.c || {
    echo "boot_policy_qemu.c не содержит malformed policy marker" >&2
    exit 1
}
grep -Fq '[BOOT-POLICY] malformed manifest selected recovery image=9' \
    kernel/src/boot_policy_qemu.c || {
    echo "boot_policy_qemu.c не содержит malformed manifest marker" >&2
    exit 1
}
grep -Fq '[BOOT-POLICY] mismatch selected recovery image=9' \
    kernel/src/boot_policy_qemu.c || {
    echo "boot_policy_qemu.c не содержит mismatch marker" >&2
    exit 1
}
grep -Fq '[BOOT-POLICY] recovery selected image=9' \
    kernel/src/boot_policy_qemu.c || {
    echo "boot_policy_qemu.c не содержит recovery marker" >&2
    exit 1
}
grep -Fq '[BOOT-POLICY] clean shutdown' \
    kernel/src/boot_policy_qemu.c || {
    echo "boot_policy_qemu.c не содержит clean shutdown marker" >&2
    exit 1
}
grep -Fq 'boot_policy_qemu_test_init();' kernel/src/kernel.c || {
    echo "kernel.c не вызывает boot_policy_qemu_test_init" >&2
    exit 1
}
for boot_policy_marker in \
    '[BOOT-POLICY] proof init' \
    '[BOOT-POLICY] normal selected image=7' \
    '[BOOT-POLICY] malformed policy rejected image=0' \
    '[BOOT-POLICY] malformed manifest selected recovery image=9' \
    '[BOOT-POLICY] mismatch selected recovery image=9' \
    '[BOOT-POLICY] recovery selected image=9' \
    '[BOOT-POLICY] clean shutdown' \
    '[BOOT-POLICY] proof OK'; do
    grep -Fq "$boot_policy_marker" \
        scripts/test-qemu-boot-policy-regression.sh || {
        echo "QEMU boot policy script не требует $boot_policy_marker" >&2
        exit 1
    }
done
grep -Fq 'QEMU boot policy regression test: OK' \
    scripts/test-qemu-boot-policy-regression.sh || {
    echo "QEMU boot policy script не содержит success marker" >&2
    exit 1
}
if nm -g build/hfoc-kernel.elf | grep 'boot_policy_qemu_test_'; then
    echo "Boot policy QEMU test symbols попали в kernel ELF" >&2
    exit 1
fi
if bash scripts/hfoc-file-marker.sh build/hfoc-kernel.elf '[BOOT-POLICY]'; then
    echo "Production kernel содержит boot policy proof marker" >&2
    exit 1
fi
grep -Fq '[t]est-qemu-boot-policy-regression' \
    .github/workflows/ci.yml || {
    echo "CI не проверяет утечку boot policy QEMU процесса" >&2
    exit 1
}
for boot_policy_doc in README.md docs/ARCHITECTURE.md docs/ROADMAP.md; do
    if ! grep -Eiq 'boot.?policy.*regression' \
            "$boot_policy_doc"; then
        echo "$boot_policy_doc не документирует boot policy regression" >&2
        exit 1
    fi
done

# Bounded block-device contracts.
grep -Fq 'test-block-device-host' Makefile || {
    echo "Makefile не содержит test-block-device-host" >&2
    exit 1
}
# shellcheck disable=SC2016
grep -Fq '$(MAKE) test-block-device-host' Makefile || {
    echo "test: target не содержит test-block-device-host" >&2
    exit 1
}
grep -Fq 'test-block-device' Makefile || {
    echo "Makefile не содержит test-block-device" >&2
    exit 1
}
# shellcheck disable=SC2016
grep -Fq '$(MAKE) test-block-device' Makefile || {
    echo "test: target не содержит test-block-device" >&2
    exit 1
}
grep -Fq 'BLOCK_DEVICE_BLOCK_SIZE_MIN' kernel/include/block_device.h || {
    echo "block_device.h не содержит BLOCK_DEVICE_BLOCK_SIZE_MIN" >&2
    exit 1
}
grep -Fq 'BLOCK_DEVICE_BLOCK_SIZE_MAX' kernel/include/block_device.h || {
    echo "block_device.h не содержит BLOCK_DEVICE_BLOCK_SIZE_MAX" >&2
    exit 1
}
grep -Fq 'BLOCK_DEVICE_BLOCK_COUNT_MAX' kernel/include/block_device.h || {
    echo "block_device.h не содержит BLOCK_DEVICE_BLOCK_COUNT_MAX" >&2
    exit 1
}
grep -Fq 'struct block_device' kernel/include/block_device.h || {
    echo "block_device.h не содержит struct block_device" >&2
    exit 1
}
for block_device_symbol in block_device_validate block_device_read \
                           block_device_write block_device_status_string; do
    nm build/hfoc-kernel.elf | grep -F " $block_device_symbol" \
        >/dev/null || {
        echo "Production kernel не содержит $block_device_symbol" >&2
        exit 1
    }
done
if grep -nE '\b(kmalloc|kfree|pmm_alloc_contiguous_pages|pmm_free_contiguous_pages)\b' \
        kernel/src/block_device.c; then
    echo "block_device.c выполняет запрещённое выделение" >&2
    exit 1
fi
if grep -nE 'block_device_(read|write)\(' \
        kernel/src/arch/x86_64/interrupts.c; then
    echo "Block-device I/O вызывается из interrupt path" >&2
    exit 1
fi
grep -Fq 'block_device_qemu_test_init();' kernel/src/kernel.c || {
    echo "kernel.c не вызывает block_device_qemu_test_init" >&2
    exit 1
}
for block_device_marker in \
    '[BLOCK-DEVICE] proof init' \
    '[BLOCK-DEVICE] alignment rejected' \
    '[BLOCK-DEVICE] range rejected' \
    '[BLOCK-DEVICE] injected read failure' \
    '[BLOCK-DEVICE] injected write failure' \
    '[BLOCK-DEVICE] clean shutdown' \
    '[BLOCK-DEVICE] proof OK'; do
    grep -Fq "$block_device_marker" \
        kernel/src/block_device_qemu.c || {
        echo "block_device_qemu.c не содержит $block_device_marker" >&2
        exit 1
    }
    grep -Fq "$block_device_marker" \
        scripts/test-qemu-block-device.sh || {
        echo "QEMU block-device script не требует $block_device_marker" >&2
        exit 1
    }
done
grep -Fq 'QEMU block-device test: OK' \
    scripts/test-qemu-block-device.sh || {
    echo "QEMU block-device script не содержит success marker" >&2
    exit 1
}
if nm -g build/hfoc-kernel.elf | grep 'block_device_qemu_test_'; then
    echo "Block-device QEMU test symbols попали в kernel ELF" >&2
    exit 1
fi
if bash scripts/hfoc-file-marker.sh build/hfoc-kernel.elf \
        'Block-device host tests: ALL OK'; then
    echo "Production kernel содержит block-device host marker" >&2
    exit 1
fi
if bash scripts/hfoc-file-marker.sh build/hfoc-kernel.elf '[BLOCK-DEVICE]'; then
    echo "Production kernel содержит block-device proof marker" >&2
    exit 1
fi
grep -Fq '[t]est-qemu-block-device' \
    .github/workflows/ci.yml || {
    echo "CI не проверяет утечку block-device QEMU процесса" >&2
    exit 1
}
for block_device_doc in README.md docs/ARCHITECTURE.md docs/ROADMAP.md; do
    if ! grep -Eiq 'block.device|block-device' \
            "$block_device_doc"; then
        echo "$block_device_doc не документирует block-device boundary" >&2
        exit 1
    fi
done

# Fixed data-region discovery contracts.
grep -Fq 'test-data-region-host' Makefile || {
    echo "Makefile не содержит test-data-region-host" >&2
    exit 1
}
# shellcheck disable=SC2016
grep -Fq '$(MAKE) test-data-region-host' Makefile || {
    echo "test: target не содержит test-data-region-host" >&2
    exit 1
}
grep -Fq 'test-data-region' Makefile || {
    echo "Makefile не содержит test-data-region" >&2
    exit 1
}
# shellcheck disable=SC2016
grep -Fq '$(MAKE) test-data-region' Makefile || {
    echo "test: target не содержит test-data-region" >&2
    exit 1
}
grep -Fq 'DATA_REGION_HEADER_BLOCK_SIZE' kernel/include/data_region.h || {
    echo "data_region.h не содержит DATA_REGION_HEADER_BLOCK_SIZE" >&2
    exit 1
}
grep -Fq 'DATA_REGION_HEADER_SIZE' kernel/include/data_region.h || {
    echo "data_region.h не содержит DATA_REGION_HEADER_SIZE" >&2
    exit 1
}
grep -Fq 'DATA_REGION_MAGIC_SIZE' kernel/include/data_region.h || {
    echo "data_region.h не содержит DATA_REGION_MAGIC_SIZE" >&2
    exit 1
}
grep -Fq 'DATA_REGION_FORMAT_VERSION' kernel/include/data_region.h || {
    echo "data_region.h не содержит DATA_REGION_FORMAT_VERSION" >&2
    exit 1
}
grep -Fq 'struct data_region' kernel/include/data_region.h || {
    echo "data_region.h не содержит struct data_region" >&2
    exit 1
}
for data_region_symbol in data_region_discover data_region_status_string; do
    nm build/hfoc-kernel.elf | grep -F " $data_region_symbol" \
        >/dev/null || {
        echo "Production kernel не содержит $data_region_symbol" >&2
        exit 1
    }
done
if grep -nE '\b(kmalloc|kfree|pmm_alloc_contiguous_pages|pmm_free_contiguous_pages)\b' \
        kernel/src/data_region.c; then
    echo "data_region.c выполняет запрещённое выделение" >&2
    exit 1
fi
if grep -nE 'data_region_discover\(' \
        kernel/src/arch/x86_64/interrupts.c; then
    echo "Data-region discovery вызывается из interrupt path" >&2
    exit 1
fi
grep -Fq 'data_region_qemu_test_init();' kernel/src/kernel.c || {
    echo "kernel.c не вызывает data_region_qemu_test_init" >&2
    exit 1
}
for data_region_marker in \
    '[DATA-REGION] proof init' \
    '[DATA-REGION] valid region published' \
    '[DATA-REGION] malformed magic rejected' \
    '[DATA-REGION] unsupported version rejected' \
    '[DATA-REGION] reserved bytes rejected' \
    '[DATA-REGION] overlapping layout rejected' \
    '[DATA-REGION] out-of-range layout rejected' \
    '[DATA-REGION] injected read failure rejected' \
    '[DATA-REGION] clean shutdown' \
    '[DATA-REGION] proof OK'; do
    grep -Fq "$data_region_marker" \
        kernel/src/data_region_qemu.c || {
        echo "data_region_qemu.c не содержит $data_region_marker" >&2
        exit 1
    }
    grep -Fq "$data_region_marker" \
        scripts/test-qemu-data-region.sh || {
        echo "QEMU data-region script не требует $data_region_marker" >&2
        exit 1
    }
done
grep -Fq 'QEMU data-region test: OK' \
    scripts/test-qemu-data-region.sh || {
    echo "QEMU data-region script не содержит success marker" >&2
    exit 1
}
if nm -g build/hfoc-kernel.elf | grep 'data_region_qemu_test_'; then
    echo "Data-region QEMU test symbols попали в kernel ELF" >&2
    exit 1
fi
if bash scripts/hfoc-file-marker.sh build/hfoc-kernel.elf \
        'Data-region host tests: ALL OK'; then
    echo "Production kernel содержит data-region host marker" >&2
    exit 1
fi
if bash scripts/hfoc-file-marker.sh build/hfoc-kernel.elf '[DATA-REGION]'; then
    echo "Production kernel содержит data-region proof marker" >&2
    exit 1
fi
grep -Fq '[t]est-qemu-data-region' \
    .github/workflows/ci.yml || {
    echo "CI не проверяет утечку data-region QEMU процесса" >&2
    exit 1
}
for data_region_doc in README.md docs/ARCHITECTURE.md docs/ROADMAP.md; do
    if ! grep -Eiq 'data.?region' \
            "$data_region_doc"; then
        echo "$data_region_doc не документирует data-region boundary" >&2
        exit 1
    fi
done
