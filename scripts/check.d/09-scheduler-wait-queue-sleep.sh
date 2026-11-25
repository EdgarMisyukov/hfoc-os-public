#!/usr/bin/env bash
# ITEM-387: contracted slice of the HFOC OS check suite. Sourced in pinned
#           order by scripts/check.sh; lint it on its own with shellcheck.

# Scheduler wait-queue and sleep timeout contracts.
for wait_api in \
    'struct scheduler_wait_queue' \
    'sched_wait_queue_init(' \
    'sched_wait(' \
    'sched_wake_one(' \
    'sched_wake_all(' \
    'sched_sleep('; do
    grep -Fq "$wait_api" kernel/include/scheduler.h || {
        echo "scheduler.h не содержит wait API: $wait_api" >&2
        exit 1
    }
done
grep -Fq 'test-scheduler-wait-host' Makefile || {
    echo "Makefile не содержит test-scheduler-wait-host" >&2; exit 1; }
# shellcheck disable=SC2016
grep -Fq '$(MAKE) test-scheduler-wait-host' Makefile || {
    echo "test: target не содержит test-scheduler-wait-host" >&2; exit 1; }
grep -Fq 'test-scheduler-wait' Makefile || {
    echo "Makefile не содержит test-scheduler-wait" >&2; exit 1; }
# shellcheck disable=SC2016
grep -Fq '$(MAKE) test-scheduler-wait' Makefile || {
    echo "test: target не содержит test-scheduler-wait" >&2; exit 1; }
bash -n scripts/test-qemu-scheduler-wait.sh || {
    echo "Синтаксис scripts/test-qemu-scheduler-wait.sh некорректен" >&2
    exit 1
}
grep -Fq 'QEMU APIC baseline: machine=q35 smp=1 cpu=default' \
    scripts/test-qemu-scheduler-wait.sh || {
    echo "test-qemu-scheduler-wait.sh не фиксирует portable baseline" >&2
    exit 1
}
grep -Fq 'QEMU scheduler wait test: OK' \
    scripts/test-qemu-scheduler-wait.sh || {
    echo "test-qemu-scheduler-wait.sh не содержит success marker" >&2
    exit 1
}
grep -Fq 'Servicing hardware INT=0x41' \
    scripts/test-qemu-scheduler-wait.sh || {
    echo "test-qemu-scheduler-wait.sh не требует INT=0x41" >&2
    exit 1
}
grep -Fq '[LAPIC-TIMER] irq vector=0x42' \
    scripts/test-qemu-scheduler-wait.sh || {
    echo "test-qemu-scheduler-wait.sh не требует LAPIC timer marker" >&2
    exit 1
}
if bash scripts/hfoc-file-marker.sh build/hfoc-kernel.elf '[WAIT] sleep start'; then
    echo "Production kernel содержит scheduler wait test marker" >&2
    exit 1
fi

# Scheduler queue-mutation discipline contracts.
grep -Fq 'test-scheduler-discipline-host' Makefile || {
    echo "Makefile не содержит test-scheduler-discipline-host" >&2; exit 1; }
# shellcheck disable=SC2016
grep -Fq '$(MAKE) test-scheduler-discipline-host' Makefile || {
    echo "test: target не содержит test-scheduler-discipline-host" >&2; exit 1; }
grep -Fq 'sched_enqueue_locked' kernel/src/scheduler.c || {
    echo "scheduler.c не содержит sched_enqueue_locked" >&2; exit 1; }
grep -Fq 'sched_next_locked' kernel/src/scheduler.c || {
    echo "scheduler.c не содержит sched_next_locked" >&2; exit 1; }
grep -Fq 'sched_transition_locked' kernel/src/scheduler.c || {
    echo "scheduler.c не содержит sched_transition_locked" >&2; exit 1; }
if bash scripts/hfoc-file-marker.sh build/hfoc-kernel.elf \
    'Scheduler queue mutation discipline host tests'; then
    echo "Production kernel содержит scheduler discipline host marker" >&2
    exit 1
fi

# Kernel task exit and deferred stack reclamation contracts.
grep -Fq 'sched_exit(void);' kernel/include/scheduler.h || {
    echo "scheduler.h не объявляет sched_exit" >&2; exit 1; }
grep -Fq 'sched_reclaim_deferred' kernel/src/scheduler.c || {
    echo "scheduler.c не содержит deferred reclamation" >&2; exit 1; }
grep -Fq 'call sched_reclaim_deferred' \
    kernel/src/arch/x86_64/sched_trampoline.S || {
    echo "sched_trampoline.S не реклаймит deferred stack" >&2; exit 1; }
grep -Fq 'call sched_exit' kernel/src/arch/x86_64/sched_trampoline.S || {
    echo "sched_trampoline.S не вызывает sched_exit" >&2; exit 1; }
grep -Fq 'test-scheduler-exit-host' Makefile || {
    echo "Makefile не содержит test-scheduler-exit-host" >&2; exit 1; }
# shellcheck disable=SC2016
grep -Fq '$(MAKE) test-scheduler-exit-host' Makefile || {
    echo "test: target не содержит test-scheduler-exit-host" >&2; exit 1; }
grep -Fq 'test-scheduler-exit' Makefile || {
    echo "Makefile не содержит test-scheduler-exit" >&2; exit 1; }
# shellcheck disable=SC2016
grep -Fq '$(MAKE) test-scheduler-exit' Makefile || {
    echo "test: target не содержит test-scheduler-exit" >&2; exit 1; }
bash -n scripts/test-qemu-scheduler-exit.sh || {
    echo "Синтаксис scripts/test-qemu-scheduler-exit.sh некорректен" >&2
    exit 1
}
grep -Fq 'QEMU APIC baseline: machine=q35 smp=1 cpu=default' \
    scripts/test-qemu-scheduler-exit.sh || {
    echo "test-qemu-scheduler-exit.sh не фиксирует portable baseline" >&2
    exit 1
}
grep -Fq 'QEMU scheduler exit test: OK' \
    scripts/test-qemu-scheduler-exit.sh || {
    echo "test-qemu-scheduler-exit.sh не содержит success marker" >&2
    exit 1
}
grep -Fq 'Servicing hardware INT=0x41' \
    scripts/test-qemu-scheduler-exit.sh || {
    echo "test-qemu-scheduler-exit.sh не требует INT=0x41" >&2
    exit 1
}
grep -Fq '[LAPIC-TIMER] irq vector=0x42' \
    scripts/test-qemu-scheduler-exit.sh || {
    echo "test-qemu-scheduler-exit.sh не требует LAPIC timer marker" >&2
    exit 1
}
if bash scripts/hfoc-file-marker.sh build/hfoc-kernel.elf '[EXIT] thread-A before exit'; then
    echo "Production kernel содержит scheduler exit test marker" >&2
    exit 1
fi
if bash scripts/hfoc-file-marker.sh build/hfoc-kernel.elf '[EXIT] thread-A after exit'; then
    echo "Production kernel содержит post-exit test marker" >&2
    exit 1
fi

# Deterministic scheduler stress and fault-injection contracts.
grep -Fq 'test-scheduler-stress-host' Makefile || {
    echo "Makefile не содержит test-scheduler-stress-host" >&2; exit 1; }
# shellcheck disable=SC2016
grep -Fq '$(MAKE) test-scheduler-stress-host' Makefile || {
    echo "test: target не содержит test-scheduler-stress-host" >&2; exit 1; }
grep -Fq 'test-scheduler-stress' Makefile || {
    echo "Makefile не содержит test-scheduler-stress" >&2; exit 1; }
# shellcheck disable=SC2016
grep -Fq '$(MAKE) test-scheduler-stress' Makefile || {
    echo "test: target не содержит test-scheduler-stress" >&2; exit 1; }
bash -n scripts/test-qemu-scheduler-stress.sh || {
    echo "Синтаксис scripts/test-qemu-scheduler-stress.sh некорректен" >&2
    exit 1
}
grep -Fq 'QEMU APIC baseline: machine=q35 smp=1 cpu=default' \
    scripts/test-qemu-scheduler-stress.sh || {
    echo "test-qemu-scheduler-stress.sh не фиксирует portable baseline" >&2
    exit 1
}
grep -Fq 'QEMU scheduler stress test: OK' \
    scripts/test-qemu-scheduler-stress.sh || {
    echo "test-qemu-scheduler-stress.sh не содержит success marker" >&2
    exit 1
}
grep -Fq 'Servicing hardware INT=0x41' \
    scripts/test-qemu-scheduler-stress.sh || {
    echo "test-qemu-scheduler-stress.sh не требует INT=0x41" >&2
    exit 1
}
grep -Fq '[LAPIC-TIMER] irq vector=0x42' \
    scripts/test-qemu-scheduler-stress.sh || {
    echo "test-qemu-scheduler-stress.sh не требует LAPIC timer marker" >&2
    exit 1
}
grep -Fq '[STRESS] complete workers=7 reclaimed pages=28' \
    scripts/test-qemu-scheduler-stress.sh || {
    echo "test-qemu-scheduler-stress.sh не требует exact stress completion" >&2
    exit 1
}
if bash scripts/hfoc-file-marker.sh build/hfoc-kernel.elf '[STRESS] driver start'; then
    echo "Production kernel содержит scheduler stress driver marker" >&2
    exit 1
fi
if bash scripts/hfoc-file-marker.sh build/hfoc-kernel.elf '[STRESS] complete workers='; then
    echo "Production kernel содержит scheduler stress completion marker" >&2
    exit 1
fi

# Process scheduler integration proof contracts.
grep -Fq 'test-process-scheduler-host' Makefile || {
    echo "Makefile не содержит test-process-scheduler-host" >&2; exit 1; }
# shellcheck disable=SC2016
grep -Fq '$(MAKE) test-process-scheduler-host' Makefile || {
    echo "test: target не содержит test-process-scheduler-host" >&2; exit 1; }
grep -Fq 'test-process-scheduler' Makefile || {
    echo "Makefile не содержит test-process-scheduler" >&2; exit 1; }
# shellcheck disable=SC2016
grep -Fq '$(MAKE) test-process-scheduler' Makefile || {
    echo "test: target не содержит test-process-scheduler" >&2; exit 1; }
bash -n scripts/test-qemu-process-scheduler.sh || {
    echo "Синтаксис scripts/test-qemu-process-scheduler.sh некорректен" >&2
    exit 1
}
grep -Fq 'QEMU APIC baseline: machine=q35 smp=1 cpu=default' \
    scripts/test-qemu-process-scheduler.sh || {
    echo "test-qemu-process-scheduler.sh не фиксирует portable baseline" >&2
    exit 1
}
grep -Fq 'QEMU process scheduler test: OK' \
    scripts/test-qemu-process-scheduler.sh || {
    echo "test-qemu-process-scheduler.sh не содержит success marker" >&2
    exit 1
}
grep -Fq 'test-user-iretq' Makefile || {
    echo "Makefile не содержит test-user-iretq" >&2; exit 1; }
# shellcheck disable=SC2016
grep -Fq '$(MAKE) test-user-iretq' Makefile || {
    echo "test: target не содержит test-user-iretq" >&2; exit 1; }
bash -n scripts/test-qemu-user-iretq.sh || {
    echo "Синтаксис scripts/test-qemu-user-iretq.sh некорректен" >&2
    exit 1
}
grep -Fq 'QEMU APIC baseline: machine=q35 smp=1 cpu=default' \
    scripts/test-qemu-user-iretq.sh || {
    echo "test-qemu-user-iretq.sh не фиксирует portable baseline" >&2
    exit 1
}
grep -Fq 'QEMU user iretq test: OK' \
    scripts/test-qemu-user-iretq.sh || {
    echo "test-qemu-user-iretq.sh не содержит success marker" >&2
    exit 1
}
grep -Fq '[USER] proof OK' \
    scripts/test-qemu-user-iretq.sh || {
    echo "test-qemu-user-iretq.sh не требует proof OK" >&2
    exit 1
}
grep -Fq 'test-syscall' Makefile || {
    echo "Makefile не содержит test-syscall" >&2; exit 1; }
# shellcheck disable=SC2016
grep -Fq '$(MAKE) test-syscall' Makefile || {
    echo "test: target не содержит test-syscall" >&2; exit 1; }
bash -n scripts/test-qemu-syscall.sh || {
    echo "Синтаксис scripts/test-qemu-syscall.sh некорректен" >&2
    exit 1
}
grep -Fq 'QEMU APIC baseline: machine=q35 smp=1 cpu=default' \
    scripts/test-qemu-syscall.sh || {
    echo "test-qemu-syscall.sh не фиксирует portable baseline" >&2
    exit 1
}
grep -Fq 'QEMU syscall ABI test: OK' \
    scripts/test-qemu-syscall.sh || {
    echo "test-qemu-syscall.sh не содержит success marker" >&2
    exit 1
}
grep -Fq '[SYSCALL] proof OK' \
    scripts/test-qemu-syscall.sh || {
    echo "test-qemu-syscall.sh не требует proof OK" >&2
    exit 1
}
grep -Fq 'syscall\.sh' .github/workflows/ci.yml || {
    echo "CI pgrep pattern не содержит test-qemu-syscall.sh" >&2
    exit 1
}
grep -Fq 'test-user-init' Makefile || {
    echo "Makefile не содержит test-user-init" >&2; exit 1; }
# shellcheck disable=SC2016
grep -Fq '$(MAKE) test-user-init' Makefile || {
    echo "test: target не содержит test-user-init" >&2; exit 1; }
bash -n scripts/test-qemu-user-init.sh || {
    echo "Синтаксис scripts/test-qemu-user-init.sh некорректен" >&2
    exit 1
}
grep -Fq 'QEMU APIC baseline: machine=q35 smp=1 cpu=default' \
    scripts/test-qemu-user-init.sh || {
    echo "test-qemu-user-init.sh не фиксирует portable baseline" >&2
    exit 1
}
grep -Fq 'QEMU user init test: OK' \
    scripts/test-qemu-user-init.sh || {
    echo "test-qemu-user-init.sh не содержит success marker" >&2
    exit 1
}
grep -Fq 'user-init\.sh' .github/workflows/ci.yml || {
    echo "CI pgrep pattern не содержит test-qemu-user-init.sh" >&2
    exit 1
}
grep -Fq '[SECURITY] capture OK' scripts/test-qemu-user-init.sh || {
    echo "test-qemu-user-init.sh не требует security capture marker" >&2
    exit 1
}
grep -Fq '[SECURITY] cross-domain rejected' \
    scripts/test-qemu-user-init.sh || {
    echo "test-qemu-user-init.sh не требует cross-domain rejection" >&2
    exit 1
}
grep -Fq '[SECURITY] stale-origin rejected' \
    scripts/test-qemu-user-init.sh || {
    echo "test-qemu-user-init.sh не требует stale-origin rejection" >&2
    exit 1
}
grep -Fq '[CAPABILITY] child syscall capabilities OK' \
    scripts/test-qemu-user-init.sh || {
    echo "test-qemu-user-init.sh не требует capability publication" >&2
    exit 1
}
grep -Fq '[CAPABILITY] write denied' \
    scripts/test-qemu-user-init.sh || {
    echo "test-qemu-user-init.sh не требует denied capability proof" >&2
    exit 1
}
grep -Fq '[AUDIT-OBS] published operation=0 result=6' \
    scripts/test-qemu-user-init.sh || {
    echo "test-qemu-user-init.sh не требует audit syscall publication" >&2
    exit 1
}
grep -Fq '[AUDIT-OBS] consumed index=0 sequence=0 type=2 operation=0 result=6 pid=2 domain=1 generation=0' \
    scripts/test-qemu-user-init.sh || {
    echo "test-qemu-user-init.sh не требует exact first audit consumption" >&2
    exit 1
}
grep -Fq '[AUDIT-OBS] parent consumed exactly once' \
    scripts/test-qemu-user-init.sh || {
    echo "test-qemu-user-init.sh не требует exact-once audit consumption" >&2
    exit 1
}
grep -Fq 'syscall_origin_capture' kernel/src/user_init_qemu.c || {
    echo "user_init_qemu.c не использует syscall_origin_capture" >&2
    exit 1
}
grep -Fq 'syscall_handle_checked' kernel/src/user_init_qemu.c || {
    echo "user_init_qemu.c не использует syscall_handle_checked" >&2
    exit 1
}
grep -Fq 'Servicing hardware INT=0x41' \
    scripts/test-qemu-process-scheduler.sh || {
    echo "test-qemu-process-scheduler.sh не требует INT=0x41" >&2
    exit 1
}
grep -Fq '[LAPIC-TIMER] irq vector=0x42' \
    scripts/test-qemu-process-scheduler.sh || {
    echo "test-qemu-process-scheduler.sh не требует LAPIC timer marker" >&2
    exit 1
}
grep -Fq '[PROC] parent reaped all' \
    scripts/test-qemu-process-scheduler.sh || {
    echo "test-qemu-process-scheduler.sh не требует parent reap marker" >&2
    exit 1
}
grep -Fq '[PROC] limited child start' \
    scripts/test-qemu-process-scheduler.sh || {
    echo "test-qemu-process-scheduler.sh не требует limited child marker" >&2
    exit 1
}
grep -Fq 'status=4294967295' \
    scripts/test-qemu-process-scheduler.sh || {
    echo "test-qemu-process-scheduler.sh не требует exact limit exit status" >&2
    exit 1
}
grep -Fq 'process-scheduler\.sh' .github/workflows/ci.yml || {
    echo "CI pgrep pattern не содержит test-qemu-process-scheduler.sh" >&2
    exit 1
}
if bash scripts/hfoc-file-marker.sh build/hfoc-kernel.elf '[PROC] parent reaped all'; then
    echo "Production kernel содержит process scheduler test marker" >&2
    exit 1
fi
if bash scripts/hfoc-file-marker.sh build/hfoc-kernel.elf '[PROC] limited child start'; then
    echo "Production kernel содержит process limit test marker" >&2
    exit 1
fi

# Per-process CPU tick and user-memory limit contracts.
grep -Fq 'test-process-limit-host' Makefile || {
    echo "Makefile не содержит test-process-limit-host" >&2; exit 1; }
# shellcheck disable=SC2016
grep -Fq '$(MAKE) test-process-limit-host' Makefile || {
    echo "test: target не содержит test-process-limit-host" >&2; exit 1; }
grep -Fq 'PROCESS_CPU_TICK_LIMIT_UNLIMITED' kernel/include/process.h || {
    echo "process.h не содержит PROCESS_CPU_TICK_LIMIT_UNLIMITED" >&2; exit 1; }
grep -Fq 'PROCESS_USER_MEMORY_LIMIT_UNLIMITED' kernel/include/process.h || {
    echo "process.h не содержит PROCESS_USER_MEMORY_LIMIT_UNLIMITED" >&2; exit 1; }
grep -Fq 'PROCESS_EXIT_LIMIT_EXCEEDED' kernel/include/process.h || {
    echo "process.h не содержит PROCESS_EXIT_LIMIT_EXCEEDED" >&2; exit 1; }
grep -Fq 'PROCESS_USER_MEMORY_LIMIT_EXCEEDED' kernel/include/process.h || {
    echo "process.h не содержит PROCESS_USER_MEMORY_LIMIT_EXCEEDED" >&2; exit 1; }
for process_limit_symbol in process_limit_set_cpu_ticks \
                            process_limit_set_user_memory_pages \
                            process_limit_charge_user_memory \
                            process_limit_cpu_ticks \
                            process_limit_user_memory_limit_pages \
                            process_limit_user_memory_pages; do
    nm build/hfoc-kernel.elf | grep -F " $process_limit_symbol" \
        >/dev/null || {
        echo "Production kernel не содержит $process_limit_symbol" >&2
        exit 1
    }
done
grep -Fq 'sched_set_limit_hook' kernel/include/scheduler.h || {
    echo "scheduler.h не содержит sched_set_limit_hook" >&2; exit 1; }
grep -Fq 'cpu_tick_limit' kernel/include/scheduler.h || {
    echo "scheduler.h не содержит cpu_tick_limit" >&2; exit 1; }
grep -Fq 'scheduler_exit_current_locked' kernel/src/scheduler.c || {
    echo "scheduler.c не содержит scheduler_exit_current_locked" >&2; exit 1; }
grep -Fq 'process_limit_charge_user_memory(process, 2U)' \
    kernel/src/process.c || {
    echo "process.c не учитывает user pages при user-init publication" >&2
    exit 1
}
if bash scripts/hfoc-file-marker.sh build/hfoc-kernel.elf \
        'Process limit host tests: ALL OK'; then
    echo "Production kernel содержит process limit host marker" >&2
    exit 1
fi
for limit_doc in README.md docs/ARCHITECTURE.md docs/ROADMAP.md; do
    if ! grep -Eiq 'process limit|CPU-tick|user-memory limit' "$limit_doc"; then
        echo "$limit_doc не документирует process limits" >&2
        exit 1
    fi
done

# PCB, PID and parent/child ownership contracts.
grep -Fq 'test-process-host' Makefile || {
    echo "Makefile не содержит test-process-host" >&2; exit 1; }
# shellcheck disable=SC2016
grep -Fq '$(MAKE) test-process-host' Makefile || {
    echo "test: target не содержит test-process-host" >&2; exit 1; }
grep -Fq 'test-process-create-host' Makefile || {
    echo "Makefile не содержит test-process-create-host" >&2; exit 1; }
# shellcheck disable=SC2016
grep -Fq '$(MAKE) test-process-create-host' Makefile || {
    echo "test: target не содержит test-process-create-host" >&2; exit 1; }
grep -Fq 'PROCESS_MAX_PIDS' kernel/include/process.h || {
    echo "process.h не содержит PROCESS_MAX_PIDS" >&2; exit 1; }
grep -Fq 'PROCESS_KERNEL_STACK_PAGES' kernel/include/process.h || {
    echo "process.h не содержит PROCESS_KERNEL_STACK_PAGES" >&2; exit 1; }
grep -Fq 'PROCESS_LIFECYCLE_RUNNING' kernel/include/process.h || {
    echo "process.h не содержит PROCESS_LIFECYCLE_RUNNING" >&2; exit 1; }
grep -Fq 'PROCESS_LIFECYCLE_EXITED' kernel/include/process.h || {
    echo "process.h не содержит PROCESS_LIFECYCLE_EXITED" >&2; exit 1; }
grep -Fq 'PROCESS_NO_CHILDREN' kernel/include/process.h || {
    echo "process.h не содержит PROCESS_NO_CHILDREN" >&2; exit 1; }
grep -Fq 'PROCESS_NOT_EXITED' kernel/include/process.h || {
    echo "process.h не содержит PROCESS_NOT_EXITED" >&2; exit 1; }
grep -Fq 'PROCESS_ALREADY_EXITED' kernel/include/process.h || {
    echo "process.h не содержит PROCESS_ALREADY_EXITED" >&2; exit 1; }
grep -Fq 'struct kernel_process' kernel/include/process.h || {
    echo "process.h не содержит struct kernel_process" >&2; exit 1; }
grep -Fq 'bool orphaned' kernel/include/process.h || {
    echo "process.h не содержит orphaned PCB marker" >&2; exit 1; }
grep -Fq 'process_exit_kernel' kernel/include/process.h || {
    echo "process.h не содержит process_exit_kernel" >&2; exit 1; }
grep -Fq 'process_wait_kernel' kernel/include/process.h || {
    echo "process.h не содержит process_wait_kernel" >&2; exit 1; }
grep -Fq 'process_reap_kernel' kernel/include/process.h || {
    echo "process.h не содержит process_reap_kernel" >&2; exit 1; }
grep -Fq 'process_reap_kernel' kernel/src/process.c || {
    echo "process.c не содержит process_reap_kernel" >&2; exit 1; }
grep -Fq 'process_spawn_kernel' kernel/include/process.h || {
    echo "process.h не содержит process_spawn_kernel" >&2; exit 1; }
grep -Fq 'scheduler_published' kernel/include/process.h || {
    echo "process.h не содержит scheduler_published" >&2; exit 1; }
grep -Fq 'process_owner' kernel/include/scheduler.h || {
    echo "scheduler.h не содержит process_owner" >&2; exit 1; }
grep -Fq 'sched_enqueue' kernel/src/process.c || {
    echo "process.c не вызывает sched_enqueue" >&2; exit 1; }
for process_symbol in process_pid_alloc process_pid_free process_pcb_init \
                      process_pcb_destroy process_pcb_child_count \
                      process_status_string process_create_kernel \
                      process_destroy_kernel process_exit_kernel \
                      process_wait_kernel process_reap_kernel \
                      process_spawn_kernel; do
    nm build/hfoc-kernel.elf | grep -F " $process_symbol" >/dev/null || {
        echo "Production kernel не содержит $process_symbol" >&2
        exit 1
    }
done
if nm -g build/hfoc-kernel.elf |
        grep -E 'process_host_test_|process_test_'; then
    echo "Process host test symbols попали в kernel ELF" >&2
    exit 1
fi
grep -Fq 'vmm_destroy_address_space' kernel/include/vmm.h || {
    echo "vmm.h не содержит vmm_destroy_address_space" >&2; exit 1; }
nm build/hfoc-kernel.elf | grep -F ' vmm_destroy_address_space' >/dev/null || {
    echo "Production kernel не содержит vmm_destroy_address_space" >&2
    exit 1
}
if bash scripts/hfoc-file-marker.sh build/hfoc-kernel.elf \
        'Process creation host tests: ALL OK'; then
    echo "Production kernel содержит process creation host marker" >&2
    exit 1
fi
if bash scripts/hfoc-file-marker.sh build/hfoc-kernel.elf \
        'Process scheduler host tests: ALL OK'; then
    echo "Production kernel содержит process scheduler host marker" >&2
    exit 1
fi

# Descriptor table lifetime ownership contracts.
grep -Fq 'test-descriptor-host' Makefile || {
    echo "Makefile не содержит test-descriptor-host" >&2; exit 1; }
# shellcheck disable=SC2016
grep -Fq '$(MAKE) test-descriptor-host' Makefile || {
    echo "test: target не содержит test-descriptor-host" >&2; exit 1; }
grep -Fq 'PROCESS_DESCRIPTOR_CAPACITY' kernel/include/descriptor.h || {
    echo "descriptor.h не содержит PROCESS_DESCRIPTOR_CAPACITY" >&2; exit 1; }
grep -Fq 'struct descriptor_table' kernel/include/process.h || {
    echo "process.h не содержит descriptor_table" >&2; exit 1; }
grep -Fq 'descriptor_table_destroy' kernel/src/process.c || {
    echo "process.c не вызывает descriptor_table_destroy" >&2; exit 1; }
for descriptor_symbol in descriptor_table_init descriptor_table_destroy \
                         descriptor_table_reserve descriptor_table_release \
                         descriptor_table_open_count \
                         descriptor_table_is_published \
                         descriptor_status_string; do
    nm build/hfoc-kernel.elf | grep -F " $descriptor_symbol" >/dev/null || {
        echo "Production kernel не содержит $descriptor_symbol" >&2
        exit 1
    }
done
if nm -g build/hfoc-kernel.elf |
        grep -E 'descriptor_host_test_|descriptor_test_'; then
    echo "Descriptor host test symbols попали в kernel ELF" >&2
    exit 1
fi
if bash scripts/hfoc-file-marker.sh build/hfoc-kernel.elf \
        'Descriptor table host tests: ALL OK'; then
    echo "Production kernel содержит descriptor host marker" >&2
    exit 1
fi

# Strict appliance configuration schema parser contracts.
grep -Fq 'test-config-schema-host' Makefile || {
    echo "Makefile не содержит test-config-schema-host" >&2; exit 1; }
# shellcheck disable=SC2016
grep -Fq '$(MAKE) test-config-schema-host' Makefile || {
    echo "test: target не содержит test-config-schema-host" >&2; exit 1; }
grep -Fq 'CONFIG_SCHEMA_VERSION' kernel/include/config_schema.h || {
    echo "config_schema.h не содержит CONFIG_SCHEMA_VERSION" >&2; exit 1; }
grep -Fq 'CONFIG_INPUT_MAX' kernel/include/config_schema.h || {
    echo "config_schema.h не содержит CONFIG_INPUT_MAX" >&2; exit 1; }
grep -Fq 'struct appliance_config' kernel/include/config_schema.h || {
    echo "config_schema.h не содержит struct appliance_config" >&2; exit 1; }
grep -Fq 'config_schema_parse' kernel/include/config_schema.h || {
    echo "config_schema.h не содержит config_schema_parse" >&2; exit 1; }
for config_symbol in config_schema_parse config_status_string; do
    nm build/hfoc-kernel.elf | grep -F " $config_symbol" >/dev/null || {
        echo "Production kernel не содержит $config_symbol" >&2
        exit 1
    }
done
if nm -g build/hfoc-kernel.elf |
        grep -E 'config_schema_host_test_|config_schema_test_'; then
    echo "Configuration schema host test symbols попали в kernel ELF" >&2
    exit 1
fi
if bash scripts/hfoc-file-marker.sh build/hfoc-kernel.elf \
        'Configuration schema host tests: ALL OK'; then
    echo "Production kernel содержит configuration schema host marker" >&2
    exit 1
fi
if bash scripts/hfoc-file-marker.sh build/hfoc-kernel.elf '[CONFIG-SCHEMA]'; then
    echo "Production kernel содержит configuration schema proof marker" >&2
    exit 1
fi
for config_doc in README.md docs/ARCHITECTURE.md docs/ROADMAP.md; do
    if ! grep -Eiq 'configuration schema|config schema' "$config_doc"; then
        echo "$config_doc не документирует configuration schema" >&2
        exit 1
    fi
done

# Transactional appliance configuration snapshot contracts.
grep -Fq 'test-config-snapshot-host' Makefile || {
    echo "Makefile не содержит test-config-snapshot-host" >&2; exit 1; }
# shellcheck disable=SC2016
grep -Fq '$(MAKE) test-config-snapshot-host' Makefile || {
    echo "test: target не содержит test-config-snapshot-host" >&2; exit 1; }
grep -Fq 'struct config_snapshot' kernel/include/config_schema.h || {
    echo "config_schema.h не содержит struct config_snapshot" >&2; exit 1; }
grep -Fq 'config_commit_validator' kernel/include/config_schema.h || {
    echo "config_schema.h не содержит config_commit_validator" >&2; exit 1; }
for config_snapshot_symbol in config_snapshot_init config_snapshot_prepare \
                              config_snapshot_commit config_snapshot_rollback \
                              config_snapshot_destroy \
                              config_snapshot_active \
                              config_snapshot_has_pending \
                              config_snapshot_status_string; do
    nm build/hfoc-kernel.elf | grep -F " $config_snapshot_symbol" \
        >/dev/null || {
        echo "Production kernel не содержит $config_snapshot_symbol" >&2
        exit 1
    }
done
if grep -nE '\b(kmalloc|kfree|pmm_alloc_contiguous_pages|pmm_free_contiguous_pages)\b' \
        kernel/src/config_schema.c; then
    echo "config_schema.c выполняет запрещённое выделение" >&2
    exit 1
fi
if bash scripts/hfoc-file-marker.sh build/hfoc-kernel.elf \
        'Configuration snapshot host tests: ALL OK'; then
    echo "Production kernel содержит configuration snapshot host marker" >&2
    exit 1
fi
if bash scripts/hfoc-file-marker.sh build/hfoc-kernel.elf '[CONFIG-SNAPSHOT]'; then
    echo "Production kernel содержит configuration snapshot proof marker" >&2
    exit 1
fi
for config_snapshot_doc in README.md docs/ARCHITECTURE.md docs/ROADMAP.md; do
    if ! grep -Eiq 'configuration snapshot|config snapshot|transactional.*configuration' \
            "$config_snapshot_doc"; then
        echo "$config_snapshot_doc не документирует configuration snapshot" >&2
        exit 1
    fi
done
