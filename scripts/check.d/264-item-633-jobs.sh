#!/usr/bin/env bash
# ITEM-633: jobs, process groups, terminal signals and the SSH proof.
#           Sourced in pinned order by scripts/check.sh; lint it on its own.
# shellcheck disable=SC2016  # the grep patterns are literal shell syntax

[[ -f docs/hfoc-sh-jobs-633.md ]] || {
    echo "ITEM-633: отсутствует матрица job control" >&2
    exit 1
}
for row in 'Ctrl-C' 'Ctrl-Z' 'jobs' 'fg [%n]' 'bg [%n]' 'WAIT2' \
           'Only the session leader' '8 jobs'; do
    grep -Fq -- "$row" docs/hfoc-sh-jobs-633.md || {
        echo "ITEM-633: документ не называет:" "$row" >&2
        exit 1
    }
done

for field in 'uint32_t pgid;' 'uint32_t session_id;' \
             'uint32_t wait_stopped;' 'uint32_t wait_continued;' \
             'PROCESS_LIFECYCLE_STOPPED' 'PROCESS_WAIT_STATUS_STOPPED' \
             'PROCESS_SIGNAL_TSTP'; do
    grep -Fq -- "$field" kernel/include/process.h || {
        echo "ITEM-633: process.h не несёт:" "$field" >&2
        exit 1
    }
done
for part in 'process_signal_group' 'process_signal_session' \
            'process_wait2_kernel' 'process_setpgid_kernel' \
            'process_find_pid' 'sched_suspend' 'sched_resume'; do
    grep -Fq -- "$part" kernel/src/process.c kernel/src/scheduler.c || {
        echo "ITEM-633: нет реализации:" "$part" >&2
        exit 1
    }
done

for syscall in 'SYSCALL_KILL = 21' 'SYSCALL_WAIT2 = 22' \
               'SYSCALL_SETPGID = 23' 'SYSCALL_TTY_FOREGROUND = 24' \
               'SYSCALL_SLEEP = 25' '#define SYSCALL_COUNT 26U'; do
    grep -Fq -- "$syscall" kernel/include/syscall.h || {
        echo "ITEM-633: ABI не закреплён:" "$syscall" >&2
        exit 1
    }
done

for tty_part in 'tty_set_foreground' 'foreground_pgid' \
                'PROCESS_SIGNAL_TSTP' 'a background group cannot read the TTY'; do
    grep -Fq -- "$tty_part" kernel/src/tty.c kernel/include/tty.h || {
        echo "ITEM-633: TTY не несёт:" "$tty_part" >&2
        exit 1
    }
done

# The timeout queue must be serviced from the real tick path, and blocking
# waits must allow the PIT to fire while a sleeping child is waited for.
grep -Fq 'sched_timeout_tick' kernel/src/arch/x86_64/interrupts.c || {
    echo "ITEM-633: PIT/IOAPIC tick не будит таймауты" >&2
    exit 1
}
grep -Fq 'sched_timeout_tick' kernel/include/scheduler.h || {
    echo "ITEM-633: нет sched_timeout_tick" >&2
    exit 1
}
grep -Fq '__asm__ volatile ("sti"' kernel/src/syscall_exec.c || {
    echo "ITEM-633: blocking WAIT не разрешает PIT" >&2
    exit 1
}

# The shell owns the bounded job table and the background wrapper must not
# drive the terminal foreground.
for shell_part in 'struct sh_job' 'SH_JOBS_MAX' 'sh_spawn_background' \
                  'sh_job_reap_nohang' 'sh_job_wait' 'HFOC_JOB_CONTROL=0' \
                  'hfoc_tty_foreground' 'SFOC_SPAWN_PGID'; do
    grep -Fq -- "${shell_part/SFOC/HFOC}" user/bin/sh.c || {
        echo "ITEM-633: shell не несёт:" "$shell_part" >&2
        exit 1
    }
done
for builtin in '"jobs"' '"fg"' '"bg"' '"wait"'; do
    grep -Fq -- "$builtin" user/bin/sh.c || {
        echo "ITEM-633: нет builtin $builtin" >&2
        exit 1
    }
done

[[ -f user/bin/sleep.c ]] || {
    echo "ITEM-633: нет native sleep" >&2
    exit 1
}
grep -Fq 'sleep' Makefile || {
    echo "ITEM-633: sleep не собран" >&2
    exit 1
}
grep -Fq 'if [ "$$program" = sleep ]; then continue; fi' Makefile || {
    echo "ITEM-633: sleep не исключён из тесного user-init rootfs" >&2
    exit 1
}

[[ -f scripts/test-ssh-jobs.py ]] || {
    echo "ITEM-633: нет SSH job-control proof" >&2
    exit 1
}
for marker in 'sleep 2 &' 'Ctrl-Z' 'job-done' 'Stopped' 'Running'; do
    grep -Fq -- "$marker" scripts/test-ssh-jobs.py || {
        echo "ITEM-633: proof не проверяет:" "$marker" >&2
        exit 1
    }
done
grep -Fq 'scripts/test-ssh-jobs.py' scripts/test-qemu-ssh.sh || {
    echo "ITEM-633: SSH proof не запускается" >&2
    exit 1
}
grep -Fq 'SSH shell/job-control proof' scripts/test-ssh-jobs.py || {
    echo "ITEM-633: proof не публикует вердикт" >&2
    exit 1
}
