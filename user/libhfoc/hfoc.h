#ifndef HFOC_USER_H
#define HFOC_USER_H

/*
 * ITEM-626: the native HFOC user ABI and runtime.
 *
 * This is not Linux and not POSIX.  The numbers, flags and error values below
 * are the kernel's published ABI (kernel/include/syscall.h); the kernel returns
 * the small positive error codes there, so the runtime asks SYSCALL_RESULT when
 * a call could be either a small success value or an error and reports errors
 * as negative values, matching the usual Unix convention inside the program.
 */

#include <stddef.h>
#include <stdint.h>

#define HFOC_SYSCALL_WRITE      0U
#define HFOC_SYSCALL_YIELD      1U
#define HFOC_SYSCALL_EXIT       2U
#define HFOC_SYSCALL_READ       3U
#define HFOC_SYSCALL_OPEN       4U
#define HFOC_SYSCALL_CLOSE      5U
#define HFOC_SYSCALL_STAT       6U
#define HFOC_SYSCALL_GETDENTS   7U
#define HFOC_SYSCALL_LSEEK      8U
#define HFOC_SYSCALL_CHDIR      9U
#define HFOC_SYSCALL_GETCWD     10U
#define HFOC_SYSCALL_MKDIR      11U
#define HFOC_SYSCALL_UNLINK     12U
#define HFOC_SYSCALL_RENAME     13U
#define HFOC_SYSCALL_FILE_WRITE 14U
#define HFOC_SYSCALL_EXEC       15U
#define HFOC_SYSCALL_RESULT     16U
#define HFOC_SYSCALL_PIPE       17U
#define HFOC_SYSCALL_DUP2       18U
#define HFOC_SYSCALL_SPAWN      19U
#define HFOC_SYSCALL_WAIT       20U
/* ITEM-633: job-control calls appended after the published numbers. */
#define HFOC_SYSCALL_KILL       21U
#define HFOC_SYSCALL_WAIT2      22U
#define HFOC_SYSCALL_SETPGID    23U
#define HFOC_SYSCALL_TTY_FOREGROUND 24U
#define HFOC_SYSCALL_SLEEP      25U

#define HFOC_OPEN_READ    0x1U
#define HFOC_OPEN_WRITE   0x2U
#define HFOC_OPEN_CREATE  0x4U
#define HFOC_OPEN_TRUNC   0x8U

#define HFOC_SEEK_SET 0U
#define HFOC_SEEK_CUR 1U
#define HFOC_SEEK_END 2U

#define HFOC_STAT_FILE     1U
#define HFOC_STAT_DIR      2U
#define HFOC_STAT_UNLINKED 0x1U
#define HFOC_STAT_RDONLY   0x2U

/* Negative errors the runtime reports.  The magnitude is the kernel code. */
#define HFOC_EINVAL      (-1)
#define HFOC_EFAULT      (-2)
#define HFOC_ENOSYS      (-3)
#define HFOC_EPERM       (-4)
#define HFOC_ENOENT      (-5)
#define HFOC_EACCES      (-6)
#define HFOC_EBADF       (-7)
#define HFOC_ENOSPC      (-8)
#define HFOC_EIO         (-9)
#define HFOC_ENOTDIR     (-10)
#define HFOC_EISDIR      (-11)
#define HFOC_ENAMETOOLONG (-12)
#define HFOC_EEXIST      (-13)
#define HFOC_ENOTEMPTY   (-14)
#define HFOC_EROFS       (-15)
#define HFOC_EMFILE      (-16)
#define HFOC_EPIPE       (-17)
#define HFOC_EAGAIN      (-18)
#define HFOC_ECHILD      (-19)

#define HFOC_PATH_MAX 1024U
#define HFOC_NAME_MAX 255U
#define HFOC_IO_MAX   65536U

struct hfoc_stat {
    uint32_t type;
    uint32_t size;
    uint32_t open_count;
    uint32_t flags;
};

struct hfoc_dirent {
    uint32_t type;
    uint32_t size;
    uint32_t flags;
    uint32_t name_length;
    char name[HFOC_NAME_MAX + 1U];
};

/*
 * ITEM-628: spawn request.  Actions are applied in order to the child's
 * standard streams before it is published, so `2>&1` can duplicate the fd an
 * earlier action installed.  ADOPT names a parent pipe fd; OPEN opens a path
 * in the child; DUP duplicates an already-applied child fd.
 */
#define HFOC_SPAWN_ACTION_MAX 8U
#define HFOC_SPAWN_OPEN  1U
#define HFOC_SPAWN_ADOPT 2U
#define HFOC_SPAWN_DUP   3U
/* ITEM-633: set the child's process group before publish. */
#define HFOC_SPAWN_PGID  4U
#define HFOC_SPAWN_CLOSE 5U

struct hfoc_fd_action {
    uint32_t destination;
    uint32_t operation;
    uint32_t source;
    uint32_t flags;
    char path[HFOC_PATH_MAX];
};

struct hfoc_spawn_request {
    uint64_t path;
    uint64_t argv;
    uint64_t envp;
    uint64_t actions;
    uint32_t action_count;
    uint32_t reserved;
};

extern long hfoc_argc;
extern char **hfoc_argv;
extern char **hfoc_envp;
extern char **environ;

/* Raw ABI.  Programs should use the wrappers below. */
long hfoc_raw_syscall3(long number, long arg0, long arg1, long arg2);
long hfoc_raw_syscall0(long number);

long hfoc_write(int fd, const void *buffer, size_t length);
long hfoc_read(int fd, void *buffer, size_t length);
long hfoc_open(const char *path, uint32_t flags);
long hfoc_close(int fd);
long hfoc_stat(const char *path, struct hfoc_stat *out);
long hfoc_getdents(const char *path, uint32_t index, struct hfoc_dirent *out);
long hfoc_lseek(int fd, long offset, uint32_t whence);
long hfoc_chdir(const char *path);
long hfoc_getcwd(char *buffer, size_t capacity);
long hfoc_mkdir(const char *path);
long hfoc_unlink(const char *path);
long hfoc_rename(const char *old_path, const char *new_path);
long hfoc_exec(const char *path, char *const *argv, char *const *envp);
long hfoc_spawn(const char *path, char *const *argv, char *const *envp,
                const struct hfoc_fd_action *actions, size_t action_count);
long hfoc_wait(int *status_out);
/* ITEM-633: job control.  SIG_* are the bounded native signal numbers. */
#define HFOC_SIG_HUP  1U
#define HFOC_SIG_INT  2U
#define HFOC_SIG_QUIT 3U
#define HFOC_SIG_TERM 15U
#define HFOC_SIG_CHLD 17U
#define HFOC_SIG_CONT 18U
#define HFOC_SIG_TSTP 20U

#define HFOC_WAIT_NOHANG    0x1U
#define HFOC_WAIT_UNTRACED  0x2U
#define HFOC_WAIT_CONTINUED 0x4U
#define HFOC_WAIT_STATUS_EXITED(code) ((((uint32_t)(code)) & 0xffU) << 8U)
#define HFOC_WAIT_STATUS_SIGNALED(status) ((uint32_t)(status) & 0x7fU)
#define HFOC_WAIT_STATUS_STOPPED(signal)     (((((uint32_t)(signal)) & 0xffU) << 8U) | 0x7fU)
#define HFOC_WAIT_STATUS_CONTINUED UINT32_C(0xffff)

long hfoc_kill(uint32_t pid_or_pgid, uint32_t signal, int group);
long hfoc_wait2(uint32_t flags, uint32_t result[2]);
long hfoc_setpgid(uint32_t pid, uint32_t pgid);
long hfoc_tty_foreground(uint32_t pgid);
long hfoc_sleep(uint32_t ticks);
long hfoc_pipe(int descriptors[2]);
long hfoc_dup2(int old_descriptor, int new_descriptor);
long hfoc_yield(void);

__attribute__((noreturn)) void hfoc_exit(int status);
const char *hfoc_strerror(long error);

/* Bounded allocation: a fixed arena inside the program, never the kernel. */
#define HFOC_ARENA_BYTES (32U * 1024U)
void *hfoc_alloc(size_t bytes);
void hfoc_alloc_reset(void);

/* String and memory helpers (the compiler may call these). */
size_t hfoc_strlen(const char *text);
int hfoc_strcmp(const char *left, const char *right);
int hfoc_strncmp(const char *left, const char *right, size_t count);
char *hfoc_strcpy(char *destination, const char *source);
char *hfoc_strncpy(char *destination, const char *source, size_t count);
char *hfoc_strchr(const char *text, int character);
char *hfoc_strrchr(const char *text, int character);
char *hfoc_strstr(const char *haystack, const char *needle);
int hfoc_strcasecmp(const char *left, const char *right);
int hfoc_memcmp(const void *left, const void *right, size_t count);
void *hfoc_memcpy(void *destination, const void *source, size_t count);
void *hfoc_memmove(void *destination, const void *source, size_t count);
void *hfoc_memset(void *destination, int value, size_t count);
long hfoc_parse_int(const char *text, int *ok);

/* Formatted output.  Supported: %% %c %s %d %i %u %x %p, '-'/'0' flags and
   a decimal width.  No floating point, no positional arguments. */
int hfoc_vsnprintf(char *buffer, size_t capacity, const char *format,
                   __builtin_va_list arguments);
int hfoc_snprintf(char *buffer, size_t capacity, const char *format, ...);
int hfoc_printf(const char *format, ...);
int hfoc_puts(const char *text);
/* Diagnostics and usage errors: the same formatting and bounded write loop,
   but on standard error (fd 2), so the shell's `2>` redirects only errors. */
int hfoc_eprintf(const char *format, ...);
int hfoc_eputs(const char *text);
int hfoc_write_all(int fd, const void *buffer, size_t length);

/* Whole-file helpers shared by cat/cp/wc/grep. */
long hfoc_copy_file(const char *source, const char *destination);
int hfoc_is_option(const char *argument);

#endif
