#ifndef HFOC_KERNEL_VFS_H
#define HFOC_KERNEL_VFS_H

#include "vfs.h"

/*
 * ITEM-624: the ordinary boot's VFS instance, published only after
 * kernel_vfs_boot_init() succeeded.  A file syscall that arrives before the
 * namespace exists gets a refusal, not a half-initialised tree.
 */
const struct vfs *kernel_vfs_boot_instance(void);

#endif
