#!/usr/bin/env bash
# ITEM-387: contracted slice of the HFOC OS check suite. Sourced in pinned
#           order by scripts/check.sh; lint it on its own with shellcheck.

# lwIP peer close and local abort policy (item 248).
grep -Fq 'test-lwip-tcp-close-policy-host' Makefile || {
    echo "Makefile не содержит test-lwip-tcp-close-policy-host" >&2;
    exit 1; }
# shellcheck disable=SC2016
grep -Fq '$(MAKE) test-lwip-tcp-close-policy-host' Makefile || {
    echo "test-host-suite не содержит test-lwip-tcp-close-policy-host" >&2;
    exit 1; }
grep -Fq 'HFOC_LWIP_TCP_CLOSE_POLICY_VERSION 1U' \
    kernel/include/hfoc_lwip_tcp_close_policy.h || {
    echo "hfoc_lwip_tcp_close_policy.h не содержит HFOC_LWIP_TCP_CLOSE_POLICY_VERSION 1U" >&2;
    exit 1; }
grep -Fq 'HFOC_LWIP_TCP_CLOSE_POLICY_REASON_SERVICE_SHUTDOWN' \
    kernel/include/hfoc_lwip_tcp_close_policy.h || {
    echo "hfoc_lwip_tcp_close_policy.h не содержит SERVICE_SHUTDOWN reason" >&2;
    exit 1; }
grep -Fq 'hfoc_lwip_tcp_close_policy_init' \
    kernel/src/hfoc_lwip_tcp_close_policy.c || {
    echo "hfoc_lwip_tcp_close_policy.c не реализует init" >&2; exit 1; }
grep -Fq 'hfoc_lwip_tcp_close_policy_attach' \
    kernel/src/hfoc_lwip_tcp_close_policy.c || {
    echo "hfoc_lwip_tcp_close_policy.c не реализует attach" >&2; exit 1; }
grep -Fq 'hfoc_lwip_tcp_close_policy_handle' \
    kernel/src/hfoc_lwip_tcp_close_policy.c || {
    echo "hfoc_lwip_tcp_close_policy.c не реализует handle" >&2; exit 1; }
grep -Fq 'hfoc_lwip_tcp_close_policy_detach' \
    kernel/src/hfoc_lwip_tcp_close_policy.c || {
    echo "hfoc_lwip_tcp_close_policy.c не реализует detach" >&2; exit 1; }
grep -Fq 'hfoc_lwip_tcp_pool_close' \
    kernel/src/hfoc_lwip_tcp_close_policy.c || {
    echo "hfoc_lwip_tcp_close_policy.c не вызывает pool close" >&2;
    exit 1; }
grep -Fq 'hfoc_lwip_tcp_pool_abort' \
    kernel/src/hfoc_lwip_tcp_close_policy.c || {
    echo "hfoc_lwip_tcp_close_policy.c не вызывает pool abort" >&2;
    exit 1; }
grep -Fq 'hfoc_lwip_tcp_quarantine_check' \
    kernel/src/hfoc_lwip_tcp_close_policy.c || {
    echo "hfoc_lwip_tcp_close_policy.c не вызывает quarantine check" >&2;
    exit 1; }
if grep -nE '\b(kmalloc|kfree|pmm_alloc_contiguous_pages|pmm_free_contiguous_pages)\b|\b(serial_printf|printf|fprintf|serial_write|console_write|panic)\b|\b(sleep|delay|hlt|sti|cli|pic_|lapic_|ioapic_|smp_)\b|\b(block_device_read|block_device_write)\b|\b(event_ring_|audit_)\b' \
        kernel/src/hfoc_lwip_tcp_close_policy.c; then
    echo "hfoc_lwip_tcp_close_policy.c выполняет запрещённое выделение, диагностику, аудит или блокирование" >&2
    exit 1
fi
if nm -g build/hfoc-kernel.elf |
    grep -E ' (hfoc_lwip_tcp_close_policy_)'; then
    echo "Production kernel содержит peer close/local abort policy symbol" >&2
    exit 1
fi
for close_policy_doc in README.md docs/ARCHITECTURE.md docs/ROADMAP.md; do
    if ! grep -Eiq 'peer close and local abort policy|close policy' \
            "$close_policy_doc"; then
        echo "$close_policy_doc не документирует peer close/local abort policy" >&2
        exit 1
    fi
done
grep -Fq -- '- [x] **248. Peer close and local abort policy' \
    docs/superpowers/plans/2026-08-22-https-vertical-slice-items-201-300.md || {
    echo "https queue plan не помечает item 248 завершённым" >&2
    exit 1
}

# lwIP listener drain and restart (item 249).
grep -Fq 'test-lwip-tcp-drain-restart-host' Makefile || {
    echo "Makefile не содержит test-lwip-tcp-drain-restart-host" >&2;
    exit 1; }
# shellcheck disable=SC2016
grep -Fq '$(MAKE) test-lwip-tcp-drain-restart-host' Makefile || {
    echo "test-host-suite не содержит test-lwip-tcp-drain-restart-host" >&2;
    exit 1; }
grep -Fq 'HFOC_LWIP_TCP_DRAIN_RESTART_VERSION 1U' \
    kernel/include/hfoc_lwip_tcp_drain_restart.h || {
    echo "hfoc_lwip_tcp_drain_restart.h не содержит HFOC_LWIP_TCP_DRAIN_RESTART_VERSION 1U" >&2;
    exit 1; }
grep -Fq 'HFOC_LWIP_TCP_DRAIN_RESTART_PHASE_COUNT 5U' \
    kernel/include/hfoc_lwip_tcp_drain_restart.h || {
    echo "hfoc_lwip_tcp_drain_restart.h не содержит PHASE_COUNT 5U" >&2;
    exit 1; }
grep -Fq 'hfoc_lwip_tcp_drain_restart_init' \
    kernel/src/hfoc_lwip_tcp_drain_restart.c || {
    echo "hfoc_lwip_tcp_drain_restart.c не реализует init" >&2; exit 1; }
grep -Fq 'hfoc_lwip_tcp_drain_restart_start' \
    kernel/src/hfoc_lwip_tcp_drain_restart.c || {
    echo "hfoc_lwip_tcp_drain_restart.c не реализует start" >&2; exit 1; }
grep -Fq 'hfoc_lwip_tcp_drain_restart_stop_admission' \
    kernel/src/hfoc_lwip_tcp_drain_restart.c || {
    echo "hfoc_lwip_tcp_drain_restart.c не реализует stop admission" >&2;
    exit 1; }
grep -Fq 'hfoc_lwip_tcp_drain_restart_step' \
    kernel/src/hfoc_lwip_tcp_drain_restart.c || {
    echo "hfoc_lwip_tcp_drain_restart.c не реализует step" >&2; exit 1; }
grep -Fq 'hfoc_lwip_tcp_drain_restart_finish_drain' \
    kernel/src/hfoc_lwip_tcp_drain_restart.c || {
    echo "hfoc_lwip_tcp_drain_restart.c не реализует finish drain" >&2;
    exit 1; }
grep -Fq 'hfoc_lwip_tcp_drain_restart_restart' \
    kernel/src/hfoc_lwip_tcp_drain_restart.c || {
    echo "hfoc_lwip_tcp_drain_restart.c не реализует restart" >&2; exit 1; }
grep -Fq 'hfoc_lwip_tcp_shutdown' \
    kernel/src/hfoc_lwip_tcp_drain_restart.c || {
    echo "hfoc_lwip_tcp_drain_restart.c не останавливает listener" >&2;
    exit 1; }
grep -Fq 'hfoc_lwip_netif_remove' \
    kernel/src/hfoc_lwip_tcp_drain_restart.c || {
    echo "hfoc_lwip_tcp_drain_restart.c не удаляет netif" >&2;
    exit 1; }
grep -Fq 'hfoc_lwip_tcp_backlog_drain' \
    kernel/src/hfoc_lwip_tcp_drain_restart.c || {
    echo "hfoc_lwip_tcp_drain_restart.c не дренирует backlog" >&2;
    exit 1; }
grep -Fq 'hfoc_lwip_tcp_deadline_check' \
    kernel/src/hfoc_lwip_tcp_drain_restart.c || {
    echo "hfoc_lwip_tcp_drain_restart.c не проверяет deadlines" >&2;
    exit 1; }
grep -Fq 'hfoc_lwip_tcp_close_policy_handle' \
    kernel/src/hfoc_lwip_tcp_drain_restart.c || {
    echo "hfoc_lwip_tcp_drain_restart.c не вызывает close policy" >&2;
    exit 1; }
grep -Fq 'hfoc_lwip_tcp_pool_release' \
    kernel/src/hfoc_lwip_tcp_drain_restart.c || {
    echo "hfoc_lwip_tcp_drain_restart.c не освобождает pool slots" >&2;
    exit 1; }
if grep -nE '\b(kmalloc|kfree|pmm_alloc_contiguous_pages|pmm_free_contiguous_pages)\b|\b(serial_printf|printf|fprintf|serial_write|console_write|panic)\b|\b(sleep|delay|hlt|sti|cli|pic_|lapic_|ioapic_|smp_)\b|\b(block_device_read|block_device_write)\b|\b(event_ring_|audit_)\b' \
        kernel/src/hfoc_lwip_tcp_drain_restart.c; then
    echo "hfoc_lwip_tcp_drain_restart.c выполняет запрещённое выделение, диагностику, аудит или блокирование" >&2
    exit 1
fi
if nm -g build/hfoc-kernel.elf |
    grep -E ' (hfoc_lwip_tcp_drain_restart_)'; then
    echo "Production kernel содержит listener drain/restart symbol" >&2
    exit 1
fi
for drain_restart_doc in README.md docs/ARCHITECTURE.md docs/ROADMAP.md; do
    if ! grep -Eiq 'listener drain and restart|drain/restart' \
            "$drain_restart_doc"; then
        echo "$drain_restart_doc не документирует listener drain and restart" >&2
        exit 1
    fi
done
grep -Fq -- '- [x] **249. Listener drain and restart' \
    docs/superpowers/plans/2026-08-22-https-vertical-slice-items-201-300.md || {
    echo "https queue plan не помечает item 249 завершённым" >&2
    exit 1
}

# Network resource QEMU stress (item 250).
grep -Fq 'test-network-resource-qemu' Makefile || {
    echo "Makefile не содержит test-network-resource-qemu" >&2; exit 1; }
# shellcheck disable=SC2016
grep -Fq '$(MAKE) test-network-resource-qemu' Makefile || {
    echo "test-serial/test не содержит test-network-resource-qemu" >&2;
    exit 1; }
grep -Fq 'scripts/test-qemu-network-resource.sh' Makefile || {
    echo "Makefile не содержит network-resource QEMU script" >&2; exit 1; }
grep -Fq 'HFOC_NETWORK_RESOURCE_QEMU_TEST' \
    kernel/src/virtio_net_frame_qemu.c || {
    echo "virtio_net_frame_qemu.c не содержит HFOC_NETWORK_RESOURCE_QEMU_TEST gate" >&2;
    exit 1; }
if bash scripts/hfoc-file-marker.sh build/hfoc-kernel.elf '[NET-RESOURCE]'; then
    echo "Production kernel содержит network-resource marker" >&2
    exit 1
fi
if nm -g build/hfoc-kernel.elf |
    grep -F ' vnf_netres_'; then
    echo "Production kernel содержит network-resource proof symbol" >&2
    exit 1
fi
if grep -Fq 'qemu-network-resource' .github/workflows/ci.yml; then
    :
else
    echo "CI pgrep guard не содержит network-resource script" >&2
    exit 1
fi
grep -Fq 'QEMU network resource baseline: machine=q35 smp=1 cpu=default' \
    scripts/test-qemu-network-resource.sh || {
    echo "test-qemu-network-resource.sh не фиксирует portable baseline" >&2
    exit 1
}
grep -Fq 'QEMU network resource test: OK' \
    scripts/test-qemu-network-resource.sh || {
    echo "test-qemu-network-resource.sh не содержит success marker" >&2
    exit 1
}
for netres_marker in \
    '[NET-RESOURCE] proof init' \
    '[NET-RESOURCE] listener live' \
    '[NET-RESOURCE] backlog pressure' \
    '[NET-RESOURCE] backlog ok' \
    '[NET-RESOURCE] pool full' \
    '[NET-RESOURCE] packet pressure ok' \
    '[NET-RESOURCE] reset ok' \
    '[NET-RESOURCE] timeout ok' \
    '[NET-RESOURCE] restart ok' \
    '[NET-RESOURCE] restarted echo ok' \
    '[NET-RESOURCE] balanced pool' \
    '[NET-RESOURCE] clean shutdown' \
    '[NET-RESOURCE] proof OK'; do
    grep -Fq "$netres_marker" \
        kernel/src/virtio_net_frame_qemu.c || {
        echo "virtio_net_frame_qemu.c lacks $netres_marker" >&2
        exit 1
    }
    grep -Fq "$netres_marker" \
        scripts/test-qemu-network-resource.sh || {
        echo "test-qemu-network-resource.sh lacks $netres_marker" >&2
        exit 1
    }
done
for netres_doc in README.md docs/ARCHITECTURE.md docs/ROADMAP.md; do
    if ! grep -Eiq 'network resource.*qemu|network-resource.*qemu|network resource qemu stress' \
            "$netres_doc"; then
        echo "$netres_doc не документирует Network resource QEMU stress" >&2
        exit 1
    fi
done
grep -Fq -- '- [x] **250. Network resource QEMU stress' \
    docs/superpowers/plans/2026-08-22-https-vertical-slice-items-201-300.md || {
    echo "https queue plan не помечает item 250 завершённым" >&2
    exit 1
}

# Cooperative scheduler yield contracts.
grep -Fq 'sched_yield(void);' kernel/include/scheduler.h || {
    echo "scheduler.h не объявляет sched_yield" >&2; exit 1; }
grep -Fq 'sched_current(struct scheduler_task **task);' \
    kernel/include/scheduler.h || {
    echo "scheduler.h не объявляет sched_current" >&2; exit 1; }
grep -Fq 'SCHEDULER_BAD_CONTEXT' kernel/include/scheduler.h || {
    echo "scheduler.h не определяет SCHEDULER_BAD_CONTEXT" >&2; exit 1; }
grep -Fq 'uintptr_t kernel_stack_top;' kernel/include/scheduler.h || {
    echo "scheduler.h не содержит kernel_stack_top" >&2; exit 1; }
grep -Fq 'sched_host_test_last_rsp0' kernel/include/scheduler.h || {
    echo "scheduler.h не содержит rsp0 host seam" >&2; exit 1; }
grep -Fq 'scheduler_set_rsp0' kernel/src/scheduler.c || {
    echo "scheduler.c не содержит scheduler_set_rsp0" >&2; exit 1; }
grep -Fq 'tss_set_rsp0(stack_top);' kernel/src/scheduler.c || {
    echo "scheduler.c не публикует TSS rsp0" >&2; exit 1; }
grep -Fq 'scheduler_task.kernel_stack_top = process->stack_top;' \
    kernel/src/process.c || {
    echo "process.c не публикует kernel_stack_top" >&2; exit 1; }
if nm -g build/hfoc-kernel.elf |
    grep -E 'sched_host_test_|scheduler_rsp0_host_test_'; then
    echo "Scheduler TSS rsp0 host symbols попали в kernel ELF" >&2
    exit 1
fi
grep -Fq 'context_frame_is_valid' kernel/include/context.h || {
    echo "context.h не объявляет context_frame_is_valid" >&2; exit 1; }
grep -Fq 'test-user-iretq-host' Makefile || {
    echo "Makefile не содержит test-user-iretq-host" >&2; exit 1; }
# shellcheck disable=SC2016
grep -Fq '$(MAKE) test-user-iretq-host' Makefile || {
    echo "test: target не содержит test-user-iretq-host" >&2; exit 1; }
grep -Fq 'user_iret_frame_init' kernel/include/user.h || {
    echo "user.h не содержит user_iret_frame_init" >&2; exit 1; }
grep -Fq 'user_iret_frame_valid' kernel/include/user.h || {
    echo "user.h не содержит user_iret_frame_valid" >&2; exit 1; }
grep -Fq 'user_enter_iretq' kernel/include/user.h || {
    echo "user.h не содержит user_enter_iretq" >&2; exit 1; }
grep -Fq 'iretq' kernel/src/arch/x86_64/user_entry.S || {
    echo "user_entry.S не содержит iretq" >&2; exit 1; }
if nm -g build/hfoc-kernel.elf | grep 'user_iretq_host_test_'; then
    echo "user_iretq_host_test_ символы попали в kernel ELF" >&2
    exit 1
fi
grep -Fq 'test-syscall-host' Makefile || {
    echo "Makefile не содержит test-syscall-host" >&2; exit 1; }
# shellcheck disable=SC2016
grep -Fq '$(MAKE) test-syscall-host' Makefile || {
    echo "test: target не содержит test-syscall-host" >&2; exit 1; }
grep -Fq 'IA32_STAR_MSR' kernel/include/syscall.h || {
    echo "syscall.h не содержит IA32_STAR_MSR" >&2; exit 1; }
grep -Fq 'IA32_LSTAR_MSR' kernel/include/syscall.h || {
    echo "syscall.h не содержит IA32_LSTAR_MSR" >&2; exit 1; }
grep -Fq 'IA32_FMASK_MSR' kernel/include/syscall.h || {
    echo "syscall.h не содержит IA32_FMASK_MSR" >&2; exit 1; }
grep -Fq 'SYSCALL_STAR_VALUE' kernel/include/syscall.h || {
    echo "syscall.h не содержит SYSCALL_STAR_VALUE" >&2; exit 1; }
grep -Fq 'SYSCALL_FMASK' kernel/include/syscall.h || {
    echo "syscall.h не содержит SYSCALL_FMASK" >&2; exit 1; }
grep -Fq 'sysretq' kernel/src/syscall_entry.S || {
    echo "syscall_entry.S не содержит sysretq" >&2; exit 1; }
for syscall_symbol in syscall_msr_prepare syscall_msr_verify \
                      syscall_handle syscall_entry; do
    nm build/hfoc-kernel.elf | grep -F " $syscall_symbol" >/dev/null || {
        echo "Production kernel не содержит $syscall_symbol" >&2
        exit 1
    }
done
if nm -g build/hfoc-kernel.elf | grep 'syscall_host_test_'; then
    echo "syscall_host_test_ символы попали в kernel ELF" >&2
    exit 1
fi
if bash scripts/hfoc-file-marker.sh build/hfoc-kernel.elf '[SYSCALL] proof init'; then
    echo "Production kernel содержит syscall proof marker" >&2
    exit 1
fi
grep -Fq 'test-user-init-process-host' Makefile || {
    echo "Makefile не содержит test-user-init-process-host" >&2; exit 1; }
# shellcheck disable=SC2016
grep -Fq '$(MAKE) test-user-init-process-host' Makefile || {
    echo "test: target не содержит test-user-init-process-host" >&2; exit 1; }
grep -Fq 'test-user-init' Makefile || {
    echo "Makefile не содержит test-user-init" >&2; exit 1; }
# shellcheck disable=SC2016
grep -Fq '$(MAKE) test-user-init' Makefile || {
    echo "test: target не содержит test-user-init" >&2; exit 1; }
grep -Fq 'user_init_image_copy' kernel/include/user_init.h || {
    echo "user_init.h не содержит user_init_image_copy" >&2; exit 1; }
grep -Fq 'process_create_user_init' kernel/include/process.h || {
    echo "process.h не содержит process_create_user_init" >&2; exit 1; }
if bash scripts/hfoc-file-marker.sh build/hfoc-kernel.elf '[USER-INIT] proof init'; then
    echo "Production kernel содержит user init proof marker" >&2
    exit 1
fi
grep -Fq 'test-syscall-origin-host' Makefile || {
    echo "Makefile не содержит test-syscall-origin-host" >&2; exit 1; }
# shellcheck disable=SC2016
grep -Fq '$(MAKE) test-syscall-origin-host' Makefile || {
    echo "test: target не содержит test-syscall-origin-host" >&2; exit 1; }
grep -Fq 'PROCESS_SECURITY_DOMAIN_KERNEL' kernel/include/process.h || {
    echo "process.h не содержит PROCESS_SECURITY_DOMAIN_KERNEL" >&2; exit 1; }
grep -Fq 'PROCESS_SECURITY_DOMAIN_USER' kernel/include/process.h || {
    echo "process.h не содержит PROCESS_SECURITY_DOMAIN_USER" >&2; exit 1; }
grep -Fq 'PROCESS_BAD_DOMAIN' kernel/include/process.h || {
    echo "process.h не содержит PROCESS_BAD_DOMAIN" >&2; exit 1; }
grep -Fq 'process_pcb_init_domain' kernel/include/process.h || {
    echo "process.h не содержит process_pcb_init_domain" >&2; exit 1; }
grep -Fq 'process_pcb_security_domain' kernel/include/process.h || {
    echo "process.h не содержит process_pcb_security_domain" >&2; exit 1; }
grep -Fq 'process_pcb_security_generation' kernel/include/process.h || {
    echo "process.h не содержит process_pcb_security_generation" >&2; exit 1; }
grep -Fq 'PROCESS_SECURITY_DOMAIN_USER' kernel/src/process.c || {
    echo "process.c не публикует user security domain" >&2; exit 1; }
grep -Fq '++process->pcb.security_generation;' kernel/src/process.c || {
    echo "process.c не инвалидирует security generation при exit" >&2; exit 1; }
grep -Fq 'struct syscall_origin' kernel/include/syscall.h || {
    echo "syscall.h не содержит struct syscall_origin" >&2; exit 1; }
grep -Fq 'SYSCALL_RET_EPERM' kernel/include/syscall.h || {
    echo "syscall.h не содержит SYSCALL_RET_EPERM" >&2; exit 1; }
grep -Fq 'syscall_origin_capture' kernel/include/syscall.h || {
    echo "syscall.h не содержит syscall_origin_capture" >&2; exit 1; }
grep -Fq 'syscall_origin_validate' kernel/include/syscall.h || {
    echo "syscall.h не содержит syscall_origin_validate" >&2; exit 1; }
grep -Fq 'syscall_handle_checked' kernel/include/syscall.h || {
    echo "syscall.h не содержит syscall_handle_checked" >&2; exit 1; }
for origin_symbol in syscall_origin_capture syscall_origin_validate \
                     syscall_handle_checked; do
    nm build/hfoc-kernel.elf | grep -F " $origin_symbol" >/dev/null || {
        echo "Production kernel не содержит $origin_symbol" >&2
        exit 1
    }
done
if nm -g build/hfoc-kernel.elf | grep 'syscall_origin_host_test_'; then
    echo "syscall_origin_host_test_ символы попали в kernel ELF" >&2
    exit 1
fi
if bash scripts/hfoc-file-marker.sh build/hfoc-kernel.elf '[SECURITY]'; then
    echo "Production kernel содержит security proof marker" >&2
    exit 1
fi
for security_doc in README.md docs/ARCHITECTURE.md docs/ROADMAP.md; do
    if ! grep -Eiq 'security domain|security-domain' "$security_doc"; then
        echo "$security_doc не документирует security domain" >&2
        exit 1
    fi
done
grep -Fq 'test-capability-host' Makefile || {
    echo "Makefile не содержит test-capability-host" >&2; exit 1; }
# shellcheck disable=SC2016
grep -Fq '$(MAKE) test-capability-host' Makefile || {
    echo "test: target не содержит test-capability-host" >&2; exit 1; }
grep -Fq 'PROCESS_CAPABILITY_CAPACITY' kernel/include/capability.h || {
    echo "capability.h не содержит PROCESS_CAPABILITY_CAPACITY" >&2; exit 1; }
grep -Fq 'PROCESS_CAPABILITY_HANDLE_INVALID' kernel/include/capability.h || {
    echo "capability.h не содержит PROCESS_CAPABILITY_HANDLE_INVALID" >&2; exit 1; }
grep -Fq 'CAPABILITY_TYPE_SYSCALL_WRITE' kernel/include/capability.h || {
    echo "capability.h не содержит CAPABILITY_TYPE_SYSCALL_WRITE" >&2; exit 1; }
grep -Fq 'CAPABILITY_TYPE_SYSCALL_YIELD' kernel/include/capability.h || {
    echo "capability.h не содержит CAPABILITY_TYPE_SYSCALL_YIELD" >&2; exit 1; }
grep -Fq 'CAPABILITY_TYPE_SYSCALL_EXIT' kernel/include/capability.h || {
    echo "capability.h не содержит CAPABILITY_TYPE_SYSCALL_EXIT" >&2; exit 1; }
grep -Fq 'capability_table_has' kernel/include/capability.h || {
    echo "capability.h не содержит capability_table_has" >&2; exit 1; }
grep -Fq 'capability_table_has' kernel/src/syscall.c || {
    echo "syscall.c не использует capability_table_has" >&2; exit 1; }
grep -Fq 'CAPABILITY_TYPE_SYSCALL_WRITE' kernel/src/process.c || {
    echo "process.c не публикует write syscall capability" >&2; exit 1; }
grep -Fq 'capability_table_add(&process->pcb.capability_table' \
    kernel/src/process.c || {
    echo "process.c не публикует user syscall capabilities" >&2; exit 1; }
grep -Fq 'test-syscall-gate-host' Makefile || {
    echo "Makefile не содержит test-syscall-gate-host" >&2; exit 1; }
# shellcheck disable=SC2016
grep -Fq '$(MAKE) test-syscall-gate-host' Makefile || {
    echo "test: target не содержит test-syscall-gate-host" >&2; exit 1; }
grep -Fq 'syscall_requires_capability' kernel/include/syscall.h || {
    echo "syscall.h не содержит syscall_requires_capability" >&2; exit 1; }
for capability_symbol in capability_table_init capability_table_destroy \
                         capability_table_reserve capability_table_commit \
                         capability_table_rollback capability_table_add \
                         capability_table_close capability_table_revoke \
                         capability_table_get capability_table_has \
                         capability_status_string; do
    nm build/hfoc-kernel.elf | grep -F " $capability_symbol" >/dev/null || {
        echo "Production kernel не содержит $capability_symbol" >&2
        exit 1
    }
done
nm build/hfoc-kernel.elf | grep -F ' syscall_requires_capability' \
    >/dev/null || {
    echo "Production kernel не содержит syscall_requires_capability" >&2
    exit 1
}
if nm -g build/hfoc-kernel.elf | grep 'capability_host_test_'; then
    echo "capability_host_test_ символы попали в kernel ELF" >&2
    exit 1
fi
if nm -g build/hfoc-kernel.elf | grep 'syscall_gate_host_test_'; then
    echo "syscall_gate_host_test_ символы попали в kernel ELF" >&2
    exit 1
fi
if bash scripts/hfoc-file-marker.sh build/hfoc-kernel.elf '[CAPABILITY]'; then
    echo "Production kernel содержит capability proof marker" >&2
    exit 1
fi
grep -Fq 'struct capability_table capability_table;' \
    kernel/include/process.h || {
    echo "process.h не содержит process-owned capability table" >&2; exit 1; }
grep -Fq 'capability_table_init(&pcb->capability_table);' \
    kernel/src/process.c || {
    echo "process.c не публикует capability table при PCB init" >&2; exit 1; }
grep -Fq 'capability_table_destroy(&process->pcb.capability_table)' \
    kernel/src/process.c || {
    echo "process.c не уничтожает capability table при exit" >&2; exit 1; }
for capability_doc in README.md docs/ARCHITECTURE.md docs/ROADMAP.md; do
    if ! grep -Eiq 'capability' "$capability_doc"; then
        echo "$capability_doc не документирует capability handle table" >&2
        exit 1
    fi
    if ! grep -Eiq 'capability-gated' "$capability_doc"; then
        echo "$capability_doc не документирует capability-gated syscalls" >&2
        exit 1
    fi
done
grep -Fq 'test-scheduler-yield-host' Makefile || {
    echo "Makefile не содержит test-scheduler-yield-host" >&2; exit 1; }
grep -Fq 'test-scheduler-rsp0-host' Makefile || {
    echo "Makefile не содержит test-scheduler-rsp0-host" >&2; exit 1; }
# shellcheck disable=SC2016
grep -Fq '$(MAKE) test-scheduler-rsp0-host' Makefile || {
    echo "test: target не содержит test-scheduler-rsp0-host" >&2; exit 1; }
# shellcheck disable=SC2016
grep -Fq '$(MAKE) test-scheduler-yield-host' Makefile || {
    echo "test: target не содержит test-scheduler-yield-host" >&2; exit 1; }
grep -Fq 'test-scheduler-yield' Makefile || {
    echo "Makefile не содержит test-scheduler-yield" >&2; exit 1; }
# shellcheck disable=SC2016
grep -Fq '$(MAKE) test-scheduler-yield' Makefile || {
    echo "test: target не содержит test-scheduler-yield" >&2; exit 1; }
bash -n scripts/test-qemu-scheduler-yield.sh || {
    echo "Синтаксис scripts/test-qemu-scheduler-yield.sh некорректен" >&2
    exit 1
}
grep -Fq 'QEMU APIC baseline: machine=q35 smp=1 cpu=default' \
    scripts/test-qemu-scheduler-yield.sh || {
    echo "test-qemu-scheduler-yield.sh не фиксирует portable baseline" >&2
    exit 1
}
grep -Fq 'QEMU scheduler yield test: OK' \
    scripts/test-qemu-scheduler-yield.sh || {
    echo "test-qemu-scheduler-yield.sh не содержит success marker" >&2
    exit 1
}
grep -Fq 'Servicing hardware INT=0x41' \
    scripts/test-qemu-scheduler-yield.sh || {
    echo "test-qemu-scheduler-yield.sh не требует INT=0x41" >&2
    exit 1
}
grep -Fq '[LAPIC-TIMER] irq vector=0x42' \
    scripts/test-qemu-scheduler-yield.sh || {
    echo "test-qemu-scheduler-yield.sh не требует LAPIC timer marker" >&2
    exit 1
}
if bash scripts/hfoc-file-marker.sh build/hfoc-kernel.elf '[SCHED] thread-A step='; then
    echo "Production kernel содержит scheduler yield test marker" >&2
    exit 1
fi

# Tick-observed scheduler preemption contracts.
grep -Fq 'sched_on_tick(void);' kernel/include/scheduler.h || {
    echo "scheduler.h не объявляет sched_on_tick" >&2; exit 1; }
grep -Fq '(void)sched_on_tick();' \
    kernel/src/arch/x86_64/interrupts.c || {
    echo "interrupts.c не вызывает sched_on_tick из vector 0x42" >&2
    exit 1
}
if ! awk '
    /if \(frame->vector == 0x42U\)/ { in_block = 1 }
    in_block && /lapic_eoi\(\);/ { eoi_seen = 1 }
    in_block && /\(void\)sched_on_tick\(\);/ {
        tick_seen = 1
        if (!eoi_seen) { bad_order = 1 }
    }
    in_block && /^    }/ {
        if (!eoi_seen || !tick_seen || bad_order) { exit 1 }
        exit 0
    }
    END { if (!in_block) exit 1 }
' kernel/src/arch/x86_64/interrupts.c; then
    echo "vector 0x42 должен вызывать lapic_eoi до sched_on_tick" >&2
    exit 1
fi
grep -Fq 'test-scheduler-tick-host' Makefile || {
    echo "Makefile не содержит test-scheduler-tick-host" >&2; exit 1; }
# shellcheck disable=SC2016
grep -Fq '$(MAKE) test-scheduler-tick-host' Makefile || {
    echo "test: target не содержит test-scheduler-tick-host" >&2; exit 1; }
grep -Fq 'test-scheduler-tick' Makefile || {
    echo "Makefile не содержит test-scheduler-tick" >&2; exit 1; }
# shellcheck disable=SC2016
grep -Fq '$(MAKE) test-scheduler-tick' Makefile || {
    echo "test: target не содержит test-scheduler-tick" >&2; exit 1; }
bash -n scripts/test-qemu-scheduler-tick.sh || {
    echo "Синтаксис scripts/test-qemu-scheduler-tick.sh некорректен" >&2
    exit 1
}
grep -Fq 'QEMU APIC baseline: machine=q35 smp=1 cpu=default' \
    scripts/test-qemu-scheduler-tick.sh || {
    echo "test-qemu-scheduler-tick.sh не фиксирует portable baseline" >&2
    exit 1
}
grep -Fq 'QEMU scheduler tick test: OK' \
    scripts/test-qemu-scheduler-tick.sh || {
    echo "test-qemu-scheduler-tick.sh не содержит success marker" >&2
    exit 1
}
grep -Fq 'Servicing hardware INT=0x41' \
    scripts/test-qemu-scheduler-tick.sh || {
    echo "test-qemu-scheduler-tick.sh не требует INT=0x41" >&2
    exit 1
}
grep -Fq '[LAPIC-TIMER] irq vector=0x42' \
    scripts/test-qemu-scheduler-tick.sh || {
    echo "test-qemu-scheduler-tick.sh не требует LAPIC timer marker" >&2
    exit 1
}
grep -Fq 'LAPIC timer EOI must precede first scheduler tick progress marker' \
    scripts/test-qemu-scheduler-tick.sh || {
    echo "test-qemu-scheduler-tick.sh не требует EOI до thread progress" >&2
    exit 1
}
if bash scripts/hfoc-file-marker.sh build/hfoc-kernel.elf '[SCHED-TICK] thread-A step='; then
    echo "Production kernel содержит scheduler tick test marker" >&2
    exit 1
fi
