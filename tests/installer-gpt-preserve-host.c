/*
 * ITEM-396 host proof: the GPT writer preserves foreign entries.
 *
 * This proof runs the writer against a synthetic disk that carries a foreign
 * GPT - a shared EFI System Partition, a Debian root, an entry immediately
 * after the free region the appliance will use, and an entry above 4 GiB - and
 * shows what the RED reproduction of the same disk showed the ITEM-391 writer
 * doing instead: every foreign entry still present, byte-for-byte, in its own
 * slot, with our roles added next to them.
 *
 * It is macro-gated like every other host proof in this tree:
 * HFOC_INSTALLER_GPT_PRESERVE_HOST_TEST.
 */
#include "installer_gpt_preserve.h"

#include "block_device.h"
#include "installer_layout.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifdef HFOC_INSTALLER_GPT_PRESERVE_HOST_TEST

#define FIXTURE_SECTOR_SIZE 512U
#define FIXTURE_SECTOR_SLOTS 8192U
/* The acceptance disk's size class, as in the ITEM-391 proof. */
#define FIXTURE_MEDIA_LBA UINT64_C(234375000)
#define FIXTURE_ENTRY_COUNT 128U
#define FIXTURE_ENTRY_SIZE 128U
#define FIXTURE_ENTRY_PER_SECTOR 4U
#define FIXTURE_ARRAY_SECTORS 32U
#define FIXTURE_GPT_TAIL_LBA 34U
#define FIXTURE_FIRST_USABLE_LBA 34U
#define FIXTURE_ALIGN_LBA 2048U
#define FIXTURE_SLOT_CAPACITY 8U

#define FOREIGN_ESP_FIRST UINT64_C(2048)
#define FOREIGN_ESP_LENGTH UINT64_C(4096)
#define FOREIGN_ROOT_FIRST UINT64_C(6144)
#define FOREIGN_ROOT_LENGTH UINT64_C(1000000)
/* Immediately above the free region the appliance's roles are placed in: the
 * appliance's three roles must land below it and leave it alone. */
#define FOREIGN_ADJACENT_FIRST UINT64_C(1310720)
#define FOREIGN_ADJACENT_LENGTH UINT64_C(4096)
/* Above 4 GiB: LBA 8,388,608 is exactly 4 GiB at 512 bytes per sector. */
#define FOREIGN_HIGH_FIRST UINT64_C(8388608)
#define FOREIGN_HIGH_LENGTH UINT64_C(1000000)

#define APPLIANCE_SYSTEM_FIRST UINT64_C(1048576)
#define APPLIANCE_SYSTEM_LENGTH UINT64_C(131072)
#define APPLIANCE_PERSISTENCE_FIRST \
    (APPLIANCE_SYSTEM_FIRST + APPLIANCE_SYSTEM_LENGTH)
#define APPLIANCE_PERSISTENCE_LENGTH UINT64_C(65536)
#define APPLIANCE_RECOVERY_FIRST \
    (APPLIANCE_PERSISTENCE_FIRST + APPLIANCE_PERSISTENCE_LENGTH)
#define APPLIANCE_RECOVERY_LENGTH UINT64_C(65536)

static int failures;

#define CHECK(cond, msg) do { \
    if (!(cond)) { \
        fprintf(stderr, "FAIL: %s\n", msg); \
        ++failures; \
    } else { \
        printf("PASS: %s\n", msg); \
    } \
} while (0)

struct fixture_sector {
    bool used;
    uint64_t lba;
    unsigned char data[FIXTURE_SECTOR_SIZE];
};

struct fixture_entry_spec {
    uint32_t slot;
    bool present;
    unsigned char type_guid[16U];
    uint64_t first_lba;
    uint64_t last_lba;
    const char *name;
};

static struct fixture_sector fixture_sectors[FIXTURE_SECTOR_SLOTS];
static size_t fixture_sector_count;
static uint64_t fixture_block_count = FIXTURE_MEDIA_LBA;
static size_t fixture_write_calls;
static size_t fixture_fail_read_call;
static size_t fixture_fail_write_call;
static size_t fixture_read_calls;

/*
 * The array as it was before the install: the proof compares the preserved
 * slots on the disk against these bytes, which is what "byte-for-byte" means.
 */
static unsigned char fixture_array_before[
    FIXTURE_ARRAY_SECTORS][FIXTURE_SECTOR_SIZE];
static unsigned char fixture_mbr_before[FIXTURE_SECTOR_SIZE];
static unsigned char fixture_primary_header_before[FIXTURE_SECTOR_SIZE];
static unsigned char fixture_backup_header_before[FIXTURE_SECTOR_SIZE];

/*
 * The bookkeeping table is caller-owned storage in production (it is about
 * 12 KiB), so the proof owns it statically rather than on the stack.
 */
static struct installer_gpt_preserve_table preserve_table;

static struct fixture_sector *fixture_find(uint64_t lba)
{
    size_t index;

    for (index = 0U; index < fixture_sector_count; ++index) {
        if (fixture_sectors[index].used &&
            fixture_sectors[index].lba == lba) {
            return &fixture_sectors[index];
        }
    }
    return NULL;
}

static struct fixture_sector *fixture_allocate(uint64_t lba)
{
    struct fixture_sector *sector = fixture_find(lba);

    if (sector != NULL) {
        return sector;
    }
    if (fixture_sector_count >= FIXTURE_SECTOR_SLOTS) {
        return NULL;
    }
    sector = &fixture_sectors[fixture_sector_count++];
    memset(sector, 0, sizeof(*sector));
    sector->used = true;
    sector->lba = lba;
    return sector;
}

static enum block_device_status fixture_read(
    struct block_device *device,
    uint64_t offset,
    void *buffer,
    size_t length)
{
    unsigned char *bytes = (unsigned char *)buffer;
    uint64_t lba;
    size_t count;
    size_t index;

    (void)device;
    if (buffer == NULL || length == 0U ||
        offset % FIXTURE_SECTOR_SIZE != 0U ||
        length % FIXTURE_SECTOR_SIZE != 0U) {
        return BLOCK_DEVICE_UNALIGNED;
    }
    lba = offset / FIXTURE_SECTOR_SIZE;
    count = length / FIXTURE_SECTOR_SIZE;
    if (lba >= fixture_block_count ||
        count > fixture_block_count - lba) {
        return BLOCK_DEVICE_OUT_OF_RANGE;
    }
    ++fixture_read_calls;
    if (fixture_fail_read_call != 0U &&
        fixture_read_calls == fixture_fail_read_call) {
        return BLOCK_DEVICE_IO_ERROR;
    }
    for (index = 0U; index < count; ++index) {
        const struct fixture_sector *sector = fixture_find(lba + index);

        if (sector == NULL) {
            memset(bytes + index * FIXTURE_SECTOR_SIZE, 0,
                   FIXTURE_SECTOR_SIZE);
        } else {
            memcpy(bytes + index * FIXTURE_SECTOR_SIZE,
                   sector->data, FIXTURE_SECTOR_SIZE);
        }
    }
    return BLOCK_DEVICE_OK;
}

static enum block_device_status fixture_write(
    struct block_device *device,
    uint64_t offset,
    const void *buffer,
    size_t length)
{
    const unsigned char *bytes = (const unsigned char *)buffer;
    uint64_t lba;
    size_t count;
    size_t index;

    (void)device;
    if (buffer == NULL || length == 0U ||
        offset % FIXTURE_SECTOR_SIZE != 0U ||
        length % FIXTURE_SECTOR_SIZE != 0U) {
        return BLOCK_DEVICE_UNALIGNED;
    }
    ++fixture_write_calls;
    if (fixture_fail_write_call != 0U &&
        fixture_write_calls == fixture_fail_write_call) {
        fixture_fail_write_call = 0U;
        return BLOCK_DEVICE_IO_ERROR;
    }
    lba = offset / FIXTURE_SECTOR_SIZE;
    count = length / FIXTURE_SECTOR_SIZE;
    if (lba >= fixture_block_count ||
        count > fixture_block_count - lba) {
        return BLOCK_DEVICE_OUT_OF_RANGE;
    }
    for (index = 0U; index < count; ++index) {
        struct fixture_sector *sector = fixture_allocate(lba + index);

        if (sector == NULL) {
            return BLOCK_DEVICE_IO_ERROR;
        }
        memcpy(sector->data,
               bytes + index * FIXTURE_SECTOR_SIZE,
               FIXTURE_SECTOR_SIZE);
    }
    return BLOCK_DEVICE_OK;
}

static enum block_device_status fixture_flush(
    struct block_device *device)
{
    (void)device;
    return BLOCK_DEVICE_OK;
}

static struct block_device fixture_device(void)
{
    struct block_device device;

    memset(&device, 0, sizeof(device));
    device.block_size = FIXTURE_SECTOR_SIZE;
    device.block_count = fixture_block_count;
    device.read_fn = fixture_read;
    device.write_fn = fixture_write;
    device.flush_fn = fixture_flush;
    return device;
}

static void fixture_store_le32(unsigned char *bytes, uint32_t value)
{
    bytes[0U] = (unsigned char)(value & 0xFFU);
    bytes[1U] = (unsigned char)((value >> 8U) & 0xFFU);
    bytes[2U] = (unsigned char)((value >> 16U) & 0xFFU);
    bytes[3U] = (unsigned char)((value >> 24U) & 0xFFU);
}

static void fixture_store_le64(unsigned char *bytes, uint64_t value)
{
    size_t index;

    for (index = 0U; index < 8U; ++index) {
        bytes[index] = (unsigned char)(value >> (8U * index));
    }
}

static uint32_t fixture_crc32(const unsigned char *bytes, size_t length)
{
    uint32_t crc = 0xFFFFFFFFU;
    size_t index;

    for (index = 0U; index < length; ++index) {
        unsigned int bit;

        crc ^= bytes[index];
        for (bit = 0U; bit < 8U; ++bit) {
            if ((crc & 1U) != 0U) {
                crc = (crc >> 1U) ^ 0xEDB88320U;
            } else {
                crc >>= 1U;
            }
        }
    }
    return crc ^ 0xFFFFFFFFU;
}

static const unsigned char fixture_esp_guid[16U] = {
    0x28U, 0x73U, 0x2AU, 0xC1U, 0x1FU, 0xF8U, 0xD2U, 0x11U,
    0xBAU, 0x4BU, 0x00U, 0xA0U, 0xC9U, 0x3EU, 0xC9U, 0x3BU
};
static const unsigned char fixture_linux_guid[16U] = {
    0xAFU, 0x3DU, 0xC6U, 0x0FU, 0x83U, 0x84U, 0x72U, 0x47U,
    0x8EU, 0x79U, 0x3DU, 0x69U, 0xD8U, 0x47U, 0x7DU, 0xE4U
};

static bool installer_test_all_zero(
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

static void fixture_write_entry(
    unsigned char *array,
    const struct fixture_entry_spec *spec)
{
    unsigned char *entry = array + spec->slot * FIXTURE_ENTRY_SIZE;
    size_t index;

    /*
     * A spec without a type GUID is a foreign entry: the fixture gives the ESP
     * slot the standard EFI System Partition type and every other slot the
     * Linux filesystem data type, which is what the disk it models carries.
     */
    if (!installer_test_all_zero(spec->type_guid, 16U)) {
        memcpy(entry, spec->type_guid, 16U);
    } else if (spec->slot == 0U) {
        memcpy(entry, fixture_esp_guid, 16U);
    } else {
        memcpy(entry, fixture_linux_guid, 16U);
    }
    for (index = 0U; index < 16U; ++index) {
        entry[16U + index] = (unsigned char)(0x80U + spec->slot);
    }
    fixture_store_le64(entry + 32U, spec->first_lba);
    fixture_store_le64(entry + 40U, spec->last_lba);
    /* A UTF-16 partition name, as every real GPT carries. */
    for (index = 0U;
         spec->name != NULL && spec->name[index] != '\0' &&
         index < 35U;
         ++index) {
        entry[56U + index * 2U] = (unsigned char)spec->name[index];
    }
}

static void fixture_build_header(
    unsigned char *header,
    uint64_t block_count,
    uint64_t my_lba,
    uint64_t alternate_lba,
    uint64_t entry_lba,
    const unsigned char *array,
    uint32_t entry_count,
    uint32_t entry_size)
{
    uint32_t crc;

    memset(header, 0, FIXTURE_SECTOR_SIZE);
    memcpy(header, "EFI PART", 8U);
    header[9U] = 0x00U;
    header[10U] = 0x01U;
    fixture_store_le32(header + 12U, 92U);
    fixture_store_le64(header + 24U, my_lba);
    fixture_store_le64(header + 32U, alternate_lba);
    fixture_store_le64(header + 40U, FIXTURE_FIRST_USABLE_LBA);
    fixture_store_le64(header + 48U, block_count - FIXTURE_GPT_TAIL_LBA);
    for (crc = 0U; crc < 16U; ++crc) {
        header[56U + crc] = (unsigned char)(0x40U + crc);
    }
    fixture_store_le64(header + 72U, entry_lba);
    fixture_store_le32(header + 80U, entry_count);
    fixture_store_le32(header + 84U, entry_size);
    fixture_store_le32(
        header + 88U,
        fixture_crc32(array, (size_t)entry_count * entry_size));
    crc = fixture_crc32(header, 92U);
    fixture_store_le32(header + 16U, crc);
}

static void fixture_build_mbr(unsigned char *mbr, uint64_t block_count)
{
    size_t index;

    memset(mbr, 0, FIXTURE_SECTOR_SIZE);
    /*
     * Boot code and a disk signature in bytes 0..445: the boot record the
     * owner asked about.  The writer must give these bytes back untouched.
     */
    for (index = 0U; index < 446U; ++index) {
        mbr[index] = (unsigned char)(0x30U + (index % 7U));
    }
    mbr[440U] = 0xDEU;
    mbr[441U] = 0xADU;
    mbr[444U] = 0xBEU;
    mbr[445U] = 0xEFU;
    mbr[446U] = 0x00U;
    mbr[447U] = 0x00U;
    mbr[448U] = 0x02U;
    mbr[449U] = 0x00U;
    mbr[450U] = 0xEEU;
    mbr[451U] = 0xFFU;
    mbr[452U] = 0xFFU;
    mbr[453U] = 0xFFU;
    fixture_store_le32(mbr + 454U, 1U);
    fixture_store_le32(
        mbr + 458U,
        block_count - 1U > UINT32_C(0xFFFFFFFE)
            ? UINT32_C(0xFFFFFFFF)
            : (uint32_t)(block_count - 1U));
    mbr[510U] = 0x55U;
    mbr[511U] = 0xAAU;
}

/*
 * Build the foreign disk: protective MBR with boot code, primary and backup GPT
 * with 128 entries, the given foreign entries in their slots, and everything
 * else zero.
 */
static void fixture_build_disk(
    const struct fixture_entry_spec *specs,
    size_t spec_count)
{
    unsigned char array[FIXTURE_ARRAY_SECTORS * FIXTURE_SECTOR_SIZE];
    unsigned char header[FIXTURE_SECTOR_SIZE];
    unsigned char mbr[FIXTURE_SECTOR_SIZE];
    uint64_t backup_lba = fixture_block_count - 1U;
    uint64_t backup_array_lba =
        fixture_block_count - (FIXTURE_ARRAY_SECTORS + 1U);
    size_t index;

    memset(fixture_sectors, 0, sizeof(fixture_sectors));
    fixture_sector_count = 0U;
    fixture_write_calls = 0U;
    fixture_read_calls = 0U;
    fixture_fail_read_call = 0U;
    fixture_fail_write_call = 0U;
    fixture_block_count = FIXTURE_MEDIA_LBA;

    memset(array, 0, sizeof(array));
    for (index = 0U; index < spec_count; ++index) {
        if (specs[index].present) {
            fixture_write_entry(array, &specs[index]);
        }
    }
    fixture_build_mbr(mbr, fixture_block_count);
    memcpy(fixture_mbr_before, mbr, sizeof(mbr));
    memcpy(fixture_array_before, array, sizeof(array));
    memcpy(fixture_allocate(0U)->data, mbr, FIXTURE_SECTOR_SIZE);

    fixture_build_header(
        header, fixture_block_count, 1U, backup_lba, 2U, array,
        FIXTURE_ENTRY_COUNT, FIXTURE_ENTRY_SIZE);
    memcpy(fixture_primary_header_before, header, sizeof(header));
    memcpy(fixture_allocate(1U)->data, header, FIXTURE_SECTOR_SIZE);
    for (index = 0U; index < FIXTURE_ARRAY_SECTORS; ++index) {
        memcpy(fixture_allocate(2U + index)->data,
               array + index * FIXTURE_SECTOR_SIZE,
               FIXTURE_SECTOR_SIZE);
    }
    fixture_build_header(
        header, fixture_block_count, backup_lba, 1U, backup_array_lba,
        array, FIXTURE_ENTRY_COUNT, FIXTURE_ENTRY_SIZE);
    memcpy(fixture_backup_header_before, header, sizeof(header));
    memcpy(fixture_allocate(backup_lba)->data, header,
           FIXTURE_SECTOR_SIZE);
    for (index = 0U; index < FIXTURE_ARRAY_SECTORS; ++index) {
        memcpy(fixture_allocate(backup_array_lba + index)->data,
               array + index * FIXTURE_SECTOR_SIZE,
               FIXTURE_SECTOR_SIZE);
    }
}

/*
 * The fixture's foreign table, written the way a Debian installation leaves it:
 * the shared ESP, the Debian root, a Debian entry above 4 GiB, and an entry
 * immediately above the free region the appliance's roles go into.
 */
static const struct fixture_entry_spec fixture_foreign_specs[4U] = {
    {
        .slot = 0U, .present = true,
        .first_lba = FOREIGN_ESP_FIRST,
        .last_lba = FOREIGN_ESP_FIRST + FOREIGN_ESP_LENGTH - 1U,
        .name = "EFI System Partition"
    },
    {
        .slot = 1U, .present = true,
        .first_lba = FOREIGN_ROOT_FIRST,
        .last_lba = FOREIGN_ROOT_FIRST + FOREIGN_ROOT_LENGTH - 1U,
        .name = "Debian GNU/Linux"
    },
    {
        .slot = 2U, .present = true,
        .first_lba = FOREIGN_HIGH_FIRST,
        .last_lba = FOREIGN_HIGH_FIRST + FOREIGN_HIGH_LENGTH - 1U,
        .name = "Debian home"
    },
    {
        .slot = 5U, .present = true,
        .first_lba = FOREIGN_ADJACENT_FIRST,
        .last_lba = FOREIGN_ADJACENT_FIRST + FOREIGN_ADJACENT_LENGTH - 1U,
        .name = "Debian data"
    }
};

/* The three roles a dual-boot install adds; the ESP is reused, not added. */
static void fixture_dual_boot_plan(
    struct installer_layout_plan *plan)
{
    memset(plan, 0, sizeof(*plan));
    plan->version = INSTALLER_LAYOUT_VERSION;
    plan->mode = INSTALLER_LAYOUT_MODE_WHOLE_DISK;
    plan->disk_id = 1U;
    plan->firmware_mode = INSTALLER_FIRMWARE_UEFI;
    plan->block_size = FIXTURE_SECTOR_SIZE;
    plan->block_count = FIXTURE_MEDIA_LBA;
    plan->existing_partition_count = 4U;
    plan->destructive = false;
    plan->partition_count = 3U;
    strcpy(plan->identity, "preserve-fixture-disk");
    plan->partitions[0U].present = true;
    plan->partitions[0U].role = INSTALLER_LAYOUT_ROLE_HFOC_SYSTEM;
    plan->partitions[0U].partition_index = 1U;
    plan->partitions[0U].first_lba = APPLIANCE_SYSTEM_FIRST;
    plan->partitions[0U].length_lba = APPLIANCE_SYSTEM_LENGTH;
    plan->partitions[1U].present = true;
    plan->partitions[1U].role = INSTALLER_LAYOUT_ROLE_HFOC_PERSISTENCE;
    plan->partitions[1U].partition_index = 2U;
    plan->partitions[1U].first_lba = APPLIANCE_PERSISTENCE_FIRST;
    plan->partitions[1U].length_lba = APPLIANCE_PERSISTENCE_LENGTH;
    plan->partitions[2U].present = true;
    plan->partitions[2U].role = INSTALLER_LAYOUT_ROLE_HFOC_RECOVERY;
    plan->partitions[2U].partition_index = 3U;
    plan->partitions[2U].first_lba = APPLIANCE_RECOVERY_FIRST;
    plan->partitions[2U].length_lba = APPLIANCE_RECOVERY_LENGTH;
    plan->confirmed = true;
}

static void fixture_reuse(
    struct installer_gpt_preserve_reuse *reuse)
{
    memset(reuse, 0, sizeof(*reuse));
    reuse->present = true;
    reuse->role = INSTALLER_LAYOUT_ROLE_EFI_SYSTEM;
    reuse->first_lba = FOREIGN_ESP_FIRST;
    reuse->length_lba = FOREIGN_ESP_LENGTH;
}

static bool fixture_slot_matches(
    uint64_t slot,
    const unsigned char *expected)
{
    const struct fixture_sector *sector =
        fixture_find(2U + slot / FIXTURE_ENTRY_PER_SECTOR);

    if (sector == NULL) {
        return false;
    }
    return memcmp(
               sector->data +
                   (slot % FIXTURE_ENTRY_PER_SECTOR) *
                       FIXTURE_ENTRY_SIZE,
               expected, FIXTURE_ENTRY_SIZE) == 0;
}

static bool fixture_slot_is_zero(uint64_t slot)
{
    unsigned char zero[FIXTURE_ENTRY_SIZE];

    memset(zero, 0, sizeof(zero));
    return fixture_slot_matches(slot, zero);
}

static bool fixture_entry_first_lba(uint64_t slot, uint64_t *value)
{
    const struct fixture_sector *sector =
        fixture_find(2U + slot / FIXTURE_ENTRY_PER_SECTOR);

    if (sector == NULL) {
        return false;
    }
    *value = 0U;
    {
        const unsigned char *entry =
            sector->data +
            (slot % FIXTURE_ENTRY_PER_SECTOR) * FIXTURE_ENTRY_SIZE;
        size_t index;

        for (index = 0U; index < 8U; ++index) {
            *value |= (uint64_t)entry[32U + index] << (8U * index);
        }
    }
    return true;
}

static bool fixture_entry_type_is(
    uint64_t slot,
    enum installer_layout_role role)
{
    const struct fixture_sector *sector =
        fixture_find(2U + slot / FIXTURE_ENTRY_PER_SECTOR);
    const unsigned char *type_guid =
        installer_layout_role_type_guid(role);

    if (sector == NULL || type_guid == NULL) {
        return false;
    }
    return memcmp(
               sector->data +
                   (slot % FIXTURE_ENTRY_PER_SECTOR) *
                       FIXTURE_ENTRY_SIZE,
               type_guid, 16U) == 0;
}

/* Every foreign entry as it was before the install, still on the disk. */
static void check_preserved_entries_byte_identical(void)
{
    size_t index;

    for (index = 0U; index < 4U; ++index) {
        const uint32_t slot = fixture_foreign_specs[index].slot;
        const struct fixture_sector *sector =
            fixture_find(2U + slot / FIXTURE_ENTRY_PER_SECTOR);

        CHECK(sector != NULL &&
                  memcmp(sector->data +
                             (slot % FIXTURE_ENTRY_PER_SECTOR) *
                                 FIXTURE_ENTRY_SIZE,
                         fixture_array_before[
                             slot / FIXTURE_ENTRY_PER_SECTOR] +
                             (slot % FIXTURE_ENTRY_PER_SECTOR) *
                                 FIXTURE_ENTRY_SIZE,
                         FIXTURE_ENTRY_SIZE) == 0,
              "preserved entry is byte-identical to the pre-install table");
    }
}

/*
 * Every array byte the install did not place one of its own entries in must
 * come back exactly as it was: the writer only ever writes slots whose 128
 * bytes are all zero.
 */
static void check_untouched_array_bytes(void)
{
    size_t index;

    for (index = 0U; index < FIXTURE_ARRAY_SECTORS; ++index) {
        const struct fixture_sector *sector = fixture_find(2U + index);
        bool touched = false;
        size_t entry;

        if (sector == NULL) {
            CHECK(false, "an array sector is missing after the install");
            continue;
        }
        for (entry = 0U; entry < FIXTURE_ENTRY_PER_SECTOR; ++entry) {
            const uint32_t slot =
                (uint32_t)(index * FIXTURE_ENTRY_PER_SECTOR + entry);

            if (preserve_table.entries[slot].origin ==
                INSTALLER_GPT_PRESERVE_SLOT_ADDED) {
                touched = true;
            }
        }
        if (!touched) {
            char message[80U];

            snprintf(message, sizeof(message),
                     "array sector %u unchanged", (unsigned)index);
            if (memcmp(sector->data, fixture_array_before[index],
                       FIXTURE_SECTOR_SIZE) != 0) {
                size_t byte_index;

                for (byte_index = 0U; byte_index < FIXTURE_SECTOR_SIZE;
                     ++byte_index) {
                    if (sector->data[byte_index] !=
                        fixture_array_before[index][byte_index]) {
                        fprintf(stderr,
                                "sector %u byte %u: %02x -> %02x\n",
                                (unsigned)index, (unsigned)byte_index,
                                fixture_array_before[index][byte_index],
                                sector->data[byte_index]);
                        break;
                    }
                }
            }
            CHECK(memcmp(sector->data, fixture_array_before[index],
                         FIXTURE_SECTOR_SIZE) == 0,
                  message);
            continue;
        }
        for (entry = 0U; entry < FIXTURE_ENTRY_PER_SECTOR; ++entry) {
            const uint32_t slot =
                (uint32_t)(index * FIXTURE_ENTRY_PER_SECTOR + entry);

            if (preserve_table.entries[slot].origin ==
                INSTALLER_GPT_PRESERVE_SLOT_ADDED) {
                continue;
            }
            CHECK(memcmp(
                      sector->data + entry * FIXTURE_ENTRY_SIZE,
                      fixture_array_before[index] +
                          entry * FIXTURE_ENTRY_SIZE,
                      FIXTURE_ENTRY_SIZE) == 0,
                  "an entry next to our own is byte-identical");
        }
    }
}

static void test_preserve_foreign_table(void)
{
    struct block_device device = fixture_device();
    struct installer_layout_plan plan;
    struct installer_gpt_preserve_reuse reuse;
    char record[INSTALLER_GPT_PRESERVE_RECORD_LENGTH];
    size_t record_length = 0U;
    uint64_t first = 0U;

    fixture_build_disk(
        fixture_foreign_specs,
        sizeof(fixture_foreign_specs) / sizeof(fixture_foreign_specs[0U]));
    fixture_dual_boot_plan(&plan);
    fixture_reuse(&reuse);
    memset(&preserve_table, 0, sizeof(preserve_table));

    CHECK(installer_gpt_preserve_read(&device, 1U, &preserve_table) ==
              INSTALLER_GPT_PRESERVE_OK,
          "a well-formed foreign GPT is read");
    CHECK(preserve_table.preserved_count == 4U,
          "all four foreign entries are recorded as preserved");
    CHECK(preserve_table.esp_index == INSTALLER_GPT_PRESERVE_NO_SLOT &&
              preserve_table.esp_reused == false,
          "the reuse decision belongs to the check, not to the read");
    CHECK(installer_gpt_preserve_check(
              &preserve_table, &plan, &reuse) ==
              INSTALLER_GPT_PRESERVE_OK,
          "the dual-boot plan fits next to the foreign entries");
    CHECK(preserve_table.esp_reused &&
              preserve_table.esp_index == 0U,
          "the reused ESP is the foreign ESP entry, byte-for-byte");
    CHECK(preserve_table.free_first_lba ==
              FOREIGN_ROOT_FIRST + FOREIGN_ROOT_LENGTH &&
              preserve_table.free_length_lba ==
                  FOREIGN_ADJACENT_FIRST -
                      (FOREIGN_ROOT_FIRST + FOREIGN_ROOT_LENGTH),
          "the free region used ends at the adjacent foreign entry");
    CHECK(plan.executed == false, "nothing is executed before the write");

    CHECK(installer_gpt_preserve_write(
              &device, &plan, &reuse, &preserve_table) ==
              INSTALLER_GPT_PRESERVE_OK,
          "the preserving write succeeds");
    CHECK(plan.executed, "the write marks the plan executed");
    CHECK(installer_gpt_preserve_verify(&device, &preserve_table) ==
              INSTALLER_GPT_PRESERVE_OK,
          "the written table verifies against the table that was read");

    check_preserved_entries_byte_identical();
    check_untouched_array_bytes();
    CHECK(memcmp(fixture_find(0U)->data, fixture_mbr_before,
                 FIXTURE_SECTOR_SIZE) == 0,
          "the protective MBR is byte-identical, boot code included");
    CHECK(memcmp(fixture_find(fixture_block_count - 1U)->data,
                 fixture_find(1U)->data, FIXTURE_SECTOR_SIZE) != 0,
          "the backup header describes the backup location");

    /* Our three roles landed somewhere, with the role's own type GUID. */
    {
        uint32_t found = 0U;
        uint64_t slot;

        for (slot = 0U; slot < FIXTURE_ENTRY_COUNT; ++slot) {
            if (fixture_entry_type_is(
                    slot, INSTALLER_LAYOUT_ROLE_HFOC_SYSTEM) ||
                fixture_entry_type_is(
                    slot, INSTALLER_LAYOUT_ROLE_HFOC_PERSISTENCE) ||
                fixture_entry_type_is(
                    slot, INSTALLER_LAYOUT_ROLE_HFOC_RECOVERY)) {
                ++found;
            }
        }
        CHECK(found == 3U, "exactly the three appliance roles are added");
    }
    CHECK(fixture_entry_first_lba(3U, &first) &&
              first == APPLIANCE_SYSTEM_FIRST,
          "the appliance's roles are added in the free region");
    CHECK(fixture_entry_type_is(
              3U, INSTALLER_LAYOUT_ROLE_HFOC_SYSTEM) &&
              fixture_entry_type_is(
                  4U, INSTALLER_LAYOUT_ROLE_HFOC_PERSISTENCE) &&
              fixture_entry_type_is(
                  6U, INSTALLER_LAYOUT_ROLE_HFOC_RECOVERY),
          "the roles take the lowest all-zero slots, in plan order");
    CHECK(fixture_slot_is_zero(7U),
          "a slot above the added entries stays empty");

    CHECK(installer_gpt_preserve_render_record(
              &preserve_table, &plan, 3U, record, sizeof(record),
              &record_length) == INSTALLER_GPT_PRESERVE_OK &&
              record_length > 0U,
          "the preservation record renders");
    CHECK(strstr(record, "[INSTALLER-PRESERVE] plan mode=dual-boot") ==
              record,
          "the record starts with the single-line plan");
    CHECK(strstr(record, "preserved=4 added=3 esp_reused=1") != NULL,
          "the record publishes what is preserved and what is added");
    CHECK(strstr(record, "menu_entries=3") != NULL,
          "the record publishes the menu entry count");
    CHECK(strstr(record, "slot index=0 origin=preserved") != NULL &&
              strstr(record, "slot index=1 origin=preserved role=foreign") !=
                  NULL,
          "the record lists the preserved entries");
    CHECK(strstr(record, "origin=added role=hfoc-system") != NULL,
          "the record lists the added roles");
    printf("--- preservation record ---\n%s", record);

    /*
     * Re-running the install must not duplicate the appliance's roles: the
     * writer recognises its own entries by their type and unique GUID.
     */
    {
        struct installer_layout_plan second;
        uint32_t found = 0U;
        uint64_t slot;

        fixture_dual_boot_plan(&second);
        CHECK(installer_gpt_preserve_read(&device, 1U, &preserve_table) ==
                  INSTALLER_GPT_PRESERVE_OK,
              "the written table reads back");
        CHECK(preserve_table.preserved_count == 7U,
              "the appliance's own entries read back as present");
        CHECK(installer_gpt_preserve_write(
                  &device, &second, &reuse, &preserve_table) ==
                  INSTALLER_GPT_PRESERVE_OK,
              "re-running the install succeeds");
        for (slot = 0U; slot < FIXTURE_ENTRY_COUNT; ++slot) {
            if (fixture_entry_type_is(
                    slot, INSTALLER_LAYOUT_ROLE_HFOC_SYSTEM)) {
                ++found;
            }
        }
        CHECK(found == 1U,
              "re-running does not duplicate the appliance's entries");
        check_untouched_array_bytes();
        check_preserved_entries_byte_identical();
    }
}

static void test_corrupt_table_refused(void)
{
    struct block_device device = fixture_device();
    struct installer_layout_plan plan;
    struct installer_gpt_preserve_reuse reuse;

    /* A single flipped byte in the primary array, CRC no longer matching. */
    fixture_build_disk(
        fixture_foreign_specs,
        sizeof(fixture_foreign_specs) / sizeof(fixture_foreign_specs[0U]));
    fixture_find(2U)->data[64U] ^= 0x5AU;
    memset(&preserve_table, 0, sizeof(preserve_table));
    CHECK(installer_gpt_preserve_read(&device, 1U, &preserve_table) ==
              INSTALLER_GPT_PRESERVE_CORRUPT_TABLE,
          "a primary array that does not match its CRC is refused");

    /* The primary and the backup array disagree. */
    fixture_build_disk(
        fixture_foreign_specs,
        sizeof(fixture_foreign_specs) / sizeof(fixture_foreign_specs[0U]));
    fixture_find(fixture_block_count - 2U)->data[0U] ^= 0xFFU;
    memset(&preserve_table, 0, sizeof(preserve_table));
    CHECK(installer_gpt_preserve_read(&device, 1U, &preserve_table) ==
              INSTALLER_GPT_PRESERVE_CORRUPT_TABLE,
          "a backup array that differs from the primary is refused");

    /* A header CRC that does not hold. */
    fixture_build_disk(
        fixture_foreign_specs,
        sizeof(fixture_foreign_specs) / sizeof(fixture_foreign_specs[0U]));
    fixture_find(1U)->data[16U] ^= 0x01U;
    memset(&preserve_table, 0, sizeof(preserve_table));
    CHECK(installer_gpt_preserve_read(&device, 1U, &preserve_table) ==
              INSTALLER_GPT_PRESERVE_CORRUPT_TABLE,
          "a primary header whose CRC does not hold is refused");

    /* No protective MBR at all: an empty sector, never "assume empty". */
    fixture_build_disk(
        fixture_foreign_specs,
        sizeof(fixture_foreign_specs) / sizeof(fixture_foreign_specs[0U]));
    memset(fixture_find(0U)->data, 0, FIXTURE_SECTOR_SIZE);
    memset(&preserve_table, 0, sizeof(preserve_table));
    CHECK(installer_gpt_preserve_read(&device, 1U, &preserve_table) ==
              INSTALLER_GPT_PRESERVE_CORRUPT_TABLE,
          "a disk without a protective MBR is refused, not assumed empty");

    /* An entry whose extent leaves the usable region. */
    {
        struct fixture_entry_spec specs[4U];

        memcpy(specs, fixture_foreign_specs, sizeof(specs));
        specs[3U].last_lba = FIXTURE_MEDIA_LBA;
        fixture_build_disk(specs, 4U);
        memset(&preserve_table, 0, sizeof(preserve_table));
        CHECK(installer_gpt_preserve_read(&device, 1U, &preserve_table) ==
                  INSTALLER_GPT_PRESERVE_CORRUPT_TABLE,
              "an entry outside the usable region is refused");
    }
    /* Two entries that claim the same sectors. */
    {
        struct fixture_entry_spec specs[4U];

        memcpy(specs, fixture_foreign_specs, sizeof(specs));
        specs[3U].first_lba = FOREIGN_ROOT_FIRST;
        specs[3U].last_lba = FOREIGN_ROOT_FIRST + 100U;
        fixture_build_disk(specs, 4U);
        memset(&preserve_table, 0, sizeof(preserve_table));
        CHECK(installer_gpt_preserve_read(&device, 1U, &preserve_table) ==
                  INSTALLER_GPT_PRESERVE_CORRUPT_TABLE,
              "overlapping foreign entries are refused");
    }
    /* A table this writer is not allowed to rewrite. */
    {
        unsigned char array[FIXTURE_ARRAY_SECTORS * FIXTURE_SECTOR_SIZE];
        unsigned char header[FIXTURE_SECTOR_SIZE];

        fixture_build_disk(
            fixture_foreign_specs,
            sizeof(fixture_foreign_specs) /
                sizeof(fixture_foreign_specs[0U]));
        memcpy(array, fixture_array_before, sizeof(array));
        fixture_build_header(
            header, fixture_block_count, 1U, fixture_block_count - 1U,
            2U, array, 4U, FIXTURE_ENTRY_SIZE);
        memcpy(fixture_find(1U)->data, header, FIXTURE_SECTOR_SIZE);
        memset(&preserve_table, 0, sizeof(preserve_table));
        CHECK(installer_gpt_preserve_read(&device, 1U, &preserve_table) ==
                  INSTALLER_GPT_PRESERVE_UNSUPPORTED_TABLE,
              "a table with a non-standard entry count is refused");
    }
    /* Nothing was written by any of the refusals above. */
    CHECK(fixture_write_calls == 0U,
          "a refused table is refused before the first write");

    fixture_dual_boot_plan(&plan);
    fixture_reuse(&reuse);
    CHECK(installer_gpt_preserve_check(
              &preserve_table, &plan, &reuse) ==
              INSTALLER_GPT_PRESERVE_BAD_STATE &&
              installer_gpt_preserve_write(
                  &device, &plan, &reuse, &preserve_table) ==
                  INSTALLER_GPT_PRESERVE_BAD_STATE,
          "a table that was refused is never planned or written against");
    CHECK(fixture_write_calls == 0U,
          "the refused table wrote nothing");
}

static void test_unreadable_table_refused(void)
{
    struct block_device device = fixture_device();

    fixture_build_disk(
        fixture_foreign_specs,
        sizeof(fixture_foreign_specs) / sizeof(fixture_foreign_specs[0U]));
    memset(&preserve_table, 0, sizeof(preserve_table));
    fixture_fail_read_call = 2U;
    CHECK(installer_gpt_preserve_read(&device, 1U, &preserve_table) ==
              INSTALLER_GPT_PRESERVE_UNREADABLE_TABLE,
          "an I/O failure while reading the table is refused");
    CHECK(fixture_write_calls == 0U,
          "an unreadable table is refused before the first write");
    fixture_fail_read_call = 0U;
}

static void test_no_room_refused(void)
{
    struct block_device device = fixture_device();
    struct installer_layout_plan plan;
    struct installer_gpt_preserve_reuse reuse;
    struct fixture_entry_spec specs[4U];

    /*
     * A foreign entry inside the plan's span, between two of its roles: every
     * role is clear of it, but no single free region is large enough for the
     * declared footprint.  That is the NO_FREE_SPACE refusal, and it happens
     * before the first write.
     */
    memcpy(specs, fixture_foreign_specs, sizeof(specs));
    specs[3U].first_lba = APPLIANCE_PERSISTENCE_FIRST + 8192U;
    specs[3U].last_lba = specs[3U].first_lba + 4095U;
    fixture_build_disk(specs, 4U);
    fixture_dual_boot_plan(&plan);
    fixture_reuse(&reuse);
    /* Open a gap between persistence and recovery for the foreign entry. */
    plan.partitions[1U].length_lba = 8192U;
    plan.partitions[2U].first_lba =
        specs[3U].last_lba + 1U + 2048U;
    memset(&preserve_table, 0, sizeof(preserve_table));
    CHECK(installer_gpt_preserve_read(&device, 1U, &preserve_table) ==
              INSTALLER_GPT_PRESERVE_OK,
          "the packed table reads");
    CHECK(installer_gpt_preserve_check(
              &preserve_table, &plan, &reuse) ==
              INSTALLER_GPT_PRESERVE_NO_FREE_SPACE,
          "a layout that cannot fit one free region is refused");
    CHECK(installer_gpt_preserve_write(
              &device, &plan, &reuse, &preserve_table) ==
              INSTALLER_GPT_PRESERVE_NO_FREE_SPACE &&
              fixture_write_calls == 0U,
          "no free space is refused before the first write");

    /* A role that overlaps a foreign entry is the overlap refusal. */
    fixture_build_disk(
        fixture_foreign_specs,
        sizeof(fixture_foreign_specs) / sizeof(fixture_foreign_specs[0U]));
    fixture_dual_boot_plan(&plan);
    plan.partitions[0U].first_lba = FOREIGN_ROOT_FIRST + 2048U;
    memset(&preserve_table, 0, sizeof(preserve_table));
    CHECK(installer_gpt_preserve_read(&device, 1U, &preserve_table) ==
              INSTALLER_GPT_PRESERVE_OK,
          "the foreign table reads again");
    CHECK(installer_gpt_preserve_check(
              &preserve_table, &plan, &reuse) ==
              INSTALLER_GPT_PRESERVE_EXISTING_SYSTEM_OVERLAP,
          "a role that overlaps a foreign entry is refused");

    /* A plan whose roles leave the usable region. */
    fixture_dual_boot_plan(&plan);
    /*
     * The highest 1 MiB-aligned LBA on the disk: the role's 64 MiB then runs
     * past the last usable LBA.
     */
    plan.partitions[2U].first_lba =
        (FIXTURE_MEDIA_LBA / FIXTURE_ALIGN_LBA) * FIXTURE_ALIGN_LBA;
    memset(&preserve_table, 0, sizeof(preserve_table));
    CHECK(installer_gpt_preserve_read(&device, 1U, &preserve_table) ==
              INSTALLER_GPT_PRESERVE_OK,
          "the foreign table reads once more");
    CHECK(installer_gpt_preserve_check(
              &preserve_table, &plan, &reuse) ==
              INSTALLER_GPT_PRESERVE_OUT_OF_RANGE,
          "a role that leaves the usable region is refused");

    /* An unaligned role is not a product layout. */
    fixture_dual_boot_plan(&plan);
    plan.partitions[0U].first_lba = APPLIANCE_SYSTEM_FIRST + 1U;
    memset(&preserve_table, 0, sizeof(preserve_table));
    CHECK(installer_gpt_preserve_read(&device, 1U, &preserve_table) ==
              INSTALLER_GPT_PRESERVE_OK,
          "the foreign table reads for the alignment check");
    CHECK(installer_gpt_preserve_check(
              &preserve_table, &plan, &reuse) ==
              INSTALLER_GPT_PRESERVE_INVALID_ARGUMENT,
          "an unaligned role is refused");

    /* A destructive plan is not this writer's plan. */
    fixture_dual_boot_plan(&plan);
    plan.destructive = true;
    memset(&preserve_table, 0, sizeof(preserve_table));
    CHECK(installer_gpt_preserve_read(&device, 1U, &preserve_table) ==
              INSTALLER_GPT_PRESERVE_OK,
          "the foreign table reads for the destructive check");
    CHECK(installer_gpt_preserve_check(
              &preserve_table, &plan, &reuse) ==
              INSTALLER_GPT_PRESERVE_BAD_STATE,
          "the destructive whole-disk plan stays with its own writer");
    CHECK(fixture_write_calls == 0U,
          "every refusal above happened before the first write");
}

static void test_reuse_range_must_match(void)
{
    struct block_device device = fixture_device();
    struct installer_layout_plan plan;
    struct installer_gpt_preserve_reuse reuse;

    fixture_build_disk(
        fixture_foreign_specs,
        sizeof(fixture_foreign_specs) / sizeof(fixture_foreign_specs[0U]));
    fixture_dual_boot_plan(&plan);
    fixture_reuse(&reuse);
    reuse.first_lba = FOREIGN_ESP_FIRST + FIXTURE_ALIGN_LBA;
    memset(&preserve_table, 0, sizeof(preserve_table));
    CHECK(installer_gpt_preserve_read(&device, 1U, &preserve_table) ==
              INSTALLER_GPT_PRESERVE_OK,
          "the foreign table reads for the reuse check");
    CHECK(installer_gpt_preserve_check(
              &preserve_table, &plan, &reuse) ==
              INSTALLER_GPT_PRESERVE_EXISTING_SYSTEM_OVERLAP,
          "a reused ESP the disk does not carry is refused");
    CHECK(fixture_write_calls == 0U,
          "the reuse refusal happens before the first write");
}

static void test_write_failure_leaves_entries(void)
{
    struct block_device device = fixture_device();
    struct installer_layout_plan plan;
    struct installer_gpt_preserve_reuse reuse;

    fixture_build_disk(
        fixture_foreign_specs,
        sizeof(fixture_foreign_specs) / sizeof(fixture_foreign_specs[0U]));
    fixture_dual_boot_plan(&plan);
    fixture_reuse(&reuse);
    memset(&preserve_table, 0, sizeof(preserve_table));
    CHECK(installer_gpt_preserve_read(&device, 1U, &preserve_table) ==
              INSTALLER_GPT_PRESERVE_OK,
          "the foreign table reads before the failure injection");
    fixture_fail_write_call = 5U;
    CHECK(installer_gpt_preserve_write(
              &device, &plan, &reuse, &preserve_table) ==
              INSTALLER_GPT_PRESERVE_IO_ERROR,
          "an injected write failure is reported");
    check_preserved_entries_byte_identical();
    {
        size_t sector_index;

        for (sector_index = 0U;
             sector_index < FIXTURE_ARRAY_SECTORS; ++sector_index) {
            const struct fixture_sector *sector =
                fixture_find(2U + sector_index);

            CHECK(sector != NULL &&
                      memcmp(sector->data,
                             fixture_array_before[sector_index],
                             FIXTURE_SECTOR_SIZE) == 0,
                  "a failed write leaves the primary array untouched");
        }
    }
    CHECK(plan.executed == false,
          "a failed write does not mark the plan executed");
    /*
     * The operation is idempotent: re-running it completes the table without
     * duplicating anything.
     */
    fixture_fail_write_call = 0U;
    CHECK(installer_gpt_preserve_write(
              &device, &plan, &reuse, &preserve_table) ==
              INSTALLER_GPT_PRESERVE_OK &&
              installer_gpt_preserve_verify(&device, &preserve_table) ==
                  INSTALLER_GPT_PRESERVE_OK,
          "re-running the failed write completes the table");
    check_untouched_array_bytes();
    check_preserved_entries_byte_identical();
}

static void test_null_and_argument_guards(void)
{
    struct block_device device = fixture_device();
    struct installer_layout_plan plan;

    fixture_build_disk(
        fixture_foreign_specs,
        sizeof(fixture_foreign_specs) / sizeof(fixture_foreign_specs[0U]));
    fixture_dual_boot_plan(&plan);
    memset(&preserve_table, 0, sizeof(preserve_table));
    CHECK(installer_gpt_preserve_read(NULL, 1U, &preserve_table) ==
              INSTALLER_GPT_PRESERVE_NULL &&
              installer_gpt_preserve_read(&device, 0U, &preserve_table) ==
                  INSTALLER_GPT_PRESERVE_INVALID_ARGUMENT,
          "the read entry point guards its arguments");
    CHECK(installer_gpt_preserve_read(&device, 1U, &preserve_table) ==
              INSTALLER_GPT_PRESERVE_OK,
          "the table reads for the guard checks");
    CHECK(installer_gpt_preserve_check(&preserve_table, NULL, NULL) ==
              INSTALLER_GPT_PRESERVE_NULL,
          "the check entry point guards its arguments");
    {
        struct installer_layout_plan unconfirmed;
        size_t write_calls_before = fixture_write_calls;

        fixture_dual_boot_plan(&unconfirmed);
        unconfirmed.confirmed = false;
        /*
         * Planning is read-only, so it states what the write would do before
         * the operator confirms it; the write itself still refuses an
         * unconfirmed plan and writes nothing.
         */
        CHECK(installer_gpt_preserve_check(
                  &preserve_table, &unconfirmed, NULL) ==
                  INSTALLER_GPT_PRESERVE_OK,
              "an unconfirmed plan can still be checked and published");
        CHECK(installer_gpt_preserve_write(
                  &device, &unconfirmed, NULL, &preserve_table) ==
                  INSTALLER_GPT_PRESERVE_NOT_CONFIRMED &&
              fixture_write_calls == write_calls_before,
              "an unconfirmed plan is refused by the write, with no write");
    }
    CHECK(installer_gpt_preserve_write(
              &device, NULL, NULL, &preserve_table) ==
              INSTALLER_GPT_PRESERVE_NULL &&
              installer_gpt_preserve_write(
                  NULL, &plan, NULL, &preserve_table) ==
                  INSTALLER_GPT_PRESERVE_NULL,
          "the write entry point guards its arguments");
    preserve_table.version = 0U;
    CHECK(installer_gpt_preserve_write(
              &device, &plan, NULL, &preserve_table) ==
              INSTALLER_GPT_PRESERVE_BAD_VERSION,
          "a table of another version is refused");
    CHECK(fixture_write_calls == 0U,
          "no guard path wrote to the device");
}

/*
 * ITEM-501: the plan that gives up the Debian root entry and puts the three
 * roles inside the extent it occupied.  The release is named exactly as the
 * discovery names it - slot 1 is partition index 2.
 */
static void fixture_release_plan(struct installer_layout_plan *plan)
{
    fixture_dual_boot_plan(plan);
    plan->releases_partition = true;
    plan->released_index = 2U;
    plan->released_first_lba = FOREIGN_ROOT_FIRST;
    plan->released_length_lba = FOREIGN_ROOT_LENGTH;
    plan->partitions[0U].first_lba = FOREIGN_ROOT_FIRST;
    plan->partitions[1U].first_lba =
        FOREIGN_ROOT_FIRST + APPLIANCE_SYSTEM_LENGTH;
    plan->partitions[2U].first_lba =
        plan->partitions[1U].first_lba + APPLIANCE_PERSISTENCE_LENGTH;
}

static void test_release_one_partition(void)
{
    struct block_device device = fixture_device();
    struct installer_layout_plan plan;
    struct installer_gpt_preserve_reuse reuse;
    char record[INSTALLER_GPT_PRESERVE_RECORD_LENGTH];
    size_t record_length = 0U;
    uint32_t slot;

    fixture_build_disk(
        fixture_foreign_specs,
        sizeof(fixture_foreign_specs) / sizeof(fixture_foreign_specs[0U]));
    fixture_release_plan(&plan);
    fixture_reuse(&reuse);
    memset(&preserve_table, 0, sizeof(preserve_table));

    CHECK(installer_gpt_preserve_read(&device, 1U, &preserve_table) ==
              INSTALLER_GPT_PRESERVE_OK,
          "the release fixture table is read");
    CHECK(installer_gpt_preserve_check(
              &preserve_table, &plan, &reuse) ==
              INSTALLER_GPT_PRESERVE_OK,
          "the release plan fits in the extent it gives up");
    CHECK(preserve_table.released_index == 1U,
          "the released entry is the slot the plan named");
    CHECK(preserve_table.free_first_lba == FOREIGN_ROOT_FIRST &&
              preserve_table.free_length_lba ==
                  FOREIGN_ADJACENT_FIRST - FOREIGN_ROOT_FIRST,
          "the freed extent joins the free region up to the next entry");
    CHECK(installer_gpt_preserve_write(
              &device, &plan, &reuse, &preserve_table) ==
              INSTALLER_GPT_PRESERVE_OK,
          "the release write succeeds");
    CHECK(installer_gpt_preserve_verify(&device, &preserve_table) ==
              INSTALLER_GPT_PRESERVE_OK,
          "the released table verifies");
    CHECK(fixture_slot_is_zero(1U),
          "the given-up entry is zero bytes after the write");
    for (slot = 0U; slot < FIXTURE_SLOT_CAPACITY; ++slot) {
        if (slot == 1U || slot == 3U || slot == 4U || slot == 6U) {
            continue;
        }
        CHECK(fixture_slot_matches(
                  slot,
                  &fixture_array_before[0U][0U] +
                      (size_t)slot * FIXTURE_ENTRY_SIZE),
              "every entry that was not given up stays byte-identical");
    }
    CHECK(installer_gpt_preserve_render_record(
              &preserve_table, &plan, 0U, record, sizeof(record),
              &record_length) == INSTALLER_GPT_PRESERVE_OK &&
              strstr(record, "[INSTALLER-PRESERVE] plan mode=release") ==
                  record &&
              strstr(record, " released_index=1") != NULL,
          "the record names the release and its slot");

    /* A plan whose extent does not match the disk is refused. */
    fixture_build_disk(
        fixture_foreign_specs,
        sizeof(fixture_foreign_specs) / sizeof(fixture_foreign_specs[0U]));
    fixture_release_plan(&plan);
    plan.released_first_lba += 2048U;
    memset(&preserve_table, 0, sizeof(preserve_table));
    CHECK(installer_gpt_preserve_read(&device, 1U, &preserve_table) ==
              INSTALLER_GPT_PRESERVE_OK &&
              installer_gpt_preserve_check(
                  &preserve_table, &plan, &reuse) ==
                  INSTALLER_GPT_PRESERVE_EXISTING_SYSTEM_OVERLAP,
          "a released extent that is not on the disk is refused");
    CHECK(fixture_write_calls == 0U,
          "a refused release writes nothing");

    /* A role outside the released extent is not the install confirmed. */
    fixture_build_disk(
        fixture_foreign_specs,
        sizeof(fixture_foreign_specs) / sizeof(fixture_foreign_specs[0U]));
    fixture_release_plan(&plan);
    plan.partitions[0U].first_lba = APPLIANCE_SYSTEM_FIRST;
    plan.partitions[1U].first_lba =
        APPLIANCE_SYSTEM_FIRST + APPLIANCE_SYSTEM_LENGTH;
    plan.partitions[2U].first_lba =
        plan.partitions[1U].first_lba + APPLIANCE_PERSISTENCE_LENGTH;
    memset(&preserve_table, 0, sizeof(preserve_table));
    CHECK(installer_gpt_preserve_read(&device, 1U, &preserve_table) ==
              INSTALLER_GPT_PRESERVE_OK &&
              installer_gpt_preserve_check(
                  &preserve_table, &plan, &reuse) ==
                  INSTALLER_GPT_PRESERVE_INVALID_ARGUMENT,
          "a role outside the given-up extent is refused");
}

int main(void)
{
    test_preserve_foreign_table();
    test_corrupt_table_refused();
    test_unreadable_table_refused();
    test_no_room_refused();
    test_reuse_range_must_match();
    test_write_failure_leaves_entries();
    test_null_and_argument_guards();
    test_release_one_partition();
    if (failures != 0) {
        fprintf(stderr, "installer-gpt-preserve-host: %d failure(s)\n",
                failures);
        return 1;
    }
    printf("installer-gpt-preserve-host: ALL OK\n");
    return 0;
}

#else
int main(void)
{
    return 0;
}
#endif
