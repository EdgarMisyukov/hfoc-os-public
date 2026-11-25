#ifndef HFOC_INSTALL_MEDIUM_H
#define HFOC_INSTALL_MEDIUM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * ITEM-519 increment 5: does this block device carry the medium the machine
 * booted from?
 *
 * The install journal (ITEM-521) may write only to the medium the installer
 * booted from, never to the disk being installed, so the write must be guarded
 * by an identity the disk cannot fake by being merely attached.  The loader
 * already knows which disk and partition it loaded the kernel from
 * (`struct limine_file`), and the medium itself carries the same identity in
 * its MBR; this module compares the two and names, without touching a device,
 * whether the bytes in hand are that medium and where its partition lives.
 *
 * The functions here are pure: they take already-read sectors and return a
 * verdict with a token naming the first fact that decided it.  Nothing here
 * reads, writes, binds or remembers a device.
 */

#define INSTALL_MEDIUM_SECTOR_BYTES 512U
#define INSTALL_MEDIUM_MBR_SIGNATURE_OFFSET 510U
#define INSTALL_MEDIUM_MBR_DISK_ID_OFFSET 440U
#define INSTALL_MEDIUM_MBR_PARTITION_OFFSET 446U
#define INSTALL_MEDIUM_MBR_PARTITION_SIZE 16U
#define INSTALL_MEDIUM_MBR_PARTITION_COUNT 4U
#define INSTALL_MEDIUM_MBR_TYPE_PROTECTIVE 0xEEU
#define INSTALL_MEDIUM_MBR_TYPE_FAT12 0x01U

/*
 * ITEM-582: the diagnostic medium is a second, separately prepared stick.
 *
 * The installed machine may only write its journal to the medium it booted
 * from (the installer guard above), so a diagnostic export from an installed
 * SSD could never reach a stick.  The diagnostic stick is opted in explicitly:
 * its MBR carries this disk id (the four bytes "HFCD") and the stick carries a
 * preallocated \HFOC\INSTALL.LOG in a FAT12 partition, written by
 * `scripts/hfoc-diag-stick-prepare.py`.  A random FAT volume has neither, so
 * the first FAT volume the machine sees never becomes the write target.
 */
#define INSTALL_MEDIUM_DIAGNOSTIC_DISK_ID 0x48464344U


/* `struct limine_file.media_type`, upstream values. */
#define INSTALL_MEDIUM_MEDIA_GENERIC 0U
#define INSTALL_MEDIUM_MEDIA_OPTICAL 1U
#define INSTALL_MEDIUM_MEDIA_TFTP 2U

/*
 * What the loader said about the file it loaded.  `mbr_disk_id_valid` is false
 * when the loader published no usable disk id (GPT media, optical boot, or a
 * loader that does not fill the field); the raw value is kept either way so a
 * record can publish what was actually reported.
 */
struct install_medium_boot_identity {
    uint32_t media_type;
    uint32_t partition_index;
    uint32_t mbr_disk_id;
    bool mbr_disk_id_valid;
};

enum install_medium_result {
    /* The device carries the boot medium's MBR identity. */
    INSTALL_MEDIUM_MATCH = 0,
    /* A table with an identity was read, and it is another disk's. */
    INSTALL_MEDIUM_NO_MATCH,
    /* No MBR signature (0x55AA) on the device. */
    INSTALL_MEDIUM_NO_TABLE,
    /* Nothing to compare: the loader published no identity. */
    INSTALL_MEDIUM_NO_IDENTITY,
    /* The id matched, but the partition entry the loader named is unusable. */
    INSTALL_MEDIUM_MALFORMED,
    /* The id matched and the table is the protective GPT MBR, not a layout. */
    INSTALL_MEDIUM_PROTECTIVE_MBR,
};

/*
 * The verdict.  `reason` is the first fact that decided it ("OK", "DISK_ID",
 * "SIGNATURE", "NO_LOADER", "NO_LOADER_ID", "PARTITION", "RANGE", "GPT",
 * "NO_SECTOR") and is never NULL, so a false answer always has a name.
 * `first_lba` and `length_lba` are filled only for MATCH: with a loader
 * partition index they are that entry's extent, and with index 0 (the loader
 * read the file through a filesystem, as an isohybrid medium does) the whole
 * device is the medium, so the extent is 0..block_count.
 */
struct install_medium_verdict {
    enum install_medium_result result;
    const char *reason;
    uint32_t partition_index;
    uint32_t disk_id;
    uint8_t partition_type;
    uint64_t first_lba;
    uint64_t length_lba;
};

const char *install_medium_result_string(enum install_medium_result result);

/*
 * Compare the loader's identity with one sector of the device.  `sector` is the
 * device's LBA 0; `block_count` is the device's capacity in 512-byte blocks, so
 * a partition entry that runs past the end of the device is refused instead of
 * being handed to a writer.  A NULL `boot` or `sector` is NO_IDENTITY with the
 * "NO_SECTOR" reason.
 */
struct install_medium_verdict install_medium_identify(
    const struct install_medium_boot_identity *boot,
    const uint8_t sector[INSTALL_MEDIUM_SECTOR_BYTES],
    uint64_t block_count);

/*
 * ITEM-582: the diagnostic medium's own identity, independent of the loader.
 * The sector is the device's LBA 0; `block_count` is the capacity in 512-byte
 * blocks, so a partition entry that runs past the device is refused before a
 * writer sees it.  MATCH means the disk id is the diagnostic id and the first
 * FAT12 partition's extent is inside the device; the verdict carries that
 * extent, exactly like the boot-medium verdict does.
 */
struct install_medium_verdict install_medium_diagnostic_identify(
    const uint8_t sector[INSTALL_MEDIUM_SECTOR_BYTES],
    uint64_t block_count);

/*
 * The loader's identity for this boot, implemented by the kernel that owns the
 * loader request.  False means the loader published no file identity at all;
 * the output is zeroed first, so a caller may publish it either way.
 */
bool install_medium_boot_identity(struct install_medium_boot_identity *out);

#endif
