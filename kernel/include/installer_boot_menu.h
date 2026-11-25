#ifndef HFOC_INSTALLER_BOOT_MENU_H
#define HFOC_INSTALLER_BOOT_MENU_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "efi_fat32_esp.h"
#include "installer_hardware_discovery.h"

/*
 * ITEM-396 boot menu that keeps the existing system selectable.
 *
 * A dual-boot install adds the appliance to a disk that already boots something
 * else.  Writing our partition entries without touching the existing system's
 * boot loader is not enough: the appliance's Limine configuration has to carry
 * an entry for the appliance *and* an entry for every system the discovery
 * found, so the machine that came with Debian still reaches Debian from the
 * menu this install writes.
 *
 * The configuration is built from three inputs and nothing else:
 *
 *   * the discovery report, whose OS entries name the systems that were found
 *     (`INSTALLER_OS_LINUX`, `INSTALLER_OS_WINDOWS`, ...);
 *   * the loader inventory of the shared ESP
 *     (`efi_fat32_esp_list_loaders()`), which is where the chainload path of a
 *     system comes from - a real `\EFI\<vendor>\<loader>.efi` that is on the
 *     disk, not a guessed one;
 *   * the appliance's own kernel and recovery paths, supplied by the caller.
 *
 * Rendering rules:
 *
 *   * the appliance and recovery entries keep the label, path and module paths
 *     the caller passes (`boot():/...` paths, the same convention
 *     `boot/limine.conf` uses in this tree), so a generated menu carries the
 *     module paths the installed system's verifiable boot chain needs;
 *   * a discovered system is chainloaded with `protocol: efi` and the path of
 *     the loader found on the ESP, which is what Limine's EFI protocol is for;
 *   * a system whose loader cannot be identified is a refusal
 *     (UNKNOWN_BOOT_PATH), not an entry that would not boot: an install that
 *     cannot keep the existing system selectable must not claim it does.
 *
 * `installer_boot_menu_render_record()` publishes the menu in the house
 * single-line style and `installer_boot_menu.render()` writes the configuration
 * itself, so the preview can publish exactly the menu that will be written.
 */

#define INSTALLER_BOOT_MENU_VERSION 1U
#define INSTALLER_BOOT_MENU_ENTRY_CAPACITY 6U
#define INSTALLER_BOOT_MENU_DISK_CAPACITY INSTALLER_DISCOVERY_DISK_CAPACITY
#define INSTALLER_BOOT_MENU_VOLUME_LENGTH 48U
#define INSTALLER_BOOT_MENU_COMMENT_LENGTH 96U
/*
 * ITEM-409: the configuration is the installed boot configuration for every
 * install now, not only for one that has a system to chainload, so the bound
 * has to hold the worst case the builder can admit: six entries (the appliance,
 * four discovered systems, one leftover loader) with their labels, paths,
 * command lines and module paths, plus one comment per system and per disk the
 * menu could not offer.  A build whose configuration does not fit is refused -
 * a truncated configuration is not written - so this is a capacity, never a
 * truncation.
 */
#define INSTALLER_BOOT_MENU_TEXT_LENGTH 2048U
#define INSTALLER_BOOT_MENU_LABEL_LENGTH 48U
#define INSTALLER_BOOT_MENU_PATH_LENGTH 96U
#define INSTALLER_BOOT_MENU_RECORD_LENGTH 4096U
/* The longest single record line, reservable before it is written. */
#define INSTALLER_BOOT_MENU_RECORD_LINE_LENGTH 200U
/* The record lists at most this many loaders, then reports how many it left out. */
#define INSTALLER_BOOT_MENU_RECORD_LOADER_LINES 8U
/*
 * ITEM-409: the timeout the installed menu is rendered with.  The menu is the
 * configuration the loader reads for every install now, and a person has to be
 * able to read it and choose: five seconds is what an operator described as
 * "flashes past", and the sealed configuration this menu replaces carried three.
 * Ten seconds is the documented default, `INSTALLER_BOOT_MENU_TIMEOUT_SECONDS`
 * is what the configuration, the boot-entry record and the proofs all quote,
 * and the appliance entry is the first entry - Limine's default - so a machine
 * nobody touches still boots this appliance after the timeout.
 */
#define INSTALLER_BOOT_MENU_TIMEOUT_SECONDS 10U
#define INSTALLER_BOOT_MENU_NO_LOADER 0xFFFFFFFFU
/*
 * ITEM-398: the module paths an entry needs.  The installed system's own boot
 * chain is verifiable only when the entry that boots the kernel also names the
 * sealed boot manifest and the configuration, which is what the sealed
 * `boot/hfoc-installed.conf` does and what `[BOOT-CHAIN] verdict=ATTESTED`
 * depends on - so a generated menu carries exactly the same lines.
 */
#define INSTALLER_BOOT_MENU_MODULE_CAPACITY 3U
/*
 * ITEM-399: the command line an entry the caller owns may carry.  The installed
 * appliance states its management scope there
 * (`hfoc.management=local-only|management-interface`), and the value is the one
 * the operator declared on the install medium, so a generated menu carries the
 * same scope the sealed installed configuration carries.
 */
/*
 * ITEM-446: the generated entry carries the management scope and, when the
 * operator declared one, the whole static network (mode, address, gateway and
 * DNS), so the buffer has to hold both declarations.
 */
/*
 * ITEM-449: the entry's command line carries the management scope, the
 * declared network and the declared forward; the forward's worst case - four
 * clients with full-length addresses - is what sets the bound.
 */
#define INSTALLER_BOOT_MENU_CMDLINE_LENGTH 384U

enum installer_boot_menu_status {
    INSTALLER_BOOT_MENU_OK = 0,
    INSTALLER_BOOT_MENU_NULL = 1,
    INSTALLER_BOOT_MENU_BAD_STATE = 2,
    INSTALLER_BOOT_MENU_BAD_VERSION = 3,
    INSTALLER_BOOT_MENU_INVALID_ARGUMENT = 4,
    INSTALLER_BOOT_MENU_CAPACITY = 5,
    INSTALLER_BOOT_MENU_UNSUPPORTED_FIRMWARE = 6,
    INSTALLER_BOOT_MENU_NO_EXISTING_SYSTEM = 7,
    INSTALLER_BOOT_MENU_UNKNOWN_BOOT_PATH = 8,
};

/*
 * What the scan of one ESP found.  Every value is published: the owner's Case B
 * requires the run to say what it could not offer and why, instead of silently
 * omitting the system that lives on the other disk.
 *
 * ITEM-517: on a legacy-BIOS boot no ESP can serve a chainload at all, so the
 * scan reads no device and records every discovered disk as NO_ESP; the target
 * disk still names its volume `boot()`, the FAT boot volume the BIOS layout
 * creates and the configuration is written to.
 */
enum installer_boot_menu_scan {
    INSTALLER_BOOT_MENU_SCAN_OK = 0,
    INSTALLER_BOOT_MENU_SCAN_NO_DEVICE = 1,
    INSTALLER_BOOT_MENU_SCAN_NO_ESP = 2,
    INSTALLER_BOOT_MENU_SCAN_UNREADABLE = 3,
    INSTALLER_BOOT_MENU_SCAN_CORRUPT = 4,
    INSTALLER_BOOT_MENU_SCAN_UNSUPPORTED = 5,
    INSTALLER_BOOT_MENU_SCAN_NO_LOADER = 6,
};

/* One disk's ESP: where it is, how the menu names it, and what is on it. */
struct installer_boot_menu_disk {
    uint32_t disk_id;
    bool esp_present;
    uint64_t esp_first_lba;
    uint64_t esp_length_lba;
    /*
     * The Limine resource that names this ESP: `boot()` for the target disk,
     * whose ESP carries the configuration file, and `guid(<GUID>)` for any
     * other disk, because a firmware drive number is not stable.
     */
    char volume[INSTALLER_BOOT_MENU_VOLUME_LENGTH];
    unsigned char esp_guid[16U];
    enum installer_boot_menu_scan scan;
    uint32_t loader_count;
    struct efi_fat32_esp_loader loaders[EFI_FAT32_ESP_LOADER_CAPACITY];
};

struct installer_boot_menu_inventory {
    uint32_t version;
    uint32_t disk_count;
    uint32_t target_disk_id;
    struct installer_boot_menu_disk
        disks[INSTALLER_BOOT_MENU_DISK_CAPACITY];
};

/*
 * Hand the scan the block device of a disk id, or NULL when this run has no
 * device for it.  The scan only ever reads through it.
 */
typedef struct block_device *(*installer_boot_menu_device_fn)(
    void *context,
    uint32_t disk_id);

enum installer_boot_menu_protocol {
    INSTALLER_BOOT_MENU_PROTOCOL_UNKNOWN = 0,
    INSTALLER_BOOT_MENU_PROTOCOL_LIMINE = 1,
    INSTALLER_BOOT_MENU_PROTOCOL_EFI = 2,
};

struct installer_boot_menu_entry {
    bool present;
    enum installer_boot_menu_protocol protocol;
    enum installer_os_kind os_kind;
    uint32_t os_id;
    uint32_t module_count;
    char label[INSTALLER_BOOT_MENU_LABEL_LENGTH];
    char path[INSTALLER_BOOT_MENU_PATH_LENGTH];
    /*
     * ITEM-399: the entry's own command line, when it has one.  The appliance
     * entry always does: the installed system must state the management scope
     * its medium declared instead of falling back to the kernel's default.
     */
    char cmdline[INSTALLER_BOOT_MENU_CMDLINE_LENGTH];
    char module_paths[INSTALLER_BOOT_MENU_MODULE_CAPACITY]
                     [INSTALLER_BOOT_MENU_PATH_LENGTH];
};

/*
 * What the caller supplies for an entry it owns (the appliance and the recovery
 * image): the label, the path, the module paths that entry must carry, and the
 * command line it declares.  A NULL command line writes no `cmdline:` line; a
 * system that is chainloaded through the firmware gets none of the latter - its
 * own loader names its own modules and its own command line.
 */
struct installer_boot_menu_entry_source {
    const char *label;
    const char *path;
    const char *module_paths[INSTALLER_BOOT_MENU_MODULE_CAPACITY];
    size_t module_count;
    const char *cmdline;
};

struct installer_boot_menu {
    uint32_t version;
    enum installer_firmware_mode firmware_mode;
    uint32_t loader_count;
    uint32_t entry_count;
    /*
     * Systems the discovery found that the menu could not offer a chainload
     * entry for, and disks the scan could not read.  Both are published with
     * their reason and written into the configuration as comments, so the menu
     * never silently omits the system that is already on the machine.
     */
    uint32_t missing_count;
    uint32_t unreadable_disk_count;
    char text[INSTALLER_BOOT_MENU_TEXT_LENGTH];
    struct installer_boot_menu_entry
        entries[INSTALLER_BOOT_MENU_ENTRY_CAPACITY];
};

/*
 * ITEM-396, owner refinement: read every discovered disk's ESP and record what
 * is on it.  Read-only - it binds a FAT32 volume and lists its loaders, and
 * takes a GPT partition GUID through `installer_gpt_preserve_esp_guid()` - so
 * the acceptance host's Debian disk is never written while its loader is offered
 * in the appliance's menu.  A disk whose ESP cannot be read is recorded with its
 * scan status and does not stop the menu from being built.
 */
enum installer_boot_menu_status installer_boot_menu_scan(
    const struct installer_discovery_report *report,
    uint32_t target_disk_id,
    installer_boot_menu_device_fn device_fn,
    void *context,
    struct installer_boot_menu_inventory *inventory);

/*
 * Build the menu and render its Limine configuration into `menu->text`.  The
 * ESP loaders are the inventory `efi_fat32_esp_list_loaders()` filled; the two
 * paths are the appliance's own entries.  A report without an existing system is
 * NO_EXISTING_SYSTEM (the clean whole-disk install has nothing to keep
 * selectable and does not build this menu), a system without an identifiable
 * loader is UNKNOWN_BOOT_PATH, and more entries than the menu holds is CAPACITY.
 */
enum installer_boot_menu_status installer_boot_menu_build(
    const struct installer_discovery_report *report,
    const struct installer_boot_menu_inventory *inventory,
    const struct installer_boot_menu_entry_source *appliance,
    const struct installer_boot_menu_entry_source *recovery,
    struct installer_boot_menu *menu);

/* Re-render the configuration of a menu that was already built. */
enum installer_boot_menu_status installer_boot_menu_render(
    struct installer_boot_menu *menu);

/*
 * The machine-readable publication of the menu, one line per entry:
 *
 *   [INSTALLER-BOOT-MENU] menu firmware=uefi entries=3 loaders=2\n
 *   [INSTALLER-BOOT-MENU] entry index=1 kind=linux protocol=efi
 *     label=Debian path=boot():/EFI/debian/grubx64.efi\n
 *
 * terminated and bounded; the caller writes it inside one serial sequence.
 */
enum installer_boot_menu_status installer_boot_menu_render_record(
    const struct installer_boot_menu *menu,
    const struct installer_boot_menu_inventory *inventory,
    char *buffer,
    size_t capacity,
    size_t *length);

const char *installer_boot_menu_status_string(
    enum installer_boot_menu_status status);
const char *installer_boot_menu_scan_status_string(
    enum installer_boot_menu_scan scan);
const char *installer_boot_menu_protocol_string(
    enum installer_boot_menu_protocol protocol);

#endif
