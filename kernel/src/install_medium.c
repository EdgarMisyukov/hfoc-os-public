/*
 * ITEM-519 increment 5: the pure half of the boot-medium guard.
 *
 * The journal may write only to the medium this machine booted from.  The
 * loader names that medium (the disk id in the MBR it read the kernel from),
 * the device itself carries the same 32-bit id at offset 440, and the entry
 * the loader named gives the partition the medium uses.  Comparing them needs
 * no controller, no BOT command and no allocation, so the whole rule is
 * host-provable and the kernel only has to read one sector.
 *
 * The comparison fails closed.  A device with no MBR, a loader with no usable
 * id, a partition entry outside the table or past the device's own capacity:
 * each is a named result, and none of them is MATCH.
 */

#include "install_medium.h"

static uint32_t install_medium_le32(const uint8_t *bytes) {
    return (uint32_t)bytes[0] | ((uint32_t)bytes[1] << 8U) |
           ((uint32_t)bytes[2] << 16U) | ((uint32_t)bytes[3] << 24U);
}

const char *install_medium_result_string(enum install_medium_result result) {
    switch (result) {
    case INSTALL_MEDIUM_MATCH:
        return "MATCH";
    case INSTALL_MEDIUM_NO_MATCH:
        return "NO_MATCH";
    case INSTALL_MEDIUM_NO_TABLE:
        return "NO_TABLE";
    case INSTALL_MEDIUM_NO_IDENTITY:
        return "NO_IDENTITY";
    case INSTALL_MEDIUM_MALFORMED:
        return "MALFORMED";
    case INSTALL_MEDIUM_PROTECTIVE_MBR:
        return "PROTECTIVE_MBR";
    default:
        return "UNKNOWN";
    }
}

static struct install_medium_verdict install_medium_make_verdict(
    enum install_medium_result result,
    const char *reason) {
    struct install_medium_verdict verdict;

    verdict.result = result;
    verdict.reason = reason;
    verdict.partition_index = 0U;
    verdict.disk_id = 0U;
    verdict.partition_type = 0U;
    verdict.first_lba = 0U;
    verdict.length_lba = 0U;
    return verdict;
}

/*
 * ITEM-582: the diagnostic stick the operator prepared.  It is a different
 * device from the boot medium by construction (its own disk id), and it must
 * carry a FAT12 partition whose extent the writer may trust.
 */
struct install_medium_verdict install_medium_diagnostic_identify(
    const uint8_t sector[INSTALL_MEDIUM_SECTOR_BYTES],
    uint64_t block_count) {
    struct install_medium_verdict verdict;

    if (sector == NULL) {
        return install_medium_make_verdict(INSTALL_MEDIUM_NO_IDENTITY,
                                           "NO_SECTOR");
    }
    if (sector[INSTALL_MEDIUM_MBR_SIGNATURE_OFFSET] != 0x55U ||
        sector[INSTALL_MEDIUM_MBR_SIGNATURE_OFFSET + 1U] != 0xAAU) {
        return install_medium_make_verdict(INSTALL_MEDIUM_NO_TABLE,
                                           "SIGNATURE");
    }
    verdict = install_medium_make_verdict(INSTALL_MEDIUM_NO_MATCH, "DISK_ID");
    verdict.disk_id = install_medium_le32(
        sector + INSTALL_MEDIUM_MBR_DISK_ID_OFFSET);
    if (verdict.disk_id != INSTALL_MEDIUM_DIAGNOSTIC_DISK_ID) {
        return verdict;
    }
    for (uint32_t index = 0U; index < INSTALL_MEDIUM_MBR_PARTITION_COUNT;
         ++index) {
        const size_t offset = INSTALL_MEDIUM_MBR_PARTITION_OFFSET +
                              ((size_t)index * INSTALL_MEDIUM_MBR_PARTITION_SIZE);
        const uint8_t type = sector[offset + 4U];
        const uint32_t first_lba = install_medium_le32(sector + offset + 8U);
        const uint32_t length_lba = install_medium_le32(sector + offset + 12U);

        if (type != INSTALL_MEDIUM_MBR_TYPE_FAT12 || first_lba == 0U ||
            length_lba == 0U) {
            continue;
        }
        if ((uint64_t)first_lba >= block_count ||
            (uint64_t)length_lba > block_count - (uint64_t)first_lba) {
            verdict.result = INSTALL_MEDIUM_MALFORMED;
            verdict.reason = "RANGE";
            return verdict;
        }
        verdict.result = INSTALL_MEDIUM_MATCH;
        verdict.reason = "DIAGNOSTIC";
        verdict.partition_index = index + 1U;
        verdict.partition_type = type;
        verdict.first_lba = first_lba;
        verdict.length_lba = length_lba;
        return verdict;
    }
    verdict.result = INSTALL_MEDIUM_NO_MATCH;
    verdict.reason = "NO_FAT12";
    return verdict;
}

struct install_medium_verdict install_medium_identify(
    const struct install_medium_boot_identity *boot,
    const uint8_t sector[INSTALL_MEDIUM_SECTOR_BYTES],
    uint64_t block_count) {
    struct install_medium_verdict verdict;
    uint32_t partition_index;
    size_t offset;
    uint8_t type;
    uint32_t first_lba;
    uint32_t length_lba;

    if (boot == NULL) {
        return install_medium_make_verdict(INSTALL_MEDIUM_NO_IDENTITY,
                                           "NO_LOADER");
    }
    if (sector == NULL) {
        return install_medium_make_verdict(INSTALL_MEDIUM_NO_IDENTITY,
                                           "NO_SECTOR");
    }
    if (sector[INSTALL_MEDIUM_MBR_SIGNATURE_OFFSET] != 0x55U ||
        sector[INSTALL_MEDIUM_MBR_SIGNATURE_OFFSET + 1U] != 0xAAU) {
        return install_medium_make_verdict(INSTALL_MEDIUM_NO_TABLE,
                                           "SIGNATURE");
    }
    verdict = install_medium_make_verdict(INSTALL_MEDIUM_NO_IDENTITY,
                                          "NO_LOADER_ID");
    verdict.disk_id = install_medium_le32(
        sector + INSTALL_MEDIUM_MBR_DISK_ID_OFFSET);
    if (!boot->mbr_disk_id_valid || boot->mbr_disk_id == 0U) {
        return verdict;
    }
    if (verdict.disk_id != boot->mbr_disk_id) {
        verdict.result = INSTALL_MEDIUM_NO_MATCH;
        verdict.reason = "DISK_ID";
        return verdict;
    }
    /*
     * The id matches, so this is the boot medium.  The loader's partition index
     * is 1-based and names one of the four table entries; zero means the loader
     * read the file through a filesystem (an isohybrid medium booted from its
     * El Torito image reports this), and then the whole device is the medium.
     * An index past the table is not "no partition", it is a table the guard
     * cannot trust.
     */
    partition_index = boot->partition_index;
    if (partition_index > INSTALL_MEDIUM_MBR_PARTITION_COUNT) {
        verdict.result = INSTALL_MEDIUM_MALFORMED;
        verdict.reason = "PARTITION";
        return verdict;
    }
    if (partition_index == 0U) {
        if (block_count == 0U) {
            verdict.result = INSTALL_MEDIUM_MALFORMED;
            verdict.reason = "RANGE";
            return verdict;
        }
        verdict.result = INSTALL_MEDIUM_MATCH;
        verdict.reason = "OK";
        verdict.first_lba = 0U;
        verdict.length_lba = block_count;
        return verdict;
    }
    offset = INSTALL_MEDIUM_MBR_PARTITION_OFFSET +
             ((size_t)partition_index - 1U) *
                 INSTALL_MEDIUM_MBR_PARTITION_SIZE;
    type = sector[offset + 4U];
    first_lba = install_medium_le32(sector + offset + 8U);
    length_lba = install_medium_le32(sector + offset + 12U);
    verdict.partition_index = partition_index;
    verdict.partition_type = type;
    if (type == INSTALL_MEDIUM_MBR_TYPE_PROTECTIVE) {
        verdict.result = INSTALL_MEDIUM_PROTECTIVE_MBR;
        verdict.reason = "GPT";
        return verdict;
    }
    if (type == 0U || first_lba == 0U || length_lba == 0U) {
        verdict.result = INSTALL_MEDIUM_MALFORMED;
        verdict.reason = "PARTITION";
        return verdict;
    }
    if ((uint64_t)first_lba >= block_count ||
        (uint64_t)length_lba > block_count - (uint64_t)first_lba) {
        verdict.result = INSTALL_MEDIUM_MALFORMED;
        verdict.reason = "RANGE";
        return verdict;
    }
    verdict.result = INSTALL_MEDIUM_MATCH;
    verdict.reason = "OK";
    verdict.first_lba = first_lba;
    verdict.length_lba = length_lba;
    return verdict;
}
