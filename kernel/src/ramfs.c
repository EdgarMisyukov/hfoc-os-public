/*
 * ITEM-622: bounded RAM storage for the VFS.
 *
 * A file is a singly linked list of 4 KiB blocks.  Allocation is first-fit
 * over the fixed 64-block table, so the 256 KiB data budget is a real bound
 * and ENOSPC is a named result instead of a silent short write.  The block
 * link lives in the block header; a freshly allocated block is zeroed before
 * it is linked, so a hole created by a seek-like offset reads as zero bytes
 * and never as stale data from another file.
 */

#include "ramfs.h"

#include <string.h>

void ramfs_reset(struct vfs *fs)
{
    if (fs == NULL) {
        return;
    }
    memset(fs->nodes, 0, sizeof(fs->nodes));
    memset(fs->blocks, 0, sizeof(fs->blocks));
    memset(fs->files, 0, sizeof(fs->files));
    fs->node_count = 0U;
    fs->open_count = 0U;
    fs->data_blocks = 0U;
    fs->root = 0U;
    fs->generation = 0U;
    fs->initialized = false;
}

bool ramfs_node_valid(const struct vfs *fs, uint32_t index)
{
    return fs != NULL && index < VFS_MAX_NODES && fs->nodes[index].used;
}

bool ramfs_node_alloc(struct vfs *fs, enum vfs_node_type type, uint32_t parent,
                      const char *name, size_t name_length,
                      uint32_t *index_out)
{
    uint32_t index;

    if (fs == NULL || name == NULL || name_length == 0U ||
        name_length > VFS_NAME_MAX || index_out == NULL) {
        return false;
    }
    for (index = 0U; index < VFS_MAX_NODES; ++index) {
        struct vfs_node *node = &fs->nodes[index];

        if (node->used) {
            continue;
        }
        memset(node, 0, sizeof(*node));
        node->used = true;
        node->type = type;
        node->parent = parent;
        node->first_child = UINT32_MAX;
        node->next_sibling = UINT32_MAX;
        node->first_block = UINT32_MAX;
        memcpy(node->name, name, name_length);
        node->name[name_length] = '\0';
        ++fs->node_count;
        *index_out = index;
        return true;
    }
    return false;
}

void ramfs_node_free(struct vfs *fs, uint32_t index)
{
    struct vfs_node *node;
    uint32_t block;

    if (!ramfs_node_valid(fs, index)) {
        return;
    }
    node = &fs->nodes[index];
    block = node->first_block;
    while (block != UINT32_MAX && block < VFS_BLOCK_COUNT) {
        const uint32_t next = fs->blocks[block].next;

        if (fs->blocks[block].used) {
            memset(&fs->blocks[block], 0, sizeof(fs->blocks[block]));
            fs->blocks[block].next = UINT32_MAX;
            if (fs->data_blocks != 0U) {
                --fs->data_blocks;
            }
        }
        block = next;
    }
    memset(node, 0, sizeof(*node));
    if (fs->node_count != 0U) {
        --fs->node_count;
    }
}

static uint32_t ramfs_block_alloc(struct vfs *fs, uint32_t *previous_tail)
{
    uint32_t index;

    for (index = 0U; index < VFS_BLOCK_COUNT; ++index) {
        struct vfs_block *block = &fs->blocks[index];

        if (block->used) {
            continue;
        }
        memset(block, 0, sizeof(*block));
        block->used = true;
        block->next = UINT32_MAX;
        ++fs->data_blocks;
        if (previous_tail != NULL) {
            if (*previous_tail != UINT32_MAX) {
                fs->blocks[*previous_tail].next = index;
            }
            *previous_tail = index;
        }
        return index;
    }
    return UINT32_MAX;
}

static bool ramfs_block_at(const struct vfs *fs, uint32_t first,
                           uint32_t block_number, uint32_t *index_out)
{
    uint32_t index = first;
    uint32_t step = 0U;

    while (index != UINT32_MAX && step < block_number) {
        index = fs->blocks[index].next;
        ++step;
    }
    if (index == UINT32_MAX || index >= VFS_BLOCK_COUNT ||
        !fs->blocks[index].used) {
        return false;
    }
    *index_out = index;
    return true;
}

enum vfs_status ramfs_read(const struct vfs *fs, uint32_t node, uint32_t offset,
                           void *buffer, size_t size, size_t *done_out)
{
    const struct vfs_node *entry;
    size_t done = 0U;

    if (done_out != NULL) {
        *done_out = 0U;
    }
    if (fs == NULL || buffer == NULL || done_out == NULL) {
        return VFS_NULL;
    }
    if (!ramfs_node_valid(fs, node) || fs->nodes[node].type != VFS_NODE_FILE) {
        return VFS_INVALID;
    }
    entry = &fs->nodes[node];
    if (offset > entry->size) {
        return VFS_BAD_OFFSET;
    }
    while (done < size && offset + done < entry->size) {
        const uint32_t block_number = (offset + (uint32_t)done) / VFS_BLOCK_SIZE;
        const uint32_t within = (offset + (uint32_t)done) % VFS_BLOCK_SIZE;
        uint32_t block_index;
        size_t take = VFS_BLOCK_SIZE - within;
        const size_t remaining = entry->size - (offset + done);

        if (take > remaining) {
            take = remaining;
        }
        if (take > size - done) {
            take = size - done;
        }
        if (!ramfs_block_at(fs, entry->first_block, block_number,
                            &block_index)) {
            return VFS_NO_DATA;
        }
        memcpy((uint8_t *)buffer + done, &fs->blocks[block_index].data[within],
               take);
        done += take;
    }
    *done_out = done;
    return VFS_OK;
}

enum vfs_status ramfs_write(struct vfs *fs, uint32_t node, uint32_t offset,
                            const void *buffer, size_t size,
                            size_t *done_out)
{
    struct vfs_node *entry;
    size_t done = 0U;
    uint32_t needed;
    uint32_t have;
    uint32_t fresh[VFS_BLOCK_COUNT];
    uint32_t fresh_count = 0U;
    uint32_t new_size;

    if (done_out != NULL) {
        *done_out = 0U;
    }
    if (fs == NULL || buffer == NULL || done_out == NULL) {
        return VFS_NULL;
    }
    if (!ramfs_node_valid(fs, node) || fs->nodes[node].type != VFS_NODE_FILE) {
        return VFS_INVALID;
    }
    entry = &fs->nodes[node];
    if (size > (size_t)VFS_FILE_MAX || offset > VFS_FILE_MAX ||
        size > (size_t)VFS_FILE_MAX - offset) {
        return VFS_NO_SPACE;
    }
    if (size == 0U) {
        return VFS_OK;
    }
    needed = (uint32_t)((offset + size + VFS_BLOCK_SIZE - 1U) /
                        VFS_BLOCK_SIZE);
    have = (entry->size + VFS_BLOCK_SIZE - 1U) / VFS_BLOCK_SIZE;
    if (needed > VFS_BLOCK_COUNT) {
        return VFS_NO_SPACE;
    }
    /*
     * Reserve every new block before the first byte changes.  A block that
     * cannot be reserved is returned to the free table, so a refused write
     * leaves the old file exactly as it was.
     */
    for (uint32_t index = have; index < needed; ++index) {
        const uint32_t allocated = ramfs_block_alloc(fs, NULL);

        if (allocated == UINT32_MAX) {
            for (uint32_t undo = 0U; undo < fresh_count; ++undo) {
                memset(&fs->blocks[fresh[undo]], 0,
                       sizeof(fs->blocks[fresh[undo]]));
                fs->blocks[fresh[undo]].next = UINT32_MAX;
                if (fs->data_blocks != 0U) {
                    --fs->data_blocks;
                }
            }
            return VFS_NO_SPACE;
        }
        fresh[fresh_count++] = allocated;
    }
    if (fresh_count != 0U) {
        if (entry->first_block == UINT32_MAX) {
            entry->first_block = fresh[0];
        } else {
            uint32_t tail = entry->first_block;

            while (fs->blocks[tail].next != UINT32_MAX) {
                tail = fs->blocks[tail].next;
            }
            fs->blocks[tail].next = fresh[0];
        }
        for (uint32_t index = 1U; index < fresh_count; ++index) {
            fs->blocks[fresh[index - 1U]].next = fresh[index];
        }
        fs->blocks[fresh[fresh_count - 1U]].next = UINT32_MAX;
    }
    while (done < size) {
        const uint32_t position = offset + (uint32_t)done;
        const uint32_t block_number = position / VFS_BLOCK_SIZE;
        const uint32_t within = position % VFS_BLOCK_SIZE;
        uint32_t block_index;
        size_t take = VFS_BLOCK_SIZE - within;

        if (take > size - done) {
            take = size - done;
        }
        if (!ramfs_block_at(fs, entry->first_block, block_number,
                            &block_index)) {
            return VFS_NO_DATA;
        }
        memcpy(&fs->blocks[block_index].data[within],
               (const uint8_t *)buffer + done, take);
        done += take;
    }
    new_size = offset + (uint32_t)done;
    if (new_size > entry->size) {
        entry->size = new_size;
    }
    *done_out = done;
    return VFS_OK;
}

enum vfs_status ramfs_truncate(struct vfs *fs, uint32_t node)
{
    struct vfs_node *entry;
    uint32_t block;

    if (fs == NULL) {
        return VFS_NULL;
    }
    if (!ramfs_node_valid(fs, node) || fs->nodes[node].type != VFS_NODE_FILE) {
        return VFS_INVALID;
    }
    entry = &fs->nodes[node];
    block = entry->first_block;
    while (block != UINT32_MAX && block < VFS_BLOCK_COUNT) {
        const uint32_t next = fs->blocks[block].next;

        if (fs->blocks[block].used) {
            memset(&fs->blocks[block], 0, sizeof(fs->blocks[block]));
            fs->blocks[block].next = UINT32_MAX;
            if (fs->data_blocks != 0U) {
                --fs->data_blocks;
            }
        }
        block = next;
    }
    entry->first_block = UINT32_MAX;
    entry->size = 0U;
    return VFS_OK;
}

uint32_t ramfs_data_used(const struct vfs *fs)
{
    return fs != NULL ? fs->data_blocks * VFS_BLOCK_SIZE : 0U;
}

uint32_t ramfs_node_count(const struct vfs *fs)
{
    return fs != NULL ? fs->node_count : 0U;
}
