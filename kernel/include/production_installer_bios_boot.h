#ifndef HFOC_PRODUCTION_INSTALLER_BIOS_BOOT_H
#define HFOC_PRODUCTION_INSTALLER_BIOS_BOOT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "block_device.h"
#include "installer_layout.h"
#include "production_installer.h"
#include "production_installer_boot_stage.h"
#include "sha256.h"

/*
 * ITEM-517 step 2: the BIOS boot path of the installed disk.
 *
 * The UEFI path (`production_installer_boot_stage`) stages a payload set into
 * an EFI System Partition and lets the firmware find `\EFI\BOOT\BOOTX64.EFI`.
 * A machine without UEFI has no ESP and no firmware boot manager: it executes
 * the 512-byte MBR stage1 that `limine bios-install` writes into LBA0, which
 * loads Limine's stage2 from the *BIOS boot partition* (GPT type
 * `Hah!IdontNeedEFI`) and then reads `limine.conf`, the kernel and the boot
 * manifest from the FAT boot volume.
 *
 * The byte contract is measured (ITEM-516 preparation, section 1(g)) on the
 * pinned Limine 12.3.2 release:
 *
 *   * the stage blob is 22,516 bytes: stage1 = bytes [0:512), stage2 = bytes
 *     [512:22516);
 *   * stage1 is written at LBA0 and one little-endian u64 at byte offset 0x1a4
 *     is patched with the stage2 byte offset (bios_boot_first_lba * 512); the
 *     blob's own value there is zero, so the patch is mandatory;
 *   * before overwriting LBA0 the writer saves bytes [218:224) and [440:510)
 *     and restores them afterwards, which keeps the protective MBR entry and
 *     therefore the GPT valid;
 *   * the restored table is then made *bootable* the way `limine bios-install`
 *     makes an MBR disk bootable: a table with no active entry gets partition
 *     1's flag set to 0x80.  The layout's protective MBR carries no active
 *     entry, and firmware that consults the flag before it runs the boot
 *     sector - the L875's InsydeH2O is one - otherwise skips the disk and
 *     falls through to the next boot device (`PXE-E53` on that machine);
 *   * stage2 is written verbatim at `bios_boot_first_lba * 512`; the last
 *     sector is a partial one (22,004 = 42 * 512 + 500) and its trailing 12
 *     bytes are left untouched, so the stage writer changes exactly 44
 *     sectors:
 *     LBA0 plus the 43 sectors at the BIOS boot partition start;
 *   * `limine-bios.sys` (290,964 bytes) is *not* written by the tool: the boot
 *     volume must carry it so stage2 can load the full loader.
 *
 * The two blobs live in the installer kernel only, behind
 * HFOC_PRODUCTION_INSTALL_MEDIA: the 22,516-byte stage image is the
 * `binary_limine_hdd_bin_data` array of `deps/limine/limine-bios-hdd.h`, and
 * `deps/limine/limine-bios.sys` is linked as a binary object.  Both are
 * checked against the SHA-256 of the pinned release before a byte is written,
 * so a partial cache or a substituted blob is refused instead of being
 * installed.
 */

#define PRODUCTION_INSTALLER_BIOS_BOOT_VERSION 1U
#define PRODUCTION_INSTALLER_BIOS_RECORD_LENGTH 1024U

/*
 * Installed BIOS boot volume paths.  Limine BIOS scans the boot volume for
 * its configuration and `\boot\limine\limine.conf` is the first candidate it
 * tries, so that file is the generated menu.  The sealed configuration the
 * boot chain attests is staged as a *second* file, `\boot\hfoc\limine.conf`:
 * the menu's appliance entry names it as a module
 * (`boot():/boot/hfoc/limine.conf`), so the kernel's `boot_chain_collect()`
 * receives the sealed bytes and not the menu the install generated.  On UEFI
 * the same split exists - `\EFI\BOOT\limine.conf` is what the firmware reads,
 * `\boot\limine\limine.conf` is the module - and this is its BIOS
 * counterpart; a single file for both roles would make the chain attest the
 * menu, whose bytes the signed manifest does not seal.
 */
#define PRODUCTION_INSTALLER_BIOS_CONFIG_PATH "\\boot\\limine\\limine.conf"
#define PRODUCTION_INSTALLER_BIOS_CHAIN_CONFIG_PATH \
    "\\boot\\hfoc\\limine.conf"
/*
 * The module reference the generated menu carries for the chain
 * configuration.  It has to end in `limine.conf`: that is the suffix the
 * kernel's boot-chain collector matches for the sealed configuration
 * component.
 */
#define PRODUCTION_INSTALLER_BIOS_CHAIN_CONFIG_REFERENCE \
    "boot():/boot/hfoc/limine.conf"
#define PRODUCTION_INSTALLER_BIOS_KERNEL_PATH "\\boot\\hfoc-kernel.elf"
#define PRODUCTION_INSTALLER_BIOS_MANIFEST_PATH "\\boot\\hfoc-boot.man"
#define PRODUCTION_INSTALLER_BIOS_STAGE2_PATH \
    "\\boot\\limine\\limine-bios.sys"
/* ITEM-623: the installed system's root initramfs, staged beside the set. */
#define PRODUCTION_INSTALLER_BIOS_ROOTFS_PATH "\\boot\\hfoc-rootfs.arc"

/*
 * The sealed payload set the BIOS boot volume carries: the generated menu,
 * the sealed chain configuration, the kernel, the manifest and the loader.
 * ITEM-623 appends the root initramfs after these when the caller carries
 * one, so the set is at most one entry larger.
 */
#define PRODUCTION_INSTALLER_BIOS_SET_COUNT 5U

/* The measured Limine 12.3.2 stage contract (ITEM-516 notes, section 1(g)). */
#define PRODUCTION_INSTALLER_BIOS_STAGE1_SIZE 512U
#define PRODUCTION_INSTALLER_BIOS_STAGE2_SIZE 22004U
#define PRODUCTION_INSTALLER_BIOS_STAGE_SIZE \
    (PRODUCTION_INSTALLER_BIOS_STAGE1_SIZE + \
     PRODUCTION_INSTALLER_BIOS_STAGE2_SIZE)
#define PRODUCTION_INSTALLER_BIOS_STAGE2_OFFSET_FIELD 0x1A4U
/* The same field in decimal, as the record publishes it. */
#define PRODUCTION_INSTALLER_BIOS_STAGE2_OFFSET_FIELD_DECIMAL 420U
#define PRODUCTION_INSTALLER_BIOS_LIMINE_SYS_SIZE 290964U
/*
 * 1 stage1 sector + 42 full stage2 sectors + the partial 43rd sector, whose
 * trailing bytes are preserved by a read-modify-write.  Nothing else on the
 * disk is written by the stage writer.
 */
#define PRODUCTION_INSTALLER_BIOS_STAGE2_FULL_SECTORS 42U
#define PRODUCTION_INSTALLER_BIOS_STAGE_SECTOR_WRITES 44U

/*
 * The MBR frame the stage1 write restores and the one field it changes.
 *
 * Limine's stage image carries no partition table of its own: the writer puts
 * the disk's own bytes [440:510) back over the boot sector, so the layout's
 * protective MBR entry - and with it the GPT - survives.  What Limine does
 * *not* leave alone on an MBR disk is the active flag: `limine bios-install`
 * sets partition 1 active when no entry is, because "no active partition
 * found, some systems may not boot".  Our boot media are built by that tool
 * and carry the flag; the installed disk is written by this module and must
 * carry it too, or a firmware that requires an active entry never runs the
 * chain.
 */
#define PRODUCTION_INSTALLER_BIOS_MBR_FIRST_ENTRY_OFFSET 446U
#define PRODUCTION_INSTALLER_BIOS_MBR_ENTRY_SIZE 16U
#define PRODUCTION_INSTALLER_BIOS_MBR_ENTRY_COUNT 4U
#define PRODUCTION_INSTALLER_BIOS_MBR_ACTIVE_FLAG 0x80U

/*
 * The register-preservation patch of the pinned Stage1, measured on the L875
 * (2026-10-25).
 *
 * This firmware's legacy AHCI `INT 13h` handler does not preserve ECX, and
 * Limine's Stage1 counts its stage2 sectors in ECX (`loop .loop` in
 * `stage1/hdd/disk.asm`).  With ECX clobbered the loop runs about 65,536 times
 * instead of 63, the disk address packet's buffer offset wraps inside the
 * 0x7000 segment and the runaway reads overwrite low memory - the machine then
 * reboots before the protected-mode handoff, with no stage1 output at all.  The
 * USB path of the same firmware does preserve ECX, which is why the identical
 * stage boots from a stick and not from the installed disk.
 *
 * The writer therefore keeps ECX around both BIOS calls of the stage:
 *
 *   * the `AH=48h` parameters call at 0xfc  (`mov word [si],30; int 13h`);
 *   * the `AH=42h` read call at 0x13a       (`mov ah,42; clc; int 13h`);
 *
 * by redirecting each site into a trampoline placed in the stage's own zero
 * padding, and by turning the carry test after the read (0x8b) into a jump to
 * the tail of the second trampoline, which performs the same test and returns.
 * Every other byte of the pinned image stays as pinned; the padding is the 45
 * zero bytes at 0xad..0xda, and the trampolines use the first 32 of them.
 */
#define PRODUCTION_INSTALLER_BIOS_ECX_READ_CHECK 0x8BU
#define PRODUCTION_INSTALLER_BIOS_ECX_PARAMS 0xFCU
#define PRODUCTION_INSTALLER_BIOS_ECX_READ 0x13AU
#define PRODUCTION_INSTALLER_BIOS_ECX_PADDING 0xADU
#define PRODUCTION_INSTALLER_BIOS_ECX_PADDING_SIZE 45U
#define PRODUCTION_INSTALLER_BIOS_ECX_TRAMPOLINES_SIZE 32U

/*
 * The pinned blobs, as the installer kernel links them.  A zero `stage` /
 * `stage_size` (no installer blob transport in this build) is refused by the
 * gate with BIOS_STAGE_UNAVAILABLE, so a product build can never stage a BIOS
 * disk even if it somehow reached this module.  The sizes and digests of the
 * pinned release are checked by the gate as well.
 *
 * The value is returned by value because `limine-bios.sys`'s length is the
 * difference of two linker symbols and is therefore a runtime value, not a
 * constant initializer.
 */
struct production_installer_bios_blob {
    uint32_t version;
    const unsigned char *stage;
    size_t stage_size;
    const unsigned char *limine_bios_sys;
    size_t limine_bios_sys_size;
};

struct production_installer_bios_blob production_installer_bios_blob(void);

/*
 * The two partition extents the BIOS boot chain is written into.  A whole-disk
 * BIOS install owns both (`installer_layout_contract_plan_clean_bios`), so the
 * FAT boot volume is always formatted by this install; a BIOS preserve install
 * is refused by the controller before any plan exists.
 */
struct production_installer_bios_partition {
    bool present;
    uint64_t first_lba;
    uint64_t length_lba;
};

struct production_installer_bios_boot_outcome {
    uint32_t version;
    bool payload_staged;
    bool payload_verified;
    bool stage1_written;
    bool stage2_written;
    bool stage1_verified;
    bool stage2_verified;
    bool config_verified;
    bool gpt_verified;
    bool menu_installed;
    enum production_installer_boot_mechanism mechanism;
    uint32_t menu_entries;
    uint32_t menu_timeout_seconds;
    uint64_t bios_boot_first_lba;
    uint64_t bios_boot_length_lba;
    uint64_t boot_volume_first_lba;
    uint64_t boot_volume_length_lba;
    uint64_t payload_count;
    uint64_t payload_bytes;
    uint64_t required_sectors;
    uint64_t stage1_lba;
    uint64_t stage2_bytes;
    uint64_t stage2_offset;
    uint64_t stage2_field_offset;
    uint64_t stage_sector_writes;
    uint64_t limine_bios_sys_bytes;
    unsigned char blob_digest[SHA256_DIGEST_SIZE];
    unsigned char limine_bios_sys_digest[SHA256_DIGEST_SIZE];
};

/*
 * Pre-write gate.  No device I/O: it requires a legacy-BIOS layout (the
 * summary mode the BIOS contract publishes, so a UEFI install cannot enter the
 * path), the pinned stage blobs, a BIOS boot partition of at least the
 * measured 64 sectors and a FAT boot volume large enough for the payload set.
 * Named refusals: BIOS_STAGE_UNAVAILABLE (no BIOS layout, no blob transport or
 * a partition absent), BIOS_BOOT_PARTITION_TOO_SMALL, BOOT_VOLUME_TOO_SMALL.
 */
enum production_installer_status
production_installer_bios_boot_requirements(
    const struct production_installer *installer,
    const struct production_installer_bios_partition *bios_boot,
    const struct production_installer_bios_partition *boot_volume,
    const struct production_installer_boot_media *media,
    struct production_installer_bios_boot_outcome *outcome);

/*
 * Stage the BIOS boot path: format and bind the FAT boot volume, write the
 * payload set (the generated menu, the sealed chain configuration, the kernel,
 * the sealed boot manifest and `\boot\limine\limine-bios.sys`), write
 * stage1/stage2 into the target disk by the measured recipe, verify every byte
 * by read-back - the stage1 recipe, the stage2 region, both staged
 * configurations' kernel references, the boot volume's `limine-bios.sys` and
 * the primary/backup GPT CRCs - and publish the one
 * `boot-entry mechanism=BIOS_MBR` record.
 */
enum production_installer_status production_installer_bios_boot_run(
    struct production_installer *installer,
    struct block_device *device,
    const struct production_installer_bios_partition *bios_boot,
    const struct production_installer_bios_partition *boot_volume,
    const struct production_installer_boot_media *media,
    struct production_installer_bios_boot_outcome *outcome);

/*
 * Publish the boot-entry record for an outcome and a status: one bounded line
 * inside one serial sequence.  A refusal publishes state=UNAVAILABLE with the
 * reason string, so the acceptance INSTALLER stage quotes the same vocabulary
 * the UEFI path uses.
 */
enum production_installer_status production_installer_bios_boot_publish(
    const struct production_installer_bios_boot_outcome *outcome,
    enum production_installer_status status);

#endif
