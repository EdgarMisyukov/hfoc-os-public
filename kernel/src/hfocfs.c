/*
 * ITEM-630: HFOCFS v1, the native writable filesystem for a caller-supplied
 * image.  The normative byte-level format is /srv/hfoc-example/queue/
 * 630-hfocfs-format.md.
 *
 * The implementation is deliberately heap-free: the caller provides one
 * bounded state object, all tree nodes live in it, and the only I/O is whole
 * 4096-byte filesystem blocks through the existing block_device interface.
 * Every commit is copy-on-write: new blocks are written and flushed, then the
 * commit record, then the superblock slot.  A power cut therefore leaves the
 * previous or the next generation recoverable, never a partial tree.
 *
 * This is not an encryption claim and not a POSIX/Linux ABI claim.
 */

#include "hfocfs.h"

#include "sha256.h"

#include <string.h>

#define SB_SLOT_SIZE 512U
#define SB_CRC_OFFSET 0x1f8U
#define SB_CRC_LENGTH 0x1f8U
#define SB_MAGIC_OFFSET 0x000U
#define SB_VERSION_OFFSET 0x008U
#define SB_BLOCK_SIZE_OFFSET 0x00cU
#define SB_BLOCK_COUNT_OFFSET 0x010U
#define SB_GENERATION_OFFSET 0x018U
#define SB_COMMIT_BLOCK_OFFSET 0x020U
#define SB_COMMIT_LENGTH_OFFSET 0x028U
#define SB_FLAGS_OFFSET 0x02cU
#define SB_SHA_OFFSET 0x030U
#define SB_UUID_OFFSET 0x050U
#define SB_LABEL_OFFSET 0x060U
#define SB_LABEL_BYTES 32U

#define HDR_MAGIC_OFFSET 0x00U
#define HDR_KIND_OFFSET 0x04U
#define HDR_FLAGS_OFFSET 0x06U
#define HDR_GENERATION_OFFSET 0x08U
#define HDR_PAYLOAD_LEN_OFFSET 0x10U
#define HDR_CRC_OFFSET 0x14U
#define HDR_BLOCK_NO_OFFSET 0x18U

#define COMMIT_GENERATION_OFFSET 0x00U
#define COMMIT_INODE_COUNT_OFFSET 0x08U
#define COMMIT_BITMAP_COUNT_OFFSET 0x0cU
#define COMMIT_ROOT_OFFSET 0x10U
#define COMMIT_NODE_COUNT_OFFSET 0x14U
#define COMMIT_INODE_BLOCKS_OFFSET 0x18U
#define COMMIT_BITMAP_BLOCKS_OFFSET 0x98U
#define COMMIT_CRC_OFFSET 0xd8U

#define INODE_SIZE 176U
#define INODE_INO_OFFSET 0x00U
#define INODE_TYPE_OFFSET 0x04U
#define INODE_MODE_OFFSET 0x06U
#define INODE_SIZE_OFFSET 0x08U
#define INODE_LINK_OFFSET 0x0cU
#define INODE_MTIME_OFFSET 0x10U
#define INODE_BLOCKS_OFFSET 0x18U
#define INODE_CRC_OFFSET 0xa8U
#define INODE_CRC_LENGTH 0xa8U

#define DIRENT_HEADER_SIZE 8U
#define DIRENT_ALIGN 8U
#define DIRENT_MIN_SIZE 16U

#define BITMAP_BITS_PER_BYTE 8U
#define BITMAP_BITS_PER_BLOCK \
    (HFOCFS_BLOCK_PAYLOAD_SIZE * BITMAP_BITS_PER_BYTE)
#define BITMAP_BYTES_MAX HFOCFS_BLOCK_PAYLOAD_SIZE

struct hfocfs_node {
    bool used;
    bool dirty;
    bool unlinked;
    enum hfocfs_node_type type;
    uint16_t mode;
    uint32_t size;
    uint32_t link_count;
    uint32_t open_count;
    uint64_t mtime;
    uint32_t parent;
    uint16_t name_len;
    char name[HFOCFS_MAX_NAME + 1U];
    uint64_t blocks[HFOCFS_DIRECT_BLOCKS];
};

struct hfocfs {
    bool used;
    bool read_only;
    bool dirty;
    struct block_device *device;
    uint64_t block_count;
    uint64_t generation;
    uint64_t commit_block;
    uint32_t node_count;
    uint32_t root_ino;
    bool clean;
    uint8_t uuid[HFOCFS_UUID_SIZE];
    char label[HFOCFS_LABEL_SIZE + 1U];
    /* Allocation view: the selected tree plus every older block kept safe. */
    uint8_t bitmap[BITMAP_BYTES_MAX];
    /* Only the selected generation's reachable blocks, for the next commit. */
    uint8_t base_bitmap[BITMAP_BYTES_MAX];
    struct hfocfs_node nodes[HFOCFS_MAX_NODES];
    uint8_t block[HFOCFS_BLOCK_SIZE];
    uint8_t aux[HFOCFS_BLOCK_SIZE];
    uint8_t block2[HFOCFS_BLOCK_SIZE];
};

static uint32_t read_u32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint64_t read_u64(const uint8_t *p)
{
    return (uint64_t)read_u32(p) | ((uint64_t)read_u32(p + 4) << 32);
}

static void write_u16(uint8_t *p, uint16_t value)
{
    p[0] = (uint8_t)value;
    p[1] = (uint8_t)(value >> 8);
}

static void write_u32(uint8_t *p, uint32_t value)
{
    p[0] = (uint8_t)value;
    p[1] = (uint8_t)(value >> 8);
    p[2] = (uint8_t)(value >> 16);
    p[3] = (uint8_t)(value >> 24);
}

static void write_u64(uint8_t *p, uint64_t value)
{
    write_u32(p, (uint32_t)value);
    write_u32(p + 4, (uint32_t)(value >> 32));
}

/* zlib CRC-32 (reflected, polynomial 0xEDB88320). */
static uint32_t crc32_bytes(const uint8_t *bytes, size_t length)
{
    uint32_t crc = 0xffffffffU;
    size_t i;
    unsigned bit;

    for (i = 0; i < length; ++i) {
        crc ^= bytes[i];
        for (bit = 0; bit < 8U; ++bit) {
            uint32_t mask = (uint32_t)-(int32_t)(crc & 1U);
            crc = (crc >> 1) ^ (0xedb88320U & mask);
        }
    }
    return crc ^ 0xffffffffU;
}

static uint32_t header_crc(const uint8_t *block)
{
    uint8_t joined[HFOCFS_BLOCK_HEADER_SIZE - 4U];
    size_t used = 0;

    memcpy(joined, block, HDR_CRC_OFFSET);
    used = HDR_CRC_OFFSET;
    /* CRC covers 0x00..0x13 and 0x18..0x3f; 0x14..0x17 stays zero. */
    memcpy(joined + used, block + HDR_BLOCK_NO_OFFSET,
           HFOCFS_BLOCK_HEADER_SIZE - HDR_BLOCK_NO_OFFSET);
    used += HFOCFS_BLOCK_HEADER_SIZE - HDR_BLOCK_NO_OFFSET;
    return crc32_bytes(joined, used);
}

static bool header_valid_any(const uint8_t *block, uint16_t kind,
                             uint64_t block_no)
{
    if (read_u32(block + HDR_MAGIC_OFFSET) != HFOCFS_BLOCK_MAGIC) {
        return false;
    }
    if ((uint16_t)block[HDR_KIND_OFFSET] != kind ||
        block[HDR_KIND_OFFSET + 1U] != 0U) {
        return false;
    }
    if (read_u64(block + HDR_BLOCK_NO_OFFSET) != block_no) {
        return false;
    }
    if (read_u32(block + HDR_CRC_OFFSET) != header_crc(block)) {
        return false;
    }
    return read_u32(block + HDR_PAYLOAD_LEN_OFFSET) <=
           HFOCFS_BLOCK_PAYLOAD_SIZE;
}

static bool header_valid_exact(const uint8_t *block, uint16_t kind,
                               uint64_t generation, uint64_t block_no)
{
    return header_valid_any(block, kind, block_no) &&
           read_u64(block + HDR_GENERATION_OFFSET) == generation;
}

static uint64_t header_generation(const uint8_t *block)
{
    return read_u64(block + HDR_GENERATION_OFFSET);
}

static enum hfocfs_status read_bytes(struct block_device *device,
                                     uint64_t index, void *buffer,
                                     size_t length)
{
    enum block_device_status status;
    uint64_t offset;

    if (device == NULL || index >= device->block_count || length == 0U ||
        length > HFOCFS_BLOCK_SIZE) {
        return HFOCFS_CORRUPT;
    }
    offset = index * (uint64_t)HFOCFS_BLOCK_SIZE;
    if (offset + length > device->block_count * (uint64_t)device->block_size) {
        return HFOCFS_CORRUPT;
    }
    status = block_device_read(device, offset, buffer, length);
    return status == BLOCK_DEVICE_OK ? HFOCFS_OK : HFOCFS_IO;
}

static enum hfocfs_status read_block(struct block_device *device,
                                     uint64_t index, void *buffer)
{
    return read_bytes(device, index, buffer, HFOCFS_BLOCK_SIZE);
}

static enum hfocfs_status write_block(struct block_device *device,
                                      uint64_t index, const void *buffer)
{
    enum block_device_status status;
    uint64_t offset;

    if (device == NULL || index >= device->block_count) {
        return HFOCFS_BAD_ARGUMENT;
    }
    offset = index * (uint64_t)HFOCFS_BLOCK_SIZE;
    status = block_device_write(device, offset, buffer, HFOCFS_BLOCK_SIZE);
    return status == BLOCK_DEVICE_OK ? HFOCFS_OK : HFOCFS_IO;
}

static enum hfocfs_status flush_device(struct block_device *device)
{
    return block_device_flush(device) == BLOCK_DEVICE_OK ? HFOCFS_OK
                                                         : HFOCFS_IO;
}

static void set_bitmap_bit(uint8_t *payload, uint64_t bit)
{
    payload[bit / BITMAP_BITS_PER_BYTE] |=
        (uint8_t)(1U << (bit % BITMAP_BITS_PER_BYTE));
}

static bool bitmap_bit(const uint8_t *payload, uint64_t bit)
{
    return (payload[bit / BITMAP_BITS_PER_BYTE] &
            (uint8_t)(1U << (bit % BITMAP_BITS_PER_BYTE))) != 0U;
}

static void finalise_bitmap_tail(uint8_t *payload, uint64_t block_count)
{
    uint64_t bit;

    for (bit = block_count; bit < BITMAP_BITS_PER_BLOCK; ++bit) {
        set_bitmap_bit(payload, bit);
    }
}

static bool bitmap_tail_valid(const uint8_t *payload, uint64_t block_count)
{
    uint64_t bit;

    for (bit = block_count; bit < BITMAP_BITS_PER_BLOCK; ++bit) {
        if (!bitmap_bit(payload, bit)) {
            return false;
        }
    }
    return true;
}

static uint64_t allocate_block(struct hfocfs *fs)
{
    uint64_t block;

    for (block = 2U; block < fs->block_count; ++block) {
        if (!bitmap_bit(fs->bitmap, block)) {
            set_bitmap_bit(fs->bitmap, block);
            return block;
        }
    }
    return 0U;
}

static void finish_block_header(uint8_t *block, uint16_t kind,
                                uint64_t generation, uint64_t block_no,
                                uint32_t payload_len)
{
    write_u32(block + HDR_MAGIC_OFFSET, HFOCFS_BLOCK_MAGIC);
    block[HDR_KIND_OFFSET] = (uint8_t)kind;
    block[HDR_KIND_OFFSET + 1U] = 0U;
    block[HDR_FLAGS_OFFSET] = 0U;
    block[HDR_FLAGS_OFFSET + 1U] = 0U;
    write_u64(block + HDR_GENERATION_OFFSET, generation);
    write_u32(block + HDR_PAYLOAD_LEN_OFFSET, payload_len);
    write_u64(block + HDR_BLOCK_NO_OFFSET, block_no);
    write_u32(block + HDR_CRC_OFFSET, header_crc(block));
}

static void begin_block(uint8_t *block, uint16_t kind, uint64_t generation,
                        uint64_t block_no, uint32_t payload_len)
{
    memset(block, 0, HFOCFS_BLOCK_SIZE);
    finish_block_header(block, kind, generation, block_no, payload_len);
}

static void seal_block(uint8_t *block)
{
    write_u32(block + HDR_CRC_OFFSET, header_crc(block));
}

static uint8_t *block_payload(uint8_t *block)
{
    return block + HFOCFS_BLOCK_HEADER_SIZE;
}

static const uint8_t *const_block_payload(const uint8_t *block)
{
    return block + HFOCFS_BLOCK_HEADER_SIZE;
}

static bool bytes_contain(const uint8_t *bytes, size_t length, uint8_t value)
{
    size_t index;

    for (index = 0U; index < length; ++index) {
        if (bytes[index] == value) {
            return true;
        }
    }
    return false;
}

static size_t align8(size_t value)
{
    return (value + (DIRENT_ALIGN - 1U)) & ~(size_t)(DIRENT_ALIGN - 1U);
}

static size_t dirent_size(size_t name_length)
{
    size_t length = align8(DIRENT_HEADER_SIZE + name_length);

    return length < DIRENT_MIN_SIZE ? DIRENT_MIN_SIZE : length;
}

static struct hfocfs_node *node_at(struct hfocfs *fs, uint32_t ino)
{
    if (ino == 0U || ino > HFOCFS_MAX_NODES) {
        return NULL;
    }
    if (!fs->nodes[ino - 1U].used) {
        return NULL;
    }
    return &fs->nodes[ino - 1U];
}

static const struct hfocfs_node *node_view(const struct hfocfs *fs,
                                           uint32_t ino)
{
    if (ino == 0U || ino > HFOCFS_MAX_NODES) {
        return NULL;
    }
    if (!fs->nodes[ino - 1U].used) {
        return NULL;
    }
    return &fs->nodes[ino - 1U];
}

static void mark_node_dirty(struct hfocfs *fs, uint32_t ino)
{
    struct hfocfs_node *node = node_at(fs, ino);

    if (node != NULL) {
        node->dirty = true;
        fs->dirty = true;
    }
}

static void recompute_node_count(struct hfocfs *fs)
{
    uint32_t count = 0U;
    uint32_t index;

    for (index = 0U; index < HFOCFS_MAX_NODES; ++index) {
        if (fs->nodes[index].used) {
            count = index + 1U;
        }
    }
    fs->node_count = count;
}

static void release_node(struct hfocfs *fs, uint32_t ino)
{
    struct hfocfs_node *node = node_at(fs, ino);

    if (node == NULL) {
        return;
    }
    memset(node, 0, sizeof(*node));
    recompute_node_count(fs);
    fs->dirty = true;
}

static bool name_equal(const struct hfocfs_node *node, const char *name,
                       size_t length)
{
    return node->name_len == length &&
           memcmp(node->name, name, length) == 0;
}

static enum hfocfs_status child_lookup(struct hfocfs *fs, uint32_t parent,
                                       const char *name, size_t name_length,
                                       uint32_t *ino_out)
{
    uint32_t index;

    for (index = 0U; index < HFOCFS_MAX_NODES; ++index) {
        const struct hfocfs_node *node = &fs->nodes[index];

        if (!node->used || node->parent != parent || node->unlinked) {
            continue;
        }
        if (name_equal(node, name, name_length)) {
            *ino_out = index + 1U;
            return HFOCFS_OK;
        }
    }
    return HFOCFS_NOT_FOUND;
}

static enum hfocfs_status component_next(const char **path, char *name,
                                         size_t *length_out)
{
    const char *cursor = *path;
    size_t length = 0U;

    while (*cursor == '/') {
        ++cursor;
    }
    if (*cursor == '\0') {
        *path = cursor;
        return HFOCFS_NOT_FOUND;
    }
    while (*cursor != '\0' && *cursor != '/') {
        if (length >= HFOCFS_MAX_NAME) {
            return HFOCFS_BAD_ARGUMENT;
        }
        name[length++] = *cursor++;
    }
    if (*cursor == '/') {
        ++cursor;
    }
    name[length] = '\0';
    *path = cursor;
    *length_out = length;
    return HFOCFS_OK;
}

static bool path_is_absolute(const char *path)
{
    return path != NULL && path[0] == '/' && strlen(path) < HFOCFS_MAX_PATH;
}

static enum hfocfs_status resolve_path(struct hfocfs *fs, const char *path,
                                       uint32_t *ino_out)
{
    uint32_t current = fs->root_ino;
    char name[HFOCFS_MAX_NAME + 1U];
    size_t length;
    enum hfocfs_status status;

    if (fs == NULL || path == NULL || ino_out == NULL) {
        return HFOCFS_NULL;
    }
    if (!fs->used) {
        return HFOCFS_NOT_MOUNTED;
    }
    if (!path_is_absolute(path)) {
        return HFOCFS_BAD_ARGUMENT;
    }
    if (path[1] == '\0') {
        *ino_out = current;
        return HFOCFS_OK;
    }
    for (;;) {
        status = component_next(&path, name, &length);
        if (status == HFOCFS_NOT_FOUND) {
            *ino_out = current;
            return HFOCFS_OK;
        }
        if (status != HFOCFS_OK) {
            return status;
        }
        if (strcmp(name, ".") == 0) {
            continue;
        }
        if (strcmp(name, "..") == 0) {
            const struct hfocfs_node *node = node_view(fs, current);

            if (node == NULL) {
                return HFOCFS_CORRUPT;
            }
            if (current != fs->root_ino) {
                current = node->parent;
            }
            continue;
        }
        status = child_lookup(fs, current, name, length, &current);
        if (status != HFOCFS_OK) {
            return status;
        }
    }
}

static enum hfocfs_status split_parent(const char *path, char *parent,
                                       size_t parent_capacity,
                                       char *leaf, size_t *leaf_length)
{
    size_t length;
    size_t slash;
    size_t index;

    if (path == NULL || parent == NULL || leaf == NULL ||
        leaf_length == NULL) {
        return HFOCFS_NULL;
    }
    if (!path_is_absolute(path)) {
        return HFOCFS_BAD_ARGUMENT;
    }
    length = strlen(path);
    if (length <= 1U || path[length - 1U] == '/') {
        return HFOCFS_BAD_ARGUMENT;
    }
    slash = length - 1U;
    while (slash > 0U && path[slash] != '/') {
        --slash;
    }
    if (slash == 0U) {
        parent[0] = '/';
        parent[1] = '\0';
    } else {
        if (slash + 1U > parent_capacity) {
            return HFOCFS_PATH_TOO_LONG;
        }
        for (index = 0U; index < slash; ++index) {
            parent[index] = path[index];
        }
        parent[slash] = '\0';
    }
    *leaf_length = length - (slash + 1U);
    if (*leaf_length == 0U || *leaf_length > HFOCFS_MAX_NAME) {
        return HFOCFS_BAD_ARGUMENT;
    }
    memcpy(leaf, path + slash + 1U, *leaf_length);
    leaf[*leaf_length] = '\0';
    if (strcmp(leaf, ".") == 0 || strcmp(leaf, "..") == 0) {
        return HFOCFS_BAD_ARGUMENT;
    }
    return HFOCFS_OK;
}

static enum hfocfs_status allocate_node(struct hfocfs *fs,
                                        enum hfocfs_node_type type,
                                        uint16_t mode, uint32_t parent,
                                        const char *name, size_t name_length,
                                        uint32_t *ino_out)
{
    uint32_t index;

    if (name_length == 0U || name_length > HFOCFS_MAX_NAME) {
        return HFOCFS_BAD_ARGUMENT;
    }
    for (index = 0U; index < HFOCFS_MAX_NODES; ++index) {
        struct hfocfs_node *node = &fs->nodes[index];

        if (node->used) {
            continue;
        }
        memset(node, 0, sizeof(*node));
        node->used = true;
        node->dirty = true;
        node->type = type;
        node->mode = mode;
        node->parent = parent;
        node->name_len = (uint16_t)name_length;
        memcpy(node->name, name, name_length);
        node->name[name_length] = '\0';
        node->link_count = 1U;
        node->mtime = fs->generation;
        if ((index + 1U) > fs->node_count) {
            fs->node_count = index + 1U;
        }
        fs->dirty = true;
        *ino_out = index + 1U;
        return HFOCFS_OK;
    }
    return HFOCFS_NO_MEMORY;
}

static enum hfocfs_status read_node_block(struct hfocfs *fs,
                                          const struct hfocfs_node *node,
                                          uint32_t index, uint16_t kind,
                                          uint8_t *destination)
{
    uint64_t block;
    enum hfocfs_status status;

    if (index >= HFOCFS_DIRECT_BLOCKS) {
        return HFOCFS_BAD_ARGUMENT;
    }
    block = node->blocks[index];
    if (block == 0U) {
        memset(destination, 0, HFOCFS_BLOCK_SIZE);
        return HFOCFS_OK;
    }
    if (block >= fs->block_count) {
        return HFOCFS_CORRUPT;
    }
    status = read_block(fs->device, block, destination);
    if (status != HFOCFS_OK) {
        return status;
    }
    if (!header_valid_any(destination, kind, block)) {
        return HFOCFS_CORRUPT;
    }
    return HFOCFS_OK;
}

/* ------------------------------------------------------------------------- */
/* Format                                                                     */

static void build_root_inode(uint8_t *payload)
{
    uint8_t *inode = payload;

    memset(inode, 0, INODE_SIZE);
    write_u32(inode + INODE_INO_OFFSET, 1U);
    inode[INODE_TYPE_OFFSET] = (uint8_t)HFOCFS_NODE_DIRECTORY;
    inode[INODE_TYPE_OFFSET + 1U] = 0U;
    inode[INODE_MODE_OFFSET] = 0755U & 0xffU;
    inode[INODE_MODE_OFFSET + 1U] = 0U;
    write_u32(inode + INODE_SIZE_OFFSET, 0U);
    write_u32(inode + INODE_LINK_OFFSET, 1U);
    write_u64(inode + INODE_MTIME_OFFSET, 0U);
    write_u32(inode + INODE_CRC_OFFSET,
              crc32_bytes(inode, INODE_CRC_LENGTH));
}

size_t hfocfs_state_size(void)
{
    return sizeof(struct hfocfs);
}

size_t hfocfs_state_alignment(void)
{
    return _Alignof(struct hfocfs);
}

enum hfocfs_status hfocfs_format(struct block_device *device,
                                 const struct hfocfs_options *options,
                                 void *state, size_t state_size)
{
    struct hfocfs *scratch;
    uint8_t *superblock;
    uint8_t *commit;
    uint8_t *inode_block;
    uint8_t *bitmap;
    uint8_t digest[HFOCFS_DIGEST_SIZE];
    uint8_t uuid[HFOCFS_UUID_SIZE];
    char label[SB_LABEL_BYTES];
    uint64_t image_bytes;
    uint64_t image_blocks;
    enum hfocfs_status status;

    if (device == NULL || state == NULL) {
        return HFOCFS_NULL;
    }
    if (state_size < sizeof(struct hfocfs) ||
        ((uintptr_t)state % _Alignof(struct hfocfs)) != 0U) {
        return HFOCFS_BAD_ARGUMENT;
    }
    if (block_device_validate(device) != BLOCK_DEVICE_OK) {
        return HFOCFS_BAD_ARGUMENT;
    }
    if (device->block_size != 512U) {
        return HFOCFS_UNSUPPORTED;
    }
    image_bytes = device->block_count * (uint64_t)device->block_size;
    if (image_bytes < HFOCFS_IMAGE_MIN_BYTES) {
        return HFOCFS_TOO_SMALL;
    }
    if (image_bytes > HFOCFS_IMAGE_MAX_BYTES ||
        (image_bytes % HFOCFS_BLOCK_SIZE) != 0U) {
        return HFOCFS_TOO_LARGE;
    }
    image_blocks = image_bytes / HFOCFS_BLOCK_SIZE;
    if (image_blocks > HFOCFS_BLOCK_COUNT_MAX) {
        return HFOCFS_TOO_LARGE;
    }

    memset(label, 0, sizeof(label));
    if (options != NULL && options->label != NULL) {
        size_t length = strlen(options->label);
        if (length > sizeof(label)) {
            return HFOCFS_BAD_ARGUMENT;
        }
        memcpy(label, options->label, length);
    }
    if (options != NULL && options->uuid != NULL) {
        memcpy(uuid, options->uuid, sizeof(uuid));
    } else {
        struct sha256 context;
        uint8_t material[HFOCFS_MAGIC_SIZE + SB_LABEL_BYTES];
        memcpy(material, HFOCFS_MAGIC, HFOCFS_MAGIC_SIZE);
        memcpy(material + HFOCFS_MAGIC_SIZE, label, SB_LABEL_BYTES);
        if (sha256_init(&context) != SHA256_OK ||
            sha256_update(&context, material, sizeof(material)) != SHA256_OK ||
            sha256_final(&context, digest) != SHA256_OK) {
            return HFOCFS_BAD_ARGUMENT;
        }
        memcpy(uuid, digest, sizeof(uuid));
    }

    scratch = (struct hfocfs *)state;
    memset(scratch, 0, sizeof(*scratch));
    superblock = scratch->block;
    commit = scratch->aux;
    inode_block = scratch->block2;
    bitmap = scratch->block; /* reused after the inode block is written */

    begin_block(inode_block, HFOCFS_BLOCK_INODE_TABLE, 1U, 3U,
                HFOCFS_BLOCK_PAYLOAD_SIZE);
    build_root_inode(block_payload(inode_block));
    seal_block(inode_block);

    begin_block(bitmap, HFOCFS_BLOCK_BITMAP, 1U, 4U,
                HFOCFS_BLOCK_PAYLOAD_SIZE);
    {
        uint8_t *payload = block_payload(bitmap);

        set_bitmap_bit(payload, 0U);
        set_bitmap_bit(payload, 1U);
        set_bitmap_bit(payload, 2U);
        set_bitmap_bit(payload, 3U);
        set_bitmap_bit(payload, 4U);
        finalise_bitmap_tail(payload, image_blocks);
    }
    seal_block(bitmap);

    begin_block(commit, HFOCFS_BLOCK_COMMIT, 1U, 2U,
                HFOCFS_BLOCK_PAYLOAD_SIZE);
    {
        uint8_t *record = block_payload(commit);

        write_u64(record + COMMIT_GENERATION_OFFSET, 1U);
        write_u32(record + COMMIT_INODE_COUNT_OFFSET, 1U);
        write_u32(record + COMMIT_BITMAP_COUNT_OFFSET, 1U);
        write_u32(record + COMMIT_ROOT_OFFSET, 1U);
        write_u32(record + COMMIT_NODE_COUNT_OFFSET, 1U);
        write_u64(record + COMMIT_INODE_BLOCKS_OFFSET, 3U);
        write_u64(record + COMMIT_BITMAP_BLOCKS_OFFSET, 4U);
        write_u32(record + COMMIT_CRC_OFFSET,
                  crc32_bytes(record, COMMIT_CRC_OFFSET));
    }
    seal_block(commit);
    if (sha256_compute(commit, HFOCFS_BLOCK_SIZE, digest) != SHA256_OK) {
        memset(scratch, 0, sizeof(*scratch));
        return HFOCFS_BAD_ARGUMENT;
    }

    /* Blocks 3, 4, 2 first; a torn format has no valid superblock. */
    status = write_block(device, 3U, inode_block);
    if (status == HFOCFS_OK) {
        status = write_block(device, 4U, bitmap);
    }
    if (status == HFOCFS_OK) {
        status = write_block(device, 2U, commit);
    }
    if (status == HFOCFS_OK) {
        status = flush_device(device);
    }

    /* `superblock` aliases the bitmap scratch, so build it after that write. */
    memset(superblock, 0, HFOCFS_BLOCK_SIZE);
    memcpy(superblock + SB_MAGIC_OFFSET, HFOCFS_MAGIC, HFOCFS_MAGIC_SIZE);
    write_u32(superblock + SB_VERSION_OFFSET, HFOCFS_FORMAT_VERSION);
    write_u32(superblock + SB_BLOCK_SIZE_OFFSET, HFOCFS_BLOCK_SIZE);
    write_u64(superblock + SB_BLOCK_COUNT_OFFSET, image_blocks);
    write_u64(superblock + SB_GENERATION_OFFSET, 1U);
    write_u64(superblock + SB_COMMIT_BLOCK_OFFSET, 2U);
    write_u32(superblock + SB_COMMIT_LENGTH_OFFSET, HFOCFS_BLOCK_SIZE);
    write_u32(superblock + SB_FLAGS_OFFSET, 1U);
    memcpy(superblock + SB_SHA_OFFSET, digest, sizeof(digest));
    memcpy(superblock + SB_UUID_OFFSET, uuid, sizeof(uuid));
    memcpy(superblock + SB_LABEL_OFFSET, label, sizeof(label));
    write_u32(superblock + SB_CRC_OFFSET,
              crc32_bytes(superblock, SB_CRC_LENGTH));

    if (status == HFOCFS_OK) {
        status = write_block(device, 1U, superblock);
    }
    if (status == HFOCFS_OK) {
        status = flush_device(device);
    }
    if (status == HFOCFS_OK) {
        status = write_block(device, 0U, superblock);
    }
    if (status == HFOCFS_OK) {
        status = flush_device(device);
    }
    memset(scratch, 0, sizeof(*scratch));
    return status;
}

/* ------------------------------------------------------------------------- */
/* Mount validation                                                           */

struct slot_view {
    bool valid;
    bool io_error;
    uint64_t generation;
    uint64_t commit_block;
    uint32_t flags;
    uint8_t sha[HFOCFS_DIGEST_SIZE];
    uint8_t uuid[HFOCFS_UUID_SIZE];
    char label[HFOCFS_LABEL_SIZE + 1U];
};

static void parse_slot(const uint8_t *slot, uint64_t image_blocks,
                       struct slot_view *view)
{
    uint32_t saved_crc;

    memset(view, 0, sizeof(*view));
    if (memcmp(slot + SB_MAGIC_OFFSET, HFOCFS_MAGIC, HFOCFS_MAGIC_SIZE) != 0) {
        return;
    }
    if (read_u32(slot + SB_VERSION_OFFSET) != HFOCFS_FORMAT_VERSION ||
        read_u32(slot + SB_BLOCK_SIZE_OFFSET) != HFOCFS_BLOCK_SIZE ||
        read_u64(slot + SB_BLOCK_COUNT_OFFSET) != image_blocks ||
        read_u32(slot + SB_COMMIT_LENGTH_OFFSET) != HFOCFS_BLOCK_SIZE) {
        return;
    }
    if (read_u64(slot + SB_COMMIT_BLOCK_OFFSET) < 2U ||
        read_u64(slot + SB_COMMIT_BLOCK_OFFSET) >= image_blocks) {
        return;
    }
    saved_crc = read_u32(slot + SB_CRC_OFFSET);
    if (crc32_bytes(slot, SB_CRC_LENGTH) != saved_crc) {
        return;
    }
    view->valid = true;
    view->generation = read_u64(slot + SB_GENERATION_OFFSET);
    view->commit_block = read_u64(slot + SB_COMMIT_BLOCK_OFFSET);
    view->flags = read_u32(slot + SB_FLAGS_OFFSET);
    memcpy(view->sha, slot + SB_SHA_OFFSET, sizeof(view->sha));
    memcpy(view->uuid, slot + SB_UUID_OFFSET, sizeof(view->uuid));
    memcpy(view->label, slot + SB_LABEL_OFFSET, SB_LABEL_BYTES);
    view->label[SB_LABEL_BYTES] = '\0';
}

static enum hfocfs_status load_inode(struct hfocfs *fs, const uint8_t *inode,
                                     uint32_t expected_ino)
{
    struct hfocfs_node *node;
    uint32_t ino = read_u32(inode + INODE_INO_OFFSET);
    uint16_t type;
    uint32_t size;
    uint32_t link_count;
    uint32_t index;

    if (ino == 0U) {
        return HFOCFS_OK;
    }
    if (ino != expected_ino || ino > fs->node_count) {
        return HFOCFS_CORRUPT;
    }
    if (read_u32(inode + INODE_CRC_OFFSET) !=
        crc32_bytes(inode, INODE_CRC_LENGTH)) {
        return HFOCFS_CORRUPT;
    }
    type = (uint16_t)inode[INODE_TYPE_OFFSET];
    size = read_u32(inode + INODE_SIZE_OFFSET);
    link_count = read_u32(inode + INODE_LINK_OFFSET);
    if (inode[INODE_TYPE_OFFSET + 1U] != 0U ||
        (type != HFOCFS_NODE_FILE && type != HFOCFS_NODE_DIRECTORY) ||
        (type == HFOCFS_NODE_FILE && size > HFOCFS_MAX_FILE_BYTES) ||
        (type == HFOCFS_NODE_DIRECTORY && size != 0U) ||
        link_count > HFOCFS_MAX_NODES) {
        return HFOCFS_CORRUPT;
    }
    node = &fs->nodes[ino - 1U];
    memset(node, 0, sizeof(*node));
    node->used = true;
    node->type = (enum hfocfs_node_type)type;
    node->mode = (uint16_t)inode[INODE_MODE_OFFSET] |
                 (uint16_t)((uint16_t)inode[INODE_MODE_OFFSET + 1U] << 8);
    node->size = size;
    node->link_count = link_count;
    node->mtime = read_u64(inode + INODE_MTIME_OFFSET);
    for (index = 0U; index < HFOCFS_DIRECT_BLOCKS; ++index) {
        uint64_t block = read_u64(inode + INODE_BLOCKS_OFFSET + index * 8U);

        if (block >= fs->block_count && block != 0U) {
            return HFOCFS_CORRUPT;
        }
        node->blocks[index] = block;
    }
    return HFOCFS_OK;
}

static enum hfocfs_status parse_directory(struct hfocfs *fs, uint32_t dir_ino,
                                          uint8_t *block, uint64_t block_no)
{
    const uint8_t *payload;
    size_t position = 0U;

    if (node_at(fs, dir_ino) == NULL) {
        return HFOCFS_CORRUPT;
    }
    if (!header_valid_any(block, HFOCFS_BLOCK_DIRECTORY, block_no)) {
        return HFOCFS_CORRUPT;
    }
    set_bitmap_bit(fs->base_bitmap, block_no);
    payload = const_block_payload(block);
    while (position + DIRENT_HEADER_SIZE <= HFOCFS_BLOCK_PAYLOAD_SIZE) {
        uint32_t ino = read_u32(payload + position);
        uint16_t name_length = (uint16_t)read_u32(payload + position + 4U);
        uint16_t rec_length = (uint16_t)read_u32(payload + position + 6U);
        struct hfocfs_node *child;

        if (ino == 0U && name_length == 0U && rec_length == 0U) {
            break; /* zero padding at the tail */
        }
        if (rec_length < DIRENT_MIN_SIZE ||
            (rec_length % DIRENT_ALIGN) != 0U ||
            rec_length > HFOCFS_BLOCK_PAYLOAD_SIZE - position ||
            name_length == 0U || name_length > HFOCFS_MAX_NAME ||
            DIRENT_HEADER_SIZE + name_length > rec_length) {
            return HFOCFS_CORRUPT;
        }
        if (ino == 0U || ino > fs->node_count || ino == fs->root_ino ||
            ino == dir_ino) {
            return HFOCFS_CORRUPT;
        }
        if ((name_length == 1U &&
             payload[position + DIRENT_HEADER_SIZE] == '.') ||
            (name_length == 2U &&
             payload[position + DIRENT_HEADER_SIZE] == '.' &&
             payload[position + DIRENT_HEADER_SIZE + 1U] == '.')) {
            return HFOCFS_CORRUPT;
        }
        if (bytes_contain(payload + position + DIRENT_HEADER_SIZE,
                          name_length, '/') ||
            bytes_contain(payload + position + DIRENT_HEADER_SIZE,
                          name_length, '\0')) {
            return HFOCFS_CORRUPT;
        }
        child = &fs->nodes[ino - 1U];
        if (!child->used || child->parent != 0U) {
            return HFOCFS_CORRUPT;
        }
        {
            uint32_t sibling;
            const char *name = (const char *)(payload + position +
                                              DIRENT_HEADER_SIZE);

            for (sibling = 0U; sibling < HFOCFS_MAX_NODES; ++sibling) {
                const struct hfocfs_node *other = &fs->nodes[sibling];

                if (other == child || !other->used ||
                    other->parent != dir_ino) {
                    continue;
                }
                if (other->name_len == name_length &&
                    memcmp(other->name, name, name_length) == 0) {
                    return HFOCFS_CORRUPT;
                }
            }
            memcpy(child->name, name, name_length);
            child->name[name_length] = '\0';
            child->name_len = name_length;
        }
        child->parent = dir_ino;
        position += rec_length;
    }
    return HFOCFS_OK;
}

static bool node_reachable_from_root(struct hfocfs *fs, uint32_t ino)
{
    uint32_t cursor = ino;
    uint32_t depth;

    for (depth = 0U; depth <= HFOCFS_MAX_NODES; ++depth) {
        const struct hfocfs_node *node;

        if (cursor == fs->root_ino) {
            return true;
        }
        node = node_view(fs, cursor);
        if (node == NULL || node->parent == 0U ||
            node->parent == cursor) {
            return false;
        }
        cursor = node->parent;
    }
    return false;
}

static enum hfocfs_status validate_loaded_tree(struct hfocfs *fs,
                                               const uint8_t *inode_blocks,
                                               uint32_t inode_block_count,
                                               uint64_t commit_block,
                                               uint64_t bitmap_block,
                                               const uint8_t *bitmap_payload)
{
    uint8_t seen[BITMAP_BYTES_MAX];
    uint32_t ino;
    uint32_t block_index;
    uint64_t bit;

    if (!bitmap_tail_valid(bitmap_payload, fs->block_count)) {
        return HFOCFS_CORRUPT;
    }
    memset(fs->base_bitmap, 0, sizeof(fs->base_bitmap));
    set_bitmap_bit(fs->base_bitmap, 0U);
    set_bitmap_bit(fs->base_bitmap, 1U);
    set_bitmap_bit(fs->base_bitmap, commit_block);
    set_bitmap_bit(fs->base_bitmap, bitmap_block);
    for (block_index = 0U; block_index < inode_block_count; ++block_index) {
        set_bitmap_bit(fs->base_bitmap, inode_blocks[block_index]);
    }
    if (!bitmap_bit(bitmap_payload, commit_block) ||
        !bitmap_bit(bitmap_payload, bitmap_block) ||
        !bitmap_bit(bitmap_payload, 0U) || !bitmap_bit(bitmap_payload, 1U)) {
        return HFOCFS_CORRUPT;
    }
    for (block_index = 0U; block_index < inode_block_count; ++block_index) {
        if (!bitmap_bit(bitmap_payload, inode_blocks[block_index])) {
            return HFOCFS_CORRUPT;
        }
    }

    memset(seen, 0, sizeof(seen));
    for (block_index = 0U; block_index < inode_block_count; ++block_index) {
        set_bitmap_bit(seen, inode_blocks[block_index]);
    }
    set_bitmap_bit(seen, commit_block);
    set_bitmap_bit(seen, bitmap_block);

    /*
     * Every directory entry points at one child; this rejects shared nodes and
     * leaves the parent chain ready for the cycle check below.
     */
    for (ino = 1U; ino <= fs->node_count; ++ino) {
        struct hfocfs_node *node = node_at(fs, ino);
        uint32_t index;

        if (node == NULL || node->type != HFOCFS_NODE_DIRECTORY) {
            continue;
        }
        for (index = 0U; index < HFOCFS_DIRECT_BLOCKS; ++index) {
            uint64_t block_no = node->blocks[index];
            enum hfocfs_status status;

            if (block_no == 0U) {
                continue;
            }
            if (block_no < 2U || bitmap_bit(seen, block_no)) {
                return HFOCFS_CORRUPT;
            }
            set_bitmap_bit(seen, block_no);
            status = read_block(fs->device, block_no, fs->block);
            if (status != HFOCFS_OK) {
                return status;
            }
            status = parse_directory(fs, ino, fs->block, block_no);
            if (status != HFOCFS_OK) {
                return status;
            }
        }
    }

    /* Root has no stored parent; every other used node must name one. */
    {
        struct hfocfs_node *root = node_at(fs, fs->root_ino);

        if (root == NULL || root->type != HFOCFS_NODE_DIRECTORY ||
            root->parent != 0U) {
            return HFOCFS_CORRUPT;
        }
        root->name[0] = '\0';
        root->name_len = 0U;
    }
    for (ino = 1U; ino <= fs->node_count; ++ino) {
        struct hfocfs_node *node = node_at(fs, ino);

        if (node == NULL) {
            continue;
        }
        if (ino != fs->root_ino && node->parent == 0U) {
            if (node->link_count == 0U) {
                /* An unlinked-open node whose owner is gone after reboot. */
                memset(node, 0, sizeof(*node));
                continue;
            }
            return HFOCFS_CORRUPT;
        }
        if (!node_reachable_from_root(fs, ino)) {
            return HFOCFS_CORRUPT;
        }
    }
    recompute_node_count(fs);

    for (ino = 1U; ino <= fs->node_count; ++ino) {
        const struct hfocfs_node *node = node_view(fs, ino);
        uint16_t kind;
        uint32_t index;

        if (node == NULL) {
            continue;
        }
        kind = node->type == HFOCFS_NODE_DIRECTORY
                   ? HFOCFS_BLOCK_DIRECTORY : HFOCFS_BLOCK_FILE_DATA;
        for (index = 0U; index < HFOCFS_DIRECT_BLOCKS; ++index) {
            uint64_t block_no = node->blocks[index];

            if (block_no == 0U) {
                continue;
            }
            if (block_no < 2U || block_no >= fs->block_count) {
                return HFOCFS_CORRUPT;
            }
            if (bitmap_bit(seen, block_no)) {
                if (node->type == HFOCFS_NODE_DIRECTORY) {
                    continue; /* already parsed and marked above */
                }
                return HFOCFS_CORRUPT;
            }
            set_bitmap_bit(seen, block_no);
            set_bitmap_bit(fs->base_bitmap, block_no);
            if (!bitmap_bit(bitmap_payload, block_no)) {
                return HFOCFS_CORRUPT;
            }
            {
                enum hfocfs_status status =
                    read_block(fs->device, block_no, fs->block);

                if (status != HFOCFS_OK) {
                    return status;
                }
                if (!header_valid_any(fs->block, kind, block_no)) {
                    return HFOCFS_CORRUPT;
                }
            }
        }
    }

    for (bit = 0U; bit < fs->block_count; ++bit) {
        if (bitmap_bit(fs->base_bitmap, bit) &&
            !bitmap_bit(bitmap_payload, bit)) {
            return HFOCFS_CORRUPT;
        }
    }
    return HFOCFS_OK;
}

static enum hfocfs_status load_state(struct hfocfs *fs)
{
    struct slot_view slots[2];
    uint8_t raw[SB_SLOT_SIZE];
    uint64_t inode_block_numbers[HFOCFS_INODE_BLOCKS_MAX];
    uint32_t inode_block_count;
    uint32_t stored_node_count;
    uint64_t bitmap_block;
    uint64_t commit_block;
    uint64_t generation;
    uint64_t image_bytes;
    uint64_t image_blocks;
    uint8_t digest[HFOCFS_DIGEST_SIZE];
    const uint8_t *record;
    const uint8_t *inode;
    enum hfocfs_status status;
    int chosen;
    uint32_t index;
    uint32_t ino;

    image_bytes = fs->device->block_count * (uint64_t)fs->device->block_size;
    image_blocks = image_bytes / HFOCFS_BLOCK_SIZE;
    memset(&slots, 0, sizeof(slots));
    status = read_bytes(fs->device, 0U, raw, SB_SLOT_SIZE);
    if (status == HFOCFS_OK) {
        parse_slot(raw, image_blocks, &slots[0]);
    } else {
        slots[0].io_error = true;
    }
    status = read_bytes(fs->device, 1U, raw, SB_SLOT_SIZE);
    if (status == HFOCFS_OK) {
        parse_slot(raw, image_blocks, &slots[1]);
    } else {
        slots[1].io_error = true;
    }
    if (!slots[0].valid && !slots[1].valid) {
        return (slots[0].io_error || slots[1].io_error) ? HFOCFS_IO
                                                        : HFOCFS_CORRUPT;
    }
    if (slots[0].valid && slots[1].valid &&
        slots[0].generation == slots[1].generation) {
        if (slots[0].commit_block != slots[1].commit_block ||
            memcmp(slots[0].sha, slots[1].sha, HFOCFS_DIGEST_SIZE) != 0) {
            return HFOCFS_CORRUPT;
        }
    }
    if (slots[0].valid && slots[1].valid) {
        chosen = slots[0].generation >= slots[1].generation ? 0 : 1;
    } else {
        chosen = slots[0].valid ? 0 : 1;
    }

    generation = slots[chosen].generation;
    commit_block = slots[chosen].commit_block;
    status = read_block(fs->device, commit_block, fs->block);
    if (status != HFOCFS_OK) {
        return status;
    }
    if (!header_valid_exact(fs->block, HFOCFS_BLOCK_COMMIT, generation,
                            commit_block) ||
        sha256_compute(fs->block, HFOCFS_BLOCK_SIZE, digest) != SHA256_OK ||
        memcmp(digest, slots[chosen].sha, sizeof(digest)) != 0) {
        return HFOCFS_CORRUPT;
    }
    record = const_block_payload(fs->block);
    if (read_u64(record + COMMIT_GENERATION_OFFSET) != generation ||
        read_u32(record + COMMIT_CRC_OFFSET) !=
            crc32_bytes(record, COMMIT_CRC_OFFSET) ||
        read_u32(record + COMMIT_ROOT_OFFSET) != 1U) {
        return HFOCFS_CORRUPT;
    }
    inode_block_count = read_u32(record + COMMIT_INODE_COUNT_OFFSET);
    stored_node_count = read_u32(record + COMMIT_NODE_COUNT_OFFSET);
    bitmap_block = read_u64(record + COMMIT_BITMAP_BLOCKS_OFFSET);
    if (inode_block_count == 0U ||
        inode_block_count > HFOCFS_INODE_BLOCKS_MAX ||
        read_u32(record + COMMIT_BITMAP_COUNT_OFFSET) != 1U ||
        stored_node_count == 0U ||
        stored_node_count > HFOCFS_MAX_NODES ||
        inode_block_count !=
            (stored_node_count + HFOCFS_INODES_PER_BLOCK - 1U) /
                HFOCFS_INODES_PER_BLOCK) {
        return HFOCFS_CORRUPT;
    }
    for (index = 0U; index < inode_block_count; ++index) {
        inode_block_numbers[index] =
            read_u64(record + COMMIT_INODE_BLOCKS_OFFSET + index * 8U);
    }

    memset(fs->nodes, 0, sizeof(fs->nodes));
    fs->node_count = stored_node_count;
    fs->root_ino = 1U;
    for (index = 0U; index < inode_block_count; ++index) {
        uint64_t block_no = inode_block_numbers[index];

        if (block_no < 2U || block_no >= image_blocks) {
            return HFOCFS_CORRUPT;
        }
        for (ino = 0U; ino < index; ++ino) {
            if (inode_block_numbers[ino] == block_no) {
                return HFOCFS_CORRUPT;
            }
        }
        status = read_block(fs->device, block_no, fs->block);
        if (status != HFOCFS_OK) {
            return status;
        }
        if (!header_valid_exact(fs->block, HFOCFS_BLOCK_INODE_TABLE,
                                generation, block_no)) {
            return HFOCFS_CORRUPT;
        }
        inode = const_block_payload(fs->block);
        for (ino = 0U; ino < HFOCFS_INODES_PER_BLOCK; ++ino) {
            uint32_t expected = index * HFOCFS_INODES_PER_BLOCK + ino + 1U;

            if (expected > fs->node_count) {
                break;
            }
            status = load_inode(fs, inode + ino * INODE_SIZE, expected);
            if (status != HFOCFS_OK) {
                return status;
            }
        }
    }
    if (node_at(fs, fs->root_ino) == NULL) {
        return HFOCFS_CORRUPT;
    }

    if (bitmap_block < 2U || bitmap_block >= image_blocks) {
        return HFOCFS_CORRUPT;
    }
    status = read_block(fs->device, bitmap_block, fs->aux);
    if (status != HFOCFS_OK) {
        return status;
    }
    if (!header_valid_exact(fs->aux, HFOCFS_BLOCK_BITMAP, generation,
                            bitmap_block)) {
        return HFOCFS_CORRUPT;
    }
    memcpy(fs->bitmap, block_payload(fs->aux), sizeof(fs->bitmap));
    status = validate_loaded_tree(fs, (const uint8_t *)inode_block_numbers,
                                  inode_block_count, commit_block,
                                  bitmap_block, fs->bitmap);
    if (status != HFOCFS_OK) {
        return status;
    }

    fs->generation = generation;
    fs->commit_block = commit_block;
    fs->clean = (slots[chosen].flags & 1U) != 0U;
    memcpy(fs->uuid, slots[chosen].uuid, sizeof(fs->uuid));
    memcpy(fs->label, slots[chosen].label, sizeof(fs->label));
    fs->dirty = false;
    return HFOCFS_OK;
}

/* ------------------------------------------------------------------------- */
/* Public mount / info / tree operations                                      */

static enum hfocfs_status image_geometry(struct block_device *device,
                                         uint64_t *blocks_out)
{
    uint64_t image_bytes;

    if (device == NULL || blocks_out == NULL) {
        return HFOCFS_NULL;
    }
    if (block_device_validate(device) != BLOCK_DEVICE_OK ||
        device->block_size != 512U) {
        return HFOCFS_BAD_ARGUMENT;
    }
    image_bytes = device->block_count * (uint64_t)device->block_size;
    if (image_bytes < HFOCFS_IMAGE_MIN_BYTES) {
        return HFOCFS_TOO_SMALL;
    }
    if (image_bytes > HFOCFS_IMAGE_MAX_BYTES ||
        (image_bytes % HFOCFS_BLOCK_SIZE) != 0U ||
        (image_bytes / HFOCFS_BLOCK_SIZE) > HFOCFS_BLOCK_COUNT_MAX) {
        return HFOCFS_TOO_LARGE;
    }
    *blocks_out = image_bytes / HFOCFS_BLOCK_SIZE;
    return HFOCFS_OK;
}

enum hfocfs_status hfocfs_mount(struct block_device *device,
                                void *state, size_t state_size,
                                struct hfocfs **out)
{
    struct hfocfs *fs;
    enum hfocfs_status status;
    uint64_t blocks;

    if (device == NULL || state == NULL || out == NULL) {
        return HFOCFS_NULL;
    }
    *out = NULL;
    if (state_size < sizeof(struct hfocfs) ||
        ((uintptr_t)state % _Alignof(struct hfocfs)) != 0U) {
        return HFOCFS_BAD_ARGUMENT;
    }
    status = image_geometry(device, &blocks);
    if (status != HFOCFS_OK) {
        return status;
    }
    fs = (struct hfocfs *)state;
    memset(fs, 0, sizeof(*fs));
    fs->device = device;
    fs->block_count = blocks;
    fs->root_ino = 1U;
    status = load_state(fs);
    if (status != HFOCFS_OK) {
        memset(fs, 0, sizeof(*fs));
        return status;
    }
    fs->used = true;
    fs->read_only = false;
    *out = fs;
    return HFOCFS_OK;
}

enum hfocfs_status hfocfs_info(const struct hfocfs *fs,
                               struct hfocfs_info *out)
{
    if (fs == NULL || out == NULL) {
        return HFOCFS_NULL;
    }
    if (!fs->used) {
        return HFOCFS_NOT_MOUNTED;
    }
    memset(out, 0, sizeof(*out));
    out->generation = fs->generation;
    out->block_count = fs->block_count;
    out->node_count = fs->node_count;
    out->root_ino = fs->root_ino;
    out->clean = fs->clean;
    memcpy(out->uuid, fs->uuid, sizeof(out->uuid));
    memcpy(out->label, fs->label, sizeof(fs->label));
    return HFOCFS_OK;
}

enum hfocfs_status hfocfs_lookup(struct hfocfs *fs, const char *path,
                                 uint32_t *ino_out)
{
    return resolve_path(fs, path, ino_out);
}

enum hfocfs_status hfocfs_stat_path(struct hfocfs *fs, const char *path,
                                    struct hfocfs_stat *stat_out)
{
    uint32_t ino;
    const struct hfocfs_node *node;
    enum hfocfs_status status;

    if (fs == NULL || path == NULL || stat_out == NULL) {
        return HFOCFS_NULL;
    }
    status = resolve_path(fs, path, &ino);
    if (status != HFOCFS_OK) {
        return status;
    }
    node = node_view(fs, ino);
    if (node == NULL) {
        return HFOCFS_CORRUPT;
    }
    memset(stat_out, 0, sizeof(*stat_out));
    stat_out->ino = ino;
    stat_out->type = node->type;
    stat_out->mode = node->mode;
    stat_out->size = node->size;
    stat_out->link_count = node->link_count;
    stat_out->open_count = node->open_count;
    stat_out->mtime = node->mtime;
    return HFOCFS_OK;
}

enum hfocfs_status hfocfs_create(struct hfocfs *fs, const char *path,
                                 enum hfocfs_node_type type, uint16_t mode,
                                 uint32_t *ino_out)
{
    char parent_path[HFOCFS_MAX_PATH];
    char leaf[HFOCFS_MAX_NAME + 1U];
    size_t leaf_length;
    uint32_t parent_ino;
    uint32_t existing;
    const struct hfocfs_node *parent;
    enum hfocfs_status status;

    if (fs == NULL || path == NULL || ino_out == NULL) {
        return HFOCFS_NULL;
    }
    if (type != HFOCFS_NODE_FILE && type != HFOCFS_NODE_DIRECTORY) {
        return HFOCFS_BAD_ARGUMENT;
    }
    status = split_parent(path, parent_path, sizeof(parent_path), leaf,
                          &leaf_length);
    if (status != HFOCFS_OK) {
        return status;
    }
    status = resolve_path(fs, parent_path, &parent_ino);
    if (status != HFOCFS_OK) {
        return status;
    }
    parent = node_view(fs, parent_ino);
    if (parent == NULL) {
        return HFOCFS_CORRUPT;
    }
    if (parent->type != HFOCFS_NODE_DIRECTORY) {
        return HFOCFS_NOT_DIRECTORY;
    }
    if (child_lookup(fs, parent_ino, leaf, leaf_length, &existing) ==
        HFOCFS_OK) {
        return HFOCFS_EXISTS;
    }
    if (mode == 0U) {
        mode = type == HFOCFS_NODE_DIRECTORY ? 0755U : 0644U;
    }
    status = allocate_node(fs, type, mode, parent_ino, leaf, leaf_length,
                           ino_out);
    if (status == HFOCFS_OK) {
        mark_node_dirty(fs, parent_ino);
    }
    return status;
}

enum hfocfs_status hfocfs_readdir(struct hfocfs *fs, uint32_t dir_ino,
                                  uint32_t index, char *name,
                                  size_t name_capacity, uint32_t *ino_out,
                                  enum hfocfs_node_type *type_out)
{
    const struct hfocfs_node *dir;
    uint32_t found = 0U;
    uint32_t cursor;

    if (fs == NULL) {
        return HFOCFS_NULL;
    }
    if (!fs->used) {
        return HFOCFS_NOT_MOUNTED;
    }
    dir = node_view(fs, dir_ino);
    if (dir == NULL) {
        return HFOCFS_NOT_FOUND;
    }
    if (dir->type != HFOCFS_NODE_DIRECTORY) {
        return HFOCFS_NOT_DIRECTORY;
    }
    for (cursor = 0U; cursor < HFOCFS_MAX_NODES; ++cursor) {
        const struct hfocfs_node *node = &fs->nodes[cursor];

        if (!node->used || node->parent != dir_ino || node->unlinked) {
            continue;
        }
        if (found++ != index) {
            continue;
        }
        if (name == NULL || ino_out == NULL || type_out == NULL) {
            return HFOCFS_NULL;
        }
        if (name_capacity <= node->name_len) {
            return HFOCFS_NAME_TOO_LONG;
        }
        memcpy(name, node->name, node->name_len);
        name[node->name_len] = '\0';
        *ino_out = cursor + 1U;
        *type_out = node->type;
        return HFOCFS_OK;
    }
    return HFOCFS_NOT_FOUND;
}

enum hfocfs_status hfocfs_read(struct hfocfs *fs, uint32_t ino,
                               uint32_t offset, void *buffer, size_t size,
                               size_t *done_out)
{
    const struct hfocfs_node *node;
    size_t done = 0U;
    uint8_t *destination = (uint8_t *)buffer;

    if (fs == NULL || buffer == NULL || done_out == NULL) {
        return HFOCFS_NULL;
    }
    if (!fs->used) {
        return HFOCFS_NOT_MOUNTED;
    }
    node = node_view(fs, ino);
    if (node == NULL) {
        return HFOCFS_NOT_FOUND;
    }
    if (node->type != HFOCFS_NODE_FILE) {
        return HFOCFS_IS_DIRECTORY;
    }
    *done_out = 0U;
    if (offset >= node->size || size == 0U) {
        return HFOCFS_OK;
    }
    if (size > node->size - offset) {
        size = node->size - offset;
    }
    while (done < size) {
        uint32_t position = offset + (uint32_t)done;
        uint32_t block_index = position / HFOCFS_BLOCK_PAYLOAD_SIZE;
        uint32_t within = position % HFOCFS_BLOCK_PAYLOAD_SIZE;
        size_t chunk = HFOCFS_BLOCK_PAYLOAD_SIZE - within;
        enum hfocfs_status status;

        if (chunk > size - done) {
            chunk = size - done;
        }
        status = read_node_block(fs, node, block_index,
                                 HFOCFS_BLOCK_FILE_DATA, fs->block);
        if (status != HFOCFS_OK) {
            return status;
        }
        memcpy(destination + done,
               block_payload(fs->block) + within, chunk);
        done += chunk;
    }
    *done_out = done;
    return HFOCFS_OK;
}

static enum hfocfs_status write_file_chunk(struct hfocfs *fs,
                                           struct hfocfs_node *node,
                                           uint32_t block_index,
                                           uint32_t within,
                                           const uint8_t *source,
                                           size_t length)
{
    uint64_t block = node->blocks[block_index];
    uint64_t new_block;
    enum hfocfs_status status;

    if (block != 0U) {
        status = read_block(fs->device, block, fs->block);
        if (status != HFOCFS_OK) {
            return status;
        }
        if (!header_valid_any(fs->block, HFOCFS_BLOCK_FILE_DATA, block)) {
            return HFOCFS_CORRUPT;
        }
        if (header_generation(fs->block) == fs->generation) {
            memcpy(block_payload(fs->block) + within, source, length);
            seal_block(fs->block);
            return write_block(fs->device, block, fs->block);
        }
        memcpy(fs->aux, fs->block, HFOCFS_BLOCK_SIZE);
    } else {
        begin_block(fs->aux, HFOCFS_BLOCK_FILE_DATA, fs->generation, 0U,
                    HFOCFS_BLOCK_PAYLOAD_SIZE);
    }
    memcpy(block_payload(fs->aux) + within, source, length);
    new_block = allocate_block(fs);
    if (new_block == 0U) {
        return HFOCFS_NOSPC;
    }
    write_u64(fs->aux + HDR_BLOCK_NO_OFFSET, new_block);
    write_u64(fs->aux + HDR_GENERATION_OFFSET, fs->generation);
    seal_block(fs->aux);
    status = write_block(fs->device, new_block, fs->aux);
    if (status != HFOCFS_OK) {
        return status;
    }
    node->blocks[block_index] = new_block;
    node->dirty = true;
    fs->dirty = true;
    return HFOCFS_OK;
}

enum hfocfs_status hfocfs_write(struct hfocfs *fs, uint32_t ino,
                                uint32_t offset, const void *buffer,
                                size_t size, size_t *done_out)
{
    struct hfocfs_node *node;
    const uint8_t *source = (const uint8_t *)buffer;
    size_t done = 0U;

    if (fs == NULL || buffer == NULL || done_out == NULL) {
        return HFOCFS_NULL;
    }
    if (!fs->used) {
        return HFOCFS_NOT_MOUNTED;
    }
    node = node_at(fs, ino);
    if (node == NULL) {
        return HFOCFS_NOT_FOUND;
    }
    if (node->type != HFOCFS_NODE_FILE) {
        return HFOCFS_IS_DIRECTORY;
    }
    *done_out = 0U;
    if ((uint64_t)offset + (uint64_t)size > HFOCFS_MAX_FILE_BYTES) {
        return HFOCFS_NOSPC;
    }
    if (size == 0U) {
        return HFOCFS_OK;
    }
    while (done < size) {
        uint32_t position = offset + (uint32_t)done;
        uint32_t block_index = position / HFOCFS_BLOCK_PAYLOAD_SIZE;
        uint32_t within = position % HFOCFS_BLOCK_PAYLOAD_SIZE;
        size_t chunk = HFOCFS_BLOCK_PAYLOAD_SIZE - within;
        enum hfocfs_status status;

        if (chunk > size - done) {
            chunk = size - done;
        }
        status = write_file_chunk(fs, node, block_index, within,
                                  source + done, chunk);
        if (status != HFOCFS_OK) {
            return status;
        }
        done += chunk;
    }
    if (offset + (uint32_t)size > node->size) {
        node->size = offset + (uint32_t)size;
    }
    node->dirty = true;
    fs->dirty = true;
    *done_out = done;
    return HFOCFS_OK;
}

enum hfocfs_status hfocfs_truncate(struct hfocfs *fs, uint32_t ino)
{
    struct hfocfs_node *node;
    uint32_t index;

    if (fs == NULL) {
        return HFOCFS_NULL;
    }
    if (!fs->used) {
        return HFOCFS_NOT_MOUNTED;
    }
    node = node_at(fs, ino);
    if (node == NULL) {
        return HFOCFS_NOT_FOUND;
    }
    if (node->type != HFOCFS_NODE_FILE) {
        return HFOCFS_IS_DIRECTORY;
    }
    node->size = 0U;
    for (index = 0U; index < HFOCFS_DIRECT_BLOCKS; ++index) {
        node->blocks[index] = 0U;
    }
    node->dirty = true;
    fs->dirty = true;
    return HFOCFS_OK;
}

static enum hfocfs_status unlink_node(struct hfocfs *fs, uint32_t ino)
{
    struct hfocfs_node *node = node_at(fs, ino);
    uint32_t index;

    if (node == NULL) {
        return HFOCFS_NOT_FOUND;
    }
    if (node->type == HFOCFS_NODE_DIRECTORY) {
        for (index = 0U; index < HFOCFS_MAX_NODES; ++index) {
            const struct hfocfs_node *child = &fs->nodes[index];

            if (child->used && child->parent == ino && !child->unlinked) {
                return HFOCFS_NOT_EMPTY;
            }
        }
    }
    node->unlinked = true;
    node->link_count = 0U;
    node->parent = 0U;
    node->dirty = true;
    fs->dirty = true;
    if (node->open_count == 0U) {
        release_node(fs, ino);
    }
    return HFOCFS_OK;
}

enum hfocfs_status hfocfs_unlink(struct hfocfs *fs, const char *path)
{
    uint32_t ino;
    enum hfocfs_status status;

    if (fs == NULL || path == NULL) {
        return HFOCFS_NULL;
    }
    status = resolve_path(fs, path, &ino);
    if (status != HFOCFS_OK) {
        return status;
    }
    if (ino == fs->root_ino) {
        return HFOCFS_BUSY;
    }
    {
        const struct hfocfs_node *node = node_view(fs, ino);
        uint32_t parent;

        if (node == NULL) {
            return HFOCFS_CORRUPT;
        }
        parent = node->parent;
        status = unlink_node(fs, ino);
        if (status == HFOCFS_OK) {
            mark_node_dirty(fs, parent);
        }
    }
    return status;
}

enum hfocfs_status hfocfs_rename(struct hfocfs *fs, const char *old_path,
                                 const char *new_path)
{
    char parent_path[HFOCFS_MAX_PATH];
    char leaf[HFOCFS_MAX_NAME + 1U];
    size_t leaf_length;
    uint32_t old_ino;
    uint32_t new_parent;
    uint32_t existing;
    struct hfocfs_node *node;
    uint32_t old_parent;
    enum hfocfs_status status;

    if (fs == NULL || old_path == NULL || new_path == NULL) {
        return HFOCFS_NULL;
    }
    status = resolve_path(fs, old_path, &old_ino);
    if (status != HFOCFS_OK) {
        return status;
    }
    if (old_ino == fs->root_ino) {
        return HFOCFS_BUSY;
    }
    status = split_parent(new_path, parent_path, sizeof(parent_path), leaf,
                          &leaf_length);
    if (status != HFOCFS_OK) {
        return status;
    }
    status = resolve_path(fs, parent_path, &new_parent);
    if (status != HFOCFS_OK) {
        return status;
    }
    node = node_at(fs, old_ino);
    if (node == NULL) {
        return HFOCFS_CORRUPT;
    }
    {
        const struct hfocfs_node *parent = node_view(fs, new_parent);

        if (parent == NULL) {
            return HFOCFS_CORRUPT;
        }
        if (parent->type != HFOCFS_NODE_DIRECTORY) {
            return HFOCFS_NOT_DIRECTORY;
        }
    }
    if (child_lookup(fs, new_parent, leaf, leaf_length, &existing) ==
            HFOCFS_OK && existing != old_ino) {
        return HFOCFS_EXISTS;
    }
    if (node->type == HFOCFS_NODE_DIRECTORY) {
        uint32_t cursor = new_parent;
        uint32_t depth;

        for (depth = 0U; depth <= HFOCFS_MAX_NODES; ++depth) {
            if (cursor == old_ino) {
                return HFOCFS_BAD_ARGUMENT;
            }
            if (cursor == fs->root_ino) {
                break;
            }
            {
                const struct hfocfs_node *walk = node_view(fs, cursor);

                if (walk == NULL) {
                    return HFOCFS_CORRUPT;
                }
                cursor = walk->parent;
            }
        }
    }
    old_parent = node->parent;
    node->parent = new_parent;
    node->name_len = (uint16_t)leaf_length;
    memcpy(node->name, leaf, leaf_length);
    node->name[leaf_length] = '\0';
    node->dirty = true;
    fs->dirty = true;
    mark_node_dirty(fs, old_parent);
    mark_node_dirty(fs, new_parent);
    return HFOCFS_OK;
}

enum hfocfs_status hfocfs_open(struct hfocfs *fs, uint32_t ino)
{
    struct hfocfs_node *node;

    if (fs == NULL) {
        return HFOCFS_NULL;
    }
    if (!fs->used) {
        return HFOCFS_NOT_MOUNTED;
    }
    node = node_at(fs, ino);
    if (node == NULL) {
        return HFOCFS_NOT_FOUND;
    }
    if (node->open_count >= HFOCFS_MAX_NODES) {
        return HFOCFS_BUSY;
    }
    ++node->open_count;
    return HFOCFS_OK;
}

enum hfocfs_status hfocfs_close(struct hfocfs *fs, uint32_t ino)
{
    struct hfocfs_node *node;

    if (fs == NULL) {
        return HFOCFS_NULL;
    }
    if (!fs->used) {
        return HFOCFS_NOT_MOUNTED;
    }
    node = node_at(fs, ino);
    if (node == NULL) {
        return HFOCFS_NOT_FOUND;
    }
    if (node->open_count == 0U) {
        return HFOCFS_BUSY;
    }
    --node->open_count;
    if (node->open_count == 0U && node->unlinked) {
        release_node(fs, ino);
    }
    return HFOCFS_OK;
}

/* ------------------------------------------------------------------------- */
/* Commit                                                                     */

static enum hfocfs_status serialize_directory(struct hfocfs *fs,
                                              struct hfocfs_node *dir)
{
    uint32_t dir_ino = (uint32_t)(dir - fs->nodes) + 1U;
    uint32_t cursor;
    uint32_t block_index = 0U;
    size_t position = 0U;
    bool started = false;
    uint64_t allocated[HFOCFS_DIRECT_BLOCKS];
    enum hfocfs_status status;

    memset(allocated, 0, sizeof(allocated));
    begin_block(fs->block2, HFOCFS_BLOCK_DIRECTORY, fs->generation, 0U,
                HFOCFS_BLOCK_PAYLOAD_SIZE);
    for (cursor = 0U; cursor < HFOCFS_MAX_NODES; ++cursor) {
        const struct hfocfs_node *child = &fs->nodes[cursor];
        size_t need;

        if (!child->used || child->parent != dir_ino || child->unlinked) {
            continue;
        }
        need = dirent_size(child->name_len);
        if (position + need > HFOCFS_BLOCK_PAYLOAD_SIZE) {
            uint64_t block = allocated[block_index];

            if (!started || block == 0U) {
                return HFOCFS_CORRUPT;
            }
            write_u64(fs->block2 + HDR_BLOCK_NO_OFFSET, block);
            seal_block(fs->block2);
            status = write_block(fs->device, block, fs->block2);
            if (status != HFOCFS_OK) {
                return status;
            }
            ++block_index;
            position = 0U;
            started = false;
            if (block_index >= HFOCFS_DIRECT_BLOCKS) {
                return HFOCFS_NOSPC;
            }
            begin_block(fs->block2, HFOCFS_BLOCK_DIRECTORY,
                        fs->generation, 0U, HFOCFS_BLOCK_PAYLOAD_SIZE);
        }
        {
            uint8_t *payload = block_payload(fs->block2);

            write_u32(payload + position, cursor + 1U);
            write_u16(payload + position + 4U, child->name_len);
            write_u16(payload + position + 6U, (uint16_t)need);
            memcpy(payload + position + DIRENT_HEADER_SIZE, child->name,
                   child->name_len);
            position += need;
            started = true;
        }
        if (allocated[block_index] == 0U) {
            uint64_t block = allocate_block(fs);

            if (block == 0U) {
                return HFOCFS_NOSPC;
            }
            allocated[block_index] = block;
        }
    }
    if (started) {
        uint64_t block = allocated[block_index];

        if (block == 0U) {
            block = allocate_block(fs);
            if (block == 0U) {
                return HFOCFS_NOSPC;
            }
            allocated[block_index] = block;
        }
        write_u64(fs->block2 + HDR_BLOCK_NO_OFFSET, block);
        seal_block(fs->block2);
        status = write_block(fs->device, block, fs->block2);
        if (status != HFOCFS_OK) {
            return status;
        }
        ++block_index;
    }
    memset(dir->blocks, 0, sizeof(dir->blocks));
    memcpy(dir->blocks, allocated, sizeof(allocated));
    return HFOCFS_OK;
}

static void build_inode_record(uint8_t *inode, const struct hfocfs_node *node,
                               uint32_t ino)
{
    uint32_t index;

    memset(inode, 0, INODE_SIZE);
    write_u32(inode + INODE_INO_OFFSET, ino);
    inode[INODE_TYPE_OFFSET] = (uint8_t)node->type;
    inode[INODE_MODE_OFFSET] = (uint8_t)(node->mode & 0xffU);
    inode[INODE_MODE_OFFSET + 1U] = (uint8_t)(node->mode >> 8);
    write_u32(inode + INODE_SIZE_OFFSET, node->size);
    write_u32(inode + INODE_LINK_OFFSET, node->link_count);
    write_u64(inode + INODE_MTIME_OFFSET, node->mtime);
    for (index = 0U; index < HFOCFS_DIRECT_BLOCKS; ++index) {
        write_u64(inode + INODE_BLOCKS_OFFSET + index * 8U,
                  node->blocks[index]);
    }
    write_u32(inode + INODE_CRC_OFFSET,
              crc32_bytes(inode, INODE_CRC_LENGTH));
}

static enum hfocfs_status commit_tree(struct hfocfs *fs)
{
    uint8_t inode_block_numbers[HFOCFS_INODE_BLOCKS_MAX];
    uint32_t inode_block_count;
    uint64_t bitmap_block;
    uint64_t commit_block;
    uint64_t new_generation = fs->generation + 1U;
    uint8_t digest[HFOCFS_DIGEST_SIZE];
    uint8_t *bitmap_payload;
    uint8_t *superblock;
    uint8_t *record;
    enum hfocfs_status status;
    uint32_t index;
    uint32_t ino;
    uint64_t bit;

    memset(inode_block_numbers, 0, sizeof(inode_block_numbers));
    for (index = 0U; index < HFOCFS_MAX_NODES; ++index) {
        struct hfocfs_node *node = &fs->nodes[index];

        if (node->used && node->type == HFOCFS_NODE_DIRECTORY && node->dirty) {
            status = serialize_directory(fs, node);
            if (status != HFOCFS_OK) {
                return status;
            }
        }
    }

    inode_block_count =
        (fs->node_count + HFOCFS_INODES_PER_BLOCK - 1U) /
        HFOCFS_INODES_PER_BLOCK;
    if (inode_block_count == 0U ||
        inode_block_count > HFOCFS_INODE_BLOCKS_MAX) {
        return HFOCFS_CORRUPT;
    }
    for (index = 0U; index < inode_block_count; ++index) {
        uint64_t block = allocate_block(fs);

        if (block == 0U) {
            return HFOCFS_NOSPC;
        }
        inode_block_numbers[index] = block;
        begin_block(fs->block, HFOCFS_BLOCK_INODE_TABLE, new_generation,
                    block, HFOCFS_BLOCK_PAYLOAD_SIZE);
        for (ino = 0U; ino < HFOCFS_INODES_PER_BLOCK; ++ino) {
            uint32_t number = index * HFOCFS_INODES_PER_BLOCK + ino + 1U;
            const struct hfocfs_node *node;

            if (number > fs->node_count) {
                break;
            }
            node = node_view(fs, number);
            if (node != NULL) {
                build_inode_record(block_payload(fs->block) + ino * INODE_SIZE,
                                   node, number);
            }
        }
        seal_block(fs->block);
        status = write_block(fs->device, block, fs->block);
        if (status != HFOCFS_OK) {
            return status;
        }
    }

    bitmap_block = allocate_block(fs);
    commit_block = allocate_block(fs);
    if (bitmap_block == 0U || commit_block == 0U) {
        return HFOCFS_NOSPC;
    }

    /* New bitmap: the new tree plus the tree that remains the fallback. */
    memset(fs->block2, 0, HFOCFS_BLOCK_SIZE);
    bitmap_payload = block_payload(fs->block2);
    set_bitmap_bit(bitmap_payload, 0U);
    set_bitmap_bit(bitmap_payload, 1U);
    for (bit = 0U; bit < fs->block_count; ++bit) {
        if (bitmap_bit(fs->base_bitmap, bit)) {
            set_bitmap_bit(bitmap_payload, bit);
        }
    }
    for (index = 0U; index < inode_block_count; ++index) {
        set_bitmap_bit(bitmap_payload, inode_block_numbers[index]);
    }
    set_bitmap_bit(bitmap_payload, bitmap_block);
    set_bitmap_bit(bitmap_payload, commit_block);
    for (index = 0U; index < HFOCFS_MAX_NODES; ++index) {
        const struct hfocfs_node *node = &fs->nodes[index];
        uint32_t block_index;

        if (!node->used) {
            continue;
        }
        for (block_index = 0U; block_index < HFOCFS_DIRECT_BLOCKS;
             ++block_index) {
            if (node->blocks[block_index] != 0U) {
                set_bitmap_bit(bitmap_payload, node->blocks[block_index]);
            }
        }
    }
    finalise_bitmap_tail(bitmap_payload, fs->block_count);
    finish_block_header(fs->block2, HFOCFS_BLOCK_BITMAP, new_generation,
                        bitmap_block, HFOCFS_BLOCK_PAYLOAD_SIZE);
    status = write_block(fs->device, bitmap_block, fs->block2);
    if (status != HFOCFS_OK) {
        return status;
    }
    status = flush_device(fs->device);
    if (status != HFOCFS_OK) {
        return status;
    }

    begin_block(fs->aux, HFOCFS_BLOCK_COMMIT, new_generation, commit_block,
                HFOCFS_BLOCK_PAYLOAD_SIZE);
    record = block_payload(fs->aux);
    write_u64(record + COMMIT_GENERATION_OFFSET, new_generation);
    write_u32(record + COMMIT_INODE_COUNT_OFFSET, inode_block_count);
    write_u32(record + COMMIT_BITMAP_COUNT_OFFSET, 1U);
    write_u32(record + COMMIT_ROOT_OFFSET, 1U);
    write_u32(record + COMMIT_NODE_COUNT_OFFSET, fs->node_count);
    for (index = 0U; index < inode_block_count; ++index) {
        write_u64(record + COMMIT_INODE_BLOCKS_OFFSET + index * 8U,
                  inode_block_numbers[index]);
    }
    write_u64(record + COMMIT_BITMAP_BLOCKS_OFFSET, bitmap_block);
    write_u32(record + COMMIT_CRC_OFFSET,
              crc32_bytes(record, COMMIT_CRC_OFFSET));
    seal_block(fs->aux);
    if (sha256_compute(fs->aux, HFOCFS_BLOCK_SIZE, digest) != SHA256_OK) {
        return HFOCFS_IO;
    }
    status = write_block(fs->device, commit_block, fs->aux);
    if (status != HFOCFS_OK) {
        return status;
    }
    status = flush_device(fs->device);
    if (status != HFOCFS_OK) {
        return status;
    }

    superblock = fs->block;
    memset(superblock, 0, HFOCFS_BLOCK_SIZE);
    memcpy(superblock + SB_MAGIC_OFFSET, HFOCFS_MAGIC, HFOCFS_MAGIC_SIZE);
    write_u32(superblock + SB_VERSION_OFFSET, HFOCFS_FORMAT_VERSION);
    write_u32(superblock + SB_BLOCK_SIZE_OFFSET, HFOCFS_BLOCK_SIZE);
    write_u64(superblock + SB_BLOCK_COUNT_OFFSET, fs->block_count);
    write_u64(superblock + SB_GENERATION_OFFSET, new_generation);
    write_u64(superblock + SB_COMMIT_BLOCK_OFFSET, commit_block);
    write_u32(superblock + SB_COMMIT_LENGTH_OFFSET, HFOCFS_BLOCK_SIZE);
    write_u32(superblock + SB_FLAGS_OFFSET, 1U);
    memcpy(superblock + SB_SHA_OFFSET, digest, sizeof(digest));
    memcpy(superblock + SB_UUID_OFFSET, fs->uuid, sizeof(fs->uuid));
    memcpy(superblock + SB_LABEL_OFFSET, fs->label, sizeof(fs->label));
    write_u32(superblock + SB_CRC_OFFSET,
              crc32_bytes(superblock, SB_CRC_LENGTH));
    status = write_block(fs->device, (uint64_t)(new_generation & 1U),
                         superblock);
    if (status != HFOCFS_OK) {
        return status;
    }
    status = flush_device(fs->device);
    if (status != HFOCFS_OK) {
        return status;
    }

    fs->generation = new_generation;
    fs->commit_block = commit_block;
    fs->clean = true;
    {
        uint8_t reachable[BITMAP_BYTES_MAX];

        memset(reachable, 0, sizeof(reachable));
        set_bitmap_bit(reachable, 0U);
        set_bitmap_bit(reachable, 1U);
        set_bitmap_bit(reachable, commit_block);
        set_bitmap_bit(reachable, bitmap_block);
        for (index = 0U; index < inode_block_count; ++index) {
            set_bitmap_bit(reachable, inode_block_numbers[index]);
        }
        for (index = 0U; index < HFOCFS_MAX_NODES; ++index) {
            const struct hfocfs_node *node = &fs->nodes[index];
            uint32_t block_index;

            if (!node->used) {
                continue;
            }
            for (block_index = 0U; block_index < HFOCFS_DIRECT_BLOCKS;
                 ++block_index) {
                if (node->blocks[block_index] != 0U) {
                    set_bitmap_bit(reachable, node->blocks[block_index]);
                }
            }
        }
        memcpy(fs->base_bitmap, reachable, sizeof(fs->base_bitmap));
        /* Keep the just-superseded tree protected until the next commit. */
        memcpy(fs->bitmap, bitmap_payload, sizeof(fs->bitmap));
        for (bit = 0U; bit < fs->block_count; ++bit) {
            if (bitmap_bit(fs->base_bitmap, bit)) {
                set_bitmap_bit(fs->bitmap, bit);
            }
        }
    }
    for (index = 0U; index < HFOCFS_MAX_NODES; ++index) {
        fs->nodes[index].dirty = false;
    }
    fs->dirty = false;
    return HFOCFS_OK;
}

enum hfocfs_status hfocfs_flush(struct hfocfs *fs)
{
    enum hfocfs_status status;

    if (fs == NULL) {
        return HFOCFS_NULL;
    }
    if (!fs->used) {
        return HFOCFS_NOT_MOUNTED;
    }
    if (fs->read_only) {
        return HFOCFS_READ_ONLY;
    }
    if (!fs->dirty) {
        return HFOCFS_OK;
    }
    status = commit_tree(fs);
    if (status != HFOCFS_OK) {
        /* Never leave the caller with a half-updated in-memory generation. */
        if (load_state(fs) != HFOCFS_OK) {
            fs->used = false;
            fs->dirty = false;
            return HFOCFS_IO;
        }
        return status == HFOCFS_NOSPC ? HFOCFS_NOSPC : HFOCFS_IO;
    }
    return HFOCFS_OK;
}

enum hfocfs_status hfocfs_unmount(struct hfocfs *fs)
{
    enum hfocfs_status status = HFOCFS_OK;

    if (fs == NULL) {
        return HFOCFS_NULL;
    }
    if (!fs->used) {
        return HFOCFS_NOT_MOUNTED;
    }
    if (fs->dirty && !fs->read_only) {
        status = hfocfs_flush(fs);
        if (status != HFOCFS_OK) {
            return status;
        }
    }
    memset(fs, 0, sizeof(*fs));
    return status;
}

const char *hfocfs_status_string(enum hfocfs_status status)
{
    switch (status) {
    case HFOCFS_OK: return "OK";
    case HFOCFS_NULL: return "NULL";
    case HFOCFS_BAD_ARGUMENT: return "BAD_ARGUMENT";
    case HFOCFS_TOO_SMALL: return "TOO_SMALL";
    case HFOCFS_TOO_LARGE: return "TOO_LARGE";
    case HFOCFS_IO: return "IO";
    case HFOCFS_CORRUPT: return "CORRUPT";
    case HFOCFS_UNSUPPORTED: return "UNSUPPORTED";
    case HFOCFS_NOSPC: return "NOSPC";
    case HFOCFS_NO_MEMORY: return "NO_MEMORY";
    case HFOCFS_READ_ONLY: return "READ_ONLY";
    case HFOCFS_NOT_MOUNTED: return "NOT_MOUNTED";
    case HFOCFS_BUSY: return "BUSY";
    case HFOCFS_NOT_FOUND: return "NOT_FOUND";
    case HFOCFS_EXISTS: return "EXISTS";
    case HFOCFS_NOT_DIRECTORY: return "NOT_DIRECTORY";
    case HFOCFS_IS_DIRECTORY: return "IS_DIRECTORY";
    case HFOCFS_NOT_EMPTY: return "NOT_EMPTY";
    case HFOCFS_NAME_TOO_LONG: return "NAME_TOO_LONG";
    case HFOCFS_PATH_TOO_LONG: return "PATH_TOO_LONG";
    }
    return "UNKNOWN";
}
