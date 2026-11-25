#ifndef HFOC_HFOCFS_WORKSPACE_H
#define HFOC_HFOCFS_WORKSPACE_H

#include "appliance_host_seed.h"
#include "appliance_state_block_device.h"
#include "appliance_state_layout.h"
#include "block_device.h"
#include "hfocfs.h"
#include "vfs.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * ITEM-631: bounded production workspace allocation.
 *
 * The declared persistence partition is 65536 sectors (32 MiB).  The eight
 * fixed appliance state stores occupy partition blocks 0..511.  Immediately
 * after them the host-seed companion slots own blocks 512..513: the state
 * layout ends at APPLIANCE_STATE_LAYOUT_DEFAULT_CAPACITY_BLOCKS and
 * appliance_host_seed_store_present() reads its two companion slots there.
 * The workspace must not start at 512; doing so writes HFOCFS over the first
 * host-seed slot, which makes an absent host seed look present and starts the
 * entropy login prompt on a machine that has no local secret.  ITEM-635 moved
 * the image past those slots:
 *
 *   block 0..511       fixed state stores (never touched here)
 *   block 512..513     host-seed companion slots (never touched here)
 *   block 514..33281   16 MiB HFOCFS image (32768 sectors)
 *   block 33282..65534 free / future allocation
 *   block 65535        sealed state custody record (never touched here)
 *
 * The first-use rule is fail-closed: an HFOCFS superblock is mounted, an
 * all-zero range is formatted, and any other content is refused without a
 * write.  No partition table, state-layout version, store offset, custody
 * block or encrypted secret slot is changed.
 */

#define HFOCFS_WORKSPACE_PARTITION_BLOCKS UINT64_C(65536)
#define HFOCFS_WORKSPACE_LAYOUT_BLOCKS \
    ((uint64_t)APPLIANCE_STATE_LAYOUT_DEFAULT_CAPACITY_BLOCKS)
#define HFOCFS_WORKSPACE_HOST_SEED_BLOCKS \
    ((uint64_t)APPLIANCE_HOST_SEED_STATE_SLOT_COUNT)
#define HFOCFS_WORKSPACE_OFFSET_BLOCKS \
    (HFOCFS_WORKSPACE_LAYOUT_BLOCKS + HFOCFS_WORKSPACE_HOST_SEED_BLOCKS)
#define HFOCFS_WORKSPACE_IMAGE_BLOCKS UINT64_C(32768)
#define HFOCFS_WORKSPACE_IMAGE_BYTES \
    (HFOCFS_WORKSPACE_IMAGE_BLOCKS * UINT64_C(512))
#define HFOCFS_WORKSPACE_PREFIX "/home/owner"

enum hfocfs_workspace_status {
    HFOCFS_WORKSPACE_OK = 0,
    HFOCFS_WORKSPACE_NULL = 1,
    HFOCFS_WORKSPACE_BAD_DEVICE = 2,
    HFOCFS_WORKSPACE_BAD_TARGET = 3,
    HFOCFS_WORKSPACE_REFUSED = 4,
    HFOCFS_WORKSPACE_IO = 5,
    HFOCFS_WORKSPACE_CORRUPT = 6,
    HFOCFS_WORKSPACE_VFS = 7,
    HFOCFS_WORKSPACE_NO_SPACE = 8,
    HFOCFS_WORKSPACE_NOT_BOUND = 9,
    HFOCFS_WORKSPACE_UNINITIALIZED = 10,
};

struct hfocfs_workspace {
    bool bound;
    struct appliance_state_block_device range;
    struct block_device image;
    struct hfocfs *fs;
    void *state;
    size_t state_size;
};

/*
 * Bind exactly the declared range.  `format_empty` is true for the first-use
 * path (format only an all-zero image, never a foreign one); a caller that
 * passes false gets UNINITIALIZED on an all-zero range instead of a write.
 */
enum hfocfs_workspace_status hfocfs_workspace_bind(
    struct block_device *persistence_partition,
    void *state, size_t state_size, bool format_empty,
    struct hfocfs_workspace *workspace);

/* Import the persisted tree into the VFS prefix; an absent tree is empty. */
enum hfocfs_workspace_status hfocfs_workspace_import(
    struct hfocfs_workspace *workspace, struct vfs *fs,
    struct vfs_context *context);

/* Replace the persisted tree with the current VFS prefix and commit it. */
enum hfocfs_workspace_status hfocfs_workspace_sync(
    struct hfocfs_workspace *workspace, struct vfs *fs,
    struct vfs_context *context);

/*
 * Publish the one bound production workspace.  The file syscalls call the
 * commit hook after a mutation; it is a no-op until activate() has run and is
 * re-entrancy guarded because the sync itself uses VFS helpers.
 */
void hfocfs_workspace_activate(struct hfocfs_workspace *workspace,
                               struct vfs *fs, struct vfs_context *context);
int hfocfs_workspace_commit(void);

const char *hfocfs_workspace_status_string(
    enum hfocfs_workspace_status status);

#endif
