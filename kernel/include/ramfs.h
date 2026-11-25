#ifndef HFOC_RAMFS_H
#define HFOC_RAMFS_H

#include "vfs.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * ITEM-622: the bounded RAM allocation half of the VFS.  Namespaces live in
 * vfs.c; this unit owns the node table, the 4 KiB data blocks, the free lists
 * and the file byte operations.  It never calls the heap and never grows past
 * its declared tables.
 */

void ramfs_reset(struct vfs *fs);
bool ramfs_node_alloc(struct vfs *fs, enum vfs_node_type type, uint32_t parent,
                      const char *name, size_t name_length,
                      uint32_t *index_out);
void ramfs_node_free(struct vfs *fs, uint32_t index);
bool ramfs_node_valid(const struct vfs *fs, uint32_t index);
uint32_t ramfs_data_used(const struct vfs *fs);
uint32_t ramfs_node_count(const struct vfs *fs);

/* Return the number of bytes copied, or a VFS status through `status_out`. */
enum vfs_status ramfs_read(const struct vfs *fs, uint32_t node, uint32_t offset,
                           void *buffer, size_t size, size_t *done_out);
enum vfs_status ramfs_write(struct vfs *fs, uint32_t node, uint32_t offset,
                            const void *buffer, size_t size,
                            size_t *done_out);
enum vfs_status ramfs_truncate(struct vfs *fs, uint32_t node);

#endif
