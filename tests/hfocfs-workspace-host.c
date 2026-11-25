/*
 * ITEM-631/635 host proof: the workspace owns exactly partition blocks
 * 514..33281, leaves the state prefix (0..511), the two host-seed companion
 * slots (512..513) and the custody block untouched, refuses an unknown first
 * use, and round-trips a VFS tree through the HFOCFS image.
 */
#include "hfocfs_workspace.h"

#include <stdio.h>
#include <string.h>

#define PARTITION_BYTES (32U * 1024U * 1024U)
#define PARTITION_SECTORS (PARTITION_BYTES / 512U)
#define IMAGE_OFFSET (HFOCFS_WORKSPACE_OFFSET_BLOCKS * 512U)

static unsigned char image[PARTITION_BYTES];
static unsigned char state[262144] __attribute__((aligned(64)));
static int failures;

#define CHECK(cond)                                                          \
    do {                                                                     \
        if (!(cond)) {                                                       \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);           \
            ++failures;                                                      \
        }                                                                    \
    } while (0)

static enum block_device_status dev_read(struct block_device *device,
                                         uint64_t offset, void *buffer,
                                         size_t length)
{
    (void)device;
    if (offset + length > PARTITION_BYTES) {
        return BLOCK_DEVICE_OUT_OF_RANGE;
    }
    memcpy(buffer, image + (size_t)offset, length);
    return BLOCK_DEVICE_OK;
}

static enum block_device_status dev_write(struct block_device *device,
                                          uint64_t offset,
                                          const void *buffer, size_t length)
{
    (void)device;
    if (offset + length > PARTITION_BYTES) {
        return BLOCK_DEVICE_OUT_OF_RANGE;
    }
    memcpy(image + (size_t)offset, buffer, length);
    return BLOCK_DEVICE_OK;
}

static enum block_device_status dev_flush(struct block_device *device)
{
    (void)device;
    return BLOCK_DEVICE_OK;
}

static void device_init(struct block_device *device)
{
    memset(device, 0, sizeof(*device));
    device->block_size = 512U;
    device->block_count = PARTITION_SECTORS;
    device->read_fn = dev_read;
    device->write_fn = dev_write;
    device->flush_fn = dev_flush;
}

static void fill(unsigned char *bytes, size_t length, unsigned value)
{
    memset(bytes, (int)value, length);
}

static int prefix_intact(void)
{
    size_t index;

    for (index = 0U; index < IMAGE_OFFSET; ++index) {
        if (image[index] != 0xa5U) {
            return 0;
        }
    }
    return 1;
}

static int custody_intact(void)
{
    size_t index;

    for (index = PARTITION_BYTES - 512U; index < PARTITION_BYTES; ++index) {
        if (image[index] != 0x5aU) {
            return 0;
        }
    }
    return 1;
}

static size_t host_seed_offset(void)
{
    return (size_t)HFOCFS_WORKSPACE_LAYOUT_BLOCKS * 512U;
}

static size_t host_seed_bytes(void)
{
    return (size_t)HFOCFS_WORKSPACE_HOST_SEED_BLOCKS * 512U;
}

static int host_seed_intact(unsigned value)
{
    size_t index;

    for (index = host_seed_offset();
         index < host_seed_offset() + host_seed_bytes(); ++index) {
        if (image[index] != value) {
            return 0;
        }
    }
    return 1;
}

static int read_file(struct vfs_context *context, const char *path,
                     const char *expected)
{
    struct vfs_file *file = NULL;
    char buffer[256];
    size_t done = 0U;
    size_t length = strlen(expected);

    if (length > sizeof(buffer) ||
        vfs_open(context, path, VFS_OPEN_READ, &file) != VFS_OK) {
        return 0;
    }
    if (vfs_read(file, buffer, length, &done) != VFS_OK ||
        done != length || memcmp(buffer, expected, length) != 0 ||
        vfs_close(file) != VFS_OK) {
        return 0;
    }
    return 1;
}

static int write_file(struct vfs_context *context, const char *path,
                      const char *bytes)
{
    struct vfs_file *file = NULL;
    size_t done = 0U;
    size_t length = strlen(bytes);

    if (vfs_open(context, path, VFS_OPEN_WRITE | VFS_OPEN_CREATE |
                 VFS_OPEN_TRUNC, &file) != VFS_OK) {
        return 0;
    }
    if (vfs_write(file, bytes, length, &done) != VFS_OK ||
        done != length || vfs_close(file) != VFS_OK) {
        return 0;
    }
    return 1;
}

static void test_roundtrip(void)
{
    struct block_device device;
    struct hfocfs_workspace workspace;
    struct vfs fs;
    struct vfs_context context;

    fill(image, sizeof(image), 0U);
    fill(image, IMAGE_OFFSET, 0xa5U);
    fill(image + PARTITION_BYTES - 512U, 512U, 0x5aU);
    device_init(&device);
    CHECK(hfocfs_workspace_bind(&device, state, sizeof(state), true, &workspace) ==
          HFOCFS_WORKSPACE_OK);
    CHECK(prefix_intact());
    CHECK(host_seed_intact(0xa5U));
    CHECK(custody_intact());
    CHECK(memcmp(image + IMAGE_OFFSET, HFOCFS_MAGIC, HFOCFS_MAGIC_SIZE) == 0);

    CHECK(vfs_init(&fs) == VFS_OK);
    CHECK(vfs_context_init(&context, &fs) == VFS_OK);
    CHECK(hfocfs_workspace_import(&workspace, &fs, &context) ==
          HFOCFS_WORKSPACE_OK);
    CHECK(vfs_mkdir(&context, "/home/owner/docs") == VFS_OK);
    CHECK(write_file(&context, "/home/owner/docs/readme.txt", "persisted-1"));
    CHECK(write_file(&context, "/home/owner/note.txt", "hello"));
    CHECK(hfocfs_workspace_sync(&workspace, &fs, &context) ==
          HFOCFS_WORKSPACE_OK);
    CHECK(prefix_intact());
    CHECK(custody_intact());

    CHECK(vfs_init(&fs) == VFS_OK);
    CHECK(vfs_context_init(&context, &fs) == VFS_OK);
    CHECK(hfocfs_workspace_import(&workspace, &fs, &context) ==
          HFOCFS_WORKSPACE_OK);
    CHECK(read_file(&context, "/home/owner/docs/readme.txt", "persisted-1"));
    CHECK(read_file(&context, "/home/owner/note.txt", "hello"));

    CHECK(write_file(&context, "/home/owner/docs/readme.txt", "persisted-2"));
    CHECK(hfocfs_workspace_sync(&workspace, &fs, &context) ==
          HFOCFS_WORKSPACE_OK);
    CHECK(vfs_init(&fs) == VFS_OK);
    CHECK(vfs_context_init(&context, &fs) == VFS_OK);
    CHECK(hfocfs_workspace_import(&workspace, &fs, &context) ==
          HFOCFS_WORKSPACE_OK);
    CHECK(read_file(&context, "/home/owner/docs/readme.txt", "persisted-2"));
    CHECK(read_file(&context, "/home/owner/note.txt", "hello"));
    CHECK(prefix_intact());
    CHECK(custody_intact());
}

static void test_refusal_and_target(void)
{
    struct block_device device;
    struct hfocfs_workspace workspace;

    fill(image, sizeof(image), 0U);
    fill(image + IMAGE_OFFSET, 512U, 0x11U);
    device_init(&device);
    CHECK(hfocfs_workspace_bind(&device, state, sizeof(state), true, &workspace) ==
          HFOCFS_WORKSPACE_REFUSED);
    CHECK(image[IMAGE_OFFSET] == 0x11U);

    fill(image, sizeof(image), 0U);
    device_init(&device);
    CHECK(hfocfs_workspace_bind(&device, state, sizeof(state), false,
                                &workspace) ==
          HFOCFS_WORKSPACE_UNINITIALIZED);
    CHECK(image[IMAGE_OFFSET + 16] == 0U);

    /*
     * ITEM-635 regression: an all-zero first use formats only the declared
     * image.  The two host-seed companion slots must stay zero, otherwise
     * appliance_host_seed_store_present() reads the HFOCFS magic as a seed and
     * the console starts an entropy login prompt that has no local secret.
     */
    fill(image, sizeof(image), 0U);
    device_init(&device);
    CHECK(hfocfs_workspace_bind(&device, state, sizeof(state), true, &workspace) ==
          HFOCFS_WORKSPACE_OK);
    CHECK(host_seed_intact(0U));
    CHECK(memcmp(image + IMAGE_OFFSET, HFOCFS_MAGIC, HFOCFS_MAGIC_SIZE) == 0);

    /* A non-zero host-seed slot is also outside the image and is preserved. */
    fill(image, sizeof(image), 0U);
    fill(image + host_seed_offset(), host_seed_bytes(), 0x7eU);
    device_init(&device);
    CHECK(hfocfs_workspace_bind(&device, state, sizeof(state), true, &workspace) ==
          HFOCFS_WORKSPACE_OK);
    CHECK(host_seed_intact(0x7eU));
    CHECK(memcmp(image + IMAGE_OFFSET, HFOCFS_MAGIC, HFOCFS_MAGIC_SIZE) == 0);

    fill(image, sizeof(image), 0U);
    device_init(&device);
    device.block_count = 1024U; /* too small to be the declared partition */
    CHECK(hfocfs_workspace_bind(&device, state, sizeof(state), true, &workspace) ==
          HFOCFS_WORKSPACE_BAD_TARGET);

    device_init(&device);
    CHECK(hfocfs_workspace_bind(NULL, state, sizeof(state), true,
                                &workspace) == HFOCFS_WORKSPACE_NULL);
    CHECK(hfocfs_workspace_sync(&workspace, NULL, NULL) ==
          HFOCFS_WORKSPACE_NULL);
}

int main(void)
{
    test_roundtrip();
    test_refusal_and_target();
    if (failures != 0) {
        printf("HFOCFS-WORKSPACE-HOST FAILED: %d\n", failures);
        return 1;
    }
    printf("HFOCFS-WORKSPACE-HOST PASS\n");
    return 0;
}
