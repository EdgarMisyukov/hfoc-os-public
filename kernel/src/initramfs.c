/*
 * ITEM-623: parse and publish the deterministic HFOC initramfs archive.
 *
 * The parser is total: it validates the complete directory (sizes, offsets,
 * alignment, checksums, canonical paths, parent order and duplicates) before
 * the first VFS node is created.  A malformed module therefore cannot publish
 * a partial tree and cannot make the kernel read outside the module.
 */

#include "initramfs.h"

#include <string.h>

#define INITRAMFS_NO_ENTRY UINT32_MAX

static uint16_t initramfs_read_u16(const uint8_t *bytes)
{
    return (uint16_t)((uint16_t)bytes[0] | ((uint16_t)bytes[1] << 8));
}

static uint32_t initramfs_read_u32(const uint8_t *bytes)
{
    return (uint32_t)bytes[0] | ((uint32_t)bytes[1] << 8) |
        ((uint32_t)bytes[2] << 16) | ((uint32_t)bytes[3] << 24);
}

static uint32_t initramfs_checksum(const uint8_t *bytes, size_t length)
{
    uint32_t hash = UINT32_C(2166136261);
    size_t index;

    for (index = 0U; index < length; ++index) {
        hash ^= bytes[index];
        hash *= UINT32_C(16777619);
    }
    return hash;
}

static bool initramfs_path_valid(const char *path, size_t length)
{
    size_t index = 0U;

    if (length == 0U || length >= VFS_PATH_MAX || path[0] != '/' ||
        path[length - 1U] == '/') {
        return false;
    }
    while (index < length) {
        size_t start;
        size_t component;

        while (index < length && path[index] == '/') {
            ++index;
        }
        start = index;
        while (index < length && path[index] != '/') {
            ++index;
        }
        component = index - start;
        if (component == 0U || component > VFS_NAME_MAX) {
            return false;
        }
        if (component == 1U && path[start] == '.') {
            return false;
        }
        if (component == 2U && path[start] == '.' &&
            path[start + 1U] == '.') {
            return false;
        }
    }
    return true;
}

static enum initramfs_status initramfs_find_parent(
    const struct initramfs_archive *archive, const char *path,
    uint32_t *parent_out)
{
    char parent[VFS_PATH_MAX];
    size_t length = strlen(path);
    size_t slash;
    uint32_t index;

    *parent_out = INITRAMFS_NO_ENTRY;
    if (length <= 1U) {
        return INITRAMFS_OK;
    }
    slash = length;
    while (slash > 0U && path[slash - 1U] != '/') {
        --slash;
    }
    if (slash <= 1U) {
        return INITRAMFS_OK;
    }
    memcpy(parent, path, slash - 1U);
    parent[slash - 1U] = '\0';
    for (index = 0U; index < archive->entry_count; ++index) {
        if (strcmp(archive->entries[index].path, parent) == 0) {
            if (archive->entries[index].type != INITRAMFS_ENTRY_DIR) {
                return INITRAMFS_NOT_DIRECTORY;
            }
            *parent_out = index;
            return INITRAMFS_OK;
        }
    }
    return INITRAMFS_MISSING_PARENT;
}

enum initramfs_status initramfs_parse(const void *bytes, size_t size,
                                      struct initramfs_archive *archive_out)
{
    const uint8_t *archive = bytes;
    uint32_t total_size;
    uint32_t entry_count;
    uint32_t cursor;
    uint32_t index;
    uint32_t data_bytes = 0U;

    if (bytes == NULL || archive_out == NULL) {
        return INITRAMFS_NULL;
    }
    memset(archive_out, 0, sizeof(*archive_out));
    if (size < INITRAMFS_HEADER_SIZE) {
        return INITRAMFS_TOO_SMALL;
    }
    if (size > INITRAMFS_MAX_SIZE) {
        return INITRAMFS_TOO_LARGE;
    }
    if (memcmp(archive, INITRAMFS_MAGIC, INITRAMFS_MAGIC_SIZE) != 0) {
        return INITRAMFS_BAD_MAGIC;
    }
    if (initramfs_read_u32(archive + 8U) != INITRAMFS_VERSION) {
        return INITRAMFS_BAD_VERSION;
    }
    entry_count = initramfs_read_u32(archive + 12U);
    total_size = initramfs_read_u32(archive + 16U);
    if (total_size != size) {
        return INITRAMFS_BAD_SIZE;
    }
    if (total_size < INITRAMFS_HEADER_SIZE) {
        return INITRAMFS_BAD_SIZE;
    }
    if (initramfs_read_u32(archive + 20U) !=
        initramfs_checksum(archive + INITRAMFS_HEADER_SIZE,
                           total_size - INITRAMFS_HEADER_SIZE)) {
        return INITRAMFS_BAD_CHECKSUM;
    }
    if (initramfs_read_u32(archive + 24U) != 0U ||
        initramfs_read_u32(archive + 28U) != 0U) {
        return INITRAMFS_BAD_FLAGS;
    }
    if (entry_count > INITRAMFS_MAX_ENTRIES) {
        return INITRAMFS_TOO_MANY_ENTRIES;
    }

    archive_out->bytes = archive;
    archive_out->size = total_size;
    archive_out->entry_count = entry_count;
    cursor = INITRAMFS_HEADER_SIZE;
    for (index = 0U; index < entry_count; ++index) {
        struct initramfs_entry *entry = &archive_out->entries[index];
        uint32_t data_offset;
        uint32_t path_length;
        uint32_t data_length;
        uint32_t entry_checksum;
        uint32_t path_end;
        uint32_t next;
        uint16_t type;
        uint16_t mode;
        uint32_t parent = INITRAMFS_NO_ENTRY;
        uint32_t earlier;

        if (total_size - cursor < INITRAMFS_ENTRY_HEADER_SIZE) {
            return INITRAMFS_BAD_LENGTH;
        }
        type = initramfs_read_u16(archive + cursor);
        mode = initramfs_read_u16(archive + cursor + 2U);
        path_length = initramfs_read_u32(archive + cursor + 4U);
        data_length = initramfs_read_u32(archive + cursor + 8U);
        data_offset = initramfs_read_u32(archive + cursor + 12U);
        entry_checksum = initramfs_read_u32(archive + cursor + 16U);
        if (initramfs_read_u32(archive + cursor + 20U) != 0U) {
            return INITRAMFS_BAD_FLAGS;
        }
        if (type != INITRAMFS_ENTRY_FILE && type != INITRAMFS_ENTRY_DIR) {
            return INITRAMFS_BAD_TYPE;
        }
        if (mode > UINT16_C(0777)) {
            return INITRAMFS_BAD_MODE;
        }
        if (path_length == 0U || path_length >= VFS_PATH_MAX) {
            return INITRAMFS_BAD_PATH;
        }
        if ((type == INITRAMFS_ENTRY_DIR && data_length != 0U) ||
            data_length > VFS_FILE_MAX ||
            data_length > total_size) {
            return INITRAMFS_BAD_LENGTH;
        }
        path_end = cursor + INITRAMFS_ENTRY_HEADER_SIZE + path_length;
        if (path_end > total_size) {
            return INITRAMFS_BAD_LENGTH;
        }
        if (!initramfs_path_valid(
                (const char *)(archive + cursor + INITRAMFS_ENTRY_HEADER_SIZE),
                path_length)) {
            return INITRAMFS_BAD_PATH;
        }
        next = (path_end + 3U) & ~UINT32_C(3);
        if (next > total_size || data_offset != next) {
            return INITRAMFS_BAD_OFFSET;
        }
        if ((data_offset & 3U) != 0U) {
            return INITRAMFS_BAD_ALIGNMENT;
        }
        if (data_length > total_size - data_offset) {
            return INITRAMFS_BAD_LENGTH;
        }
        next = (data_offset + data_length + 3U) & ~UINT32_C(3);
        if (next > total_size) {
            return INITRAMFS_BAD_LENGTH;
        }
        entry->type = (enum initramfs_entry_type)type;
        entry->mode = mode;
        entry->data_offset = data_offset;
        entry->data_length = data_length;
        memcpy(entry->path,
               archive + cursor + INITRAMFS_ENTRY_HEADER_SIZE, path_length);
        entry->path[path_length] = '\0';
        {
            uint32_t hash = UINT32_C(2166136261);
            uint32_t byte;

            for (byte = 0U; byte < path_length; ++byte) {
                hash ^= (uint8_t)entry->path[byte];
                hash *= UINT32_C(16777619);
            }
            for (byte = 0U; byte < data_length; ++byte) {
                hash ^= archive[data_offset + byte];
                hash *= UINT32_C(16777619);
            }
            if (entry_checksum != hash) {
                return INITRAMFS_BAD_ENTRY_CHECKSUM;
            }
        }
        for (earlier = 0U; earlier < index; ++earlier) {
            if (strcmp(archive_out->entries[earlier].path, entry->path) == 0) {
                return INITRAMFS_DUPLICATE;
            }
        }
        {
            enum initramfs_status parent_status =
                initramfs_find_parent(archive_out, entry->path, &parent);

            if (parent_status != INITRAMFS_OK) {
                return parent_status;
            }
            if (parent != INITRAMFS_NO_ENTRY && parent >= index) {
                return INITRAMFS_MISSING_PARENT;
            }
        }
        data_bytes += data_length;
        if (data_bytes > VFS_DATA_MAX) {
            return INITRAMFS_BAD_LENGTH;
        }
        cursor = next;
    }
    if (cursor != total_size) {
        return INITRAMFS_TRAILING_BYTES;
    }
    archive_out->data_bytes = data_bytes;
    return INITRAMFS_OK;
}

const char *initramfs_status_string(enum initramfs_status status)
{
    switch (status) {
    case INITRAMFS_OK:
        return "INITRAMFS_OK";
    case INITRAMFS_NULL:
        return "INITRAMFS_NULL";
    case INITRAMFS_TOO_SMALL:
        return "INITRAMFS_TOO_SMALL";
    case INITRAMFS_BAD_MAGIC:
        return "INITRAMFS_BAD_MAGIC";
    case INITRAMFS_BAD_VERSION:
        return "INITRAMFS_BAD_VERSION";
    case INITRAMFS_TOO_LARGE:
        return "INITRAMFS_TOO_LARGE";
    case INITRAMFS_BAD_SIZE:
        return "INITRAMFS_BAD_SIZE";
    case INITRAMFS_BAD_CHECKSUM:
        return "INITRAMFS_BAD_CHECKSUM";
    case INITRAMFS_BAD_FLAGS:
        return "INITRAMFS_BAD_FLAGS";
    case INITRAMFS_TOO_MANY_ENTRIES:
        return "INITRAMFS_TOO_MANY_ENTRIES";
    case INITRAMFS_BAD_TYPE:
        return "INITRAMFS_BAD_TYPE";
    case INITRAMFS_BAD_MODE:
        return "INITRAMFS_BAD_MODE";
    case INITRAMFS_BAD_PATH:
        return "INITRAMFS_BAD_PATH";
    case INITRAMFS_BAD_LENGTH:
        return "INITRAMFS_BAD_LENGTH";
    case INITRAMFS_BAD_OFFSET:
        return "INITRAMFS_BAD_OFFSET";
    case INITRAMFS_BAD_ALIGNMENT:
        return "INITRAMFS_BAD_ALIGNMENT";
    case INITRAMFS_DUPLICATE:
        return "INITRAMFS_DUPLICATE";
    case INITRAMFS_MISSING_PARENT:
        return "INITRAMFS_MISSING_PARENT";
    case INITRAMFS_NOT_DIRECTORY:
        return "INITRAMFS_NOT_DIRECTORY";
    case INITRAMFS_BAD_ENTRY_CHECKSUM:
        return "INITRAMFS_BAD_ENTRY_CHECKSUM";
    case INITRAMFS_TRAILING_BYTES:
        return "INITRAMFS_TRAILING_BYTES";
    case INITRAMFS_IMPORT_FAILED:
        return "INITRAMFS_IMPORT_FAILED";
    case INITRAMFS_ROLLBACK_FAILED:
        return "INITRAMFS_ROLLBACK_FAILED";
    default:
        return "INITRAMFS_UNKNOWN";
    }
}

uint32_t initramfs_imported_nodes(const struct initramfs_archive *archive)
{
    return archive != NULL ? archive->entry_count : 0U;
}

static enum initramfs_status initramfs_target_path(
    const char *prefix, const char *path, char *out, size_t capacity)
{
    size_t prefix_length = strlen(prefix);
    size_t path_length = strlen(path);

    if (strcmp(prefix, "/") == 0) {
        if (path_length >= capacity) {
            return INITRAMFS_BAD_PATH;
        }
        memcpy(out, path, path_length + 1U);
        return INITRAMFS_OK;
    }
    if (prefix_length == 0U || prefix_length + path_length >= capacity) {
        return INITRAMFS_BAD_PATH;
    }
    memcpy(out, prefix, prefix_length);
    memcpy(out + prefix_length, path, path_length + 1U);
    return INITRAMFS_OK;
}

static enum initramfs_status initramfs_prefix_valid(const char *prefix)
{
    if (prefix == NULL || prefix[0] != '/' || strlen(prefix) >= VFS_PATH_MAX) {
        return INITRAMFS_BAD_PATH;
    }
    if (strcmp(prefix, "/") == 0) {
        return INITRAMFS_OK;
    }
    if (prefix[strlen(prefix) - 1U] == '/' ||
        !initramfs_path_valid(prefix, strlen(prefix))) {
        return INITRAMFS_BAD_PATH;
    }
    return INITRAMFS_OK;
}

enum initramfs_status initramfs_import(const struct initramfs_archive *archive,
                                       struct vfs *fs,
                                       struct vfs_context *context,
                                       const char *prefix)
{
    struct vfs_stat stat;
    char target[VFS_PATH_MAX];
    uint32_t index;
    uint32_t created = 0U;
    enum initramfs_status result = INITRAMFS_OK;

    if (archive == NULL || fs == NULL || context == NULL ||
        archive->bytes == NULL) {
        return INITRAMFS_NULL;
    }
    if (initramfs_prefix_valid(prefix) != INITRAMFS_OK || context->fs != fs) {
        return INITRAMFS_BAD_PATH;
    }
    if (strcmp(prefix, "/") != 0) {
        if (vfs_stat(context, prefix, &stat) != VFS_OK ||
            stat.type != VFS_NODE_DIR) {
            return INITRAMFS_MISSING_PARENT;
        }
    }
    if (archive->entry_count + vfs_node_count(fs) > VFS_MAX_NODES) {
        return INITRAMFS_IMPORT_FAILED;
    }
    if (archive->data_bytes + vfs_data_used(fs) > VFS_DATA_MAX) {
        return INITRAMFS_IMPORT_FAILED;
    }

    for (index = 0U; index < archive->entry_count; ++index) {
        const struct initramfs_entry *entry = &archive->entries[index];
        struct vfs_file *file = NULL;
        size_t done = 0U;

        if (initramfs_target_path(prefix, entry->path, target,
                                  sizeof(target)) != INITRAMFS_OK) {
            result = INITRAMFS_IMPORT_FAILED;
            break;
        }
        if (entry->type == INITRAMFS_ENTRY_DIR) {
            if (vfs_mkdir(context, target) != VFS_OK) {
                result = INITRAMFS_IMPORT_FAILED;
                break;
            }
        } else {
            if (vfs_open(context, target,
                         VFS_OPEN_CREATE | VFS_OPEN_WRITE | VFS_OPEN_READ,
                         &file) != VFS_OK) {
                result = INITRAMFS_IMPORT_FAILED;
                break;
            }
            if (vfs_write(file, archive->bytes + entry->data_offset,
                          entry->data_length, &done) != VFS_OK ||
                done != entry->data_length ||
                vfs_close(file) != VFS_OK) {
                if (file != NULL) {
                    (void)vfs_close(file);
                }
                result = INITRAMFS_IMPORT_FAILED;
                break;
            }
        }
        ++created;
    }

    if (result == INITRAMFS_OK) {
        for (index = 0U; index < archive->entry_count; ++index) {
            const struct initramfs_entry *entry = &archive->entries[index];

            if (initramfs_target_path(prefix, entry->path, target,
                                      sizeof(target)) != INITRAMFS_OK ||
                vfs_set_read_only(context, target) != VFS_OK) {
                result = INITRAMFS_IMPORT_FAILED;
                break;
            }
        }
    }

    if (result != INITRAMFS_OK) {
        while (created > 0U) {
            --created;
            if (initramfs_target_path(prefix,
                                      archive->entries[created].path, target,
                                      sizeof(target)) != INITRAMFS_OK ||
                vfs_unlink(context, target) != VFS_OK) {
                return INITRAMFS_ROLLBACK_FAILED;
            }
        }
    }
    return result;
}
