#include "syscall.h"

#include "process.h"

extern uint64_t vmm_hhdm_offset(void);
#if !defined(HFOC_SYSCALL_HOST_TEST) && !defined(HFOC_SYSCALL_ORIGIN_HOST_TEST)
#include "descriptor.h"
#include "scheduler.h"
#include "tty.h"
#include "user_copy.h"
#endif
#if defined(HFOC_SYSCALL_HOST_TEST) || defined(HFOC_SYSCALL_ORIGIN_HOST_TEST)
/* The focused host proofs do not link the VFS/descriptor owners; the file
   calls are not part of what they exercise. */
static bool syscall_file_number(uint64_t number) { (void)number; return false; }
static uint64_t syscall_file_handle(const struct syscall_origin *origin,
                                    uint64_t number, uint64_t arg0,
                                    uint64_t arg1, uint64_t arg2)
{
    (void)origin; (void)number; (void)arg0; (void)arg1; (void)arg2;
    return SYSCALL_RET_ENOSYS;
}
static bool syscall_exec_number(uint64_t number) { (void)number; return false; }
static uint64_t syscall_exec_handle(const struct syscall_origin *origin,
                                    uint64_t arg0, uint64_t arg1,
                                    uint64_t arg2)
{
    (void)origin; (void)arg0; (void)arg1; (void)arg2;
    return SYSCALL_RET_ENOSYS;
}
static uint64_t syscall_spawn_handle(const struct syscall_origin *origin,
                                     uint64_t arg0)
{
    (void)origin; (void)arg0;
    return SYSCALL_RET_ENOSYS;
}
static uint64_t syscall_wait_handle(const struct syscall_origin *origin,
                                    uint64_t arg0, uint64_t arg1)
{
    (void)origin; (void)arg0; (void)arg1;
    return SYSCALL_RET_ENOSYS;
}
#else
#include "syscall_exec.h"
#include "syscall_file.h"
#endif
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include "serial.h"

#if defined(HFOC_USER_INIT_QEMU_TEST)
#include "audit.h"
#include "event_ring.h"
#include "process.h"
#include "scheduler.h"
#include "serial.h"
#include "user_init.h"
#endif

#ifdef HFOC_SYSCALL_HOST_TEST
uint64_t host_rdmsr(uint32_t msr);
void host_wrmsr(uint32_t msr, uint64_t value);
#define rdmsr(msr)       host_rdmsr(msr)
#define wrmsr(msr, val)  host_wrmsr(msr, val)
#else
static uint64_t rdmsr(uint32_t msr) {
    uint32_t low;
    uint32_t high;
    __asm__ volatile ("rdmsr" : "=a"(low), "=d"(high) : "c"(msr));
    return ((uint64_t)high << 32U) | low;
}

static void wrmsr(uint32_t msr, uint64_t value) {
    const uint32_t low = (uint32_t)(value & UINT32_MAX);
    const uint32_t high = (uint32_t)(value >> 32U);
    __asm__ volatile ("wrmsr" : : "a"(low), "d"(high), "c"(msr));
}
#endif

static uint64_t saved_efer;
static uint64_t saved_star;
static uint64_t saved_lstar;
static uint64_t saved_fmask;
static bool saved;
static bool published;

static bool syscall_msr_values_exact(void) {
    return (rdmsr(IA32_EFER_MSR) & IA32_EFER_SCE) == IA32_EFER_SCE &&
           rdmsr(IA32_STAR_MSR) == SYSCALL_STAR_VALUE &&
           rdmsr(IA32_LSTAR_MSR) == (uint64_t)(uintptr_t)&syscall_entry &&
           rdmsr(IA32_FMASK_MSR) == SYSCALL_FMASK;
}

static enum syscall_abi_status restore_msrs(void) {
    bool restored;

    if (!saved) {
        return SYSCALL_ABI_NOT_PUBLISHED;
    }
    published = false;
    wrmsr(IA32_EFER_MSR, saved_efer);
    wrmsr(IA32_STAR_MSR, saved_star);
    wrmsr(IA32_LSTAR_MSR, saved_lstar);
    wrmsr(IA32_FMASK_MSR, saved_fmask);
    restored = rdmsr(IA32_EFER_MSR) == saved_efer &&
               rdmsr(IA32_STAR_MSR) == saved_star &&
               rdmsr(IA32_LSTAR_MSR) == saved_lstar &&
               rdmsr(IA32_FMASK_MSR) == saved_fmask;
    saved = false;
    return restored ? SYSCALL_ABI_OK : SYSCALL_ABI_RESTORE_FAILED;
}

enum syscall_abi_status syscall_msr_prepare(void) {
    if (published) {
        return SYSCALL_ABI_ALREADY_PUBLISHED;
    }

    saved_efer = rdmsr(IA32_EFER_MSR);
    saved_star = rdmsr(IA32_STAR_MSR);
    saved_lstar = rdmsr(IA32_LSTAR_MSR);
    saved_fmask = rdmsr(IA32_FMASK_MSR);
    saved = true;

    wrmsr(IA32_EFER_MSR, saved_efer | IA32_EFER_SCE);
    wrmsr(IA32_STAR_MSR, SYSCALL_STAR_VALUE);
    wrmsr(IA32_LSTAR_MSR, (uint64_t)(uintptr_t)&syscall_entry);
    wrmsr(IA32_FMASK_MSR, SYSCALL_FMASK);

    if (!syscall_msr_values_exact()) {
        const enum syscall_abi_status restore_status = restore_msrs();
        return restore_status == SYSCALL_ABI_OK
            ? SYSCALL_ABI_READBACK_MISMATCH
            : SYSCALL_ABI_RESTORE_FAILED;
    }
    published = true;
    return SYSCALL_ABI_OK;
}

enum syscall_abi_status syscall_msr_verify(void) {
    if (!published) {
        return SYSCALL_ABI_NOT_PUBLISHED;
    }
    return syscall_msr_values_exact()
        ? SYSCALL_ABI_OK
        : SYSCALL_ABI_READBACK_MISMATCH;
}

enum syscall_abi_status syscall_msr_restore(void) {
    return restore_msrs();
}

bool syscall_msr_published(void) {
    return published;
}

static bool syscall_user_range_ok(uint64_t address, uint64_t length) {
    if (address == 0U || length == 0U) {
        return false;
    }
#ifdef HFOC_SYSCALL_HOST_TEST
    if (address > VMM_USER_STACK_TOP ||
        length > VMM_USER_STACK_TOP - address) {
        return false;
    }
#else
    if (address < VMM_USER_IMAGE_BASE ||
        address > VMM_USER_STACK_TOP ||
        length > VMM_USER_STACK_TOP - address) {
        return false;
    }
#endif
    return true;
}

enum syscall_origin_status syscall_origin_capture(
    const struct process_control_block *pcb,
    struct syscall_origin *origin)
{
    if (pcb == NULL || origin == NULL) {
        return SYSCALL_ORIGIN_NULL;
    }
    if (!pcb->published) {
        return SYSCALL_ORIGIN_BAD_PCB;
    }
    origin->pcb = pcb;
    origin->domain = pcb->security_domain;
    origin->generation = pcb->security_generation;
    return SYSCALL_ORIGIN_OK;
}

enum syscall_origin_status syscall_origin_validate(
    const struct syscall_origin *origin)
{
    if (origin == NULL) {
        return SYSCALL_ORIGIN_NULL;
    }
    if (origin->pcb == NULL || !origin->pcb->published) {
        return SYSCALL_ORIGIN_BAD_PCB;
    }
    if (origin->domain != origin->pcb->security_domain) {
        return SYSCALL_ORIGIN_DOMAIN_MISMATCH;
    }
    if (origin->generation != origin->pcb->security_generation) {
        return SYSCALL_ORIGIN_STALE;
    }
    return SYSCALL_ORIGIN_OK;
}

const char *syscall_origin_status_string(enum syscall_origin_status status) {
    switch (status) {
    case SYSCALL_ORIGIN_OK:
        return "SYSCALL_ORIGIN_OK";
    case SYSCALL_ORIGIN_NULL:
        return "SYSCALL_ORIGIN_NULL";
    case SYSCALL_ORIGIN_BAD_PCB:
        return "SYSCALL_ORIGIN_BAD_PCB";
    case SYSCALL_ORIGIN_DOMAIN_MISMATCH:
        return "SYSCALL_ORIGIN_DOMAIN_MISMATCH";
    case SYSCALL_ORIGIN_STALE:
        return "SYSCALL_ORIGIN_STALE";
    default:
        return "SYSCALL_ORIGIN_UNKNOWN";
    }
}

bool syscall_requires_capability(uint64_t number, uint32_t *type_out) {
    if (type_out == NULL) {
        return false;
    }
    switch (number) {
    case SYSCALL_WRITE:
        *type_out = CAPABILITY_TYPE_SYSCALL_WRITE;
        return true;
    case SYSCALL_YIELD:
        *type_out = CAPABILITY_TYPE_SYSCALL_YIELD;
        return true;
    case SYSCALL_EXIT:
        *type_out = CAPABILITY_TYPE_SYSCALL_EXIT;
        return true;
    case SYSCALL_EXEC:
        /* Native exec is a file-backed operation; the fd owner still checks
           the path and the process owner still checks its own state. */
        *type_out = CAPABILITY_TYPE_SYSCALL_FILE;
        return true;
    case SYSCALL_SPAWN:
    case SYSCALL_WAIT:
    case SYSCALL_KILL:
    case SYSCALL_WAIT2:
    case SYSCALL_SETPGID:
    case SYSCALL_TTY_FOREGROUND:
    case SYSCALL_SLEEP:
        /* ITEM-628/633: child and terminal operations need the file
           capability; no new capability type is invented. */
        *type_out = CAPABILITY_TYPE_SYSCALL_FILE;
        return true;
    default:
        if (syscall_file_number(number)) {
            *type_out = CAPABILITY_TYPE_SYSCALL_FILE;
            return true;
        }
        return false;
    }
}

#ifdef HFOC_USER_INIT_QEMU_TEST
static void audit_qemu_append_syscall(const struct syscall_origin *origin,
                                      uint64_t number,
                                      uint64_t result)
{
    struct event_ring *ring = kernel_event_ring_get();
    struct audit_record record;
    enum audit_status status;

    if (origin == NULL || origin->pcb == NULL || ring == NULL ||
        !event_ring_is_published(ring)) {
        serial_write("[AUDIT-OBS] append unavailable\n");
        return;
    }
    status = audit_record_init(&record, AUDIT_RECORD_SYSCALL,
                               (uint32_t)number, (uint32_t)result,
                               origin->pcb->pid, origin->domain,
                               origin->generation, 0U);
    if (status != AUDIT_OK) {
        serial_write("[AUDIT-OBS] append init failed\n");
        return;
    }
    status = audit_record_append(ring, &record, NULL);
    if (status != AUDIT_OK) {
        {
            const uint64_t serial_flags = serial_sequence_begin();
            serial_write("[AUDIT-OBS] append failed: ");
            serial_write(audit_status_string(status));
            serial_write("\n");
            serial_sequence_end(serial_flags);
        }
        return;
    }
    {
        const uint64_t serial_flags = serial_sequence_begin();
        serial_write("[AUDIT-OBS] published operation=");
        serial_write_u64(number);
        serial_write(" result=");
        serial_write_u64(result);
        serial_write("\n");
        serial_sequence_end(serial_flags);
    }
}
#endif

static uint64_t syscall_dispatch(const struct syscall_origin *origin,
                                 uint64_t number,
                                 uint64_t arg0,
                                 uint64_t arg1,
                                 uint64_t arg2)
{
    (void)origin;

    switch (number) {
    case SYSCALL_WRITE: {
        char buffer[SYSCALL_WRITE_MAX];
        if (arg0 != 1U || arg2 == 0U || arg2 > SYSCALL_WRITE_MAX) {
            return SYSCALL_RET_EINVAL;
        }
        if (!syscall_user_range_ok(arg1, arg2)) {
            return SYSCALL_RET_EFAULT;
        }
        (void)memcpy(buffer, (const void *)(uintptr_t)arg1,
                     (size_t)arg2);
#if defined(HFOC_USER_INIT_QEMU_TEST)
        if (arg2 == 6U) {
            uint16_t cs_value;
            uint32_t magic_value;
            memcpy(&cs_value, buffer, sizeof(cs_value));
            memcpy(&magic_value, buffer + 2U, sizeof(magic_value));
            if (cs_value == USER_CS_SELECTOR &&
                magic_value == USER_INIT_MAGIC) {
                {
                    const uint64_t serial_flags = serial_sequence_begin();
                    serial_write("[USER-INIT] ring3 marker OK cs=");
                    serial_write_hex(cs_value);
                    serial_write(" magic=");
                    serial_write_hex(magic_value);
                    serial_write("\n");
                    serial_sequence_end(serial_flags);
                }
            }
        }
        {
            const uint64_t serial_flags = serial_sequence_begin();
            serial_write("[USER-INIT] write ok len=");
            serial_write_u64(arg2);
            serial_write("\n");
            serial_sequence_end(serial_flags);
        }
#endif
#ifdef HFOC_USER_INIT_QEMU_TEST
        audit_qemu_append_syscall(origin, number, arg2);
#endif
        return arg2;
    }
    case SYSCALL_YIELD: {
        if (arg0 != 0U || arg1 != 0U || arg2 != 0U) {
            return SYSCALL_RET_EINVAL;
        }
#if defined(HFOC_USER_INIT_QEMU_TEST)
        serial_write("[USER-INIT] yield syscall\n");
        const enum scheduler_status yield_status = sched_yield();
        if (yield_status != SCHEDULER_OK) {
            serial_write("[USER-INIT] failed: yield\n");
            return SYSCALL_RET_EINVAL;
        }
        serial_write("[USER-INIT] yield ok\n");
#endif
#ifdef HFOC_USER_INIT_QEMU_TEST
        audit_qemu_append_syscall(origin, number, SYSCALL_RET_OK);
#endif
        return SYSCALL_RET_OK;
    }
    case SYSCALL_EXIT: {
        /*
         * ITEM-626: the status is the low byte of arg0, so external programs
         * return real exit codes (false=1, shell 126/127).  The legacy test
         * calls passed zero and keep their exact meaning.
         */
        if (arg0 > 255U || arg1 != 0U || arg2 != 0U) {
            return SYSCALL_RET_EINVAL;
        }
#if defined(HFOC_SYSCALL_HOST_TEST) || \
    defined(HFOC_SYSCALL_ORIGIN_HOST_TEST) || \
    defined(HFOC_SYSCALL_QEMU_TEST)
        /*
         * The focused ABI proof must observe the return value of EXIT and
         * continue to its marker check; the real exit path is proven by the
         * user-init matrix and the external programs.
         */
        return SYSCALL_RET_OK;
#else
        struct scheduler_task *task = NULL;
        struct kernel_process *process = NULL;
        struct address_space kernel_as;
        enum process_status exit_status;
        enum scheduler_status scheduler_status;

        if (sched_current(&task) == SCHEDULER_OK && task != NULL &&
            task->process_owner != NULL) {
            process = (struct kernel_process *)task->process_owner;
        }
        if (process == NULL) {
#if defined(HFOC_USER_INIT_QEMU_TEST)
            serial_write("[USER-INIT] failed: no process\n");
#endif
            return SYSCALL_RET_EINVAL;
        }
#if defined(HFOC_USER_INIT_QEMU_TEST)
        {
            const uint64_t serial_flags = serial_sequence_begin();
            serial_write("[USER-INIT] exit syscall pid=");
            serial_write_u64(process->pcb.pid);
            serial_write(" status=");
            serial_write_u64(arg0);
            serial_write("\n");
            serial_sequence_end(serial_flags);
        }
        audit_qemu_append_syscall(origin, number, SYSCALL_RET_OK);
#endif
        if (vmm_kernel_address_space() == NULL) {
#if defined(HFOC_USER_INIT_QEMU_TEST)
            serial_write("[USER-INIT] failed: no kernel address space\n");
#endif
            return SYSCALL_RET_EINVAL;
        }
        kernel_as = *vmm_kernel_address_space();
        if (vmm_activate_address_space(&kernel_as) != VMM_OK) {
#if defined(HFOC_USER_INIT_QEMU_TEST)
            serial_write("[USER-INIT] failed: address space restore\n");
#endif
            return SYSCALL_RET_EINVAL;
        }
        exit_status = process_exit_kernel(process, (uint32_t)arg0);
        if (exit_status != PROCESS_OK) {
#if defined(HFOC_USER_INIT_QEMU_TEST)
            serial_write("[USER-INIT] failed: process exit\n");
#endif
            return SYSCALL_RET_EINVAL;
        }
        scheduler_status = sched_exit();
#if defined(HFOC_USER_INIT_QEMU_TEST)
        {
            const uint64_t serial_flags = serial_sequence_begin();
            serial_write("[USER-INIT] failed: scheduler exit ");
            serial_write(scheduler_status_string(scheduler_status));
            serial_write("\n");
            serial_sequence_end(serial_flags);
        }
#else
        (void)scheduler_status;
#endif
        return SYSCALL_RET_EINVAL;
#endif
    }
    case SYSCALL_SLEEP:
#if defined(HFOC_SYSCALL_HOST_TEST) || defined(HFOC_SYSCALL_ORIGIN_HOST_TEST)
        return SYSCALL_RET_ENOSYS;
#else
        if (arg0 == 0U || arg0 > 100000U || arg1 != 0U || arg2 != 0U) {
            return SYSCALL_RET_EINVAL;
        }
        return sched_sleep(arg0) == SCHEDULER_OK ? SYSCALL_RET_OK
                                                 : SYSCALL_RET_EAGAIN;
#endif
    case SYSCALL_KILL:
#if defined(HFOC_SYSCALL_HOST_TEST) || defined(HFOC_SYSCALL_ORIGIN_HOST_TEST)
        return SYSCALL_RET_ENOSYS;
#else
    {
        enum process_status status;
        struct kernel_process *caller;

        if (origin == NULL || origin->pcb == NULL || arg1 == 0U ||
            arg1 > PROCESS_SIGNAL_MAX ||
            (arg2 != 0U && arg2 != SYSCALL_KILL_GROUP)) {
            return SYSCALL_RET_EINVAL;
        }
        caller = process_find_pid(origin->pcb->pid);
        if (caller == NULL) {
            return SYSCALL_RET_EPERM;
        }
        if (arg2 == SYSCALL_KILL_GROUP) {
            uint32_t probe;

            /* A group signal is confined to the caller's session. */
            for (probe = 1U; probe <= PROCESS_MAX_PIDS; ++probe) {
                struct kernel_process *member = process_find_pid(probe);

                if (member != NULL &&
                    member->pcb.pgid == (uint32_t)arg0 &&
                    member->pcb.session_id != caller->pcb.session_id) {
                    return SYSCALL_RET_EPERM;
                }
            }
            status = process_signal_group((uint32_t)arg0, (uint32_t)arg1);
        } else {
            struct kernel_process *target =
                process_find_pid((uint32_t)arg0);

            if (target == NULL) {
                return SYSCALL_RET_ESRCH;
            }
            if (target->pcb.session_id != caller->pcb.session_id) {
                return SYSCALL_RET_EPERM;
            }
            status = process_signal_kernel(target, (uint32_t)arg1);
        }
        if (status == PROCESS_OK) {
            return SYSCALL_RET_OK;
        }
        if (status == PROCESS_BAD_PID) {
            return SYSCALL_RET_ESRCH;
        }
        if (status == PROCESS_SELF_SIGNAL) {
            return SYSCALL_RET_EPERM;
        }
        return SYSCALL_RET_EINVAL;
    }
#endif
    case SYSCALL_WAIT2:
#if defined(HFOC_SYSCALL_HOST_TEST) || defined(HFOC_SYSCALL_ORIGIN_HOST_TEST)
        return SYSCALL_RET_ENOSYS;
#else
    {
        struct kernel_process *caller;
        uint32_t result[2] = { 0U, 0U };
        enum process_status status;

        if (origin == NULL || origin->pcb == NULL ||
            (arg0 & ~(PROCESS_WAIT_NOHANG | PROCESS_WAIT_UNTRACED |
                      PROCESS_WAIT_CONTINUED)) != 0U ||
            user_copy_check(arg1, sizeof(result), true) != USER_COPY_OK) {
            return SYSCALL_RET_EINVAL;
        }
        caller = process_find_pid(origin->pcb->pid);
        if (caller == NULL) {
            return SYSCALL_RET_EPERM;
        }
        for (;;) {
            status = process_wait2_kernel(
                &caller->pcb, (uint32_t)arg0, &result[0], &result[1],
                vmm_hhdm_offset());
            if (status != PROCESS_NOT_EXITED) {
                break;
            }
            if ((arg0 & PROCESS_WAIT_NOHANG) != 0U) {
                return SYSCALL_RET_EAGAIN;
            }
            /* Poll with interrupts enabled: the syscall entry masks IF, and
               a child sleeping on a timeout needs the PIT to fire before it
               can become runnable again. */
            __asm__ volatile ("sti" : : : "memory");
            (void)sched_yield();
            __asm__ volatile ("cli" : : : "memory");
        }
        if (status == PROCESS_NO_CHILDREN) {
            return SYSCALL_RET_ECHILD;
        }
        if (status != PROCESS_OK) {
            return SYSCALL_RET_EINVAL;
        }
        if (user_copy_out(arg1, result, sizeof(result)) != USER_COPY_OK) {
            return SYSCALL_RET_EFAULT;
        }
        return SYSCALL_RET_OK;
    }
#endif
    case SYSCALL_SETPGID:
#if defined(HFOC_SYSCALL_HOST_TEST) || defined(HFOC_SYSCALL_ORIGIN_HOST_TEST)
        return SYSCALL_RET_ENOSYS;
#else
    {
        struct kernel_process *caller;
        struct kernel_process *target;
        enum process_status status;

        if (origin == NULL || origin->pcb == NULL) {
            return SYSCALL_RET_EPERM;
        }
        caller = process_find_pid(origin->pcb->pid);
        if (caller == NULL) {
            return SYSCALL_RET_EPERM;
        }
        target = arg0 == 0U ? caller : process_find_pid((uint32_t)arg0);
        if (target == NULL) {
            return SYSCALL_RET_ESRCH;
        }
        if (target->pcb.session_id != caller->pcb.session_id) {
            return SYSCALL_RET_EPERM;
        }
        status = process_setpgid_kernel(target, (uint32_t)arg1);
        return status == PROCESS_OK ? SYSCALL_RET_OK : SYSCALL_RET_EINVAL;
    }
#endif
    case SYSCALL_TTY_FOREGROUND:
#if defined(HFOC_SYSCALL_HOST_TEST) || defined(HFOC_SYSCALL_ORIGIN_HOST_TEST)
        return SYSCALL_RET_ENOSYS;
#else
    {
        struct kernel_process *caller;
        struct file_description *description = NULL;
        struct tty *tty;
        uint32_t pgid = (uint32_t)arg0;

        if (origin == NULL || origin->pcb == NULL || arg1 != 0U ||
            arg2 != 0U) {
            return SYSCALL_RET_EINVAL;
        }
        caller = process_find_pid(origin->pcb->pid);
        if (caller == NULL) {
            return SYSCALL_RET_EPERM;
        }
        /* Only the session leader (the owner shell) drives the terminal. */
        if (caller->pcb.pid != caller->pcb.session_id) {
            return SYSCALL_RET_EPERM;
        }
        if (descriptor_table_lookup(&caller->descriptor_table, 0U,
                                    &description) != DESCRIPTOR_OK ||
            !tty_description_is_tty(description)) {
            return SYSCALL_RET_EBADF;
        }
        if (pgid != 0U) {
            struct kernel_process *leader = process_find_pid(pgid);

            if (leader == NULL ||
                leader->pcb.session_id != caller->pcb.session_id) {
                return SYSCALL_RET_ESRCH;
            }
        }
        tty = (struct tty *)description->context;
        tty_set_foreground(tty, pgid);
        return SYSCALL_RET_OK;
    }
#endif
    default:
        if (syscall_file_number(number)) {
            return syscall_file_handle(origin, number, arg0, arg1, arg2);
        }
        if (syscall_exec_number(number)) {
            if (number == SYSCALL_SPAWN) {
                return syscall_spawn_handle(origin, arg0);
            }
            if (number == SYSCALL_WAIT) {
                return syscall_wait_handle(origin, arg0, arg1);
            }
            return syscall_exec_handle(origin, arg0, arg1, arg2);
        }
        return SYSCALL_RET_ENOSYS;
    }
}

uint64_t syscall_handle_checked(const struct syscall_origin *origin,
                                uint64_t number,
                                uint64_t arg0,
                                uint64_t arg1,
                                uint64_t arg2)
{
    uint32_t required_type = 0U;
    struct process_control_block *pcb;

    if (syscall_origin_validate(origin) != SYSCALL_ORIGIN_OK) {
        return SYSCALL_RET_EPERM;
    }
    pcb = (struct process_control_block *)(uintptr_t)origin->pcb;
    /*
     * ITEM-626: the native runtime reads the error of the previous call.  The
     * query itself must not clear the value it is about to report.
     */
    if (number == SYSCALL_RESULT) {
        return pcb->syscall_last_error;
    }
    pcb->syscall_last_error = 0U;
    if (syscall_requires_capability(number, &required_type) &&
        !capability_table_has(&origin->pcb->capability_table,
                              required_type)) {
        return SYSCALL_RET_EPERM;
    }
    return syscall_dispatch(origin, number, arg0, arg1, arg2);
}

uint64_t syscall_handle(uint64_t number,
                        uint64_t arg0,
                        uint64_t arg1,
                        uint64_t arg2)
{
#if !defined(HFOC_SYSCALL_HOST_TEST) && \
    !defined(HFOC_SYSCALL_ORIGIN_HOST_TEST)
    struct scheduler_task *task = NULL;

    /*
     * ITEM-625: capture the calling PCB in every build, not only in the QEMU
     * proof.  A user process must never be dispatched through the kernel-only
     * path; the extra serial line stays limited to the proof build.
     */
    if (sched_current(&task) == SCHEDULER_OK && task != NULL &&
        task->process_owner != NULL) {
        const struct kernel_process *process =
            (const struct kernel_process *)task->process_owner;
        struct syscall_origin origin;
        const enum syscall_origin_status origin_status =
            syscall_origin_capture(&process->pcb, &origin);

        if (origin_status != SYSCALL_ORIGIN_OK) {
#if defined(HFOC_USER_INIT_QEMU_TEST)
            serial_write("[SECURITY] failed: origin capture\n");
#endif
            return SYSCALL_RET_EPERM;
        }
#if defined(HFOC_USER_INIT_QEMU_TEST)
        {
            const uint64_t serial_flags = serial_sequence_begin();
            serial_write("[SECURITY] syscall origin validated pid=");
            serial_write_u64(process->pcb.pid);
            serial_write(" domain=");
            serial_write_u64(process->pcb.security_domain);
            serial_write("\n");
            serial_sequence_end(serial_flags);
        }
#endif
        return syscall_handle_checked(&origin, number, arg0, arg1, arg2);
    }
#endif
    return syscall_dispatch(NULL, number, arg0, arg1, arg2);
}

const char *syscall_abi_status_string(enum syscall_abi_status status) {
    switch (status) {
    case SYSCALL_ABI_OK:
        return "SYSCALL_ABI_OK";
    case SYSCALL_ABI_NOT_PUBLISHED:
        return "SYSCALL_ABI_NOT_PUBLISHED";
    case SYSCALL_ABI_ALREADY_PUBLISHED:
        return "SYSCALL_ABI_ALREADY_PUBLISHED";
    case SYSCALL_ABI_READBACK_MISMATCH:
        return "SYSCALL_ABI_READBACK_MISMATCH";
    case SYSCALL_ABI_RESTORE_FAILED:
        return "SYSCALL_ABI_RESTORE_FAILED";
    default:
        return "SYSCALL_ABI_UNKNOWN";
    }
}
