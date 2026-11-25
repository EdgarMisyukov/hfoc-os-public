#include "installer_layout.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define INSTALLER_LAYOUT_GPT_SIGNATURE \
    { 'E', 'F', 'I', ' ', 'P', 'A', 'R', 'T' }
#define INSTALLER_LAYOUT_GPT_HEADER_SIZE 92U
/*
 * ITEM-414: the partition entry array a GPT reserves is 16 KiB - 128 entries of
 * 128 bytes - and the array's size is `entry count * entry size`, which the
 * header's CRC covers.  This writer declared four entries, one block of array
 * where a conformant table has thirty-two, so the disk it produced described
 * itself as a table no other implementation has to accept: this project's own
 * reader refuses a table whose entry count is not 128
 * (`installer_gpt_preserve.c`), which is why the menu could not offer a system
 * on a disk this installer had written, and a firmware is free to refuse it the
 * same way.  mkimg was fixed for exactly this in FreeBSD
 * (https://github.com/freebsd/freebsd-src/commit/9b42d3e12ffc6896fcb4e60c1b239ddf60705831).
 */
#define INSTALLER_LAYOUT_GPT_ENTRY_COUNT 128U
#define INSTALLER_LAYOUT_GPT_ENTRY_SIZE 128U
#define INSTALLER_LAYOUT_GPT_ENTRIES_PER_BLOCK 4U
#define INSTALLER_LAYOUT_GPT_ARRAY_BLOCKS \
    (INSTALLER_LAYOUT_GPT_ENTRY_COUNT / \
     INSTALLER_LAYOUT_GPT_ENTRIES_PER_BLOCK)
/* The array occupies LBA 2 .. 2 + 32 - 1, so the first usable LBA is 34. */
#define INSTALLER_LAYOUT_GPT_FIRST_USABLE_LBA \
    (2U + INSTALLER_LAYOUT_GPT_ARRAY_BLOCKS)
#define INSTALLER_LAYOUT_MBR_PARTITION_OFFSET 446U
#define INSTALLER_LAYOUT_MBR_PARTITION_SIZE 16U

static const unsigned char installer_layout_gpt_signature[8U] =
    INSTALLER_LAYOUT_GPT_SIGNATURE;

static const unsigned char installer_layout_efi_guid[16U] = {
    0x28U, 0x73U, 0x2AU, 0xC1U, 0x1FU, 0xF8U, 0xD2U, 0x11U,
    0xBAU, 0x4BU, 0x00U, 0xA0U, 0xC9U, 0x3EU, 0xC9U, 0x3BU
};

static const unsigned char installer_layout_hfoc_system_guid[16U] = {
    0x8DU, 0x0BU, 0x19U, 0xA2U, 0x86U, 0x5CU, 0x0AU, 0x4AU,
    0x8EU, 0x5AU, 0x1AU, 0x7BU, 0x8CU, 0x4EU, 0x60U, 0xA1U
};

static const unsigned char installer_layout_persistence_guid[16U] = {
    0xE1U, 0x44U, 0x01U, 0x4CU, 0x0DU, 0x6EU, 0x7AU, 0x4BU,
    0xB4U, 0x9DU, 0x22U, 0x10U, 0x33U, 0x3DU, 0x41U, 0x31U
};

static const unsigned char installer_layout_recovery_guid[16U] = {
    0x2BU, 0xE5U, 0x71U, 0x6DU, 0xD4U, 0x53U, 0x47U, 0x4DU,
    0x9DU, 0x31U, 0x28U, 0x14U, 0x2DU, 0x3DU, 0x10U, 0xB8U
};

/*
 * ITEM-517: the legacy-BIOS boot partition type, `Hah!IdontNeedEFI`
 * (21686148-6449-6E6F-744E-656564454649).  The raw bytes are the ASCII name:
 * the GUID's first three fields are stored little-endian, which the four
 * chunks below reproduce.  A BIOS-boot partition is not a filesystem and must
 * never be formatted; Limine's stage2 is written verbatim at its first sector.
 */
static const unsigned char installer_layout_bios_boot_guid[16U] = {
    0x48U, 0x61U, 0x68U, 0x21U, 0x49U, 0x64U, 0x6FU, 0x6EU,
    0x74U, 0x4EU, 0x65U, 0x65U, 0x64U, 0x45U, 0x46U, 0x49U
};

/*
 * ITEM-517: the FAT boot volume of a legacy-BIOS install.  Microsoft basic
 * data (EBD0A0A2-B9E5-4433-87C0-68B6B72699C7) is the GPT type a plain FAT32
 * volume carries; the EFI System type is deliberately not used, because on a
 * BIOS machine no firmware is asked to boot from this volume.
 */
static const unsigned char installer_layout_boot_volume_guid[16U] = {
    0xA2U, 0xA0U, 0xD0U, 0xEBU, 0xE5U, 0xB9U, 0x33U, 0x44U,
    0x87U, 0xC0U, 0x68U, 0xB6U, 0xB7U, 0x26U, 0x99U, 0xC7U
};

static void installer_layout_store_le32(
    unsigned char *bytes,
    uint32_t value)
{
    bytes[0] = (unsigned char)(value & 0xFFU);
    bytes[1] = (unsigned char)((value >> 8U) & 0xFFU);
    bytes[2] = (unsigned char)((value >> 16U) & 0xFFU);
    bytes[3] = (unsigned char)((value >> 24U) & 0xFFU);
}

static void installer_layout_store_le64(
    unsigned char *bytes,
    uint64_t value)
{
    size_t index;

    for (index = 0U; index < 8U; ++index) {
        bytes[index] = (unsigned char)(value >> (8U * index));
    }
}

/*
 * ITEM-414: the CRC of the partition entry array is taken in blocks, because
 * the array is 16 KiB of which this installer fills one block and writes the
 * rest as zeros: the running value is carried from block to block instead of
 * keeping the whole array in memory.
 */
static uint32_t installer_layout_crc32_update(
    uint32_t crc,
    const unsigned char *bytes,
    size_t length)
{
    size_t byte_index;

    for (byte_index = 0U; byte_index < length; ++byte_index) {
        unsigned int bit;

        crc ^= bytes[byte_index];
        for (bit = 0U; bit < 8U; ++bit) {
            if ((crc & 1U) != 0U) {
                crc = (crc >> 1U) ^ 0xEDB88320U;
            } else {
                crc >>= 1U;
            }
        }
    }
    return crc;
}

static uint32_t installer_layout_crc32(
    const unsigned char *bytes,
    size_t length)
{
    return installer_layout_crc32_update(0xFFFFFFFFU, bytes, length) ^
        0xFFFFFFFFU;
}

static void installer_layout_build_partition_array(
    unsigned char block[INSTALLER_LAYOUT_BLOCK_SIZE],
    const struct installer_layout_plan *plan,
    size_t block_index);

/*
 * ITEM-517: the array CRC, carried across the blocks exactly as the writer
 * carries it.  Taking the plan rather than one built block is what lets a
 * five-entry BIOS plan have a CRC that covers the entries in the second array
 * block; for a plan of four entries or fewer every block after the first is
 * zeros, so the value is unchanged.
 */
static uint32_t installer_layout_crc32_array(
    const struct installer_layout_plan *plan)
{
    unsigned char block[INSTALLER_LAYOUT_BLOCK_SIZE];
    uint32_t crc = 0xFFFFFFFFU;
    size_t index;

    for (index = 0U; index < INSTALLER_LAYOUT_GPT_ARRAY_BLOCKS; ++index) {
        installer_layout_build_partition_array(block, plan, index);
        crc = installer_layout_crc32_update(crc, block, sizeof(block));
    }
    return crc ^ 0xFFFFFFFFU;
}

static bool installer_layout_plan_valid(
    const struct installer_layout_plan *plan)
{
    return plan != NULL &&
           plan->version == INSTALLER_LAYOUT_VERSION &&
           !plan->executed &&
           plan->mode >= INSTALLER_LAYOUT_MODE_WHOLE_DISK &&
           plan->mode <= INSTALLER_LAYOUT_MODE_MANUAL;
}

static bool installer_layout_disk_valid(
    const struct installer_discovery_disk *disk)
{
    return disk != NULL && disk->present &&
           disk->id != 0U &&
           disk->block_size == INSTALLER_LAYOUT_BLOCK_SIZE &&
           disk->block_count > INSTALLER_LAYOUT_GPT_FIRST_USABLE_LBA;
}

static const struct installer_discovery_disk *
installer_layout_find_disk(
    const struct installer_discovery_report *report,
    uint32_t disk_id)
{
    size_t index;

    if (report == NULL ||
        report->version != INSTALLER_HARDWARE_DISCOVERY_VERSION ||
        report->read_only == false ||
        !report->scan_complete) {
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

static bool installer_layout_identity_valid(
    const char *identity,
    size_t capacity)
{
    size_t index;

    if (identity == NULL || capacity == 0U) {
        return false;
    }
    for (index = 0U; index < capacity; ++index) {
        const unsigned char value = (unsigned char)identity[index];

        if (value == 0U) {
            return index > 0U;
        }
        if (value < 0x20U || value > 0x7EU) {
            return false;
        }
    }
    return false;
}

static void installer_layout_identity_copy(
    char destination[INSTALLER_LAYOUT_IDENTITY_LENGTH],
    const char *identity)
{
    size_t index;

    for (index = 0U; index < INSTALLER_LAYOUT_IDENTITY_LENGTH; ++index) {
        destination[index] = '\0';
    }
    if (identity == NULL) {
        return;
    }
    for (index = 0U;
         index + 1U < INSTALLER_LAYOUT_IDENTITY_LENGTH &&
         identity[index] != '\0';
         ++index) {
        destination[index] = identity[index];
    }
}

static uint32_t installer_layout_existing_partition_count(
    const struct installer_discovery_report *report,
    uint32_t disk_id)
{
    uint32_t count = 0U;
    size_t index;

    if (report == NULL) {
        return 0U;
    }
    for (index = 0U;
         index < INSTALLER_DISCOVERY_PARTITION_CAPACITY;
         ++index) {
        if (report->partitions[index].present &&
            report->partitions[index].disk_id == disk_id) {
            ++count;
        }
    }
    return count;
}

static enum installer_layout_status installer_layout_validate_requests(
    const struct installer_layout_partition *requests,
    size_t request_count,
    bool manual)
{
    size_t index;
    size_t other;

    if (requests == NULL || request_count == 0U) {
        return INSTALLER_LAYOUT_INVALID_ARGUMENT;
    }
    if (request_count > INSTALLER_LAYOUT_PARTITION_CAPACITY) {
        return INSTALLER_LAYOUT_CAPACITY;
    }
    for (index = 0U; index < request_count; ++index) {
        if (!requests[index].present ||
            requests[index].role <= INSTALLER_LAYOUT_ROLE_UNKNOWN ||
            requests[index].role > INSTALLER_LAYOUT_ROLE_BOOT_VOLUME ||
            requests[index].length_lba == 0U) {
            return INSTALLER_LAYOUT_INVALID_ARGUMENT;
        }
        if (manual &&
            (requests[index].first_lba == 0U ||
             requests[index].first_lba % INSTALLER_LAYOUT_ALIGN_LBA != 0U)) {
            return INSTALLER_LAYOUT_INVALID_ARGUMENT;
        }
        for (other = index + 1U; other < request_count; ++other) {
            if (requests[other].role == requests[index].role) {
                return INSTALLER_LAYOUT_INVALID_ARGUMENT;
            }
        }
    }
    return INSTALLER_LAYOUT_OK;
}

static enum installer_layout_status installer_layout_common_plan(
    const struct installer_discovery_report *report,
    uint32_t disk_id,
    const struct installer_layout_partition *requests,
    size_t request_count,
    enum installer_layout_mode mode,
    struct installer_layout_plan *plan)
{
    const struct installer_discovery_disk *disk;
    enum installer_layout_status status;

    if (plan == NULL) {
        return INSTALLER_LAYOUT_NULL;
    }
    status = installer_layout_validate_requests(
        requests, request_count, mode == INSTALLER_LAYOUT_MODE_MANUAL);
    if (status != INSTALLER_LAYOUT_OK) {
        return status;
    }
    disk = installer_layout_find_disk(report, disk_id);
    if (!installer_layout_disk_valid(disk)) {
        return INSTALLER_LAYOUT_INVALID_ARGUMENT;
    }
    if (!installer_layout_identity_valid(
            disk->identity, sizeof(disk->identity))) {
        return INSTALLER_LAYOUT_INVALID_ARGUMENT;
    }

    memset(plan, 0, sizeof(*plan));
    plan->version = INSTALLER_LAYOUT_VERSION;
    plan->mode = mode;
    plan->disk_id = disk_id;
    plan->firmware_mode = report->firmware_mode;
    plan->block_size = disk->block_size;
    plan->block_count = disk->block_count;
    plan->existing_partition_count =
        installer_layout_existing_partition_count(report, disk_id);
    plan->destructive =
        mode == INSTALLER_LAYOUT_MODE_WHOLE_DISK ||
        plan->existing_partition_count != 0U ||
        disk->partition_table != INSTALLER_PARTITION_TABLE_NONE;
    installer_layout_identity_copy(plan->identity, disk->identity);
    if (plan->firmware_mode == INSTALLER_FIRMWARE_UNKNOWN) {
        return INSTALLER_LAYOUT_BAD_STATE;
    }
    return INSTALLER_LAYOUT_OK;
}

static bool installer_layout_intervals_overlap(
    uint64_t left_first,
    uint64_t left_length,
    uint64_t right_first,
    uint64_t right_length)
{
    return left_first < right_first + right_length &&
           right_first < left_first + left_length;
}

static enum installer_layout_status installer_layout_assign_manual(
    struct installer_layout_plan *plan,
    const struct installer_layout_partition *requests,
    size_t request_count)
{
    size_t index;
    size_t other;

    for (index = 0U; index < request_count; ++index) {
        const uint64_t max_last =
            plan->block_count - INSTALLER_LAYOUT_GPT_FIRST_USABLE_LBA;
        const uint64_t last_lba =
            requests[index].first_lba + requests[index].length_lba - 1U;

        if (requests[index].first_lba > max_last ||
            requests[index].length_lba > max_last -
                requests[index].first_lba) {
            return INSTALLER_LAYOUT_OUT_OF_RANGE;
        }
        if (last_lba > max_last) {
            return INSTALLER_LAYOUT_OUT_OF_RANGE;
        }
        for (other = 0U; other < index; ++other) {
            if (installer_layout_intervals_overlap(
                    requests[other].first_lba,
                    requests[other].length_lba,
                    requests[index].first_lba,
                    requests[index].length_lba)) {
                return INSTALLER_LAYOUT_AMBIGUOUS;
            }
        }
        plan->partitions[index].partition_index = (uint32_t)index + 1U;
        plan->partitions[index].present = true;
        plan->partitions[index].role = requests[index].role;
        plan->partitions[index].first_lba = requests[index].first_lba;
        plan->partitions[index].length_lba = requests[index].length_lba;
    }
    plan->partition_count = (uint32_t)request_count;
    return INSTALLER_LAYOUT_OK;
}

static const struct installer_layout_partition *
installer_layout_find_request(
    const struct installer_layout_partition *requests,
    size_t request_count,
    enum installer_layout_role role)
{
    size_t index;

    for (index = 0U; index < request_count; ++index) {
        if (requests[index].present && requests[index].role == role) {
            return &requests[index];
        }
    }
    return NULL;
}

/*
 * ITEM-517: place one requested role at the cursor.  The check and the write
 * are exactly the ones the whole-disk assignment always performed; they are a
 * helper now so the UEFI order (the role enum) and the BIOS order (BIOS boot
 * first, then the payload roles) share one arithmetic and cannot drift apart.
 */
static enum installer_layout_status installer_layout_assign_role(
    struct installer_layout_plan *plan,
    enum installer_layout_role role,
    const struct installer_layout_partition *request,
    uint64_t *cursor,
    uint32_t *partition_index)
{
    if (request == NULL) {
        return INSTALLER_LAYOUT_OK;
    }
    if (*cursor >
            plan->block_count - INSTALLER_LAYOUT_GPT_FIRST_USABLE_LBA ||
        request->length_lba >
            plan->block_count - INSTALLER_LAYOUT_GPT_FIRST_USABLE_LBA -
                *cursor) {
        return INSTALLER_LAYOUT_OUT_OF_RANGE;
    }
    plan->partitions[*partition_index].partition_index =
        *partition_index + 1U;
    plan->partitions[*partition_index].present = true;
    plan->partitions[*partition_index].role = role;
    plan->partitions[*partition_index].first_lba = *cursor;
    plan->partitions[*partition_index].length_lba = request->length_lba;
    *cursor += request->length_lba;
    ++*partition_index;
    return INSTALLER_LAYOUT_OK;
}

static enum installer_layout_status installer_layout_assign_whole_disk(
    struct installer_layout_plan *plan,
    const struct installer_layout_partition *requests,
    size_t request_count)
{
    enum installer_layout_role role;
    uint64_t cursor = INSTALLER_LAYOUT_ALIGN_LBA;
    uint32_t partition_index = 0U;

    for (role = INSTALLER_LAYOUT_ROLE_EFI_SYSTEM;
         role <= INSTALLER_LAYOUT_ROLE_HFOC_RECOVERY;
         ++role) {
        const enum installer_layout_status status =
            installer_layout_assign_role(
                plan, role,
                installer_layout_find_request(
                    requests, request_count, role),
                &cursor, &partition_index);

        if (status != INSTALLER_LAYOUT_OK) {
            return status;
        }
    }
    plan->partition_count = partition_index;
    if (partition_index == 0U ||
        cursor >
            plan->block_count - INSTALLER_LAYOUT_GPT_FIRST_USABLE_LBA) {
        return INSTALLER_LAYOUT_OUT_OF_RANGE;
    }
    return INSTALLER_LAYOUT_OK;
}

/*
 * ITEM-517: the legacy-BIOS order.  The BIOS boot role is partition 1 (the
 * arrangement the measured Limine contract was first exercised with), then the
 * FAT boot volume the payload is staged on, then the three HFOC roles.  The
 * role values stay 5 and 6; this function is the only place that decides the
 * BIOS order, and the contract's BIOS role table reads it back in the same
 * order.
 */
static enum installer_layout_status installer_layout_assign_whole_disk_bios(
    struct installer_layout_plan *plan,
    const struct installer_layout_partition *requests,
    size_t request_count)
{
    static const enum installer_layout_role bios_order[] = {
        INSTALLER_LAYOUT_ROLE_BIOS_BOOT,
        INSTALLER_LAYOUT_ROLE_BOOT_VOLUME,
        INSTALLER_LAYOUT_ROLE_HFOC_SYSTEM,
        INSTALLER_LAYOUT_ROLE_HFOC_PERSISTENCE,
        INSTALLER_LAYOUT_ROLE_HFOC_RECOVERY,
    };
    uint64_t cursor = INSTALLER_LAYOUT_ALIGN_LBA;
    uint32_t partition_index = 0U;
    size_t index;

    for (index = 0U;
         index < sizeof(bios_order) / sizeof(bios_order[0U]); ++index) {
        const enum installer_layout_status status =
            installer_layout_assign_role(
                plan, bios_order[index],
                installer_layout_find_request(
                    requests, request_count, bios_order[index]),
                &cursor, &partition_index);

        if (status != INSTALLER_LAYOUT_OK) {
            return status;
        }
    }
    plan->partition_count = partition_index;
    if (partition_index == 0U ||
        cursor >
            plan->block_count - INSTALLER_LAYOUT_GPT_FIRST_USABLE_LBA) {
        return INSTALLER_LAYOUT_OUT_OF_RANGE;
    }
    return INSTALLER_LAYOUT_OK;
}

enum installer_layout_status installer_layout_plan_whole_disk(
    const struct installer_discovery_report *report,
    uint32_t disk_id,
    const struct installer_layout_partition *requests,
    size_t request_count,
    struct installer_layout_plan *plan)
{
    enum installer_layout_status status;

    status = installer_layout_common_plan(
        report, disk_id, requests, request_count,
        INSTALLER_LAYOUT_MODE_WHOLE_DISK, plan);
    if (status != INSTALLER_LAYOUT_OK) {
        return status;
    }
    return installer_layout_assign_whole_disk(
        plan, requests, request_count);
}

enum installer_layout_status installer_layout_plan_whole_disk_bios(
    const struct installer_discovery_report *report,
    uint32_t disk_id,
    const struct installer_layout_partition *requests,
    size_t request_count,
    struct installer_layout_plan *plan)
{
    enum installer_layout_status status;

    status = installer_layout_common_plan(
        report, disk_id, requests, request_count,
        INSTALLER_LAYOUT_MODE_WHOLE_DISK, plan);
    if (status != INSTALLER_LAYOUT_OK) {
        return status;
    }
    return installer_layout_assign_whole_disk_bios(
        plan, requests, request_count);
}

enum installer_layout_status installer_layout_plan_manual(
    const struct installer_discovery_report *report,
    uint32_t disk_id,
    const struct installer_layout_partition *requests,
    size_t request_count,
    struct installer_layout_plan *plan)
{
    enum installer_layout_status status;

    status = installer_layout_common_plan(
        report, disk_id, requests, request_count,
        INSTALLER_LAYOUT_MODE_MANUAL, plan);
    if (status != INSTALLER_LAYOUT_OK) {
        return status;
    }
    if (plan->existing_partition_count != 0U ||
        installer_layout_find_disk(report, disk_id)->partition_table !=
            INSTALLER_PARTITION_TABLE_NONE) {
        return INSTALLER_LAYOUT_AMBIGUOUS;
    }
    return installer_layout_assign_manual(
        plan, requests, request_count);
}

static void installer_layout_append_char(
    char *buffer,
    size_t capacity,
    size_t *length,
    char value)
{
    if (*length + 1U < capacity) {
        buffer[*length] = value;
        ++*length;
        buffer[*length] = '\0';
    }
}

static void installer_layout_append_uint64(
    char *buffer,
    size_t capacity,
    size_t *length,
    uint64_t value)
{
    char reversed[20U];
    size_t count = 0U;

    if (value == 0U) {
        installer_layout_append_char(
            buffer, capacity, length, '0');
        return;
    }
    while (value != 0U && count < sizeof(reversed)) {
        reversed[count] = (char)('0' + (value % 10U));
        value /= 10U;
        ++count;
    }
    while (count > 0U) {
        --count;
        installer_layout_append_char(
            buffer, capacity, length, reversed[count]);
    }
}

static void installer_layout_append_text(
    char *buffer,
    size_t capacity,
    size_t *length,
    const char *text)
{
    size_t index;

    for (index = 0U; text[index] != '\0'; ++index) {
        installer_layout_append_char(
            buffer, capacity, length, text[index]);
    }
}

static const char *installer_layout_mode_text(
    enum installer_layout_mode mode)
{
    switch (mode) {
    case INSTALLER_LAYOUT_MODE_WHOLE_DISK:
        return "whole-disk";
    case INSTALLER_LAYOUT_MODE_MANUAL:
        return "manual";
    default:
        return "unknown";
    }
}

enum installer_layout_status installer_layout_preview(
    const struct installer_layout_plan *plan,
    struct installer_layout_preview *preview)
{
    size_t length = 0U;
    size_t index;

    if (preview == NULL) {
        return INSTALLER_LAYOUT_NULL;
    }
    if (!installer_layout_plan_valid(plan)) {
        return INSTALLER_LAYOUT_BAD_STATE;
    }
    memset(preview, 0, sizeof(*preview));
    preview->version = INSTALLER_LAYOUT_VERSION;
    preview->disk_id = plan->disk_id;
    preview->mode = plan->mode;
    preview->destructive = plan->destructive;
    preview->existing_partition_count = plan->existing_partition_count;
    preview->partition_count = plan->partition_count;
    installer_layout_append_text(
        preview->text, sizeof(preview->text), &length,
        plan->identity);
    installer_layout_append_text(
        preview->text, sizeof(preview->text), &length, " ");
    installer_layout_append_text(
        preview->text, sizeof(preview->text), &length,
        installer_layout_mode_text(plan->mode));
    installer_layout_append_text(
        preview->text, sizeof(preview->text), &length,
        " existing=");
    installer_layout_append_uint64(
        preview->text, sizeof(preview->text), &length,
        plan->existing_partition_count);
    installer_layout_append_text(
        preview->text, sizeof(preview->text), &length,
        " destructive=");
    installer_layout_append_uint64(
        preview->text, sizeof(preview->text), &length,
        plan->destructive ? 1U : 0U);
    for (index = 0U; index < plan->partition_count; ++index) {
        const struct installer_layout_partition *partition =
            &plan->partitions[index];

        installer_layout_append_text(
            preview->text, sizeof(preview->text), &length, " p");
        installer_layout_append_uint64(
            preview->text, sizeof(preview->text), &length,
            partition->partition_index);
        installer_layout_append_text(
            preview->text, sizeof(preview->text), &length, "=");
        installer_layout_append_uint64(
            preview->text, sizeof(preview->text), &length,
            partition->first_lba);
        installer_layout_append_text(
            preview->text, sizeof(preview->text), &length, ":");
        installer_layout_append_uint64(
            preview->text, sizeof(preview->text), &length,
            partition->length_lba);
    }
    return INSTALLER_LAYOUT_OK;
}

enum installer_layout_status installer_layout_confirm(
    struct installer_layout_plan *plan)
{
    if (plan == NULL) {
        return INSTALLER_LAYOUT_NULL;
    }
    if (!installer_layout_plan_valid(plan)) {
        return INSTALLER_LAYOUT_BAD_STATE;
    }
    plan->confirmed = true;
    return INSTALLER_LAYOUT_OK;
}

static void installer_layout_write_guid(
    unsigned char *bytes,
    const unsigned char guid[16U])
{
    size_t index;

    for (index = 0U; index < 16U; ++index) {
        bytes[index] = guid[index];
    }
}

/*
 * ITEM-396: published entry identity.  The role type table stays the single
 * source of that identity; the two entry points after it expose the table and
 * the derived unique GUID, so the preservation writer (`installer_gpt_preserve`)
 * adds the appliance's entries with exactly the GUIDs this planner would have
 * written instead of keeping a second copy that could drift.
 */
static void installer_layout_write_unique_guid(
    unsigned char *bytes,
    enum installer_layout_role role,
    uint32_t partition_index)
{
    size_t index;

    for (index = 0U; index < 16U; ++index) {
        bytes[index] = 0U;
    }
    bytes[0] = (unsigned char)role;
    bytes[1] = (unsigned char)partition_index;
    bytes[2] = 0xA1U;
    bytes[3] = 0xB2U;
    bytes[4] = 0xC3U;
    bytes[5] = 0xD4U;
    bytes[6] = 0xE5U;
    bytes[7] = 0xF6U;
}

static const unsigned char *installer_layout_role_guid(
    enum installer_layout_role role)
{
    switch (role) {
    case INSTALLER_LAYOUT_ROLE_EFI_SYSTEM:
        return installer_layout_efi_guid;
    case INSTALLER_LAYOUT_ROLE_HFOC_SYSTEM:
        return installer_layout_hfoc_system_guid;
    case INSTALLER_LAYOUT_ROLE_HFOC_PERSISTENCE:
        return installer_layout_persistence_guid;
    case INSTALLER_LAYOUT_ROLE_HFOC_RECOVERY:
        return installer_layout_recovery_guid;
    case INSTALLER_LAYOUT_ROLE_BIOS_BOOT:
        return installer_layout_bios_boot_guid;
    case INSTALLER_LAYOUT_ROLE_BOOT_VOLUME:
        return installer_layout_boot_volume_guid;
    default:
        return NULL;
    }
}

const unsigned char *installer_layout_role_type_guid(
    enum installer_layout_role role)
{
    return installer_layout_role_guid(role);
}

void installer_layout_role_unique_guid(
    unsigned char guid[16U],
    enum installer_layout_role role,
    uint32_t partition_index)
{
    if (guid == NULL) {
        return;
    }
    installer_layout_write_unique_guid(guid, role, partition_index);
}

static void installer_layout_build_mbr(
    unsigned char sector[INSTALLER_LAYOUT_BLOCK_SIZE],
    uint64_t block_count)
{
    unsigned char *partition =
        sector + INSTALLER_LAYOUT_MBR_PARTITION_OFFSET;

    memset(sector, 0, INSTALLER_LAYOUT_BLOCK_SIZE);
    partition[0U] = 0x00U;
    partition[1U] = 0x00U;
    partition[2U] = 0x02U;
    partition[3U] = 0x00U;
    partition[4U] = 0xEEU;
    partition[5U] = 0xFFU;
    partition[6U] = 0xFFU;
    partition[7U] = 0xFFU;
    installer_layout_store_le32(partition + 8U, 1U);
    installer_layout_store_le32(
        partition + 12U, (uint32_t)(block_count - 1U));
    sector[510U] = 0x55U;
    sector[511U] = 0xAAU;
}

/*
 * ITEM-517: build one 512-byte block of the 16 KiB partition array.  The
 * array is still never held in memory: the caller builds a block, writes it
 * and moves on, and the CRC is carried across the blocks.  Before this item
 * the writer only ever built the first block, because the plan could not hold
 * more than four entries; the BIOS layout has five.  For a plan of four
 * entries or fewer the bytes are exactly what the old writer produced: the
 * first block carries the entries and every later block is zeros.
 */
static void installer_layout_build_partition_array(
    unsigned char block[INSTALLER_LAYOUT_BLOCK_SIZE],
    const struct installer_layout_plan *plan,
    size_t block_index)
{
    const size_t first_entry =
        block_index * INSTALLER_LAYOUT_GPT_ENTRIES_PER_BLOCK;
    size_t index;

    memset(block, 0, INSTALLER_LAYOUT_BLOCK_SIZE);
    for (index = first_entry;
         index < plan->partition_count &&
             index < first_entry + INSTALLER_LAYOUT_GPT_ENTRIES_PER_BLOCK;
         ++index) {
        const struct installer_layout_partition *partition =
            &plan->partitions[index];
        const unsigned char *type_guid;
        unsigned char *entry;

        if (!partition->present) {
            continue;
        }
        type_guid = installer_layout_role_guid(partition->role);
        if (type_guid == NULL) {
            continue;
        }
        entry = block +
                (index - first_entry) * INSTALLER_LAYOUT_GPT_ENTRY_SIZE;
        installer_layout_write_guid(entry, type_guid);
        installer_layout_write_unique_guid(
            entry + 16U, partition->role, partition->partition_index);
        installer_layout_store_le64(
            entry + 32U, partition->first_lba);
        installer_layout_store_le64(
            entry + 40U,
            partition->first_lba + partition->length_lba - 1U);
    }
}

static void installer_layout_build_header(
    unsigned char header[INSTALLER_LAYOUT_BLOCK_SIZE],
    uint64_t block_count,
    uint64_t current_lba,
    uint64_t backup_lba,
    uint64_t array_lba,
    uint32_t array_crc)
{
    static const unsigned char disk_guid[16U] = {
        0x01U, 0x02U, 0x03U, 0x04U, 0x05U, 0x06U, 0x07U, 0x08U,
        0x09U, 0x0AU, 0x0BU, 0x0CU, 0x0DU, 0x0EU, 0x0FU, 0x10U
    };
    uint32_t crc;

    memset(header, 0, INSTALLER_LAYOUT_BLOCK_SIZE);
    memcpy(header, installer_layout_gpt_signature, 8U);
    header[8U] = 0x00U;
    header[9U] = 0x00U;
    header[10U] = 0x01U;
    header[11U] = 0x00U;
    installer_layout_store_le32(
        header + 12U, INSTALLER_LAYOUT_GPT_HEADER_SIZE);
    installer_layout_write_guid(header + 56U, disk_guid);
    installer_layout_store_le64(header + 24U, current_lba);
    installer_layout_store_le64(header + 32U, backup_lba);
    installer_layout_store_le64(
        header + 40U, INSTALLER_LAYOUT_GPT_FIRST_USABLE_LBA);
    installer_layout_store_le64(
        header + 48U, block_count - INSTALLER_LAYOUT_GPT_FIRST_USABLE_LBA);
    /*
     * ITEM-414: each header names the array that belongs to it - the primary
     * header the primary array at LBA 2, the backup header the backup array at
     * the end of the disk.  Both headers named LBA 2 before this item, so a
     * reader that fell back to the backup header read the primary array.
     */
    installer_layout_store_le64(header + 72U, array_lba);
    installer_layout_store_le32(
        header + 80U, INSTALLER_LAYOUT_GPT_ENTRY_COUNT);
    installer_layout_store_le32(
        header + 84U, INSTALLER_LAYOUT_GPT_ENTRY_SIZE);
    installer_layout_store_le32(header + 88U, array_crc);
    memset(header + 16U, 0, 4U);
    crc = installer_layout_crc32(
        header, INSTALLER_LAYOUT_GPT_HEADER_SIZE);
    installer_layout_store_le32(header + 16U, crc);
}

static enum block_device_status installer_layout_device_write(
    struct block_device *device,
    uint64_t lba,
    const unsigned char *sector)
{
    return block_device_write(
        device, lba * INSTALLER_LAYOUT_BLOCK_SIZE,
        sector, INSTALLER_LAYOUT_BLOCK_SIZE);
}

static enum block_device_status installer_layout_device_read(
    struct block_device *device,
    uint64_t lba,
    unsigned char *sector)
{
    return block_device_read(
        device, lba * INSTALLER_LAYOUT_BLOCK_SIZE,
        sector, INSTALLER_LAYOUT_BLOCK_SIZE);
}

static enum installer_layout_status installer_layout_execute_internal(
    struct block_device *device,
    struct installer_layout_plan *plan,
    struct installer_layout_snapshot *snapshot)
{
    unsigned char mbr[INSTALLER_LAYOUT_BLOCK_SIZE];
    unsigned char array[INSTALLER_LAYOUT_BLOCK_SIZE];
    unsigned char block[INSTALLER_LAYOUT_BLOCK_SIZE];
    unsigned char zeros[INSTALLER_LAYOUT_BLOCK_SIZE];
    unsigned char header[INSTALLER_LAYOUT_BLOCK_SIZE];
    unsigned char backup_header[INSTALLER_LAYOUT_BLOCK_SIZE];
    unsigned char verify[INSTALLER_LAYOUT_BLOCK_SIZE];
    uint64_t backup_lba;
    uint64_t backup_array_lba;
    uint32_t array_crc;
    enum block_device_status io_status;
    size_t index;
    uint32_t planned_lbas[5U];

    backup_lba = plan->block_count - 1U;
    /*
     * ITEM-414: the backup array is as long as the primary one, so it starts
     * where the last usable LBA stops and ends one block before the backup
     * header; the block that carries the entries is its first, exactly as the
     * primary array's first block carries them.
     */
    backup_array_lba =
        backup_lba - INSTALLER_LAYOUT_GPT_ARRAY_BLOCKS;
    planned_lbas[0U] = 0U;
    planned_lbas[1U] = 1U;
    planned_lbas[2U] = 2U;
    planned_lbas[3U] = (uint32_t)backup_array_lba;
    planned_lbas[4U] = (uint32_t)backup_lba;
    if (snapshot != NULL) {
        memset(snapshot, 0, sizeof(*snapshot));
        snapshot->sector_count = 5U;
        for (index = 0U; index < 5U; ++index) {
            snapshot->sector_lbas[index] = planned_lbas[index];
            io_status = installer_layout_device_read(
                device, planned_lbas[index],
                snapshot->sectors[index]);
            if (io_status != BLOCK_DEVICE_OK) {
                snapshot->sector_count = (uint32_t)index;
                return INSTALLER_LAYOUT_IO_ERROR;
            }
        }
    }

    installer_layout_build_mbr(mbr, plan->block_count);
    installer_layout_build_partition_array(array, plan, 0U);
    array_crc = installer_layout_crc32_array(plan);
    installer_layout_build_header(
        header, plan->block_count, 1U, backup_lba, 2U, array_crc);
    installer_layout_build_header(
        backup_header, plan->block_count, backup_lba, 1U,
        backup_array_lba, array_crc);

    io_status = installer_layout_device_write(device, 0U, mbr);
    if (io_status == BLOCK_DEVICE_OK) {
        io_status = installer_layout_device_write(device, 1U, header);
    }
    if (io_status == BLOCK_DEVICE_OK) {
        io_status = installer_layout_device_write(device, 2U, array);
    }
    memset(zeros, 0, sizeof(zeros));
    /*
     * The array is 16 KiB and is written block by block, each block built from
     * the plan: the header's CRC was taken over exactly these bytes, and a
     * table that leaves the tail as it found it is not the table the CRC
     * describes.  A plan of four entries or fewer only fills the first block,
     * so every later block is the zeros this writer always wrote.
     */
    for (index = 1U;
         io_status == BLOCK_DEVICE_OK &&
             index < INSTALLER_LAYOUT_GPT_ARRAY_BLOCKS;
         ++index) {
        installer_layout_build_partition_array(block, plan, index);
        io_status = installer_layout_device_write(
            device, 2U + index, block);
    }
    if (io_status == BLOCK_DEVICE_OK) {
        io_status = installer_layout_device_write(
            device, backup_lba, backup_header);
    }
    if (io_status == BLOCK_DEVICE_OK) {
        io_status = installer_layout_device_write(
            device, backup_array_lba, array);
    }
    for (index = 1U;
         io_status == BLOCK_DEVICE_OK &&
             index < INSTALLER_LAYOUT_GPT_ARRAY_BLOCKS;
         ++index) {
        installer_layout_build_partition_array(block, plan, index);
        io_status = installer_layout_device_write(
            device, backup_array_lba + index, block);
    }
    if (io_status == BLOCK_DEVICE_OK) {
        io_status = installer_layout_device_read(device, 1U, verify);
        if (io_status == BLOCK_DEVICE_OK &&
            memcmp(verify, header, sizeof(header)) != 0) {
            io_status = BLOCK_DEVICE_IO_ERROR;
        }
    }
    if (io_status == BLOCK_DEVICE_OK) {
        io_status = installer_layout_device_read(device, 2U, verify);
        if (io_status == BLOCK_DEVICE_OK &&
            memcmp(verify, array, sizeof(array)) != 0) {
            io_status = BLOCK_DEVICE_IO_ERROR;
        }
    }
    if (io_status == BLOCK_DEVICE_OK) {
        /*
         * The last block of the array is the one a reader reaches only through
         * the entry count this table declares, so reading it back proves the
         * array the CRC covers is the array on the disk.
         */
        io_status = installer_layout_device_read(
            device, 1U + INSTALLER_LAYOUT_GPT_ARRAY_BLOCKS, verify);
        if (io_status == BLOCK_DEVICE_OK &&
            memcmp(verify, zeros, sizeof(zeros)) != 0) {
            io_status = BLOCK_DEVICE_IO_ERROR;
        }
    }
    if (io_status != BLOCK_DEVICE_OK) {
        if (snapshot != NULL) {
            for (index = snapshot->sector_count; index > 0U; --index) {
                (void)installer_layout_device_write(
                    device, snapshot->sector_lbas[index - 1U],
                    snapshot->sectors[index - 1U]);
            }
        }
        return INSTALLER_LAYOUT_IO_ERROR;
    }
    plan->executed = true;
    return INSTALLER_LAYOUT_OK;
}

enum installer_layout_status installer_layout_execute(
    struct block_device *device,
    struct installer_layout_plan *plan,
    struct installer_layout_snapshot *snapshot)
{
    if (device == NULL || plan == NULL) {
        return INSTALLER_LAYOUT_NULL;
    }
    if (!installer_layout_plan_valid(plan)) {
        return INSTALLER_LAYOUT_BAD_STATE;
    }
    if (!plan->confirmed) {
        return INSTALLER_LAYOUT_NOT_CONFIRMED;
    }
    if (block_device_validate(device) != BLOCK_DEVICE_OK ||
        device->block_size != INSTALLER_LAYOUT_BLOCK_SIZE ||
        device->block_count != plan->block_count) {
        return INSTALLER_LAYOUT_INVALID_BLOCK_DEVICE;
    }
    return installer_layout_execute_internal(device, plan, snapshot);
}

enum installer_layout_status installer_layout_rollback(
    struct block_device *device,
    const struct installer_layout_snapshot *snapshot)
{
    size_t index;

    if (device == NULL || snapshot == NULL) {
        return INSTALLER_LAYOUT_NULL;
    }
    if (block_device_validate(device) != BLOCK_DEVICE_OK ||
        device->block_size != INSTALLER_LAYOUT_BLOCK_SIZE ||
        snapshot->sector_count == 0U ||
        snapshot->sector_count > INSTALLER_LAYOUT_SNAPSHOT_CAPACITY) {
        return INSTALLER_LAYOUT_INVALID_BLOCK_DEVICE;
    }
    for (index = snapshot->sector_count; index > 0U; --index) {
        const enum block_device_status io_status =
            installer_layout_device_write(
                device, snapshot->sector_lbas[index - 1U],
                snapshot->sectors[index - 1U]);

        if (io_status != BLOCK_DEVICE_OK) {
            return INSTALLER_LAYOUT_IO_ERROR;
        }
    }
    return INSTALLER_LAYOUT_OK;
}

const char *installer_layout_status_string(
    enum installer_layout_status status)
{
    switch (status) {
    case INSTALLER_LAYOUT_OK:
        return "INSTALLER_LAYOUT_OK";
    case INSTALLER_LAYOUT_NULL:
        return "INSTALLER_LAYOUT_NULL";
    case INSTALLER_LAYOUT_BAD_VERSION:
        return "INSTALLER_LAYOUT_BAD_VERSION";
    case INSTALLER_LAYOUT_BAD_STATE:
        return "INSTALLER_LAYOUT_BAD_STATE";
    case INSTALLER_LAYOUT_INVALID_ARGUMENT:
        return "INSTALLER_LAYOUT_INVALID_ARGUMENT";
    case INSTALLER_LAYOUT_OUT_OF_RANGE:
        return "INSTALLER_LAYOUT_OUT_OF_RANGE";
    case INSTALLER_LAYOUT_INVALID_BLOCK_DEVICE:
        return "INSTALLER_LAYOUT_INVALID_BLOCK_DEVICE";
    case INSTALLER_LAYOUT_AMBIGUOUS:
        return "INSTALLER_LAYOUT_AMBIGUOUS";
    case INSTALLER_LAYOUT_CAPACITY:
        return "INSTALLER_LAYOUT_CAPACITY";
    case INSTALLER_LAYOUT_NOT_CONFIRMED:
        return "INSTALLER_LAYOUT_NOT_CONFIRMED";
    case INSTALLER_LAYOUT_ALREADY_EXECUTED:
        return "INSTALLER_LAYOUT_ALREADY_EXECUTED";
    case INSTALLER_LAYOUT_IO_ERROR:
        return "INSTALLER_LAYOUT_IO_ERROR";
    case INSTALLER_LAYOUT_READ_ONLY_VIOLATION:
        return "INSTALLER_LAYOUT_READ_ONLY_VIOLATION";
    default:
        return NULL;
    }
}
