#ifndef HFOC_INSTALLER_LAYOUT_H
#define HFOC_INSTALLER_LAYOUT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "block_device.h"
#include "installer_hardware_discovery.h"

#define INSTALLER_LAYOUT_VERSION 1U
/*
 * ITEM-517: the capacity grew from four to five roles for the legacy-BIOS
 * layout, which needs a BIOS-boot partition and a FAT boot volume in addition
 * to the three HFOC roles.  Role values 1-4 keep their numbers: `bytes[0] =
 * role` is the partition's unique GUID, so renumbering would move every UEFI
 * GPT this installer has ever written.  A UEFI plan still fills four roles and
 * every UEFI proof still sees four.
 */
#define INSTALLER_LAYOUT_PARTITION_CAPACITY 5U
#define INSTALLER_LAYOUT_SNAPSHOT_CAPACITY 6U
#define INSTALLER_LAYOUT_BLOCK_SIZE 512U
#define INSTALLER_LAYOUT_ALIGN_LBA 2048U
#define INSTALLER_LAYOUT_PREVIEW_LENGTH 512U
#define INSTALLER_LAYOUT_IDENTITY_LENGTH 32U

enum installer_layout_mode {
    INSTALLER_LAYOUT_MODE_UNKNOWN = 0,
    INSTALLER_LAYOUT_MODE_WHOLE_DISK = 1,
    INSTALLER_LAYOUT_MODE_MANUAL = 2,
};

enum installer_layout_role {
    INSTALLER_LAYOUT_ROLE_UNKNOWN = 0,
    INSTALLER_LAYOUT_ROLE_EFI_SYSTEM = 1,
    INSTALLER_LAYOUT_ROLE_HFOC_SYSTEM = 2,
    INSTALLER_LAYOUT_ROLE_HFOC_PERSISTENCE = 3,
    INSTALLER_LAYOUT_ROLE_HFOC_RECOVERY = 4,
    /*
     * ITEM-517: the legacy-BIOS boot partition.  It carries the
     * `Hah!IdontNeedEFI` GPT type and holds Limine's stage2; the firmware's
     * stage1 in LBA0 patches its own stage2 offset from this extent.  The
     * measured minimum is 64 sectors (32768 bytes); the declared role is one
     * alignment unit so every following role start stays aligned.
     */
    INSTALLER_LAYOUT_ROLE_BIOS_BOOT = 5,
    /*
     * ITEM-517: the BIOS FAT boot volume.  It plays the ESP's part on a
     * machine without UEFI - it carries the configuration, the kernel and the
     * boot manifest - but it is not an EFI System Partition and no UEFI
     * firmware is asked to read it.
     */
    INSTALLER_LAYOUT_ROLE_BOOT_VOLUME = 6,
};

#define INSTALLER_LAYOUT_BIOS_BOOT_MINIMUM_SECTORS 64U
#define INSTALLER_LAYOUT_BIOS_BOOT_MINIMUM_BYTES \
    (INSTALLER_LAYOUT_BIOS_BOOT_MINIMUM_SECTORS * \
     INSTALLER_LAYOUT_BLOCK_SIZE)

enum installer_layout_status {
    INSTALLER_LAYOUT_OK = 0,
    INSTALLER_LAYOUT_NULL = 1,
    INSTALLER_LAYOUT_BAD_VERSION = 2,
    INSTALLER_LAYOUT_BAD_STATE = 3,
    INSTALLER_LAYOUT_INVALID_ARGUMENT = 4,
    INSTALLER_LAYOUT_OUT_OF_RANGE = 5,
    INSTALLER_LAYOUT_INVALID_BLOCK_DEVICE = 6,
    INSTALLER_LAYOUT_AMBIGUOUS = 7,
    INSTALLER_LAYOUT_CAPACITY = 8,
    INSTALLER_LAYOUT_NOT_CONFIRMED = 9,
    INSTALLER_LAYOUT_ALREADY_EXECUTED = 10,
    INSTALLER_LAYOUT_IO_ERROR = 11,
    INSTALLER_LAYOUT_READ_ONLY_VIOLATION = 12,
};

struct installer_layout_partition {
    uint32_t partition_index;
    bool present;
    enum installer_layout_role role;
    uint64_t first_lba;
    uint64_t length_lba;
};

struct installer_layout_plan {
    uint32_t version;
    enum installer_layout_mode mode;
    uint32_t disk_id;
    enum installer_firmware_mode firmware_mode;
    uint32_t block_size;
    uint64_t block_count;
    uint32_t existing_partition_count;
    bool destructive;
    bool confirmed;
    bool executed;
    /*
     * ITEM-501: when set, this plan gives up one existing partition instead of
     * adding to free space.  `released_index` is the discovery's own one-based
     * partition index on this disk; the preserving writer removes exactly the
     * entry whose extent is `released_first_lba`/`released_length_lba`.  The
     * destructive whole-disk writer ignores these fields: it rebuilds the
     * table anyway.
     */
    bool releases_partition;
    uint32_t released_index;
    uint64_t released_first_lba;
    uint64_t released_length_lba;
    uint32_t partition_count;
    char identity[INSTALLER_LAYOUT_IDENTITY_LENGTH];
    struct installer_layout_partition
        partitions[INSTALLER_LAYOUT_PARTITION_CAPACITY];
};

struct installer_layout_preview {
    uint32_t version;
    uint32_t disk_id;
    enum installer_layout_mode mode;
    bool destructive;
    uint32_t existing_partition_count;
    uint32_t partition_count;
    char text[INSTALLER_LAYOUT_PREVIEW_LENGTH];
};

struct installer_layout_snapshot {
    uint32_t sector_count;
    uint64_t sector_lbas[INSTALLER_LAYOUT_SNAPSHOT_CAPACITY];
    unsigned char sectors[INSTALLER_LAYOUT_SNAPSHOT_CAPACITY]
        [INSTALLER_LAYOUT_BLOCK_SIZE];
};

/*
 * ITEM-396: the partition-entry identity the planner writes for a role.  The
 * GUID bytes are the planner's own table, exposed so the preservation writer
 * (`installer_gpt_preserve`) adds the appliance's entries with exactly the
 * identity `installer_layout_execute()` would have written, instead of keeping
 * a second copy of the GUIDs that could drift.  `_type_guid` returns the raw
 * 16 GPT bytes of the role's partition type (NULL for an unknown role);
 * `_unique_guid` fills the 16 raw bytes of the per-role unique GUID the planner
 * derives from the role and the plan slot.
 */
const unsigned char *installer_layout_role_type_guid(
    enum installer_layout_role role);
void installer_layout_role_unique_guid(
    unsigned char guid[16U],
    enum installer_layout_role role,
    uint32_t partition_index);

enum installer_layout_status installer_layout_plan_whole_disk(
    const struct installer_discovery_report *report,
    uint32_t disk_id,
    const struct installer_layout_partition *requests,
    size_t request_count,
    struct installer_layout_plan *plan);
/*
 * ITEM-517: the legacy-BIOS whole-disk plan.  Same validation and same
 * execution as the entry point above; the difference is the placement order,
 * which puts the BIOS boot role first (the measured Limine contract patches
 * stage1 with that partition's own first LBA, and partition 1 is what the
 * measurement exercised first) instead of walking the role enum.
 */
enum installer_layout_status installer_layout_plan_whole_disk_bios(
    const struct installer_discovery_report *report,
    uint32_t disk_id,
    const struct installer_layout_partition *requests,
    size_t request_count,
    struct installer_layout_plan *plan);
enum installer_layout_status installer_layout_plan_manual(
    const struct installer_discovery_report *report,
    uint32_t disk_id,
    const struct installer_layout_partition *requests,
    size_t request_count,
    struct installer_layout_plan *plan);
enum installer_layout_status installer_layout_preview(
    const struct installer_layout_plan *plan,
    struct installer_layout_preview *preview);
enum installer_layout_status installer_layout_confirm(
    struct installer_layout_plan *plan);
enum installer_layout_status installer_layout_execute(
    struct block_device *device,
    struct installer_layout_plan *plan,
    struct installer_layout_snapshot *snapshot);
enum installer_layout_status installer_layout_rollback(
    struct block_device *device,
    const struct installer_layout_snapshot *snapshot);
const char *installer_layout_status_string(
    enum installer_layout_status status);

#endif
