/*
 * ITEM-631/635 QEMU proof: one 32 MiB persistence-partition image keeps the
 * declared 512-block state prefix, the two host-seed companion slots and the
 * last custody block untouched while a 16 MiB HFOCFS workspace at block 514
 * survives a QEMU restart through the VFS bridge.
 */
#ifdef HFOC_HFOCFS_WORKSPACE_QEMU_TEST

#include "block_device.h"
#include "hfocfs_workspace.h"
#include "serial.h"
#include "vfs.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define WS_QEMU_STATE_BYTES (256U * 1024U)
#define WS_QEMU_PREFIX_FILE "/home/owner/phase1.txt"
#define WS_QEMU_SECOND_FILE "/home/owner/phase2.txt"

static unsigned char ws_qemu_state[WS_QEMU_STATE_BYTES]
    __attribute__((aligned(64)));
static struct vfs ws_qemu_vfs;
static struct vfs_context ws_qemu_context;

extern int ahci_qemu_bind_hfocfs(uint64_t hhdm_offset,
                                 struct block_device **device_out);

static void ws_qemu_halt(void) __attribute__((noreturn));
static void ws_qemu_halt(void)
{
    for (;;) {
        __asm__ volatile ("hlt" : : : "memory");
    }
}

static void ws_qemu_fail(const char *where,
                         enum hfocfs_workspace_status status)
{
    const uint64_t flags = serial_sequence_begin();

    serial_write("[WORKSPACE-QEMU] failed: ");
    serial_write(where);
    serial_write(" status=");
    serial_write(hfocfs_workspace_status_string(status));
    serial_write("\n");
    serial_sequence_end(flags);
    ws_qemu_halt();
}

static void ws_qemu_require(bool condition, const char *where,
                            enum hfocfs_workspace_status status)
{
    if (!condition) {
        ws_qemu_fail(where, status);
    }
}

static bool ws_qemu_read_file(const char *path, const char *expected)
{
    struct vfs_file *file = NULL;
    char buffer[64];
    size_t length = strlen(expected);
    size_t done = 0U;

    if (length >= sizeof(buffer) ||
        vfs_open(&ws_qemu_context, path, VFS_OPEN_READ, &file) != VFS_OK) {
        return false;
    }
    if (vfs_read(file, buffer, length, &done) != VFS_OK ||
        vfs_close(file) != VFS_OK) {
        return false;
    }
    return done == length && memcmp(buffer, expected, length) == 0;
}

static void ws_qemu_write_file(const char *path, const char *bytes)
{
    struct vfs_file *file = NULL;
    size_t length = strlen(bytes);
    size_t done = 0U;
    enum vfs_status status;

    status = vfs_open(&ws_qemu_context, path,
                      VFS_OPEN_WRITE | VFS_OPEN_CREATE | VFS_OPEN_TRUNC,
                      &file);
    ws_qemu_require(status == VFS_OK, "open workspace file",
                    HFOCFS_WORKSPACE_VFS);
    status = vfs_write(file, bytes, length, &done);
    ws_qemu_require(status == VFS_OK && done == length, "write workspace file",
                    HFOCFS_WORKSPACE_VFS);
    ws_qemu_require(vfs_close(file) == VFS_OK, "close workspace file",
                    HFOCFS_WORKSPACE_VFS);
}

void hfocfs_workspace_qemu_test_init(uint64_t hhdm_offset)
{
    struct block_device *device = NULL;
    struct hfocfs_workspace workspace;
    struct vfs_file *file = NULL;
    enum hfocfs_workspace_status status;
    enum vfs_status vfs_status;
    bool phase1;

    serial_write("[WORKSPACE-QEMU] proof init\n");
    if (ahci_qemu_bind_hfocfs(hhdm_offset, &device) != 0 || device == NULL) {
        ws_qemu_fail("bind AHCI disk", HFOCFS_WORKSPACE_BAD_DEVICE);
    }
    status = hfocfs_workspace_bind(device, ws_qemu_state,
                                   sizeof(ws_qemu_state), true, &workspace);
    ws_qemu_require(status == HFOCFS_WORKSPACE_OK, "bind workspace", status);
    vfs_status = vfs_init(&ws_qemu_vfs);
    ws_qemu_require(vfs_status == VFS_OK, "vfs init", HFOCFS_WORKSPACE_VFS);
    vfs_status = vfs_context_init(&ws_qemu_context, &ws_qemu_vfs);
    ws_qemu_require(vfs_status == VFS_OK, "vfs context", HFOCFS_WORKSPACE_VFS);
    status = hfocfs_workspace_import(&workspace, &ws_qemu_vfs,
                                     &ws_qemu_context);
    ws_qemu_require(status == HFOCFS_WORKSPACE_OK, "import workspace", status);

    phase1 = vfs_open(&ws_qemu_context, WS_QEMU_PREFIX_FILE, VFS_OPEN_READ,
                      &file) == VFS_OK;
    if (phase1) {
        ws_qemu_require(vfs_close(file) == VFS_OK, "close phase1",
                        HFOCFS_WORKSPACE_VFS);
        ws_qemu_require(ws_qemu_read_file(WS_QEMU_PREFIX_FILE,
                                          "phase-one-workspace"),
                        "verify phase1", HFOCFS_WORKSPACE_CORRUPT);
        ws_qemu_write_file(WS_QEMU_SECOND_FILE, "phase-two-workspace");
        status = hfocfs_workspace_sync(&workspace, &ws_qemu_vfs,
                                       &ws_qemu_context);
        ws_qemu_require(status == HFOCFS_WORKSPACE_OK, "sync phase2", status);
        serial_write("[WORKSPACE-QEMU] proof OK\n");
    } else {
        ws_qemu_write_file(WS_QEMU_PREFIX_FILE, "phase-one-workspace");
        status = hfocfs_workspace_sync(&workspace, &ws_qemu_vfs,
                                       &ws_qemu_context);
        ws_qemu_require(status == HFOCFS_WORKSPACE_OK, "sync phase1", status);
        serial_write("[WORKSPACE-QEMU] phase1 wrote\n");
    }
    ws_qemu_halt();
}

#else
int hfoc_hfocfs_workspace_qemu_translation_unit_marker;
#endif
