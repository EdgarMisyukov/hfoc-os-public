/* ITEM-626: the native syscall wrappers.  See hfoc.h for the error rule. */
#include "hfoc.h"

long hfoc_argc;
char **hfoc_argv;
char **hfoc_envp;
char **environ;

long hfoc_raw_syscall3(long number, long arg0, long arg1, long arg2)
{
    long result;

    __asm__ volatile ("syscall"
                      : "=a"(result)
                      : "a"(number), "D"(arg0), "S"(arg1), "d"(arg2)
                      : "rcx", "r11", "r8", "r9", "r10", "memory");
    return result;
}

long hfoc_raw_syscall0(long number)
{
    return hfoc_raw_syscall3(number, 0L, 0L, 0L);
}

/*
 * A return of 0..20 can be either a legitimate small success (a byte count or
 * a descriptor) or one of the kernel's positive error codes.  The kernel keeps
 * the last error in the calling process, so only that ambiguous range pays for
 * the extra query.  Every other result is returned as it is.
 */
static long hfoc_call3(long number, long arg0, long arg1, long arg2)
{
    const long result = hfoc_raw_syscall3(number, arg0, arg1, arg2);

    if (result >= 0L && result <= 20L) {
        const long error = hfoc_raw_syscall0(HFOC_SYSCALL_RESULT);

        if (error != 0L) {
            return -error;
        }
    }
    return result;
}

long hfoc_write(int fd, const void *buffer, size_t length)
{
    return hfoc_call3(HFOC_SYSCALL_FILE_WRITE, fd, (long)(uintptr_t)buffer,
                      (long)length);
}

long hfoc_read(int fd, void *buffer, size_t length)
{
    return hfoc_call3(HFOC_SYSCALL_READ, fd, (long)(uintptr_t)buffer,
                      (long)length);
}

long hfoc_open(const char *path, uint32_t flags)
{
    return hfoc_call3(HFOC_SYSCALL_OPEN, (long)(uintptr_t)path, (long)flags, 0L);
}

long hfoc_close(int fd)
{
    return hfoc_call3(HFOC_SYSCALL_CLOSE, fd, 0L, 0L);
}

long hfoc_stat(const char *path, struct hfoc_stat *out)
{
    return hfoc_call3(HFOC_SYSCALL_STAT, (long)(uintptr_t)path,
                      (long)(uintptr_t)out, 0L);
}

long hfoc_getdents(const char *path, uint32_t index, struct hfoc_dirent *out)
{
    return hfoc_call3(HFOC_SYSCALL_GETDENTS, (long)(uintptr_t)path,
                      (long)index, (long)(uintptr_t)out);
}

long hfoc_lseek(int fd, long offset, uint32_t whence)
{
    return hfoc_call3(HFOC_SYSCALL_LSEEK, fd, offset, (long)whence);
}

long hfoc_chdir(const char *path)
{
    return hfoc_call3(HFOC_SYSCALL_CHDIR, (long)(uintptr_t)path, 0L, 0L);
}

long hfoc_getcwd(char *buffer, size_t capacity)
{
    return hfoc_call3(HFOC_SYSCALL_GETCWD, (long)(uintptr_t)buffer,
                      (long)capacity, 0L);
}

long hfoc_mkdir(const char *path)
{
    return hfoc_call3(HFOC_SYSCALL_MKDIR, (long)(uintptr_t)path, 0L, 0L);
}

long hfoc_unlink(const char *path)
{
    return hfoc_call3(HFOC_SYSCALL_UNLINK, (long)(uintptr_t)path, 0L, 0L);
}

long hfoc_rename(const char *old_path, const char *new_path)
{
    return hfoc_call3(HFOC_SYSCALL_RENAME, (long)(uintptr_t)old_path,
                      (long)(uintptr_t)new_path, 0L);
}

long hfoc_exec(const char *path, char *const *argv, char *const *envp)
{
    return hfoc_call3(HFOC_SYSCALL_EXEC, (long)(uintptr_t)path,
                      (long)(uintptr_t)argv, (long)(uintptr_t)envp);
}

long hfoc_spawn(const char *path, char *const *argv, char *const *envp,
                const struct hfoc_fd_action *actions, size_t action_count)
{
    struct hfoc_spawn_request request;

    if (action_count > HFOC_SPAWN_ACTION_MAX) {
        return HFOC_EINVAL;
    }
    request.path = (uint64_t)(uintptr_t)path;
    request.argv = (uint64_t)(uintptr_t)argv;
    request.envp = (uint64_t)(uintptr_t)envp;
    request.actions = (uint64_t)(uintptr_t)actions;
    request.action_count = (uint32_t)action_count;
    request.reserved = 0U;
    return hfoc_call3(HFOC_SYSCALL_SPAWN, (long)(uintptr_t)&request, 0L, 0L);
}

long hfoc_wait(int *status_out)
{
    uint32_t result[2] = { 0U, 0U };

    if (status_out == NULL) {
        return HFOC_EINVAL;
    }
    {
        const long pid = hfoc_call3(HFOC_SYSCALL_WAIT, 0L,
                                    (long)(uintptr_t)result, 0L);

        if (pid < 0L) {
            return pid;
        }
        *status_out = (int)result[1];
        return pid;
    }
}

long hfoc_pipe(int descriptors[2])
{
    return hfoc_call3(HFOC_SYSCALL_PIPE, (long)(uintptr_t)descriptors, 0L, 0L);
}

long hfoc_dup2(int old_descriptor, int new_descriptor)
{
    return hfoc_call3(HFOC_SYSCALL_DUP2, old_descriptor, new_descriptor, 0L);
}

long hfoc_yield(void)
{
    return hfoc_call3(HFOC_SYSCALL_YIELD, 0L, 0L, 0L);
}

__attribute__((noreturn)) void hfoc_exit(int status)
{
    (void)hfoc_raw_syscall3(HFOC_SYSCALL_EXIT, (long)(status & 0xFF), 0L, 0L);
    for (;;) {
        __asm__ volatile ("hlt" : : : "memory");
    }
}

const char *hfoc_strerror(long error)
{
    if (error > 0L) {
        error = -error;
    }
    switch (error) {
    case 0: return "success";
    case HFOC_EINVAL: return "invalid argument";
    case HFOC_EFAULT: return "bad address";
    case HFOC_ENOSYS: return "not implemented";
    case HFOC_EPERM: return "operation not permitted";
    case HFOC_ENOENT: return "no such file or directory";
    case HFOC_EACCES: return "permission denied";
    case HFOC_EBADF: return "bad descriptor";
    case HFOC_ENOSPC: return "no space left";
    case HFOC_EIO: return "input/output error";
    case HFOC_ENOTDIR: return "not a directory";
    case HFOC_EISDIR: return "is a directory";
    case HFOC_ENAMETOOLONG: return "name too long";
    case HFOC_EEXIST: return "already exists";
    case HFOC_ENOTEMPTY: return "directory not empty";
    case HFOC_EROFS: return "read-only filesystem";
    case HFOC_EMFILE: return "too many open files";
    case HFOC_ECHILD: return "no child process";
    case HFOC_EPIPE: return "broken pipe";
    case HFOC_EAGAIN: return "try again";
    default: return "unknown error";
    }
}

/* ITEM-633: job control over the appended native calls. */
long hfoc_kill(uint32_t pid_or_pgid, uint32_t signal, int group)
{
    return hfoc_call3(HFOC_SYSCALL_KILL, (long)pid_or_pgid, (long)signal,
                      group != 0 ? 1L : 0L);
}

long hfoc_wait2(uint32_t flags, uint32_t result[2])
{
    if (result == NULL) {
        return HFOC_EINVAL;
    }
    return hfoc_call3(HFOC_SYSCALL_WAIT2, (long)flags,
                      (long)(uintptr_t)result, 0L);
}

long hfoc_setpgid(uint32_t pid, uint32_t pgid)
{
    return hfoc_call3(HFOC_SYSCALL_SETPGID, (long)pid, (long)pgid, 0L);
}

long hfoc_tty_foreground(uint32_t pgid)
{
    return hfoc_call3(HFOC_SYSCALL_TTY_FOREGROUND, (long)pgid, 0L, 0L);
}

long hfoc_sleep(uint32_t ticks)
{
    return hfoc_call3(HFOC_SYSCALL_SLEEP, (long)ticks, 0L, 0L);
}
