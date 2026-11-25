#ifndef HFOC_VFS_MOUNT_H
#define HFOC_VFS_MOUNT_H

#include "initramfs.h"
#include "vfs.h"

#include <stdbool.h>
#include <stdint.h>

/*
 * ITEM-623: a bounded mount table over one VFS instance.
 *
 * The table records where a tree was published and whether it is read-only.
 * The read-only guarantee is enforced by the VFS node flag, not by this
 * bookkeeping: the table is the auditable map the boot status and the later
 * shell report.  Mounts never overlap and are never nested, so a lookup has
 * exactly one answer and an unmount cannot expose an arbitrary subtree.
 */

#define VFS_MAX_MOUNTS 8U

enum vfs_mount_kind {
    VFS_MOUNT_RAM = 1,
    VFS_MOUNT_INITRAMFS = 2,
};

struct vfs_mount {
    bool used;
    enum vfs_mount_kind kind;
    bool read_only;
    uint32_t node;
    char prefix[VFS_PATH_MAX];
};

struct vfs_mount_table {
    bool initialized;
    uint32_t count;
    struct vfs_mount mounts[VFS_MAX_MOUNTS];
};

enum vfs_status vfs_mount_table_init(struct vfs_mount_table *table);
enum vfs_status vfs_mount_ram(struct vfs_mount_table *table, struct vfs *fs,
                              struct vfs_context *context, const char *prefix);
enum vfs_status vfs_mount_initramfs(struct vfs_mount_table *table,
                                    struct vfs *fs,
                                    struct vfs_context *context,
                                    const char *prefix, const void *bytes,
                                    size_t size);
enum vfs_status vfs_mount_lookup(const struct vfs_mount_table *table,
                                 const char *path,
                                 const struct vfs_mount **mount_out,
                                 const char **relative_out);
uint32_t vfs_mount_count(const struct vfs_mount_table *table);
const struct vfs_mount *vfs_mount_at(const struct vfs_mount_table *table,
                                     uint32_t index);

#endif
