#!/usr/bin/env bash
# ITEM-622: contracted slice for the bounded VFS and writable RAM filesystem.
#           Sourced in pinned order by scripts/check.sh; lint it on its own.

# ITEM-622 starts the Unix-shell program with a real namespace, not a console
# command table.  The limits and the unlink-open lifetime are the contract; the
# host proof must drive the production module and the ordinary boot must publish
# the root it created.
for vfs_file in \
    kernel/include/vfs.h \
    kernel/src/vfs.c \
    kernel/include/ramfs.h \
    kernel/src/ramfs.c \
    tests/vfs-host.c; do
    [[ -f $vfs_file ]] || {
        echo "ITEM-622: отсутствует $vfs_file" >&2
        exit 1
    }
done
for vfs_limit in \
    '#define VFS_NAME_MAX 255U' \
    '#define VFS_PATH_MAX 1024U' \
    '#define VFS_MAX_NODES 256U' \
    '#define VFS_MAX_FILES 64U' \
    '#define VFS_FILE_MAX (16U * VFS_BLOCK_SIZE)' \
    '#define VFS_DATA_MAX (VFS_BLOCK_COUNT * VFS_BLOCK_SIZE)'; do
    grep -Fq -- "$vfs_limit" kernel/include/vfs.h || {
        echo "ITEM-622: предел VFS не закреплён:" "$vfs_limit" >&2
        exit 1
    }
done
for vfs_symbol in \
    'vfs_open(' \
    'vfs_read(' \
    'vfs_write(' \
    'vfs_close(' \
    'vfs_stat(' \
    'vfs_mkdir(' \
    'vfs_unlink(' \
    'vfs_rename(' \
    'vfs_getcwd(' \
    'vfs_readdir(' \
    'VFS_BUSY' \
    'unlinked'; do
    grep -Fq -- "$vfs_symbol" kernel/src/vfs.c || {
        echo "ITEM-622: VFS не несёт:" "$vfs_symbol" >&2
        exit 1
    }
done
# No heap: the bounded VFS arena is the bound, and a refusal cannot grow it.
if grep -nE '\b(kmalloc|kfree|malloc|free)\s*\(' \
        kernel/src/vfs.c kernel/src/ramfs.c; then
    echo "ITEM-622: VFS/ramfs выделяет память из кучи" >&2
    exit 1
fi
grep -Fq 'kernel_vfs_boot_init();' kernel/src/kernel.c || {
    echo "ITEM-622: обычная загрузка не создаёт VFS" >&2
    exit 1
}
grep -Fq '[VFS] state=READY root=/ nodes=' kernel/src/kernel.c || {
    echo "ITEM-622: загрузка не публикует состояние VFS" >&2
    exit 1
}
grep -Fq 'test-vfs-host' Makefile || {
    echo "ITEM-622: host-проба не собрана" >&2
    exit 1
}
for vfs_proof in \
    'the node is reclaimed after the last close' \
    'a write with no free block is refused, not truncated' \
    'the files fill the declared data budget' \
    'the node table is a hard bound' \
    'a directory cannot move into its own subtree'; do
    grep -Fq -- "$vfs_proof" tests/vfs-host.c || {
        echo "ITEM-622: host-проба не утверждает:" "$vfs_proof" >&2
        exit 1
    }
done
