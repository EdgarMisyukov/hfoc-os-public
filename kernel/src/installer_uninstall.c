#include "installer_uninstall.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define INSTALLER_UNINSTALL_BOOT_NUMBER_MAX 65535U

static bool installer_uninstall_scenario_valid(
    enum installer_uninstall_scenario scenario)
{
    return scenario == INSTALLER_UNINSTALL_SCENARIO_RESTORE_PREVIOUS_OS ||
           scenario == INSTALLER_UNINSTALL_SCENARIO_ERASE_HFOC_ONLY;
}

static bool installer_uninstall_identity_valid(
    const char *identity,
    size_t length)
{
    size_t index;

    if (identity == NULL || length == 0U ||
        length >= INSTALLER_UNINSTALL_IDENTITY_LENGTH) {
        return false;
    }
    for (index = 0U; index < length; ++index) {
        const unsigned char value = (unsigned char)identity[index];

        if (value < 0x20U || value > 0x7EU) {
            return false;
        }
    }
    return true;
}

static size_t installer_uninstall_identity_length(
    const char identity[INSTALLER_UNINSTALL_IDENTITY_LENGTH])
{
    size_t length = 0U;

    while (length < INSTALLER_UNINSTALL_IDENTITY_LENGTH &&
           identity[length] != '\0') {
        ++length;
    }
    return length;
}

static void installer_uninstall_identity_copy(
    char destination[INSTALLER_UNINSTALL_IDENTITY_LENGTH],
    const char *identity,
    size_t identity_length)
{
    size_t index;

    for (index = 0U; index < INSTALLER_UNINSTALL_IDENTITY_LENGTH;
         ++index) {
        destination[index] = '\0';
    }
    for (index = 0U;
         index + 1U < INSTALLER_UNINSTALL_IDENTITY_LENGTH &&
         index < identity_length;
         ++index) {
        destination[index] = identity[index];
    }
}

static bool installer_uninstall_zero_digest(
    const unsigned char digest[SHA256_DIGEST_SIZE])
{
    size_t index;

    for (index = 0U; index < SHA256_DIGEST_SIZE; ++index) {
        if (digest[index] != 0U) {
            return false;
        }
    }
    return true;
}

static void installer_uninstall_store_le64(unsigned char *bytes,
                                           uint64_t value)
{
    size_t index;

    for (index = 0U; index < 8U; ++index) {
        bytes[index] = (unsigned char)(value >> (index * 8U));
    }
}

enum installer_uninstall_status installer_uninstall_snapshot_digest(
    const struct installer_layout_snapshot *snapshot,
    unsigned char digest[SHA256_DIGEST_SIZE])
{
    struct sha256 context;
    unsigned char encoded[8U];
    size_t index;

    if (snapshot == NULL || digest == NULL ||
        snapshot->sector_count == 0U ||
        snapshot->sector_count > INSTALLER_LAYOUT_SNAPSHOT_CAPACITY) {
        return INSTALLER_UNINSTALL_INVALID_ARGUMENT;
    }
    memset(&context, 0, sizeof(context));
    installer_uninstall_store_le64(
        encoded, (uint64_t)snapshot->sector_count);
    if (sha256_init(&context) != SHA256_OK ||
        sha256_update(&context, encoded, sizeof(encoded)) != SHA256_OK) {
        return INSTALLER_UNINSTALL_INVALID_ARGUMENT;
    }
    for (index = 0U; index < snapshot->sector_count; ++index) {
        installer_uninstall_store_le64(
            encoded, snapshot->sector_lbas[index]);
        if (sha256_update(&context, encoded, sizeof(encoded)) !=
                SHA256_OK ||
            sha256_update(&context, snapshot->sectors[index],
                          INSTALLER_LAYOUT_BLOCK_SIZE) != SHA256_OK) {
            return INSTALLER_UNINSTALL_INVALID_ARGUMENT;
        }
    }
    if (sha256_final(&context, digest) != SHA256_OK) {
        return INSTALLER_UNINSTALL_INVALID_ARGUMENT;
    }
    return INSTALLER_UNINSTALL_OK;
}

static const struct installer_discovery_disk *installer_uninstall_find_disk(
    const struct installer_discovery_report *report,
    uint32_t disk_id)
{
    uint32_t index;

    for (index = 0U; index < INSTALLER_DISCOVERY_DISK_CAPACITY; ++index) {
        if (report->disks[index].present &&
            report->disks[index].id == disk_id) {
            return &report->disks[index];
        }
    }
    return NULL;
}

static enum installer_uninstall_status installer_uninstall_evidence_check(
    const struct installer_uninstall_evidence *evidence)
{
    uint32_t index;

    if (evidence->version != INSTALLER_UNINSTALL_VERSION) {
        return INSTALLER_UNINSTALL_BAD_VERSION;
    }
    if (evidence->disk_id == 0U ||
        !installer_uninstall_identity_valid(
            evidence->identity,
            installer_uninstall_identity_length(evidence->identity))) {
        return INSTALLER_UNINSTALL_INVALID_ARGUMENT;
    }
    if (evidence->hfoc_partition_count == 0U) {
        return INSTALLER_UNINSTALL_NO_HFOC_FOOTPRINT;
    }
    if (evidence->hfoc_boot_entry_count >
        INSTALLER_UNINSTALL_BOOT_ENTRY_CAPACITY) {
        return INSTALLER_UNINSTALL_CAPACITY;
    }
    for (index = 0U; index < evidence->hfoc_boot_entry_count; ++index) {
        if (evidence->hfoc_boot_entry_indices[index] >
            INSTALLER_UNINSTALL_BOOT_NUMBER_MAX) {
            return INSTALLER_UNINSTALL_INVALID_ARGUMENT;
        }
    }
    if (evidence->previous_boot_entry_count >
        INSTALLER_UNINSTALL_ORDER_CAPACITY / 2U) {
        return INSTALLER_UNINSTALL_CAPACITY;
    }
    for (index = 0U; index < evidence->hfoc_boot_entry_count; ++index) {
        uint32_t other;

        for (other = index + 1U;
             other < evidence->hfoc_boot_entry_count; ++other) {
            if (evidence->hfoc_boot_entry_indices[index] ==
                evidence->hfoc_boot_entry_indices[other]) {
                return INSTALLER_UNINSTALL_INVALID_ARGUMENT;
            }
        }
    }
    return INSTALLER_UNINSTALL_OK;
}

enum installer_uninstall_status installer_uninstall_plan_build(
    const struct installer_discovery_report *report,
    const struct installer_uninstall_evidence *evidence,
    enum installer_uninstall_scenario scenario,
    struct installer_uninstall_plan *plan)
{
    enum installer_uninstall_status status;
    const struct installer_discovery_disk *disk;
    uint32_t index;

    if (report == NULL || evidence == NULL || plan == NULL) {
        return INSTALLER_UNINSTALL_NULL;
    }
    if (!installer_uninstall_scenario_valid(scenario)) {
        return INSTALLER_UNINSTALL_INVALID_ARGUMENT;
    }
    if (report->version != INSTALLER_HARDWARE_DISCOVERY_VERSION ||
        !report->read_only || !report->scan_complete) {
        return INSTALLER_UNINSTALL_INVALID_ARGUMENT;
    }
    status = installer_uninstall_evidence_check(evidence);
    if (status != INSTALLER_UNINSTALL_OK) {
        return status;
    }
    if (evidence->previous_os_count != report->os_count) {
        return INSTALLER_UNINSTALL_INVALID_ARGUMENT;
    }
    disk = installer_uninstall_find_disk(report, evidence->disk_id);
    if (disk == NULL) {
        return INSTALLER_UNINSTALL_INVALID_ARGUMENT;
    }
    if (installer_uninstall_identity_length(disk->identity) !=
            installer_uninstall_identity_length(evidence->identity) ||
        memcmp(disk->identity, evidence->identity,
               installer_uninstall_identity_length(
                   evidence->identity)) != 0) {
        return INSTALLER_UNINSTALL_INVALID_ARGUMENT;
    }
    if (scenario == INSTALLER_UNINSTALL_SCENARIO_RESTORE_PREVIOUS_OS) {
        if (report->os_count == 0U) {
            return INSTALLER_UNINSTALL_NO_PREVIOUS_OS;
        }
        if (!evidence->previous_boot_order_available) {
            return INSTALLER_UNINSTALL_BOOT_EVIDENCE_MISSING;
        }
        if (evidence->previous_boot_order_length !=
            (size_t)evidence->previous_boot_entry_count * 2U) {
            return INSTALLER_UNINSTALL_BOOT_EVIDENCE_MISSING;
        }
        if (!evidence->partition_table_snapshot_available ||
            installer_uninstall_zero_digest(evidence->snapshot_digest)) {
            return INSTALLER_UNINSTALL_SNAPSHOT_UNAVAILABLE;
        }
    }

    memset(plan, 0, sizeof(*plan));
    plan->version = INSTALLER_UNINSTALL_VERSION;
    plan->stage = INSTALLER_UNINSTALL_STAGE_PLANNED;
    plan->scenario = scenario;
    plan->disk_id = evidence->disk_id;
    plan->dry_run = true;
    plan->confirmed = false;
    plan->previous_os_preserved_required =
        scenario == INSTALLER_UNINSTALL_SCENARIO_RESTORE_PREVIOUS_OS;
    plan->restore_previous_boot_order =
        scenario == INSTALLER_UNINSTALL_SCENARIO_RESTORE_PREVIOUS_OS;
    plan->remove_partition_count = evidence->hfoc_partition_count;
    plan->remove_boot_entry_count = evidence->hfoc_boot_entry_count;
    for (index = 0U; index < evidence->hfoc_boot_entry_count; ++index) {
        plan->remove_boot_entry_indices[index] =
            evidence->hfoc_boot_entry_indices[index];
    }
    plan->preserve_boot_entry_count = evidence->previous_boot_entry_count;
    plan->previous_boot_order_length =
        evidence->previous_boot_order_length;
    if (evidence->previous_boot_order_length != 0U) {
        memcpy(plan->previous_boot_order,
               evidence->previous_boot_order,
               evidence->previous_boot_order_length);
    }
    memcpy(plan->snapshot_digest, evidence->snapshot_digest,
           SHA256_DIGEST_SIZE);
    installer_uninstall_identity_copy(
        plan->identity, evidence->identity,
        installer_uninstall_identity_length(evidence->identity));
    return INSTALLER_UNINSTALL_OK;
}

static void installer_uninstall_store_decimal(
    char *buffer,
    size_t capacity,
    size_t *length,
    uint32_t value)
{
    char digits[16U];
    size_t digit_count = 0U;

    do {
        digits[digit_count++] = (char)('0' + (value % 10U));
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

static void installer_uninstall_append_text(
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

enum installer_uninstall_status installer_uninstall_preview(
    struct installer_uninstall_plan *plan,
    struct installer_uninstall_preview *preview)
{
    size_t length = 0U;

    if (plan == NULL || preview == NULL) {
        return INSTALLER_UNINSTALL_NULL;
    }
    if (plan->version != INSTALLER_UNINSTALL_VERSION ||
        plan->stage != INSTALLER_UNINSTALL_STAGE_PLANNED) {
        return INSTALLER_UNINSTALL_BAD_STATE;
    }
    memset(preview, 0, sizeof(*preview));
    preview->version = INSTALLER_UNINSTALL_VERSION;
    preview->scenario = plan->scenario;
    preview->disk_id = plan->disk_id;
    preview->destructive =
        plan->remove_partition_count != 0U ||
        plan->remove_boot_entry_count != 0U;
    preview->previous_os_preserved =
        plan->previous_os_preserved_required;
    preview->remove_partition_count = plan->remove_partition_count;
    preview->preserve_boot_entry_count = plan->preserve_boot_entry_count;

    installer_uninstall_append_text(
        preview->text, sizeof(preview->text), &length,
        "HFOC UNINSTALL dry-run scenario=");
    installer_uninstall_append_text(
        preview->text, sizeof(preview->text), &length,
        plan->scenario ==
                INSTALLER_UNINSTALL_SCENARIO_RESTORE_PREVIOUS_OS
            ? "restore-previous-os"
            : "erase-hfoc-only");
    installer_uninstall_append_text(
        preview->text, sizeof(preview->text), &length, " disk=");
    installer_uninstall_store_decimal(
        preview->text, sizeof(preview->text), &length, plan->disk_id);
    installer_uninstall_append_text(
        preview->text, sizeof(preview->text), &length,
        " remove-partitions=");
    installer_uninstall_store_decimal(
        preview->text, sizeof(preview->text), &length,
        plan->remove_partition_count);
    installer_uninstall_append_text(
        preview->text, sizeof(preview->text), &length,
        " remove-boot-entries=");
    installer_uninstall_store_decimal(
        preview->text, sizeof(preview->text), &length,
        plan->remove_boot_entry_count);
    installer_uninstall_append_text(
        preview->text, sizeof(preview->text), &length,
        " preserve-boot-entries=");
    installer_uninstall_store_decimal(
        preview->text, sizeof(preview->text), &length,
        plan->preserve_boot_entry_count);
    installer_uninstall_append_text(
        preview->text, sizeof(preview->text), &length,
        plan->previous_os_preserved_required
            ? " previous-os=restored"
            : " previous-os=none");
    plan->stage = INSTALLER_UNINSTALL_STAGE_PREVIEW_PUBLISHED;
    return INSTALLER_UNINSTALL_OK;
}

enum installer_uninstall_status installer_uninstall_confirm(
    struct installer_uninstall_plan *plan)
{
    if (plan == NULL) {
        return INSTALLER_UNINSTALL_NULL;
    }
    if (plan->version != INSTALLER_UNINSTALL_VERSION) {
        return INSTALLER_UNINSTALL_BAD_VERSION;
    }
    if (plan->stage != INSTALLER_UNINSTALL_STAGE_PREVIEW_PUBLISHED) {
        return INSTALLER_UNINSTALL_BAD_STATE;
    }
    plan->dry_run = false;
    plan->confirmed = true;
    plan->stage = INSTALLER_UNINSTALL_STAGE_CONFIRMED;
    return INSTALLER_UNINSTALL_OK;
}

enum installer_uninstall_status installer_uninstall_verify_outcome(
    struct installer_uninstall_plan *plan,
    const struct installer_uninstall_outcome *outcome)
{
    if (plan == NULL || outcome == NULL) {
        return INSTALLER_UNINSTALL_NULL;
    }
    if (plan->version != INSTALLER_UNINSTALL_VERSION) {
        return INSTALLER_UNINSTALL_BAD_VERSION;
    }
    if (plan->stage != INSTALLER_UNINSTALL_STAGE_EXECUTING) {
        return INSTALLER_UNINSTALL_BAD_STATE;
    }
    if (outcome->version != INSTALLER_UNINSTALL_VERSION) {
        return INSTALLER_UNINSTALL_BAD_VERSION;
    }
    if (outcome->hfoc_partitions_remaining ||
        outcome->hfoc_boot_entry_present ||
        !outcome->partition_table_restored) {
        plan->stage = INSTALLER_UNINSTALL_STAGE_FAILED;
        return INSTALLER_UNINSTALL_OUTCOME_INCOMPLETE;
    }
    if (plan->previous_os_preserved_required &&
        (!outcome->previous_boot_order_restored ||
         !outcome->previous_os_boot_paths_preserved)) {
        plan->stage = INSTALLER_UNINSTALL_STAGE_FAILED;
        return INSTALLER_UNINSTALL_PREVIOUS_OS_DAMAGED;
    }
    plan->stage = INSTALLER_UNINSTALL_STAGE_COMPLETE;
    return INSTALLER_UNINSTALL_OK;
}

enum installer_uninstall_status installer_uninstall_execute(
    struct installer_uninstall_plan *plan,
    struct block_device *device,
    const struct installer_layout_snapshot *snapshot,
    const struct installer_uninstall_boot_ops *boot_ops)
{
    enum installer_layout_status layout_status;
    enum installer_uninstall_boot_status boot_status;
    enum installer_uninstall_status status;
    struct installer_uninstall_outcome outcome;
    uint32_t index;

    if (plan == NULL) {
        return INSTALLER_UNINSTALL_NULL;
    }
    if (plan->version != INSTALLER_UNINSTALL_VERSION) {
        return INSTALLER_UNINSTALL_BAD_VERSION;
    }
    if (plan->stage != INSTALLER_UNINSTALL_STAGE_CONFIRMED ||
        !plan->confirmed || plan->dry_run) {
        return INSTALLER_UNINSTALL_NOT_CONFIRMED;
    }
    if (boot_ops == NULL || boot_ops->remove_boot_entry == NULL ||
        boot_ops->restore_boot_order == NULL ||
        boot_ops->observe_outcome == NULL) {
        return INSTALLER_UNINSTALL_BOOT_OPS_UNAVAILABLE;
    }
    if (device == NULL || snapshot == NULL ||
        block_device_validate(device) != BLOCK_DEVICE_OK) {
        return INSTALLER_UNINSTALL_INVALID_ARGUMENT;
    }

    {
        unsigned char snapshot_digest[SHA256_DIGEST_SIZE];

        if (installer_uninstall_snapshot_digest(
                snapshot, snapshot_digest) != INSTALLER_UNINSTALL_OK ||
            memcmp(snapshot_digest, plan->snapshot_digest,
                   SHA256_DIGEST_SIZE) != 0) {
            plan->stage = INSTALLER_UNINSTALL_STAGE_FAILED;
            return INSTALLER_UNINSTALL_SNAPSHOT_MISMATCH;
        }
    }

    plan->stage = INSTALLER_UNINSTALL_STAGE_EXECUTING;
    layout_status = installer_layout_rollback(device, snapshot);
    if (layout_status != INSTALLER_LAYOUT_OK) {
        plan->stage = INSTALLER_UNINSTALL_STAGE_FAILED;
        return INSTALLER_UNINSTALL_LAYOUT_ERROR;
    }
    for (index = 0U; index < plan->remove_boot_entry_count; ++index) {
        boot_status = boot_ops->remove_boot_entry(
            boot_ops->context, plan->remove_boot_entry_indices[index]);
        if (boot_status != INSTALLER_UNINSTALL_BOOT_OK) {
            plan->stage = INSTALLER_UNINSTALL_STAGE_FAILED;
            return INSTALLER_UNINSTALL_BOOT_OPS_FAILED;
        }
    }
    if (plan->restore_previous_boot_order) {
        boot_status = boot_ops->restore_boot_order(
            boot_ops->context, plan->previous_boot_order,
            plan->previous_boot_order_length,
            plan->preserve_boot_entry_count);
        if (boot_status != INSTALLER_UNINSTALL_BOOT_OK) {
            plan->stage = INSTALLER_UNINSTALL_STAGE_FAILED;
            return INSTALLER_UNINSTALL_BOOT_OPS_FAILED;
        }
    }

    memset(&outcome, 0, sizeof(outcome));
    outcome.version = INSTALLER_UNINSTALL_VERSION;
    boot_status = boot_ops->observe_outcome(
        boot_ops->context, &outcome);
    if (boot_status != INSTALLER_UNINSTALL_BOOT_OK) {
        plan->stage = INSTALLER_UNINSTALL_STAGE_FAILED;
        return INSTALLER_UNINSTALL_BOOT_OPS_FAILED;
    }
    status = installer_uninstall_verify_outcome(plan, &outcome);
    if (status != INSTALLER_UNINSTALL_OK) {
        return status;
    }
    return INSTALLER_UNINSTALL_OK;
}

const char *installer_uninstall_stage_string(
    enum installer_uninstall_stage stage)
{
    switch (stage) {
    case INSTALLER_UNINSTALL_STAGE_UNPREPARED:
        return "INSTALLER_UNINSTALL_STAGE_UNPREPARED";
    case INSTALLER_UNINSTALL_STAGE_PLANNED:
        return "INSTALLER_UNINSTALL_STAGE_PLANNED";
    case INSTALLER_UNINSTALL_STAGE_PREVIEW_PUBLISHED:
        return "INSTALLER_UNINSTALL_STAGE_PREVIEW_PUBLISHED";
    case INSTALLER_UNINSTALL_STAGE_CONFIRMED:
        return "INSTALLER_UNINSTALL_STAGE_CONFIRMED";
    case INSTALLER_UNINSTALL_STAGE_EXECUTING:
        return "INSTALLER_UNINSTALL_STAGE_EXECUTING";
    case INSTALLER_UNINSTALL_STAGE_COMPLETE:
        return "INSTALLER_UNINSTALL_STAGE_COMPLETE";
    case INSTALLER_UNINSTALL_STAGE_FAILED:
        return "INSTALLER_UNINSTALL_STAGE_FAILED";
    default:
        return NULL;
    }
}

const char *installer_uninstall_scenario_string(
    enum installer_uninstall_scenario scenario)
{
    switch (scenario) {
    case INSTALLER_UNINSTALL_SCENARIO_RESTORE_PREVIOUS_OS:
        return "INSTALLER_UNINSTALL_SCENARIO_RESTORE_PREVIOUS_OS";
    case INSTALLER_UNINSTALL_SCENARIO_ERASE_HFOC_ONLY:
        return "INSTALLER_UNINSTALL_SCENARIO_ERASE_HFOC_ONLY";
    default:
        return NULL;
    }
}

const char *installer_uninstall_status_string(
    enum installer_uninstall_status status)
{
    switch (status) {
    case INSTALLER_UNINSTALL_OK:
        return "INSTALLER_UNINSTALL_OK";
    case INSTALLER_UNINSTALL_NULL:
        return "INSTALLER_UNINSTALL_NULL";
    case INSTALLER_UNINSTALL_BAD_VERSION:
        return "INSTALLER_UNINSTALL_BAD_VERSION";
    case INSTALLER_UNINSTALL_BAD_STATE:
        return "INSTALLER_UNINSTALL_BAD_STATE";
    case INSTALLER_UNINSTALL_INVALID_ARGUMENT:
        return "INSTALLER_UNINSTALL_INVALID_ARGUMENT";
    case INSTALLER_UNINSTALL_NO_HFOC_FOOTPRINT:
        return "INSTALLER_UNINSTALL_NO_HFOC_FOOTPRINT";
    case INSTALLER_UNINSTALL_NO_PREVIOUS_OS:
        return "INSTALLER_UNINSTALL_NO_PREVIOUS_OS";
    case INSTALLER_UNINSTALL_BOOT_EVIDENCE_MISSING:
        return "INSTALLER_UNINSTALL_BOOT_EVIDENCE_MISSING";
    case INSTALLER_UNINSTALL_SNAPSHOT_UNAVAILABLE:
        return "INSTALLER_UNINSTALL_SNAPSHOT_UNAVAILABLE";
    case INSTALLER_UNINSTALL_NOT_CONFIRMED:
        return "INSTALLER_UNINSTALL_NOT_CONFIRMED";
    case INSTALLER_UNINSTALL_BOOT_OPS_UNAVAILABLE:
        return "INSTALLER_UNINSTALL_BOOT_OPS_UNAVAILABLE";
    case INSTALLER_UNINSTALL_BOOT_OPS_FAILED:
        return "INSTALLER_UNINSTALL_BOOT_OPS_FAILED";
    case INSTALLER_UNINSTALL_LAYOUT_ERROR:
        return "INSTALLER_UNINSTALL_LAYOUT_ERROR";
    case INSTALLER_UNINSTALL_OUTCOME_INCOMPLETE:
        return "INSTALLER_UNINSTALL_OUTCOME_INCOMPLETE";
    case INSTALLER_UNINSTALL_PREVIOUS_OS_DAMAGED:
        return "INSTALLER_UNINSTALL_PREVIOUS_OS_DAMAGED";
    case INSTALLER_UNINSTALL_SNAPSHOT_MISMATCH:
        return "INSTALLER_UNINSTALL_SNAPSHOT_MISMATCH";
    case INSTALLER_UNINSTALL_CAPACITY:
        return "INSTALLER_UNINSTALL_CAPACITY";
    default:
        return NULL;
    }
}
