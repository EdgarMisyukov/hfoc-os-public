#include "production_installer.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "installer_layout.h"

/*
 * Worst case of the ITEM-396 preservation fragment the preview appends: its
 * fixed text plus six decimal fields of at most 20 digits each, with room for
 * the terminator.
 */
#define PRODUCTION_INSTALLER_PRESERVE_FRAGMENT_MAX 192U

static bool production_installer_mode_valid(
    enum production_installer_mode mode)
{
    return mode >= PRODUCTION_INSTALLER_MODE_CLEAN_WHOLE_DISK &&
           mode <= PRODUCTION_INSTALLER_MODE_DUAL_BOOT;
}

static bool production_installer_report_valid(
    const struct installer_discovery_report *report)
{
    return report != NULL &&
           report->version == INSTALLER_HARDWARE_DISCOVERY_VERSION &&
           (report->firmware_mode == INSTALLER_FIRMWARE_UEFI ||
            report->firmware_mode == INSTALLER_FIRMWARE_BIOS_LEGACY) &&
           report->read_only &&
           report->scan_complete;
}

static bool production_installer_identity_valid(
    const char *identity,
    size_t identity_length)
{
    size_t index;

    if (identity == NULL ||
        identity_length == 0U ||
        identity_length >= PRODUCTION_INSTALLER_IDENTITY_LENGTH) {
        return false;
    }
    for (index = 0U; index < identity_length; ++index) {
        unsigned char value = (unsigned char)identity[index];

        if (value < 0x20U || value > 0x7EU) {
            return false;
        }
    }
    return true;
}

static bool production_installer_disk_valid(
    const struct installer_discovery_disk *disk)
{
    return disk != NULL &&
           disk->present &&
           disk->id != 0U &&
           disk->block_size == INSTALLER_LAYOUT_BLOCK_SIZE &&
           disk->block_count > 34U &&
           disk->block_count <= BLOCK_DEVICE_BLOCK_COUNT_MAX;
}

static size_t production_installer_identity_length(
    const char identity[INSTALLER_DISCOVERY_IDENTITY_LENGTH])
{
    size_t length = 0U;

    while (length < INSTALLER_DISCOVERY_IDENTITY_LENGTH &&
           identity[length] != '\0') {
        ++length;
    }
    return length;
}

static size_t production_installer_stored_identity_length(
    const char identity[PRODUCTION_INSTALLER_IDENTITY_LENGTH])
{
    size_t length = 0U;

    while (length < PRODUCTION_INSTALLER_IDENTITY_LENGTH &&
           identity[length] != '\0') {
        ++length;
    }
    return length;
}

static bool production_installer_identity_matches_disk(
    const struct installer_discovery_disk *disk,
    const char *identity,
    size_t identity_length)
{
    if (identity == NULL) {
        return false;
    }
    return production_installer_identity_length(
               disk->identity) == identity_length &&
           memcmp(disk->identity, identity, identity_length) == 0;
}

static void production_installer_identity_copy(
    char destination[PRODUCTION_INSTALLER_IDENTITY_LENGTH],
    const char *identity,
    size_t identity_length)
{
    size_t index;

    for (index = 0U;
         index < PRODUCTION_INSTALLER_IDENTITY_LENGTH;
         ++index) {
        destination[index] = '\0';
    }
    if (identity == NULL || identity_length == 0U) {
        return;
    }
    for (index = 0U;
         index + 1U < PRODUCTION_INSTALLER_IDENTITY_LENGTH &&
         index < identity_length;
         ++index) {
        destination[index] = identity[index];
    }
}

static const struct installer_discovery_disk *
production_installer_find_disk(
    const struct installer_discovery_report *report,
    uint32_t disk_id)
{
    size_t index;

    if (!production_installer_report_valid(report)) {
        return NULL;
    }
    for (index = 0U;
         index < INSTALLER_DISCOVERY_DISK_CAPACITY;
         ++index) {
        if (report->disks[index].present &&
            report->disks[index].id == disk_id) {
            return &report->disks[index];
        }
    }
    return NULL;
}

static bool production_installer_source_valid(
    const struct production_installer_source *source)
{
    return source != NULL &&
           source->version == PRODUCTION_INSTALLER_SOURCE_VERSION &&
           source->boot_payload != NULL &&
           source->boot_payload_size != 0U &&
           source->boot_payload_size <=
               PRODUCTION_INSTALLER_BOOT_PAYLOAD_MAX &&
           source->limine_config != NULL &&
           source->limine_config_size != 0U &&
           source->kernel != NULL &&
           source->kernel_size != 0U;
}

static const char *production_installer_mode_text(
    enum production_installer_mode mode)
{
    switch (mode) {
    case PRODUCTION_INSTALLER_MODE_CLEAN_WHOLE_DISK:
        return "clean-whole-disk";
    case PRODUCTION_INSTALLER_MODE_CLEAN_MANUAL:
        return "clean-manual";
    case PRODUCTION_INSTALLER_MODE_DUAL_BOOT:
        return "dual-boot";
    default:
        return "unknown";
    }
}

static void production_installer_store_decimal(
    char *buffer,
    size_t capacity,
    size_t *length,
    uint32_t value)
{
    char digits[16U];
    size_t digit_count = 0U;

    do {
        digits[digit_count++] =
            (char)('0' + (value % 10U));
        value /= 10U;
    } while (value != 0U && digit_count < sizeof(digits));

    while (digit_count != 0U) {
        if (*length + 1U < capacity) {
            buffer[(*length)++] = digits[--digit_count];
        } else {
            --digit_count;
        }
    }
}

static void production_installer_append_text(
    char *buffer,
    size_t capacity,
    size_t *length,
    const char *text)
{
    size_t index;

    for (index = 0U; text[index] != '\0'; ++index) {
        if (*length + 1U < capacity) {
            buffer[(*length)++] = text[index];
        }
    }
}

static enum production_installer_status
production_installer_map_layout_status(
    enum installer_layout_status status)
{
    switch (status) {
    case INSTALLER_LAYOUT_OK:
        return PRODUCTION_INSTALLER_OK;
    case INSTALLER_LAYOUT_NULL:
        return PRODUCTION_INSTALLER_NULL;
    case INSTALLER_LAYOUT_BAD_VERSION:
    case INSTALLER_LAYOUT_BAD_STATE:
    case INSTALLER_LAYOUT_NOT_CONFIRMED:
    case INSTALLER_LAYOUT_ALREADY_EXECUTED:
        return PRODUCTION_INSTALLER_BAD_STATE;
    case INSTALLER_LAYOUT_INVALID_ARGUMENT:
    case INSTALLER_LAYOUT_INVALID_BLOCK_DEVICE:
    case INSTALLER_LAYOUT_READ_ONLY_VIOLATION:
        return PRODUCTION_INSTALLER_INVALID_ARGUMENT;
    case INSTALLER_LAYOUT_OUT_OF_RANGE:
    case INSTALLER_LAYOUT_AMBIGUOUS:
    case INSTALLER_LAYOUT_CAPACITY:
        return PRODUCTION_INSTALLER_CAPACITY;
    case INSTALLER_LAYOUT_IO_ERROR:
        return PRODUCTION_INSTALLER_IO_ERROR;
    default:
        return PRODUCTION_INSTALLER_BAD_STATE;
    }
}

enum production_installer_status production_installer_init(
    struct production_installer *installer)
{
    if (installer == NULL) {
        return PRODUCTION_INSTALLER_NULL;
    }
    memset(installer, 0, sizeof(*installer));
    installer->version = PRODUCTION_INSTALLER_VERSION;
    installer->stage = PRODUCTION_INSTALLER_STAGE_UNPREPARED;
    return PRODUCTION_INSTALLER_OK;
}

enum production_installer_status production_installer_prepare(
    struct production_installer *installer,
    const struct installer_discovery_report *report,
    enum production_installer_mode mode,
    uint32_t disk_id,
    const char *identity,
    size_t identity_length,
    const struct production_installer_source *source,
    bool efi_ready)
{
    const struct installer_discovery_disk *disk;

    if (installer == NULL || report == NULL) {
        return PRODUCTION_INSTALLER_NULL;
    }
    if (installer->version != PRODUCTION_INSTALLER_VERSION ||
        installer->stage != PRODUCTION_INSTALLER_STAGE_UNPREPARED) {
        return PRODUCTION_INSTALLER_BAD_STATE;
    }
    if (!production_installer_mode_valid(mode)) {
        return PRODUCTION_INSTALLER_INVALID_ARGUMENT;
    }
    if (disk_id == 0U ||
        !production_installer_identity_valid(
            identity, identity_length)) {
        return PRODUCTION_INSTALLER_INVALID_ARGUMENT;
    }
    if (!production_installer_source_valid(source)) {
        return PRODUCTION_INSTALLER_SOURCE_UNAVAILABLE;
    }
    if (!efi_ready) {
        return PRODUCTION_INSTALLER_EFI_UNAVAILABLE;
    }
    disk = production_installer_find_disk(report, disk_id);
    if (disk == NULL || !production_installer_disk_valid(disk)) {
        return PRODUCTION_INSTALLER_UNSAFE_EVIDENCE;
    }
    if (!production_installer_identity_matches_disk(
            disk, identity, identity_length)) {
        return PRODUCTION_INSTALLER_UNSAFE_EVIDENCE;
    }

    installer->mode = mode;
    installer->disk_id = disk_id;
    installer->source = *source;
    installer->source_ready = true;
    installer->efi_ready = true;
    installer->confirmed = false;
    production_installer_identity_copy(
        installer->identity, identity, identity_length);
    installer->stage =
        PRODUCTION_INSTALLER_STAGE_EVIDENCE_VALIDATED;
    return PRODUCTION_INSTALLER_OK;
}

enum production_installer_status production_installer_verify_media(
    struct production_installer *installer,
    const struct verification_policy *policy,
    const struct installer_media_verifier *verifier,
    uint32_t expected_release_id,
    const void *envelope,
    size_t envelope_length,
    const void *media,
    size_t media_length)
{
    struct installer_media_verification_report report;
    struct installer_media_source media_source;
    enum installer_media_verification_status media_status;

    if (installer == NULL) {
        return PRODUCTION_INSTALLER_NULL;
    }
    if (installer->version != PRODUCTION_INSTALLER_VERSION) {
        return PRODUCTION_INSTALLER_BAD_VERSION;
    }
    if (installer->stage !=
        PRODUCTION_INSTALLER_STAGE_EVIDENCE_VALIDATED) {
        return PRODUCTION_INSTALLER_BAD_STATE;
    }
    memset(&report, 0, sizeof(report));
    memset(&media_source, 0, sizeof(media_source));
    media_source.boot_payload = installer->source.boot_payload;
    media_source.boot_payload_size =
        installer->source.boot_payload_size;
    media_source.limine_config = installer->source.limine_config;
    media_source.limine_config_size =
        installer->source.limine_config_size;
    media_source.kernel = installer->source.kernel;
    media_source.kernel_size = installer->source.kernel_size;
    media_status = installer_media_verification_run(
        policy, verifier, expected_release_id, envelope,
        envelope_length, media, media_length, &media_source, &report);
    installer->media_status = media_status;
    installer->media_verified = false;
    if (media_status != INSTALLER_MEDIA_OK ||
        !installer_media_verification_authorizes_write(&report)) {
        return PRODUCTION_INSTALLER_MEDIA_UNVERIFIED;
    }
    installer->media_verified = true;
    installer->media_release_id = report.release_id;
    installer->media_signer_id = report.signer_id;
    memcpy(installer->source_digest, report.source_digest,
           sizeof(installer->source_digest));
    installer->source_length = report.source_length;
    installer->source_digest_ready = true;
    return PRODUCTION_INSTALLER_OK;
}

enum production_installer_status production_installer_verify_hardware(
    struct production_installer *installer,
    const struct installer_discovery_report *report,
    const struct installer_supported_hardware_policy *policy)
{
    struct installer_supported_hardware_verdict verdict;
    enum installer_supported_hardware_status hardware_status;

    if (installer == NULL) {
        return PRODUCTION_INSTALLER_NULL;
    }
    if (installer->version != PRODUCTION_INSTALLER_VERSION) {
        return PRODUCTION_INSTALLER_BAD_VERSION;
    }
    if (installer->stage !=
        PRODUCTION_INSTALLER_STAGE_EVIDENCE_VALIDATED) {
        return PRODUCTION_INSTALLER_BAD_STATE;
    }
    memset(&verdict, 0, sizeof(verdict));
    verdict.version = INSTALLER_SUPPORTED_HARDWARE_VERSION;
    hardware_status = installer_supported_hardware_evaluate(
        report, installer->disk_id, policy, &verdict);
    installer->hardware_supported = false;
    installer->hardware_reason = verdict.reason;
    if (hardware_status != INSTALLER_SUPPORTED_HARDWARE_OK ||
        !installer_supported_hardware_verdict_supported(&verdict)) {
        if (verdict.reason == INSTALLER_HARDWARE_REASON_SUPPORTED) {
            installer->hardware_reason =
                INSTALLER_HARDWARE_REASON_INVALID_REPORT;
        }
        return PRODUCTION_INSTALLER_UNSUPPORTED_HARDWARE;
    }
    installer->hardware_supported = true;
    return PRODUCTION_INSTALLER_OK;
}

enum production_installer_status production_installer_build_plan(
    struct production_installer *installer)
{
    if (installer == NULL) {
        return PRODUCTION_INSTALLER_NULL;
    }
    if (installer->version != PRODUCTION_INSTALLER_VERSION ||
        installer->stage !=
            PRODUCTION_INSTALLER_STAGE_EVIDENCE_VALIDATED) {
        return PRODUCTION_INSTALLER_BAD_STATE;
    }
    if (!installer->source_ready || !installer->efi_ready) {
        return PRODUCTION_INSTALLER_BAD_STATE;
    }
    if (!installer->media_verified) {
        return PRODUCTION_INSTALLER_MEDIA_UNVERIFIED;
    }
    if (!installer->hardware_supported) {
        return PRODUCTION_INSTALLER_UNSUPPORTED_HARDWARE;
    }
    installer->stage = PRODUCTION_INSTALLER_STAGE_PLAN_BUILT;
    return PRODUCTION_INSTALLER_OK;
}

/*
 * ITEM-391: the production install plan is the declared product layout, not a
 * caller-supplied range set.  The controller re-checks the evidence it was
 * prepared with, then derives the plan from the product constants through the
 * ITEM-340 planner (clean install) or the ITEM-341 planner (dual boot).  No
 * second planner is added here, and the summary it stores is what the preview
 * and the serial record publish.
 */
static const struct installer_discovery_disk *
production_installer_target_disk(
    const struct production_installer *installer,
    const struct installer_discovery_report *report)
{
    const struct installer_discovery_disk *disk;
    size_t identity_length;

    if (!production_installer_report_valid(report)) {
        return NULL;
    }
    disk = production_installer_find_disk(report, installer->disk_id);
    if (disk == NULL || !production_installer_disk_valid(disk)) {
        return NULL;
    }
    identity_length = production_installer_stored_identity_length(
        installer->identity);
    if (identity_length == 0U ||
        !production_installer_identity_matches_disk(
            disk, installer->identity, identity_length)) {
        return NULL;
    }
    return disk;
}

static enum production_installer_status
production_installer_map_contract_status(
    enum installer_layout_contract_status status,
    enum installer_layout_status layout_status)
{
    switch (status) {
    case INSTALLER_LAYOUT_CONTRACT_OK:
        return PRODUCTION_INSTALLER_OK;
    case INSTALLER_LAYOUT_CONTRACT_NULL:
        return PRODUCTION_INSTALLER_NULL;
    case INSTALLER_LAYOUT_CONTRACT_BAD_VERSION:
    case INSTALLER_LAYOUT_CONTRACT_BAD_MODE:
        return PRODUCTION_INSTALLER_BAD_STATE;
    case INSTALLER_LAYOUT_CONTRACT_INVALID_ARGUMENT:
        return PRODUCTION_INSTALLER_INVALID_ARGUMENT;
    case INSTALLER_LAYOUT_CONTRACT_MEDIA_TOO_SMALL:
    case INSTALLER_LAYOUT_CONTRACT_NO_RESERVE:
        return PRODUCTION_INSTALLER_MEDIA_TOO_SMALL;
    case INSTALLER_LAYOUT_CONTRACT_EXISTING_SYSTEM_OVERLAP:
        return PRODUCTION_INSTALLER_EXISTING_SYSTEM_CONFLICT;
    case INSTALLER_LAYOUT_CONTRACT_PLAN_REJECTED:
        return production_installer_map_layout_status(layout_status);
    default:
        return PRODUCTION_INSTALLER_INVALID_ARGUMENT;
    }
}

enum production_installer_status production_installer_plan_layout(
    struct production_installer *installer,
    const struct installer_discovery_report *report,
    struct installer_layout_plan *plan,
    struct installer_dual_boot_plan *dual_boot_plan)
{
    enum installer_layout_contract_status contract_status;
    enum production_installer_status status;

    if (installer == NULL || report == NULL || plan == NULL) {
        return PRODUCTION_INSTALLER_NULL;
    }
    if (installer->version != PRODUCTION_INSTALLER_VERSION) {
        return PRODUCTION_INSTALLER_BAD_VERSION;
    }
    if (installer->stage != PRODUCTION_INSTALLER_STAGE_PLAN_BUILT ||
        installer->layout_ready) {
        return PRODUCTION_INSTALLER_BAD_STATE;
    }
    if (!installer->media_verified) {
        return PRODUCTION_INSTALLER_MEDIA_UNVERIFIED;
    }
    if (!installer->hardware_supported) {
        return PRODUCTION_INSTALLER_UNSUPPORTED_HARDWARE;
    }
    if (production_installer_target_disk(installer, report) == NULL) {
        return PRODUCTION_INSTALLER_UNSAFE_EVIDENCE;
    }
    if (installer->mode == PRODUCTION_INSTALLER_MODE_CLEAN_WHOLE_DISK) {
        /*
         * ITEM-517: the firmware mode selects the whole-disk planner.  A
         * legacy-BIOS machine boots through a BIOS boot partition plus a FAT
         * boot volume and has no ESP to require; a UEFI machine keeps the
         * declared ESP layout byte for byte.
         */
        contract_status =
            report->firmware_mode == INSTALLER_FIRMWARE_BIOS_LEGACY
                ? installer_layout_contract_plan_clean_bios(
                      report, installer->disk_id, plan, &installer->layout)
                : installer_layout_contract_plan_clean(
                      report, installer->disk_id, plan, &installer->layout);
        if (contract_status != INSTALLER_LAYOUT_CONTRACT_OK) {
            status = production_installer_map_contract_status(
                contract_status, installer->layout.layout_status);
            memset(&installer->layout, 0, sizeof(installer->layout));
            memset(plan, 0, sizeof(*plan));
            return status;
        }
    } else if (installer->mode == PRODUCTION_INSTALLER_MODE_DUAL_BOOT) {
        if (dual_boot_plan == NULL) {
            return PRODUCTION_INSTALLER_NULL;
        }
        /*
         * ITEM-517: the preserve/release contract reuses the previous
         * system's ESP (`installer_dual_boot` is UEFI-only).  A legacy-BIOS
         * machine has no such chain, so this install is refused by name
         * before any plan is built; `installer_dual_boot.c` is not widened.
         */
        if (report->firmware_mode == INSTALLER_FIRMWARE_BIOS_LEGACY) {
            memset(&installer->layout, 0, sizeof(installer->layout));
            memset(plan, 0, sizeof(*plan));
            memset(dual_boot_plan, 0, sizeof(*dual_boot_plan));
            return PRODUCTION_INSTALLER_BIOS_PRESERVE_UNSUPPORTED;
        }
        /*
         * ITEM-501: the same preserving contract, with the appliance's roles
         * placed in the extent of one existing partition the operator named
         * instead of in free space.  The summary and the executable plan both
         * carry the partition, and the writer removes exactly that entry.
         */
        contract_status = installer->release_partition
            ? installer_layout_contract_plan_release(
                  report, installer->disk_id,
                  installer->release_partition_index, dual_boot_plan,
                  &installer->layout)
            : installer_layout_contract_plan_dual_boot(
                  report, installer->disk_id, dual_boot_plan,
                  &installer->layout);
        if (contract_status == INSTALLER_LAYOUT_CONTRACT_OK) {
            /*
             * ITEM-396: the dual-boot contract sizes the layout and publishes
             * the numbers, but it does not hand back an executable plan - the
             * ESP belongs to the previous system and is reused, so only the
             * three added roles are the appliance's to write.  This is that
             * plan: same identity and disk as the clean plan, no partition for
             * the reused ESP, and not destructive, because the writer that
             * executes it (`production_installer_execute_layout_preserving()`)
             * keeps every existing entry.
             */
            const struct installer_discovery_disk *disk =
                production_installer_target_disk(installer, report);
            const struct installer_layout_contract_role *roles =
                installer->layout.roles;

            memset(plan, 0, sizeof(*plan));
            plan->version = INSTALLER_LAYOUT_VERSION;
            plan->mode = INSTALLER_LAYOUT_MODE_WHOLE_DISK;
            plan->disk_id = installer->disk_id;
            plan->firmware_mode = report->firmware_mode;
            plan->block_size = disk->block_size;
            plan->block_count = disk->block_count;
            plan->existing_partition_count =
                installer->layout.existing_partition_count;
            plan->destructive = false;
            plan->partition_count = 3U;
            /*
             * ITEM-501: a release plan names the existing entry the writer is
             * to remove; a plain preserve plan leaves the fields zero and every
             * foreign entry untouched.
             */
            plan->releases_partition = dual_boot_plan->releases_partition;
            plan->released_index = dual_boot_plan->released_index;
            plan->released_first_lba =
                dual_boot_plan->released_first_lba;
            plan->released_length_lba =
                dual_boot_plan->released_length_lba;
            production_installer_identity_copy(
                plan->identity, installer->identity,
                production_installer_stored_identity_length(
                    installer->identity));
            plan->partitions[0U].present = true;
            plan->partitions[0U].role =
                INSTALLER_LAYOUT_ROLE_HFOC_SYSTEM;
            plan->partitions[0U].partition_index = 1U;
            plan->partitions[0U].first_lba = roles[1U].first_lba;
            plan->partitions[0U].length_lba = roles[1U].length_lba;
            plan->partitions[1U].present = true;
            plan->partitions[1U].role =
                INSTALLER_LAYOUT_ROLE_HFOC_PERSISTENCE;
            plan->partitions[1U].partition_index = 2U;
            plan->partitions[1U].first_lba = roles[2U].first_lba;
            plan->partitions[1U].length_lba = roles[2U].length_lba;
            plan->partitions[2U].present = true;
            plan->partitions[2U].role =
                INSTALLER_LAYOUT_ROLE_HFOC_RECOVERY;
            plan->partitions[2U].partition_index = 3U;
            plan->partitions[2U].first_lba = roles[3U].first_lba;
            plan->partitions[2U].length_lba = roles[3U].length_lba;
        }
        if (contract_status != INSTALLER_LAYOUT_CONTRACT_OK) {
            status = production_installer_map_contract_status(
                contract_status, installer->layout.layout_status);
            memset(&installer->layout, 0, sizeof(installer->layout));
            memset(plan, 0, sizeof(*plan));
            memset(dual_boot_plan, 0, sizeof(*dual_boot_plan));
            /* A dual-boot target that is not a dual-boot candidate is a
             * state refusal, not a size refusal. */
            if (contract_status ==
                INSTALLER_LAYOUT_CONTRACT_PLAN_REJECTED) {
                return PRODUCTION_INSTALLER_BAD_STATE;
            }
            return status;
        }
    } else {
        /* Manual ranges are caller-owned by definition, so the product
         * layout contract does not apply to this mode. */
        return PRODUCTION_INSTALLER_INVALID_ARGUMENT;
    }
    installer->layout_ready = true;
    return PRODUCTION_INSTALLER_OK;
}

enum production_installer_status production_installer_publish_preview(
    struct production_installer *installer)
{
    char fragment[INSTALLER_LAYOUT_CONTRACT_PREVIEW_LENGTH];
    size_t fragment_length = 0U;
    size_t length = 0U;

    if (installer == NULL) {
        return PRODUCTION_INSTALLER_NULL;
    }
    if (installer->version != PRODUCTION_INSTALLER_VERSION ||
        installer->stage != PRODUCTION_INSTALLER_STAGE_PLAN_BUILT) {
        return PRODUCTION_INSTALLER_BAD_STATE;
    }
    memset(installer->preview, 0, sizeof(installer->preview));
    production_installer_append_text(
        installer->preview, sizeof(installer->preview), &length,
        "HFOC INSTALL preview mode=");
    production_installer_append_text(
        installer->preview, sizeof(installer->preview), &length,
        production_installer_mode_text(installer->mode));
    production_installer_append_text(
        installer->preview, sizeof(installer->preview), &length,
        " disk=");
    production_installer_store_decimal(
        installer->preview, sizeof(installer->preview), &length,
        installer->disk_id);
    production_installer_append_text(
        installer->preview, sizeof(installer->preview), &length,
        " source=ready efi=ready");
    production_installer_append_text(
        installer->preview, sizeof(installer->preview), &length,
        installer->media_verified ? " media=verified"
                                  : " media=unverified");
    production_installer_append_text(
        installer->preview, sizeof(installer->preview), &length,
        installer->hardware_supported ? " hardware=supported"
                                      : " hardware=unsupported");
    /*
     * ITEM-391: the preview carries the contracted sizes it will write, so the
     * operator confirms the installed footprint and not just the mode.
     */
    if (installer->layout_ready) {
        if (installer_layout_contract_render_preview(
                &installer->layout, fragment, sizeof(fragment),
                &fragment_length) != INSTALLER_LAYOUT_CONTRACT_OK ||
            fragment_length == 0U ||
            fragment_length + length + 1U >
                sizeof(installer->preview)) {
            return PRODUCTION_INSTALLER_VERIFICATION_FAILED;
        }
        production_installer_append_text(
            installer->preview, sizeof(installer->preview), &length,
            fragment);
    }
    /*
     * ITEM-396: a dual-boot preview also states what is preserved and what is
     * added, so the operator confirms the preservation and not only the mode.
     * The fragment is bounded by its fixed text plus six decimal fields, so the
     * worst case is checked before it is written and nothing is truncated.  The
     * menu itself is published by
     * `production_installer_publish_preserve_record()`.
     */
    if (installer->preserve_ready) {
        if (length + PRODUCTION_INSTALLER_PRESERVE_FRAGMENT_MAX >=
            sizeof(installer->preview)) {
            return PRODUCTION_INSTALLER_VERIFICATION_FAILED;
        }
        production_installer_append_text(
            installer->preview, sizeof(installer->preview), &length,
            " preserve preserved=");
        production_installer_store_decimal(
            installer->preview, sizeof(installer->preview), &length,
            installer->preserve.preserved_count);
        production_installer_append_text(
            installer->preview, sizeof(installer->preview), &length,
            " added=");
        production_installer_store_decimal(
            installer->preview, sizeof(installer->preview), &length,
            installer->preserve.added_count);
        production_installer_append_text(
            installer->preview, sizeof(installer->preview), &length,
            " esp_reused=");
        production_installer_append_text(
            installer->preview, sizeof(installer->preview), &length,
            installer->preserve.esp_reused ? "1" : "0");
        production_installer_append_text(
            installer->preview, sizeof(installer->preview), &length,
            " free_first_lba=");
        production_installer_store_decimal(
            installer->preview, sizeof(installer->preview), &length,
            (uint32_t)installer->preserve.free_first_lba);
        production_installer_append_text(
            installer->preview, sizeof(installer->preview), &length,
            " free_lba=");
        production_installer_store_decimal(
            installer->preview, sizeof(installer->preview), &length,
            (uint32_t)installer->preserve.free_length_lba);
        production_installer_append_text(
            installer->preview, sizeof(installer->preview), &length,
            " menu_entries=");
        production_installer_store_decimal(
            installer->preview, sizeof(installer->preview), &length,
            installer->preserve.menu_entry_count);
        production_installer_append_text(
            installer->preview, sizeof(installer->preview), &length,
            " menu_missing=");
        production_installer_store_decimal(
            installer->preview, sizeof(installer->preview), &length,
            installer->preserve.menu_missing_count);
        production_installer_append_text(
            installer->preview, sizeof(installer->preview), &length,
            " unreadable_disks=");
        production_installer_store_decimal(
            installer->preview, sizeof(installer->preview), &length,
            installer->preserve.unreadable_disk_count);
    }
    installer->stage =
        PRODUCTION_INSTALLER_STAGE_PREVIEW_PUBLISHED;
    return PRODUCTION_INSTALLER_OK;
}

enum production_installer_status production_installer_confirm(
    struct production_installer *installer)
{
    if (installer == NULL) {
        return PRODUCTION_INSTALLER_NULL;
    }
    if (installer->version != PRODUCTION_INSTALLER_VERSION) {
        return PRODUCTION_INSTALLER_BAD_VERSION;
    }
    if (installer->confirmed) {
        return PRODUCTION_INSTALLER_ALREADY_CONFIRMED;
    }
    if (installer->stage !=
        PRODUCTION_INSTALLER_STAGE_PREVIEW_PUBLISHED) {
        return PRODUCTION_INSTALLER_BAD_STATE;
    }
    installer->confirmed = true;
    installer->stage = PRODUCTION_INSTALLER_STAGE_CONFIRMED;
    return PRODUCTION_INSTALLER_OK;
}

enum production_installer_status production_installer_reset(
    struct production_installer *installer)
{
    if (installer == NULL) {
        return PRODUCTION_INSTALLER_NULL;
    }
    if (installer->version != PRODUCTION_INSTALLER_VERSION) {
        return PRODUCTION_INSTALLER_BAD_VERSION;
    }
    memset(installer, 0, sizeof(*installer));
    installer->version = PRODUCTION_INSTALLER_VERSION;
    installer->stage = PRODUCTION_INSTALLER_STAGE_UNPREPARED;
    return PRODUCTION_INSTALLER_OK;
}

enum production_installer_status production_installer_execute_layout(
    struct production_installer *installer,
    struct block_device *device,
    struct installer_layout_plan *plan,
    struct installer_layout_snapshot *snapshot)
{
    enum installer_layout_status layout_status;

    if (installer == NULL || device == NULL ||
        plan == NULL || snapshot == NULL) {
        return PRODUCTION_INSTALLER_NULL;
    }
    if (installer->version != PRODUCTION_INSTALLER_VERSION) {
        return PRODUCTION_INSTALLER_BAD_VERSION;
    }
    if (installer->stage !=
            PRODUCTION_INSTALLER_STAGE_CONFIRMED ||
        !installer->confirmed) {
        return PRODUCTION_INSTALLER_BAD_STATE;
    }
    if (!installer->media_verified) {
        return PRODUCTION_INSTALLER_MEDIA_UNVERIFIED;
    }
    if (!installer->hardware_supported) {
        return PRODUCTION_INSTALLER_UNSUPPORTED_HARDWARE;
    }
    if (plan->version != INSTALLER_LAYOUT_VERSION ||
        !plan->confirmed || plan->executed ||
        plan->disk_id != installer->disk_id ||
        strcmp(plan->identity, installer->identity) != 0 ||
        block_device_validate(device) != BLOCK_DEVICE_OK ||
        device->block_size != plan->block_size ||
        device->block_count != plan->block_count) {
        return PRODUCTION_INSTALLER_INVALID_ARGUMENT;
    }

    installer->stage = PRODUCTION_INSTALLER_STAGE_EXECUTING;
    layout_status = installer_layout_execute(device, plan, snapshot);
    if (layout_status != INSTALLER_LAYOUT_OK) {
        (void)installer_layout_rollback(device, snapshot);
        installer->stage = PRODUCTION_INSTALLER_STAGE_FAILED;
        return production_installer_map_layout_status(layout_status);
    }
    return PRODUCTION_INSTALLER_OK;
}

enum production_installer_status production_installer_verify_layout(
    struct production_installer *installer,
    struct block_device *device,
    const struct installer_layout_plan *plan)
{
    if (installer == NULL || device == NULL || plan == NULL) {
        return PRODUCTION_INSTALLER_NULL;
    }
    if (installer->version != PRODUCTION_INSTALLER_VERSION ||
        installer->stage !=
            PRODUCTION_INSTALLER_STAGE_EXECUTING ||
        !installer->confirmed) {
        return PRODUCTION_INSTALLER_BAD_STATE;
    }
    /*
     * ITEM-502: the layout is only verified after the state it will carry has
     * been decided and proved: either erased and read back clean, or - ITEM-586
     * - explicitly preserved after the preflight loaded it in the range the
     * plan writes.  A run that did neither cannot advance to the verified
     * stage and cannot publish `applied`.
     */
    if (!installer->state_erased && !installer->state_preserved) {
        return PRODUCTION_INSTALLER_BAD_STATE;
    }
    if (plan->version != INSTALLER_LAYOUT_VERSION ||
        !plan->confirmed || !plan->executed ||
        plan->disk_id != installer->disk_id ||
        block_device_validate(device) != BLOCK_DEVICE_OK ||
        device->block_size != plan->block_size ||
        device->block_count != plan->block_count) {
        return PRODUCTION_INSTALLER_VERIFICATION_FAILED;
    }
    installer->stage = PRODUCTION_INSTALLER_STAGE_VERIFIED;
    return PRODUCTION_INSTALLER_OK;
}

const char *production_installer_stage_string(
    enum production_installer_stage stage)
{
    switch (stage) {
    case PRODUCTION_INSTALLER_STAGE_UNPREPARED:
        return "PRODUCTION_INSTALLER_STAGE_UNPREPARED";
    case PRODUCTION_INSTALLER_STAGE_EVIDENCE_VALIDATED:
        return "PRODUCTION_INSTALLER_STAGE_EVIDENCE_VALIDATED";
    case PRODUCTION_INSTALLER_STAGE_PLAN_BUILT:
        return "PRODUCTION_INSTALLER_STAGE_PLAN_BUILT";
    case PRODUCTION_INSTALLER_STAGE_PREVIEW_PUBLISHED:
        return "PRODUCTION_INSTALLER_STAGE_PREVIEW_PUBLISHED";
    case PRODUCTION_INSTALLER_STAGE_CONFIRMED:
        return "PRODUCTION_INSTALLER_STAGE_CONFIRMED";
    case PRODUCTION_INSTALLER_STAGE_EXECUTING:
        return "PRODUCTION_INSTALLER_STAGE_EXECUTING";
    case PRODUCTION_INSTALLER_STAGE_VERIFIED:
        return "PRODUCTION_INSTALLER_STAGE_VERIFIED";
    case PRODUCTION_INSTALLER_STAGE_FAILED:
        return "PRODUCTION_INSTALLER_STAGE_FAILED";
    default:
        return NULL;
    }
}

const char *production_installer_mode_string(
    enum production_installer_mode mode)
{
    switch (mode) {
    case PRODUCTION_INSTALLER_MODE_CLEAN_WHOLE_DISK:
        return "PRODUCTION_INSTALLER_MODE_CLEAN_WHOLE_DISK";
    case PRODUCTION_INSTALLER_MODE_CLEAN_MANUAL:
        return "PRODUCTION_INSTALLER_MODE_CLEAN_MANUAL";
    case PRODUCTION_INSTALLER_MODE_DUAL_BOOT:
        return "PRODUCTION_INSTALLER_MODE_DUAL_BOOT";
    default:
        return NULL;
    }
}

const char *production_installer_status_string(
    enum production_installer_status status)
{
    switch (status) {
    case PRODUCTION_INSTALLER_OK:
        return "PRODUCTION_INSTALLER_OK";
    case PRODUCTION_INSTALLER_NULL:
        return "PRODUCTION_INSTALLER_NULL";
    case PRODUCTION_INSTALLER_BAD_STATE:
        return "PRODUCTION_INSTALLER_BAD_STATE";
    case PRODUCTION_INSTALLER_BAD_VERSION:
        return "PRODUCTION_INSTALLER_BAD_VERSION";
    case PRODUCTION_INSTALLER_INVALID_ARGUMENT:
        return "PRODUCTION_INSTALLER_INVALID_ARGUMENT";
    case PRODUCTION_INSTALLER_UNSAFE_EVIDENCE:
        return "PRODUCTION_INSTALLER_UNSAFE_EVIDENCE";
    case PRODUCTION_INSTALLER_SOURCE_UNAVAILABLE:
        return "PRODUCTION_INSTALLER_SOURCE_UNAVAILABLE";
    case PRODUCTION_INSTALLER_EFI_UNAVAILABLE:
        return "PRODUCTION_INSTALLER_EFI_UNAVAILABLE";
    case PRODUCTION_INSTALLER_NOT_CONFIRMED:
        return "PRODUCTION_INSTALLER_NOT_CONFIRMED";
    case PRODUCTION_INSTALLER_ALREADY_CONFIRMED:
        return "PRODUCTION_INSTALLER_ALREADY_CONFIRMED";
    case PRODUCTION_INSTALLER_CAPACITY:
        return "PRODUCTION_INSTALLER_CAPACITY";
    case PRODUCTION_INSTALLER_IO_ERROR:
        return "PRODUCTION_INSTALLER_IO_ERROR";
    case PRODUCTION_INSTALLER_VERIFICATION_FAILED:
        return "PRODUCTION_INSTALLER_VERIFICATION_FAILED";
    case PRODUCTION_INSTALLER_MEDIA_UNVERIFIED:
        return "PRODUCTION_INSTALLER_MEDIA_UNVERIFIED";
    case PRODUCTION_INSTALLER_UNSUPPORTED_HARDWARE:
        return "PRODUCTION_INSTALLER_UNSUPPORTED_HARDWARE";
    case PRODUCTION_INSTALLER_MEDIA_TOO_SMALL:
        return "PRODUCTION_INSTALLER_MEDIA_TOO_SMALL";
    case PRODUCTION_INSTALLER_EXISTING_SYSTEM_CONFLICT:
        return "PRODUCTION_INSTALLER_EXISTING_SYSTEM_CONFLICT";
    case PRODUCTION_INSTALLER_ESP_TOO_SMALL:
        return "PRODUCTION_INSTALLER_ESP_TOO_SMALL";
    case PRODUCTION_INSTALLER_BOOT_CONFIG_INVALID:
        return "PRODUCTION_INSTALLER_BOOT_CONFIG_INVALID";
    /*
     * ITEM-396 refusals.  Each one names what could not be preserved, so the
     * acceptance record says which of them the install hit.
     */
    case PRODUCTION_INSTALLER_TABLE_UNREADABLE:
        return "PRODUCTION_INSTALLER_TABLE_UNREADABLE";
    case PRODUCTION_INSTALLER_TABLE_CORRUPT:
        return "PRODUCTION_INSTALLER_TABLE_CORRUPT";
    case PRODUCTION_INSTALLER_NO_FREE_SPACE:
        return "PRODUCTION_INSTALLER_NO_FREE_SPACE";
    case PRODUCTION_INSTALLER_ESP_UNSUITABLE:
        return "PRODUCTION_INSTALLER_ESP_UNSUITABLE";
    case PRODUCTION_INSTALLER_MENU_UNAVAILABLE:
        return "PRODUCTION_INSTALLER_MENU_UNAVAILABLE";
    /*
     * ITEM-517: the legacy-BIOS boot chain is a whole-disk chain; a BIOS
     * preserve/dual-boot install is named instead of planned.
     */
    case PRODUCTION_INSTALLER_BIOS_PRESERVE_UNSUPPORTED:
        return "PRODUCTION_INSTALLER_BIOS_PRESERVE_UNSUPPORTED";
    /*
     * ITEM-517 step 2: the BIOS boot path refusals.
     */
    case PRODUCTION_INSTALLER_BIOS_STAGE_UNAVAILABLE:
        return "PRODUCTION_INSTALLER_BIOS_STAGE_UNAVAILABLE";
    case PRODUCTION_INSTALLER_BIOS_BOOT_PARTITION_TOO_SMALL:
        return "PRODUCTION_INSTALLER_BIOS_BOOT_PARTITION_TOO_SMALL";
    case PRODUCTION_INSTALLER_BOOT_VOLUME_TOO_SMALL:
        return "PRODUCTION_INSTALLER_BOOT_VOLUME_TOO_SMALL";
    case PRODUCTION_INSTALLER_STATE_NOT_PRESERVABLE:
        return "PRODUCTION_INSTALLER_STATE_NOT_PRESERVABLE";
    default:
        return NULL;
    }
}
