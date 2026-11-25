#ifndef HFOC_PRODUCTION_INSTALLER_ESP_H
#define HFOC_PRODUCTION_INSTALLER_ESP_H

#include <stdbool.h>
#include <stdint.h>

#include "efi_boot_environment.h"
#include "efi_fat32_esp.h"
#include "production_installer.h"

/*
 * Bind the ESP the layout declares, formatting it first when this install owns
 * the role.  ITEM-397 exposes the step because the installed boot payload set
 * is staged by a second writer (`production_installer_boot_stage_run()`), which
 * must reach the same formatted volume the single-payload path uses.
 *
 * ITEM-405: `format_owned` says who the role belongs to, not what the bytes in
 * it look like.  When it is true - a whole-disk install created that role from
 * the layout it planned - the role is formatted and then bound, so a stale
 * filesystem left in that range by a replaced partition table can never be
 * mistaken for a volume this install may write into.  When it is false - a
 * shared ESP that belongs to a system already on the machine - the volume is
 * bound as it is and is *never* formatted, whatever state it is in; a volume
 * that does not fit its partition, or that this module cannot otherwise use, is
 * refused and nothing is written to it.
 */
enum production_installer_status production_installer_esp_bind(
    const struct efi_boot_environment *environment,
    uint64_t first_lba,
    uint64_t block_count,
    bool format_owned,
    struct efi_fat32_esp *esp);

/*
 * ITEM-517: the same bind and format without an EFI environment.  The
 * legacy-BIOS FAT boot volume is a plain FAT32 volume on a machine that has no
 * UEFI at all, so the BIOS boot path binds it by device.  The formatting and
 * the resulting volume are identical to the environment entry point's; only
 * the environment is absent, and `format_owned` keeps its meaning (a role this
 * install planned is formatted, a volume that belongs to somebody else is
 * never touched).
 */
enum production_installer_status production_installer_esp_bind_device(
    struct block_device *device,
    uint64_t first_lba,
    uint64_t block_count,
    bool format_owned,
    struct efi_fat32_esp *esp);

enum production_installer_status production_installer_esp_install(
    struct production_installer *installer,
    const struct efi_boot_environment *environment,
    uint64_t first_lba,
    uint64_t block_count,
    const struct production_installer_source *source,
    bool format_owned);

/*
 * ITEM-396: install into an ESP that belongs to somebody else.  The volume is
 * bound as it is - it is never formatted, whatever its state - and only
 * `\EFI\HFOC\BOOTX64.EFI` and `\EFI\HFOC\LIMINE.CONF` are written, so a
 * foreign `\EFI\<vendor>` directory and its loaders stay exactly as they were.
 * The free-cluster count is read first, so an ESP that cannot hold both files
 * is refused with PRODUCTION_INSTALLER_CAPACITY before the first write, and an
 * ESP that is not a FAT32 volume this module understands is refused with
 * PRODUCTION_INSTALLER_ESP_UNSUITABLE - never reformatted.
 */
enum production_installer_status production_installer_esp_install_shared(
    struct production_installer *installer,
    const struct efi_boot_environment *environment,
    uint64_t first_lba,
    uint64_t block_count,
    const struct production_installer_source *source,
    const char *menu_text,
    size_t menu_length);

#endif
