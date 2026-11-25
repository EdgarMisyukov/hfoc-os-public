/*
 * ITEM-624: native file syscalls over the ITEM-622/623 VFS and descriptor
 * owners.
 *
 * The caller has already validated the syscall origin.  This unit only uses
 * the process that origin names: no global fd table, no path parsing in user
 * memory, no direct dereference of a user pointer.  Every buffer crosses
 * user_copy, and every descriptor comes from that process's own table.
 */
#include "syscall_file.h"

#include "descriptor.h"
#include "kernel_vfs.h"
#include "pipe.h"
#include "process.h"
#include "user_copy.h"
#include "vfs_fd.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#if defined(HFOC_USER_INIT_QEMU_TEST)
#include "serial.h"
#endif

#define SYSCALL_FILE_IO_CHUNK 4096U

/* The workspace owner is optional in host tests; production links it. */
extern int hfocfs_workspace_commit(void) __attribute__((weak));

_Static_assert(offsetof(struct kernel_process, pcb) == 0U,
               "the syscall origin must name the process control block first");

static struct kernel_process *syscall_file_process(
    const struct syscall_origin *origin)
{
    if (origin == NULL || origin->pcb == NULL ||
        !origin->pcb->published ||
        origin->pcb->security_domain != PROCESS_SECURITY_DOMAIN_USER) {
        return NULL;
    }
    return (struct kernel_process *)(void *)origin->pcb;
}

/* Record the operation's error for SYSCALL_RESULT.  Success paths never call
   this, so a descriptor or byte count in 1..16 stays a success. */
static uint64_t syscall_file_fail(struct kernel_process *process,
                                  uint64_t code)
{
    if (process != NULL) {
        process->pcb.syscall_last_error = (uint32_t)code;
    }
    return code;
}

static uint64_t syscall_file_vfs_error(enum vfs_status status)
{
    switch (status) {
    case VFS_OK:
        return SYSCALL_RET_OK;
    case VFS_NOT_FOUND:
        return SYSCALL_RET_ENOENT;
    case VFS_EXISTS:
        return SYSCALL_RET_EEXIST;
    case VFS_NOT_DIR:
        return SYSCALL_RET_ENOTDIR;
    case VFS_IS_DIR:
        return SYSCALL_RET_EISDIR;
    case VFS_NOT_EMPTY:
        return SYSCALL_RET_ENOTEMPTY;
    case VFS_NO_SPACE:
    case VFS_TOO_MANY_FILES:
    case VFS_TOO_MANY_NODES:
        return SYSCALL_RET_ENOSPC;
    case VFS_NAME_TOO_LONG:
    case VFS_PATH_TOO_LONG:
        return SYSCALL_RET_ENAMETOOLONG;
    case VFS_READ_ONLY:
        return SYSCALL_RET_EROFS;
    case VFS_UNSUPPORTED:
        return SYSCALL_RET_ENOSYS;
    case VFS_BAD_FILE:
    case VFS_BAD_OFFSET:
    case VFS_NO_DATA:
    case VFS_INVALID:
        return SYSCALL_RET_EINVAL;
    case VFS_BUSY:
        return SYSCALL_RET_EACCES;
    case VFS_BROKEN_PIPE:
        return SYSCALL_RET_EPIPE;
    case VFS_AGAIN:
        return SYSCALL_RET_EAGAIN;
    default:
        return SYSCALL_RET_EIO;
    }
}

static uint64_t syscall_file_copy_error(enum user_copy_status status)
{
    switch (status) {
    case USER_COPY_OK:
        return SYSCALL_RET_OK;
    case USER_COPY_RANGE:
    case USER_COPY_NO_NUL:
        return SYSCALL_RET_EINVAL;
    case USER_COPY_BAD_ACCESS:
        return SYSCALL_RET_EACCES;
    case USER_COPY_NULL:
    case USER_COPY_NOT_MAPPED:
        return SYSCALL_RET_EFAULT;
    default:
        return SYSCALL_RET_EIO;
    }
}

static bool syscall_file_context(struct kernel_process *process,
                                 struct vfs_context *context)
{
    const struct vfs *fs;

    if (process == NULL || context == NULL) {
        return false;
    }
    fs = kernel_vfs_boot_instance();
    if (fs == NULL) {
        return false;
    }
    memset(context, 0, sizeof(*context));
    context->fs = (struct vfs *)fs;
    if (process->vfs_cwd_ready) {
        context->cwd = process->vfs_cwd;
        return true;
    }
    if (vfs_context_init(context, context->fs) != VFS_OK) {
        return false;
    }
    process->vfs_cwd = context->cwd;
    process->vfs_cwd_ready = true;
    return true;
}

static enum user_copy_status syscall_file_path(char *path, uint64_t user_path,
                                               size_t *length_out)
{
    return user_copy_string_in(path, SYSCALL_PATH_MAX, user_path, length_out);
}

static struct file_description *syscall_file_free_description(
    struct kernel_process *process)
{
    uint32_t index;

    for (index = 0U; index < PROCESS_DESCRIPTOR_CAPACITY; ++index) {
        if (!process->file_descriptions[index].published) {
            return &process->file_descriptions[index];
        }
    }
    return NULL;
}

static uint64_t syscall_file_open(struct kernel_process *process,
                                  struct vfs_context *context, uint64_t arg0,
                                  uint64_t arg1)
{
    char path[SYSCALL_PATH_MAX];
    struct file_description *storage;
    enum vfs_status vfs_status = VFS_BAD_STATE;
    enum descriptor_status descriptor_status;
    uint32_t descriptor = 0U;
    size_t length = 0U;
    uint64_t copy_status;

    if ((arg1 & ~(uint64_t)SYSCALL_OPEN_FLAGS_MASK) != 0U ||
        (arg1 & (SYSCALL_OPEN_READ | SYSCALL_OPEN_WRITE)) == 0U) {
        return syscall_file_fail(process, SYSCALL_RET_EINVAL);
    }
    copy_status = syscall_file_copy_error(
        syscall_file_path(path, arg0, &length));
    if (copy_status != SYSCALL_RET_OK) {
        return syscall_file_fail(process, copy_status);
    }
    storage = syscall_file_free_description(process);
    if (storage == NULL) {
        return syscall_file_fail(process, SYSCALL_RET_EMFILE);
    }
    descriptor_status = vfs_file_bind_checked(
        &process->descriptor_table, context, path, (uint32_t)arg1, storage,
        &descriptor, &vfs_status);
    if (descriptor_status != DESCRIPTOR_OK) {
        return syscall_file_fail(
            process, descriptor_status == DESCRIPTOR_FULL
                         ? SYSCALL_RET_EMFILE
                         : syscall_file_vfs_error(vfs_status));
    }
    return descriptor;
}

static uint64_t syscall_file_read(struct kernel_process *process, uint64_t arg0,
                                  uint64_t arg1, uint64_t arg2)
{
    struct file_description *description = NULL;
    unsigned char buffer[SYSCALL_FILE_IO_CHUNK];
    uint64_t done_total = 0U;
    uint64_t copy_status;

    if (arg2 == 0U) {
        return 0U;
    }
    if (arg2 > SYSCALL_FILE_IO_MAX) {
        return syscall_file_fail(process, SYSCALL_RET_EINVAL);
    }
    if (descriptor_table_lookup(&process->descriptor_table, (uint32_t)arg0,
                                &description) != DESCRIPTOR_OK ||
        description == NULL) {
        return syscall_file_fail(process, SYSCALL_RET_EBADF);
    }
    if (!description->readable || description->ops == NULL ||
        description->ops->read == NULL) {
        return syscall_file_fail(process, SYSCALL_RET_EBADF);
    }
    copy_status = syscall_file_copy_error(
        user_copy_check(arg1, (size_t)arg2, true));
    if (copy_status != SYSCALL_RET_OK) {
        return syscall_file_fail(process, copy_status);
    }
    while (done_total < arg2) {
        const size_t want = (size_t)(arg2 - done_total) >
                                    SYSCALL_FILE_IO_CHUNK
                                ? SYSCALL_FILE_IO_CHUNK
                                : (size_t)(arg2 - done_total);
        size_t done = 0U;
        enum vfs_status status = description->ops->read(
            description, buffer, want, &done);

        if (status != VFS_OK) {
            if (done_total != 0U) {
                break;
            }
            return syscall_file_fail(process, syscall_file_vfs_error(status));
        }
        if (done == 0U) {
            break;
        }
        if (user_copy_out(arg1 + done_total, buffer, done) != USER_COPY_OK) {
            if (done_total != 0U) {
                return done_total;
            }
            return syscall_file_fail(process, SYSCALL_RET_EFAULT);
        }
        done_total += done;
    }
    return done_total;
}

static uint64_t syscall_file_write(struct kernel_process *process,
                                   uint64_t arg0, uint64_t arg1, uint64_t arg2)
{
    struct file_description *description = NULL;
    unsigned char buffer[SYSCALL_FILE_IO_CHUNK];
    uint64_t done_total = 0U;
    uint64_t copy_status;

    if (arg2 == 0U) {
        return 0U;
    }
    if (arg2 > SYSCALL_FILE_IO_MAX) {
        return syscall_file_fail(process, SYSCALL_RET_EINVAL);
    }
    if (descriptor_table_lookup(&process->descriptor_table, (uint32_t)arg0,
                                &description) != DESCRIPTOR_OK ||
        description == NULL) {
        return syscall_file_fail(process, SYSCALL_RET_EBADF);
    }
    if (!description->writable || description->ops == NULL ||
        description->ops->write == NULL) {
        return syscall_file_fail(process, SYSCALL_RET_EBADF);
    }
    copy_status = syscall_file_copy_error(
        user_copy_check(arg1, (size_t)arg2, false));
    if (copy_status != SYSCALL_RET_OK) {
        return syscall_file_fail(process, copy_status);
    }
    while (done_total < arg2) {
        const size_t want = (size_t)(arg2 - done_total) >
                                    SYSCALL_FILE_IO_CHUNK
                                ? SYSCALL_FILE_IO_CHUNK
                                : (size_t)(arg2 - done_total);
        size_t done = 0U;
        enum vfs_status status;

        if (user_copy_in(buffer, arg1 + done_total, want) != USER_COPY_OK) {
            if (done_total != 0U) {
                return done_total;
            }
            return syscall_file_fail(process, SYSCALL_RET_EFAULT);
        }
        status = description->ops->write(description, buffer, want, &done);
        if (status != VFS_OK) {
            if (done_total != 0U) {
                break;
            }
            return syscall_file_fail(process, syscall_file_vfs_error(status));
        }
        done_total += done;
        if (done == 0U) {
            break;
        }
    }
    return done_total;
}

static uint64_t syscall_file_persist(struct kernel_process *process)
{
    if (hfocfs_workspace_commit != NULL &&
        hfocfs_workspace_commit() != 0) {
        return syscall_file_fail(process, SYSCALL_RET_EIO);
    }
    return SYSCALL_RET_OK;
}

static uint64_t syscall_file_close(struct kernel_process *process,
                                   uint64_t arg0)
{
    if (arg0 >= PROCESS_DESCRIPTOR_CAPACITY) {
        return syscall_file_fail(process, SYSCALL_RET_EBADF);
    }
    if (descriptor_table_close(&process->descriptor_table, (uint32_t)arg0) !=
        DESCRIPTOR_OK) {
        return syscall_file_fail(process, SYSCALL_RET_EBADF);
    }
    return syscall_file_persist(process);
}

static uint64_t syscall_file_stat(struct kernel_process *process,
                                  struct vfs_context *context, uint64_t arg0,
                                  uint64_t arg1)
{
    char path[SYSCALL_PATH_MAX];
    struct vfs_stat stat;
    struct syscall_stat out;
    size_t length = 0U;
    uint64_t copy_status;
    enum vfs_status status;

    copy_status = syscall_file_copy_error(
        syscall_file_path(path, arg0, &length));
    if (copy_status != SYSCALL_RET_OK) {
        return syscall_file_fail(process, copy_status);
    }
    status = vfs_stat(context, path, &stat);
    if (status != VFS_OK) {
        return syscall_file_fail(process, syscall_file_vfs_error(status));
    }
    memset(&out, 0, sizeof(out));
    out.type = stat.type == VFS_NODE_DIR ? SYSCALL_STAT_DIR
                                         : SYSCALL_STAT_FILE;
    out.size = stat.size;
    out.open_count = stat.open_count;
    out.flags = (stat.unlinked ? SYSCALL_STAT_UNLINKED : 0U) |
                (stat.read_only ? SYSCALL_STAT_READ_ONLY : 0U);
    if (user_copy_out(arg1, &out, sizeof(out)) != USER_COPY_OK) {
        return syscall_file_fail(process, SYSCALL_RET_EFAULT);
    }
    return SYSCALL_RET_OK;
}

static uint64_t syscall_file_getdents(struct kernel_process *process,
                                      struct vfs_context *context,
                                      uint64_t arg0, uint64_t arg1,
                                      uint64_t arg2)
{
    char path[SYSCALL_PATH_MAX];
    char name[VFS_NAME_MAX + 1U];
    struct vfs_stat stat;
    struct syscall_dirent out;
    size_t path_length = 0U;
    uint64_t copy_status;
    enum vfs_status status;

    copy_status = syscall_file_copy_error(
        syscall_file_path(path, arg0, &path_length));
    if (copy_status != SYSCALL_RET_OK) {
        return syscall_file_fail(process, copy_status);
    }
    status = vfs_readdir(context, path, (uint32_t)arg1, name, sizeof(name),
                         &stat);
    if (status != VFS_OK) {
        return syscall_file_fail(process, syscall_file_vfs_error(status));
    }
    memset(&out, 0, sizeof(out));
    out.type = stat.type == VFS_NODE_DIR ? SYSCALL_STAT_DIR
                                         : SYSCALL_STAT_FILE;
    out.size = stat.size;
    out.flags = (stat.unlinked ? SYSCALL_STAT_UNLINKED : 0U) |
                (stat.read_only ? SYSCALL_STAT_READ_ONLY : 0U);
    out.name_length = (uint32_t)strlen(name);
    memcpy(out.name, name, strlen(name) + 1U);
    if (user_copy_out(arg2, &out, sizeof(out)) != USER_COPY_OK) {
        return syscall_file_fail(process, SYSCALL_RET_EFAULT);
    }
    return SYSCALL_RET_OK;
}

static uint64_t syscall_file_lseek(struct kernel_process *process,
                                   uint64_t arg0, uint64_t arg1,
                                   uint64_t arg2)
{
    struct file_description *description = NULL;
    struct vfs_file *file;
    uint64_t base = 0U;
    uint64_t target;

    if (descriptor_table_lookup(&process->descriptor_table, (uint32_t)arg0,
                                &description) != DESCRIPTOR_OK ||
        description == NULL || description->context == NULL) {
        return syscall_file_fail(process, SYSCALL_RET_EBADF);
    }
    file = (struct vfs_file *)description->context;
    if (!file->used || file->fs == NULL) {
        return syscall_file_fail(process, SYSCALL_RET_EBADF);
    }
    switch (arg2) {
    case SYSCALL_SEEK_SET:
        base = 0U;
        break;
    case SYSCALL_SEEK_CUR:
        base = file->offset;
        break;
    case SYSCALL_SEEK_END:
        base = file->fs->nodes[file->node].size;
        break;
    default:
        return syscall_file_fail(process, SYSCALL_RET_EINVAL);
    }
    if (arg1 > UINT64_MAX - base) {
        return syscall_file_fail(process, SYSCALL_RET_EINVAL);
    }
    target = base + arg1;
    if (target > VFS_FILE_MAX) {
        return syscall_file_fail(process, SYSCALL_RET_EINVAL);
    }
    if (vfs_seek(file, (uint32_t)target) != VFS_OK) {
        return syscall_file_fail(process, SYSCALL_RET_EINVAL);
    }
    return target;
}

static uint64_t syscall_file_chdir(struct kernel_process *process,
                                   struct vfs_context *context, uint64_t arg0)
{
    char path[SYSCALL_PATH_MAX];
    size_t length = 0U;
    uint64_t copy_status;
    enum vfs_status status;

    copy_status = syscall_file_copy_error(
        syscall_file_path(path, arg0, &length));
    if (copy_status != SYSCALL_RET_OK) {
        return syscall_file_fail(process, copy_status);
    }
    status = vfs_chdir(context, path);
    if (status != VFS_OK) {
        return syscall_file_fail(process, syscall_file_vfs_error(status));
    }
    process->vfs_cwd = context->cwd;
    process->vfs_cwd_ready = true;
    return SYSCALL_RET_OK;
}

static uint64_t syscall_file_getcwd(struct kernel_process *process,
                                    struct vfs_context *context, uint64_t arg0,
                                    uint64_t arg1)
{
    char path[SYSCALL_PATH_MAX];
    size_t length = 0U;
    enum vfs_status status;

    if (arg1 == 0U || arg1 > SYSCALL_PATH_MAX) {
        return syscall_file_fail(process, SYSCALL_RET_EINVAL);
    }
    if (user_copy_check(arg0, (size_t)arg1, true) != USER_COPY_OK) {
        return syscall_file_fail(process, SYSCALL_RET_EFAULT);
    }
    status = vfs_getcwd(context, path, arg1, &length);
    if (status != VFS_OK) {
        return syscall_file_fail(process, syscall_file_vfs_error(status));
    }
    if (length + 1U > arg1) {
        return syscall_file_fail(process, SYSCALL_RET_EINVAL);
    }
    if (user_copy_out(arg0, path, length + 1U) != USER_COPY_OK) {
        return syscall_file_fail(process, SYSCALL_RET_EFAULT);
    }
    return SYSCALL_RET_OK;
}

static uint64_t syscall_file_single_path(struct kernel_process *process,
                                         struct vfs_context *context,
                                         uint64_t arg0, uint64_t number)
{
    char path[SYSCALL_PATH_MAX];
    size_t length = 0U;
    uint64_t copy_status;
    enum vfs_status status;

    copy_status = syscall_file_copy_error(
        syscall_file_path(path, arg0, &length));
    if (copy_status != SYSCALL_RET_OK) {
        return syscall_file_fail(process, copy_status);
    }
    status = number == SYSCALL_MKDIR ? vfs_mkdir(context, path)
                                     : vfs_unlink(context, path);
    if (status != VFS_OK) {
        return syscall_file_fail(process, syscall_file_vfs_error(status));
    }
    return syscall_file_persist(process);
}

static uint64_t syscall_file_rename(struct kernel_process *process,
                                    struct vfs_context *context,
                                    uint64_t arg0, uint64_t arg1)
{
    char old_path[SYSCALL_PATH_MAX];
    char new_path[SYSCALL_PATH_MAX];
    size_t old_length = 0U;
    size_t new_length = 0U;
    uint64_t copy_status;
    enum vfs_status status;

    copy_status = syscall_file_copy_error(
        syscall_file_path(old_path, arg0, &old_length));
    if (copy_status != SYSCALL_RET_OK) {
        return syscall_file_fail(process, copy_status);
    }
    copy_status = syscall_file_copy_error(
        syscall_file_path(new_path, arg1, &new_length));
    if (copy_status != SYSCALL_RET_OK) {
        return syscall_file_fail(process, copy_status);
    }
    status = vfs_rename(context, old_path, new_path);
    if (status != VFS_OK) {
        return syscall_file_fail(process, syscall_file_vfs_error(status));
    }
    return syscall_file_persist(process);
}

static uint64_t syscall_file_pipe(struct kernel_process *process,
                                 uint64_t arg0)
{
#ifdef HFOC_PIPE_HOST_TEST
    (void)process;
    (void)arg0;
    return SYSCALL_RET_EPERM;
#else
    struct hfoc_pipe *pipe = NULL;
    uint32_t descriptors[2] = { 0U, 0U };
    enum pipe_status status;
    uint64_t copy_status;

    if (user_copy_check(arg0, sizeof(descriptors), true) != USER_COPY_OK) {
        return syscall_file_fail(process, SYSCALL_RET_EFAULT);
    }
    status = pipe_create(&pipe);
    if (status != PIPE_OK) {
        return syscall_file_fail(process, SYSCALL_RET_EMFILE);
    }
    status = pipe_bind_fd(process, pipe, false, &descriptors[0]);
    if (status == PIPE_OK) {
        status = pipe_bind_fd(process, pipe, true, &descriptors[1]);
    }
    if (status != PIPE_OK) {
        if (descriptors[0] != 0U) {
            (void)descriptor_table_close(&process->descriptor_table,
                                         descriptors[0]);
        }
        if (descriptors[1] != 0U) {
            (void)descriptor_table_close(&process->descriptor_table,
                                         descriptors[1]);
        }
        pipe_end_release(&pipe->read_end);
        pipe_end_release(&pipe->write_end);
        return syscall_file_fail(process, SYSCALL_RET_EMFILE);
    }
    /*
     * The creator's two references are transferred to the descriptors; from
     * here the pipe follows the descriptor lifetime (dup2/inheritance/close).
     */
    pipe_end_release(&pipe->read_end);
    pipe_end_release(&pipe->write_end);
    copy_status = syscall_file_copy_error(
        user_copy_out(arg0, descriptors, sizeof(descriptors)));
    if (copy_status != SYSCALL_RET_OK) {
        (void)descriptor_table_close(&process->descriptor_table,
                                     descriptors[0]);
        (void)descriptor_table_close(&process->descriptor_table,
                                     descriptors[1]);
        return syscall_file_fail(process, copy_status);
    }
    return SYSCALL_RET_OK;
#endif
}

static uint64_t syscall_file_dup2(struct kernel_process *process,
                                  uint64_t arg0, uint64_t arg1)
{
    enum descriptor_status status;

    if (arg0 >= PROCESS_DESCRIPTOR_CAPACITY ||
        arg1 >= PROCESS_DESCRIPTOR_CAPACITY) {
        return syscall_file_fail(process, SYSCALL_RET_EBADF);
    }
    status = descriptor_table_dup2(&process->descriptor_table, (uint32_t)arg0,
                                   (uint32_t)arg1);
    if (status == DESCRIPTOR_OK) {
        return SYSCALL_RET_OK;
    }
    if (status == DESCRIPTOR_BAD_DESCRIPTOR ||
        status == DESCRIPTOR_BAD_DESCRIPTION) {
        return syscall_file_fail(process, SYSCALL_RET_EBADF);
    }
    return syscall_file_fail(process, SYSCALL_RET_EIO);
}

#if defined(HFOC_USER_INIT_QEMU_TEST)
static void syscall_file_trace(uint64_t number, uint64_t result)
{
    const uint64_t flags = serial_sequence_begin();

    serial_write("[USER-FS] op=");
    serial_write_u64(number);
    serial_write(" result=");
    serial_write_u64(result);
    serial_write("\n");
    serial_sequence_end(flags);
}
#else
#define syscall_file_trace(number, result) do { (void)(number); (void)(result); } while (0)
#endif

bool syscall_file_number(uint64_t number)
{
    return (number >= SYSCALL_READ && number <= SYSCALL_RENAME) ||
           number == SYSCALL_FILE_WRITE || number == SYSCALL_PIPE ||
           number == SYSCALL_DUP2;
}

uint64_t syscall_file_handle(const struct syscall_origin *origin,
                             uint64_t number, uint64_t arg0, uint64_t arg1,
                             uint64_t arg2)
{
    struct kernel_process *process = syscall_file_process(origin);
    struct vfs_context context;

    if (process == NULL) {
        return SYSCALL_RET_EPERM;
    }
    process->pcb.syscall_last_error = 0U;
    if (!syscall_file_context(process, &context)) {
        return syscall_file_fail(process, SYSCALL_RET_EIO);
    }
    uint64_t result;

    switch (number) {
    case SYSCALL_READ:
        result = syscall_file_read(process, arg0, arg1, arg2);
        break;
    case SYSCALL_FILE_WRITE:
        result = syscall_file_write(process, arg0, arg1, arg2);
        break;
    case SYSCALL_OPEN:
        result = syscall_file_open(process, &context, arg0, arg1);
        break;
    case SYSCALL_CLOSE:
        result = syscall_file_close(process, arg0);
        break;
    case SYSCALL_STAT:
        result = syscall_file_stat(process, &context, arg0, arg1);
        break;
    case SYSCALL_GETDENTS:
        result = syscall_file_getdents(process, &context, arg0, arg1, arg2);
        break;
    case SYSCALL_LSEEK:
        result = syscall_file_lseek(process, arg0, arg1, arg2);
        break;
    case SYSCALL_CHDIR:
        result = syscall_file_chdir(process, &context, arg0);
        break;
    case SYSCALL_GETCWD:
        result = syscall_file_getcwd(process, &context, arg0, arg1);
        break;
    case SYSCALL_MKDIR:
    case SYSCALL_UNLINK:
        result = syscall_file_single_path(process, &context, arg0, number);
        break;
    case SYSCALL_RENAME:
        result = syscall_file_rename(process, &context, arg0, arg1);
        break;
    case SYSCALL_PIPE:
        result = syscall_file_pipe(process, arg0);
        break;
    case SYSCALL_DUP2:
        result = syscall_file_dup2(process, arg0, arg1);
        break;
    default:
        result = syscall_file_fail(process, SYSCALL_RET_ENOSYS);
        break;
    }
    syscall_file_trace(number, result);
    return result;
}
