#include "installer_hardware_discovery.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define INSTALLER_DISCOVERY_BLOCK_SIZE 512U
#define INSTALLER_DISCOVERY_MBR_SIGNATURE_OFFSET 510U
#define INSTALLER_DISCOVERY_MBR_PARTITION_OFFSET 446U
#define INSTALLER_DISCOVERY_MBR_PARTITION_SIZE 16U
#define INSTALLER_DISCOVERY_MBR_PARTITION_COUNT 4U
/* ITEM-501: the extended container and its logical partitions. */
#define INSTALLER_DISCOVERY_EBR_MAX 32U
#define INSTALLER_DISCOVERY_MBR_EXTENDED_FIRST 0x05U
#define INSTALLER_DISCOVERY_MBR_EXTENDED_LBA 0x0FU
#define INSTALLER_DISCOVERY_MBR_EXTENDED_LINUX 0x85U
#define INSTALLER_DISCOVERY_GPT_HEADER_SIGNATURE \
    { 'E', 'F', 'I', ' ', 'P', 'A', 'R', 'T' }
#define INSTALLER_DISCOVERY_GPT_HEADER_SIZE_OFFSET 12U
#define INSTALLER_DISCOVERY_GPT_ENTRY_LBA_OFFSET 72U
#define INSTALLER_DISCOVERY_GPT_ENTRY_COUNT_OFFSET 80U
#define INSTALLER_DISCOVERY_GPT_ENTRY_SIZE_OFFSET 84U
#define INSTALLER_DISCOVERY_GPT_ENTRY_SIZE 128U
#define INSTALLER_DISCOVERY_GPT_ENTRIES_PER_BLOCK 4U

static const unsigned char installer_discovery_gpt_signature[8U] =
    INSTALLER_DISCOVERY_GPT_HEADER_SIGNATURE;

static const unsigned char installer_discovery_efi_guid[16U] = {
    0x28U, 0x73U, 0x2AU, 0xC1U, 0x1FU, 0xF8U, 0xD2U, 0x11U,
    0xBAU, 0x4BU, 0x00U, 0xA0U, 0xC9U, 0x3EU, 0xC9U, 0x3BU
};

static const unsigned char installer_discovery_linux_guid[16U] = {
    0xAFU, 0x3DU, 0xC6U, 0x0FU, 0x83U, 0x84U, 0x72U, 0x47U,
    0x8EU, 0x79U, 0x3DU, 0x69U, 0xD8U, 0x47U, 0x7DU, 0xE4U
};

static const unsigned char installer_discovery_windows_guid[16U] = {
    0xA2U, 0xA0U, 0xD0U, 0xEBU, 0xE5U, 0xB9U, 0x33U, 0x44U,
    0x87U, 0xC0U, 0x68U, 0xB6U, 0xB7U, 0x26U, 0x99U, 0xC7U
};

static const unsigned char installer_discovery_swap_guid[16U] = {
    0x65U, 0x57U, 0x81U, 0x06U, 0x27U, 0x94U, 0x2FU, 0x4BU,
    0xA4U, 0xDFU, 0xD8U, 0xEFU, 0x8EU, 0x9AU, 0x8BU, 0xD1U
};

static enum installer_discovery_status installer_discovery_report_check(
    const struct installer_discovery_report *report)
{
    if (report == NULL) {
        return INSTALLER_DISCOVERY_NULL;
    }
    if (report->version != INSTALLER_HARDWARE_DISCOVERY_VERSION) {
        return INSTALLER_DISCOVERY_BAD_VERSION;
    }
    if (!report->read_only) {
        return INSTALLER_DISCOVERY_READ_ONLY_VIOLATION;
    }
    return INSTALLER_DISCOVERY_OK;
}

static uint32_t installer_discovery_load_le32(const unsigned char *bytes)
{
    return (uint32_t)bytes[0] |
           ((uint32_t)bytes[1] << 8U) |
           ((uint32_t)bytes[2] << 16U) |
           ((uint32_t)bytes[3] << 24U);
}

static uint64_t installer_discovery_load_le64(const unsigned char *bytes)
{
    uint64_t value = 0U;
    size_t index;

    for (index = 0U; index < 8U; ++index) {
        value |= (uint64_t)bytes[index] << (8U * index);
    }
    return value;
}

static bool installer_discovery_identity_valid(
    const char *identity,
    size_t identity_length)
{
    size_t index;

    if (identity == NULL || identity_length == 0U) {
        return false;
    }
    if (identity_length >= INSTALLER_DISCOVERY_IDENTITY_LENGTH) {
        return false;
    }
    for (index = 0U; index < identity_length; ++index) {
        const unsigned char value = (unsigned char)identity[index];

        if (value == 0U ||
            value < 0x20U ||
            value > 0x7EU) {
            return false;
        }
    }
    return true;
}

static void installer_discovery_identity_copy(
    char destination[INSTALLER_DISCOVERY_IDENTITY_LENGTH],
    const char *identity,
    size_t identity_length)
{
    size_t index;

    destination[0] = '\0';
    if (identity_length >= INSTALLER_DISCOVERY_IDENTITY_LENGTH) {
        identity_length = INSTALLER_DISCOVERY_IDENTITY_LENGTH - 1U;
    }
    for (index = 0U; index < identity_length; ++index) {
        destination[index] = identity[index];
    }
    destination[identity_length] = '\0';
}

static bool installer_discovery_fixed_identity_valid(
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

static bool installer_discovery_mac_valid(const uint8_t mac[6U])
{
    size_t index;
    bool nonzero = false;

    for (index = 0U; index < 6U; ++index) {
        if (mac[index] != 0U) {
            nonzero = true;
        }
    }
    return nonzero && (mac[0] & 1U) == 0U;
}

static bool installer_discovery_guid_zero(
    const unsigned char guid[16U])
{
    size_t index;

    for (index = 0U; index < 16U; ++index) {
        if (guid[index] != 0U) {
            return false;
        }
    }
    return true;
}

static bool installer_discovery_guid_equal(
    const unsigned char left[16U],
    const unsigned char right[16U])
{
    size_t index;

    for (index = 0U; index < 16U; ++index) {
        if (left[index] != right[index]) {
            return false;
        }
    }
    return true;
}

static enum installer_partition_kind
installer_discovery_gpt_partition_kind(
    const unsigned char guid[16U],
    bool *efi_system)
{
    if (efi_system != NULL) {
        *efi_system = false;
    }
    if (installer_discovery_guid_equal(
            guid, installer_discovery_efi_guid)) {
        if (efi_system != NULL) {
            *efi_system = true;
        }
        return INSTALLER_PARTITION_EFI_SYSTEM;
    }
    if (installer_discovery_guid_equal(
            guid, installer_discovery_linux_guid)) {
        return INSTALLER_PARTITION_LINUX_ROOT;
    }
    if (installer_discovery_guid_equal(
            guid, installer_discovery_windows_guid)) {
        return INSTALLER_PARTITION_WINDOWS;
    }
    if (installer_discovery_guid_equal(
            guid, installer_discovery_swap_guid)) {
        return INSTALLER_PARTITION_LINUX_SWAP;
    }
    return INSTALLER_PARTITION_OTHER;
}

/*
 * ITEM-501: the GPT type GUID of a Linux LVM physical volume.  Fedora's default
 * install puts the whole system in a volume group, so this is the row that used
 * to read as one anonymous "Linux/unknown" partition.
 */
static const unsigned char installer_discovery_lvm_guid[16U] = {
    0x79U, 0xD3U, 0xD6U, 0xE6U, 0x07U, 0xF5U, 0xC2U, 0x44U,
    0xA2U, 0x3CU, 0x23U, 0x8FU, 0x2AU, 0x3DU, 0xF9U, 0x28U
};

/* The Btrfs superblock magic lives at byte 0x10040 of the filesystem. */
#define INSTALLER_DISCOVERY_BTRFS_MAGIC_OFFSET UINT64_C(0x10040)
static const unsigned char installer_discovery_btrfs_magic[8U] = {
    '_', 'B', 'H', 'R', 'f', 'S', '_', 'M'
};

/*
 * ITEM-501: a Btrfs filesystem and an ext4 filesystem share the same GPT type
 * GUID, so the type alone cannot name the layout.  The superblock magic can,
 * and a partition shorter than the superblock is simply not a Btrfs.
 */
static enum installer_partition_layout
installer_discovery_probe_btrfs(
    struct block_device *device,
    uint64_t first_lba,
    uint64_t length_lba)
{
    /*
     * The block layer reads whole blocks, so the superblock's block is read
     * and the magic is compared inside it.
     */
    unsigned char block[INSTALLER_DISCOVERY_BLOCK_SIZE];
    const uint64_t magic_lba =
        first_lba +
        INSTALLER_DISCOVERY_BTRFS_MAGIC_OFFSET /
            INSTALLER_DISCOVERY_BLOCK_SIZE;
    const size_t magic_offset = (size_t)(
        INSTALLER_DISCOVERY_BTRFS_MAGIC_OFFSET %
        INSTALLER_DISCOVERY_BLOCK_SIZE);

    if (device == NULL ||
        length_lba <=
            INSTALLER_DISCOVERY_BTRFS_MAGIC_OFFSET /
                INSTALLER_DISCOVERY_BLOCK_SIZE) {
        return INSTALLER_PARTITION_LAYOUT_PLAIN;
    }
    if (block_device_read(
            device, magic_lba * INSTALLER_DISCOVERY_BLOCK_SIZE, block,
            sizeof(block)) != BLOCK_DEVICE_OK) {
        return INSTALLER_PARTITION_LAYOUT_PLAIN;
    }
    if (memcmp(block + magic_offset, installer_discovery_btrfs_magic,
               sizeof(installer_discovery_btrfs_magic)) == 0) {
        return INSTALLER_PARTITION_LAYOUT_BTRFS;
    }
    return INSTALLER_PARTITION_LAYOUT_PLAIN;
}

static enum installer_partition_kind
installer_discovery_mbr_partition_kind(
    uint8_t type,
    bool *efi_system)
{
    if (efi_system != NULL) {
        *efi_system = false;
    }
    switch (type) {
    case 0xEFU:
        if (efi_system != NULL) {
            *efi_system = true;
        }
        return INSTALLER_PARTITION_EFI_SYSTEM;
    case 0x83U:
        return INSTALLER_PARTITION_LINUX_ROOT;
    case 0x82U:
        return INSTALLER_PARTITION_LINUX_SWAP;
    case 0x07U:
        return INSTALLER_PARTITION_WINDOWS;
    default:
        return INSTALLER_PARTITION_OTHER;
    }
}

static enum installer_os_kind installer_discovery_os_kind(
    enum installer_partition_kind kind)
{
    switch (kind) {
    case INSTALLER_PARTITION_LINUX_ROOT:
        return INSTALLER_OS_LINUX;
    case INSTALLER_PARTITION_WINDOWS:
        return INSTALLER_OS_WINDOWS;
    default:
        return INSTALLER_OS_UNKNOWN;
    }
}

static bool installer_discovery_all_zero(
    const unsigned char *bytes,
    size_t length)
{
    size_t index;

    for (index = 0U; index < length; ++index) {
        if (bytes[index] != 0U) {
            return false;
        }
    }
    return true;
}

static bool installer_discovery_partitions_overlap(
    const struct installer_discovery_partition *left,
    const struct installer_discovery_partition *right)
{
    if (!left->present || !right->present ||
        left->disk_id != right->disk_id) {
        return false;
    }
    return left->first_lba < right->first_lba + right->length_lba &&
           right->first_lba < left->first_lba + left->length_lba;
}

static size_t installer_discovery_disk_index(
    const struct installer_discovery_report *report,
    uint32_t disk_id)
{
    size_t index;

    for (index = 0U; index < INSTALLER_DISCOVERY_DISK_CAPACITY; ++index) {
        if (report->disks[index].present &&
            report->disks[index].id == disk_id) {
            return index;
        }
    }
    return INSTALLER_DISCOVERY_DISK_CAPACITY;
}

static size_t installer_discovery_next_free_disk(
    const struct installer_discovery_report *report)
{
    size_t index;

    for (index = 0U;
         index < INSTALLER_DISCOVERY_DISK_CAPACITY;
         ++index) {
        if (!report->disks[index].present) {
            return index;
        }
    }
    return INSTALLER_DISCOVERY_DISK_CAPACITY;
}

static size_t installer_discovery_next_free_partition(
    const struct installer_discovery_report *report)
{
    size_t index;

    for (index = 0U;
         index < INSTALLER_DISCOVERY_PARTITION_CAPACITY;
         ++index) {
        if (!report->partitions[index].present) {
            return index;
        }
    }
    return INSTALLER_DISCOVERY_PARTITION_CAPACITY;
}

static size_t installer_discovery_next_free_os(
    const struct installer_discovery_report *report)
{
    size_t index;

    for (index = 0U;
         index < INSTALLER_DISCOVERY_OS_CAPACITY;
         ++index) {
        if (!report->oss[index].present) {
            return index;
        }
    }
    return INSTALLER_DISCOVERY_OS_CAPACITY;
}

static bool installer_discovery_os_id_in_use(
    const struct installer_discovery_report *report,
    uint32_t os_id)
{
    size_t index;

    for (index = 0U;
         index < INSTALLER_DISCOVERY_OS_CAPACITY;
         ++index) {
        if (report->oss[index].present &&
            report->oss[index].id == os_id) {
            return true;
        }
    }
    return false;
}

static size_t installer_discovery_next_free_nic(
    const struct installer_discovery_report *report)
{
    size_t index;

    for (index = 0U;
         index < INSTALLER_DISCOVERY_NIC_CAPACITY;
         ++index) {
        if (!report->nics[index].present) {
            return index;
        }
    }
    return INSTALLER_DISCOVERY_NIC_CAPACITY;
}

static size_t installer_discovery_next_free_pci(
    const struct installer_discovery_report *report)
{
    size_t index;

    for (index = 0U;
         index < INSTALLER_DISCOVERY_PCI_CAPACITY;
         ++index) {
        if (!report->pcis[index].present) {
            return index;
        }
    }
    return INSTALLER_DISCOVERY_PCI_CAPACITY;
}

static enum installer_discovery_status installer_discovery_append_partition(
    struct installer_discovery_report *report,
    const struct installer_discovery_partition *partition)
{
    size_t index;
    size_t slot;

    if (report == NULL || partition == NULL) {
        return INSTALLER_DISCOVERY_NULL;
    }
    if (!partition->present) {
        return INSTALLER_DISCOVERY_INVALID_ARGUMENT;
    }
    for (index = 0U;
         index < INSTALLER_DISCOVERY_PARTITION_CAPACITY;
         ++index) {
        if (report->partitions[index].present &&
            (report->partitions[index].id == partition->id ||
             (report->partitions[index].disk_id == partition->disk_id &&
              report->partitions[index].partition_index ==
                  partition->partition_index))) {
            return INSTALLER_DISCOVERY_ALREADY_PUBLISHED;
        }
    }
    slot = installer_discovery_next_free_partition(report);
    if (slot == INSTALLER_DISCOVERY_PARTITION_CAPACITY) {
        return INSTALLER_DISCOVERY_TOO_MANY_PARTITIONS;
    }
    report->partitions[slot] = *partition;
    report->partitions[slot].present = true;
    ++report->partition_count;
    return INSTALLER_DISCOVERY_OK;
}

static enum installer_discovery_status
installer_discovery_append_parsed_partitions(
    struct installer_discovery_report *report,
    const struct installer_discovery_partition *partitions,
    size_t partition_count)
{
    enum installer_discovery_status status;
    size_t index;

    if (partition_count >
        INSTALLER_DISCOVERY_PARTITION_CAPACITY -
            report->partition_count) {
        return INSTALLER_DISCOVERY_TOO_MANY_PARTITIONS;
    }
    for (index = 0U; index < partition_count; ++index) {
        status = installer_discovery_append_partition(
            report, &partitions[index]);
        if (status != INSTALLER_DISCOVERY_OK) {
            return status;
        }
    }
    return INSTALLER_DISCOVERY_OK;
}

static bool installer_discovery_mbr_extended(uint8_t type)
{
    return type == INSTALLER_DISCOVERY_MBR_EXTENDED_FIRST ||
           type == INSTALLER_DISCOVERY_MBR_EXTENDED_LBA ||
           type == INSTALLER_DISCOVERY_MBR_EXTENDED_LINUX;
}

/*
 * ITEM-501: read the extended partition's chain of EBR sectors and publish
 * every logical partition it holds.  Each EBR names one logical partition
 * (relative to its own sector) and the next EBR (relative to the start of the
 * extended partition); the walk is bounded and a table it cannot read, or a
 * chain longer than the bound, is refused - never truncated silently.
 */
static enum installer_discovery_status
installer_discovery_parse_ebr_chain(
    struct block_device *device,
    struct installer_discovery_report *report,
    uint32_t disk_id,
    uint64_t extended_lba,
    struct installer_discovery_partition *parsed,
    size_t *parsed_count)
{
    unsigned char ebr[INSTALLER_DISCOVERY_BLOCK_SIZE];
    uint64_t next_lba = extended_lba;
    uint32_t next_id = report->partition_count + 1U + (uint32_t)*parsed_count;
    size_t logical_index = 0U;

    while (next_lba != 0U) {
        size_t entry;
        uint32_t logical_lba = 0U;
        uint32_t logical_length = 0U;
        uint8_t logical_type = 0x00U;
        uint32_t link_lba = 0U;

        if (logical_index >= INSTALLER_DISCOVERY_EBR_MAX) {
            return INSTALLER_DISCOVERY_TOO_MANY_PARTITIONS;
        }
        if (block_device_read(
                device, next_lba * INSTALLER_DISCOVERY_BLOCK_SIZE, ebr,
                sizeof(ebr)) != BLOCK_DEVICE_OK) {
            return INSTALLER_DISCOVERY_IO_ERROR;
        }
        if (ebr[INSTALLER_DISCOVERY_MBR_SIGNATURE_OFFSET] != 0x55U ||
            ebr[INSTALLER_DISCOVERY_MBR_SIGNATURE_OFFSET + 1U] != 0xAAU) {
            return INSTALLER_DISCOVERY_INVALID_PARTITION_TABLE;
        }
        for (entry = 0U; entry < INSTALLER_DISCOVERY_MBR_PARTITION_COUNT;
             ++entry) {
            const size_t offset = INSTALLER_DISCOVERY_MBR_PARTITION_OFFSET +
                entry * INSTALLER_DISCOVERY_MBR_PARTITION_SIZE;
            const uint8_t type = ebr[offset + 4U];
            const uint32_t first_lba = installer_discovery_load_le32(
                ebr + offset + 8U);
            const uint32_t length_lba = installer_discovery_load_le32(
                ebr + offset + 12U);

            if (type == 0x00U || first_lba == 0U || length_lba == 0U) {
                continue;
            }
            if (entry == 0U) {
                logical_type = type;
                logical_lba = first_lba;
                logical_length = length_lba;
            } else if (entry == 1U &&
                       installer_discovery_mbr_extended(type)) {
                link_lba = first_lba;
            }
        }
        if (logical_type != 0x00U && logical_length != 0U &&
            !installer_discovery_mbr_extended(logical_type)) {
            bool efi_system = false;
            const enum installer_partition_kind kind =
                installer_discovery_mbr_partition_kind(logical_type,
                                                       &efi_system);

            if (*parsed_count >= INSTALLER_DISCOVERY_PARTITION_CAPACITY) {
                return INSTALLER_DISCOVERY_TOO_MANY_PARTITIONS;
            }
            memset(&parsed[*parsed_count], 0, sizeof(parsed[*parsed_count]));
            parsed[*parsed_count].id = next_id;
            parsed[*parsed_count].present = true;
            parsed[*parsed_count].disk_id = disk_id;
            parsed[*parsed_count].partition_index =
                5U + (uint32_t)logical_index;
            parsed[*parsed_count].first_lba =
                next_lba + (uint64_t)logical_lba;
            parsed[*parsed_count].length_lba = logical_length;
            parsed[*parsed_count].kind = kind;
            parsed[*parsed_count].owner = installer_discovery_os_kind(kind);
            parsed[*parsed_count].efi_system = efi_system;
            parsed[*parsed_count].layout =
                INSTALLER_PARTITION_LAYOUT_LOGICAL;
            ++*parsed_count;
            ++next_id;
            ++logical_index;
        }
        next_lba = link_lba != 0U ? extended_lba + (uint64_t)link_lba : 0U;
    }
    return INSTALLER_DISCOVERY_OK;
}

static enum installer_discovery_status
installer_discovery_parse_mbr(
    struct block_device *device,
    const unsigned char *sector,
    struct installer_discovery_report *report,
    uint32_t disk_id,
    struct installer_discovery_partition *parsed,
    size_t *parsed_count)
{
    size_t count = 0U;
    size_t partition_index;
    uint64_t extended_lba = 0U;
    uint32_t next_id = report->partition_count + 1U;

    if (sector[INSTALLER_DISCOVERY_MBR_SIGNATURE_OFFSET] != 0x55U ||
        sector[INSTALLER_DISCOVERY_MBR_SIGNATURE_OFFSET + 1U] != 0xAAU) {
        return INSTALLER_DISCOVERY_INVALID_PARTITION_TABLE;
    }

    for (partition_index = 0U;
         partition_index < INSTALLER_DISCOVERY_MBR_PARTITION_COUNT;
         ++partition_index) {
        const size_t offset = INSTALLER_DISCOVERY_MBR_PARTITION_OFFSET +
            partition_index * INSTALLER_DISCOVERY_MBR_PARTITION_SIZE;
        const uint8_t type = sector[offset + 4U];
        const uint32_t first_lba = installer_discovery_load_le32(
            sector + offset + 8U);
        const uint32_t length_lba = installer_discovery_load_le32(
            sector + offset + 12U);
        bool efi_system = false;
        enum installer_partition_kind kind;

        if (type == 0xEEU) {
            return INSTALLER_DISCOVERY_UNSUPPORTED_PARTITION_TABLE;
        }
        if (type == 0x00U || first_lba == 0U || length_lba == 0U) {
            continue;
        }
        if (first_lba > report->disks[
                installer_discovery_disk_index(report, disk_id)].block_count ||
            length_lba >
                report->disks[
                    installer_discovery_disk_index(report, disk_id)]
                    .block_count - first_lba) {
            return INSTALLER_DISCOVERY_INVALID_PARTITION_TABLE;
        }
        if (count >= INSTALLER_DISCOVERY_PARTITION_CAPACITY) {
            return INSTALLER_DISCOVERY_TOO_MANY_PARTITIONS;
        }
        kind = installer_discovery_mbr_partition_kind(
            type, &efi_system);
        memset(&parsed[count], 0, sizeof(parsed[count]));
        parsed[count].id = next_id + (uint32_t)count;
        parsed[count].present = true;
        parsed[count].disk_id = disk_id;
        parsed[count].partition_index = (uint32_t)partition_index + 1U;
        parsed[count].first_lba = first_lba;
        parsed[count].length_lba = length_lba;
        parsed[count].kind = kind;
        parsed[count].owner = installer_discovery_os_kind(kind);
        parsed[count].efi_system = efi_system;
        if (installer_discovery_mbr_extended(type)) {
            /* The container itself is a table, not a target. */
            parsed[count].layout = INSTALLER_PARTITION_LAYOUT_EXTENDED;
            parsed[count].container = true;
            if (extended_lba == 0U) {
                extended_lba = first_lba;
            }
        }
        ++count;
    }
    if (extended_lba != 0U) {
        const enum installer_discovery_status chain_status =
            installer_discovery_parse_ebr_chain(
                device, report, disk_id, extended_lba, parsed, &count);

        if (chain_status != INSTALLER_DISCOVERY_OK) {
            return chain_status;
        }
    }

    *parsed_count = count;
    return INSTALLER_DISCOVERY_OK;
}

static enum installer_discovery_status
installer_discovery_parse_gpt(
    struct block_device *device,
    struct installer_discovery_report *report,
    uint32_t disk_id,
    struct installer_discovery_partition *parsed,
    size_t *parsed_count)
{
    unsigned char header[INSTALLER_DISCOVERY_BLOCK_SIZE];
    unsigned char entries[INSTALLER_DISCOVERY_BLOCK_SIZE];
    enum block_device_status io_status;
    uint32_t header_size;
    uint32_t entry_count;
    uint32_t entry_size;
    uint64_t entry_lba;
    uint64_t current_entry_lba = UINT64_MAX;
    size_t count = 0U;
    uint32_t partition_index;
    uint32_t next_id = report->partition_count + 1U;
    size_t index;
    size_t disk_index;

    disk_index = installer_discovery_disk_index(report, disk_id);
    if (disk_index == INSTALLER_DISCOVERY_DISK_CAPACITY ||
        report->disks[disk_index].block_count < 2U) {
        return INSTALLER_DISCOVERY_INVALID_PARTITION_TABLE;
    }
    io_status = block_device_read(device, INSTALLER_DISCOVERY_BLOCK_SIZE,
                                  header, sizeof(header));
    if (io_status != BLOCK_DEVICE_OK) {
        return INSTALLER_DISCOVERY_IO_ERROR;
    }
    for (index = 0U; index < 8U; ++index) {
        if (header[index] != installer_discovery_gpt_signature[index]) {
            return INSTALLER_DISCOVERY_INVALID_PARTITION_TABLE;
        }
    }
    header_size = installer_discovery_load_le32(
        header + INSTALLER_DISCOVERY_GPT_HEADER_SIZE_OFFSET);
    entry_lba = installer_discovery_load_le64(
        header + INSTALLER_DISCOVERY_GPT_ENTRY_LBA_OFFSET);
    entry_count = installer_discovery_load_le32(
        header + INSTALLER_DISCOVERY_GPT_ENTRY_COUNT_OFFSET);
    entry_size = installer_discovery_load_le32(
        header + INSTALLER_DISCOVERY_GPT_ENTRY_SIZE_OFFSET);

    if (header_size < 92U ||
        entry_size != INSTALLER_DISCOVERY_GPT_ENTRY_SIZE ||
        entry_lba == 0U ||
        entry_lba >= report->disks[disk_index].block_count) {
        return INSTALLER_DISCOVERY_INVALID_PARTITION_TABLE;
    }

    for (partition_index = 0U;
         partition_index < entry_count;
         ++partition_index) {
        const uint64_t entry_sector =
            entry_lba +
            (uint64_t)(partition_index /
                       INSTALLER_DISCOVERY_GPT_ENTRIES_PER_BLOCK);
        const size_t offset = (size_t)(
            partition_index % INSTALLER_DISCOVERY_GPT_ENTRIES_PER_BLOCK) *
            INSTALLER_DISCOVERY_GPT_ENTRY_SIZE;
        const unsigned char *type_guid;
        uint64_t first_lba;
        uint64_t last_lba;
        bool efi_system = false;
        enum installer_partition_kind kind;
        const struct installer_discovery_disk *disk;

        if (entry_sector >= report->disks[disk_index].block_count) {
            return INSTALLER_DISCOVERY_INVALID_PARTITION_TABLE;
        }
        if (entry_sector != current_entry_lba) {
            io_status = block_device_read(
                device, entry_sector * INSTALLER_DISCOVERY_BLOCK_SIZE,
                entries, sizeof(entries));
            if (io_status != BLOCK_DEVICE_OK) {
                return INSTALLER_DISCOVERY_IO_ERROR;
            }
            current_entry_lba = entry_sector;
        }
        type_guid = entries + offset;
        if (installer_discovery_guid_zero(type_guid)) {
            continue;
        }
        first_lba = installer_discovery_load_le64(entries + offset + 32U);
        last_lba = installer_discovery_load_le64(entries + offset + 40U);
        disk = &report->disks[
            installer_discovery_disk_index(report, disk_id)];
        if (first_lba == 0U ||
            last_lba < first_lba ||
            last_lba >= disk->block_count) {
            return INSTALLER_DISCOVERY_INVALID_PARTITION_TABLE;
        }
        kind = installer_discovery_gpt_partition_kind(
            type_guid, &efi_system);
        if (count >= INSTALLER_DISCOVERY_PARTITION_CAPACITY) {
            return INSTALLER_DISCOVERY_TOO_MANY_PARTITIONS;
        }
        memset(&parsed[count], 0, sizeof(parsed[count]));
        parsed[count].id = next_id + (uint32_t)count;
        parsed[count].present = true;
        parsed[count].disk_id = disk_id;
        parsed[count].partition_index = partition_index + 1U;
        parsed[count].first_lba = first_lba;
        parsed[count].length_lba = last_lba - first_lba + 1U;
        parsed[count].kind = kind;
        parsed[count].owner = installer_discovery_os_kind(kind);
        parsed[count].efi_system = efi_system;
        /*
         * ITEM-501: name what the type GUID alone cannot.  An LVM physical
         * volume is its own layout; a Linux filesystem whose superblock says
         * Btrfs is a Btrfs, not a plain partition.
         */
        if (installer_discovery_guid_equal(
                type_guid, installer_discovery_lvm_guid)) {
            parsed[count].layout = INSTALLER_PARTITION_LAYOUT_LVM;
        } else if (kind == INSTALLER_PARTITION_LINUX_ROOT) {
            parsed[count].layout = installer_discovery_probe_btrfs(
                device, first_lba, last_lba - first_lba + 1U);
        }
        ++count;
    }

    *parsed_count = count;
    return INSTALLER_DISCOVERY_OK;
}

enum installer_discovery_status installer_discovery_init(
    struct installer_discovery_report *report)
{
    if (report == NULL) {
        return INSTALLER_DISCOVERY_NULL;
    }
    memset(report, 0, sizeof(*report));
    report->version = INSTALLER_HARDWARE_DISCOVERY_VERSION;
    report->firmware_mode = INSTALLER_FIRMWARE_UNKNOWN;
    report->read_only = true;
    return INSTALLER_DISCOVERY_OK;
}

enum installer_discovery_status installer_discovery_publish_firmware(
    struct installer_discovery_report *report,
    enum installer_firmware_mode mode)
{
    enum installer_discovery_status status;

    status = installer_discovery_report_check(report);
    if (status != INSTALLER_DISCOVERY_OK) {
        return status;
    }
    if (mode != INSTALLER_FIRMWARE_UEFI &&
        mode != INSTALLER_FIRMWARE_BIOS_LEGACY) {
        return INSTALLER_DISCOVERY_INVALID_ARGUMENT;
    }
    if (report->firmware_mode != INSTALLER_FIRMWARE_UNKNOWN &&
        report->firmware_mode != mode) {
        return INSTALLER_DISCOVERY_ALREADY_PUBLISHED;
    }
    report->firmware_mode = mode;
    return INSTALLER_DISCOVERY_OK;
}

enum installer_discovery_status installer_discovery_publish_cpu(
    struct installer_discovery_report *report,
    uint32_t cpu_cores)
{
    enum installer_discovery_status status;

    status = installer_discovery_report_check(report);
    if (status != INSTALLER_DISCOVERY_OK) {
        return status;
    }
    if (cpu_cores == 0U || cpu_cores > 4096U) {
        return INSTALLER_DISCOVERY_OUT_OF_RANGE;
    }
    if (report->cpu_present &&
        report->cpu_cores != cpu_cores) {
        return INSTALLER_DISCOVERY_ALREADY_PUBLISHED;
    }
    report->cpu_present = true;
    report->cpu_cores = cpu_cores;
    return INSTALLER_DISCOVERY_OK;
}

enum installer_discovery_status installer_discovery_publish_memory(
    struct installer_discovery_report *report,
    uint64_t ram_bytes)
{
    enum installer_discovery_status status;

    status = installer_discovery_report_check(report);
    if (status != INSTALLER_DISCOVERY_OK) {
        return status;
    }
    if (ram_bytes == 0U) {
        return INSTALLER_DISCOVERY_OUT_OF_RANGE;
    }
    if (report->ram_present && report->ram_bytes != ram_bytes) {
        return INSTALLER_DISCOVERY_ALREADY_PUBLISHED;
    }
    report->ram_present = true;
    report->ram_bytes = ram_bytes;
    return INSTALLER_DISCOVERY_OK;
}

enum installer_discovery_status installer_discovery_publish_disk(
    struct installer_discovery_report *report,
    const struct installer_discovery_disk *disk)
{
    enum installer_discovery_status status;
    size_t slot;

    status = installer_discovery_report_check(report);
    if (status != INSTALLER_DISCOVERY_OK) {
        return status;
    }
    if (disk == NULL) {
        return INSTALLER_DISCOVERY_NULL;
    }
    if (!disk->present ||
        disk->id == 0U ||
        disk->id > INSTALLER_DISCOVERY_DISK_CAPACITY ||
        disk->block_size < 512U ||
        (disk->block_size & (disk->block_size - 1U)) != 0U ||
        disk->block_count == 0U ||
        disk->controller > INSTALLER_DISK_CONTROLLER_NVME ||
        disk->partition_table > INSTALLER_PARTITION_TABLE_GPT ||
        !installer_discovery_fixed_identity_valid(
            disk->identity, sizeof(disk->identity))) {
        return INSTALLER_DISCOVERY_INVALID_ARGUMENT;
    }
    if (installer_discovery_disk_index(report, disk->id) !=
        INSTALLER_DISCOVERY_DISK_CAPACITY) {
        return INSTALLER_DISCOVERY_ALREADY_PUBLISHED;
    }
    slot = installer_discovery_next_free_disk(report);
    if (slot == INSTALLER_DISCOVERY_DISK_CAPACITY) {
        return INSTALLER_DISCOVERY_OUT_OF_RANGE;
    }
    report->disks[slot] = *disk;
    ++report->disk_count;
    return INSTALLER_DISCOVERY_OK;
}

enum installer_discovery_status installer_discovery_publish_partition(
    struct installer_discovery_report *report,
    const struct installer_discovery_partition *partition)
{
    enum installer_discovery_status status;
    struct installer_discovery_partition candidate;
    enum installer_os_kind expected_owner;
    size_t disk_index;
    struct installer_discovery_disk *disk;

    status = installer_discovery_report_check(report);
    if (status != INSTALLER_DISCOVERY_OK) {
        return status;
    }
    if (partition == NULL) {
        return INSTALLER_DISCOVERY_NULL;
    }
    if (!partition->present ||
        partition->id == 0U ||
        partition->disk_id == 0U ||
        partition->partition_index == 0U ||
        partition->first_lba == 0U ||
        partition->length_lba == 0U ||
        partition->kind <= INSTALLER_PARTITION_UNKNOWN ||
        partition->kind > INSTALLER_PARTITION_OTHER ||
        partition->owner > INSTALLER_OS_OTHER) {
        return INSTALLER_DISCOVERY_INVALID_ARGUMENT;
    }
    expected_owner = installer_discovery_os_kind(partition->kind);
    if (partition->owner != INSTALLER_OS_UNKNOWN &&
        partition->owner != expected_owner) {
        return INSTALLER_DISCOVERY_INVALID_ARGUMENT;
    }
    disk_index = installer_discovery_disk_index(
        report, partition->disk_id);
    if (disk_index == INSTALLER_DISCOVERY_DISK_CAPACITY) {
        return INSTALLER_DISCOVERY_INVALID_ARGUMENT;
    }
    disk = &report->disks[disk_index];
    if (partition->first_lba > disk->block_count ||
        partition->length_lba > disk->block_count - partition->first_lba) {
        return INSTALLER_DISCOVERY_OUT_OF_RANGE;
    }
    if (partition->efi_system &&
        partition->kind != INSTALLER_PARTITION_EFI_SYSTEM) {
        return INSTALLER_DISCOVERY_INVALID_ARGUMENT;
    }
    candidate = *partition;
    candidate.owner = expected_owner;
    status = installer_discovery_append_partition(report, &candidate);
    if (status == INSTALLER_DISCOVERY_OK &&
        candidate.efi_system) {
        disk->has_efi_system_partition = true;
    }
    return status;
}

enum installer_discovery_status installer_discovery_publish_nic(
    struct installer_discovery_report *report,
    const struct installer_discovery_nic *nic)
{
    enum installer_discovery_status status;
    size_t slot;
    size_t index;

    status = installer_discovery_report_check(report);
    if (status != INSTALLER_DISCOVERY_OK) {
        return status;
    }
    if (nic == NULL) {
        return INSTALLER_DISCOVERY_NULL;
    }
    if (!nic->present ||
        nic->id == 0U ||
        nic->id > INSTALLER_DISCOVERY_NIC_CAPACITY ||
        nic->vendor_id == 0U ||
        nic->product_id == 0U ||
        !installer_discovery_mac_valid(nic->mac_address)) {
        return INSTALLER_DISCOVERY_INVALID_ARGUMENT;
    }
    for (index = 0U; index < INSTALLER_DISCOVERY_NIC_CAPACITY; ++index) {
        if (report->nics[index].present &&
            report->nics[index].id == nic->id) {
            return INSTALLER_DISCOVERY_ALREADY_PUBLISHED;
        }
    }
    slot = installer_discovery_next_free_nic(report);
    if (slot == INSTALLER_DISCOVERY_NIC_CAPACITY) {
        return INSTALLER_DISCOVERY_OUT_OF_RANGE;
    }
    report->nics[slot] = *nic;
    ++report->nic_count;
    return INSTALLER_DISCOVERY_OK;
}

enum installer_discovery_status installer_discovery_publish_pci(
    struct installer_discovery_report *report,
    const struct installer_discovery_pci *pci)
{
    enum installer_discovery_status status;
    size_t slot;
    size_t index;
    enum installer_pci_category inferred;

    status = installer_discovery_report_check(report);
    if (status != INSTALLER_DISCOVERY_OK) {
        return status;
    }
    if (pci == NULL) {
        return INSTALLER_DISCOVERY_NULL;
    }
    if (!pci->present ||
        pci->id == 0U ||
        pci->id > INSTALLER_DISCOVERY_PCI_CAPACITY ||
        pci->vendor_id == 0U ||
        pci->product_id == 0U ||
        pci->bus_address > 0xFFFFU ||
        pci->class_code == 0U) {
        return INSTALLER_DISCOVERY_INVALID_ARGUMENT;
    }
    inferred = INSTALLER_PCI_OTHER;
    if ((pci->class_code >> 8U) == 0x01U) {
        inferred = INSTALLER_PCI_STORAGE;
    } else if ((pci->class_code >> 8U) == 0x02U) {
        inferred = INSTALLER_PCI_NETWORK;
    } else if ((pci->class_code >> 8U) == 0x03U) {
        inferred = INSTALLER_PCI_DISPLAY;
    }
    if (pci->category > INSTALLER_PCI_OTHER ||
        (pci->category != INSTALLER_PCI_UNKNOWN &&
         pci->category != inferred)) {
        return INSTALLER_DISCOVERY_INVALID_ARGUMENT;
    }
    for (index = 0U; index < INSTALLER_DISCOVERY_PCI_CAPACITY; ++index) {
        if (report->pcis[index].present &&
            report->pcis[index].id == pci->id) {
            return INSTALLER_DISCOVERY_ALREADY_PUBLISHED;
        }
    }
    slot = installer_discovery_next_free_pci(report);
    if (slot == INSTALLER_DISCOVERY_PCI_CAPACITY) {
        return INSTALLER_DISCOVERY_OUT_OF_RANGE;
    }
    report->pcis[slot] = *pci;
    report->pcis[slot].category = inferred;
    ++report->pci_count;
    return INSTALLER_DISCOVERY_OK;
}

enum installer_discovery_status installer_discovery_publish_os(
    struct installer_discovery_report *report,
    const struct installer_discovery_os *os)
{
    enum installer_discovery_status status;
    size_t slot;
    size_t index;
    bool boot_path_valid;

    status = installer_discovery_report_check(report);
    if (status != INSTALLER_DISCOVERY_OK) {
        return status;
    }
    if (os == NULL) {
        return INSTALLER_DISCOVERY_NULL;
    }
    if (!os->present ||
        os->id == 0U ||
        os->id > INSTALLER_DISCOVERY_OS_CAPACITY ||
        os->kind <= INSTALLER_OS_UNKNOWN ||
        os->kind > INSTALLER_OS_OTHER) {
        return INSTALLER_DISCOVERY_INVALID_ARGUMENT;
    }
    boot_path_valid = os->boot_path[0] == '\0';
    if (!boot_path_valid) {
        boot_path_valid = installer_discovery_fixed_identity_valid(
            os->boot_path, sizeof(os->boot_path));
    }
    if (!boot_path_valid) {
        return INSTALLER_DISCOVERY_INVALID_ARGUMENT;
    }
    for (index = 0U; index < INSTALLER_DISCOVERY_OS_CAPACITY; ++index) {
        if (report->oss[index].present &&
            (report->oss[index].id == os->id ||
             (report->oss[index].kind == os->kind &&
              report->oss[index].partition_id == os->partition_id &&
              os->partition_id != 0U))) {
            return INSTALLER_DISCOVERY_ALREADY_PUBLISHED;
        }
    }
    slot = installer_discovery_next_free_os(report);
    if (slot == INSTALLER_DISCOVERY_OS_CAPACITY) {
        return INSTALLER_DISCOVERY_OUT_OF_RANGE;
    }
    report->oss[slot] = *os;
    ++report->os_count;
    return INSTALLER_DISCOVERY_OK;
}

enum installer_discovery_status installer_discovery_scan_block_device(
    struct installer_discovery_report *report,
    struct block_device *device,
    uint32_t disk_id,
    enum installer_disk_controller_kind controller,
    const char *identity,
    size_t identity_length)
{
    struct installer_discovery_disk disk;
    struct installer_discovery_partition parsed[
        INSTALLER_DISCOVERY_PARTITION_CAPACITY];
    enum installer_discovery_status status;
    enum block_device_status io_status;
    unsigned char sector[INSTALLER_DISCOVERY_BLOCK_SIZE];
    size_t parsed_count = 0U;
    size_t disk_index;
    bool protective_mbr = false;
    size_t partition_offset;
    size_t partition_index;
    size_t parsed_index;

    status = installer_discovery_report_check(report);
    if (status != INSTALLER_DISCOVERY_OK) {
        return status;
    }
    if (device == NULL || identity == NULL) {
        return INSTALLER_DISCOVERY_NULL;
    }
    if (disk_id == 0U ||
        disk_id > INSTALLER_DISCOVERY_DISK_CAPACITY ||
        controller > INSTALLER_DISK_CONTROLLER_NVME ||
        !installer_discovery_identity_valid(identity, identity_length)) {
        return INSTALLER_DISCOVERY_INVALID_ARGUMENT;
    }
    if (block_device_validate(device) != BLOCK_DEVICE_OK) {
        return INSTALLER_DISCOVERY_INVALID_BLOCK_DEVICE;
    }
    if (device->block_size != INSTALLER_DISCOVERY_BLOCK_SIZE) {
        return INSTALLER_DISCOVERY_UNSUPPORTED_BLOCK_SIZE;
    }

    memset(&disk, 0, sizeof(disk));
    disk.id = disk_id;
    disk.present = true;
    disk.controller = controller;
    disk.partition_table = INSTALLER_PARTITION_TABLE_NONE;
    disk.block_size = device->block_size;
    disk.block_count = device->block_count;
    installer_discovery_identity_copy(disk.identity, identity, identity_length);

    if (installer_discovery_disk_index(report, disk_id) ==
        INSTALLER_DISCOVERY_DISK_CAPACITY) {
        status = installer_discovery_publish_disk(report, &disk);
        if (status != INSTALLER_DISCOVERY_OK) {
            return status;
        }
    }
    disk_index = installer_discovery_disk_index(report, disk_id);

    if (report->disks[disk_index].block_size != device->block_size ||
        report->disks[disk_index].block_count != device->block_count) {
        return INSTALLER_DISCOVERY_INVALID_BLOCK_DEVICE;
    }

    io_status = block_device_read(device, 0U, sector, sizeof(sector));
    if (io_status != BLOCK_DEVICE_OK) {
        return INSTALLER_DISCOVERY_IO_ERROR;
    }
    if (installer_discovery_all_zero(sector, sizeof(sector))) {
        return INSTALLER_DISCOVERY_OK;
    }
    if (sector[INSTALLER_DISCOVERY_MBR_SIGNATURE_OFFSET] != 0x55U ||
        sector[INSTALLER_DISCOVERY_MBR_SIGNATURE_OFFSET + 1U] != 0xAAU) {
        return INSTALLER_DISCOVERY_INVALID_PARTITION_TABLE;
    }

    for (partition_index = 0U;
         partition_index < INSTALLER_DISCOVERY_MBR_PARTITION_COUNT;
         ++partition_index) {
        partition_offset = INSTALLER_DISCOVERY_MBR_PARTITION_OFFSET +
            partition_index * INSTALLER_DISCOVERY_MBR_PARTITION_SIZE;
        if (sector[partition_offset + 4U] == 0xEEU) {
            protective_mbr = true;
        }
    }

    if (protective_mbr) {
        report->disks[disk_index].partition_table =
            INSTALLER_PARTITION_TABLE_GPT;
        status = installer_discovery_parse_gpt(
            device, report, disk_id, parsed, &parsed_count);
        if (status != INSTALLER_DISCOVERY_OK) {
            return status;
        }
    } else {
        report->disks[disk_index].partition_table =
            INSTALLER_PARTITION_TABLE_MBR;
        status = installer_discovery_parse_mbr(
            device, sector, report, disk_id, parsed, &parsed_count);
        if (status != INSTALLER_DISCOVERY_OK) {
            return status;
        }
    }

    status = installer_discovery_append_parsed_partitions(
        report, parsed, parsed_count);
    if (status != INSTALLER_DISCOVERY_OK) {
        return status;
    }
    for (parsed_index = 0U;
         parsed_index < parsed_count;
         ++parsed_index) {
        if (parsed[parsed_index].efi_system) {
            report->disks[disk_index].has_efi_system_partition = true;
            break;
        }
    }
    return INSTALLER_DISCOVERY_OK;
}

/*
 * ITEM-495: the model text, validated the way the identity is: bounded, never
 * empty, and only bytes a terminal can show.  A name nobody can read is worse
 * than no name, and a name that wrapped the screen would push the capacity off
 * it, so the bound is the record's, not the caller's.
 */
static bool installer_discovery_model_valid(
    const char *model,
    size_t model_length)
{
    size_t index;

    if (model == NULL || model_length == 0U) {
        return false;
    }
    if (model_length >= INSTALLER_DISCOVERY_MODEL_LENGTH) {
        return false;
    }
    for (index = 0U; index < model_length; ++index) {
        const unsigned char value = (unsigned char)model[index];

        if (value == 0U || value < 0x20U || value > 0x7EU) {
            return false;
        }
    }
    return true;
}

enum installer_discovery_status installer_discovery_set_disk_model(
    struct installer_discovery_report *report,
    uint32_t disk_id,
    const char *model,
    size_t model_length)
{
    enum installer_discovery_status status;
    size_t disk_index;
    size_t index;

    status = installer_discovery_report_check(report);
    if (status != INSTALLER_DISCOVERY_OK) {
        return status;
    }
    /* A finished report is a reading, not a draft: nothing renames a disk after
     * the scan was declared complete. */
    if (report->scan_complete) {
        return INSTALLER_DISCOVERY_ALREADY_PUBLISHED;
    }
    if (!installer_discovery_model_valid(model, model_length)) {
        return INSTALLER_DISCOVERY_INVALID_ARGUMENT;
    }
    disk_index = installer_discovery_disk_index(report, disk_id);
    if (disk_index == INSTALLER_DISCOVERY_DISK_CAPACITY) {
        return INSTALLER_DISCOVERY_OUT_OF_RANGE;
    }
    memset(report->disks[disk_index].model, 0,
           sizeof(report->disks[disk_index].model));
    for (index = 0U; index < model_length; ++index) {
        report->disks[disk_index].model[index] = model[index];
    }
    return INSTALLER_DISCOVERY_OK;
}

enum installer_discovery_status installer_discovery_finalize(
    struct installer_discovery_report *report)
{
    enum installer_discovery_status status;
    size_t index;
    uint32_t os_id;

    status = installer_discovery_report_check(report);
    if (status != INSTALLER_DISCOVERY_OK) {
        return status;
    }
    if (report->firmware_mode == INSTALLER_FIRMWARE_UNKNOWN ||
        !report->cpu_present ||
        !report->ram_present) {
        return INSTALLER_DISCOVERY_BAD_STATE;
    }

    for (index = 0U; index < INSTALLER_DISCOVERY_PARTITION_CAPACITY; ++index) {
        const struct installer_discovery_partition *partition =
            &report->partitions[index];
        enum installer_os_kind owner;
        struct installer_discovery_os os;
        size_t os_index;
        bool found = false;

        if (!partition->present) {
            continue;
        }
        owner = partition->owner;
        if (owner != INSTALLER_OS_LINUX &&
            owner != INSTALLER_OS_WINDOWS) {
            continue;
        }
        for (os_index = 0U;
             os_index < INSTALLER_DISCOVERY_OS_CAPACITY;
             ++os_index) {
            if (report->oss[os_index].present &&
                report->oss[os_index].kind == owner) {
                found = true;
                break;
            }
        }
        if (found) {
            continue;
        }
        memset(&os, 0, sizeof(os));
        os_id = 1U;
        while (os_id <= INSTALLER_DISCOVERY_OS_CAPACITY &&
               installer_discovery_os_id_in_use(report, os_id)) {
            ++os_id;
        }
        if (os_id > INSTALLER_DISCOVERY_OS_CAPACITY) {
            return INSTALLER_DISCOVERY_OUT_OF_RANGE;
        }
        os.id = os_id;
        os.present = true;
        os.kind = owner;
        os.partition_id = partition->id;
        os.has_efi_entry = false;
        status = installer_discovery_publish_os(report, &os);
        if (status != INSTALLER_DISCOVERY_OK) {
            return status;
        }
    }

    report->scan_complete = true;
    return INSTALLER_DISCOVERY_OK;
}

enum installer_discovery_status installer_discovery_validate(
    const struct installer_discovery_report *report)
{
    enum installer_discovery_status status;
    size_t index;

    status = installer_discovery_report_check(report);
    if (status != INSTALLER_DISCOVERY_OK) {
        return status;
    }
    if (!report->scan_complete ||
        report->manual_selection_required) {
        return INSTALLER_DISCOVERY_BAD_STATE;
    }
    for (index = 0U; index < INSTALLER_DISCOVERY_DISK_CAPACITY; ++index) {
        if (!report->disks[index].present) {
            continue;
        }
        if (report->disks[index].block_size == 0U ||
            report->disks[index].block_count == 0U) {
            return INSTALLER_DISCOVERY_INVALID_ARGUMENT;
        }
    }
    for (index = 0U;
         index < INSTALLER_DISCOVERY_PARTITION_CAPACITY;
         ++index) {
        const struct installer_discovery_partition *partition =
            &report->partitions[index];
        size_t disk_index;
        size_t other_index;

        if (!partition->present) {
            continue;
        }
        disk_index = installer_discovery_disk_index(
            report, partition->disk_id);
        if (disk_index == INSTALLER_DISCOVERY_DISK_CAPACITY ||
            partition->first_lba >
                report->disks[disk_index].block_count ||
                    partition->length_lba >
                        report->disks[disk_index].block_count -
                            partition->first_lba) {
            return INSTALLER_DISCOVERY_AMBIGUOUS;
        }
        for (other_index = index + 1U;
             other_index < INSTALLER_DISCOVERY_PARTITION_CAPACITY;
             ++other_index) {
            /*
             * ITEM-501: a logical partition lives inside the extended
             * container's extent, so the two overlap by construction.  The
             * container is a table, not a claim on the space for a caller, so
             * it never takes part in the overlap rule.
             */
            if (partition->container ||
                report->partitions[other_index].container) {
                continue;
            }
            if (installer_discovery_partitions_overlap(
                    partition, &report->partitions[other_index])) {
                return INSTALLER_DISCOVERY_AMBIGUOUS;
            }
        }
    }
    return INSTALLER_DISCOVERY_OK;
}

const char *installer_partition_layout_name(
    enum installer_partition_layout layout)
{
    switch (layout) {
    case INSTALLER_PARTITION_LAYOUT_PLAIN:
        return "PLAIN";
    case INSTALLER_PARTITION_LAYOUT_LVM:
        return "LVM";
    case INSTALLER_PARTITION_LAYOUT_BTRFS:
        return "BTRFS";
    case INSTALLER_PARTITION_LAYOUT_EXTENDED:
        return "EXTENDED";
    case INSTALLER_PARTITION_LAYOUT_LOGICAL:
        return "LOGICAL";
    default:
        return NULL;
    }
}

const char *installer_discovery_status_string(
    enum installer_discovery_status status)
{
    switch (status) {
    case INSTALLER_DISCOVERY_OK:
        return "INSTALLER_DISCOVERY_OK";
    case INSTALLER_DISCOVERY_NULL:
        return "INSTALLER_DISCOVERY_NULL";
    case INSTALLER_DISCOVERY_BAD_STATE:
        return "INSTALLER_DISCOVERY_BAD_STATE";
    case INSTALLER_DISCOVERY_BAD_VERSION:
        return "INSTALLER_DISCOVERY_BAD_VERSION";
    case INSTALLER_DISCOVERY_INVALID_ARGUMENT:
        return "INSTALLER_DISCOVERY_INVALID_ARGUMENT";
    case INSTALLER_DISCOVERY_ALREADY_PUBLISHED:
        return "INSTALLER_DISCOVERY_ALREADY_PUBLISHED";
    case INSTALLER_DISCOVERY_OUT_OF_RANGE:
        return "INSTALLER_DISCOVERY_OUT_OF_RANGE";
    case INSTALLER_DISCOVERY_INVALID_BLOCK_DEVICE:
        return "INSTALLER_DISCOVERY_INVALID_BLOCK_DEVICE";
    case INSTALLER_DISCOVERY_IO_ERROR:
        return "INSTALLER_DISCOVERY_IO_ERROR";
    case INSTALLER_DISCOVERY_INVALID_PARTITION_TABLE:
        return "INSTALLER_DISCOVERY_INVALID_PARTITION_TABLE";
    case INSTALLER_DISCOVERY_UNSUPPORTED_PARTITION_TABLE:
        return "INSTALLER_DISCOVERY_UNSUPPORTED_PARTITION_TABLE";
    case INSTALLER_DISCOVERY_TOO_MANY_PARTITIONS:
        return "INSTALLER_DISCOVERY_TOO_MANY_PARTITIONS";
    case INSTALLER_DISCOVERY_AMBIGUOUS:
        return "INSTALLER_DISCOVERY_AMBIGUOUS";
    case INSTALLER_DISCOVERY_READ_ONLY_VIOLATION:
        return "INSTALLER_DISCOVERY_READ_ONLY_VIOLATION";
    case INSTALLER_DISCOVERY_UNSUPPORTED_BLOCK_SIZE:
        return "INSTALLER_DISCOVERY_UNSUPPORTED_BLOCK_SIZE";
    default:
        return NULL;
    }
}
