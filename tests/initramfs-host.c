/*
 * ITEM-623 host proof for the deterministic initramfs archive and mount.
 *
 * The vectors are built independently of scripts/hfoc-rootfs-pack.py; the
 * make target also packs a real fixture with that script and hands the bytes
 * to this binary, so parser and packer are checked against each other without
 * either one re-implementing the production import.
 */

#include "initramfs.h"
#include "vfs.h"
#include "vfs_mount.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef HFOC_INITRAMFS_HOST_TEST

#define MAX_ARCHIVE INITRAMFS_MAX_SIZE
#define ENTRY_HEADER 24U

static int failures;

#define CHECK(cond, msg) do { \
    if (!(cond)) { \
        fprintf(stderr, "FAIL: %s\n", msg); \
        ++failures; \
    } else { \
        printf("PASS: %s\n", msg); \
    } \
} while (0)

static uint8_t archive[MAX_ARCHIVE];
static struct vfs filesystem;
static struct vfs_context context;

static uint32_t read_u32(const uint8_t *bytes)
{
    return (uint32_t)bytes[0] | ((uint32_t)bytes[1] << 8) |
        ((uint32_t)bytes[2] << 16) | ((uint32_t)bytes[3] << 24);
}

static void write_u16(uint8_t *bytes, uint16_t value)
{
    bytes[0] = (uint8_t)value;
    bytes[1] = (uint8_t)(value >> 8);
}

static void write_u32(uint8_t *bytes, uint32_t value)
{
    bytes[0] = (uint8_t)value;
    bytes[1] = (uint8_t)(value >> 8);
    bytes[2] = (uint8_t)(value >> 16);
    bytes[3] = (uint8_t)(value >> 24);
}

static uint32_t hash_bytes(uint32_t hash, const uint8_t *bytes, size_t length)
{
    size_t index;

    for (index = 0U; index < length; ++index) {
        hash ^= bytes[index];
        hash *= UINT32_C(16777619);
    }
    return hash;
}

static uint32_t align4(uint32_t value)
{
    return (value + 3U) & ~UINT32_C(3);
}

struct builder {
    uint32_t size;
    uint32_t entries;
};

static void builder_begin(struct builder *builder)
{
    memset(archive, 0, sizeof(archive));
    memset(archive, 0, 32U);
    memcpy(archive, "HFOCIR01", 8U);
    write_u32(archive + 8U, 1U);
    builder->size = 32U;
    builder->entries = 0U;
}

static bool builder_add(struct builder *builder, uint16_t type,
                        const char *path, const void *data, uint32_t length)
{
    const uint32_t path_length = (uint32_t)strlen(path);
    uint32_t header = builder->size;
    uint32_t data_offset = align4(header + ENTRY_HEADER + path_length);
    uint32_t next = align4(data_offset + length);

    if (next > sizeof(archive) || path_length >= VFS_PATH_MAX ||
        builder->entries >= INITRAMFS_MAX_ENTRIES) {
        return false;
    }
    write_u16(archive + header, type);
    write_u16(archive + header + 2U, type == INITRAMFS_ENTRY_DIR ? 0755U
                                                                 : 0644U);
    write_u32(archive + header + 4U, path_length);
    write_u32(archive + header + 8U, length);
    write_u32(archive + header + 12U, data_offset);
    write_u32(archive + header + 16U,
              hash_bytes(hash_bytes(UINT32_C(2166136261),
                                    (const uint8_t *)path, path_length),
                         data, length));
    memcpy(archive + header + ENTRY_HEADER, path, path_length);
    if (length != 0U) {
        memcpy(archive + data_offset, data, length);
    }
    builder->size = next;
    ++builder->entries;
    return true;
}

static void builder_finish(struct builder *builder)
{
    write_u32(archive + 12U, builder->entries);
    write_u32(archive + 16U, builder->size);
    write_u32(archive + 20U,
              hash_bytes(UINT32_C(2166136261), archive + 32U,
                         builder->size - 32U));
}

static enum initramfs_status parse(struct initramfs_archive *parsed)
{
    return initramfs_parse(archive, read_u32(archive + 16U), parsed);
}

static void reseal(const struct builder *builder)
{
    write_u32(archive + 20U, hash_bytes(UINT32_C(2166136261), archive + 32U,
                                        builder->size - 32U));
}

static void make_tree(struct builder *builder)
{
    static const char shell[] = "native shell placeholder\n";
    static const char motd[] = "HFOC OS\n";

    builder_begin(builder);
    CHECK(builder_add(builder, INITRAMFS_ENTRY_DIR, "/bin", NULL, 0U),
          "builder adds /bin");
    CHECK(builder_add(builder, INITRAMFS_ENTRY_DIR, "/etc", NULL, 0U),
          "builder adds /etc");
    CHECK(builder_add(builder, INITRAMFS_ENTRY_FILE, "/bin/sh", shell,
                      (uint32_t)sizeof(shell) - 1U),
          "builder adds /bin/sh");
    CHECK(builder_add(builder, INITRAMFS_ENTRY_FILE, "/etc/motd", motd,
                      (uint32_t)sizeof(motd) - 1U),
          "builder adds /etc/motd");
    builder_finish(builder);
}

static void reset_vfs(void)
{
    CHECK(vfs_init(&filesystem) == VFS_OK, "the VFS initialises");
    CHECK(vfs_context_init(&context, &filesystem) == VFS_OK,
          "the VFS context starts at the root");
}

static void test_parse_and_import(void)
{
    struct builder builder;
    struct initramfs_archive parsed;
    struct vfs_file *file = NULL;
    struct vfs_stat stat;
    char buffer[32];
    size_t done = 0U;

    make_tree(&builder);
    CHECK(parse(&parsed) == INITRAMFS_OK, "a valid archive parses");
    CHECK(parsed.entry_count == 4U && parsed.data_bytes == 33U,
          "the parser reports the declared entries and data bytes");
    reset_vfs();
    CHECK(vfs_mkdir(&context, "/tmp") == VFS_OK, "the RAM /tmp exists");
    CHECK(initramfs_import(&parsed, &filesystem, &context, "/") ==
              INITRAMFS_OK,
          "the validated tree imports at /");
    CHECK(vfs_stat(&context, "/bin/sh", &stat) == VFS_OK &&
              stat.type == VFS_NODE_FILE && stat.read_only &&
              stat.size == sizeof("native shell placeholder\n") - 1U,
          "the initramfs file is published read-only");
    CHECK(vfs_open(&context, "/etc/motd", VFS_OPEN_READ, &file) == VFS_OK,
          "an initramfs file opens for reading");
    memset(buffer, 0, sizeof(buffer));
    CHECK(vfs_read(file, buffer, sizeof(buffer), &done) == VFS_OK &&
              done == 8U && memcmp(buffer, "HFOC OS\n", 8U) == 0,
          "the initramfs bytes read back");
    CHECK(vfs_close(file) == VFS_OK, "the read-only file closes");
    CHECK(vfs_open(&context, "/etc/motd",
                   VFS_OPEN_WRITE | VFS_OPEN_TRUNC, &file) == VFS_READ_ONLY,
          "a write open of an initramfs file is refused");
    CHECK(vfs_mkdir(&context, "/bin/extra") == VFS_READ_ONLY,
          "a directory below the read-only mount is refused");
    CHECK(vfs_unlink(&context, "/etc/motd") == VFS_READ_ONLY,
          "an unlink below the read-only mount is refused");
    CHECK(vfs_open(&context, "/tmp/scratch",
                   VFS_OPEN_CREATE | VFS_OPEN_WRITE, &file) == VFS_OK,
          "the RAM /tmp stays writable");
    CHECK(vfs_close(file) == VFS_OK, "/tmp file closes");
}

static void test_rejections(void)
{
    struct builder builder;
    struct initramfs_archive parsed;
    uint32_t archive_checksum;

    make_tree(&builder);
    archive[0] = 'X';
    CHECK(parse(&parsed) == INITRAMFS_BAD_MAGIC, "a bad magic is refused");
    make_tree(&builder);
    write_u32(archive + 8U, 2U);
    CHECK(parse(&parsed) == INITRAMFS_BAD_VERSION,
          "an unknown version is refused");
    make_tree(&builder);
    write_u32(archive + 16U, builder.size + 4U);
    CHECK(initramfs_parse(archive, builder.size, &parsed) ==
              INITRAMFS_BAD_SIZE,
          "a declared size beyond the module is refused");
    make_tree(&builder);
    archive[builder.size - 1U] ^= 0x40U;
    CHECK(parse(&parsed) == INITRAMFS_BAD_CHECKSUM,
          "a corrupted body is refused by the archive checksum");
    make_tree(&builder);
    write_u32(archive + 28U, 1U);
    CHECK(parse(&parsed) == INITRAMFS_BAD_FLAGS,
          "unknown flags are refused");
    make_tree(&builder);
    write_u32(archive + 12U, INITRAMFS_MAX_ENTRIES + 1U);
    CHECK(parse(&parsed) == INITRAMFS_TOO_MANY_ENTRIES,
          "an oversized entry count is refused");
    make_tree(&builder);
    write_u16(archive + 32U, 9U);
    reseal(&builder);
    CHECK(parse(&parsed) == INITRAMFS_BAD_TYPE,
          "a special entry type is refused");
    make_tree(&builder);
    write_u32(archive + 32U + 12U, read_u32(archive + 32U + 12U) + 4U);
    archive_checksum = hash_bytes(UINT32_C(2166136261), archive + 32U,
                                  builder.size - 32U);
    write_u32(archive + 20U, archive_checksum);
    CHECK(parse(&parsed) == INITRAMFS_BAD_OFFSET,
          "a moved data offset is refused");
    make_tree(&builder);
    archive[32U + ENTRY_HEADER] = 'x';
    archive_checksum = hash_bytes(UINT32_C(2166136261), archive + 32U,
                                  builder.size - 32U);
    write_u32(archive + 20U, archive_checksum);
    CHECK(parse(&parsed) == INITRAMFS_BAD_PATH,
          "a relative path is refused");

    builder_begin(&builder);
    CHECK(builder_add(&builder, INITRAMFS_ENTRY_FILE, "/a/b", "x", 1U),
          "builder writes an orphan path");
    builder_finish(&builder);
    CHECK(parse(&parsed) == INITRAMFS_MISSING_PARENT,
          "a missing parent is refused");
    builder_begin(&builder);
    CHECK(builder_add(&builder, INITRAMFS_ENTRY_FILE, "/a", "x", 1U),
          "builder writes a file parent");
    CHECK(builder_add(&builder, INITRAMFS_ENTRY_FILE, "/a/b", "x", 1U),
          "builder writes a child of that file");
    builder_finish(&builder);
    CHECK(parse(&parsed) == INITRAMFS_NOT_DIRECTORY,
          "a file cannot be a parent");
    builder_begin(&builder);
    CHECK(builder_add(&builder, INITRAMFS_ENTRY_DIR, "/a", NULL, 0U),
          "builder writes a directory");
    CHECK(builder_add(&builder, INITRAMFS_ENTRY_DIR, "/a", NULL, 0U),
          "builder writes a duplicate");
    builder_finish(&builder);
    CHECK(parse(&parsed) == INITRAMFS_DUPLICATE,
          "a duplicate path is refused");
    builder_begin(&builder);
    CHECK(builder_add(&builder, INITRAMFS_ENTRY_FILE, "/../escape", "x", 1U),
          "builder writes a escaping path");
    builder_finish(&builder);
    CHECK(parse(&parsed) == INITRAMFS_BAD_PATH,
          "a dot-dot path is refused");
    builder_begin(&builder);
    CHECK(builder_add(&builder, INITRAMFS_ENTRY_DIR, "/d", NULL, 0U),
          "builder writes a directory for padding");
    builder_finish(&builder);
    write_u32(archive + 16U, builder.size + 4U);
    builder.size += 4U;
    reseal(&builder);
    CHECK(parse(&parsed) == INITRAMFS_TRAILING_BYTES,
          "trailing bytes after the last entry are refused");
}

static void test_mount_table(void)
{
    struct builder builder;
    struct vfs_mount_table mounts;
    struct initramfs_archive parsed;
    const struct vfs_mount *mount = NULL;
    const char *relative = NULL;
    struct vfs_stat stat;

    make_tree(&builder);
    CHECK(parse(&parsed) == INITRAMFS_OK, "the mount archive parses");
    reset_vfs();
    CHECK(vfs_mkdir(&context, "/tmp") == VFS_OK, "the RAM /tmp exists");
    CHECK(vfs_mkdir(&context, "/run") == VFS_OK, "the RAM /run exists");
    CHECK(vfs_mkdir(&context, "/opt") == VFS_OK, "the mountpoint /opt exists");
    CHECK(vfs_mount_table_init(&mounts) == VFS_OK, "the mount table starts");
    CHECK(vfs_mount_ram(&mounts, &filesystem, &context, "/tmp") == VFS_OK,
          "a RAM mount is recorded");
    CHECK(vfs_mount_initramfs(&mounts, &filesystem, &context, "/",
                              archive, read_u32(archive + 16U)) == VFS_OK,
          "the initramfs mounts at /");
    CHECK(vfs_mount_count(&mounts) == 2U, "both mounts are counted");
    CHECK(vfs_mount_ram(&mounts, &filesystem, &context, "/tmp") == VFS_BUSY,
          "a duplicate RAM mount is refused");
    CHECK(vfs_mount_initramfs(&mounts, &filesystem, &context, "/",
                              archive, read_u32(archive + 16U)) == VFS_BUSY,
          "a second root mount is refused");
    CHECK(vfs_mount_lookup(&mounts, "/bin/sh", &mount, &relative) == VFS_OK &&
              strcmp(mount->prefix, "/") == 0 &&
              strcmp(relative, "/bin/sh") == 0,
          "the root mount answers with the full relative path");
    CHECK(vfs_mount_lookup(&mounts, "/tmp/x", &mount, &relative) == VFS_OK &&
              mount->kind == VFS_MOUNT_RAM &&
              strcmp(relative, "/x") == 0,
          "the RAM mount strips its prefix");
    CHECK(vfs_stat(&context, "/bin/sh", &stat) == VFS_OK && stat.read_only,
          "the root mount is auditable as read-only");
    CHECK(vfs_mount_initramfs(&mounts, &filesystem, &context, "/nested",
                              archive, read_u32(archive + 16U)) ==
              VFS_NOT_FOUND,
          "a mount below a missing parent is refused");
}

static void load_packed_archive(const char *path)
{
    FILE *file;
    long length;
    uint8_t *bytes;
    struct initramfs_archive parsed;
    struct vfs_file *readback = NULL;
    char buffer[32];
    size_t done = 0U;

    file = fopen(path, "rb");
    CHECK(file != NULL, "the packed fixture archive opens");
    if (file == NULL) {
        return;
    }
    CHECK(fseek(file, 0, SEEK_END) == 0, "the fixture length is readable");
    length = ftell(file);
    CHECK(length > 0 && (size_t)length <= MAX_ARCHIVE,
          "the fixture fits the bounded archive size");
    CHECK(fseek(file, 0, SEEK_SET) == 0, "the fixture rewinds");
    bytes = malloc((size_t)length);
    CHECK(bytes != NULL, "the fixture buffer allocates");
    if (bytes == NULL) {
        fclose(file);
        return;
    }
    CHECK(fread(bytes, 1U, (size_t)length, file) == (size_t)length,
          "the fixture is read whole");
    fclose(file);
    CHECK(initramfs_parse(bytes, (size_t)length, &parsed) == INITRAMFS_OK,
          "the packer output parses");
    reset_vfs();
    CHECK(initramfs_import(&parsed, &filesystem, &context, "/") ==
              INITRAMFS_OK,
          "the packer output imports");
    CHECK(vfs_open(&context, "/etc/motd", VFS_OPEN_READ, &readback) ==
              VFS_OK,
          "a file from the packed tree opens");
    memset(buffer, 0, sizeof(buffer));
    CHECK(vfs_read(readback, buffer, sizeof(buffer), &done) == VFS_OK &&
              done == 8U && memcmp(buffer, "HFOC OS\n", 8U) == 0,
          "a file from the packed tree reads back");
    CHECK(vfs_close(readback) == VFS_OK, "the packed file closes");
    free(bytes);
}

int main(int argc, char **argv)
{
    test_parse_and_import();
    test_rejections();
    test_mount_table();
    if (argc > 1) {
        load_packed_archive(argv[1]);
    }
    if (failures != 0) {
        fprintf(stderr, "initramfs host test: %d failure(s)\n", failures);
        return 1;
    }
    printf("initramfs host test: ALL OK\n");
    return 0;
}

#else
int main(void)
{
    return 0;
}
#endif
