/*
 * ITEM-622 host proof for the bounded VFS and RAM filesystem.
 *
 * The proof drives the production vfs.c/ramfs.c, not a model: paths, open
 * files, directory mutation, unlink-open lifetime, ENOSPC rollback and the
 * fixed node/data limits are the same code the boot will call.
 */

#include "vfs.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static int failures;

#define CHECK(cond, msg) do { \
    if (!(cond)) { \
        fprintf(stderr, "FAIL: %s\n", msg); \
        ++failures; \
    } else { \
        printf("PASS: %s\n", msg); \
    } \
} while (0)

static struct vfs filesystem;
static struct vfs_context context;

static void reset(void)
{
    CHECK(vfs_init(&filesystem) == VFS_OK, "the RAM filesystem initialises");
    CHECK(vfs_context_init(&context, &filesystem) == VFS_OK,
          "the context starts at the root");
}

static void write_file(const char *path, const char *text)
{
    struct vfs_file *file = NULL;
    size_t done = 0U;

    CHECK(vfs_open(&context, path,
                   VFS_OPEN_CREATE | VFS_OPEN_WRITE | VFS_OPEN_READ,
                   &file) == VFS_OK,
          "the file opens for writing");
    CHECK(vfs_write(file, text, strlen(text), &done) == VFS_OK &&
              done == strlen(text),
          "the whole text is written");
    CHECK(vfs_close(file) == VFS_OK, "the written file closes");
}

static void test_root_and_paths(void)
{
    struct vfs_stat stat;
    char cwd[64];
    size_t length = 0U;

    reset();
    CHECK(vfs_stat(&context, "/", &stat) == VFS_OK &&
              stat.type == VFS_NODE_DIR,
          "the root is a directory");
    CHECK(vfs_chdir(&context, "//./") == VFS_OK,
          "repeated separators and . resolve");
    CHECK(vfs_getcwd(&context, cwd, sizeof(cwd), &length) == VFS_OK &&
              strcmp(cwd, "/") == 0 && length == 1U,
          "the root path is /");
    CHECK(vfs_stat(&context, "/missing", &stat) == VFS_NOT_FOUND,
          "a missing path is NOT_FOUND");
    CHECK(vfs_stat(&context, "/a/b", &stat) == VFS_NOT_FOUND,
          "a missing parent is NOT_FOUND");
    CHECK(vfs_mkdir(&context, "/a") == VFS_OK, "a directory is created");
    CHECK(vfs_chdir(&context, "/a") == VFS_OK, "the directory is entered");
    CHECK(vfs_getcwd(&context, cwd, sizeof(cwd), &length) == VFS_OK &&
              strcmp(cwd, "/a") == 0,
          "a relative directory reports its absolute path");
    CHECK(vfs_chdir(&context, "../../..") == VFS_OK &&
              vfs_getcwd(&context, cwd, sizeof(cwd), &length) == VFS_OK &&
              strcmp(cwd, "/") == 0,
          ".. stops at the root");
    {
        char too_long[VFS_PATH_MAX + 8U];

        memset(too_long, 'x', sizeof(too_long));
        too_long[sizeof(too_long) - 1U] = '\0';
        CHECK(vfs_stat(&context, too_long, &stat) == VFS_PATH_TOO_LONG,
              "an overlong path is refused by name");
    }
}

static void test_create_write_read(void)
{
    struct vfs_file *file = NULL;
    struct vfs_stat stat;
    char buffer[32];
    size_t done = 0U;

    reset();
    CHECK(vfs_mkdir(&context, "/dir") == VFS_OK, "the test directory exists");
    write_file("dir/file", "hello, vfs");
    CHECK(vfs_stat(&context, "/dir/file", &stat) == VFS_OK &&
              stat.type == VFS_NODE_FILE && stat.size == 10U,
          "the written file keeps its exact size");
    CHECK(vfs_open(&context, "/dir/file", VFS_OPEN_READ, &file) == VFS_OK,
          "the file opens read-only");
    memset(buffer, 0, sizeof(buffer));
    CHECK(vfs_read(file, buffer, sizeof(buffer), &done) == VFS_OK &&
              done == 10U && memcmp(buffer, "hello, vfs", 10U) == 0,
          "the bytes read back");
    CHECK(vfs_write(file, "x", 1U, &done) == VFS_READ_ONLY,
          "a read-only description refuses a write");
    CHECK(vfs_close(file) == VFS_OK, "the read-only file closes");
    CHECK(vfs_open(&context, "/dir/file",
                   VFS_OPEN_WRITE | VFS_OPEN_TRUNC, &file) == VFS_OK,
          "truncate opens the file");
    CHECK(vfs_close(file) == VFS_OK, "the truncated file closes");
    CHECK(vfs_stat(&context, "/dir/file", &stat) == VFS_OK &&
              stat.size == 0U,
          "truncate keeps the node and clears its bytes");
}

static void test_directory_errors(void)
{
    struct vfs_file *file = NULL;
    struct vfs_stat stat;

    reset();
    CHECK(vfs_mkdir(&context, "/a") == VFS_OK, "the first directory exists");
    CHECK(vfs_mkdir(&context, "/a") == VFS_EXISTS,
          "an existing name is refused");
    write_file("file", "x");
    CHECK(vfs_mkdir(&context, "/file") == VFS_EXISTS,
          "a file blocks the same directory name");
    CHECK(vfs_open(&context, "/a", VFS_OPEN_READ, &file) == VFS_IS_DIR,
          "opening a directory as a file is refused");
    CHECK(vfs_mkdir(&context, "/a/b") == VFS_OK, "a nested directory exists");
    CHECK(vfs_unlink(&context, "/a") == VFS_NOT_EMPTY,
          "a non-empty directory is not removed");
    CHECK(vfs_unlink(&context, "/") == VFS_BUSY,
          "the root is never removed");
    CHECK(vfs_unlink(&context, "/missing") == VFS_NOT_FOUND,
          "a missing unlink is NOT_FOUND");
    CHECK(vfs_stat(&context, "/file", &stat) == VFS_OK &&
              stat.type == VFS_NODE_FILE,
          "the file is still there after the refusals");
}

static void test_rename(void)
{
    struct vfs_stat stat;

    reset();
    write_file("old", "data");
    CHECK(vfs_rename(&context, "/old", "/new") == VFS_OK,
          "a file renames in place");
    CHECK(vfs_stat(&context, "/old", &stat) == VFS_NOT_FOUND,
          "the old name is gone");
    CHECK(vfs_stat(&context, "/new", &stat) == VFS_OK && stat.size == 4U,
          "the new name keeps the bytes");
    CHECK(vfs_mkdir(&context, "/dir") == VFS_OK, "the directory exists");
    write_file("dir/other", "x");
    CHECK(vfs_rename(&context, "/new", "/dir/other") == VFS_EXISTS,
          "rename never overwrites an existing name");
    CHECK(vfs_rename(&context, "/dir", "/dir/inside") == VFS_INVALID,
          "a directory cannot move into its own subtree");
    CHECK(vfs_rename(&context, "/new", "/dir/new") == VFS_OK,
          "a file moves into another directory");
    CHECK(vfs_stat(&context, "/dir/new", &stat) == VFS_OK,
          "the moved file is at its new path");
}

static void test_unlink_open_lifetime(void)
{
    struct vfs_file *file = NULL;
    struct vfs_stat stat;
    char buffer[8];
    size_t done = 0U;
    uint32_t nodes_before;
    uint32_t data_before;

    reset();
    write_file("open", "kept");
    nodes_before = vfs_node_count(&filesystem);
    data_before = vfs_data_used(&filesystem);
    CHECK(vfs_open(&context, "/open", VFS_OPEN_READ, &file) == VFS_OK,
          "the file opens before unlink");
    CHECK(vfs_unlink(&context, "/open") == VFS_OK,
          "an open file can be unlinked");
    CHECK(vfs_stat(&context, "/open", &stat) == VFS_NOT_FOUND,
          "the unlinked name is gone");
    memset(buffer, 0, sizeof(buffer));
    CHECK(vfs_read(file, buffer, sizeof(buffer), &done) == VFS_OK &&
              done == 4U && memcmp(buffer, "kept", 4U) == 0,
          "the open description keeps the bytes");
    CHECK(vfs_close(file) == VFS_OK, "the last close releases the file");
    CHECK(vfs_node_count(&filesystem) + 1U == nodes_before,
          "the node is reclaimed after the last close");
    CHECK(vfs_data_used(&filesystem) == 0U && data_before != 0U,
          "the data blocks are reclaimed after the last close");
}

static void test_enospc_rollback(void)
{
    struct vfs_file *file = NULL;
    struct vfs_stat stat;
    uint8_t page[VFS_BLOCK_SIZE];
    size_t done = 0U;
    uint32_t index;
    char name[16];

    reset();
    memset(page, 0xA5, sizeof(page));
    for (index = 0U; index < VFS_DATA_MAX / VFS_FILE_MAX; ++index) {
        snprintf(name, sizeof(name), "big%u", index);
        CHECK(vfs_open(&context, name,
                       VFS_OPEN_CREATE | VFS_OPEN_WRITE, &file) == VFS_OK,
              "a 64 KiB file opens");
        for (uint32_t block = 0U; block < 16U; ++block) {
            CHECK(vfs_write(file, page, sizeof(page), &done) == VFS_OK &&
                      done == sizeof(page),
                  "a full block is written");
        }
        CHECK(vfs_close(file) == VFS_OK, "the full file closes");
    }
    CHECK(vfs_data_used(&filesystem) == VFS_DATA_MAX,
          "the files fill the declared data budget");
    CHECK(vfs_open(&context, "overflow",
                   VFS_OPEN_CREATE | VFS_OPEN_WRITE, &file) == VFS_OK,
          "the fifth file exists but has no data block");
    CHECK(vfs_write(file, page, sizeof(page), &done) == VFS_NO_SPACE &&
              done == 0U,
          "a write with no free block is refused, not truncated");
    CHECK(vfs_close(file) == VFS_OK, "the empty fifth file closes");
    CHECK(vfs_stat(&context, "/big0", &stat) == VFS_OK &&
              stat.size == VFS_FILE_MAX,
          "the refused write left the other files whole");
}

static void test_node_limit(void)
{
    char name[24];
    uint32_t index;
    enum vfs_status status = VFS_OK;

    reset();
    for (index = 0U; index < VFS_MAX_NODES + 4U; ++index) {
        snprintf(name, sizeof(name), "n%u", index);
        status = vfs_mkdir(&context, name);
        if (status != VFS_OK) {
            break;
        }
    }
    CHECK(status == VFS_TOO_MANY_NODES,
          "the node table is a hard bound");
    CHECK(vfs_node_count(&filesystem) <= VFS_MAX_NODES,
          "the node count never exceeds the declared table");
}

static void test_readdir(void)
{
    struct vfs_stat stat;
    char name[64];

    reset();
    CHECK(vfs_mkdir(&context, "/a") == VFS_OK, "directory a");
    CHECK(vfs_mkdir(&context, "/b") == VFS_OK, "directory b");
    write_file("c", "x");
    write_file("longest-name", "x");
    CHECK(vfs_readdir(&context, "/", 0U, name, sizeof(name), &stat) == VFS_OK,
          "the first entry is readable");
    CHECK(vfs_readdir(&context, "/", 1U, name, sizeof(name), &stat) == VFS_OK,
          "the second entry is readable");
    CHECK(vfs_readdir(&context, "/", 2U, name, sizeof(name), &stat) == VFS_OK,
          "the third entry is readable");
    CHECK(vfs_readdir(&context, "/", 4U, name, sizeof(name), &stat) ==
              VFS_NOT_FOUND,
          "past the last entry is NOT_FOUND");
    {
        char small[2];

        CHECK(vfs_readdir(&context, "/", 0U, small, sizeof(small), &stat) ==
                  VFS_NAME_TOO_LONG,
              "a too-small name buffer is refused");
    }
}

int main(void)
{
    test_root_and_paths();
    test_create_write_read();
    test_directory_errors();
    test_rename();
    test_unlink_open_lifetime();
    test_enospc_rollback();
    test_node_limit();
    test_readdir();
    if (failures != 0) {
        fprintf(stderr, "vfs host test: %d failure(s)\n", failures);
        return 1;
    }
    printf("vfs host test: ALL OK\n");
    return 0;
}
