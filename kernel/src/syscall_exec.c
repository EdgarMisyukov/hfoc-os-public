/*
 * ITEM-625: native exec over the user-process image owner.
 *
 * The whole new image is loaded, parsed, mapped into a fresh address space and
 * given its argv/env stack before anything is committed.  Only then does the
 * process switch address spaces; a failed load or mapping leaves the old
 * image, descriptors and capabilities untouched.
 */
#include "syscall_exec.h"

#include "process.h"
#include "scheduler.h"
#include "user_copy.h"
#include "user_process.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#if defined(HFOC_USER_INIT_QEMU_TEST)
#include "serial.h"
#endif

#define SYSCALL_EXEC_PATH_MAX SYSCALL_PATH_MAX
#define SYSCALL_EXEC_ARG_BYTES 8192U
#define SYSCALL_EXEC_ARGV_MAX 64U
#define SYSCALL_EXEC_ENVP_MAX 32U

/* syscall_entry.S saves the interrupted user context in these slots. */
/* ITEM-627: the entry frame lives on the calling task's kernel stack. */
struct syscall_entry_frame {
    uint64_t arg2;
    uint64_t arg1;
    uint64_t arg0;
    uint64_t rflags;
    uint64_t rip;
    uint64_t user_rsp;
};
extern struct syscall_entry_frame *syscall_frame_current;
/* The kernel-wide direct map offset is owned by kernel.c/vmm.c. */
extern uint64_t vmm_hhdm_offset(void);

_Static_assert(offsetof(struct kernel_process, pcb) == 0U,
               "the syscall origin must name the process control block first");

static struct kernel_process *syscall_exec_process(
    const struct syscall_origin *origin)
{
    if (origin == NULL || origin->pcb == NULL || !origin->pcb->published ||
        origin->pcb->security_domain != PROCESS_SECURITY_DOMAIN_USER) {
        return NULL;
    }
    return (struct kernel_process *)(void *)origin->pcb;
}

static uint64_t syscall_exec_error(enum user_process_status status)
{
    switch (status) {
    case USER_PROCESS_OK:
        return SYSCALL_RET_OK;
    case USER_PROCESS_NOT_FOUND:
    case USER_PROCESS_NO_VFS:
        return SYSCALL_RET_ENOENT;
    case USER_PROCESS_TOO_LARGE:
    case USER_PROCESS_BAD_ELF:
    case USER_PROCESS_UNSUPPORTED_LAYOUT:
    case USER_PROCESS_BAD_STACK:
        return SYSCALL_RET_EINVAL;
    case USER_PROCESS_LIMIT_EXCEEDED:
        return SYSCALL_RET_ENOSPC;
    case USER_PROCESS_BAD_STATE:
        return SYSCALL_RET_EPERM;
    case USER_PROCESS_NULL:
    case USER_PROCESS_CREATE_FAILED:
    case USER_PROCESS_SPAWN_FAILED:
    default:
        return SYSCALL_RET_EIO;
    }
}

/*
 * Copy one NUL-terminated string byte by byte.  Reading one validated byte at
 * a time means a string that ends just below the top of its mapping does not
 * fail merely because a fixed capacity window would cross the end.
 */
static bool syscall_exec_copy_string(char *destination, size_t capacity,
                                     uint64_t source, size_t *length_out)
{
    size_t index = 0U;

    if (destination == NULL || capacity == 0U) {
        return false;
    }
    while (index + 1U < capacity) {
        uint8_t byte = 0U;

        if (user_copy_in(&byte, source + index, 1U) != USER_COPY_OK) {
            return false;
        }
        destination[index] = (char)byte;
        if (byte == 0U) {
            if (length_out != NULL) {
                *length_out = index;
            }
            return true;
        }
        ++index;
    }
    return false;
}

static bool syscall_exec_copy_vector(
    uint64_t user_vector, const char **vector_out, uint32_t vector_max,
    char *storage, size_t storage_capacity, size_t *storage_used,
    uint32_t *count_out)
{
    uint32_t index;

    if (vector_out == NULL || count_out == NULL || storage_used == NULL) {
        return false;
    }
    if (user_vector == 0U) {
        vector_out[0] = NULL;
        *count_out = 0U;
        return true;
    }
    /* The whole pointer vector must be addressable without wrapping. */
    if (user_vector >
        UINT64_MAX - ((uint64_t)vector_max + 1U) * sizeof(uint64_t)) {
        return false;
    }
    for (index = 0U; index < vector_max; ++index) {
        uint64_t pointer = 0U;
        size_t length = 0U;

        if (user_copy_in(&pointer, user_vector + (uint64_t)index * 8U,
                         sizeof(pointer)) != USER_COPY_OK) {
            return false;
        }
        if (pointer == 0U) {
            vector_out[index] = NULL;
            *count_out = index;
            return true;
        }
        if (storage_used == NULL ||
            *storage_used >= storage_capacity) {
            return false;
        }
        if (!syscall_exec_copy_string(storage + *storage_used,
                                      storage_capacity - *storage_used,
                                      pointer, &length)) {
            return false;
        }
        vector_out[index] = storage + *storage_used;
        *storage_used += length + 1U;
    }
    return false;
}

uint64_t syscall_exec_handle(const struct syscall_origin *origin,
                             uint64_t arg0, uint64_t arg1, uint64_t arg2)
{
    /*
     * SYSCALL runs on a 4 KiB kernel entry stack, so the bounded argument
     * buffers live in .bss.  The kernel is cooperative and single-CPU; no
     * pointer to them survives the call.
     */
    static char path[SYSCALL_EXEC_PATH_MAX];
    static char argument_storage[SYSCALL_EXEC_ARG_BYTES];
    static const char *argv[SYSCALL_EXEC_ARGV_MAX + 1U];
    static const char *envp[SYSCALL_EXEC_ENVP_MAX + 1U];
    struct kernel_process *process;
    uint64_t entry = 0U;
    uint64_t stack_top = 0U;
    size_t path_length = 0U;
    size_t storage_used = 0U;
    uint32_t argv_count = 0U;
    uint32_t envp_count = 0U;
    enum user_process_status status;

    process = syscall_exec_process(origin);
    if (process == NULL) {
        return SYSCALL_RET_EPERM;
    }
    if (!syscall_exec_copy_string(path, sizeof(path), arg0, &path_length) ||
        path_length == 0U) {
        return SYSCALL_RET_EFAULT;
    }
    if (!syscall_exec_copy_vector(arg1, argv, SYSCALL_EXEC_ARGV_MAX,
                                  argument_storage, sizeof(argument_storage),
                                  &storage_used, &argv_count) ||
        !syscall_exec_copy_vector(arg2, envp, SYSCALL_EXEC_ENVP_MAX,
                                  argument_storage, sizeof(argument_storage),
                                  &storage_used, &envp_count)) {
        return SYSCALL_RET_EFAULT;
    }
    status = user_process_exec(process, path, argv, envp,
                               vmm_hhdm_offset(), &entry, &stack_top);
    (void)argv_count;
    (void)envp_count;
    if (status != USER_PROCESS_OK) {
#if defined(HFOC_USER_INIT_QEMU_TEST)
        {
            const uint64_t serial_flags = serial_sequence_begin();

            serial_write("[USER-EXEC] exec failed ");
            serial_write(user_process_status_string(status));
            serial_write("\n");
            serial_sequence_end(serial_flags);
        }
#endif
        {
            const uint64_t code = syscall_exec_error(status);
            struct process_control_block *const pcb =
                (struct process_control_block *)(uintptr_t)origin->pcb;

            pcb->syscall_last_error = (uint32_t)code;
            return code;
        }
    }
    /*
     * Retarget the architectural return to the new entry and stack.  The
     * descriptor table and capability table were never touched, so the new
     * image inherits them exactly as exec requires.
     */
    if (syscall_frame_current != NULL) {
        syscall_frame_current->rip = entry;
        syscall_frame_current->user_rsp = stack_top;
        syscall_frame_current->arg0 = 0U;
        syscall_frame_current->arg1 = 0U;
        syscall_frame_current->arg2 = 0U;
    }
#if defined(HFOC_USER_INIT_QEMU_TEST)
    {
        const uint64_t serial_flags = serial_sequence_begin();

        serial_write("[USER-EXEC] exec pid=");
        serial_write_u64(process->pcb.pid);
        serial_write(" entry=");
        serial_write_hex(entry);
        serial_write("\n");
        serial_sequence_end(serial_flags);
    }
#endif
    return SYSCALL_RET_OK;
}

bool syscall_exec_number(uint64_t number)
{
    return number == SYSCALL_EXEC || number == SYSCALL_SPAWN ||
           number == SYSCALL_WAIT;
}

/*
 * ITEM-628: SPAWN carries one request pointer because the syscall ABI has
 * three arguments.  Everything inside is bounded and copied through
 * user_copy; no user pointer reaches the process owner.
 */
uint64_t syscall_spawn_handle(const struct syscall_origin *origin,
                              uint64_t arg0)
{
    static char spawn_path[SYSCALL_EXEC_PATH_MAX];
    static char spawn_argument_storage[SYSCALL_EXEC_ARG_BYTES];
    static const char *spawn_argv[SYSCALL_EXEC_ARGV_MAX + 1U];
    static const char *spawn_envp[SYSCALL_EXEC_ENVP_MAX + 1U];
    static struct user_process_fd_action spawn_actions[
        USER_PROCESS_FD_ACTION_MAX];
    static struct syscall_spawn_request request;
    struct kernel_process *process;
    struct kernel_process *child = NULL;
    size_t path_length = 0U;
    size_t storage_used = 0U;
    uint32_t argv_count = 0U;
    uint32_t envp_count = 0U;
    uint32_t index;
    enum user_process_status status;

    process = syscall_exec_process(origin);
    if (process == NULL) {
        return SYSCALL_RET_EPERM;
    }
    if (user_copy_in(&request, arg0, sizeof(request)) != USER_COPY_OK ||
        request.reserved != 0U ||
        request.action_count > USER_PROCESS_FD_ACTION_MAX) {
        return SYSCALL_RET_EFAULT;
    }
    if (!syscall_exec_copy_string(spawn_path, sizeof(spawn_path),
                                  request.path, &path_length) ||
        path_length == 0U) {
        return SYSCALL_RET_EFAULT;
    }
    if (!syscall_exec_copy_vector(
            request.argv, spawn_argv, SYSCALL_EXEC_ARGV_MAX,
            spawn_argument_storage, sizeof(spawn_argument_storage),
            &storage_used, &argv_count) ||
        !syscall_exec_copy_vector(
            request.envp, spawn_envp, SYSCALL_EXEC_ENVP_MAX,
            spawn_argument_storage, sizeof(spawn_argument_storage),
            &storage_used, &envp_count)) {
        return SYSCALL_RET_EFAULT;
    }
    for (index = 0U; index < request.action_count; ++index) {
        const uint64_t source =
            request.actions + (uint64_t)index * sizeof(spawn_actions[0]);

        if (user_copy_in(&spawn_actions[index], source,
                         sizeof(spawn_actions[0])) != USER_COPY_OK) {
            return SYSCALL_RET_EFAULT;
        }
        if (spawn_actions[index].operation == SYSCALL_SPAWN_PGID) {
            if (spawn_actions[index].destination != 0U) {
                return SYSCALL_RET_EINVAL;
            }
            continue;
        }
        if (spawn_actions[index].operation == SYSCALL_SPAWN_CLOSE) {
            if (spawn_actions[index].destination >= 3U) {
                return SYSCALL_RET_EINVAL;
            }
            continue;
        }
        if (spawn_actions[index].destination >= 3U ||
            spawn_actions[index].operation < SYSCALL_SPAWN_OPEN ||
            spawn_actions[index].operation > SYSCALL_SPAWN_DUP) {
            return SYSCALL_RET_EINVAL;
        }
        if (spawn_actions[index].operation == SYSCALL_SPAWN_OPEN) {
            size_t path_bytes = 0U;

            while (path_bytes < sizeof(spawn_actions[index].path) &&
                   spawn_actions[index].path[path_bytes] != '\0') {
                ++path_bytes;
            }
            if (path_bytes == sizeof(spawn_actions[index].path)) {
                return SYSCALL_RET_EINVAL;
            }
        }
    }
    status = user_process_spawn_actions(
        &child, (struct process_control_block *)(uintptr_t)origin->pcb,
        spawn_path, spawn_argv, spawn_envp, spawn_actions,
        request.action_count, vmm_hhdm_offset());
    (void)argv_count;
    (void)envp_count;
    if (status != USER_PROCESS_OK) {
        const uint64_t code = syscall_exec_error(status);

        ((struct process_control_block *)(uintptr_t)origin->pcb)
            ->syscall_last_error = (uint32_t)code;
        return code;
    }
    ((struct process_control_block *)(uintptr_t)origin->pcb)
        ->syscall_last_error = 0U;
    return child->pcb.pid;
}

uint64_t syscall_wait_handle(const struct syscall_origin *origin,
                             uint64_t arg0, uint64_t arg1)
{
    struct process_control_block *pcb;
    enum process_status status;
    uint32_t result[SYSCALL_WAIT_RESULT_WORDS];

    (void)arg0;
    if (origin == NULL || origin->pcb == NULL ||
        user_copy_check(arg1, sizeof(result), true) != USER_COPY_OK) {
        return SYSCALL_RET_EFAULT;
    }
    pcb = (struct process_control_block *)(uintptr_t)origin->pcb;
    for (;;) {
        uint32_t pid = 0U;
        uint32_t exit_status = 0U;

        status = process_wait_kernel(pcb, &pid, &exit_status,
                                     vmm_hhdm_offset());
        if (status == PROCESS_OK) {
            result[0] = pid;
            result[1] = exit_status;
            if (user_copy_out(arg1, result, sizeof(result)) != USER_COPY_OK) {
                return SYSCALL_RET_EFAULT;
            }
            pcb->syscall_last_error = 0U;
            return pid;
        }
        if (status == PROCESS_NOT_EXITED) {
            /* The syscall entry masks interrupts; a child that is sleeping
               on a scheduler timeout needs the PIT to fire before it can be
               reaped, so poll with interrupts enabled. */
            __asm__ volatile ("sti" : : : "memory");
            (void)sched_yield();
            __asm__ volatile ("cli" : : : "memory");
            continue;
        }
        if (status == PROCESS_NO_CHILDREN) {
            pcb->syscall_last_error = SYSCALL_RET_ECHILD;
            return SYSCALL_RET_ECHILD;
        }
        pcb->syscall_last_error = SYSCALL_RET_EIO;
        return SYSCALL_RET_EIO;
    }
}
