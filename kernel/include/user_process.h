#ifndef HFOC_USER_PROCESS_H
#define HFOC_USER_PROCESS_H

#include "process.h"
#include "tty.h"

#include <stdbool.h>
#include <stdint.h>

/*
 * ITEM-625: spawn a native static ELF64 process from the boot VFS and replace
 * the image of an existing user process in place.
 *
 * The spawn creates the process, maps the whole validated image and the
 * initial argv/env stack, and only publishes the scheduler task after every
 * step succeeded.  Exec builds a fresh address space and stack first, keeps
 * the old image on any failure, and only then commits the new image while
 * preserving the PID, the open descriptors and the capability table.
 */
enum user_process_status {
    USER_PROCESS_OK = 0,
    USER_PROCESS_NULL = 1,
    USER_PROCESS_NO_VFS = 2,
    USER_PROCESS_NOT_FOUND = 3,
    USER_PROCESS_TOO_LARGE = 4,
    USER_PROCESS_BAD_ELF = 5,
    USER_PROCESS_UNSUPPORTED_LAYOUT = 6,
    USER_PROCESS_CREATE_FAILED = 7,
    USER_PROCESS_SPAWN_FAILED = 8,
    USER_PROCESS_BAD_STATE = 9,
    USER_PROCESS_LIMIT_EXCEEDED = 10,
    USER_PROCESS_BAD_STACK = 11,
};

enum user_process_status user_process_spawn(
    struct kernel_process **out_process, struct process_control_block *parent,
    const char *path, const char *const *argv, const char *const *envp,
    uint64_t hhdm_offset);

/*
 * ITEM-628: a child's standard streams are set before it is published to the
 * scheduler, so it can never run a command with a half-applied redirection.
 * Actions are applied in order; DUP therefore sees the descriptor an earlier
 * action installed, which is how `2>&1` shares one description with fd 1.
 */
#define USER_PROCESS_FD_ACTION_MAX 8U
#define USER_PROCESS_FD_OPEN 1U
#define USER_PROCESS_FD_ADOPT 2U
#define USER_PROCESS_FD_DUP 3U
/* ITEM-633: set the child's process group before publish, or close a stream
   so a background job cannot read the owner terminal. */
#define USER_PROCESS_FD_PGID 4U
#define USER_PROCESS_FD_CLOSE 5U

struct user_process_fd_action {
    uint32_t destination;
    uint32_t operation;
    uint32_t source;
    uint32_t flags;
    char path[VFS_PATH_MAX];
};

enum user_process_status user_process_spawn_actions(
    struct kernel_process **out_process, struct process_control_block *parent,
    const char *path, const char *const *argv, const char *const *envp,
    const struct user_process_fd_action *actions, uint32_t action_count,
    uint64_t hhdm_offset);
/*
 * ITEM-629: the owner shell's standard streams are bound to one session TTY
 * before the task is published, so no child can run on the kernel console.
 */
enum user_process_status user_process_spawn_tty(
    struct kernel_process **out_process, struct process_control_block *parent,
    const char *path, const char *const *argv, const char *const *envp,
    struct tty *tty, uint64_t hhdm_offset);
/*
 * Replace `process` with the image at `path`.  On success the new user entry
 * and stack top are written to the out parameters; the caller owns the
 * architectural return to user mode.  On failure the process keeps its old
 * image, descriptors and capabilities unchanged.
 */
enum user_process_status user_process_exec(
    struct kernel_process *process, const char *path,
    const char *const *argv, const char *const *envp, uint64_t hhdm_offset,
    uint64_t *entry_out, uint64_t *stack_top_out);
const char *user_process_status_string(enum user_process_status status);

#endif
