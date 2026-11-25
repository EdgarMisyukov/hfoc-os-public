/*
 * ITEM-630 QEMU proof: format/write/flush a 16 MiB HFOCFS image on a QEMU
 * ide-hd, then mount it in two subsequent boots and read the same bytes.
 *
 * The boot sequence is phase-driven from the on-disk tree itself, so the host
 * script only has to start QEMU three times with the same raw disk:
 *   boot 1: no superblock -> format, create /phase1
 *   boot 2: /phase1 exists, verify it, create /phase2
 *   boot 3: both exist, verify both, print the final marker
 *
 * No host device path, raw SSD, USB or fixed appliance state store is used.
 */
#ifdef HFOC_HFOCFS_QEMU_TEST

#include "block_device.h"
#include "hfocfs.h"
#include "serial.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define HFOCFS_QEMU_STATE_BYTES (256U * 1024U)
#define HFOCFS_QEMU_BLOCK_BYTES 4096U

static unsigned char hfocfs_qemu_state[HFOCFS_QEMU_STATE_BYTES]
    __attribute__((aligned(64)));
static unsigned char hfocfs_qemu_block[HFOCFS_QEMU_BLOCK_BYTES]
    __attribute__((aligned(16)));

extern int ahci_qemu_bind_hfocfs(uint64_t hhdm_offset,
                                 struct block_device **device_out);

static void hfocfs_qemu_halt(void) __attribute__((noreturn));

static void hfocfs_qemu_halt(void)
{
    for (;;) {
        __asm__ volatile ("hlt" : : : "memory");
    }
}

static void hfocfs_qemu_fail(const char *where,
                             enum hfocfs_status status)
{
    {
        const uint64_t flags = serial_sequence_begin();

        serial_write("[HFOCFS] failed: ");
        serial_write(where);
        serial_write(" status=");
        serial_write(hfocfs_status_string(status));
        serial_write("\n");
        serial_sequence_end(flags);
    }
    hfocfs_qemu_halt();
}

static void hfocfs_qemu_require(bool condition, const char *where,
                                enum hfocfs_status status)
{
    if (!condition) {
        hfocfs_qemu_fail(where, status);
    }
}

static bool hfocfs_qemu_read_equals(struct hfocfs *fs, const char *path,
                                    const char *expected)
{
    uint32_t ino;
    unsigned char buffer[64];
    size_t length = strlen(expected);
    size_t done = 0U;

    if (length >= sizeof(buffer) ||
        hfocfs_lookup(fs, path, &ino) != HFOCFS_OK ||
        hfocfs_read(fs, ino, 0U, buffer, length, &done) != HFOCFS_OK) {
        return false;
    }
    return done == length && memcmp(buffer, expected, length) == 0;
}

static void hfocfs_qemu_create_marker(struct hfocfs *fs, const char *path,
                                      const char *bytes)
{
    uint32_t ino;
    size_t done = 0U;
    enum hfocfs_status status;

    status = hfocfs_create(fs, path, HFOCFS_NODE_FILE, 0, &ino);
    hfocfs_qemu_require(status == HFOCFS_OK, "create marker", status);
    status = hfocfs_write(fs, ino, 0U, bytes, strlen(bytes), &done);
    hfocfs_qemu_require(status == HFOCFS_OK && done == strlen(bytes),
                       "write marker", status);
}

void hfocfs_qemu_test_init(uint64_t hhdm_offset)
{
    struct block_device *device = NULL;
    struct hfocfs *fs = NULL;
    enum hfocfs_status status;
    enum block_device_status block_status;
    uint32_t ino;
    bool formatted;

    serial_write("[HFOCFS] proof init\n");
    hfocfs_qemu_require(hfocfs_state_size() <= sizeof(hfocfs_qemu_state),
                       "state size", HFOCFS_BAD_ARGUMENT);
    hfocfs_qemu_require(
        ((uintptr_t)hfocfs_qemu_state % hfocfs_state_alignment()) == 0U,
        "state alignment", HFOCFS_BAD_ARGUMENT);
    if (ahci_qemu_bind_hfocfs(hhdm_offset, &device) != 0 || device == NULL) {
        hfocfs_qemu_fail("bind AHCI disk", HFOCFS_IO);
    }
    block_status = block_device_read(device, 0U, hfocfs_qemu_block,
                                     sizeof(hfocfs_qemu_block));
    hfocfs_qemu_require(block_status == BLOCK_DEVICE_OK, "read superblock",
                        HFOCFS_IO);
    formatted = memcmp(hfocfs_qemu_block, HFOCFS_MAGIC, HFOCFS_MAGIC_SIZE) == 0;
    if (!formatted) {
        status = hfocfs_format(device, NULL, hfocfs_qemu_state,
                               sizeof(hfocfs_qemu_state));
        hfocfs_qemu_require(status == HFOCFS_OK, "format", status);
        serial_write("[HFOCFS] formatted 16MiB image\n");
    }

    status = hfocfs_mount(device, hfocfs_qemu_state,
                          sizeof(hfocfs_qemu_state), &fs);
    hfocfs_qemu_require(status == HFOCFS_OK, "mount", status);

    if (hfocfs_lookup(fs, "/phase1", &ino) != HFOCFS_OK) {
        hfocfs_qemu_create_marker(fs, "/phase1", "phase-one-bytes");
        status = hfocfs_flush(fs);
        hfocfs_qemu_require(status == HFOCFS_OK, "flush phase1", status);
        status = hfocfs_unmount(fs);
        hfocfs_qemu_require(status == HFOCFS_OK, "unmount phase1", status);
        serial_write("[HFOCFS] phase1 wrote\n");
    } else {
        hfocfs_qemu_require(
            hfocfs_qemu_read_equals(fs, "/phase1", "phase-one-bytes"),
            "verify phase1", HFOCFS_CORRUPT);
        if (hfocfs_lookup(fs, "/phase2", &ino) != HFOCFS_OK) {
            hfocfs_qemu_create_marker(fs, "/phase2", "phase-two-bytes");
            status = hfocfs_flush(fs);
            hfocfs_qemu_require(status == HFOCFS_OK, "flush phase2", status);
            status = hfocfs_unmount(fs);
            hfocfs_qemu_require(status == HFOCFS_OK, "unmount phase2", status);
            serial_write("[HFOCFS] phase2 wrote\n");
        } else {
            hfocfs_qemu_require(
                hfocfs_qemu_read_equals(fs, "/phase2", "phase-two-bytes"),
                "verify phase2", HFOCFS_CORRUPT);
            status = hfocfs_unmount(fs);
            hfocfs_qemu_require(status == HFOCFS_OK, "unmount proof", status);
            serial_write("[HFOCFS] proof OK\n");
        }
    }
    hfocfs_qemu_halt();
}

#else
int hfoc_hfocfs_qemu_translation_unit_marker;
#endif
