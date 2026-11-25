#ifndef HFOC_VFS_H
#define HFOC_VFS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * ITEM-622: the first real HFOC filesystem namespace.
 *
 * This unit is deliberately independent of user mode and of the block layer:
 * it is a bounded, writable RAM-backed tree with Unix path semantics.  The
 * descriptors a process owns are still the ITEM-452 number table; binding a
 * file description to that table is ITEM-623, and the production file
 * syscalls are ITEM-624.  Keeping the namespace separate here means the path
 * and allocation rules are proven before any user pointer can reach them.
 *
 * Every limit is explicit and enforced while the operation is being composed,
 * not by truncating a result afterwards:
 *
 *   nodes        256
 *   open files    64
 *   name bytes   255 + NUL (UTF-8 bytes, no locale folding)
 *   path bytes  1023 + NUL
 *   file bytes  64 KiB
 *   data bytes  384 KiB (96 blocks of 4 KiB)
 *
 * ITEM-628: the native rootfs now carries /bin/sh and its utilities.  The
 * previous 64-block arena mounted the test root alone at 64/64 blocks, leaving
 * no room for the external programs' files; 96 blocks keeps the same bounded
 * no-heap model and leaves the test and production roots room to work.
 */

#define VFS_NAME_MAX 255U
#define VFS_PATH_MAX 1024U
#define VFS_MAX_NODES 256U
#define VFS_MAX_FILES 64U
#define VFS_BLOCK_SIZE 4096U
#define VFS_BLOCK_COUNT 96U
#define VFS_FILE_MAX (16U * VFS_BLOCK_SIZE)
#define VFS_DATA_MAX (VFS_BLOCK_COUNT * VFS_BLOCK_SIZE)

enum vfs_node_type {
    VFS_NODE_NONE = 0,
    VFS_NODE_FILE = 1,
    VFS_NODE_DIR = 2,
};

enum vfs_status {
    VFS_OK = 0,
    VFS_NULL = 1,
    VFS_BAD_STATE = 2,
    VFS_NOT_FOUND = 3,
    VFS_EXISTS = 4,
    VFS_NOT_DIR = 5,
    VFS_IS_DIR = 6,
    VFS_NOT_EMPTY = 7,
    VFS_NO_SPACE = 8,
    VFS_NAME_TOO_LONG = 9,
    VFS_PATH_TOO_LONG = 10,
    VFS_TOO_MANY_FILES = 11,
    VFS_TOO_MANY_NODES = 12,
    VFS_INVALID = 13,
    VFS_READ_ONLY = 14,
    VFS_UNSUPPORTED = 15,
    VFS_BAD_FILE = 16,
    VFS_BAD_OFFSET = 17,
    VFS_NO_DATA = 18,
    VFS_BUSY = 19,
    /* ITEM-627: the peer reader is gone; mapped to the syscall EPIPE. */
    VFS_BROKEN_PIPE = 20,
    /* ITEM-627: the pipe is full/empty; the caller yields and retries. */
    VFS_AGAIN = 21,
};

#define VFS_OPEN_READ 0x1U
#define VFS_OPEN_WRITE 0x2U
#define VFS_OPEN_CREATE 0x4U
#define VFS_OPEN_TRUNC 0x8U

struct vfs_stat {
    enum vfs_node_type type;
    uint32_t size;
    uint32_t open_count;
    bool unlinked;
    bool read_only;
};

struct vfs_file {
    bool used;
    bool writable;
    struct vfs *fs;
    uint32_t node;
    uint32_t offset;
};

struct vfs_context {
    struct vfs *fs;
    uint32_t cwd;
};

struct vfs_node {
    bool used;
    bool unlinked;
    bool read_only;
    enum vfs_node_type type;
    uint32_t parent;
    uint32_t first_child;
    uint32_t next_sibling;
    uint32_t size;
    uint32_t first_block;
    uint32_t open_count;
    char name[VFS_NAME_MAX + 1U];
};

struct vfs_block {
    bool used;
    uint32_t next;
    uint8_t data[VFS_BLOCK_SIZE];
};

struct vfs {
    bool initialized;
    uint32_t generation;
    uint32_t node_count;
    uint32_t open_count;
    uint32_t data_blocks;
    uint32_t root;
    struct vfs_node nodes[VFS_MAX_NODES];
    struct vfs_block blocks[VFS_BLOCK_COUNT];
    struct vfs_file files[VFS_MAX_FILES];
};

enum vfs_status vfs_init(struct vfs *fs);
enum vfs_status vfs_context_init(struct vfs_context *context,
                                 struct vfs *fs);
enum vfs_status vfs_open(struct vfs_context *context, const char *path,
                         uint32_t flags, struct vfs_file **file_out);
enum vfs_status vfs_read(struct vfs_file *file, void *buffer, size_t size,
                         size_t *done_out);
/* ITEM-624: bounded repositioning of an open file description. */
enum vfs_status vfs_seek(struct vfs_file *file, uint32_t offset);
enum vfs_status vfs_write(struct vfs_file *file, const void *buffer,
                          size_t size, size_t *done_out);
enum vfs_status vfs_close(struct vfs_file *file);
enum vfs_status vfs_stat(struct vfs_context *context, const char *path,
                         struct vfs_stat *stat_out);
enum vfs_status vfs_mkdir(struct vfs_context *context, const char *path);
enum vfs_status vfs_unlink(struct vfs_context *context, const char *path);
enum vfs_status vfs_rename(struct vfs_context *context, const char *old_path,
                           const char *new_path);
enum vfs_status vfs_chdir(struct vfs_context *context, const char *path);
enum vfs_status vfs_getcwd(struct vfs_context *context, char *buffer,
                           size_t capacity, size_t *length_out);
enum vfs_status vfs_set_read_only(struct vfs_context *context,
                                  const char *path);
bool vfs_path_read_only(const struct vfs *fs, uint32_t node);
enum vfs_status vfs_readdir(struct vfs_context *context, const char *path,
                            uint32_t index, char *name, size_t name_capacity,
                            struct vfs_stat *stat_out);
const char *vfs_status_string(enum vfs_status status);
uint32_t vfs_node_count(const struct vfs *fs);
uint32_t vfs_open_count(const struct vfs *fs);
uint32_t vfs_data_used(const struct vfs *fs);

#endif
