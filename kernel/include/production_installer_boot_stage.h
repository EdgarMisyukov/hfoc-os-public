#ifndef HFOC_PRODUCTION_INSTALLER_BOOT_STAGE_H
#define HFOC_PRODUCTION_INSTALLER_BOOT_STAGE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "efi_boot_environment.h"
#include "efi_fat32_esp.h"
#include "installer_layout.h"
#include "production_installer.h"

/*
 * ITEM-397: stage the installed system into the ESP and make the firmware
 * find it.
 *
 * ITEM-350 wrote one bounded payload to the canonical HFOC path and ITEM-394
 * published the platform boot entry it could not create:
 *
 *   [PRODUCTION-INSTALL] boot-entry state=UNAVAILABLE \
 *       reason=PRODUCTION_INSTALLER_EFI_UNAVAILABLE
 *
 * The installed disk did not boot: the kernel and its boot configuration were
 * never staged, and the running installer kernel has no EFI runtime, so it
 * cannot create a Boot#### variable either.
 *
 * This module closes both halves:
 *
 *   * it stages the bounded payload set the installed system needs - the
 *     Limine loader on the removable-media path the UEFI specification
 *     requires a firmware to try, the boot configuration, the sealed boot
 *     manifest and the kernel itself - and verifies the staged bytes against
 *     the sealed ITEM-371 media digest and the staged configuration's own
 *     reference to the staged kernel;
 *   * it applies the platform boot entry when a writable EFI variable
 *     environment is available, and otherwise records the removable-media
 *     path as the mechanism that makes the disk boot, so the state of the
 *     recorded boot entry is never reported as a state of the disk.
 *
 * The pre-write gate is `production_installer_boot_stage_requirements()`: it
 * is pure arithmetic over the payload set and the ESP the layout plan
 * declares, and the caller runs it before the first write, so an ESP that
 * cannot hold the set is refused with PRODUCTION_INSTALLER_ESP_TOO_SMALL
 * instead of being filled with a partial set that cannot boot.
 */

#define PRODUCTION_INSTALLER_BOOT_STAGE_VERSION 1U

/*
 * Installed ESP paths.  The first three are the ITEM-371 sealed source
 * components in their declared order (boot payload, loader configuration,
 * kernel); the loader is what the firmware finds without a boot entry, and the
 * manifest is the sealed boot chain the installed system verifies itself
 * against.
 */
#define PRODUCTION_INSTALLER_BOOT_PAYLOAD_PATH "\\EFI\\HFOC\\BOOTX64.EFI"
#define PRODUCTION_INSTALLER_BOOT_CONFIG_PATH "\\boot\\limine\\limine.conf"
#define PRODUCTION_INSTALLER_BOOT_KERNEL_PATH "\\boot\\hfoc-kernel.elf"
#define PRODUCTION_INSTALLER_BOOT_LOADER_PATH "\\EFI\\BOOT\\BOOTX64.EFI"
#define PRODUCTION_INSTALLER_BOOT_MANIFEST_PATH "\\boot\\hfoc-boot.man"
/*
 * ITEM-623: the root initramfs is part of the installed system: the kernel
 * mounts it as `/` and the shell/userland stages consume it from there.  It is
 * staged beside the sealed set, after the sealed five, so the sealed prefix
 * and its digest keep their declared order.
 */
#define PRODUCTION_INSTALLER_BOOT_ROOTFS_PATH "\\boot\\hfoc-rootfs.arc"
/*
 * ITEM-398: where the install flow's *generated* menu goes.  Limine picks the
 * configuration next to the EFI application it was booted from first, and the
 * staged loader is `\EFI\BOOT\BOOTX64.EFI`, so this file wins over the sealed
 * `\boot\limine\limine.conf` when it exists and the sealed file is what boots
 * the system when it does not (Limine CONFIG.md, "Location of the config file").
 */
#define PRODUCTION_INSTALLER_BOOT_MENU_PATH "\\EFI\\BOOT\\limine.conf"
/*
 * Where a foreign file the install has to take over is preserved.  The name is
 * documented, the bytes are the same clusters the original occupied, and the
 * record publishes the size that was moved - so an operator can put somebody
 * else's loader or configuration back exactly as it was.
 */
#define PRODUCTION_INSTALLER_BOOT_PRESERVED_LOADER_PATH \
    "\\EFI\\HFOC\\BOOTX64.PRE"
#define PRODUCTION_INSTALLER_BOOT_PRESERVED_MENU_PATH \
    "\\EFI\\HFOC\\LIMINE.PRE"

/* The configuration line that must name the staged kernel by its real path. */
#define PRODUCTION_INSTALLER_BOOT_CONFIG_REFERENCE \
    "path: boot():/boot/hfoc-kernel.elf"

#define PRODUCTION_INSTALLER_BOOT_LOADER_MAX 1048576U
/* MOUSE-DIAG fix (2026-09-27): mirrors EFI_FAT32_ESP_FILE_MAX.  The staged
   kernel is the installed kernel, and it outgrew 8 MiB; keeping the two
   bounds equal is what makes the installer's pre-flight agree with the ESP
   writer instead of refusing a medium the writer would have accepted.
   ITEM-627: the installed kernel then outgrew 12 MiB (12,587,096 bytes), so
   both bounds move together to the 16 MiB payload-set bound. */
#define PRODUCTION_INSTALLER_BOOT_KERNEL_MAX 16777216U
#define PRODUCTION_INSTALLER_BOOT_CONFIG_MAX 65536U
#define PRODUCTION_INSTALLER_BOOT_MANIFEST_MAX 4096U
/* Mirrors INITRAMFS_MAX_SIZE: the archive the kernel accepts cannot be
   staged if it is larger than the parser bound. */
#define PRODUCTION_INSTALLER_BOOT_ROOTFS_MAX 1048576U
#define PRODUCTION_INSTALLER_BOOT_SET_COUNT 5U
#define PRODUCTION_INSTALLER_BOOT_RECORD_LENGTH 768U

struct production_installer_boot_media {
    uint32_t version;
    const unsigned char *payload;
    size_t payload_size;
    const unsigned char *config;
    size_t config_size;
    const unsigned char *kernel;
    size_t kernel_size;
    const unsigned char *loader;
    size_t loader_size;
    const unsigned char *manifest;
    size_t manifest_size;
    /*
     * ITEM-623: the validated initramfs the installed kernel mounts as `/`.
     * A caller that has none (an older hand-built medium) omits it; the
     * production flow refuses such a medium before the first write instead of
     * installing a disk with no root filesystem.
     */
    const unsigned char *rootfs;
    size_t rootfs_size;
    /*
     * ITEM-398/ITEM-409: the configuration the install flow generated from the
     * discovery (the appliance entry with its module paths plus a chainload
     * entry per system already on the machine).  ITEM-409 makes it the
     * installed configuration for *every* install: a machine that carries
     * nothing but this appliance still gets a menu, because the operator has to
     * be able to see and choose the system the disk boots.  The sealed `config`
     * is staged either way (the entry references it as a module and the signed
     * chain attests it) and is what the loader reads only when no menu could be
     * built at all.
     */
    const unsigned char *menu;
    size_t menu_size;
    /*
     * ITEM-409: how many entries the menu offers and the timeout it declares,
     * so the record states what was installed rather than only where.  Both are
     * filled by the caller that built the menu.
     */
    uint32_t menu_entries;
    uint32_t menu_timeout_seconds;
};

/*
 * ITEM-398: the ESP the boot set goes to, in either install mode.  A whole-disk
 * install declares it in its own layout plan; a preserving install reuses the
 * ESP of the system that was already there, which is why it is not one of the
 * three roles the plan carries.  `reused` is what decides whether the volume may
 * be formatted: a shared ESP is never formatted, whatever state it is in.
 */
struct production_installer_boot_esp {
    bool present;
    bool reused;
    uint64_t first_lba;
    uint64_t length_lba;
};

enum production_installer_boot_mechanism {
    PRODUCTION_INSTALLER_BOOT_MECHANISM_NONE = 0,
    /*
     * The loader is staged at \EFI\BOOT\BOOTX64.EFI.  UEFI firmware falls
     * back to that removable-media path when no boot entry matches, so the
     * disk boots on a machine whose running kernel has no EFI runtime and
     * therefore cannot write Boot####/BootOrder.
     */
    PRODUCTION_INSTALLER_BOOT_MECHANISM_REMOVABLE_MEDIA_PATH = 1,
    /* A Boot#### entry exists and BootOrder selects it. */
    PRODUCTION_INSTALLER_BOOT_MECHANISM_EFI_BOOT_VARIABLE = 2,
    /*
     * ITEM-517: the installed disk boots through Limine's BIOS stage1 in LBA0,
     * which loads the stage2 this install wrote into the BIOS boot partition
     * (`Hah!IdontNeedEFI`) and then reads the configuration and kernel from the
     * FAT boot volume.  This is the mechanism of a machine without UEFI and
     * therefore without a removable-media ESP path or a Boot#### variable.
     */
    PRODUCTION_INSTALLER_BOOT_MECHANISM_BIOS_MBR = 3,
};

/*
 * ITEM-409: which configuration the loader reads for this install.  GENERATED
 * is the menu this run built (the appliance entry, and one chainload entry per
 * system the discovery found); SEALED is the signed media configuration the
 * boot stage always stages, which is the installed configuration only when no
 * menu could be built at all - and the record then says why.
 */
enum production_installer_boot_config_source {
    PRODUCTION_INSTALLER_BOOT_CONFIG_SOURCE_SEALED = 0,
    PRODUCTION_INSTALLER_BOOT_CONFIG_SOURCE_GENERATED = 1,
};

struct production_installer_boot_outcome {
    uint32_t version;
    /*
     * ITEM-398: what was already at the two paths this set uses.  A shared ESP
     * usually carries somebody else's fallback loader at
     * `\EFI\BOOT\BOOTX64.EFI` and, when that loader is a Limine, their
     * configuration at `\EFI\BOOT\limine.conf` - which would silently shadow
     * ours.  Both are probed before the first write and preserved under the
     * documented names above when they are there.
     */
    bool loader_path_occupied;
    uint64_t loader_preserved_size;
    bool loader_preserved;
    bool menu_path_occupied;
    uint64_t menu_preserved_size;
    bool menu_preserved;
    /* The generated menu is what the loader reads for this install. */
    bool menu_installed;
    /*
     * ITEM-409: the configuration the loader reads, how many entries it offers
     * and the timeout it declares, so the record states what was installed
     * instead of only whether something was.
     */
    enum production_installer_boot_config_source config_source;
    uint32_t menu_entries;
    uint32_t menu_timeout_seconds;
    /*
     * ITEM-398/ITEM-623: how many files this run stages - the sealed five,
     * plus the initramfs when the caller carries one, plus the generated menu
     * when it is the installed configuration.  The record publishes this, so
     * it describes the set the disk actually carries.
     */
    uint32_t payload_count;
    enum production_installer_boot_mechanism mechanism;
    bool payload_staged;
    bool payload_verified;
    bool boot_entry_created;
    bool variable_attempted;
    bool variable_created;
    uint64_t esp_first_lba;
    uint64_t esp_length_lba;
    uint64_t required_sectors;
    uint64_t sealed_length;
    unsigned char sealed_digest[SHA256_DIGEST_SIZE];
};

/*
 * Pre-write gate.  No device I/O: it validates the media bounds, derives the
 * payload-set requirement for the declared ESP geometry and compares it with
 * the ESP role the plan carries.  It is callable as soon as the controller has
 * confirmed the install and has the plan, i.e. before the layout is executed,
 * which is the only place a refusal can still prevent every write.
 */
enum production_installer_status
production_installer_boot_stage_requirements(
    const struct production_installer *installer,
    const struct production_installer_boot_esp *esp,
    const struct production_installer_boot_media *media,
    struct production_installer_boot_outcome *outcome);

/*
 * Stage the payload set into the ESP, verify it against the sealed media
 * digest and the staged configuration, apply the platform boot entry when it
 * can be applied, publish the boot-entry record and fill `outcome`.
 */
enum production_installer_status production_installer_boot_stage_run(
    struct production_installer *installer,
    const struct efi_boot_environment *environment,
    const struct production_installer_boot_esp *esp,
    const struct production_installer_boot_media *media,
    struct production_installer_boot_outcome *outcome);

/*
 * Publish the boot-entry record for an outcome and a status: one bounded line
 * inside one serial sequence, so an interrupt-context trace cannot split it.
 * A refusal publishes state=UNAVAILABLE with the reason string.
 */
enum production_installer_status production_installer_boot_stage_publish(
    const struct production_installer_boot_outcome *outcome,
    enum production_installer_status status);

const char *production_installer_boot_mechanism_string(
    enum production_installer_boot_mechanism mechanism);

#endif
