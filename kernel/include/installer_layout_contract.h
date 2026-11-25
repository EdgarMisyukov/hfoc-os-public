#ifndef HFOC_INSTALLER_LAYOUT_CONTRACT_H
#define HFOC_INSTALLER_LAYOUT_CONTRACT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "installer_dual_boot.h"
#include "installer_hardware_discovery.h"
#include "installer_layout.h"

/*
 * ITEM-391 installation layout contract.
 *
 * ITEM-340 `installer_layout` validates and executes a plan somebody else
 * sized: it takes caller-supplied `requests[]` (role + length_lba) and only
 * checks overlap, alignment and the GPT usable region.  `production_installer`
 * executes a plan it is handed.  Before this header the only fixed numbers in
 * the tree were the QEMU fixture's 2048-LBA test shape, so the installed
 * footprint of the appliance was whatever the caller passed in.
 *
 * This header is the product's own declaration of that footprint.  The four
 * role sizes below are the installed layout; the target disk is measured
 * against them (INSTALLER_LAYOUT_CONTRACT_MINIMUM_MEDIA_LBA) and a smaller
 * disk is refused instead of being silently truncated.  `installer_layout`
 * keeps its validation and execution semantics unchanged: this module derives
 * the requests and validates the resulting plan through the existing
 * `installer_layout_*` entry points, it does not add a second planner.
 */

#define INSTALLER_LAYOUT_CONTRACT_VERSION 1U
#define INSTALLER_LAYOUT_CONTRACT_MIB_LBA 2048U
#define INSTALLER_LAYOUT_CONTRACT_RECORD_LENGTH 1024U
#define INSTALLER_LAYOUT_CONTRACT_PREVIEW_LENGTH 512U

/*
 * ITEM-517: the role counts of the declared layouts.  The UEFI clean and
 * dual-boot layouts keep four roles; the legacy-BIOS clean layout has five
 * (BIOS boot, FAT boot volume, system, persistence, recovery).  They are
 * separate constants so growing `INSTALLER_LAYOUT_PARTITION_CAPACITY` for the
 * BIOS roles cannot change what a UEFI plan contains.
 */
#define INSTALLER_LAYOUT_CONTRACT_CLEAN_ROLE_COUNT 4U
#define INSTALLER_LAYOUT_CONTRACT_DUAL_BOOT_ROLE_COUNT 4U
#define INSTALLER_LAYOUT_CONTRACT_BIOS_ROLE_COUNT 5U

/*
 * Reserved-space rule.  `installer_layout` places the first partition at
 * INSTALLER_LAYOUT_ALIGN_LBA (1 MiB) and refuses a partition whose end leaves
 * fewer than INSTALLER_LAYOUT_CONTRACT_GPT_TAIL_LBA sectors for the backup GPT
 * header, the backup partition array and the primary array.  The two numbers
 * are re-derived from the planner's behaviour by the host proof at the exact
 * minimum-media boundary, so a change to the planner cannot leave this
 * arithmetic silently wrong.
 */
#define INSTALLER_LAYOUT_CONTRACT_FIRST_LBA \
    ((uint64_t)INSTALLER_LAYOUT_ALIGN_LBA)
#define INSTALLER_LAYOUT_CONTRACT_GPT_TAIL_LBA UINT64_C(34)

/*
 * EFI System Partition: 67584 sectors = 33 MiB.
 *
 * ITEM-397 moved this role: an installed system boots from the ESP, so the
 * ESP holds the installed boot payload set and not only a loader stub.  Two
 * bounds fix the number, and the larger one wins:
 *
 *   * FAT32 geometry.  A volume is FAT32 only when it carries at least
 *     EFI_FAT32_ESP_MIN_FAT32_CLUSTERS (65525) clusters, and the installer
 *     writes one sector per cluster: the declared role must therefore leave at
 *     least 65525 data sectors after the reserved region and two FAT copies.
 *     The previous 4096-sector role could not be a valid FAT32 volume at all,
 *     and firmware that validates the BIOS parameter block refused to mount
 *     it; the 32768-sector role this item first tried carries only 32222
 *     clusters, which is FAT16 territory.  33 MiB is the smallest whole-MiB
 *     role that satisfies the rule, and the formatter's FAT size bound
 *     (EFI_FAT32_ESP_FAT_SECTORS_MAX) covers it;
 *   * payload capacity.  The set is the Limine loader on the removable-media
 *     path, the boot configuration, the sealed boot manifest, the root
 *     initramfs and the kernel, bounded by
 *     EFI_FAT32_ESP_PAYLOAD_SET_BYTES_MAX (16 MiB) with
 *     EFI_FAT32_ESP_FILE_MAX (16 MiB) per file.  The production kernel image
 *     measures 12,587,096 bytes at ITEM-627 (most of it DWARF, which the ESP
 *     carries because the staged kernel is the sealed artifact), so the
 *     declared role still leaves roughly half of the ESP free for the FAT,
 *     directory entries and growth.
 */
#define INSTALLER_LAYOUT_CONTRACT_EFI_SYSTEM_LENGTH_LBA UINT64_C(67584)

/*
 * One system image slot: 32768 sectors = 16 MiB.
 *
 * The production installer kernel image measures about 0.7 MiB of text,
 * rodata and data at this revision, so one slot is more than 20x today's
 * image: the slot is sized for the image to grow, not for the binary of the
 * day.  The role sizes below are declared as multiples of it so the A/B and
 * recovery margins stay visible.
 */
#define INSTALLER_LAYOUT_CONTRACT_SYSTEM_SLOT_LBA UINT64_C(32768)

/*
 * HFOC system partition: 131072 sectors = 64 MiB = four slots.
 *
 * It holds the signed system image (kernel plus appliance payload) with the
 * growth margin a released appliance needs: two complete A/B slots for the
 * atomic update and rollback path, one slot for the boot payload the
 * installer stages, and one slot of headroom before a layout change is
 * required.
 */
#define INSTALLER_LAYOUT_CONTRACT_HFOC_SYSTEM_LENGTH_LBA \
    (4U * INSTALLER_LAYOUT_CONTRACT_SYSTEM_SLOT_LBA)

/*
 * HFOC persistence partition: 65536 sectors = 32 MiB = two slots.
 *
 * It holds the persistent state volume (`appliance_state_layout`, up to eight
 * bounded stores and their journals) plus the audit chain and the exportable
 * support bundle.  The default state layout consumes 512 blocks and the
 * recovery-executor QEMU proof already drives a 16 MiB state partition
 * (`docs/hfoc-production-recovery-executor.md`), so 32 MiB is twice the
 * exercised volume: the journal, audit and factory-reset headroom.
 */
#define INSTALLER_LAYOUT_CONTRACT_HFOC_PERSISTENCE_LENGTH_LBA \
    (2U * INSTALLER_LAYOUT_CONTRACT_SYSTEM_SLOT_LBA)

/*
 * HFOC recovery partition: 65536 sectors = 32 MiB = two slots.
 *
 * It holds the recovery image and its own metadata, so a recovery boot never
 * depends on the system role being readable: one complete image slot plus the
 * same amount again for recovery state and margin.  It carries no A/B pair
 * because recovery is re-installed from media rather than updated in place.
 */
#define INSTALLER_LAYOUT_CONTRACT_HFOC_RECOVERY_LENGTH_LBA \
    (2U * INSTALLER_LAYOUT_CONTRACT_SYSTEM_SLOT_LBA)

/* Declared footprint of a clean install: all four roles, 130 MiB. */
#define INSTALLER_LAYOUT_CONTRACT_FOOTPRINT_LBA \
    (INSTALLER_LAYOUT_CONTRACT_EFI_SYSTEM_LENGTH_LBA + \
     INSTALLER_LAYOUT_CONTRACT_HFOC_SYSTEM_LENGTH_LBA + \
     INSTALLER_LAYOUT_CONTRACT_HFOC_PERSISTENCE_LENGTH_LBA + \
     INSTALLER_LAYOUT_CONTRACT_HFOC_RECOVERY_LENGTH_LBA)

/*
 * Declared footprint of a dual-boot install: the ESP belongs to the previous
 * system and is reused, so only these three roles are allocated
 * (`installer_dual_boot` preserves the existing ESP and never creates a
 * second one).
 */
#define INSTALLER_LAYOUT_CONTRACT_DUAL_BOOT_FOOTPRINT_LBA \
    (INSTALLER_LAYOUT_CONTRACT_HFOC_SYSTEM_LENGTH_LBA + \
     INSTALLER_LAYOUT_CONTRACT_HFOC_PERSISTENCE_LENGTH_LBA + \
     INSTALLER_LAYOUT_CONTRACT_HFOC_RECOVERY_LENGTH_LBA)

/*
 * Minimum-media rule: the smallest target the declared clean layout fits on.
 * It is the first usable LBA, plus the declared footprint, plus the GPT tail
 * reserve, and it is exact: one sector less does not fit and is refused as
 * INSTALLER_LAYOUT_CONTRACT_MEDIA_TOO_SMALL before any plan is built.  The
 * default supported-hardware policy floor (12288 blocks) is far below it, so
 * this product floor is the binding one.
 */
#define INSTALLER_LAYOUT_CONTRACT_MINIMUM_MEDIA_LBA \
    (INSTALLER_LAYOUT_CONTRACT_FIRST_LBA + \
     INSTALLER_LAYOUT_CONTRACT_FOOTPRINT_LBA + \
     INSTALLER_LAYOUT_CONTRACT_GPT_TAIL_LBA)

/*
 * ITEM-517, requirement 3: the legacy-BIOS layout.  There is no EFI System
 * Partition here at all - a BIOS machine does not have one to boot from - so
 * this is a second declared layout with its own roles, footprint and floor.
 *
 *   * the BIOS boot role holds Limine's stage2.  The measured contract needs
 *     at least 64 sectors (32768 bytes) and writes stage2 at the partition's
 *     first sector; the declared role is one alignment unit (2048 sectors =
 *     1 MiB) so every following role start stays 1 MiB aligned, which the
 *     shared summary validation requires;
 *   * the FAT boot volume plays the ESP's part: it carries the installed
 *     configuration, the kernel and the sealed boot manifest, and it is sized
 *     exactly like the ESP (33 MiB) for the same FAT32-geometry and
 *     payload-capacity reasons.  It is a plain FAT32 volume, not an ESP.
 */
#define INSTALLER_LAYOUT_CONTRACT_BIOS_BOOT_LENGTH_LBA \
    ((uint64_t)INSTALLER_LAYOUT_ALIGN_LBA)
#define INSTALLER_LAYOUT_CONTRACT_BIOS_BOOT_VOLUME_LENGTH_LBA UINT64_C(67584)

/* Declared footprint of a legacy-BIOS clean install: five roles, 162 MiB. */
#define INSTALLER_LAYOUT_CONTRACT_BIOS_FOOTPRINT_LBA \
    (INSTALLER_LAYOUT_CONTRACT_BIOS_BOOT_LENGTH_LBA + \
     INSTALLER_LAYOUT_CONTRACT_BIOS_BOOT_VOLUME_LENGTH_LBA + \
     INSTALLER_LAYOUT_CONTRACT_HFOC_SYSTEM_LENGTH_LBA + \
     INSTALLER_LAYOUT_CONTRACT_HFOC_PERSISTENCE_LENGTH_LBA + \
     INSTALLER_LAYOUT_CONTRACT_HFOC_RECOVERY_LENGTH_LBA)

/* The same exact floor rule, measured against the BIOS footprint. */
#define INSTALLER_LAYOUT_CONTRACT_BIOS_MINIMUM_MEDIA_LBA \
    (INSTALLER_LAYOUT_CONTRACT_FIRST_LBA + \
     INSTALLER_LAYOUT_CONTRACT_BIOS_FOOTPRINT_LBA + \
     INSTALLER_LAYOUT_CONTRACT_GPT_TAIL_LBA)

enum installer_layout_contract_mode {
    INSTALLER_LAYOUT_CONTRACT_MODE_UNKNOWN = 0,
    INSTALLER_LAYOUT_CONTRACT_MODE_CLEAN_WHOLE_DISK = 1,
    INSTALLER_LAYOUT_CONTRACT_MODE_DUAL_BOOT = 2,
    /*
     * ITEM-501: the install that gives up one existing partition the operator
     * named and places the appliance's three roles in the extent it occupied.
     * The shared ESP and every other partition stay exactly as they were.
     */
    INSTALLER_LAYOUT_CONTRACT_MODE_RELEASE = 3,
    /*
     * ITEM-517: the legacy-BIOS whole-disk install.  Same clean semantics as
     * MODE_CLEAN_WHOLE_DISK, but the role set has no ESP: it is the BIOS boot
     * partition plus the FAT boot volume plus the three HFOC roles, and it is
     * measured against the BIOS footprint and floor.
     */
    INSTALLER_LAYOUT_CONTRACT_MODE_CLEAN_WHOLE_DISK_BIOS = 4,
};

enum installer_layout_contract_status {
    INSTALLER_LAYOUT_CONTRACT_OK = 0,
    INSTALLER_LAYOUT_CONTRACT_NULL = 1,
    INSTALLER_LAYOUT_CONTRACT_BAD_VERSION = 2,
    INSTALLER_LAYOUT_CONTRACT_BAD_MODE = 3,
    INSTALLER_LAYOUT_CONTRACT_INVALID_ARGUMENT = 4,
    INSTALLER_LAYOUT_CONTRACT_MEDIA_TOO_SMALL = 5,
    INSTALLER_LAYOUT_CONTRACT_EXISTING_SYSTEM_OVERLAP = 6,
    INSTALLER_LAYOUT_CONTRACT_NO_RESERVE = 7,
    INSTALLER_LAYOUT_CONTRACT_PLAN_REJECTED = 8,
};

struct installer_layout_contract_role {
    bool present;
    bool reused;
    enum installer_layout_role role;
    uint64_t first_lba;
    uint64_t length_lba;
};

/*
 * Published summary of one contracted layout.  It carries exactly what the
 * plan preview and the boot/serial record print: the media and its minimum,
 * the declared footprint, the allocated footprint, the space left after it,
 * the previous system's span and one entry per role.
 */
struct installer_layout_contract_summary {
    uint32_t version;
    enum installer_layout_contract_mode mode;
    uint32_t disk_id;
    bool esp_reused;
    uint64_t media_lba;
    uint64_t minimum_media_lba;
    uint64_t declared_lba;
    uint64_t first_lba;
    uint64_t footprint_lba;
    uint64_t free_first_lba;
    uint64_t free_length_lba;
    /*
     * ITEM-501: how much room the previous system offers and how much the
     * appliance needs, in the same units.  The screen and the record publish
     * both numbers, so a refusal says "there are N MiB, M MiB are needed"
     * instead of leaving the operator to add up role sizes.
     */
    uint64_t available_lba;
    uint64_t required_lba;
    /*
     * ITEM-501: the partition this layout gives up (`mode == RELEASE`), named
     * by the discovery's own one-based partition_index and its extent.  Zero
     * for every other mode.
     */
    uint32_t released_index;
    uint64_t released_first_lba;
    uint64_t released_length_lba;
    uint32_t existing_partition_count;
    uint64_t existing_first_lba;
    uint64_t existing_length_lba;
    uint32_t role_count;
    struct installer_layout_contract_role
        roles[INSTALLER_LAYOUT_PARTITION_CAPACITY];
    enum installer_layout_status layout_status;
};

uint64_t installer_layout_contract_mib(uint64_t length_lba);
const char *installer_layout_contract_role_string(
    enum installer_layout_role role);
const char *installer_layout_contract_mode_string(
    enum installer_layout_contract_mode mode);
const char *installer_layout_contract_status_string(
    enum installer_layout_contract_status status);

/*
 * Product entry points.  Both derive their requests from the constants above
 * and validate the result through the existing `installer_layout_*` planner.
 *
 * `_plan_clean` builds the whole-disk clean plan for the target disk and
 * refuses a disk below INSTALLER_LAYOUT_CONTRACT_MINIMUM_MEDIA_LBA with
 * MEDIA_TOO_SMALL instead of truncating the layout.
 *
 * `_plan_dual_boot` sizes the previous system's reservation with the ITEM-341
 * dual-boot planner: the existing partitions and the existing EFI System
 * Partition are preserved, the three HFOC roles are placed in the detected
 * free region, and the summary reports the previous system's span and the
 * space left after the roles.  A plan that would leave the previous system no
 * space, or that overlaps a discovered partition, is refused.
 */
enum installer_layout_contract_status installer_layout_contract_plan_clean(
    const struct installer_discovery_report *report,
    uint32_t disk_id,
    struct installer_layout_plan *plan,
    struct installer_layout_contract_summary *summary);
/*
 * ITEM-517, requirement 3: the legacy-BIOS whole-disk plan.  It declares the
 * BIOS role set (BIOS boot + FAT boot volume + the three HFOC roles), refuses
 * a disk below INSTALLER_LAYOUT_CONTRACT_BIOS_MINIMUM_MEDIA_LBA with
 * MEDIA_TOO_SMALL, and publishes MODE_CLEAN_WHOLE_DISK_BIOS.  It shares the
 * plan and summary types with the UEFI entry point; every UEFI branch of
 * `_plan_clean` is untouched.
 */
enum installer_layout_contract_status
installer_layout_contract_plan_clean_bios(
    const struct installer_discovery_report *report,
    uint32_t disk_id,
    struct installer_layout_plan *plan,
    struct installer_layout_contract_summary *summary);
enum installer_layout_contract_status
installer_layout_contract_plan_dual_boot(
    const struct installer_discovery_report *report,
    uint32_t disk_id,
    struct installer_dual_boot_plan *dual_boot_plan,
    struct installer_layout_contract_summary *summary);
/*
 * ITEM-501: the same three roles, placed in the extent of the existing
 * partition the operator named instead of in free space.  `partition_index` is
 * the discovery's own index on the target disk.  The shared ESP, every other
 * partition and the protective MBR stay untouched; the named entry is removed
 * by the preserving writer.  A partition that cannot hold the three roles is
 * refused with NO_RESERVE, and the summary's `available_lba`/`required_lba`
 * carry the numbers either way.
 */
enum installer_layout_contract_status
installer_layout_contract_plan_release(
    const struct installer_discovery_report *report,
    uint32_t disk_id,
    uint32_t partition_index,
    struct installer_dual_boot_plan *dual_boot_plan,
    struct installer_layout_contract_summary *summary);
enum installer_layout_contract_status
installer_layout_contract_dual_boot_requirements(
    struct installer_dual_boot_requirements *requirements);

/* Re-check a summary against the report, the GPT region and itself. */
enum installer_layout_contract_status
installer_layout_contract_validate_summary(
    const struct installer_discovery_report *report,
    uint32_t disk_id,
    const struct installer_layout_contract_summary *summary);

/* Deterministic bounded publications; both end with a trailing newline. */
enum installer_layout_contract_status
installer_layout_contract_render_preview(
    const struct installer_layout_contract_summary *summary,
    char *buffer,
    size_t capacity,
    size_t *length);
enum installer_layout_contract_status
installer_layout_contract_render_record(
    const struct installer_layout_contract_summary *summary,
    char *buffer,
    size_t capacity,
    size_t *length);

#endif
