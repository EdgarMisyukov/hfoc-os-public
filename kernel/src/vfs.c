/*
 * ITEM-622: Unix path semantics over the bounded RAM filesystem.
 *
 * The namespace is a single tree rooted at "/".  Resolution accepts repeated
 * separators, "." and ".." (which stops at the root), refuses a component
 * longer than 255 bytes and a path longer than 1023 bytes, and never follows
 * a symlink: this unit has no symlinks and returns UNSUPPORTED for the
 * operations that would need them.  Names are compared byte for byte; no
 * locale, case folding or host path is involved.
 *
 * An open file owns a node reference.  Unlinking an open file removes it from
 * its directory but keeps its bytes until the last close, which is the Unix
 * guarantee ITEM-623 will expose through a descriptor.
 */

#include "vfs.h"

#include "ramfs.h"
#include "vfs_fd.h"

#include <string.h>

#define VFS_NO_NODE UINT32_MAX

static bool vfs_ready(const struct vfs *fs)
{
    return fs != NULL && fs->initialized && fs->root < VFS_MAX_NODES &&
        fs->nodes[fs->root].used;
}

bool vfs_path_read_only(const struct vfs *fs, uint32_t node)
{
    uint32_t cursor = node;

    while (cursor != VFS_NO_NODE && cursor < VFS_MAX_NODES) {
        if (fs->nodes[cursor].read_only) {
            return true;
        }
        cursor = fs->nodes[cursor].parent;
    }
    return false;
}

static bool vfs_name_equal(const struct vfs_node *node, const char *name,
                           size_t length)
{
    return strlen(node->name) == length &&
        memcmp(node->name, name, length) == 0;
}

static uint32_t vfs_child_find(const struct vfs *fs, uint32_t parent,
                               const char *name, size_t length)
{
    uint32_t child = fs->nodes[parent].first_child;

    while (child != VFS_NO_NODE && child < VFS_MAX_NODES) {
        if (vfs_name_equal(&fs->nodes[child], name, length)) {
            return child;
        }
        child = fs->nodes[child].next_sibling;
    }
    return VFS_NO_NODE;
}

static void vfs_node_link(struct vfs *fs, uint32_t parent, uint32_t child)
{
    fs->nodes[child].parent = parent;
    fs->nodes[child].next_sibling = fs->nodes[parent].first_child;
    fs->nodes[parent].first_child = child;
}

static void vfs_node_unlink(struct vfs *fs, uint32_t child)
{
    const uint32_t parent = fs->nodes[child].parent;
    uint32_t cursor;

    if (parent >= VFS_MAX_NODES) {
        return;
    }
    cursor = fs->nodes[parent].first_child;
    if (cursor == child) {
        fs->nodes[parent].first_child = fs->nodes[child].next_sibling;
    } else {
        while (cursor != VFS_NO_NODE && cursor < VFS_MAX_NODES &&
               fs->nodes[cursor].next_sibling != child) {
            cursor = fs->nodes[cursor].next_sibling;
        }
        if (cursor != VFS_NO_NODE && cursor < VFS_MAX_NODES) {
            fs->nodes[cursor].next_sibling =
                fs->nodes[child].next_sibling;
        }
    }
    fs->nodes[child].next_sibling = VFS_NO_NODE;
}

static enum vfs_status vfs_copy_name(char *destination, size_t capacity,
                                     const char *source, size_t length)
{
    if (length == 0U) {
        return VFS_INVALID;
    }
    if (length > VFS_NAME_MAX) {
        return VFS_NAME_TOO_LONG;
    }
    if (length + 1U > capacity) {
        return VFS_NAME_TOO_LONG;
    }
    memcpy(destination, source, length);
    destination[length] = '\0';
    return VFS_OK;
}

/*
 * Resolve a path to an existing node.  `want_dir` requires the final node to
 * be a directory; a trailing slash does the same.  Returns the node index in
 * `node_out`.
 */
static enum vfs_status vfs_resolve(const struct vfs_context *context,
                                   const char *path, bool want_dir,
                                   uint32_t *node_out)
{
    uint32_t current;
    size_t index = 0U;
    size_t path_length;

    if (context == NULL || context->fs == NULL || path == NULL ||
        node_out == NULL) {
        return VFS_NULL;
    }
    if (!vfs_ready(context->fs)) {
        return VFS_BAD_STATE;
    }
    path_length = strlen(path);
    if (path_length == 0U) {
        return VFS_INVALID;
    }
    if (path_length >= VFS_PATH_MAX) {
        return VFS_PATH_TOO_LONG;
    }
    current = path[0] == '/' ? context->fs->root : context->cwd;
    if (!ramfs_node_valid(context->fs, current)) {
        return VFS_BAD_STATE;
    }
    while (index < path_length) {
        size_t start;
        size_t length;

        while (index < path_length && path[index] == '/') {
            ++index;
        }
        if (index == path_length) {
            break;
        }
        start = index;
        while (index < path_length && path[index] != '/') {
            ++index;
        }
        length = index - start;
        if (length == 1U && path[start] == '.') {
            continue;
        }
        if (length == 2U && path[start] == '.' && path[start + 1U] == '.') {
            const uint32_t parent = context->fs->nodes[current].parent;

            if (parent != VFS_NO_NODE && parent < VFS_MAX_NODES) {
                current = parent;
            }
            continue;
        }
        if (length > VFS_NAME_MAX) {
            return VFS_NAME_TOO_LONG;
        }
        if (context->fs->nodes[current].type != VFS_NODE_DIR) {
            return VFS_NOT_DIR;
        }
        {
            const uint32_t child =
                vfs_child_find(context->fs, current, path + start, length);

            if (child == VFS_NO_NODE) {
                return VFS_NOT_FOUND;
            }
            current = child;
        }
    }
    if (want_dir && context->fs->nodes[current].type != VFS_NODE_DIR) {
        return VFS_NOT_DIR;
    }
    if (path[path_length - 1U] == '/' &&
        context->fs->nodes[current].type != VFS_NODE_DIR) {
        return VFS_NOT_DIR;
    }
    *node_out = current;
    return VFS_OK;
}

/*
 * Resolve the parent directory and the final component of a path that may not
 * exist yet.  A path ending in "/" is refused for creation/rename: this unit
 * has no "rename into directory" shorthand.
 */
static enum vfs_status vfs_parent(const struct vfs_context *context,
                                  const char *path, uint32_t *parent_out,
                                  char *name_out, size_t *name_length_out)
{
    size_t path_length;
    size_t leaf;
    size_t parent_length;
    char parent_path[VFS_PATH_MAX];
    enum vfs_status status;
    uint32_t parent;

    if (context == NULL || path == NULL || parent_out == NULL ||
        name_out == NULL || name_length_out == NULL) {
        return VFS_NULL;
    }
    path_length = strlen(path);
    if (path_length == 0U) {
        return VFS_INVALID;
    }
    if (path_length >= VFS_PATH_MAX) {
        return VFS_PATH_TOO_LONG;
    }
    if (path[path_length - 1U] == '/') {
        return VFS_INVALID;
    }
    leaf = path_length;
    while (leaf > 0U && path[leaf - 1U] != '/') {
        --leaf;
    }
    if (path_length - leaf > VFS_NAME_MAX) {
        return VFS_NAME_TOO_LONG;
    }
    if (leaf == 0U) {
        parent = path[0] == '/' ? context->fs->root : context->cwd;
    } else {
        parent_length = leaf;
        if (parent_length >= sizeof(parent_path)) {
            return VFS_PATH_TOO_LONG;
        }
        memcpy(parent_path, path, parent_length);
        parent_path[parent_length] = '\0';
        status = vfs_resolve(context, parent_path, true, &parent);
        if (status != VFS_OK) {
            return status == VFS_NOT_FOUND ? VFS_NOT_FOUND : status;
        }
    }
    if (!ramfs_node_valid(context->fs, parent) ||
        context->fs->nodes[parent].type != VFS_NODE_DIR) {
        return VFS_NOT_DIR;
    }
    status = vfs_copy_name(name_out, VFS_NAME_MAX + 1U, path + leaf,
                           path_length - leaf);
    if (status != VFS_OK) {
        return status;
    }
    *parent_out = parent;
    *name_length_out = path_length - leaf;
    return VFS_OK;
}

enum vfs_status vfs_init(struct vfs *fs)
{
    uint32_t root;

    if (fs == NULL) {
        return VFS_NULL;
    }
    ramfs_reset(fs);
    fs->root = VFS_NO_NODE;
    if (!ramfs_node_alloc(fs, VFS_NODE_DIR, VFS_NO_NODE, "/", 1U, &root)) {
        return VFS_NO_SPACE;
    }
    fs->nodes[root].parent = VFS_NO_NODE;
    fs->root = root;
    fs->initialized = true;
    fs->generation = 1U;
    return VFS_OK;
}

enum vfs_status vfs_context_init(struct vfs_context *context,
                                 struct vfs *fs)
{
    if (context == NULL || fs == NULL) {
        return VFS_NULL;
    }
    if (!vfs_ready(fs)) {
        return VFS_BAD_STATE;
    }
    memset(context, 0, sizeof(*context));
    context->fs = fs;
    context->cwd = fs->root;
    return VFS_OK;
}

enum vfs_status vfs_open(struct vfs_context *context, const char *path,
                         uint32_t flags, struct vfs_file **file_out)
{
    struct vfs *fs;
    uint32_t parent;
    uint32_t node;
    char name[VFS_NAME_MAX + 1U];
    size_t name_length = 0U;
    enum vfs_status status;
    uint32_t slot;

    if (context == NULL || context->fs == NULL || path == NULL ||
        file_out == NULL) {
        return VFS_NULL;
    }
    *file_out = NULL;
    fs = context->fs;
    if (!vfs_ready(fs)) {
        return VFS_BAD_STATE;
    }
    if ((flags & (VFS_OPEN_READ | VFS_OPEN_WRITE)) == 0U ||
        (flags & ~(VFS_OPEN_READ | VFS_OPEN_WRITE | VFS_OPEN_CREATE |
                   VFS_OPEN_TRUNC)) != 0U) {
        return VFS_INVALID;
    }
    status = vfs_parent(context, path, &parent, name, &name_length);
    if (status != VFS_OK) {
        return status;
    }
    node = vfs_child_find(fs, parent, name, name_length);
    if (node == VFS_NO_NODE) {
        if ((flags & VFS_OPEN_CREATE) == 0U) {
            return VFS_NOT_FOUND;
        }
        if (vfs_path_read_only(fs, parent)) {
            return VFS_READ_ONLY;
        }
        if (!ramfs_node_alloc(fs, VFS_NODE_FILE, parent, name, name_length,
                              &node)) {
            return VFS_TOO_MANY_NODES;
        }
        vfs_node_link(fs, parent, node);
    } else if (fs->nodes[node].type != VFS_NODE_FILE) {
        return VFS_IS_DIR;
    }
    if ((flags & (VFS_OPEN_WRITE | VFS_OPEN_TRUNC)) != 0U &&
        vfs_path_read_only(fs, node)) {
        return VFS_READ_ONLY;
    }
    if ((flags & VFS_OPEN_TRUNC) != 0U) {
        if ((flags & VFS_OPEN_WRITE) == 0U) {
            return VFS_INVALID;
        }
        status = ramfs_truncate(fs, node);
        if (status != VFS_OK) {
            return status;
        }
    }
    for (slot = 0U; slot < VFS_MAX_FILES; ++slot) {
        if (!fs->files[slot].used) {
            struct vfs_file *file = &fs->files[slot];

            memset(file, 0, sizeof(*file));
            file->used = true;
            file->writable = (flags & VFS_OPEN_WRITE) != 0U;
            file->fs = fs;
            file->node = node;
            ++fs->nodes[node].open_count;
            ++fs->open_count;
            *file_out = file;
            return VFS_OK;
        }
    }
    return VFS_TOO_MANY_FILES;
}

static bool vfs_file_valid(const struct vfs_file *file)
{
    return file != NULL && file->used && file->fs != NULL &&
        ramfs_node_valid(file->fs, file->node);
}

enum vfs_status vfs_read(struct vfs_file *file, void *buffer, size_t size,
                         size_t *done_out)
{
    enum vfs_status status;

    if (done_out != NULL) {
        *done_out = 0U;
    }
    if (!vfs_file_valid(file) || buffer == NULL || done_out == NULL) {
        return VFS_BAD_FILE;
    }
    status = ramfs_read(file->fs, file->node, file->offset, buffer, size,
                        done_out);
    if (status == VFS_OK) {
        file->offset += (uint32_t)*done_out;
    }
    return status;
}

enum vfs_status vfs_seek(struct vfs_file *file, uint32_t offset)
{
    if (file == NULL) {
        return VFS_NULL;
    }
    if (!file->used || file->fs == NULL || offset > VFS_FILE_MAX) {
        return VFS_BAD_OFFSET;
    }
    file->offset = offset;
    return VFS_OK;
}

enum vfs_status vfs_write(struct vfs_file *file, const void *buffer,
                          size_t size, size_t *done_out)
{
    enum vfs_status status;

    if (done_out != NULL) {
        *done_out = 0U;
    }
    if (!vfs_file_valid(file) || buffer == NULL || done_out == NULL) {
        return VFS_BAD_FILE;
    }
    if (!file->writable) {
        return VFS_READ_ONLY;
    }
    status = ramfs_write(file->fs, file->node, file->offset, buffer, size,
                         done_out);
    if (status == VFS_OK) {
        file->offset += (uint32_t)*done_out;
    }
    return status;
}

enum vfs_status vfs_close(struct vfs_file *file)
{
    struct vfs *fs;
    uint32_t node;

    if (!vfs_file_valid(file)) {
        return VFS_BAD_FILE;
    }
    fs = file->fs;
    node = file->node;
    if (fs->nodes[node].open_count != 0U) {
        --fs->nodes[node].open_count;
    }
    if (fs->open_count != 0U) {
        --fs->open_count;
    }
    if (fs->nodes[node].unlinked && fs->nodes[node].open_count == 0U) {
        ramfs_node_free(fs, node);
    }
    memset(file, 0, sizeof(*file));
    return VFS_OK;
}

enum vfs_status vfs_stat(struct vfs_context *context, const char *path,
                         struct vfs_stat *stat_out)
{
    uint32_t node;
    enum vfs_status status;

    if (stat_out == NULL) {
        return VFS_NULL;
    }
    status = vfs_resolve(context, path, false, &node);
    if (status != VFS_OK) {
        return status;
    }
    memset(stat_out, 0, sizeof(*stat_out));
    stat_out->type = context->fs->nodes[node].type;
    stat_out->size = context->fs->nodes[node].size;
    stat_out->open_count = context->fs->nodes[node].open_count;
    stat_out->unlinked = context->fs->nodes[node].unlinked;
    stat_out->read_only = vfs_path_read_only(context->fs, node);
    return VFS_OK;
}

enum vfs_status vfs_mkdir(struct vfs_context *context, const char *path)
{
    struct vfs *fs;
    uint32_t parent;
    uint32_t node;
    char name[VFS_NAME_MAX + 1U];
    size_t name_length = 0U;
    enum vfs_status status;

    if (context == NULL || context->fs == NULL || path == NULL) {
        return VFS_NULL;
    }
    fs = context->fs;
    if (!vfs_ready(fs)) {
        return VFS_BAD_STATE;
    }
    status = vfs_parent(context, path, &parent, name, &name_length);
    if (status != VFS_OK) {
        return status;
    }
    if (vfs_path_read_only(fs, parent)) {
        return VFS_READ_ONLY;
    }
    if (vfs_child_find(fs, parent, name, name_length) != VFS_NO_NODE) {
        return VFS_EXISTS;
    }
    if (!ramfs_node_alloc(fs, VFS_NODE_DIR, parent, name, name_length,
                          &node)) {
        return VFS_TOO_MANY_NODES;
    }
    vfs_node_link(fs, parent, node);
    return VFS_OK;
}

static bool vfs_dir_empty(const struct vfs *fs, uint32_t node)
{
    return fs->nodes[node].first_child == VFS_NO_NODE;
}

enum vfs_status vfs_unlink(struct vfs_context *context, const char *path)
{
    struct vfs *fs;
    uint32_t parent;
    uint32_t node;
    char name[VFS_NAME_MAX + 1U];
    size_t name_length = 0U;
    enum vfs_status status;

    if (context == NULL || context->fs == NULL || path == NULL) {
        return VFS_NULL;
    }
    fs = context->fs;
    if (!vfs_ready(fs)) {
        return VFS_BAD_STATE;
    }
    if (strcmp(path, "/") == 0) {
        return VFS_BUSY;
    }
    status = vfs_parent(context, path, &parent, name, &name_length);
    if (status != VFS_OK) {
        return status;
    }
    node = vfs_child_find(fs, parent, name, name_length);
    if (node == VFS_NO_NODE) {
        return VFS_NOT_FOUND;
    }
    if (vfs_path_read_only(fs, node)) {
        return VFS_READ_ONLY;
    }
    if (fs->nodes[node].type == VFS_NODE_DIR &&
        !vfs_dir_empty(fs, node)) {
        return VFS_NOT_EMPTY;
    }
    if (context->cwd == node) {
        return VFS_BUSY;
    }
    vfs_node_unlink(fs, node);
    fs->nodes[node].unlinked = true;
    if (fs->nodes[node].open_count == 0U) {
        ramfs_node_free(fs, node);
    }
    ++fs->generation;
    return VFS_OK;
}

static bool vfs_is_ancestor(const struct vfs *fs, uint32_t ancestor,
                            uint32_t node)
{
    uint32_t cursor = node;

    while (cursor != VFS_NO_NODE && cursor < VFS_MAX_NODES) {
        if (cursor == ancestor) {
            return true;
        }
        cursor = fs->nodes[cursor].parent;
    }
    return false;
}

enum vfs_status vfs_rename(struct vfs_context *context, const char *old_path,
                           const char *new_path)
{
    struct vfs *fs;
    uint32_t old_parent;
    uint32_t new_parent;
    uint32_t node;
    char old_name[VFS_NAME_MAX + 1U];
    char new_name[VFS_NAME_MAX + 1U];
    size_t old_length = 0U;
    size_t new_length = 0U;
    enum vfs_status status;

    if (context == NULL || context->fs == NULL || old_path == NULL ||
        new_path == NULL) {
        return VFS_NULL;
    }
    fs = context->fs;
    if (!vfs_ready(fs)) {
        return VFS_BAD_STATE;
    }
    if (strcmp(old_path, "/") == 0 || strcmp(new_path, "/") == 0) {
        return VFS_BUSY;
    }
    status = vfs_parent(context, old_path, &old_parent, old_name,
                        &old_length);
    if (status != VFS_OK) {
        return status;
    }
    node = vfs_child_find(fs, old_parent, old_name, old_length);
    if (node == VFS_NO_NODE) {
        return VFS_NOT_FOUND;
    }
    if (vfs_path_read_only(fs, node)) {
        return VFS_READ_ONLY;
    }
    status = vfs_parent(context, new_path, &new_parent, new_name,
                        &new_length);
    if (status != VFS_OK) {
        return status;
    }
    if (vfs_path_read_only(fs, new_parent)) {
        return VFS_READ_ONLY;
    }
    if (vfs_child_find(fs, new_parent, new_name, new_length) != VFS_NO_NODE) {
        return VFS_EXISTS;
    }
    if (fs->nodes[node].type == VFS_NODE_DIR &&
        vfs_is_ancestor(fs, node, new_parent)) {
        return VFS_INVALID;
    }
    vfs_node_unlink(fs, node);
    memcpy(fs->nodes[node].name, new_name, new_length);
    fs->nodes[node].name[new_length] = '\0';
    vfs_node_link(fs, new_parent, node);
    ++fs->generation;
    return VFS_OK;
}

enum vfs_status vfs_set_read_only(struct vfs_context *context,
                                  const char *path)
{
    uint32_t node;
    enum vfs_status status;

    if (context == NULL || context->fs == NULL || path == NULL) {
        return VFS_NULL;
    }
    status = vfs_resolve(context, path, false, &node);
    if (status != VFS_OK) {
        return status;
    }
    context->fs->nodes[node].read_only = true;
    return VFS_OK;
}

enum vfs_status vfs_chdir(struct vfs_context *context, const char *path)
{
    uint32_t node;
    enum vfs_status status;

    if (context == NULL || context->fs == NULL || path == NULL) {
        return VFS_NULL;
    }
    status = vfs_resolve(context, path, true, &node);
    if (status != VFS_OK) {
        return status;
    }
    context->cwd = node;
    return VFS_OK;
}

enum vfs_status vfs_getcwd(struct vfs_context *context, char *buffer,
                           size_t capacity, size_t *length_out)
{
    uint32_t chain[VFS_MAX_NODES];
    uint32_t depth = 0U;
    uint32_t cursor;
    size_t length = 0U;

    if (length_out != NULL) {
        *length_out = 0U;
    }
    if (context == NULL || context->fs == NULL || buffer == NULL ||
        length_out == NULL || capacity == 0U) {
        return VFS_NULL;
    }
    if (!vfs_ready(context->fs)) {
        return VFS_BAD_STATE;
    }
    cursor = context->cwd;
    while (cursor != VFS_NO_NODE && cursor < VFS_MAX_NODES &&
           depth < VFS_MAX_NODES) {
        chain[depth++] = cursor;
        if (cursor == context->fs->root) {
            break;
        }
        cursor = context->fs->nodes[cursor].parent;
    }
    if (depth == 0U || chain[depth - 1U] != context->fs->root) {
        return VFS_BAD_STATE;
    }
    if (capacity < 2U) {
        return VFS_PATH_TOO_LONG;
    }
    buffer[length++] = '/';
    for (uint32_t index = depth; index > 1U; --index) {
        const struct vfs_node *node = &context->fs->nodes[chain[index - 2U]];
        const size_t name_length = strlen(node->name);

        if (length + name_length + 2U > capacity) {
            buffer[0] = '\0';
            return VFS_PATH_TOO_LONG;
        }
        if (length > 1U) {
            buffer[length++] = '/';
        }
        memcpy(buffer + length, node->name, name_length);
        length += name_length;
    }
    buffer[length] = '\0';
    *length_out = length;
    return VFS_OK;
}

enum vfs_status vfs_readdir(struct vfs_context *context, const char *path,
                            uint32_t index, char *name, size_t name_capacity,
                            struct vfs_stat *stat_out)
{
    uint32_t node;
    uint32_t child;
    enum vfs_status status;
    size_t length;

    if (context == NULL || context->fs == NULL || path == NULL ||
        name == NULL || stat_out == NULL || name_capacity == 0U) {
        return VFS_NULL;
    }
    status = vfs_resolve(context, path, true, &node);
    if (status != VFS_OK) {
        return status;
    }
    child = context->fs->nodes[node].first_child;
    while (child != VFS_NO_NODE && index != 0U) {
        child = context->fs->nodes[child].next_sibling;
        --index;
    }
    if (child == VFS_NO_NODE) {
        return VFS_NOT_FOUND;
    }
    length = strlen(context->fs->nodes[child].name);
    if (length + 1U > name_capacity) {
        return VFS_NAME_TOO_LONG;
    }
    memcpy(name, context->fs->nodes[child].name, length + 1U);
    memset(stat_out, 0, sizeof(*stat_out));
    stat_out->type = context->fs->nodes[child].type;
    stat_out->size = context->fs->nodes[child].size;
    stat_out->open_count = context->fs->nodes[child].open_count;
    stat_out->unlinked = context->fs->nodes[child].unlinked;
    return VFS_OK;
}

const char *vfs_status_string(enum vfs_status status)
{
    switch (status) {
    case VFS_OK: return "OK";
    case VFS_NULL: return "NULL";
    case VFS_BAD_STATE: return "BAD_STATE";
    case VFS_NOT_FOUND: return "NOT_FOUND";
    case VFS_EXISTS: return "EXISTS";
    case VFS_NOT_DIR: return "NOT_DIR";
    case VFS_IS_DIR: return "IS_DIR";
    case VFS_NOT_EMPTY: return "NOT_EMPTY";
    case VFS_NO_SPACE: return "NO_SPACE";
    case VFS_NAME_TOO_LONG: return "NAME_TOO_LONG";
    case VFS_PATH_TOO_LONG: return "PATH_TOO_LONG";
    case VFS_TOO_MANY_FILES: return "TOO_MANY_FILES";
    case VFS_TOO_MANY_NODES: return "TOO_MANY_NODES";
    case VFS_INVALID: return "INVALID";
    case VFS_READ_ONLY: return "READ_ONLY";
    case VFS_UNSUPPORTED: return "UNSUPPORTED";
    case VFS_BAD_FILE: return "BAD_FILE";
    case VFS_BAD_OFFSET: return "BAD_OFFSET";
    case VFS_NO_DATA: return "NO_DATA";
    case VFS_BUSY: return "BUSY";
    default: return "UNKNOWN";
    }
}

uint32_t vfs_node_count(const struct vfs *fs)
{
    return ramfs_node_count(fs);
}

uint32_t vfs_open_count(const struct vfs *fs)
{
    return fs != NULL ? fs->open_count : 0U;
}

uint32_t vfs_data_used(const struct vfs *fs)
{
    return ramfs_data_used(fs);
}


static enum vfs_status vfs_description_read(struct file_description *description,
                                            void *buffer, size_t size,
                                            size_t *done_out)
{
    if (description == NULL || !description->published ||
        !description->readable) {
        return VFS_READ_ONLY;
    }
    return vfs_read((struct vfs_file *)description->context, buffer, size,
                    done_out);
}

static enum vfs_status vfs_description_write(
    struct file_description *description, const void *buffer, size_t size,
    size_t *done_out)
{
    if (description == NULL || !description->published ||
        !description->writable) {
        return VFS_READ_ONLY;
    }
    return vfs_write((struct vfs_file *)description->context, buffer, size,
                     done_out);
}

static void vfs_description_close(struct file_description *description)
{
    if (description != NULL && description->context != NULL) {
        (void)vfs_close((struct vfs_file *)description->context);
        description->context = NULL;
    }
}

static const struct descriptor_ops vfs_file_ops = {
    .read = vfs_description_read,
    .write = vfs_description_write,
    .close = vfs_description_close,
};

enum descriptor_status vfs_file_bind_checked(
    struct descriptor_table *table, struct vfs_context *context,
    const char *path, uint32_t flags, struct file_description *storage,
    uint32_t *descriptor_out, enum vfs_status *vfs_status_out)
{
    struct vfs_file *file = NULL;
    enum vfs_status status;
    enum descriptor_status descriptor_status;

    if (vfs_status_out != NULL) {
        *vfs_status_out = VFS_BAD_STATE;
    }
    if (table == NULL || context == NULL || path == NULL ||
        storage == NULL || descriptor_out == NULL) {
        return DESCRIPTOR_NULL;
    }
    status = vfs_open(context, path, flags, &file);
    if (status != VFS_OK) {
        if (vfs_status_out != NULL) {
            *vfs_status_out = status;
        }
        return DESCRIPTOR_BAD_DESCRIPTION;
    }
    descriptor_status = descriptor_description_init(
        storage, &vfs_file_ops, file, (flags & VFS_OPEN_READ) != 0U,
        (flags & VFS_OPEN_WRITE) != 0U);
    if (descriptor_status != DESCRIPTOR_OK) {
        (void)vfs_close(file);
        return descriptor_status;
    }
    descriptor_status = descriptor_table_bind(table, storage, descriptor_out);
    if (descriptor_status != DESCRIPTOR_OK) {
        descriptor_description_release(storage);
        return descriptor_status;
    }
    return DESCRIPTOR_OK;
}

enum descriptor_status vfs_file_bind(struct descriptor_table *table,
                                     struct vfs_context *context,
                                     const char *path, uint32_t flags,
                                     struct file_description *storage,
                                     uint32_t *descriptor_out)
{
    return vfs_file_bind_checked(table, context, path, flags, storage,
                                 descriptor_out, NULL);
}
