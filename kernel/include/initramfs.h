#ifndef HFOC_INITRAMFS_H
#define HFOC_INITRAMFS_H

#include "vfs.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * ITEM-623: the deterministic in-tree initramfs archive.
 *
 * The format is deliberately small enough to validate before any byte reaches
 * the VFS.  All integers are little-endian and every offset is absolute from
 * the first byte of the module.
 *
 *   header (32 bytes)
 *     0   magic "HFOCIR01"
 *     8   u32 version (1)
 *     12  u32 entry count
 *     16  u32 total size, including this header
 *     20  u32 checksum of bytes [32, total size)
 *     24  u32 flags (0)
 *     28  u32 reserved (0)
 *
 *   entries follow back to back, each:
 *     0   u16 type (1 regular file, 2 directory)
 *     2   u16 mode (permission bits, 0..0777; stored, not enforced by VFS)
 *     4   u32 path length, without NUL (1..VFS_PATH_MAX-1)
 *     8   u32 data length (directories must be 0)
 *     12  u32 data offset, 4-byte aligned and equal to the next aligned byte
 *     16  u32 entry checksum (FNV-1a over path bytes then data bytes)
 *     20  u32 reserved (0)
 *     path bytes, then zero padding to 4 bytes, then data bytes, then zero
 *     padding to 4 bytes.
 *
 * Paths are absolute, canonical and sorted by the packer so a parent always
 * precedes its children.  The parser rejects duplicate paths, "." / "..",
 * empty components, trailing slashes, missing parents, special entries and
 * any gap or overlap between entries.  The last entry must end exactly at
 * total size, so trailing hidden bytes are refused as well.
 */

#define INITRAMFS_MAGIC "HFOCIR01"
#define INITRAMFS_MAGIC_SIZE 8U
#define INITRAMFS_VERSION 1U
#define INITRAMFS_HEADER_SIZE 32U
#define INITRAMFS_ENTRY_HEADER_SIZE 24U
#define INITRAMFS_MAX_ENTRIES 64U
#define INITRAMFS_MAX_SIZE (1024U * 1024U)

enum initramfs_entry_type {
    INITRAMFS_ENTRY_FILE = 1,
    INITRAMFS_ENTRY_DIR = 2,
};

enum initramfs_status {
    INITRAMFS_OK = 0,
    INITRAMFS_NULL = 1,
    INITRAMFS_TOO_SMALL = 2,
    INITRAMFS_BAD_MAGIC = 3,
    INITRAMFS_BAD_VERSION = 4,
    INITRAMFS_TOO_LARGE = 5,
    INITRAMFS_BAD_SIZE = 6,
    INITRAMFS_BAD_CHECKSUM = 7,
    INITRAMFS_BAD_FLAGS = 8,
    INITRAMFS_TOO_MANY_ENTRIES = 9,
    INITRAMFS_BAD_TYPE = 10,
    INITRAMFS_BAD_MODE = 11,
    INITRAMFS_BAD_PATH = 12,
    INITRAMFS_BAD_LENGTH = 13,
    INITRAMFS_BAD_OFFSET = 14,
    INITRAMFS_BAD_ALIGNMENT = 15,
    INITRAMFS_DUPLICATE = 16,
    INITRAMFS_MISSING_PARENT = 17,
    INITRAMFS_NOT_DIRECTORY = 18,
    INITRAMFS_BAD_ENTRY_CHECKSUM = 19,
    INITRAMFS_TRAILING_BYTES = 20,
    INITRAMFS_IMPORT_FAILED = 21,
    INITRAMFS_ROLLBACK_FAILED = 22,
};

struct initramfs_entry {
    enum initramfs_entry_type type;
    uint16_t mode;
    uint32_t data_offset;
    uint32_t data_length;
    char path[VFS_PATH_MAX];
};

struct initramfs_archive {
    const uint8_t *bytes;
    uint32_t size;
    uint32_t entry_count;
    uint32_t data_bytes;
    struct initramfs_entry entries[INITRAMFS_MAX_ENTRIES];
};

enum initramfs_status initramfs_parse(const void *bytes, size_t size,
                                      struct initramfs_archive *archive_out);
const char *initramfs_status_string(enum initramfs_status status);
uint32_t initramfs_imported_nodes(const struct initramfs_archive *archive);

/*
 * Publish every validated entry below `prefix` ("/" or "/dir") as a read-only
 * tree.  Nothing is published until the whole archive has been parsed, and a
 * failed import removes the entries it created in reverse order.
 */
enum initramfs_status initramfs_import(const struct initramfs_archive *archive,
                                       struct vfs *fs,
                                       struct vfs_context *context,
                                       const char *prefix);

#endif
