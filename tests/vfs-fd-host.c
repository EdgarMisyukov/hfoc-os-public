/*
 * ITEM-623 host proof for real open-file descriptions bound to the process
 * descriptor table.  The proof drives the production descriptor.c and vfs.c:
 * an offset is shared across dup2/inheritance, independent opens keep their
 * own offsets, the last close releases the VFS file, and a descriptor from
 * another process table is unreachable.
 */

#include "descriptor.h"
#include "vfs.h"
#include "vfs_fd.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifdef HFOC_VFS_FD_HOST_TEST

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
static struct descriptor_table table;
static struct descriptor_table clone_table;
static struct file_description descriptions[16];
static enum vfs_status last_close_status;

static enum vfs_status description_read(struct file_description *description,
                                        void *buffer, size_t size,
                                        size_t *done_out)
{
    return vfs_read((struct vfs_file *)description->context, buffer, size,
                    done_out);
}

static enum vfs_status description_write(struct file_description *description,
                                         const void *buffer, size_t size,
                                         size_t *done_out)
{
    return vfs_write((struct vfs_file *)description->context, buffer, size,
                     done_out);
}

static void description_close(struct file_description *description)
{
    last_close_status =
        vfs_close((struct vfs_file *)description->context);
    description->context = NULL;
}

static const struct descriptor_ops ops = {
    .read = description_read,
    .write = description_write,
    .close = description_close,
};

static void reset(void)
{
    CHECK(vfs_init(&filesystem) == VFS_OK, "the VFS initialises");
    CHECK(vfs_context_init(&context, &filesystem) == VFS_OK,
          "the VFS context starts at the root");
    memset(descriptions, 0, sizeof(descriptions));
    memset(&table, 0, sizeof(table));
    memset(&clone_table, 0, sizeof(clone_table));
    CHECK(descriptor_table_init(&table) == DESCRIPTOR_OK,
          "the process descriptor table initialises");
    last_close_status = VFS_OK;
}

static enum descriptor_status bind_file(uint32_t index, const char *path,
                                        uint32_t flags, uint32_t *fd_out)
{
    return vfs_file_bind(&table, &context, path, flags,
                         &descriptions[index], fd_out);
}

static void test_bind_and_shared_offset(void)
{
    struct file_description *description = NULL;
    char buffer[8];
    size_t done = 0U;
    uint32_t first = 0U;
    uint32_t second = 0U;

    reset();
    CHECK(bind_file(0U, "/data",
                    VFS_OPEN_CREATE | VFS_OPEN_WRITE | VFS_OPEN_READ,
                    &first) == DESCRIPTOR_OK,
          "a VFS file binds to descriptor 0");
    CHECK(first == 0U && descriptor_table_open_count(&table) == 1U,
          "the first descriptor number is 0");
    CHECK(descriptor_table_lookup(&table, first, &description) ==
              DESCRIPTOR_OK && description == &descriptions[0],
          "the bound description is found by number");
    CHECK(description->ops->write(description, "abcdef", 6U, &done) ==
              VFS_OK && done == 6U,
          "the shared description writes through its ops");
    CHECK(descriptor_table_dup2(&table, first, 5U) == DESCRIPTOR_OK,
          "dup2 copies the descriptor");
    CHECK(descriptor_table_lookup(&table, 5U, &description) == DESCRIPTOR_OK &&
              description == &descriptions[0] && description->refcount == 2U,
          "dup2 shares one description and increments its refcount");
    CHECK(description->ops->write(description, "GH", 2U, &done) == VFS_OK,
          "the duplicate continues at the shared offset");
    CHECK(descriptor_table_close(&table, 0U) == DESCRIPTOR_OK,
          "closing one duplicate succeeds");
    CHECK(descriptions[0].refcount == 1U &&
              descriptor_table_open_count(&table) == 1U,
          "the description stays alive for the remaining descriptor");
    CHECK(descriptor_table_dup2(&table, 5U, 6U) == DESCRIPTOR_OK &&
              descriptor_table_close(&table, 5U) == DESCRIPTOR_OK,
          "a second dup2 works");
    CHECK(descriptor_table_lookup(&table, 6U, &description) == DESCRIPTOR_OK,
          "the surviving duplicate is reachable");
    memset(buffer, 0, sizeof(buffer));
    CHECK(description->ops->read(description, buffer, 2U, &done) == VFS_OK &&
              done == 0U,
          "the shared offset is already at the end of the file");
    CHECK(descriptor_table_close(&table, 6U) == DESCRIPTOR_OK,
          "the last close succeeds");
    CHECK(descriptions[0].published == false &&
              last_close_status == VFS_OK,
          "the last close releases the VFS file exactly once");
    {
        struct vfs_file *file = NULL;

        CHECK(vfs_open(&context, "/data", VFS_OPEN_READ, &file) == VFS_OK,
              "the file can be opened again after the release");
        CHECK(vfs_close(file) == VFS_OK, "the direct re-open closes");
    }
}

static void test_independent_offsets(void)
{
    struct file_description *read_a = NULL;
    struct file_description *read_b = NULL;
    char a[4];
    char b[4];
    size_t done = 0U;
    uint32_t fd_a = 0U;
    uint32_t fd_b = 0U;

    reset();
    {
        struct vfs_file *file = NULL;

        CHECK(vfs_open(&context, "/text",
                       VFS_OPEN_CREATE | VFS_OPEN_WRITE, &file) == VFS_OK &&
                  vfs_write(file, "abcdef", 6U, &done) == VFS_OK &&
                  vfs_close(file) == VFS_OK,
              "the fixture file is written");
    }
    CHECK(bind_file(0U, "/text", VFS_OPEN_READ, &fd_a) == DESCRIPTOR_OK &&
              bind_file(1U, "/text", VFS_OPEN_READ, &fd_b) == DESCRIPTOR_OK,
          "two independent opens bind");
    CHECK(descriptor_table_lookup(&table, fd_a, &read_a) == DESCRIPTOR_OK &&
              descriptor_table_lookup(&table, fd_b, &read_b) == DESCRIPTOR_OK &&
              read_a != read_b,
          "independent opens have distinct descriptions");
    memset(a, 0, sizeof(a));
    memset(b, 0, sizeof(b));
    CHECK(read_a->ops->read(read_a, a, 2U, &done) == VFS_OK && done == 2U &&
              memcmp(a, "ab", 2U) == 0,
          "the first open reads at its own offset");
    CHECK(read_b->ops->read(read_b, b, 2U, &done) == VFS_OK && done == 2U &&
              memcmp(b, "ab", 2U) == 0,
          "the second open still starts at zero");
    CHECK(read_a->ops->write(read_a, "x", 1U, &done) == VFS_READ_ONLY,
          "a read-only description refuses a write");
    CHECK(descriptor_table_close_all(&table) == DESCRIPTOR_OK &&
              descriptor_table_open_count(&table) == 0U,
          "close_all releases every descriptor");
}

static void test_clone_and_ownership(void)
{
    struct file_description *description = NULL;
    uint32_t descriptor = 0U;
    char buffer[4];
    size_t done = 0U;

    reset();
    {
        struct vfs_file *file = NULL;

        CHECK(vfs_open(&context, "/owned",
                       VFS_OPEN_CREATE | VFS_OPEN_WRITE, &file) == VFS_OK &&
                  vfs_write(file, "keep", 4U, &done) == VFS_OK &&
                  vfs_close(file) == VFS_OK,
              "the inheritance fixture is written");
    }
    CHECK(bind_file(0U, "/owned", VFS_OPEN_READ, &descriptor) ==
              DESCRIPTOR_OK,
          "the fixture binds");
    CHECK(descriptor_table_clone(&table, &clone_table) == DESCRIPTOR_OK,
          "the descriptor table clones for a child");
    CHECK(descriptor_table_lookup(&clone_table, descriptor, &description) ==
              DESCRIPTOR_OK && descriptions[0].refcount == 2U,
          "the clone shares the description and counts the reference");
    CHECK(descriptor_table_destroy(&table) == DESCRIPTOR_OK,
          "the parent table is destroyed");
    memset(buffer, 0, sizeof(buffer));
    CHECK(description->ops->read(description, buffer, 4U, &done) == VFS_OK &&
              done == 4U && memcmp(buffer, "keep", 4U) == 0,
          "the child keeps a working inherited descriptor");
    CHECK(descriptor_table_close(&clone_table, descriptor) == DESCRIPTOR_OK &&
              descriptions[0].published == false,
          "the inherited last close releases the file");
    CHECK(descriptor_table_destroy(&clone_table) == DESCRIPTOR_OK,
          "the child table is destroyed");
}

static void test_unreachable_descriptor(void)
{
    struct descriptor_table other;
    struct file_description *description = NULL;
    uint32_t descriptor = 0U;

    reset();
    CHECK(bind_file(0U, "/one", VFS_OPEN_CREATE | VFS_OPEN_WRITE,
                    &descriptor) == DESCRIPTOR_OK,
          "the fixture binds in the first table");
    memset(&other, 0, sizeof(other));
    CHECK(descriptor_table_init(&other) == DESCRIPTOR_OK,
          "a second process table initialises");
    CHECK(descriptor_table_lookup(&other, descriptor, &description) ==
              DESCRIPTOR_BAD_DESCRIPTOR,
          "a descriptor number from another process is unreachable");
    CHECK(descriptor_table_close(&other, descriptor) ==
              DESCRIPTOR_BAD_DESCRIPTOR,
          "another process cannot close it either");
    CHECK(descriptor_table_destroy(&table) == DESCRIPTOR_OK,
          "the original table closes its descriptor");
    CHECK(descriptor_table_destroy(&other) == DESCRIPTOR_OK,
          "the second table is empty");
}

static void test_validation(void)
{
    uint32_t descriptor = 0U;

    reset();
    CHECK(descriptor_table_lookup(&table, 64U, NULL) == DESCRIPTOR_NULL,
          "lookup with a null output is refused");
    CHECK(descriptor_table_dup2(&table, 0U, 1U) == DESCRIPTOR_BAD_DESCRIPTOR,
          "dup2 of an unbound descriptor is refused");
    CHECK(descriptor_description_init(&descriptions[0], &ops, NULL, true,
                                      false) == DESCRIPTOR_OK,
          "a description initialises");
    CHECK(descriptor_description_init(&descriptions[0], &ops, NULL, true,
                                      false) == DESCRIPTOR_BAD_STATE,
          "a published description is not re-initialised");
    CHECK(descriptor_table_bind(&table, &descriptions[0], &descriptor) ==
              DESCRIPTOR_OK && descriptor == 0U,
          "bind consumes the initial reference");
    CHECK(descriptor_table_dup2(&table, 0U, 0U) == DESCRIPTOR_OK,
          "dup2 onto the same number is a no-op");
    CHECK(descriptions[0].refcount == 1U,
          "the no-op did not change the refcount");
    CHECK(descriptor_table_destroy(&table) == DESCRIPTOR_OK,
          "the table releases the description");
}

int main(void)
{
    test_bind_and_shared_offset();
    test_independent_offsets();
    test_clone_and_ownership();
    test_unreachable_descriptor();
    test_validation();
    if (failures != 0) {
        fprintf(stderr, "vfs fd host test: %d failure(s)\n", failures);
        return 1;
    }
    printf("vfs fd host test: ALL OK\n");
    return 0;
}

#else
int main(void)
{
    return 0;
}
#endif
