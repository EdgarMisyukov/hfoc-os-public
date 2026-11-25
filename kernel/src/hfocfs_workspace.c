/*
 * ITEM-631: the declared persistent workspace range and its VFS bridge.
 *
 * This unit never sees a host path or a whole disk.  The caller hands it the
 * already-bound persistence partition; it binds exactly blocks 514..33281 of
 * that partition, leaves the eight fixed state stores (0..511), the two
 * host-seed companion slots (512..513) and the custody last block alone, and
 * refuses an unknown non-zero first use.
 */
#include "hfocfs_workspace.h"

#include "appliance_state_block_device.h"

#include <string.h>

#define WS_PATH_MAX HFOCFS_MAX_PATH
#define WS_CHUNK 4096U

/*
 * ITEM-635: the workspace must start after the state layout and the two
 * host-seed companion slots.  A start at 512 wrote an HFOCFS superblock over
 * the first host-seed slot, so appliance_host_seed_store_present() reported a
 * seed that was not there and the console started the entropy login prompt.
 * That prompt consumed the console line the update test typed, which looked
 * like PS/2 input disappearing.
 */
_Static_assert(HFOCFS_WORKSPACE_OFFSET_BLOCKS ==
                   (uint64_t)APPLIANCE_STATE_LAYOUT_DEFAULT_CAPACITY_BLOCKS +
                       (uint64_t)APPLIANCE_HOST_SEED_STATE_SLOT_COUNT,
               "the workspace must start after the host-seed slots");
_Static_assert(HFOCFS_WORKSPACE_OFFSET_BLOCKS +
                       HFOCFS_WORKSPACE_IMAGE_BLOCKS <
                   HFOCFS_WORKSPACE_PARTITION_BLOCKS,
               "the workspace image must end before the custody block");

static struct hfocfs_workspace *active_workspace;
static struct vfs *active_vfs;
static struct vfs_context *active_context;
static bool active_committing;

static enum hfocfs_workspace_status ws_from_hfocfs(
    enum hfocfs_status status)
{
    switch (status) {
    case HFOCFS_OK:
        return HFOCFS_WORKSPACE_OK;
    case HFOCFS_NULL:
    case HFOCFS_BAD_ARGUMENT:
        return HFOCFS_WORKSPACE_NULL;
    case HFOCFS_TOO_SMALL:
    case HFOCFS_TOO_LARGE:
        return HFOCFS_WORKSPACE_BAD_TARGET;
    case HFOCFS_IO:
        return HFOCFS_WORKSPACE_IO;
    case HFOCFS_CORRUPT:
    case HFOCFS_UNSUPPORTED:
        return HFOCFS_WORKSPACE_CORRUPT;
    case HFOCFS_NOSPC:
    case HFOCFS_NO_MEMORY:
        return HFOCFS_WORKSPACE_NO_SPACE;
    case HFOCFS_READ_ONLY:
    case HFOCFS_BUSY:
    case HFOCFS_NOT_MOUNTED:
    case HFOCFS_NOT_FOUND:
    case HFOCFS_EXISTS:
    case HFOCFS_NOT_DIRECTORY:
    case HFOCFS_IS_DIRECTORY:
    case HFOCFS_NOT_EMPTY:
    case HFOCFS_NAME_TOO_LONG:
    case HFOCFS_PATH_TOO_LONG:
        return HFOCFS_WORKSPACE_VFS;
    }
    return HFOCFS_WORKSPACE_IO;
}

static enum hfocfs_workspace_status ws_path_join(char *destination,
                                                 size_t capacity,
                                                 const char *base,
                                                 const char *name)
{
    size_t base_length = strlen(base);
    size_t name_length = strlen(name);

    if (base_length + 1U + name_length + 1U > capacity) {
        return HFOCFS_WORKSPACE_VFS;
    }
    memcpy(destination, base, base_length);
    if (base_length != 1U || base[0] != '/') {
        destination[base_length++] = '/';
    }
    memcpy(destination + base_length, name, name_length);
    destination[base_length + name_length] = '\0';
    return HFOCFS_WORKSPACE_OK;
}

static uint32_t ws_vfs_child(const struct vfs *fs, uint32_t parent,
                             const char *name, size_t length)
{
    uint32_t child = fs->nodes[parent].first_child;

    while (child != UINT32_MAX && child < VFS_MAX_NODES) {
        const struct vfs_node *node = &fs->nodes[child];

        if (strlen(node->name) == length &&
            memcmp(node->name, name, length) == 0) {
            return child;
        }
        child = node->next_sibling;
    }
    return UINT32_MAX;
}

static enum hfocfs_workspace_status ws_vfs_resolve(struct vfs *fs,
                                                   const char *path,
                                                   uint32_t *node_out)
{
    uint32_t current = fs->root;
    const char *cursor = path;

    if (fs == NULL || path == NULL || node_out == NULL ||
        path[0] != '/') {
        return HFOCFS_WORKSPACE_NULL;
    }
    while (*cursor != '\0') {
        const char *start;
        size_t length;

        while (*cursor == '/') {
            ++cursor;
        }
        if (*cursor == '\0') {
            break;
        }
        start = cursor;
        while (*cursor != '\0' && *cursor != '/') {
            ++cursor;
        }
        length = (size_t)(cursor - start);
        if (length > VFS_NAME_MAX) {
            return HFOCFS_WORKSPACE_VFS;
        }
        current = ws_vfs_child(fs, current, start, length);
        if (current == UINT32_MAX) {
            return HFOCFS_WORKSPACE_VFS;
        }
    }
    *node_out = current;
    return HFOCFS_WORKSPACE_OK;
}

static enum hfocfs_workspace_status ws_vfs_mkdir(struct vfs *fs,
                                                 struct vfs_context *context,
                                                 const char *path)
{
    enum vfs_status status;

    (void)fs;
    status = vfs_mkdir(context, path);

    if (status == VFS_OK || status == VFS_EXISTS) {
        return HFOCFS_WORKSPACE_OK;
    }
    return HFOCFS_WORKSPACE_VFS;
}

static enum hfocfs_workspace_status ws_vfs_clear_dir(
    struct vfs *fs, struct vfs_context *context, const char *path)
{
    uint32_t node;
    enum hfocfs_workspace_status status = ws_vfs_resolve(fs, path, &node);

    if (status != HFOCFS_WORKSPACE_OK) {
        return status;
    }
    if (fs->nodes[node].type != VFS_NODE_DIR) {
        return HFOCFS_WORKSPACE_VFS;
    }
    while (fs->nodes[node].first_child != UINT32_MAX) {
        uint32_t child = fs->nodes[node].first_child;
        char child_path[WS_PATH_MAX];

        status = ws_path_join(child_path, sizeof(child_path), path,
                              fs->nodes[child].name);
        if (status != HFOCFS_WORKSPACE_OK) {
            return status;
        }
        if (fs->nodes[child].type == VFS_NODE_DIR) {
            status = ws_vfs_clear_dir(fs, context, child_path);
            if (status != HFOCFS_WORKSPACE_OK) {
                return status;
            }
        }
        if (vfs_unlink(context, child_path) != VFS_OK) {
            return HFOCFS_WORKSPACE_VFS;
        }
    }
    return HFOCFS_WORKSPACE_OK;
}

static enum hfocfs_workspace_status ws_clear_hfocfs_dir(
    struct hfocfs *fs, const char *path, uint32_t ino)
{
    for (;;) {
        uint32_t child;
        enum hfocfs_node_type type;
        char name[HFOCFS_MAX_NAME + 1U];
        char child_path[WS_PATH_MAX];
        enum hfocfs_status status;

        status = hfocfs_readdir(fs, ino, 0U, name, sizeof(name), &child,
                                &type);
        if (status == HFOCFS_NOT_FOUND) {
            return HFOCFS_WORKSPACE_OK;
        }
        if (status != HFOCFS_OK) {
            return ws_from_hfocfs(status);
        }
        if (ws_path_join(child_path, sizeof(child_path), path, name) !=
            HFOCFS_WORKSPACE_OK) {
            return HFOCFS_WORKSPACE_VFS;
        }
        if (type == HFOCFS_NODE_DIRECTORY) {
            enum hfocfs_workspace_status clear_status =
                ws_clear_hfocfs_dir(fs, child_path, child);

            if (clear_status != HFOCFS_WORKSPACE_OK) {
                return clear_status;
            }
        }
        status = hfocfs_unlink(fs, child_path);
        if (status != HFOCFS_OK) {
            return ws_from_hfocfs(status);
        }
    }
}

static enum hfocfs_workspace_status ws_copy_hfocfs_dir(
    struct hfocfs_workspace *workspace, struct vfs *vfs,
    struct vfs_context *context, const char *hfocfs_path,
    const char *vfs_path, uint32_t ino)
{
    uint32_t index = 0U;

    for (;;) {
        uint32_t child;
        enum hfocfs_node_type type;
        char name[HFOCFS_MAX_NAME + 1U];
        char child_hfocfs[WS_PATH_MAX];
        char child_vfs[WS_PATH_MAX];
        enum hfocfs_status hfocfs_status;

        hfocfs_status = hfocfs_readdir(workspace->fs, ino, index, name,
                                       sizeof(name), &child, &type);
        if (hfocfs_status == HFOCFS_NOT_FOUND) {
            return HFOCFS_WORKSPACE_OK;
        }
        if (hfocfs_status != HFOCFS_OK) {
            return ws_from_hfocfs(hfocfs_status);
        }
        if (ws_path_join(child_hfocfs, sizeof(child_hfocfs), hfocfs_path,
                         name) != HFOCFS_WORKSPACE_OK ||
            ws_path_join(child_vfs, sizeof(child_vfs), vfs_path, name) !=
                HFOCFS_WORKSPACE_OK) {
            return HFOCFS_WORKSPACE_VFS;
        }
        if (type == HFOCFS_NODE_DIRECTORY) {
            enum hfocfs_workspace_status status =
                ws_vfs_mkdir(vfs, context, child_vfs);

            if (status != HFOCFS_WORKSPACE_OK) {
                return status;
            }
            status = ws_copy_hfocfs_dir(workspace, vfs, context,
                                        child_hfocfs, child_vfs, child);
            if (status != HFOCFS_WORKSPACE_OK) {
                return status;
            }
        } else {
            struct vfs_file *file = NULL;
            uint32_t offset = 0U;
            unsigned char buffer[WS_CHUNK];

            if (vfs_open(context, child_vfs, VFS_OPEN_WRITE | VFS_OPEN_CREATE |
                         VFS_OPEN_TRUNC, &file) != VFS_OK) {
                return HFOCFS_WORKSPACE_VFS;
            }
            for (;;) {
                size_t done = 0U;
                size_t written = 0U;

                hfocfs_status = hfocfs_read(workspace->fs, child, offset,
                                            buffer, sizeof(buffer), &done);
                if (hfocfs_status != HFOCFS_OK) {
                    (void)vfs_close(file);
                    return ws_from_hfocfs(hfocfs_status);
                }
                if (done == 0U) {
                    break;
                }
                if (vfs_write(file, buffer, done, &written) != VFS_OK ||
                    written != done) {
                    (void)vfs_close(file);
                    return HFOCFS_WORKSPACE_VFS;
                }
                offset += (uint32_t)done;
            }
            if (vfs_close(file) != VFS_OK) {
                return HFOCFS_WORKSPACE_VFS;
            }
        }
        ++index;
    }
}

static enum hfocfs_workspace_status ws_export_vfs_dir(
    struct hfocfs_workspace *workspace, struct vfs *vfs,
    struct vfs_context *context, const char *vfs_path,
    const char *hfocfs_path)
{
    uint32_t node;
    uint32_t child;
    enum hfocfs_workspace_status status =
        ws_vfs_resolve(vfs, vfs_path, &node);

    if (status != HFOCFS_WORKSPACE_OK) {
        return status;
    }
    if (vfs->nodes[node].type != VFS_NODE_DIR) {
        return HFOCFS_WORKSPACE_VFS;
    }
    child = vfs->nodes[node].first_child;
    while (child != UINT32_MAX && child < VFS_MAX_NODES) {
        char child_vfs[WS_PATH_MAX];
        char child_hfocfs[WS_PATH_MAX];
        enum hfocfs_status hfocfs_status;
        uint32_t ino;

        status = ws_path_join(child_vfs, sizeof(child_vfs), vfs_path,
                              vfs->nodes[child].name);
        if (status != HFOCFS_WORKSPACE_OK) {
            return status;
        }
        status = ws_path_join(child_hfocfs, sizeof(child_hfocfs),
                              hfocfs_path, vfs->nodes[child].name);
        if (status != HFOCFS_WORKSPACE_OK) {
            return status;
        }
        if (vfs->nodes[child].type == VFS_NODE_DIR) {
            hfocfs_status = hfocfs_create(workspace->fs, child_hfocfs,
                                          HFOCFS_NODE_DIRECTORY, 0755U,
                                          &ino);
            if (hfocfs_status != HFOCFS_OK &&
                hfocfs_status != HFOCFS_EXISTS) {
                return ws_from_hfocfs(hfocfs_status);
            }
            status = ws_export_vfs_dir(workspace, vfs, context, child_vfs,
                                       child_hfocfs);
            if (status != HFOCFS_WORKSPACE_OK) {
                return status;
            }
        } else {
            struct vfs_file *file = NULL;
            uint32_t offset = 0U;

            hfocfs_status = hfocfs_create(workspace->fs, child_hfocfs,
                                          HFOCFS_NODE_FILE, 0644U, &ino);
            if (hfocfs_status != HFOCFS_OK &&
                hfocfs_status != HFOCFS_EXISTS) {
                return ws_from_hfocfs(hfocfs_status);
            }
            if (hfocfs_status == HFOCFS_EXISTS) {
                hfocfs_status = hfocfs_lookup(workspace->fs, child_hfocfs,
                                              &ino);
                if (hfocfs_status != HFOCFS_OK ||
                    hfocfs_truncate(workspace->fs, ino) != HFOCFS_OK) {
                    return ws_from_hfocfs(hfocfs_status);
                }
            }
            if (vfs_open(context, child_vfs, VFS_OPEN_READ, &file) !=
                VFS_OK) {
                return HFOCFS_WORKSPACE_VFS;
            }
            for (;;) {
                unsigned char buffer[WS_CHUNK];
                size_t done = 0U;
                size_t written = 0U;

                if (vfs_read(file, buffer, sizeof(buffer), &done) != VFS_OK) {
                    (void)vfs_close(file);
                    return HFOCFS_WORKSPACE_VFS;
                }
                if (done == 0U) {
                    break;
                }
                hfocfs_status = hfocfs_write(workspace->fs, ino, offset,
                                             buffer, done, &written);
                if (hfocfs_status != HFOCFS_OK || written != done) {
                    (void)vfs_close(file);
                    return ws_from_hfocfs(hfocfs_status);
                }
                offset += (uint32_t)done;
            }
            if (vfs_close(file) != VFS_OK) {
                return HFOCFS_WORKSPACE_VFS;
            }
        }
        child = vfs->nodes[child].next_sibling;
    }
    return HFOCFS_WORKSPACE_OK;
}

static enum hfocfs_workspace_status ws_reset(
    struct hfocfs_workspace *workspace)
{
    struct hfocfs *fs = NULL;
    enum hfocfs_status status = hfocfs_mount(
        &workspace->image, workspace->state, workspace->state_size, &fs);

    if (status != HFOCFS_OK) {
        workspace->bound = false;
        workspace->fs = NULL;
        return ws_from_hfocfs(status);
    }
    workspace->fs = fs;
    return HFOCFS_WORKSPACE_OK;
}

enum hfocfs_workspace_status hfocfs_workspace_bind(
    struct block_device *persistence_partition,
    void *state, size_t state_size, bool format_empty,
    struct hfocfs_workspace *workspace)
{
    struct appliance_state_block_device range;
    unsigned char first[512];
    unsigned char zero[512];
    bool zero_first = true;
    enum hfocfs_status status;
    size_t index;

    if (persistence_partition == NULL || state == NULL ||
        workspace == NULL) {
        return HFOCFS_WORKSPACE_NULL;
    }
    if (block_device_validate(persistence_partition) != BLOCK_DEVICE_OK ||
        persistence_partition->block_size != 512U ||
        persistence_partition->block_count <
            HFOCFS_WORKSPACE_PARTITION_BLOCKS) {
        return HFOCFS_WORKSPACE_BAD_TARGET;
    }
    if (state_size < hfocfs_state_size() ||
        ((uintptr_t)state % hfocfs_state_alignment()) != 0U) {
        return HFOCFS_WORKSPACE_NULL;
    }
    if (appliance_state_block_device_bind_range(
            persistence_partition, HFOCFS_WORKSPACE_OFFSET_BLOCKS,
            HFOCFS_WORKSPACE_IMAGE_BLOCKS, &range) !=
        APPLIANCE_STATE_BLOCK_DEVICE_OK) {
        return HFOCFS_WORKSPACE_BAD_TARGET;
    }
    memset(workspace, 0, sizeof(*workspace));
    workspace->range = range;
    workspace->range.device.context = &workspace->range;
    workspace->image = workspace->range.device;
    workspace->state = state;
    workspace->state_size = state_size;

    status = block_device_read(&workspace->image, 0U, first, sizeof(first)) ==
                     BLOCK_DEVICE_OK
                 ? HFOCFS_OK : HFOCFS_IO;
    if (status != HFOCFS_OK) {
        return HFOCFS_WORKSPACE_IO;
    }
    memset(zero, 0, sizeof(zero));
    for (index = 0U; index < sizeof(first); ++index) {
        if (first[index] != 0U) {
            zero_first = false;
            break;
        }
    }
    if (memcmp(first, HFOCFS_MAGIC, HFOCFS_MAGIC_SIZE) == 0) {
        status = hfocfs_mount(&workspace->image, state, state_size,
                              &workspace->fs);
    } else if (zero_first && format_empty) {
        status = hfocfs_format(&workspace->image, NULL, state, state_size);
        if (status == HFOCFS_OK) {
            status = hfocfs_mount(&workspace->image, state, state_size,
                                  &workspace->fs);
        }
    } else if (zero_first) {
        return HFOCFS_WORKSPACE_UNINITIALIZED;
    } else {
        return HFOCFS_WORKSPACE_REFUSED;
    }
    if (status != HFOCFS_OK) {
        memset(workspace, 0, sizeof(*workspace));
        return ws_from_hfocfs(status);
    }
    workspace->bound = true;
    return HFOCFS_WORKSPACE_OK;
}

enum hfocfs_workspace_status hfocfs_workspace_import(
    struct hfocfs_workspace *workspace, struct vfs *fs,
    struct vfs_context *context)
{
    uint32_t ino = 0U;
    enum hfocfs_status hfocfs_status;
    enum hfocfs_workspace_status status;

    if (workspace == NULL || fs == NULL || context == NULL) {
        return HFOCFS_WORKSPACE_NULL;
    }
    if (!workspace->bound) {
        return HFOCFS_WORKSPACE_NOT_BOUND;
    }
    status = ws_vfs_mkdir(fs, context, "/home");
    if (status == HFOCFS_WORKSPACE_OK) {
        status = ws_vfs_mkdir(fs, context, HFOCFS_WORKSPACE_PREFIX);
    }
    if (status == HFOCFS_WORKSPACE_OK) {
        status = ws_vfs_clear_dir(fs, context, HFOCFS_WORKSPACE_PREFIX);
    }
    if (status != HFOCFS_WORKSPACE_OK) {
        return status;
    }
    {
        uint32_t home = 0U;

        hfocfs_status = hfocfs_lookup(workspace->fs, "/home", &home);
        if (hfocfs_status == HFOCFS_NOT_FOUND) {
            hfocfs_status = hfocfs_create(workspace->fs, "/home",
                                          HFOCFS_NODE_DIRECTORY, 0755U,
                                          &home);
        }
        if (hfocfs_status != HFOCFS_OK) {
            return ws_from_hfocfs(hfocfs_status);
        }
    }
    hfocfs_status = hfocfs_lookup(workspace->fs, HFOCFS_WORKSPACE_PREFIX,
                                  &ino);
    if (hfocfs_status == HFOCFS_NOT_FOUND) {
        hfocfs_status = hfocfs_create(workspace->fs,
                                      HFOCFS_WORKSPACE_PREFIX,
                                      HFOCFS_NODE_DIRECTORY, 0755U, &ino);
        if (hfocfs_status != HFOCFS_OK) {
            return ws_from_hfocfs(hfocfs_status);
        }
    } else if (hfocfs_status != HFOCFS_OK) {
        return ws_from_hfocfs(hfocfs_status);
    }
    return ws_copy_hfocfs_dir(workspace, fs, context,
                              HFOCFS_WORKSPACE_PREFIX,
                              HFOCFS_WORKSPACE_PREFIX, ino);
}

enum hfocfs_workspace_status hfocfs_workspace_sync(
    struct hfocfs_workspace *workspace, struct vfs *fs,
    struct vfs_context *context)
{
    uint32_t home = 0U;
    uint32_t owner = 0U;
    enum hfocfs_status hfocfs_status;
    enum hfocfs_workspace_status status;

    if (workspace == NULL || fs == NULL || context == NULL) {
        return HFOCFS_WORKSPACE_NULL;
    }
    if (!workspace->bound) {
        return HFOCFS_WORKSPACE_NOT_BOUND;
    }
    hfocfs_status = hfocfs_lookup(workspace->fs, "/home", &home);
    if (hfocfs_status == HFOCFS_NOT_FOUND) {
        hfocfs_status = hfocfs_create(workspace->fs, "/home",
                                      HFOCFS_NODE_DIRECTORY, 0755U, &home);
    }
    if (hfocfs_status != HFOCFS_OK) {
        return ws_from_hfocfs(hfocfs_status);
    }
    hfocfs_status = hfocfs_lookup(workspace->fs, HFOCFS_WORKSPACE_PREFIX,
                                  &owner);
    if (hfocfs_status == HFOCFS_NOT_FOUND) {
        hfocfs_status = hfocfs_create(workspace->fs,
                                      HFOCFS_WORKSPACE_PREFIX,
                                      HFOCFS_NODE_DIRECTORY, 0755U, &owner);
    }
    if (hfocfs_status != HFOCFS_OK) {
        return ws_from_hfocfs(hfocfs_status);
    }
    status = ws_clear_hfocfs_dir(workspace->fs, HFOCFS_WORKSPACE_PREFIX,
                                 owner);
    if (status == HFOCFS_WORKSPACE_OK) {
        status = ws_export_vfs_dir(workspace, fs, context,
                                   HFOCFS_WORKSPACE_PREFIX,
                                   HFOCFS_WORKSPACE_PREFIX);
    }
    if (status == HFOCFS_WORKSPACE_OK) {
        hfocfs_status = hfocfs_flush(workspace->fs);
        if (hfocfs_status != HFOCFS_OK) {
            status = ws_from_hfocfs(hfocfs_status);
        }
    }
    if (status != HFOCFS_WORKSPACE_OK) {
        (void)ws_reset(workspace);
    }
    return status;
}

/*
 * ITEM-635: the production bridge.  The boot path publishes the one bound
 * workspace so the file syscalls can commit it, and the commit path is
 * re-entrancy guarded because it itself calls VFS helpers.
 */
void hfocfs_workspace_activate(struct hfocfs_workspace *workspace,
                               struct vfs *fs, struct vfs_context *context)
{
    active_workspace = workspace;
    active_vfs = fs;
    active_context = context;
    active_committing = false;
}

int hfocfs_workspace_commit(void)
{
    enum hfocfs_workspace_status status;

    if (active_workspace == NULL || active_vfs == NULL ||
        active_context == NULL || active_committing) {
        return 0;
    }
    active_committing = true;
    status = hfocfs_workspace_sync(active_workspace, active_vfs,
                                   active_context);
    active_committing = false;
    return status == HFOCFS_WORKSPACE_OK ? 0 : -1;
}

const char *hfocfs_workspace_status_string(
    enum hfocfs_workspace_status status)
{
    switch (status) {
    case HFOCFS_WORKSPACE_OK: return "OK";
    case HFOCFS_WORKSPACE_NULL: return "NULL";
    case HFOCFS_WORKSPACE_BAD_DEVICE: return "BAD_DEVICE";
    case HFOCFS_WORKSPACE_BAD_TARGET: return "BAD_TARGET";
    case HFOCFS_WORKSPACE_REFUSED: return "REFUSED";
    case HFOCFS_WORKSPACE_IO: return "IO";
    case HFOCFS_WORKSPACE_CORRUPT: return "CORRUPT";
    case HFOCFS_WORKSPACE_VFS: return "VFS";
    case HFOCFS_WORKSPACE_NO_SPACE: return "NO_SPACE";
    case HFOCFS_WORKSPACE_NOT_BOUND: return "NOT_BOUND";
    case HFOCFS_WORKSPACE_UNINITIALIZED: return "UNINITIALIZED";
    }
    return "UNKNOWN";
}
