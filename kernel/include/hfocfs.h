#ifndef HFOC_HFOCFS_H
#define HFOC_HFOCFS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "block_device.h"

/*
 * ITEM-630: HFOCFS v1, the native writable filesystem for a caller-supplied
 * image.  The normative byte-level format is
 * /srv/hfoc-example/queue/630-hfocfs-format.md; this header is only the bounded
 * host/kernel-facing surface.
 *
 * The unit never takes a raw pointer from user mode and never writes a raw
 * host disk.  It works exclusively through `struct block_device`, so the same
 * code is proven on ordinary host image files and on a QEMU disk.  All I/O is
 * whole 4096-byte filesystem blocks.  Checksums detect corruption; this is not
 * encryption and not a POSIX/Linux ABI claim.
 *
 * Limits are fixed for v1 and are enforced while an operation is composed:
 *
 *   image bytes     4 MiB .. 112 MiB (1024 .. 28672 FS blocks)
 *   FS block        4096 B (64 B header + 4032 B payload)
 *   nodes           256, root ino 1
 *   name bytes      255
 *   path bytes      1024 (the VFS owner enforces path composition)
 *   file bytes      64 KiB (18 direct blocks)
 *
 * The caller supplies the whole working state.  This keeps allocation
 * deterministic, bounded and testable under fault injection; there is no
 * hidden heap growth from image contents.
 */

#define HFOCFS_BLOCK_SIZE 4096U
#define HFOCFS_BLOCK_HEADER_SIZE 64U
#define HFOCFS_BLOCK_PAYLOAD_SIZE (HFOCFS_BLOCK_SIZE - HFOCFS_BLOCK_HEADER_SIZE)

#define HFOCFS_IMAGE_MIN_BYTES (4U * 1024U * 1024U)
#define HFOCFS_IMAGE_MAX_BYTES (112U * 1024U * 1024U)
#define HFOCFS_BLOCK_COUNT_MIN (HFOCFS_IMAGE_MIN_BYTES / HFOCFS_BLOCK_SIZE)
#define HFOCFS_BLOCK_COUNT_MAX (HFOCFS_IMAGE_MAX_BYTES / HFOCFS_BLOCK_SIZE)

#define HFOCFS_MAX_NODES 256U
#define HFOCFS_MAX_NAME 255U
#define HFOCFS_MAX_PATH 1024U
#define HFOCFS_MAX_FILE_BYTES (64U * 1024U)
#define HFOCFS_DIRECT_BLOCKS 18U
#define HFOCFS_INODES_PER_BLOCK 22U
#define HFOCFS_INODE_BLOCKS_MAX 12U
#define HFOCFS_BITMAP_BLOCKS_MAX 7U
#define HFOCFS_LABEL_SIZE 32U
#define HFOCFS_UUID_SIZE 16U
#define HFOCFS_DIGEST_SIZE 32U

#define HFOCFS_MAGIC "HFOCFS1"
#define HFOCFS_MAGIC_SIZE 8U
#define HFOCFS_FORMAT_VERSION 1U
#define HFOCFS_BLOCK_MAGIC 0x48464342U /* "HFCB" */

enum hfocfs_block_kind {
    HFOCFS_BLOCK_INODE_TABLE = 1,
    HFOCFS_BLOCK_DIRECTORY = 2,
    HFOCFS_BLOCK_FILE_DATA = 3,
    HFOCFS_BLOCK_COMMIT = 4,
    HFOCFS_BLOCK_BITMAP = 5,
};

enum hfocfs_status {
    HFOCFS_OK = 0,
    HFOCFS_NULL = 1,
    HFOCFS_BAD_ARGUMENT = 2,
    HFOCFS_TOO_SMALL = 3,
    HFOCFS_TOO_LARGE = 4,
    HFOCFS_IO = 5,
    HFOCFS_CORRUPT = 6,
    HFOCFS_UNSUPPORTED = 7,
    HFOCFS_NOSPC = 8,
    HFOCFS_NO_MEMORY = 9,
    HFOCFS_READ_ONLY = 10,
    HFOCFS_NOT_MOUNTED = 11,
    HFOCFS_BUSY = 12,
    HFOCFS_NOT_FOUND = 13,
    HFOCFS_EXISTS = 14,
    HFOCFS_NOT_DIRECTORY = 15,
    HFOCFS_IS_DIRECTORY = 16,
    HFOCFS_NOT_EMPTY = 17,
    HFOCFS_NAME_TOO_LONG = 18,
    HFOCFS_PATH_TOO_LONG = 19,
};

struct hfocfs_options {
    /* NUL-padded UTF-8 label, not used for lookup. NULL means empty. */
    const char *label;
    /* Stable image identity; NULL means the formatter fills it. */
    const uint8_t *uuid;
};

/* Read-only view of the selected generation, valid until unmount. */
struct hfocfs_info {
    uint64_t generation;
    uint64_t block_count;
    uint32_t node_count;
    uint32_t root_ino;
    bool clean;
    uint8_t uuid[HFOCFS_UUID_SIZE];
    char label[HFOCFS_LABEL_SIZE + 1U];
};

struct hfocfs;

/* Bytes the caller must provide for `state` in format/mount. */
size_t hfocfs_state_size(void);
size_t hfocfs_state_alignment(void);

/*
 * Write an empty generation-1 image.  The device must validate, use a 512-byte
 * (or multiple) block size and be 4..112 MiB.  This does not read any
 * pre-existing filesystem and does not copy a host tree.
 */
enum hfocfs_status hfocfs_format(struct block_device *device,
                                 const struct hfocfs_options *options,
                                 void *state, size_t state_size);

/*
 * Read both superblock slots, select the newest valid generation and validate
 * the whole reachable metadata tree.  Mount performs no write.  A read-only
 * device is accepted as long as flush support is absent only for later writes.
 */
enum hfocfs_status hfocfs_mount(struct block_device *device,
                                void *state, size_t state_size,
                                struct hfocfs **out);
enum hfocfs_status hfocfs_info(const struct hfocfs *fs,
                               struct hfocfs_info *out);

enum hfocfs_node_type {
    HFOCFS_NODE_NONE = 0,
    HFOCFS_NODE_FILE = 1,
    HFOCFS_NODE_DIRECTORY = 2,
};

struct hfocfs_stat {
    uint32_t ino;
    enum hfocfs_node_type type;
    uint16_t mode;
    uint32_t size;
    uint32_t link_count;
    uint32_t open_count;
    uint64_t mtime;
};

/*
 * Bounded tree operations.  Paths are absolute, use '/' separators and accept
 * only "." and ".." as special components; every component is at most 255
 * bytes and the whole path at most 1023 bytes.  They never allocate outside
 * the caller-supplied state and never grow past the fixed v1 limits.
 */
enum hfocfs_status hfocfs_lookup(struct hfocfs *fs, const char *path,
                                 uint32_t *ino_out);
enum hfocfs_status hfocfs_stat_path(struct hfocfs *fs, const char *path,
                                    struct hfocfs_stat *stat_out);
enum hfocfs_status hfocfs_create(struct hfocfs *fs, const char *path,
                                 enum hfocfs_node_type type, uint16_t mode,
                                 uint32_t *ino_out);
enum hfocfs_status hfocfs_readdir(struct hfocfs *fs, uint32_t dir_ino,
                                  uint32_t index, char *name,
                                  size_t name_capacity, uint32_t *ino_out,
                                  enum hfocfs_node_type *type_out);
enum hfocfs_status hfocfs_read(struct hfocfs *fs, uint32_t ino,
                               uint32_t offset, void *buffer, size_t size,
                               size_t *done_out);
enum hfocfs_status hfocfs_write(struct hfocfs *fs, uint32_t ino,
                                uint32_t offset, const void *buffer,
                                size_t size, size_t *done_out);
enum hfocfs_status hfocfs_truncate(struct hfocfs *fs, uint32_t ino);
enum hfocfs_status hfocfs_unlink(struct hfocfs *fs, const char *path);
enum hfocfs_status hfocfs_rename(struct hfocfs *fs, const char *old_path,
                                 const char *new_path);

/*
 * Keep a node alive after its directory entry is gone.  Without an open
 * reference an unlinked node is freed in the same commit; with one it is
 * serialised with link_count 0 so a mounted reader can still finish it.
 */
enum hfocfs_status hfocfs_open(struct hfocfs *fs, uint32_t ino);
enum hfocfs_status hfocfs_close(struct hfocfs *fs, uint32_t ino);

/*
 * Commit the dirty tree with the documented write order: new blocks, commit
 * block, then superblock slot, flushing at each barrier.  On any error the
 * mounted generation stays recoverable and the caller receives HFOCFS_IO.
 */
enum hfocfs_status hfocfs_flush(struct hfocfs *fs);

/* Flush (unless read-only), then release the caller-supplied state. */
enum hfocfs_status hfocfs_unmount(struct hfocfs *fs);

const char *hfocfs_status_string(enum hfocfs_status status);

#endif
