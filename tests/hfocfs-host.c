/*
 * ITEM-630 host proof: format an empty HFOCFS v1 image and decode it back
 * with an independent byte-level reader (no writer helpers are called here).
 */
#include "hfocfs.h"
#include "sha256.h"

#include <stdio.h>
#include <string.h>

#define IMAGE_BYTES (16U * 1024U * 1024U)
#define IMAGE_SECTORS (IMAGE_BYTES / 512U)

static unsigned char image[IMAGE_BYTES];
static unsigned char snapshot[IMAGE_BYTES];

static int failures;
static int fault_armed;
static int fault_at;
static int fault_ops;

#define CHECK(cond)                                                          \
    do {                                                                     \
        if (!(cond)) {                                                       \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);           \
            ++failures;                                                      \
        }                                                                    \
    } while (0)

static unsigned read_u16(const unsigned char *p)
{
    return (unsigned)p[0] | ((unsigned)p[1] << 8);
}

static unsigned read_u32(const unsigned char *p)
{
    return (unsigned)p[0] | ((unsigned)p[1] << 8) | ((unsigned)p[2] << 16) |
           ((unsigned)p[3] << 24);
}

static unsigned long long read_u64(const unsigned char *p)
{
    return (unsigned long long)read_u32(p) |
           ((unsigned long long)read_u32(p + 4) << 32);
}

static void store_u32(unsigned char *p, unsigned value)
{
    p[0] = (unsigned char)value;
    p[1] = (unsigned char)(value >> 8);
    p[2] = (unsigned char)(value >> 16);
    p[3] = (unsigned char)(value >> 24);
}

static unsigned crc32(const unsigned char *bytes, size_t length)
{
    unsigned crc = 0xffffffffU;
    size_t i;
    unsigned bit;
    for (i = 0; i < length; ++i) {
        crc ^= bytes[i];
        for (bit = 0; bit < 8; ++bit) {
            crc = (crc >> 1) ^ (0xedb88320U & (unsigned)-(int)(crc & 1U));
        }
    }
    return crc ^ 0xffffffffU;
}

static unsigned header_crc(const unsigned char *block)
{
    unsigned char joined[60];
    memcpy(joined, block, 20);
    memcpy(joined + 20, block + 24, 40);
    return crc32(joined, sizeof(joined));
}

static enum block_device_status dev_read(struct block_device *dev,
                                         uint64_t offset, void *buffer,
                                         size_t length)
{
    if (offset + length > IMAGE_BYTES || length > IMAGE_BYTES) {
        return BLOCK_DEVICE_OUT_OF_RANGE;
    }
    memcpy(buffer, image + (size_t)offset, length);
    (void)dev;
    return BLOCK_DEVICE_OK;
}

static enum block_device_status dev_write(struct block_device *dev,
                                          uint64_t offset, const void *buffer,
                                          size_t length)
{
    if (offset + length > IMAGE_BYTES || length > IMAGE_BYTES) {
        return BLOCK_DEVICE_OUT_OF_RANGE;
    }
    memcpy(image + (size_t)offset, buffer, length);
    (void)dev;
    if (fault_armed && ++fault_ops == fault_at) {
        return BLOCK_DEVICE_IO_ERROR;
    }
    return BLOCK_DEVICE_OK;
}

static enum block_device_status dev_flush(struct block_device *dev)
{
    (void)dev;
    if (fault_armed && ++fault_ops == fault_at) {
        return BLOCK_DEVICE_IO_ERROR;
    }
    return BLOCK_DEVICE_OK;
}

static void device_init(struct block_device *dev)
{
    memset(dev, 0, sizeof(*dev));
    dev->block_size = 512;
    dev->block_count = IMAGE_SECTORS;
    dev->read_fn = dev_read;
    dev->write_fn = dev_write;
    dev->flush_fn = dev_flush;
}

static int slot_crc_ok(const unsigned char *slot)
{
    unsigned char scratch[504];
    memcpy(scratch, slot, sizeof(scratch));
    return crc32(scratch, sizeof(scratch)) == read_u32(slot + 0x1f8);
}

static void test_format_and_decode(void)
{
    struct block_device dev;
    unsigned char state[262144] __attribute__((aligned(64)));
    struct hfocfs *fs = NULL;
    struct hfocfs_info info;
    const unsigned char *sb;
    const unsigned char *commit;
    const unsigned char *inode;
    const unsigned char *bitmap;
    unsigned char digest[32];
    static const unsigned char uuid[16] = {
        0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17,
        0x18, 0x19, 0x1a, 0x1b, 0x1c, 0x1d, 0x1e, 0x1f,
    };
    struct hfocfs_options options;

    memset(image, 0x5a, sizeof(image));
    device_init(&dev);
    options.label = "hfocfs-test";
    options.uuid = uuid;
    CHECK(hfocfs_state_size() > 0U);
    CHECK(hfocfs_state_size() <= sizeof(state));
    CHECK(hfocfs_format(&dev, &options, state, sizeof(state)) == HFOCFS_OK);
    CHECK(image[0] == 'H');

    sb = image;
    CHECK(memcmp(sb, "HFOCFS1", 7) == 0);
    CHECK(read_u32(sb + 8) == 1);
    CHECK(read_u32(sb + 0x0c) == 4096);
    CHECK(read_u64(sb + 0x10) == 4096);
    CHECK(read_u64(sb + 0x18) == 1);
    CHECK(read_u64(sb + 0x20) == 2);
    CHECK(read_u32(sb + 0x28) == 4096);
    CHECK((read_u32(sb + 0x2c) & 1U) == 1U);
    CHECK(slot_crc_ok(sb));
    CHECK(slot_crc_ok(image + 4096));
    CHECK(memcmp(sb + 0x30, sb + 4096 + 0x30, 32) == 0);

    commit = image + 2 * 4096;
    CHECK(read_u32(commit) == 0x48464342U);
    CHECK(read_u32(commit + 4) == 4);
    CHECK(read_u64(commit + 8) == 1);
    CHECK(read_u64(commit + 0x18) == 2);
    CHECK(read_u32(commit + 0x14) == header_crc(commit));
    CHECK(sha256_compute(commit, 4096, digest) == SHA256_OK);
    CHECK(memcmp(digest, sb + 0x30, 32) == 0);
    CHECK(read_u64(commit + 0x40) == 1);
    CHECK(read_u32(commit + 0x48) == 1);
    CHECK(read_u32(commit + 0x4c) == 1);
    CHECK(read_u32(commit + 0x50) == 1);
    CHECK(read_u32(commit + 0x54) == 1);
    CHECK(read_u64(commit + 0x58) == 3);
    CHECK(read_u64(commit + 0xd8) == 4);
    CHECK(read_u32(commit + 0x118) ==
          crc32(commit + 0x40, 0xd8));

    inode = image + 3 * 4096 + 0x40;
    CHECK(read_u32(inode) == 1);
    CHECK(inode[4] == 2 && inode[5] == 0);
    CHECK(read_u32(inode + 0x0c) == 1);
    CHECK(read_u32(inode + 0xa8) == crc32(inode, 0xa8));

    bitmap = image + 4 * 4096 + 0x40;
    CHECK((bitmap[0] & 0x1fU) == 0x1fU);
    CHECK((bitmap[4096 / 8] & (1U << (4096 % 8))) != 0U);
    CHECK((bitmap[5 / 8] & (1U << (5 % 8))) == 0U);

    CHECK(hfocfs_mount(&dev, state, sizeof(state), &fs) == HFOCFS_OK);
    CHECK(fs != NULL);
    CHECK(hfocfs_info(fs, &info) == HFOCFS_OK);
    CHECK(info.generation == 1);
    CHECK(info.block_count == 4096);
    CHECK(info.node_count == 1);
    CHECK(info.root_ino == 1);
    CHECK(info.clean);
    CHECK(memcmp(info.uuid, uuid, 16) == 0);
    CHECK(strcmp(info.label, "hfocfs-test") == 0);
    CHECK(hfocfs_flush(fs) == HFOCFS_OK);
    CHECK(hfocfs_unmount(fs) == HFOCFS_OK);
    CHECK(hfocfs_unmount(fs) == HFOCFS_NOT_MOUNTED);
}

static void test_corruption(void)
{
    struct block_device dev;
    unsigned char state[262144] __attribute__((aligned(64)));
    struct hfocfs *fs = NULL;
    struct hfocfs_options options;
    unsigned char saved;

    device_init(&dev);
    options.label = "corrupt";
    options.uuid = NULL;
    CHECK(hfocfs_format(&dev, &options, state, sizeof(state)) == HFOCFS_OK);

    /* One torn superblock must fall back to the other one. */
    saved = image[0];
    image[0] = 'X';
    CHECK(hfocfs_mount(&dev, state, sizeof(state), &fs) == HFOCFS_OK);
    CHECK(hfocfs_unmount(fs) == HFOCFS_OK);
    image[0] = saved;

    /* Both torn: no valid slot. */
    image[0] = 'X';
    image[4096] = 'X';
    CHECK(hfocfs_mount(&dev, state, sizeof(state), &fs) == HFOCFS_CORRUPT);
    image[4096] = saved;
    image[0] = saved;

    /* A changed commit byte breaks both slots' SHA binding. */
    saved = image[2 * 4096 + 0x100];
    image[2 * 4096 + 0x100] = (unsigned char)(saved ^ 0xffU);
    CHECK(hfocfs_mount(&dev, state, sizeof(state), &fs) == HFOCFS_CORRUPT);
    image[2 * 4096 + 0x100] = saved;

    /* Equal generations pointing at different digests are ambiguous. */
    memcpy(image + 4096, image, 512);
    image[4096 + 0x30] ^= 0x01U;
    store_u32(image + 4096 + 0x1f8, crc32(image + 4096, 504));
    CHECK(hfocfs_mount(&dev, state, sizeof(state), &fs) == HFOCFS_CORRUPT);
    memcpy(image + 4096, image, 512);

    /* A metadata block missing from the bitmap is corruption. */
    saved = image[4 * 4096 + 0x40];
    image[4 * 4096 + 0x40] = (unsigned char)(saved & ~0x10U);
    CHECK(hfocfs_mount(&dev, state, sizeof(state), &fs) == HFOCFS_CORRUPT);
    image[4 * 4096 + 0x40] = saved;

    /* A clear bit past block_count in the bitmap tail is corruption. */
    saved = image[4 * 4096 + 0x40 + 5000 / 8];
    image[4 * 4096 + 0x40 + 5000 / 8] &= (unsigned char)~(1U << (5000 % 8));
    CHECK(hfocfs_mount(&dev, state, sizeof(state), &fs) == HFOCFS_CORRUPT);
    image[4 * 4096 + 0x40 + 5000 / 8] = saved;

    /* A bad inode CRC is corruption. */
    saved = image[3 * 4096 + 0x40 + 0x10];
    image[3 * 4096 + 0x40 + 0x10] ^= 0x01U;
    CHECK(hfocfs_mount(&dev, state, sizeof(state), &fs) == HFOCFS_CORRUPT);
    image[3 * 4096 + 0x40 + 0x10] = saved;

    CHECK(hfocfs_mount(&dev, state, sizeof(state), &fs) == HFOCFS_OK);
    CHECK(hfocfs_unmount(fs) == HFOCFS_OK);
}

static void test_geometry(void)
{
    struct block_device dev;
    unsigned char state[262144] __attribute__((aligned(64)));
    struct hfocfs *fs = NULL;

    device_init(&dev);
    dev.block_count = (1024U * 1024U) / 512U; /* 1 MiB */
    CHECK(hfocfs_format(&dev, NULL, state, sizeof(state)) == HFOCFS_TOO_SMALL);
    CHECK(hfocfs_mount(&dev, state, sizeof(state), &fs) == HFOCFS_TOO_SMALL);

    device_init(&dev);
    CHECK(hfocfs_format(NULL, NULL, state, sizeof(state)) == HFOCFS_NULL);
    CHECK(hfocfs_mount(NULL, state, sizeof(state), &fs) == HFOCFS_NULL);
}

/* ------------------------------------------------------------------------- */
/* Independent raw-image decoder: no hfocfs.c encoder helper is used.        */

static int raw_slot_valid(const unsigned char *slot)
{
    if (memcmp(slot, "HFOCFS1", 7) != 0) {
        return 0;
    }
    if (read_u32(slot + 0x1f8) != crc32(slot, 0x1f8)) {
        return 0;
    }
    return read_u32(slot + 0x0c) == 4096;
}

static const unsigned char *raw_selected_slot(void)
{
    int first = raw_slot_valid(image);
    int second = raw_slot_valid(image + 4096);

    if (!first && !second) {
        return NULL;
    }
    if (!first) {
        return image + 4096;
    }
    if (!second) {
        return image;
    }
    return read_u64(image + 0x18) >= read_u64(image + 4096 + 0x18)
               ? image : image + 4096;
}

static int raw_inode(unsigned ino, unsigned char out[176])
{
    const unsigned char *slot = raw_selected_slot();
    const unsigned char *commit;
    unsigned inode_blocks;
    unsigned index;
    unsigned block;
    unsigned char *records;

    if (slot == NULL || ino == 0) {
        return 0;
    }
    commit = image + (size_t)read_u64(slot + 0x20) * 4096U;
    inode_blocks = read_u32(commit + 0x40 + 0x08);
    index = (ino - 1U) / 22U;
    if (index >= inode_blocks) {
        return 0;
    }
    block = (unsigned)read_u64(commit + 0x40 + 0x18 + index * 8U);
    records = image + (size_t)block * 4096U + 0x40;
    memcpy(out, records + ((ino - 1U) % 22U) * 176U, 176U);
    return read_u32(out) == ino;
}

static int raw_lookup(unsigned dir_ino, const char *name, unsigned *ino_out)
{
    unsigned char inode[176];
    size_t wanted = strlen(name);
    unsigned index;

    if (!raw_inode(dir_ino, inode)) {
        return 0;
    }
    for (index = 0U; index < 18U; ++index) {
        unsigned block = (unsigned)read_u64(inode + 0x18 + index * 8U);
        unsigned char *payload;
        size_t position = 0U;

        if (block == 0U) {
            continue;
        }
        payload = image + (size_t)block * 4096U + 0x40;
        while (position + 8U <= 4032U) {
            unsigned ino = read_u32(payload + position);
            unsigned name_length = read_u16(payload + position + 4U);
            unsigned rec_length = read_u16(payload + position + 6U);

            if (ino == 0U && name_length == 0U && rec_length == 0U) {
                break;
            }
            if (rec_length < 16U || rec_length > 4032U - position) {
                return 0;
            }
            if (name_length == wanted &&
                memcmp(payload + position + 8U, name, wanted) == 0) {
                *ino_out = ino;
                return 1;
            }
            position += rec_length;
        }
    }
    return 0;
}

static int read_matches(struct hfocfs *fs, const char *path,
                        const unsigned char *expected, size_t length)
{
    uint32_t ino;
    unsigned char buffer[8192];
    size_t done = 0U;

    if (length > sizeof(buffer) ||
        hfocfs_lookup(fs, path, &ino) != HFOCFS_OK) {
        return 0;
    }
    if (hfocfs_read(fs, ino, 0U, buffer, length, &done) != HFOCFS_OK) {
        return 0;
    }
    return done == length && memcmp(buffer, expected, length) == 0;
}

static void fill_pattern(unsigned char *bytes, size_t length, unsigned seed)
{
    size_t index;

    for (index = 0U; index < length; ++index) {
        bytes[index] = (unsigned char)((index * 7U + seed) & 0xffU);
    }
}

static void test_tree_roundtrip(void)
{
    struct block_device dev;
    unsigned char state[262144] __attribute__((aligned(64)));
    struct hfocfs *fs = NULL;
    struct hfocfs_info info;
    struct hfocfs_stat stat;
    unsigned char pattern[5000];
    unsigned char small[32];
    size_t done;
    uint32_t ino;
    uint32_t child;
    enum hfocfs_node_type type;
    char name[64];
    unsigned raw;
    unsigned raw_child;
    unsigned char raw_inode_bytes[176];
    static const unsigned char uuid[16] = {
        0x20, 0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x27,
        0x28, 0x29, 0x2a, 0x2b, 0x2c, 0x2d, 0x2e, 0x2f,
    };
    struct hfocfs_options options;

    memset(image, 0, sizeof(image));
    device_init(&dev);
    options.label = "tree";
    options.uuid = uuid;
    CHECK(hfocfs_format(&dev, &options, state, sizeof(state)) == HFOCFS_OK);
    CHECK(hfocfs_mount(&dev, state, sizeof(state), &fs) == HFOCFS_OK);
    CHECK(hfocfs_create(fs, "/etc", HFOCFS_NODE_DIRECTORY, 0, &ino) ==
          HFOCFS_OK);
    CHECK(hfocfs_create(fs, "/etc/motd", HFOCFS_NODE_FILE, 0, &ino) ==
          HFOCFS_OK);
    CHECK(hfocfs_write(fs, ino, 0, "hello hfocfs\n", 13, &done) ==
          HFOCFS_OK && done == 13U);
    CHECK(hfocfs_create(fs, "/bin", HFOCFS_NODE_DIRECTORY, 0, &ino) ==
          HFOCFS_OK);
    CHECK(hfocfs_create(fs, "/bin/tool", HFOCFS_NODE_FILE, 0, &ino) ==
          HFOCFS_OK);
    fill_pattern(pattern, sizeof(pattern), 11U);
    CHECK(hfocfs_write(fs, ino, 0, pattern, sizeof(pattern), &done) ==
          HFOCFS_OK && done == sizeof(pattern));
    CHECK(hfocfs_stat_path(fs, "/bin/tool", &stat) == HFOCFS_OK);
    CHECK(stat.size == sizeof(pattern));
    CHECK(hfocfs_read(fs, ino, 0, small, sizeof(small), &done) == HFOCFS_OK);
    CHECK(done == sizeof(small) && memcmp(small, pattern, sizeof(small)) == 0);
    CHECK(hfocfs_readdir(fs, 1U, 0U, name, sizeof(name), &child, &type) ==
          HFOCFS_OK && strcmp(name, "etc") == 0 && type == HFOCFS_NODE_DIRECTORY);
    CHECK(hfocfs_readdir(fs, 1U, 1U, name, sizeof(name), &child, &type) ==
          HFOCFS_OK && strcmp(name, "bin") == 0 && type == HFOCFS_NODE_DIRECTORY);
    CHECK(hfocfs_readdir(fs, 1U, 2U, name, sizeof(name), &child, &type) ==
          HFOCFS_NOT_FOUND);
    CHECK(hfocfs_lookup(fs, "/..", &ino) == HFOCFS_OK && ino == 1U);
    CHECK(hfocfs_lookup(fs, "/etc/../bin", &ino) == HFOCFS_OK);
    CHECK(hfocfs_flush(fs) == HFOCFS_OK);
    CHECK(hfocfs_info(fs, &info) == HFOCFS_OK && info.generation == 2U);
    CHECK(hfocfs_unmount(fs) == HFOCFS_OK);

    /* Decode the committed image without calling a writer helper. */
    CHECK(raw_slot_valid(raw_selected_slot()));
    CHECK(raw_selected_slot() != NULL &&
          read_u64(raw_selected_slot() + 0x18) == 2U);
    CHECK(raw_lookup(1U, "etc", &raw));
    CHECK(raw_lookup(raw, "motd", &raw_child));
    CHECK(raw_inode(raw_child, raw_inode_bytes));
    CHECK(read_u32(raw_inode_bytes + 0x08) == 13U);
    {
        unsigned block = (unsigned)read_u64(raw_inode_bytes + 0x18);
        CHECK(block != 0U);
        CHECK(memcmp(image + (size_t)block * 4096U + 0x40,
                     "hello hfocfs\n", 13) == 0);
    }
    CHECK(raw_lookup(1U, "bin", &raw));
    CHECK(raw_lookup(raw, "tool", &raw_child));
    CHECK(raw_inode(raw_child, raw_inode_bytes));
    CHECK(read_u32(raw_inode_bytes + 0x08) == sizeof(pattern));
    {
        unsigned block = (unsigned)read_u64(raw_inode_bytes + 0x18);
        CHECK(block != 0U);
        CHECK(memcmp(image + (size_t)block * 4096U + 0x40,
                     pattern, 1024U) == 0);
    }

    CHECK(hfocfs_mount(&dev, state, sizeof(state), &fs) == HFOCFS_OK);
    CHECK(read_matches(fs, "/etc/motd",
                       (const unsigned char *)"hello hfocfs\n", 13U));
    CHECK(read_matches(fs, "/bin/tool", pattern, sizeof(pattern)));
    CHECK(hfocfs_rename(fs, "/etc/motd", "/etc/message") == HFOCFS_OK);
    CHECK(hfocfs_lookup(fs, "/etc/motd", &ino) == HFOCFS_NOT_FOUND);
    CHECK(hfocfs_unlink(fs, "/bin/tool") == HFOCFS_OK);
    CHECK(hfocfs_lookup(fs, "/bin/tool", &ino) == HFOCFS_NOT_FOUND);
    CHECK(hfocfs_create(fs, "/open", HFOCFS_NODE_FILE, 0, &ino) == HFOCFS_OK);
    CHECK(hfocfs_write(fs, ino, 0, "unlinked", 8, &done) == HFOCFS_OK);
    CHECK(hfocfs_open(fs, ino) == HFOCFS_OK);
    CHECK(hfocfs_unlink(fs, "/open") == HFOCFS_OK);
    CHECK(hfocfs_lookup(fs, "/open", &ino) == HFOCFS_NOT_FOUND);
    CHECK(hfocfs_flush(fs) == HFOCFS_OK);
    CHECK(hfocfs_unmount(fs) == HFOCFS_OK);

    CHECK(hfocfs_mount(&dev, state, sizeof(state), &fs) == HFOCFS_OK);
    CHECK(hfocfs_info(fs, &info) == HFOCFS_OK && info.generation == 3U);
    CHECK(read_matches(fs, "/etc/message",
                       (const unsigned char *)"hello hfocfs\n", 13U));
    CHECK(hfocfs_lookup(fs, "/etc/motd", &ino) == HFOCFS_NOT_FOUND);
    CHECK(hfocfs_lookup(fs, "/bin/tool", &ino) == HFOCFS_NOT_FOUND);
    CHECK(hfocfs_lookup(fs, "/open", &ino) == HFOCFS_NOT_FOUND);
    CHECK(hfocfs_readdir(fs, 1U, 0U, name, sizeof(name), &child, &type) ==
          HFOCFS_OK && strcmp(name, "etc") == 0);
    CHECK(hfocfs_readdir(fs, 1U, 1U, name, sizeof(name), &child, &type) ==
          HFOCFS_OK && strcmp(name, "bin") == 0);
    CHECK(hfocfs_readdir(fs, 1U, 2U, name, sizeof(name), &child, &type) ==
          HFOCFS_NOT_FOUND);
    CHECK(hfocfs_unmount(fs) == HFOCFS_OK);
}

static void test_fault_injection(void)
{
    struct block_device dev;
    unsigned char state[262144] __attribute__((aligned(64)));
    struct hfocfs *fs = NULL;
    struct hfocfs_info info;
    uint32_t ino;
    size_t done;
    int total;
    int fail_at;
    static const unsigned char base[] = "base";
    static const unsigned char fresh[] = "new-data";

    memset(image, 0, sizeof(image));
    device_init(&dev);
    CHECK(hfocfs_format(&dev, NULL, state, sizeof(state)) == HFOCFS_OK);
    CHECK(hfocfs_mount(&dev, state, sizeof(state), &fs) == HFOCFS_OK);
    CHECK(hfocfs_create(fs, "/a", HFOCFS_NODE_FILE, 0, &ino) == HFOCFS_OK);
    CHECK(hfocfs_write(fs, ino, 0, base, sizeof(base), &done) == HFOCFS_OK);
    CHECK(hfocfs_flush(fs) == HFOCFS_OK);
    CHECK(hfocfs_unmount(fs) == HFOCFS_OK);
    memcpy(snapshot, image, sizeof(snapshot));

    CHECK(hfocfs_mount(&dev, state, sizeof(state), &fs) == HFOCFS_OK);
    CHECK(hfocfs_create(fs, "/b", HFOCFS_NODE_FILE, 0, &ino) == HFOCFS_OK);
    CHECK(hfocfs_write(fs, ino, 0, fresh, sizeof(fresh), &done) == HFOCFS_OK);
    fault_ops = 0;
    fault_at = 0;
    fault_armed = 1;
    CHECK(hfocfs_flush(fs) == HFOCFS_OK);
    total = fault_ops;
    fault_armed = 0;
    CHECK(total > 3);
    CHECK(hfocfs_unmount(fs) == HFOCFS_OK);

    for (fail_at = 1; fail_at <= total; ++fail_at) {
        enum hfocfs_status status;

        memcpy(image, snapshot, sizeof(image));
        CHECK(hfocfs_mount(&dev, state, sizeof(state), &fs) == HFOCFS_OK);
        CHECK(hfocfs_create(fs, "/b", HFOCFS_NODE_FILE, 0, &ino) == HFOCFS_OK);
        CHECK(hfocfs_write(fs, ino, 0, fresh, sizeof(fresh), &done) ==
              HFOCFS_OK);
        fault_ops = 0;
        fault_at = fail_at;
        fault_armed = 1;
        status = hfocfs_flush(fs);
        fault_armed = 0;
        CHECK(status == HFOCFS_OK || status == HFOCFS_IO ||
              status == HFOCFS_NOSPC);
        CHECK(hfocfs_unmount(fs) == HFOCFS_OK);
        CHECK(hfocfs_mount(&dev, state, sizeof(state), &fs) == HFOCFS_OK);
        CHECK(hfocfs_info(fs, &info) == HFOCFS_OK);
        CHECK(info.generation == 2U || info.generation == 3U);
        CHECK(read_matches(fs, "/a", base, sizeof(base)));
        if (hfocfs_lookup(fs, "/b", &ino) == HFOCFS_OK) {
            CHECK(info.generation == 3U);
            CHECK(read_matches(fs, "/b", fresh, sizeof(fresh)));
        } else {
            CHECK(info.generation == 2U);
        }
        CHECK(hfocfs_unmount(fs) == HFOCFS_OK);
    }
}

static void test_enospc_is_explicit(void)
{
    struct block_device dev;
    unsigned char state[262144] __attribute__((aligned(64)));
    struct hfocfs *fs = NULL;
    unsigned char data[60000];
    uint32_t ino;
    size_t done;
    int index;
    int saw_nospc = 0;
    static const unsigned char keep[] = "keep";

    memset(image, 0, sizeof(image));
    device_init(&dev);
    dev.block_count = (4U * 1024U * 1024U) / 512U;
    CHECK(hfocfs_format(&dev, NULL, state, sizeof(state)) == HFOCFS_OK);
    CHECK(hfocfs_mount(&dev, state, sizeof(state), &fs) == HFOCFS_OK);
    CHECK(hfocfs_create(fs, "/keep", HFOCFS_NODE_FILE, 0, &ino) == HFOCFS_OK);
    CHECK(hfocfs_write(fs, ino, 0, keep, sizeof(keep), &done) == HFOCFS_OK);
    CHECK(hfocfs_flush(fs) == HFOCFS_OK);

    fill_pattern(data, sizeof(data), 3U);
    for (index = 0; index < 200; ++index) {
        char path[32];
        enum hfocfs_status status;

        snprintf(path, sizeof(path), "/f%03d", index);
        status = hfocfs_create(fs, path, HFOCFS_NODE_FILE, 0, &ino);
        if (status == HFOCFS_NO_MEMORY) {
            break;
        }
        CHECK(status == HFOCFS_OK);
        status = hfocfs_write(fs, ino, 0, data, sizeof(data), &done);
        if (status == HFOCFS_NOSPC) {
            saw_nospc = 1;
            break;
        }
        CHECK(status == HFOCFS_OK);
    }
    CHECK(saw_nospc || index < 200);
    {
        enum hfocfs_status status = hfocfs_flush(fs);

        CHECK(status == HFOCFS_OK || status == HFOCFS_NOSPC);
    }
    CHECK(hfocfs_unmount(fs) == HFOCFS_OK);
    CHECK(hfocfs_mount(&dev, state, sizeof(state), &fs) == HFOCFS_OK);
    CHECK(read_matches(fs, "/keep", keep, sizeof(keep)));
    CHECK(hfocfs_unmount(fs) == HFOCFS_OK);
}

static void test_tree_corruption(void)
{
    struct block_device dev;
    unsigned char state[262144] __attribute__((aligned(64)));
    struct hfocfs *fs = NULL;
    uint32_t ino;
    size_t done;
    unsigned char inode[176];
    unsigned block;
    unsigned inode_table_block;
    unsigned char *record;
    unsigned char saved;

    memset(image, 0, sizeof(image));
    device_init(&dev);
    CHECK(hfocfs_format(&dev, NULL, state, sizeof(state)) == HFOCFS_OK);
    CHECK(hfocfs_mount(&dev, state, sizeof(state), &fs) == HFOCFS_OK);
    CHECK(hfocfs_create(fs, "/a", HFOCFS_NODE_FILE, 0, &ino) == HFOCFS_OK);
    CHECK(hfocfs_write(fs, ino, 0, "x", 1, &done) == HFOCFS_OK);
    CHECK(hfocfs_flush(fs) == HFOCFS_OK);
    CHECK(hfocfs_unmount(fs) == HFOCFS_OK);

    CHECK(raw_inode(1U, inode));
    block = (unsigned)read_u64(inode + 0x18);
    CHECK(block != 0U);
    saved = image[(size_t)block * 4096U + 0x40 + 6U];
    image[(size_t)block * 4096U + 0x40 + 6U] = 0U;
    CHECK(hfocfs_mount(&dev, state, sizeof(state), &fs) == HFOCFS_CORRUPT);
    image[(size_t)block * 4096U + 0x40 + 6U] = saved;

    /* A file data block pointing past the image is corruption. */
    inode_table_block = (unsigned)read_u64(
        image + (size_t)read_u64(raw_selected_slot() + 0x20) * 4096U +
        0x40 + 0x18);
    record = image + (size_t)inode_table_block * 4096U + 0x40 +
             ((ino - 1U) % 22U) * 176U;
    memcpy(inode, record, sizeof(inode));
    {
        uint64_t bad = 0xffffffffffffffffULL;

        memcpy(record + 0x18, &bad, 8);
    }
    store_u32(record + 0xa8, crc32(record, 0xa8));
    CHECK(hfocfs_mount(&dev, state, sizeof(state), &fs) == HFOCFS_CORRUPT);
    memcpy(record, inode, sizeof(inode));
    CHECK(hfocfs_mount(&dev, state, sizeof(state), &fs) == HFOCFS_OK);
    CHECK(hfocfs_unmount(fs) == HFOCFS_OK);
}

int main(void)
{
    CHECK(crc32((const unsigned char *)"123456789", 9) == 0xcbf43926U);
    test_format_and_decode();
    test_corruption();
    test_geometry();
    test_tree_roundtrip();
    test_tree_corruption();
    test_fault_injection();
    test_enospc_is_explicit();
    if (failures != 0) {
        printf("HFOCFS-HOST FAILED: %d\n", failures);
        return 1;
    }
    printf("HFOCFS-HOST PASS\n");
    return 0;
}
