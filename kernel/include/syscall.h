#ifndef HFOC_SYSCALL_H
#define HFOC_SYSCALL_H

#include "process.h"
#include "user.h"
#include "vmm.h"
#include <stdbool.h>
#include <stdint.h>

#define IA32_EFER_MSR   0xC0000080U
#define IA32_STAR_MSR   0xC0000081U
#define IA32_LSTAR_MSR  0xC0000082U
#define IA32_FMASK_MSR  0xC0000084U
#define IA32_EFER_SCE   (UINT64_C(1) << 0U)

/* SYSCALL uses STAR[47:32]; SYSRET uses STAR[63:48]. */
#define SYSCALL_STAR_SYSCALL 0x08U
#define SYSCALL_STAR_SYSRET  0x10U
#define SYSCALL_STAR_VALUE \
    (((uint64_t)SYSCALL_STAR_SYSRET << 48U) | \
     ((uint64_t)SYSCALL_STAR_SYSCALL << 32U))
#define SYSCALL_FMASK      (UINT64_C(1) << 9U)

#define SYSCALL_WRITE_MAX  64U
/* ITEM-624: one file read/write call is bounded; the VFS file itself is
   capped at VFS_FILE_MAX and the copy is validated page by page. */
#define SYSCALL_FILE_IO_MAX 65536U
#define SYSCALL_PATH_MAX    VFS_PATH_MAX

#define SYSCALL_RET_OK      0U
#define SYSCALL_RET_EINVAL  1U
#define SYSCALL_RET_EFAULT  2U
#define SYSCALL_RET_ENOSYS  3U
#define SYSCALL_RET_EPERM   4U
/* ITEM-624: native file error values appended to the existing ABI. */
#define SYSCALL_RET_ENOENT  5U
#define SYSCALL_RET_EACCES  6U
#define SYSCALL_RET_EBADF   7U
#define SYSCALL_RET_ENOSPC  8U
#define SYSCALL_RET_EIO     9U
#define SYSCALL_RET_ENOTDIR 10U
#define SYSCALL_RET_EISDIR  11U
#define SYSCALL_RET_ENAMETOOLONG 12U
#define SYSCALL_RET_EEXIST  13U
#define SYSCALL_RET_ENOTEMPTY 14U
#define SYSCALL_RET_EROFS   15U
#define SYSCALL_RET_EMFILE  16U
/* ITEM-627: the peer reader of a pipe is gone. */
#define SYSCALL_RET_EPIPE   17U
#define SYSCALL_RET_EAGAIN  18U
/* ITEM-628: the caller has no child left to wait for. */
#define SYSCALL_RET_ECHILD  19U
/* ITEM-633: no such process/group. */
#define SYSCALL_RET_ESRCH   20U

/* Native open flags (the VFS values are the ABI). */
#define SYSCALL_OPEN_READ    0x1U
#define SYSCALL_OPEN_WRITE   0x2U
#define SYSCALL_OPEN_CREATE  0x4U
#define SYSCALL_OPEN_TRUNC   0x8U
#define SYSCALL_OPEN_FLAGS_MASK \
    (SYSCALL_OPEN_READ | SYSCALL_OPEN_WRITE | SYSCALL_OPEN_CREATE | \
     SYSCALL_OPEN_TRUNC)

/* Native lseek whence values. */
#define SYSCALL_SEEK_SET 0U
#define SYSCALL_SEEK_CUR 1U
#define SYSCALL_SEEK_END 2U

struct syscall_stat {
    uint32_t type;
    uint32_t size;
    uint32_t open_count;
    uint32_t flags;
};

#define SYSCALL_STAT_FILE 1U
#define SYSCALL_STAT_DIR  2U
#define SYSCALL_STAT_UNLINKED 0x1U
#define SYSCALL_STAT_READ_ONLY 0x2U

struct syscall_dirent {
    uint32_t type;
    uint32_t size;
    uint32_t flags;
    uint32_t name_length;
    char name[VFS_NAME_MAX + 1U];
};

/* ITEM-628: the bounded request one SPAWN call copies from the caller. */
#define SYSCALL_SPAWN_ACTION_MAX 8U
#define SYSCALL_SPAWN_OPEN  1U
#define SYSCALL_SPAWN_ADOPT 2U
#define SYSCALL_SPAWN_DUP   3U

struct syscall_spawn_action {
    uint32_t destination;
    uint32_t operation;
    uint32_t source;
    uint32_t flags;
    char path[VFS_PATH_MAX];
};

struct syscall_spawn_request {
    uint64_t path;
    uint64_t argv;
    uint64_t envp;
    uint64_t actions;
    uint32_t action_count;
    uint32_t reserved;
};

/* WAIT writes {pid, exit_status} through this bounded user window. */
#define SYSCALL_WAIT_RESULT_WORDS 2U


struct syscall_origin {
    const struct process_control_block *pcb;
    enum process_security_domain domain;
    uint32_t generation;
};

enum syscall_origin_status {
    SYSCALL_ORIGIN_OK = 0,
    SYSCALL_ORIGIN_NULL = 1,
    SYSCALL_ORIGIN_BAD_PCB = 2,
    SYSCALL_ORIGIN_DOMAIN_MISMATCH = 3,
    SYSCALL_ORIGIN_STALE = 4,
};

enum syscall_number {
    SYSCALL_WRITE = 0,
    SYSCALL_YIELD = 1,
    SYSCALL_EXIT = 2,
    /* ITEM-624: file calls appended; the first three numbers never move. */
    SYSCALL_READ = 3,
    SYSCALL_OPEN = 4,
    SYSCALL_CLOSE = 5,
    SYSCALL_STAT = 6,
    SYSCALL_GETDENTS = 7,
    SYSCALL_LSEEK = 8,
    SYSCALL_CHDIR = 9,
    SYSCALL_GETCWD = 10,
    SYSCALL_MKDIR = 11,
    SYSCALL_UNLINK = 12,
    SYSCALL_RENAME = 13,
    /* Native write for any fd; legacy WRITE=0 remains stdout-only. */
    SYSCALL_FILE_WRITE = 14,
    /* ITEM-625: replace the current image, appending after the file call. */
    SYSCALL_EXEC = 15,
    /*
     * ITEM-626: read the calling process's last file/exec error.  The legacy
     * calls keep returning their positive codes; the native runtime asks this
     * only when a result could be either a small success or an error, so no
     * existing number or meaning changes.
     */
    SYSCALL_RESULT = 16,
    /* ITEM-627: bounded pipe creation and descriptor duplication. */
    SYSCALL_PIPE = 17,
    SYSCALL_DUP2 = 18,
    /*
     * ITEM-628: spawn a child with pre-applied fd actions and wait for an
     * exited child.  Numbers 0..18 keep their published meaning.
     */
    SYSCALL_SPAWN = 19,
    SYSCALL_WAIT = 20,
    /*
     * ITEM-633: append process-group, signal, encoded-wait and terminal
     * foreground calls.  Numbers 0..20 keep their published meaning.
     */
    SYSCALL_KILL = 21,
    SYSCALL_WAIT2 = 22,
    SYSCALL_SETPGID = 23,
    SYSCALL_TTY_FOREGROUND = 24,
    /* ITEM-633: bounded scheduler sleep for the native sleep utility. */
    SYSCALL_SLEEP = 25,
};

#define SYSCALL_COUNT 26U
/* ITEM-633 spawn actions appended to OPEN/ADOPT/DUP. */
#define SYSCALL_SPAWN_PGID  4U
#define SYSCALL_SPAWN_CLOSE 5U
/* ITEM-633 kill arg2: signal the group named by arg0 instead of one PID. */
#define SYSCALL_KILL_GROUP 1U

enum syscall_abi_status {
    SYSCALL_ABI_OK = 0,
    SYSCALL_ABI_NOT_PUBLISHED = 1,
    SYSCALL_ABI_ALREADY_PUBLISHED = 2,
    SYSCALL_ABI_READBACK_MISMATCH = 3,
    SYSCALL_ABI_RESTORE_FAILED = 4,
};

extern void syscall_entry(void);

enum syscall_abi_status syscall_msr_prepare(void);
enum syscall_abi_status syscall_msr_verify(void);
enum syscall_abi_status syscall_msr_restore(void);
bool syscall_msr_published(void);
enum syscall_origin_status syscall_origin_capture(
    const struct process_control_block *pcb,
    struct syscall_origin *origin);
enum syscall_origin_status syscall_origin_validate(
    const struct syscall_origin *origin);
const char *syscall_origin_status_string(enum syscall_origin_status status);
bool syscall_requires_capability(uint64_t number, uint32_t *type_out);
uint64_t syscall_handle(uint64_t number,
                        uint64_t arg0,
                        uint64_t arg1,
                        uint64_t arg2);
uint64_t syscall_handle_checked(const struct syscall_origin *origin,
                                uint64_t number,
                                uint64_t arg0,
                                uint64_t arg1,
                                uint64_t arg2);
const char *syscall_abi_status_string(enum syscall_abi_status status);

#ifdef HFOC_SYSCALL_QEMU_TEST
enum syscall_abi_status syscall_qemu_test_init(uint64_t hhdm_offset);
__attribute__((noreturn)) void syscall_qemu_trap_complete(void);
#endif

#endif
