/*
 * ITEM-623: bounded RAM/initramfs mount table.
 *
 * A mount record is published only after the tree behind it exists.  The
 * check is done before publication, so a failed mount leaves no half-record
 * and no caller-visible tree without a map entry.
 */

#include "vfs_mount.h"

#include <string.h>

static bool vfs_mount_prefix_valid(const char *prefix)
{
    size_t length;
    size_t index;

    if (prefix == NULL || prefix[0] != '/') {
        return false;
    }
    length = strlen(prefix);
    if (length == 0U || length >= VFS_PATH_MAX) {
        return false;
    }
    if (length == 1U) {
        return true;
    }
    if (prefix[length - 1U] == '/') {
        return false;
    }
    for (index = 1U; index <= length; ++index) {
        size_t start;
        size_t component;

        while (index < length && prefix[index] == '/') {
            ++index;
        }
        start = index;
        while (index < length && prefix[index] != '/') {
            ++index;
        }
        component = index - start;
        if (component == 0U || component > VFS_NAME_MAX) {
            return false;
        }
        if ((component == 1U && prefix[start] == '.') ||
            (component == 2U && prefix[start] == '.' &&
             prefix[start + 1U] == '.')) {
            return false;
        }
    }
    return true;
}

enum vfs_status vfs_mount_table_init(struct vfs_mount_table *table)
{
    if (table == NULL) {
        return VFS_NULL;
    }
    memset(table, 0, sizeof(*table));
    table->initialized = true;
    return VFS_OK;
}

static struct vfs_mount *vfs_mount_free_slot(struct vfs_mount_table *table)
{
    uint32_t index;

    for (index = 0U; index < VFS_MAX_MOUNTS; ++index) {
        if (!table->mounts[index].used) {
            return &table->mounts[index];
        }
    }
    return NULL;
}

static enum vfs_status vfs_mount_record(struct vfs_mount_table *table,
                                        const char *prefix,
                                        enum vfs_mount_kind kind,
                                        bool read_only, uint32_t node)
{
    struct vfs_mount *mount = vfs_mount_free_slot(table);

    if (mount == NULL) {
        return VFS_NO_SPACE;
    }
    memset(mount, 0, sizeof(*mount));
    mount->used = true;
    mount->kind = kind;
    mount->read_only = read_only;
    mount->node = node;
    memcpy(mount->prefix, prefix, strlen(prefix) + 1U);
    ++table->count;
    return VFS_OK;
}

static enum vfs_status vfs_mount_check_new(const struct vfs_mount_table *table,
                                           const char *prefix)
{
    uint32_t index;

    if (!vfs_mount_prefix_valid(prefix)) {
        return VFS_INVALID;
    }
    for (index = 0U; index < VFS_MAX_MOUNTS; ++index) {
        if (table->mounts[index].used &&
            strcmp(table->mounts[index].prefix, prefix) == 0) {
            return VFS_BUSY;
        }
    }
    return VFS_OK;
}

enum vfs_status vfs_mount_ram(struct vfs_mount_table *table, struct vfs *fs,
                              struct vfs_context *context, const char *prefix)
{
    struct vfs_stat stat;
    enum vfs_status status;

    if (table == NULL || fs == NULL || context == NULL || prefix == NULL) {
        return VFS_NULL;
    }
    if (!table->initialized || context->fs != fs) {
        return VFS_BAD_STATE;
    }
    status = vfs_mount_check_new(table, prefix);
    if (status != VFS_OK) {
        return status;
    }
    if (vfs_stat(context, prefix, &stat) != VFS_OK) {
        if (strcmp(prefix, "/") != 0) {
            status = vfs_mkdir(context, prefix);
            if (status != VFS_OK) {
                return status;
            }
        }
    } else if (stat.type != VFS_NODE_DIR) {
        return VFS_NOT_DIR;
    }
    if (vfs_stat(context, prefix, &stat) != VFS_OK) {
        return VFS_BAD_STATE;
    }
    return vfs_mount_record(table, prefix, VFS_MOUNT_RAM, false, 0U);
}

enum vfs_status vfs_mount_initramfs(struct vfs_mount_table *table,
                                    struct vfs *fs,
                                    struct vfs_context *context,
                                    const char *prefix, const void *bytes,
                                    size_t size)
{
    /*
     * ITEM-623: the evaluated archive is 9 KiB.  The boot stack is a 16 KiB
     * kernel stack, so keeping this much state on it corrupts the frames
     * below and, on the boot stack, overwrites the page tables.  The mount
     * is a single boot-time operation; one static evaluation slot keeps the
     * frame small and is overwritten before every parse.
     */
    static struct initramfs_archive archive;
    struct vfs_stat stat;
    enum vfs_status status;

    if (table == NULL || fs == NULL || context == NULL || prefix == NULL ||
        bytes == NULL) {
        return VFS_NULL;
    }
    if (!table->initialized || context->fs != fs) {
        return VFS_BAD_STATE;
    }
    status = vfs_mount_check_new(table, prefix);
    if (status != VFS_OK) {
        return status;
    }
    if (strcmp(prefix, "/") != 0) {
        if (vfs_stat(context, prefix, &stat) != VFS_OK ||
            stat.type != VFS_NODE_DIR) {
            return VFS_NOT_FOUND;
        }
    }
    if (initramfs_parse(bytes, size, &archive) != INITRAMFS_OK) {
        return VFS_INVALID;
    }
    if (initramfs_import(&archive, fs, context, prefix) != INITRAMFS_OK) {
        return VFS_BAD_STATE;
    }
    if (vfs_stat(context, prefix, &stat) != VFS_OK) {
        return VFS_BAD_STATE;
    }
    return vfs_mount_record(table, prefix, VFS_MOUNT_INITRAMFS, true, 0U);
}

enum vfs_status vfs_mount_lookup(const struct vfs_mount_table *table,
                                 const char *path,
                                 const struct vfs_mount **mount_out,
                                 const char **relative_out)
{
    uint32_t index;
    const struct vfs_mount *best = NULL;

    if (table == NULL || path == NULL || mount_out == NULL ||
        relative_out == NULL) {
        return VFS_NULL;
    }
    for (index = 0U; index < VFS_MAX_MOUNTS; ++index) {
        const struct vfs_mount *mount = &table->mounts[index];
        const size_t length = strlen(mount->prefix);

        if (!mount->used) {
            continue;
        }
        if (strcmp(mount->prefix, "/") == 0) {
            if (best == NULL) {
                best = mount;
            }
            continue;
        }
        if (strncmp(path, mount->prefix, length) == 0 &&
            (path[length] == '\0' || path[length] == '/') &&
            (best == NULL || strlen(best->prefix) < length)) {
            best = mount;
        }
    }
    if (best == NULL) {
        return VFS_NOT_FOUND;
    }
    *mount_out = best;
    if (strcmp(best->prefix, "/") == 0) {
        *relative_out = path;
    } else {
        *relative_out = path + strlen(best->prefix);
    }
    return VFS_OK;
}

uint32_t vfs_mount_count(const struct vfs_mount_table *table)
{
    return table != NULL ? table->count : 0U;
}

const struct vfs_mount *vfs_mount_at(const struct vfs_mount_table *table,
                                     uint32_t index)
{
    if (table == NULL || index >= VFS_MAX_MOUNTS ||
        !table->mounts[index].used) {
        return NULL;
    }
    return &table->mounts[index];
}
