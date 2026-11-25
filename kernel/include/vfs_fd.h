#ifndef HFOC_VFS_FD_H
#define HFOC_VFS_FD_H

#include "descriptor.h"
#include "vfs.h"

#include <stdbool.h>
#include <stdint.h>

/*
 * ITEM-623: bind a VFS open file to a process descriptor table.
 *
 * The caller owns the bounded `struct file_description` storage; vfs.c keeps
 * no hidden lifetime.  On success the table owns the initial description
 * reference, and the VFS file is closed when the last descriptor (including
 * a dup2 or inherited copy) is closed.  On failure nothing is bound and the
 * description storage is untouched.
 */
enum descriptor_status vfs_file_bind(struct descriptor_table *table,
                                     struct vfs_context *context,
                                     const char *path, uint32_t flags,
                                     struct file_description *storage,
                                     uint32_t *descriptor_out);
/* Same binding, but preserves the VFS refusal for the syscall error ABI. */
enum descriptor_status vfs_file_bind_checked(
    struct descriptor_table *table, struct vfs_context *context,
    const char *path, uint32_t flags, struct file_description *storage,
    uint32_t *descriptor_out, enum vfs_status *vfs_status_out);

#endif
