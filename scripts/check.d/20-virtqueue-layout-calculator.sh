#!/usr/bin/env bash
# ITEM-387: contracted slice of the HFOC OS check suite. Sourced in pinned
#           order by scripts/check.sh; lint it on its own with shellcheck.

# Virtqueue layout calculator contracts.
grep -Fq 'test-virtio-queue-layout-host' Makefile || {
    echo "Makefile не содержит test-virtio-queue-layout-host" >&2
    exit 1
}
# shellcheck disable=SC2016
grep -Fq '$(MAKE) test-virtio-queue-layout-host' Makefile || {
    echo "test-host-suite не вызывает test-virtio-queue-layout-host" >&2
    exit 1
}
grep -Fq 'VIRTIO_QUEUE_SIZE_MAX' \
    kernel/include/virtio_queue_layout.h || {
    echo "virtio_queue_layout.h не содержит VIRTIO_QUEUE_SIZE_MAX" >&2
    exit 1
}
grep -Fq 'struct virtio_queue_layout_policy' \
    kernel/include/virtio_queue_layout.h || {
    echo "virtio_queue_layout.h не содержит layout policy" >&2
    exit 1
}
grep -Fq 'struct virtio_queue_ring_layout' \
    kernel/include/virtio_queue_layout.h || {
    echo "virtio_queue_layout.h не содержит ring layout" >&2
    exit 1
}
grep -Fq 'struct virtio_queue_layout_result' \
    kernel/include/virtio_queue_layout.h || {
    echo "virtio_queue_layout.h не содержит layout result" >&2
    exit 1
}
for virtio_queue_layout_symbol in virtio_queue_layout_calculate \
                                  virtio_queue_layout_result_validate \
                                  virtio_queue_layout_result_published \
                                  virtio_queue_layout_status_string; do
    nm build/hfoc-kernel.elf | grep -F " $virtio_queue_layout_symbol" \
        >/dev/null || {
        echo "Production kernel не содержит $virtio_queue_layout_symbol" >&2
        exit 1
    }
done
if grep -nE '\b(kmalloc|kfree|pmm_alloc_contiguous_pages|pmm_free_contiguous_pages)\b|\b(serial_printf|printf|fprintf|audit_|audit_record|panic)\b|\b(sleep|delay|hlt|sti|cli|pic_|lapic_|ioapic_|smp_)\b' \
        kernel/src/virtio_queue_layout.c; then
    echo "virtio_queue_layout.c выполняет запрещённое выделение, диагностику или блокировку" >&2
    exit 1
fi
if grep -nE 'virtio_queue_layout_(calculate|result_validate|result_published)\(' \
        kernel/src/arch/x86_64/interrupts.c; then
    echo "Virtqueue layout calculator вызывается из interrupt path" >&2
    exit 1
fi
if nm -g build/hfoc-kernel.elf | grep 'virtio_queue_layout_host_test_'; then
    echo "virtio_queue_layout_host_test_ символы попали в kernel ELF" >&2
    exit 1
fi
if bash scripts/hfoc-file-marker.sh build/hfoc-kernel.elf \
        'Virtio queue layout host tests: ALL OK'; then
    echo "Production kernel содержит virtio queue layout host marker" >&2
    exit 1
fi
for virtio_queue_layout_doc in README.md docs/ARCHITECTURE.md docs/ROADMAP.md; do
    if ! grep -Eiq 'virtqueue layout|virtio queue layout|virtio-queue-layout' \
            "$virtio_queue_layout_doc"; then
        echo "$virtio_queue_layout_doc не документирует virtqueue layout" >&2
        exit 1
    fi
done

# Virtqueue descriptor ownership contracts.
grep -Fq 'test-virtio-descriptor-ownership-host' Makefile || {
    echo "Makefile не содержит test-virtio-descriptor-ownership-host" >&2
    exit 1
}
# shellcheck disable=SC2016
grep -Fq '$(MAKE) test-virtio-descriptor-ownership-host' Makefile || {
    echo "test-host-suite не вызывает test-virtio-descriptor-ownership-host" >&2
    exit 1
}
grep -Fq 'VIRTIO_DESCRIPTOR_OWNERSHIP_CAPACITY_MAX' \
    kernel/include/virtio_descriptor_ownership.h || {
    echo "virtio_descriptor_ownership.h не содержит capacity max" >&2
    exit 1
}
grep -Fq 'struct virtio_descriptor_token' \
    kernel/include/virtio_descriptor_ownership.h || {
    echo "virtio_descriptor_ownership.h не содержит descriptor token" >&2
    exit 1
}
grep -Fq 'struct virtio_descriptor_ownership' \
    kernel/include/virtio_descriptor_ownership.h || {
    echo "virtio_descriptor_ownership.h не содержит ownership pool" >&2
    exit 1
}
for virtio_descriptor_symbol in virtio_descriptor_ownership_init \
                                virtio_descriptor_ownership_reserve \
                                virtio_descriptor_ownership_publish \
                                virtio_descriptor_ownership_rollback \
                                virtio_descriptor_ownership_consume \
                                virtio_descriptor_ownership_published \
                                virtio_descriptor_ownership_free_count \
                                virtio_descriptor_ownership_reserved_count \
                                virtio_descriptor_ownership_published_count \
                                virtio_descriptor_ownership_status_string; do
    nm build/hfoc-kernel.elf | grep -F " $virtio_descriptor_symbol" \
        >/dev/null || {
        echo "Production kernel не содержит $virtio_descriptor_symbol" >&2
        exit 1
    }
done
if grep -nE '\b(kmalloc|kfree|pmm_alloc_contiguous_pages|pmm_free_contiguous_pages)\b|\b(serial_printf|printf|fprintf|audit_|audit_record|panic)\b|\b(sleep|delay|hlt|sti|cli|pic_|lapic_|ioapic_|smp_)\b' \
        kernel/src/virtio_descriptor_ownership.c; then
    echo "virtio_descriptor_ownership.c выполняет запрещённое выделение, диагностику или блокировку" >&2
    exit 1
fi
if grep -nE 'virtio_descriptor_ownership_(reserve|publish|rollback|consume)\(' \
        kernel/src/arch/x86_64/interrupts.c; then
    echo "Virtqueue descriptor ownership вызывается из interrupt path" >&2
    exit 1
fi
if nm -g build/hfoc-kernel.elf | grep 'virtio_descriptor_ownership_host_test_'; then
    echo "virtio_descriptor_ownership_host_test_ символы попали в kernel ELF" >&2
    exit 1
fi
if bash scripts/hfoc-file-marker.sh build/hfoc-kernel.elf \
        'Virtio descriptor ownership host tests: ALL OK'; then
    echo "Production kernel содержит virtio descriptor ownership host marker" >&2
    exit 1
fi
for virtio_descriptor_doc in README.md docs/ARCHITECTURE.md docs/ROADMAP.md; do
    if ! grep -Eiq 'virtqueue descriptor ownership|virtio descriptor ownership' \
            "$virtio_descriptor_doc"; then
        echo "$virtio_descriptor_doc не документирует virtqueue descriptor ownership" >&2
        exit 1
    fi
done

# Virtqueue available publication contracts.
grep -Fq 'test-virtio-available-publication-host' Makefile || {
    echo "Makefile не содержит test-virtio-available-publication-host" >&2
    exit 1
}
# shellcheck disable=SC2016
grep -Fq '$(MAKE) test-virtio-available-publication-host' Makefile || {
    echo "test-host-suite не вызывает test-virtio-available-publication-host" >&2
    exit 1
}
grep -Fq 'VIRTIO_AVAILABLE_PUBLICATION_VERSION' \
    kernel/include/virtio_available_publication.h || {
    echo "virtio_available_publication.h не содержит version" >&2
    exit 1
}
grep -Fq 'struct virtio_available_ring' \
    kernel/include/virtio_available_publication.h || {
    echo "virtio_available_publication.h не содержит available ring" >&2
    exit 1
}
grep -Fq 'struct virtio_available_barrier' \
    kernel/include/virtio_available_publication.h || {
    echo "virtio_available_publication.h не содержит barrier seam" >&2
    exit 1
}
grep -Fq 'struct virtio_available_token' \
    kernel/include/virtio_available_publication.h || {
    echo "virtio_available_publication.h не содержит publication token" >&2
    exit 1
}
grep -Fq 'struct virtio_available_publication' \
    kernel/include/virtio_available_publication.h || {
    echo "virtio_available_publication.h не содержит controller" >&2
    exit 1
}
for virtio_available_symbol in virtio_available_publication_init \
                               virtio_available_publication_prepare \
                               virtio_available_publication_publish \
                               virtio_available_publication_cancel \
                               virtio_available_publication_published \
                               virtio_available_publication_visible_index \
                               virtio_available_publication_published_count \
                               virtio_available_publication_pending_length \
                               virtio_available_publication_status_string; do
    nm build/hfoc-kernel.elf | grep -F " $virtio_available_symbol" \
        >/dev/null || {
        echo "Production kernel не содержит $virtio_available_symbol" >&2
        exit 1
    }
done
if grep -nE '\b(kmalloc|kfree|pmm_alloc_contiguous_pages|pmm_free_contiguous_pages)\b|\b(serial_printf|printf|fprintf|audit_|audit_record|panic)\b|\b(sleep|delay|hlt|sti|cli|pic_|lapic_|ioapic_|smp_)\b' \
        kernel/src/virtio_available_publication.c; then
    echo "virtio_available_publication.c выполняет запрещённое выделение, диагностику или блокировку" >&2
    exit 1
fi
if grep -nE 'virtio_available_publication_(prepare|publish|cancel)\(' \
        kernel/src/arch/x86_64/interrupts.c; then
    echo "Virtqueue available publication вызывается из interrupt path" >&2
    exit 1
fi
if nm -g build/hfoc-kernel.elf | grep 'virtio_available_publication_host_test_'; then
    echo "virtio_available_publication_host_test_ символы попали в kernel ELF" >&2
    exit 1
fi
if bash scripts/hfoc-file-marker.sh build/hfoc-kernel.elf \
        'Virtio available publication host tests: ALL OK'; then
    echo "Production kernel содержит virtio available publication host marker" >&2
    exit 1
fi
for virtio_available_doc in README.md docs/ARCHITECTURE.md docs/ROADMAP.md; do
    if ! grep -Eiq 'virtqueue available publication|virtio available publication' \
            "$virtio_available_doc"; then
        echo "$virtio_available_doc не документирует virtqueue available publication" >&2
        exit 1
    fi
done
grep -Fq -- '- [x] **207. Virtqueue available publication' \
    docs/superpowers/plans/2026-08-22-https-vertical-slice-items-201-300.md || {
    echo "HTTPS queue plan не помечает item 207 завершённым" >&2
    exit 1
}

# Virtqueue used consumption contracts.
grep -Fq 'test-virtio-used-consumption-host' Makefile || {
    echo "Makefile не содержит test-virtio-used-consumption-host" >&2
    exit 1
}
# shellcheck disable=SC2016
grep -Fq '$(MAKE) test-virtio-used-consumption-host' Makefile || {
    echo "test-host-suite не вызывает test-virtio-used-consumption-host" >&2
    exit 1
}
grep -Fq 'VIRTIO_USED_CONSUMPTION_VERSION' \
    kernel/include/virtio_used_consumption.h || {
    echo "virtio_used_consumption.h не содержит version" >&2
    exit 1
}
grep -Fq 'struct virtio_used_element' \
    kernel/include/virtio_used_consumption.h || {
    echo "virtio_used_consumption.h не содержит used element" >&2
    exit 1
}
grep -Fq 'struct virtio_used_ring' \
    kernel/include/virtio_used_consumption.h || {
    echo "virtio_used_consumption.h не содержит used ring" >&2
    exit 1
}
grep -Fq 'struct virtio_used_reclaim' \
    kernel/include/virtio_used_consumption.h || {
    echo "virtio_used_consumption.h не содержит reclaim seam" >&2
    exit 1
}
grep -Fq 'struct virtio_used_token' \
    kernel/include/virtio_used_consumption.h || {
    echo "virtio_used_consumption.h не содержит used token" >&2
    exit 1
}
grep -Fq 'struct virtio_used_consumption' \
    kernel/include/virtio_used_consumption.h || {
    echo "virtio_used_consumption.h не содержит controller" >&2
    exit 1
}
for virtio_used_symbol in virtio_used_consumption_init \
                          virtio_used_consumption_prepare \
                          virtio_used_consumption_consume \
                          virtio_used_consumption_cancel \
                          virtio_used_consumption_published \
                          virtio_used_consumption_visible_index \
                          virtio_used_consumption_consumed_count \
                          virtio_used_consumption_pending_head \
                          virtio_used_consumption_status_string; do
    nm build/hfoc-kernel.elf | grep -F " $virtio_used_symbol" \
        >/dev/null || {
        echo "Production kernel не содержит $virtio_used_symbol" >&2
        exit 1
    }
done
if grep -nE '\b(kmalloc|kfree|pmm_alloc_contiguous_pages|pmm_free_contiguous_pages)\b|\b(serial_printf|printf|fprintf|audit_|audit_record|panic)\b|\b(sleep|delay|hlt|sti|cli|pic_|lapic_|ioapic_|smp_)\b' \
        kernel/src/virtio_used_consumption.c; then
    echo "virtio_used_consumption.c выполняет запрещённое выделение, диагностику или блокировку" >&2
    exit 1
fi
if grep -nE 'virtio_used_consumption_(prepare|consume|cancel)\(' \
        kernel/src/arch/x86_64/interrupts.c; then
    echo "Virtqueue used consumption вызывается из interrupt path" >&2
    exit 1
fi
if nm -g build/hfoc-kernel.elf | grep 'virtio_used_consumption_host_test_'; then
    echo "virtio_used_consumption_host_test_ символы попали в kernel ELF" >&2
    exit 1
fi
if bash scripts/hfoc-file-marker.sh build/hfoc-kernel.elf \
        'Virtio used consumption host tests: ALL OK'; then
    echo "Production kernel содержит virtio used consumption host marker" >&2
    exit 1
fi
for virtio_used_doc in README.md docs/ARCHITECTURE.md docs/ROADMAP.md; do
    if ! grep -Eiq 'virtqueue used consumption|virtio used consumption' \
            "$virtio_used_doc"; then
        echo "$virtio_used_doc не документирует virtqueue used consumption" >&2
        exit 1
    fi
done
grep -Fq -- '- [x] **208. Virtqueue used consumption' \
    docs/superpowers/plans/2026-08-22-https-vertical-slice-items-201-300.md || {
    echo "HTTPS queue plan не помечает item 208 завершённым" >&2
    exit 1
}

# Virtio DMA region contract checks.
grep -Fq 'test-virtio-dma-region-host' Makefile || {
    echo "Makefile не содержит test-virtio-dma-region-host" >&2
    exit 1
}
# shellcheck disable=SC2016
grep -Fq '$(MAKE) test-virtio-dma-region-host' Makefile || {
    echo "test-host-suite не вызывает test-virtio-dma-region-host" >&2
    exit 1
}
grep -Fq 'VIRTIO_DMA_REGION_VERSION' \
    kernel/include/virtio_dma_region.h || {
    echo "virtio_dma_region.h не содержит version" >&2
    exit 1
}
grep -Fq 'VIRTIO_DMA_REGION_CAPACITY_MAX' \
    kernel/include/virtio_dma_region.h || {
    echo "virtio_dma_region.h не содержит capacity max" >&2
    exit 1
}
grep -Fq 'VIRTIO_DMA_REGION_FORBIDDEN_RANGE' \
    kernel/include/virtio_dma_region.h || {
    echo "virtio_dma_region.h не содержит forbidden range status" >&2
    exit 1
}
grep -Fq 'struct virtio_dma_window_policy' \
    kernel/include/virtio_dma_region.h || {
    echo "virtio_dma_region.h не содержит window policy" >&2
    exit 1
}
grep -Fq 'struct virtio_dma_region_request' \
    kernel/include/virtio_dma_region.h || {
    echo "virtio_dma_region.h не содержит region request" >&2
    exit 1
}
grep -Fq 'struct virtio_dma_region' \
    kernel/include/virtio_dma_region.h || {
    echo "virtio_dma_region.h не содержит region descriptor" >&2
    exit 1
}
grep -Fq 'struct virtio_dma_region_token' \
    kernel/include/virtio_dma_region.h || {
    echo "virtio_dma_region.h не содержит region token" >&2
    exit 1
}
grep -Fq 'struct virtio_dma_zeroize' \
    kernel/include/virtio_dma_region.h || {
    echo "virtio_dma_region.h не содержит zeroize seam" >&2
    exit 1
}
grep -Fq 'struct virtio_dma_region_registry' \
    kernel/include/virtio_dma_region.h || {
    echo "virtio_dma_region.h не содержит registry" >&2
    exit 1
}
for virtio_dma_symbol in virtio_dma_region_init \
                         virtio_dma_region_bind \
                         virtio_dma_region_unbind \
                         virtio_dma_region_teardown \
                         virtio_dma_region_published \
                         virtio_dma_region_free_count \
                         virtio_dma_region_bound_count \
                         virtio_dma_region_status_string; do
    nm build/hfoc-kernel.elf | grep -F " $virtio_dma_symbol" \
        >/dev/null || {
        echo "Production kernel не содержит $virtio_dma_symbol" >&2
        exit 1
    }
done
if grep -nE '\b(kmalloc|kfree|pmm_alloc_contiguous_pages|pmm_free_contiguous_pages)\b|\b(serial_printf|printf|fprintf|audit_|audit_record|panic)\b|\b(sleep|delay|hlt|sti|cli|pic_|lapic_|ioapic_|smp_)\b' \
        kernel/src/virtio_dma_region.c; then
    echo "virtio_dma_region.c выполняет запрещённое выделение, диагностику или блокировку" >&2
    exit 1
fi
if grep -nE 'virtio_dma_region_(bind|unbind|teardown)\(' \
        kernel/src/arch/x86_64/interrupts.c; then
    echo "Virtio DMA region contract вызывается из interrupt path" >&2
    exit 1
fi
if nm -g build/hfoc-kernel.elf | grep 'virtio_dma_region_host_test_'; then
    echo "virtio_dma_region_host_test_ символы попали в kernel ELF" >&2
    exit 1
fi
if bash scripts/hfoc-file-marker.sh build/hfoc-kernel.elf \
        'Virtio DMA region host tests: ALL OK'; then
    echo "Production kernel содержит virtio DMA region host marker" >&2
    exit 1
fi
for virtio_dma_doc in README.md docs/ARCHITECTURE.md docs/ROADMAP.md; do
    if ! grep -Eiq 'virtio DMA region|virtio-dma-region' \
            "$virtio_dma_doc"; then
        echo "$virtio_dma_doc не документирует virtio DMA region contract" >&2
        exit 1
    fi
done
grep -Fq -- '- [x] **209. Virtio DMA region contract' \
    docs/superpowers/plans/2026-08-22-https-vertical-slice-items-201-300.md || {
    echo "HTTPS queue plan не помечает item 209 завершённым" >&2
    exit 1
}

# Virtio-net feature profile contracts.
grep -Fq 'test-virtio-net-feature-profile-host' Makefile || {
    echo "Makefile не содержит test-virtio-net-feature-profile-host" >&2
    exit 1
}
# shellcheck disable=SC2016
grep -Fq '$(MAKE) test-virtio-net-feature-profile-host' Makefile || {
    echo "test-host-suite не вызывает test-virtio-net-feature-profile-host" >&2
    exit 1
}
grep -Fq 'VIRTIO_NET_FEATURE_PROFILE_VERSION' \
    kernel/include/virtio_net_feature_profile.h || {
    echo "virtio_net_feature_profile.h не содержит version" >&2
    exit 1
}
grep -Fq 'VIRTIO_NET_FORBIDDEN_FEATURE_LO_MASK' \
    kernel/include/virtio_net_feature_profile.h || {
    echo "virtio_net_feature_profile.h не содержит forbidden low mask" >&2
    exit 1
}
grep -Fq 'VIRTIO_NET_FORBIDDEN_FEATURE_HI_MASK' \
    kernel/include/virtio_net_feature_profile.h || {
    echo "virtio_net_feature_profile.h не содержит forbidden high mask" >&2
    exit 1
}
grep -Fq 'VIRTIO_NET_F_CTRL_VQ' \
    kernel/include/virtio_net_feature_profile.h || {
    echo "virtio_net_feature_profile.h не содержит control queue bit" >&2
    exit 1
}
grep -Fq 'VIRTIO_NET_F_MQ' \
    kernel/include/virtio_net_feature_profile.h || {
    echo "virtio_net_feature_profile.h не содержит multiqueue bit" >&2
    exit 1
}
grep -Fq 'struct virtio_net_configuration' \
    kernel/include/virtio_net_feature_profile.h || {
    echo "virtio_net_feature_profile.h не содержит configuration" >&2
    exit 1
}
grep -Fq 'struct virtio_net_topology' \
    kernel/include/virtio_net_feature_profile.h || {
    echo "virtio_net_feature_profile.h не содержит topology" >&2
    exit 1
}
grep -Fq 'struct virtio_net_feature_profile' \
    kernel/include/virtio_net_feature_profile.h || {
    echo "virtio_net_feature_profile.h не содержит profile" >&2
    exit 1
}
for virtio_net_profile_symbol in virtio_net_feature_profile_activate \
                                 virtio_net_feature_profile_validate_state \
                                 virtio_net_feature_profile_published \
                                 virtio_net_feature_profile_status_string; do
    nm build/hfoc-kernel.elf | grep -F " $virtio_net_profile_symbol" \
        >/dev/null || {
        echo "Production kernel не содержит $virtio_net_profile_symbol" >&2
        exit 1
    }
done
if grep -nE '\b(kmalloc|kfree|pmm_alloc_contiguous_pages|pmm_free_contiguous_pages)\b|\b(serial_printf|printf|fprintf|audit_|audit_record|panic)\b|\b(sleep|delay|hlt|sti|cli|pic_|lapic_|ioapic_|smp_)\b' \
        kernel/src/virtio_net_feature_profile.c; then
    echo "virtio_net_feature_profile.c выполняет запрещённое выделение, диагностику или блокировку" >&2
    exit 1
fi
if grep -nE 'virtio_net_feature_profile_(activate|validate_state)\(' \
        kernel/src/arch/x86_64/interrupts.c; then
    echo "Virtio-net feature profile activation вызывается из interrupt path" >&2
    exit 1
fi
if nm -g build/hfoc-kernel.elf | grep 'virtio_net_feature_profile_host_test_'; then
    echo "virtio_net_feature_profile_host_test_ символы попали в kernel ELF" >&2
    exit 1
fi
if bash scripts/hfoc-file-marker.sh build/hfoc-kernel.elf \
        'Virtio net feature profile host tests: ALL OK'; then
    echo "Production kernel содержит virtio net feature profile host marker" >&2
    exit 1
fi
for virtio_net_profile_doc in README.md docs/ARCHITECTURE.md docs/ROADMAP.md; do
    if ! grep -Eiq 'virtio[- ].*net[- ].*feature[- ].*profile' \
            "$virtio_net_profile_doc"; then
        echo "$virtio_net_profile_doc не документирует virtio net feature profile" >&2
        exit 1
    fi
done
grep -Fq -- '- [x] **211. Virtio-net feature profile' \
    docs/superpowers/plans/2026-08-22-https-vertical-slice-items-201-300.md || {
    echo "HTTPS queue plan не помечает item 211 завершённым" >&2
    exit 1
}

# Virtio-rng feature profile contracts.
grep -Fq 'test-virtio-rng-feature-profile-host' Makefile || {
    echo "Makefile не содержит test-virtio-rng-feature-profile-host" >&2
    exit 1
}
# shellcheck disable=SC2016
grep -Fq '$(MAKE) test-virtio-rng-feature-profile-host' Makefile || {
    echo "test-host-suite не вызывает test-virtio-rng-feature-profile-host" >&2
    exit 1
}
grep -Fq 'VIRTIO_RNG_FEATURE_PROFILE_VERSION' \
    kernel/include/virtio_rng_feature_profile.h || {
    echo "virtio_rng_feature_profile.h не содержит version" >&2
    exit 1
}
grep -Fq 'VIRTIO_RNG_REQUEST_QUEUE_COUNT_MAX' \
    kernel/include/virtio_rng_feature_profile.h || {
    echo "virtio_rng_feature_profile.h не содержит request queue max" >&2
    exit 1
}
grep -Fq 'VIRTIO_RNG_BUFFER_WRITER_DEVICE' \
    kernel/include/virtio_rng_feature_profile.h || {
    echo "virtio_rng_feature_profile.h не содержит device-write ownership" >&2
    exit 1
}
grep -Fq 'struct virtio_rng_topology' \
    kernel/include/virtio_rng_feature_profile.h || {
    echo "virtio_rng_feature_profile.h не содержит topology" >&2
    exit 1
}
grep -Fq 'struct virtio_rng_feature_profile' \
    kernel/include/virtio_rng_feature_profile.h || {
    echo "virtio_rng_feature_profile.h не содержит profile" >&2
    exit 1
}
for virtio_rng_profile_symbol in virtio_rng_feature_profile_activate \
                                 virtio_rng_feature_profile_validate_state \
                                 virtio_rng_feature_profile_published \
                                 virtio_rng_feature_profile_status_string; do
    nm build/hfoc-kernel.elf | grep -F " $virtio_rng_profile_symbol" \
        >/dev/null || {
        echo "Production kernel не содержит $virtio_rng_profile_symbol" >&2
        exit 1
    }
done
if grep -nE '\b(kmalloc|kfree|pmm_alloc_contiguous_pages|pmm_free_contiguous_pages)\b|\b(serial_printf|printf|fprintf|audit_|audit_record|panic)\b|\b(sleep|delay|hlt|sti|cli|pic_|lapic_|ioapic_|smp_)\b' \
        kernel/src/virtio_rng_feature_profile.c; then
    echo "virtio_rng_feature_profile.c выполняет запрещённое выделение, диагностику или блокировку" >&2
    exit 1
fi
if grep -nE 'virtio_rng_feature_profile_(activate|validate_state)\(' \
        kernel/src/arch/x86_64/interrupts.c; then
    echo "Virtio-rng feature profile activation вызывается из interrupt path" >&2
    exit 1
fi
if nm -g build/hfoc-kernel.elf | grep 'virtio_rng_feature_profile_host_test_'; then
    echo "virtio_rng_feature_profile_host_test_ символы попали в kernel ELF" >&2
    exit 1
fi
if bash scripts/hfoc-file-marker.sh build/hfoc-kernel.elf \
        'Virtio rng feature profile host tests: ALL OK'; then
    echo "Production kernel содержит virtio rng feature profile host marker" >&2
    exit 1
fi
for virtio_rng_profile_doc in README.md docs/ARCHITECTURE.md docs/ROADMAP.md; do
    if ! grep -Eiq 'virtio[- ].*rng[- ].*feature[- ].*profile' \
            "$virtio_rng_profile_doc"; then
        echo "$virtio_rng_profile_doc не документирует virtio rng feature profile" >&2
        exit 1
    fi
done
grep -Fq -- '- [x] **251. Virtio-rng feature profile' \
    docs/superpowers/plans/2026-08-22-https-vertical-slice-items-201-300.md || {
    echo "HTTPS queue plan не помечает item 251 завершённым" >&2
    exit 1
}
