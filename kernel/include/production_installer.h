#ifndef HFOC_PRODUCTION_INSTALLER_H
#define HFOC_PRODUCTION_INSTALLER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "installer_hardware_discovery.h"
#include "installer_layout.h"
#include "installer_layout_contract.h"
#include "installer_media_verification.h"
#include "installer_supported_hardware.h"
#include "sha256.h"

#define PRODUCTION_INSTALLER_VERSION 1U
#define PRODUCTION_INSTALLER_IDENTITY_LENGTH 32U
#define PRODUCTION_INSTALLER_PREVIEW_LENGTH 1024U
#define PRODUCTION_INSTALLER_SOURCE_VERSION 1U
/*
 * ITEM-397 moved this bound from 262144 (256 KiB) to the ESP writer's own
 * single-payload bound.  The product's boot payload is the Limine
 * `BOOTX64.EFI` the ITEM-350 OVMF proof installs at the canonical
 * `\EFI\HFOC\BOOTX64.EFI` path, and that file measures 360448 bytes at
 * Limine 12.3.2: the controller refused the product's own payload while the
 * writer below it accepted it.
 *
 * The largest boot payload the controller accepts is exactly what the ESP
 * writer can install (`EFI_FAT32_ESP_PAYLOAD_MAX`): the signed Limine UEFI
 * loader the installer medium ships is 352 KiB, so a tighter controller
 * bound would refuse the product's own media.  The controller bound is
 * derived from that writer bound, never widened past it.
 */
#define PRODUCTION_INSTALLER_BOOT_PAYLOAD_MAX 524288U

enum production_installer_stage {
    PRODUCTION_INSTALLER_STAGE_UNPREPARED = 0,
    PRODUCTION_INSTALLER_STAGE_EVIDENCE_VALIDATED = 1,
    PRODUCTION_INSTALLER_STAGE_PLAN_BUILT = 2,
    PRODUCTION_INSTALLER_STAGE_PREVIEW_PUBLISHED = 3,
    PRODUCTION_INSTALLER_STAGE_CONFIRMED = 4,
    PRODUCTION_INSTALLER_STAGE_EXECUTING = 5,
    PRODUCTION_INSTALLER_STAGE_VERIFIED = 6,
    PRODUCTION_INSTALLER_STAGE_FAILED = 7,
};

enum production_installer_mode {
    PRODUCTION_INSTALLER_MODE_UNKNOWN = 0,
    PRODUCTION_INSTALLER_MODE_CLEAN_WHOLE_DISK = 1,
    PRODUCTION_INSTALLER_MODE_CLEAN_MANUAL = 2,
    PRODUCTION_INSTALLER_MODE_DUAL_BOOT = 3,
};

enum production_installer_status {
    PRODUCTION_INSTALLER_OK = 0,
    PRODUCTION_INSTALLER_NULL = 1,
    PRODUCTION_INSTALLER_BAD_STATE = 2,
    PRODUCTION_INSTALLER_BAD_VERSION = 3,
    PRODUCTION_INSTALLER_INVALID_ARGUMENT = 4,
    PRODUCTION_INSTALLER_UNSAFE_EVIDENCE = 5,
    PRODUCTION_INSTALLER_SOURCE_UNAVAILABLE = 6,
    PRODUCTION_INSTALLER_EFI_UNAVAILABLE = 7,
    PRODUCTION_INSTALLER_NOT_CONFIRMED = 8,
    PRODUCTION_INSTALLER_ALREADY_CONFIRMED = 9,
    PRODUCTION_INSTALLER_CAPACITY = 10,
    PRODUCTION_INSTALLER_IO_ERROR = 11,
    PRODUCTION_INSTALLER_VERIFICATION_FAILED = 12,
    PRODUCTION_INSTALLER_MEDIA_UNVERIFIED = 13,
    PRODUCTION_INSTALLER_UNSUPPORTED_HARDWARE = 14,
    PRODUCTION_INSTALLER_MEDIA_TOO_SMALL = 15,
    PRODUCTION_INSTALLER_EXISTING_SYSTEM_CONFLICT = 16,
    /*
     * ITEM-397: the ESP the layout declares cannot hold the installed boot
     * payload set, and the staged boot configuration does not name the staged
     * kernel.  Both are refused before the first write of the set.
     */
    PRODUCTION_INSTALLER_ESP_TOO_SMALL = 17,
    PRODUCTION_INSTALLER_BOOT_CONFIG_INVALID = 18,
    /*
     * ITEM-396 refusals, each one exact and each one taken before the first
     * write of the preserving install.
     */
    PRODUCTION_INSTALLER_TABLE_UNREADABLE = 19,
    PRODUCTION_INSTALLER_TABLE_CORRUPT = 20,
    PRODUCTION_INSTALLER_NO_FREE_SPACE = 21,
    PRODUCTION_INSTALLER_ESP_UNSUITABLE = 22,
    PRODUCTION_INSTALLER_MENU_UNAVAILABLE = 23,
    /*
     * ITEM-517: the preserving (dual-boot) planner is the UEFI ESP-reuse
     * contract.  A legacy-BIOS machine has no ESP to reuse and the BIOS boot
     * chain is a whole-disk chain, so a BIOS preserve install is refused by
     * name instead of being planned against a partition it cannot boot from.
     */
    PRODUCTION_INSTALLER_BIOS_PRESERVE_UNSUPPORTED = 24,
    /*
     * ITEM-517 step 2: the BIOS boot path of the installed disk.  The stage is
     * unavailable when this install is not a legacy-BIOS whole-disk layout, or
     * when the installer kernel carries no pinned stage blob, or when a role
     * the measured recipe needs is absent; the two size refusals name the
     * measured 64-sector BIOS boot minimum and the payload-set requirement of
     * the FAT boot volume.
     */
    PRODUCTION_INSTALLER_BIOS_STAGE_UNAVAILABLE = 25,
    PRODUCTION_INSTALLER_BIOS_BOOT_PARTITION_TOO_SMALL = 26,
    PRODUCTION_INSTALLER_BOOT_VOLUME_TOO_SMALL = 27,
    /*
     * ITEM-586: `HFOC-INSTALL-LAYOUT=update` names a state-preserving reinstall.
     * It is refused by name when the target does not carry a loadable appliance
     * state in exactly the persistence range the new plan will write, so an
     * update can never silently fall back to a clean install.
     */
    PRODUCTION_INSTALLER_STATE_NOT_PRESERVABLE = 28,
};

struct production_installer_source {
    uint32_t version;
    const unsigned char *boot_payload;
    size_t boot_payload_size;
    const unsigned char *limine_config;
    size_t limine_config_size;
    const unsigned char *kernel;
    size_t kernel_size;
};

/*
 * ITEM-396: what the preserving install will keep and what it will add, in the
 * numbers the preview and the `[INSTALLER-PRESERVE]` record publish.  The
 * tables themselves (the GPT bookkeeping and the boot menu) stay caller-owned
 * storage, because they are kilobytes, not bytes.
 */
struct production_installer_preserve_summary {
    uint32_t version;
    uint32_t preserved_count;
    uint32_t added_count;
    uint32_t loader_count;
    uint32_t menu_entry_count;
    uint32_t menu_missing_count;
    uint32_t unreadable_disk_count;
    bool esp_reused;
    uint64_t esp_first_lba;
    uint64_t esp_length_lba;
    uint64_t free_first_lba;
    uint64_t free_length_lba;
    uint64_t existing_first_lba;
    uint64_t existing_length_lba;
};

struct production_installer {
    uint32_t version;
    enum production_installer_stage stage;
    enum production_installer_mode mode;
    bool confirmed;
    bool source_ready;
    bool efi_ready;
    bool media_verified;
    bool hardware_supported;
    bool layout_ready;
    /*
     * ITEM-397: the SHA-256 the sealed ITEM-371 media manifest binds over the
     * source components (boot payload, loader configuration, kernel, in that
     * order).  The installed boot set is checked against this digest after it
     * is staged, so the bytes on the disk are the bytes the media declared.
     */
    bool source_digest_ready;
    uint64_t source_length;
    unsigned char source_digest[SHA256_DIGEST_SIZE];
    bool preserve_ready;
    /*
     * ITEM-502: whether this run erased the appliance's persistent state on the
     * target before writing a new system.  The erase is an explicit named step
     * of the install; `production_installer_state_initialize()` and the layout
     * verification refuse a run that did not perform it, so no path can reach
     * `applied` while the previous run's address, administrator credential or
     * certificates could still be read back.
     */
    bool state_erased;
    /*
     * ITEM-586: a `layout=update` run keeps the previous appliance's state
     * instead of erasing it.  Only `production_installer_state_preserve()`
     * sets this flag, and it is mutually exclusive with `state_erased`.
     */
    bool state_preserved;
    /*
     * ITEM-501: a dual-boot install that gives up one existing partition the
     * operator named instead of using free space.  The index is the discovery's
     * one-based partition index on `disk_id`; the flow sets both before
     * `production_installer_plan_layout()` and the layout contract names the
     * partition in the plan and in the published summary.
     */
    bool release_partition;
    uint32_t release_partition_index;
    uint32_t disk_id;
    uint32_t media_release_id;
    uint32_t media_signer_id;
    enum installer_media_verification_status media_status;
    enum installer_supported_hardware_reason hardware_reason;
    struct production_installer_source source;
    struct installer_layout_contract_summary layout;
    struct production_installer_preserve_summary preserve;
    char identity[PRODUCTION_INSTALLER_IDENTITY_LENGTH];
    char preview[PRODUCTION_INSTALLER_PREVIEW_LENGTH];
};

enum production_installer_status production_installer_init(
    struct production_installer *installer);
enum production_installer_status production_installer_prepare(
    struct production_installer *installer,
    const struct installer_discovery_report *report,
    enum production_installer_mode mode,
    uint32_t disk_id,
    const char *identity,
    size_t identity_length,
    const struct production_installer_source *source,
    bool efi_ready);
enum production_installer_status production_installer_build_plan(
    struct production_installer *installer);
enum production_installer_status production_installer_plan_layout(
    struct production_installer *installer,
    const struct installer_discovery_report *report,
    struct installer_layout_plan *plan,
    struct installer_dual_boot_plan *dual_boot_plan);
enum production_installer_status production_installer_verify_media(
    struct production_installer *installer,
    const struct verification_policy *policy,
    const struct installer_media_verifier *verifier,
    uint32_t expected_release_id,
    const void *envelope,
    size_t envelope_length,
    const void *media,
    size_t media_length);
enum production_installer_status production_installer_verify_hardware(
    struct production_installer *installer,
    const struct installer_discovery_report *report,
    const struct installer_supported_hardware_policy *policy);
enum production_installer_status production_installer_publish_preview(
    struct production_installer *installer);
enum production_installer_status production_installer_confirm(
    struct production_installer *installer);
enum production_installer_status production_installer_reset(
    struct production_installer *installer);
enum production_installer_status production_installer_execute_layout(
    struct production_installer *installer,
    struct block_device *device,
    struct installer_layout_plan *plan,
    struct installer_layout_snapshot *snapshot);
enum production_installer_status production_installer_verify_layout(
    struct production_installer *installer,
    struct block_device *device,
    const struct installer_layout_plan *plan);
const char *production_installer_stage_string(
    enum production_installer_stage stage);
const char *production_installer_mode_string(
    enum production_installer_mode mode);
const char *production_installer_status_string(
    enum production_installer_status status);

#endif
