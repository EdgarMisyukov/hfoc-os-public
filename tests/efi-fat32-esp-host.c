#include "efi_boot_environment.h"
#include "efi_fat32_esp.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifdef HFOC_EFI_FAT32_ESP_HOST_TEST

#define FIXTURE_BLOCK_SIZE 512U
#define FIXTURE_BLOCK_COUNT 4096U
#define FIXTURE_BYTES_PER_SECTOR 512U
#define FIXTURE_SECTORS_PER_CLUSTER 1U
#define FIXTURE_RESERVED_SECTORS 32U
#define FIXTURE_FAT_COUNT 2U
#define FIXTURE_SECTORS_PER_FAT 32U
#define FIXTURE_DATA_START 96U
#define FIXTURE_ROOT_CLUSTER 2U

static unsigned char fixture_storage[
    FIXTURE_BLOCK_COUNT * FIXTURE_BLOCK_SIZE];
static bool fixture_fail_read;
static size_t fixture_fail_write_call;
static size_t fixture_write_calls;
static bool fixture_fail_flush;
static int failures;

#define CHECK(cond, msg) do { \
    if (!(cond)) { \
        fprintf(stderr, "FAIL: %s\n", msg); \
        ++failures; \
    } else { \
        printf("PASS: %s\n", msg); \
    } \
} while (0)

static const unsigned char name_efi[11U] = {
    'E', 'F', 'I', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' '
};
static const unsigned char name_keep[11U] = {
    'K', 'E', 'E', 'P', ' ', ' ', ' ', ' ', 'T', 'X', 'T'
};
static const unsigned char name_readme[11U] = {
    'R', 'E', 'A', 'D', 'M', 'E', ' ', ' ', 'T', 'X', 'T'
};
static const unsigned char name_boot[11U] = {
    'B', 'O', 'O', 'T', ' ', ' ', ' ', ' ', ' ', ' ', ' '
};
static const unsigned char name_limine[11U] = {
    'L', 'I', 'M', 'I', 'N', 'E', '~', '1', 'C', 'O', 'N'
};
static const unsigned char name_bootx64[11U] = {
    'B', 'O', 'O', 'T', 'X', '6', '4', ' ', 'E', 'F', 'I'
};
static const unsigned char name_hfoc[11U] = {
    'H', 'F', 'O', 'C', ' ', ' ', ' ', ' ', ' ', ' ', ' '
};
static const unsigned char name_state[11U] = {
    'S', 'T', 'A', 'T', 'E', ' ', ' ', ' ', ' ', ' ', ' '
};

/*
 * ITEM-398: a long-name entry as the fixture writes it - one part, the last,
 * naming the short entry that follows it, with the checksum a reader verifies
 * before it binds the two together.
 */
static unsigned char fixture_short_checksum(
    const unsigned char name[11U])
{
    unsigned char sum = 0U;
    size_t index;

    for (index = 0U; index < 11U; ++index) {
        sum = (unsigned char)(((sum & 1U) != 0U ? 0x80U : 0U) +
                              (sum >> 1U) + name[index]);
    }
    return sum;
}

static void fixture_lfn_entry(
    uint32_t lba,
    size_t index,
    const char *name,
    const unsigned char short_name[11U])
{
    static const size_t offsets[13U] = {
        1U, 3U, 5U, 7U, 9U, 14U, 16U, 18U, 20U, 22U, 24U, 28U, 30U
    };
    unsigned char *entry =
        fixture_storage + (uint64_t)lba * FIXTURE_BLOCK_SIZE +
        index * 32U;
    const size_t length = strlen(name);
    size_t character;

    memset(entry, 0, 32U);
    entry[0U] = 0x41U;
    entry[11U] = 0x0FU;
    entry[12U] = 0U;
    entry[13U] = fixture_short_checksum(short_name);
    for (character = 0U; character < 13U; ++character) {
        entry[offsets[character]] =
            character < length ? (unsigned char)name[character] : 0U;
        entry[offsets[character] + 1U] = 0U;
    }
}

static void fixture_store_le16(
    unsigned char *bytes,
    uint16_t value)
{
    bytes[0U] = (unsigned char)(value & 0xFFU);
    bytes[1U] = (unsigned char)(value >> 8U);
}

static void fixture_store_le32(
    unsigned char *bytes,
    uint32_t value)
{
    bytes[0U] = (unsigned char)(value & 0xFFU);
    bytes[1U] = (unsigned char)((value >> 8U) & 0xFFU);
    bytes[2U] = (unsigned char)((value >> 16U) & 0xFFU);
    bytes[3U] = (unsigned char)((value >> 24U) & 0xFFU);
}

static void fixture_fat_set(
    uint32_t cluster,
    uint32_t value)
{
    uint32_t fat_index;

    for (fat_index = 0U; fat_index < FIXTURE_FAT_COUNT;
         ++fat_index) {
        uint64_t lba = FIXTURE_RESERVED_SECTORS +
            (uint64_t)fat_index * FIXTURE_SECTORS_PER_FAT;
        unsigned char *entry =
            fixture_storage + lba * FIXTURE_BLOCK_SIZE +
            (size_t)cluster * 4U;
        fixture_store_le32(entry, value);
    }
}

static void fixture_dir_entry(
    uint32_t lba,
    size_t index,
    const unsigned char name[11U],
    unsigned char attributes,
    uint32_t cluster,
    uint32_t size)
{
    unsigned char *entry =
        fixture_storage + (uint64_t)lba * FIXTURE_BLOCK_SIZE +
        index * 32U;

    memset(entry, 0, 32U);
    memcpy(entry, name, 11U);
    entry[11U] = attributes;
    fixture_store_le16(entry + 20U,
        (uint16_t)(cluster >> 16U));
    fixture_store_le16(entry + 26U,
        (uint16_t)(cluster & 0xFFFFU));
    fixture_store_le32(entry + 28U, size);
}

static void fixture_init(void)
{
    memset(fixture_storage, 0, sizeof(fixture_storage));
    fixture_fail_read = false;
    fixture_fail_write_call = 0U;
    fixture_write_calls = 0U;
    fixture_fail_flush = false;

    fixture_store_le16(fixture_storage + 11U,
        FIXTURE_BYTES_PER_SECTOR);
    fixture_storage[13U] = FIXTURE_SECTORS_PER_CLUSTER;
    fixture_store_le16(fixture_storage + 14U,
        FIXTURE_RESERVED_SECTORS);
    fixture_storage[16U] = FIXTURE_FAT_COUNT;
    /*
     * ITEM-405: the volume's declared extent (FAT32 offset 32).  The bind
     * treats the declared total as authoritative: it must exist, it must fit
     * the partition, and the cluster space is derived from it.  A formatter
     * writes this field, so a fixture that leaves it at zero is not a volume
     * any writer would produce - and is refused, which the case below proves.
     */
    fixture_store_le32(fixture_storage + 32U, FIXTURE_BLOCK_COUNT);
    fixture_store_le32(fixture_storage + 36U,
        FIXTURE_SECTORS_PER_FAT);
    fixture_store_le32(fixture_storage + 44U,
        FIXTURE_ROOT_CLUSTER);
    fixture_storage[510U] = 0x55U;
    fixture_storage[511U] = 0xAAU;

    fixture_fat_set(FIXTURE_ROOT_CLUSTER, 0x0FFFFFFFU);
    fixture_fat_set(3U, 0x0FFFFFFFU);
    fixture_fat_set(4U, 0x0FFFFFFFU);
    fixture_fat_set(6U, 0x0FFFFFFFU);

    fixture_dir_entry(
        96U, 0U, name_efi, 0x10U, 3U, 0U);
    fixture_dir_entry(
        96U, 1U, name_readme, 0x20U, 4U, 11U);
    memcpy(fixture_storage + 98U * FIXTURE_BLOCK_SIZE,
           "hello root\n", 11U);

    fixture_dir_entry(
        97U, 0U, (const unsigned char *)".          ",
        0x10U, 3U, 0U);
    fixture_dir_entry(
        97U, 1U, (const unsigned char *)"..         ",
        0x10U, FIXTURE_ROOT_CLUSTER, 0U);
    fixture_dir_entry(
        97U, 2U, name_keep, 0x20U, 6U, 13U);
    memcpy(fixture_storage + 100U * FIXTURE_BLOCK_SIZE,
           "keep me esp!\n", 13U);
}

static enum block_device_status fixture_read(
    struct block_device *device,
    uint64_t offset,
    void *buffer,
    size_t length)
{
    (void)device;
    if (fixture_fail_read ||
        offset > sizeof(fixture_storage) ||
        length > sizeof(fixture_storage) - (size_t)offset) {
        return BLOCK_DEVICE_IO_ERROR;
    }
    memcpy(buffer, fixture_storage + (size_t)offset, length);
    return BLOCK_DEVICE_OK;
}

static enum block_device_status fixture_write(
    struct block_device *device,
    uint64_t offset,
    const void *buffer,
    size_t length)
{
    (void)device;
    ++fixture_write_calls;
    if (fixture_fail_write_call != 0U &&
        fixture_write_calls == fixture_fail_write_call) {
        fixture_fail_write_call = 0U;
        return BLOCK_DEVICE_IO_ERROR;
    }
    if (offset > sizeof(fixture_storage) ||
        length > sizeof(fixture_storage) - (size_t)offset) {
        return BLOCK_DEVICE_IO_ERROR;
    }
    memcpy(fixture_storage + (size_t)offset, buffer, length);
    return BLOCK_DEVICE_OK;
}

static enum block_device_status fixture_flush(
    struct block_device *device)
{
    (void)device;
    if (fixture_fail_flush) {
        fixture_fail_flush = false;
        return BLOCK_DEVICE_IO_ERROR;
    }
    return BLOCK_DEVICE_OK;
}

static struct block_device make_device(void)
{
    struct block_device device;

    memset(&device, 0, sizeof(device));
    device.block_size = FIXTURE_BLOCK_SIZE;
    device.block_count = FIXTURE_BLOCK_COUNT;
    device.read_fn = fixture_read;
    device.write_fn = fixture_write;
    device.flush_fn = fixture_flush;
    return device;
}

static enum efi_boot_variable_io_status fixture_read_variable(
    void *context,
    const struct efi_boot_variable_key *key,
    unsigned char *data,
    size_t capacity,
    size_t *length)
{
    (void)context;
    (void)key;
    (void)data;
    (void)capacity;
    (void)length;
    return EFI_BOOT_VARIABLE_IO_NOT_FOUND;
}

static enum efi_boot_variable_io_status fixture_write_variable(
    void *context,
    const struct efi_boot_variable_key *key,
    const unsigned char *data,
    size_t length)
{
    (void)context;
    (void)key;
    (void)data;
    (void)length;
    return EFI_BOOT_VARIABLE_IO_OK;
}

static bool fixture_environment(
    struct efi_boot_environment *environment,
    struct block_device *device)
{
    memset(environment, 0, sizeof(*environment));
    return efi_boot_environment_init(
        environment, EFI_BOOT_FIRMWARE_UEFI, device,
        NULL, fixture_read_variable,
        fixture_write_variable) == EFI_BOOT_ENVIRONMENT_OK;
}

static bool fixture_preserved_bytes(void)
{
    return memcmp(
        fixture_storage + 98U * FIXTURE_BLOCK_SIZE,
        "hello root\n", 11U) == 0 &&
        memcmp(
        fixture_storage + 100U * FIXTURE_BLOCK_SIZE,
        "keep me esp!\n", 13U) == 0 &&
        fixture_storage[96U * FIXTURE_BLOCK_SIZE + 1U] == 'F' &&
        fixture_storage[96U * FIXTURE_BLOCK_SIZE + 2U] == 'I' &&
        fixture_storage[96U * FIXTURE_BLOCK_SIZE + 11U] == 0x10U &&
        fixture_storage[97U * FIXTURE_BLOCK_SIZE + 2U * 32U +
            1U] == 'E' &&
        fixture_storage[97U * FIXTURE_BLOCK_SIZE + 2U * 32U +
            2U] == 'E' &&
        fixture_storage[97U * FIXTURE_BLOCK_SIZE + 2U * 32U +
            3U] == 'P';
}

static void test_bind_and_path_contracts(void)
{
    struct block_device device = make_device();
    struct efi_boot_environment environment;
    struct efi_fat32_esp esp;
    static const char canonical[] =
        EFI_FAT32_ESP_CANONICAL_PATH;
    static const char escaped[] = "\\EFI\\..\\BOOTX64.EFI";
    static const char short_path[] = "\\EFI\\HFOC";

    fixture_init();
    CHECK(fixture_environment(&environment, &device),
          "valid fixture environment initializes");
    CHECK(efi_fat32_esp_bind(
              NULL, 0U, FIXTURE_BLOCK_COUNT, &esp) ==
              EFI_FAT32_ESP_NULL,
          "bind rejects null environment");
    CHECK(efi_fat32_esp_bind(
              &environment, 0U, FIXTURE_BLOCK_COUNT, NULL) ==
              EFI_FAT32_ESP_NULL,
          "bind rejects null esp");
    CHECK(efi_fat32_esp_bind(
              &environment, FIXTURE_BLOCK_COUNT, 1U, &esp) ==
              EFI_FAT32_ESP_OUT_OF_RANGE,
          "bind rejects out-of-range ESP");
    CHECK(efi_fat32_esp_bind(
              &environment, FIXTURE_BLOCK_COUNT - 1U, 2U,
              &esp) == EFI_FAT32_ESP_OUT_OF_RANGE,
          "bind rejects ESP crossing device end");
    CHECK(efi_fat32_esp_bind(
              &environment, 0U, 0U, &esp) ==
              EFI_FAT32_ESP_OUT_OF_RANGE,
          "bind rejects zero-length ESP");
    CHECK(efi_fat32_esp_bind(
              &environment, 0U, FIXTURE_BLOCK_COUNT, &esp) ==
              EFI_FAT32_ESP_OK &&
              esp.bound && esp.version == EFI_FAT32_ESP_VERSION,
          "valid FAT32 ESP binds");
    CHECK(efi_fat32_esp_bind(
              &environment, 0U, FIXTURE_BLOCK_COUNT, &esp) ==
              EFI_FAT32_ESP_OK &&
              esp.cluster_count > 0U,
          "ESP bind records geometry");

    memset(fixture_storage, 0, 512U);
    fixture_storage[510U] = 0x55U;
    fixture_storage[511U] = 0xAAU;
    CHECK(efi_fat32_esp_bind(
              &environment, 0U, FIXTURE_BLOCK_COUNT, &esp) ==
              EFI_FAT32_ESP_UNSUPPORTED_GEOMETRY,
          "bind rejects non-FAT32 boot sector");
    fixture_init();

    CHECK(efi_fat32_esp_validate_path(
              canonical, sizeof(canonical) - 1U) ==
              EFI_FAT32_ESP_OK,
          "canonical HFOC path validates");
    CHECK(efi_fat32_esp_validate_path(
              escaped, sizeof(escaped) - 1U) ==
              EFI_FAT32_ESP_PATH_DENIED,
          "path escape is denied");
    CHECK(efi_fat32_esp_validate_path(
              short_path, sizeof(short_path) - 1U) ==
              EFI_FAT32_ESP_PATH_DENIED,
          "non-payload path is denied");
    CHECK(efi_fat32_esp_validate_path(
              NULL, 1U) == EFI_FAT32_ESP_NULL,
          "null path is rejected");
}

static void test_volume_must_fit_its_partition(void)
{
    struct block_device device = make_device();
    struct efi_boot_environment environment;
    struct efi_fat32_esp esp;
    unsigned char payload[1200U];
    unsigned char read_back[1200U];
    size_t read_size = 0U;
    size_t index;
    bool beyond_volume_zero = true;

    /*
     * The installed layout declares a 67584-sector (33 MiB) ESP role.  A disk
     * that carried another system keeps that system's filesystem in the range
     * the role now covers: on the acceptance host the ESP still held a FAT32
     * whose boot sector described the 100 MiB volume the previous partition
     * table had - OEM "MSDOS5.0", reserved_sectors=6654, 2 FATs,
     * sectors_per_fat=769, sectors_per_cluster=2 and total_sectors=204800 -
     * inside a 33 MiB role.  The bind compared those numbers only with the
     * partition length it was handed and never read the volume's own total, so
     * the stale volume bound successfully, was never formatted, and the
     * installed boot set was staged against a geometry the volume does not
     * have.
     */
    device.block_count = 67584U;
    fixture_init();
    CHECK(fixture_environment(&environment, &device),
          "stale-volume fixture environment initializes");

    memset(fixture_storage, 0, 512U);
    memcpy(fixture_storage + 3U, "MSDOS5.0", 8U);
    fixture_store_le16(fixture_storage + 11U,
        FIXTURE_BYTES_PER_SECTOR);
    fixture_storage[13U] = 2U;
    fixture_store_le16(fixture_storage + 14U, 6654U);
    fixture_storage[16U] = 2U;
    fixture_store_le32(fixture_storage + 32U, 204800U);
    fixture_store_le32(fixture_storage + 36U, 769U);
    fixture_store_le32(fixture_storage + 44U, FIXTURE_ROOT_CLUSTER);
    fixture_storage[510U] = 0x55U;
    fixture_storage[511U] = 0xAAU;
    memset(&esp, 0, sizeof(esp));
    CHECK(efi_fat32_esp_bind(&environment, 0U, 67584U, &esp) ==
              EFI_FAT32_ESP_UNSUPPORTED_GEOMETRY,
          "a volume declaring more sectors than its partition is refused");
    CHECK(!esp.bound && esp.volume_sectors == 0U &&
              esp.cluster_count == 0U,
          "the refused volume publishes no geometry");

    /*
     * A boot sector that declares no extent at all is not a volume this writer
     * may use: without the declared total there is nothing to check the
     * partition against, so it is refused instead of trusted.
     */
    fixture_store_le32(fixture_storage + 32U, 0U);
    fixture_store_le16(fixture_storage + 19U, 0U);
    CHECK(efi_fat32_esp_bind(&environment, 0U, 67584U, &esp) ==
              EFI_FAT32_ESP_UNSUPPORTED_GEOMETRY,
          "a volume that declares no extent is refused");

    /* A volume whose FAT cannot address its declared data region: corrupt. */
    fixture_store_le32(fixture_storage + 32U, 65536U);
    fixture_store_le32(fixture_storage + 36U, 1U);
    CHECK(efi_fat32_esp_bind(&environment, 0U, 67584U, &esp) ==
              EFI_FAT32_ESP_CORRUPT,
          "a FAT that cannot address the declared volume is corrupt");

    /*
     * The other direction: the partition is larger than the volume it holds.
     * That binds, and every cluster the caller can reach comes from the
     * declared volume - 1952 clusters of the fixture's geometry - not from the
     * 67584-sector partition.
     */
    fixture_init();
    fixture_store_le32(fixture_storage + 32U, 2048U);
    memset(&esp, 0, sizeof(esp));
    CHECK(efi_fat32_esp_bind(&environment, 0U, 67584U, &esp) ==
              EFI_FAT32_ESP_OK,
          "a volume that fits inside its partition binds");
    CHECK(esp.volume_sectors == 2048U &&
              esp.block_count == 67584U &&
              esp.cluster_count ==
                  (2048U - FIXTURE_DATA_START) /
                      FIXTURE_SECTORS_PER_CLUSTER,
          "the cluster space is derived from the declared volume");

    for (index = 0U; index < sizeof(payload); ++index) {
        payload[index] =
            (unsigned char)("HFOC-ITEM-405"[(index * 5U) % 13U]);
    }
    CHECK(efi_fat32_esp_install_payload(
              &esp, payload, sizeof(payload)) == EFI_FAT32_ESP_OK,
          "the declared volume takes the payload");
    CHECK(efi_fat32_esp_read_payload(
              &esp, read_back, sizeof(read_back), &read_size) ==
              EFI_FAT32_ESP_OK &&
              read_size == sizeof(payload) &&
              memcmp(read_back, payload, sizeof(payload)) == 0,
          "the payload reads back out of the declared volume");
    for (index = 2048U * FIXTURE_BLOCK_SIZE;
         index < sizeof(fixture_storage); ++index) {
        if (fixture_storage[index] != 0U) {
            beyond_volume_zero = false;
            break;
        }
    }
    CHECK(beyond_volume_zero,
          "nothing is written past the volume the partition holds");
}

static void test_install_update_verify_and_preservation(void)
{
    struct block_device device = make_device();
    struct efi_boot_environment environment;
    struct efi_fat32_esp esp;
    unsigned char payload_one[513U];
    unsigned char payload_two[600U];
    unsigned char read_back[600U];
    size_t read_size = 0U;
    size_t index;

    for (index = 0U; index < sizeof(payload_one); ++index) {
        payload_one[index] =
            (unsigned char)("HFOC-PAYLOAD-ONE"[(index * 7U) %
                15U]);
    }
    for (index = 0U; index < sizeof(payload_two); ++index) {
        payload_two[index] =
            (unsigned char)("HFOC-PAYLOAD-TWO"[(index * 11U) %
                15U]);
    }

    fixture_init();
    CHECK(fixture_environment(&environment, &device) &&
          efi_fat32_esp_bind(
              &environment, 0U, FIXTURE_BLOCK_COUNT, &esp) ==
              EFI_FAT32_ESP_OK,
          "install fixture binds");
    CHECK(efi_fat32_esp_install_payload(
              &esp, NULL, 0U) == EFI_FAT32_ESP_NULL,
          "install rejects null payload");
    CHECK(efi_fat32_esp_install_payload(
              &esp, payload_one, 0U) ==
              EFI_FAT32_ESP_NULL,
          "install rejects zero payload");
    CHECK(efi_fat32_esp_install_payload(
              &esp, payload_one, sizeof(payload_one)) ==
              EFI_FAT32_ESP_OK,
          "install writes bounded canonical payload");
    CHECK(fixture_preserved_bytes(),
          "install preserves unrelated ESP files");
    CHECK(efi_fat32_esp_verify_payload(&esp) ==
              EFI_FAT32_ESP_OK,
          "verify detects installed payload");
    memset(read_back, 0, sizeof(read_back));
    CHECK(efi_fat32_esp_read_payload(
              &esp, read_back, sizeof(read_back),
              &read_size) == EFI_FAT32_ESP_OK &&
              read_size == sizeof(payload_one) &&
              memcmp(read_back, payload_one,
                     sizeof(payload_one)) == 0,
          "read returns exact installed payload");

    CHECK(efi_fat32_esp_install_payload(
              &esp, payload_two, sizeof(payload_two)) ==
              EFI_FAT32_ESP_OK,
          "install updates canonical payload in place");
    CHECK(fixture_preserved_bytes(),
          "update preserves unrelated ESP files");
    memset(read_back, 0, sizeof(read_back));
    CHECK(efi_fat32_esp_read_payload(
              &esp, read_back, sizeof(read_back),
              &read_size) == EFI_FAT32_ESP_OK &&
              read_size == sizeof(payload_two) &&
              memcmp(read_back, payload_two,
                     sizeof(payload_two)) == 0,
          "update returns exact replacement payload");
}

static void test_failure_isolation(void)
{
    struct block_device device = make_device();
    struct efi_boot_environment environment;
    struct efi_fat32_esp esp;
    unsigned char payload[512U];
    unsigned char read_back[512U];
    size_t read_size = 0U;

    memset(payload, 0xA5U, sizeof(payload));
    fixture_init();
    CHECK(fixture_environment(&environment, &device) &&
          efi_fat32_esp_bind(
              &environment, 0U, FIXTURE_BLOCK_COUNT, &esp) ==
              EFI_FAT32_ESP_OK &&
          efi_fat32_esp_install_payload(
              &esp, payload, sizeof(payload)) ==
              EFI_FAT32_ESP_OK,
          "failure fixture installs baseline payload");

    fixture_write_calls = 0U;
    fixture_fail_write_call = 2U;
    memset(payload, 0x5AU, sizeof(payload));
    CHECK(efi_fat32_esp_install_payload(
              &esp, payload, sizeof(payload)) !=
              EFI_FAT32_ESP_OK,
          "payload write failure is reported");
    memset(read_back, 0, sizeof(read_back));
    CHECK(efi_fat32_esp_read_payload(
              &esp, read_back, sizeof(read_back),
              &read_size) == EFI_FAT32_ESP_OK &&
              read_size == sizeof(read_back) &&
              read_back[0] == 0xA5U,
          "failed install leaves prior payload intact");
    CHECK(fixture_preserved_bytes(),
          "failed install preserves unrelated ESP files");

    memset(payload, 0xA5U, sizeof(payload));
    fixture_init();
    CHECK(fixture_environment(&environment, &device) &&
          efi_fat32_esp_bind(
              &environment, 0U, FIXTURE_BLOCK_COUNT, &esp) ==
              EFI_FAT32_ESP_OK &&
          efi_fat32_esp_install_payload(
              &esp, payload, sizeof(payload)) ==
              EFI_FAT32_ESP_OK,
          "flush failure fixture installs baseline payload");
    fixture_write_calls = 0U;
    fixture_fail_flush = true;
    CHECK(efi_fat32_esp_install_payload(
              &esp, read_back, sizeof(read_back)) !=
              EFI_FAT32_ESP_OK,
          "flush failure is reported");
    memset(read_back, 0, sizeof(read_back));
    CHECK(efi_fat32_esp_read_payload(
              &esp, read_back, sizeof(read_back),
              &read_size) == EFI_FAT32_ESP_OK &&
              read_size == sizeof(read_back) &&
              read_back[0] == 0xA5U,
          "failed flush leaves prior payload intact");
}

/* The FAT value of one cluster, read back the way a reader walks a chain. */
static uint32_t fixture_fat_get(uint32_t cluster)
{
    const uint64_t lba = FIXTURE_RESERVED_SECTORS;
    const unsigned char *entry =
        fixture_storage + lba * FIXTURE_BLOCK_SIZE + (size_t)cluster * 4U;

    return (uint32_t)entry[0] | ((uint32_t)entry[1] << 8U) |
           ((uint32_t)entry[2] << 16U) | ((uint32_t)entry[3] << 24U);
}

/*
 * ITEM-410: the fixed-extent file the appliance boot log lives in.
 *
 * The proof has to show three things a hardware run depends on: the file is
 * created once at the declared size; a later write into it touches data bytes
 * of its own clusters and nothing else - not the FAT, not the directory, not
 * another file - so a shared ESP keeps every byte that is not ours; and the
 * second boot *reuses* the extent without writing anything at all, which is
 * what makes the log survive the boot instead of starting over.
 */
static void test_fixed_extent_file(void)
{
    struct block_device device = make_device();
    struct efi_boot_environment environment;
    struct efi_fat32_esp esp;
    static unsigned char fat_before[
        FIXTURE_FAT_COUNT * FIXTURE_SECTORS_PER_FAT *
        FIXTURE_BLOCK_SIZE];
    static unsigned char directory_before[FIXTURE_BLOCK_SIZE];
    static unsigned char storage_before[
        FIXTURE_BLOCK_COUNT * FIXTURE_BLOCK_SIZE];
    static unsigned char record[EFI_FAT32_ESP_SECTOR_SIZE];
    static unsigned char read_back[EFI_FAT32_ESP_SECTOR_SIZE];
    static const char log_path[] = "\\EFI\\HFOC\\BOOTLOG.BIN";
    static const uint32_t log_size = 262144U;
    uint32_t first_cluster = 0U;
    uint32_t probed_size = 0U;
    uint32_t cluster;
    size_t length = 0U;
    size_t index;
    bool reused = true;
    bool present = false;
    size_t write_calls_before;

    fixture_init();
    CHECK(fixture_environment(&environment, &device) == true,
          "the fixed-extent fixture binds an ESP environment");
    CHECK(efi_fat32_esp_bind(
              &environment, 0U, FIXTURE_BLOCK_COUNT, &esp) ==
              EFI_FAT32_ESP_OK,
          "the fixed-extent fixture binds the volume");

    CHECK(efi_fat32_esp_create_fixed_file(
              &esp, log_path, log_size, &first_cluster, &reused) ==
              EFI_FAT32_ESP_OK,
          "the fixed extent is created at the declared size");
    CHECK(!reused && first_cluster >= 2U,
          "a created extent is not a reused one and names its first cluster");
    CHECK(efi_fat32_esp_probe_file(
              &esp, log_path, &present, &probed_size) == EFI_FAT32_ESP_OK &&
              present && probed_size == log_size,
          "the created extent is exactly the declared size");
    for (index = 0U; index < sizeof(record); ++index) {
        record[index] = 0U;
    }
    CHECK(efi_fat32_esp_read_file_range(
              &esp, log_path, 0U, read_back, sizeof(read_back), &length,
              NULL) == EFI_FAT32_ESP_OK &&
              memcmp(read_back, record, sizeof(record)) == 0,
          "a freshly created extent reads back as zeroes, not as the bytes "
          "the clusters held before");
    CHECK(efi_fat32_esp_read_file_range(
              &esp, log_path, log_size - EFI_FAT32_ESP_SECTOR_SIZE,
              read_back, sizeof(read_back), &length, &probed_size) ==
              EFI_FAT32_ESP_OK &&
              memcmp(read_back, record, sizeof(record)) == 0 &&
              probed_size == log_size,
          "the last slot of the extent is inside it and zeroed too");

    /*
     * The second boot: the extent already exists with the declared size and
     * the declared chain, so the create path must write nothing at all.
     */
    write_calls_before = fixture_write_calls;
    CHECK(efi_fat32_esp_create_fixed_file(
              &esp, log_path, log_size, &first_cluster, &reused) ==
              EFI_FAT32_ESP_OK && reused,
          "an extent that already has the declared size is reused");
    CHECK(fixture_write_calls == write_calls_before,
          "reusing the extent writes not one byte to the device");

    /* Write one record slot into the middle of the extent. */
    for (index = 0U; index < sizeof(record); ++index) {
        record[index] = (unsigned char)("HFOC-BOOTLOG-ITEM-410"[(index * 7U) %
                                                                 20U]);
    }
    memcpy(fat_before, fixture_storage +
               FIXTURE_RESERVED_SECTORS * FIXTURE_BLOCK_SIZE,
           sizeof(fat_before));
    memcpy(directory_before,
           fixture_storage + 97U * FIXTURE_BLOCK_SIZE,
           sizeof(directory_before));
    memcpy(storage_before, fixture_storage, sizeof(storage_before));
    CHECK(efi_fat32_esp_write_file_range(
              &esp, log_path, 4096U, record, sizeof(record)) ==
              EFI_FAT32_ESP_OK,
          "a record slot inside the extent is written in place");
    CHECK(efi_fat32_esp_read_file_range(
              &esp, log_path, 4096U, read_back, sizeof(read_back), &length,
              NULL) == EFI_FAT32_ESP_OK &&
              memcmp(read_back, record, sizeof(record)) == 0,
          "the slot reads back byte for byte");
    CHECK(memcmp(fixture_storage +
                     FIXTURE_RESERVED_SECTORS * FIXTURE_BLOCK_SIZE,
                 fat_before, sizeof(fat_before)) == 0,
          "writing into the extent changes no byte of the file allocation "
          "table");
    CHECK(memcmp(fixture_storage + 97U * FIXTURE_BLOCK_SIZE,
                 directory_before, sizeof(directory_before)) == 0,
          "writing into the extent changes no byte of the directory");
    CHECK(fixture_preserved_bytes(),
          "writing into the extent leaves the other files of the volume "
          "byte for byte");
    {
        size_t changed = 0U;
        uint32_t chain[600U];
        size_t chain_length = 0U;
        uint32_t walk = first_cluster;

        while (walk >= 2U && walk < 0x0FFFFFF8U &&
               chain_length < sizeof(chain) / sizeof(chain[0])) {
            chain[chain_length++] = walk;
            walk = fixture_fat_get(walk);
        }
        CHECK(chain_length == log_size / FIXTURE_BYTES_PER_SECTOR,
              "the extent is one chain of exactly its own clusters");
        size_t outside = 0U;

        for (index = 0U; index < sizeof(storage_before); ++index) {
            size_t link;

            if (fixture_storage[index] == storage_before[index]) {
                continue;
            }
            ++changed;
            for (link = 0U; link < chain_length; ++link) {
                const uint64_t cluster_byte =
                    (FIXTURE_DATA_START +
                     (uint64_t)(chain[link] - 2U) *
                         FIXTURE_SECTORS_PER_CLUSTER) *
                    FIXTURE_BLOCK_SIZE;

                if ((uint64_t)index >= cluster_byte &&
                    (uint64_t)index <
                        cluster_byte + FIXTURE_BLOCK_SIZE) {
                    break;
                }
            }
            if (link >= chain_length) {
                ++outside;
            }
        }
        CHECK(outside == 0U,
              "every byte the write changed belongs to the extent's own "
              "clusters");
        CHECK(changed == EFI_FAT32_ESP_SECTOR_SIZE,
              "exactly one slot's worth of bytes changed on the volume");
    }

    /* A range that would grow the file is refused, and nothing moves. */
    memcpy(storage_before, fixture_storage, sizeof(storage_before));
    CHECK(efi_fat32_esp_write_file_range(
              &esp, log_path, log_size, record, sizeof(record)) ==
              EFI_FAT32_ESP_OUT_OF_RANGE,
          "a write past the declared extent is refused");
    CHECK(efi_fat32_esp_write_file_range(
              &esp, log_path, log_size - 16U, record, sizeof(record)) ==
              EFI_FAT32_ESP_OUT_OF_RANGE,
          "a write that straddles the end of the extent is refused");
    CHECK(memcmp(fixture_storage, storage_before, sizeof(storage_before)) ==
              0,
          "a refused write changes nothing at all");
    CHECK(efi_fat32_esp_read_file_range(
              &esp, log_path, 0U, read_back, EFI_FAT32_ESP_SECTOR_SIZE,
              &length, NULL) == EFI_FAT32_ESP_OK,
          "the extent is still readable after the refused writes");

    /* A size that is not a whole number of clusters is not an extent. */
    CHECK(efi_fat32_esp_create_fixed_file(
              &esp, log_path, log_size - 1U, &first_cluster, &reused) ==
              EFI_FAT32_ESP_OUT_OF_RANGE,
          "a size that is not a whole number of clusters is refused");
    CHECK(efi_fat32_esp_create_fixed_file(
              &esp, log_path, 0U, &first_cluster, &reused) ==
              EFI_FAT32_ESP_OUT_OF_RANGE,
          "an empty extent is refused");
    CHECK(efi_fat32_esp_write_file_range(
              &esp, "\\EFI\\HFOC\\ABSENT.BIN", 0U, record,
              sizeof(record)) == EFI_FAT32_ESP_NOT_FOUND,
          "writing to a file that does not exist is refused");
    CHECK(efi_fat32_esp_read_file_range(
              &esp, log_path, log_size - 8U, read_back,
              EFI_FAT32_ESP_SECTOR_SIZE, &length, NULL) ==
              EFI_FAT32_ESP_CAPACITY,
          "reading past the extent is refused instead of reading garbage");

    /*
     * An extent that exists at a different size - the shape a log written by
     * an older declaration leaves behind - is replaced by the declared one,
     * and the clusters it held come back to the volume.
     */
    {
        static unsigned char small[1024U];
        struct efi_fat32_esp_payload payload;

        memset(&small, 0x5AU, sizeof(small));
        payload.path = "\\EFI\\HFOC\\SMALL.BIN";
        payload.data = small;
        payload.size = sizeof(small);
        CHECK(efi_fat32_esp_install_payload_set(&esp, &payload, 1U) ==
                  EFI_FAT32_ESP_OK,
              "the fixture stages a small file at its own path");
        CHECK(efi_fat32_esp_create_fixed_file(
                  &esp, "\\EFI\\HFOC\\SMALL.BIN", 2048U, &first_cluster,
                  &reused) == EFI_FAT32_ESP_OK &&
                  !reused,
              "a file at the wrong size is replaced by the declared extent");
        CHECK(efi_fat32_esp_probe_file(
                  &esp, "\\EFI\\HFOC\\SMALL.BIN", &present, &probed_size) ==
                  EFI_FAT32_ESP_OK &&
                  present && probed_size == 2048U,
              "the replaced file reports the declared size");
        CHECK(efi_fat32_esp_read_file_range(
                  &esp, "\\EFI\\HFOC\\SMALL.BIN", 0U, read_back,
                  EFI_FAT32_ESP_SECTOR_SIZE, &length, NULL) ==
                  EFI_FAT32_ESP_OK &&
                  read_back[0] == 0U,
              "the replaced extent is zeroed instead of carrying the old "
              "file's bytes");
        CHECK(fixture_preserved_bytes(),
              "replacing the extent leaves the other files alone");
    }

    /* The chain is bounded: a corrupt link stops the walk. */
    {
        uint32_t chain[600U];
        size_t chain_length = 0U;
        uint32_t walk = first_cluster;

        while (walk >= 2U && walk < 0x0FFFFFF8U &&
               chain_length < sizeof(chain) / sizeof(chain[0])) {
            chain[chain_length++] = walk;
            walk = fixture_fat_get(walk);
        }
        CHECK(chain_length > 2U,
              "the extent has a chain to corrupt");
        fixture_fat_set(chain[1U], 0x0FFFFFF7U);
        CHECK(efi_fat32_esp_read_file_range(
                  &esp, log_path, 0U, read_back, sizeof(read_back),
                  &length, NULL) == EFI_FAT32_ESP_CORRUPT ||
                  efi_fat32_esp_read_file_range(
                      &esp, log_path, 0U, read_back, sizeof(read_back),
                      &length, NULL) == EFI_FAT32_ESP_OK,
              "a corrupt chain stops the walk instead of looping");
        fixture_fat_set(chain[1U], chain[2U]);
        fixture_fat_set(chain[chain_length - 1U], 0x0FFFFFFFU);
    }
    (void)cluster;
}

/* The first cluster of the entry `short_name` names, as the volume stores it. */
static bool fixture_directory_entry_cluster(
    const unsigned char *cluster,
    const unsigned char short_name[11U],
    uint32_t *first_cluster)
{
    size_t index;

    for (index = 0U; index + 32U <= FIXTURE_BLOCK_SIZE; index += 32U) {
        const unsigned char *entry = cluster + index;
        uint32_t high;
        uint32_t low;

        if (entry[0] == 0x00U || entry[0] == 0xE5U ||
            entry[11U] == 0x0FU) {
            continue;
        }
        if (memcmp(entry, short_name, 11U) != 0) {
            continue;
        }
        high = (uint32_t)entry[20U] | ((uint32_t)entry[21U] << 8);
        low = (uint32_t)entry[26U] | ((uint32_t)entry[27U] << 8);
        *first_cluster = (high << 16) | low;
        return true;
    }
    return false;
}

static bool fixture_cluster_carries(
    uint32_t cluster,
    unsigned char value)
{
    const unsigned char *bytes =
        fixture_storage +
        ((uint64_t)FIXTURE_DATA_START + (uint64_t)(cluster - 2U)) *
            FIXTURE_BLOCK_SIZE;
    size_t index;

    for (index = 0U; index < FIXTURE_BLOCK_SIZE; ++index) {
        if (bytes[index] == value) {
            return true;
        }
    }
    return false;
}

/*
 * ITEM-487: the range writer's probe counts what a transfer costs.
 *
 * The `hw15` flush measurement says the durable log's writes are dominated by
 * two things a caller cannot see from its own side: resolving the path on every
 * call, and walking the file's FAT chain from its first cluster to the offset.
 * The probe is what makes them visible, so this case pins its counts on a real
 * volume, and pins the honest zero: this proof links no cycle counter, so the
 * two microsecond spans read zero rather than a fabricated number.
 */
static void test_range_probe_counts_the_work(void)
{
    struct block_device device = make_device();
    struct efi_boot_environment environment;
    struct efi_fat32_esp esp;
    struct efi_fat32_esp_io_stats stats;
    static unsigned char record[4U * EFI_FAT32_ESP_SECTOR_SIZE];
    static const char log_path[] = "\\EFI\\HFOC\\BOOTLOG.BIN";
    const uint32_t log_size = 262144U;
    uint32_t first_cluster = 0U;
    bool reused = true;
    size_t index;

    fixture_init();
    CHECK(fixture_environment(&environment, &device) == true,
          "the probe fixture binds an ESP environment");
    CHECK(efi_fat32_esp_bind(
              &environment, 0U, FIXTURE_BLOCK_COUNT, &esp) ==
              EFI_FAT32_ESP_OK,
          "the probe fixture binds the volume");
    CHECK(efi_fat32_esp_create_fixed_file(
              &esp, log_path, log_size, &first_cluster, &reused) ==
              EFI_FAT32_ESP_OK,
          "the probe fixture has an extent to write into");
    for (index = 0U; index < sizeof(record); ++index) {
        record[index] = (unsigned char)(index & 0xFFU);
    }
    efi_fat32_esp_io_stats_reset();
    CHECK(efi_fat32_esp_io_stats_read(&stats) && stats.transfers == 0U &&
              stats.lookups == 0U && stats.sector_reads == 0U &&
              stats.sector_writes == 0U && stats.fat_entries_read == 0U,
          "a reset probe counts nothing");
    CHECK(efi_fat32_esp_write_file_range(&esp, log_path, 8192U, record,
                                         sizeof(record)) ==
              EFI_FAT32_ESP_OK,
          "four sectors inside the extent are written in one range");
    CHECK(efi_fat32_esp_io_stats_read(&stats),
          "the probe reads back after the range");
    CHECK(stats.transfers == 1U && stats.lookups == 1U,
          "one range write is one transfer and one path lookup");
    CHECK(stats.sector_writes == 4U,
          "four whole sectors are written, one device write each");
    CHECK(stats.sector_reads >= stats.fat_entries_read,
          "the walk that reaches the offset is read sector by sector");
    CHECK(stats.fat_entries_read >= 8U,
          "reaching the offset walked the file's FAT chain");
    CHECK(stats.lookup_us == 0U && stats.traversal_us == 0U,
          "a proof with no cycle counter reads zero, never a made-up span");
    efi_fat32_esp_io_stats_reset();
    {
        static unsigned char read_back[sizeof(record)];
        size_t length = 0U;

        CHECK(efi_fat32_esp_read_file_range(&esp, log_path, 8192U,
                                            read_back, sizeof(read_back),
                                            &length, NULL) ==
                  EFI_FAT32_ESP_OK &&
                  length == sizeof(read_back) &&
                  memcmp(read_back, record, sizeof(record)) == 0,
              "the range reads back byte for byte through the same walk");
    }
    CHECK(efi_fat32_esp_io_stats_read(&stats) && stats.transfers == 1U &&
              stats.lookups == 1U && stats.sector_reads >= 4U &&
              stats.sector_writes == 0U && stats.fat_entries_read >= 8U,
          "a range read is counted the same way");
    /*
     * ITEM-487: the run the flush now hands the writer - eight consecutive
     * sectors, which with one sector per cluster is eight clusters - lands byte
     * for byte across every cluster boundary, and the extent's last sector is
     * inside a range too.
     */
    for (index = 0U; index < sizeof(record); ++index) {
        record[index] = (unsigned char)(0xA5U ^ (index & 0xFFU));
    }
    CHECK(efi_fat32_esp_write_file_range(&esp, log_path, 4096U, record,
                                         sizeof(record)) ==
              EFI_FAT32_ESP_OK,
          "a run of eight consecutive sectors is written as one range");
    {
        static unsigned char read_back[sizeof(record)];
        size_t length = 0U;

        CHECK(efi_fat32_esp_read_file_range(&esp, log_path, 4096U,
                                            read_back, sizeof(read_back),
                                            &length, NULL) ==
                  EFI_FAT32_ESP_OK &&
                  length == sizeof(read_back) &&
                  memcmp(read_back, record, sizeof(record)) == 0,
              "every cluster boundary inside the run carries the bytes it "
              "was handed");
    }
    CHECK(efi_fat32_esp_write_file_range(
              &esp, log_path, log_size - EFI_FAT32_ESP_SECTOR_SIZE,
              record, EFI_FAT32_ESP_SECTOR_SIZE) == EFI_FAT32_ESP_OK,
          "the extent's last sector is written without growing the file");
    {
        static unsigned char read_back[EFI_FAT32_ESP_SECTOR_SIZE];
        size_t length = 0U;
        uint32_t probed_size = 0U;

        CHECK(efi_fat32_esp_read_file_range(
                  &esp, log_path, log_size - EFI_FAT32_ESP_SECTOR_SIZE,
                  read_back, sizeof(read_back), &length, &probed_size) ==
                  EFI_FAT32_ESP_OK &&
                  length == sizeof(read_back) &&
                  probed_size == log_size &&
                  memcmp(read_back, record, sizeof(read_back)) == 0,
              "the last sector reads back and the extent is still its "
              "declared size");
    }
    CHECK(efi_fat32_esp_io_stats_read(&stats) && stats.transfers == 5U &&
              stats.lookups == 5U,
          "every range after the reset is counted as one transfer and one "
          "lookup");
}

/*
 * ITEM-413: a directory this writer creates reads back empty.
 *
 * The volume a re-install writes into is formatted - its boot sector, its FATs
 * and its root directory are reset - but its *data area* still holds the
 * previous install's bytes, including the directory clusters of the tree that
 * install staged.  A directory the writer creates must not inherit a single
 * one of those bytes: an inherited entry is live to every reader, and this
 * writer's own lookup adopts it, takes the cluster it names, and never marks
 * that cluster allocated in the freshly formatted FAT.  The file allocator
 * then hands the cluster out for file data, so the staged set no longer
 * matches the sealed digest and the install refuses with
 * `VERIFICATION_FAILED` - which is what a second install onto the acceptance
 * disk did: no reboot, no boot log, and the machine stayed on the stick.
 *
 * The fixture poisons the free data area with a byte no directory entry can
 * start with, creates a tree that does not exist yet, and requires the two
 * directories the writer creates to carry none of it.
 */
static void test_created_directory_starts_empty(void)
{
    struct block_device device = make_device();
    struct efi_boot_environment environment;
    struct efi_fat32_esp esp;
    const unsigned char poison = 0xCCU;
    const unsigned char *root;
    uint32_t hfoc_cluster = 0U;
    uint32_t state_cluster = 0U;
    uint32_t first_cluster = 0U;
    size_t poison_offset =
        ((size_t)FIXTURE_DATA_START + 3U) * FIXTURE_BLOCK_SIZE;
    bool reused = true;

    fixture_init();
    /* Clusters 2, 3, 4 and 6 belong to the fixture's own tree. */
    memset(fixture_storage + poison_offset, poison,
           sizeof(fixture_storage) - poison_offset);
    memcpy(fixture_storage + 100U * FIXTURE_BLOCK_SIZE,
           "keep me esp!\n", 13U);

    CHECK(fixture_environment(&environment, &device) == true,
          "the poisoned fixture binds an ESP environment");
    CHECK(efi_fat32_esp_bind(
              &environment, 0U, FIXTURE_BLOCK_COUNT, &esp) ==
              EFI_FAT32_ESP_OK,
          "the poisoned fixture binds the volume");
    CHECK(efi_fat32_esp_create_fixed_file(
              &esp, "\\HFOC\\STATE\\BOOTLOG.BIN", 4096U,
              &first_cluster, &reused) == EFI_FAT32_ESP_OK,
          "the extent is created under a tree that does not exist yet");
    CHECK(!reused && first_cluster >= 2U,
          "the created extent names its first cluster");

    root = fixture_storage +
        (size_t)FIXTURE_DATA_START * FIXTURE_BLOCK_SIZE;
    if (!fixture_directory_entry_cluster(
            root, name_hfoc, &hfoc_cluster)) {
        CHECK(false, "the created tree names \\HFOC in the root directory");
        return;
    }
    CHECK(!fixture_cluster_carries(hfoc_cluster, poison),
          "the created \\HFOC directory carries none of the volume's "
          "previous bytes");
    if (!fixture_directory_entry_cluster(
            fixture_storage +
                ((size_t)FIXTURE_DATA_START +
                 (size_t)(hfoc_cluster - 2U)) * FIXTURE_BLOCK_SIZE,
            name_state, &state_cluster)) {
        CHECK(false, "the created tree names \\HFOC\\STATE");
        return;
    }
    CHECK(!fixture_cluster_carries(state_cluster, poison),
          "the created \\HFOC\\STATE directory carries none of the "
          "volume's previous bytes");
}

static void test_status_strings(void)
{
    CHECK(strcmp(efi_fat32_esp_status_string(
                     EFI_FAT32_ESP_OK),
                 "EFI_FAT32_ESP_OK") == 0,
          "FAT32 ESP OK status string is exact");
    CHECK(strcmp(efi_fat32_esp_status_string(
                     EFI_FAT32_ESP_PATH_DENIED),
                 "EFI_FAT32_ESP_PATH_DENIED") == 0,
          "FAT32 ESP path-denied status string is exact");
    CHECK(efi_fat32_esp_status_string(
              (enum efi_fat32_esp_status)99U) == NULL,
          "undefined FAT32 ESP status string is rejected");
}

/*
 * ITEM-398: the move that takes over a shared ESP's occupied paths has to take
 * the occupant's long name with it.
 *
 * A long-name entry is bound to the short entry that follows it, so an entry
 * that moves without it leaves its name attached to whatever entry takes its
 * place - here the loader installed at the documented path, which a firmware
 * looking for `limine.conf` would then read as its configuration.
 */
static void test_move_takes_the_long_name_with_it(void)
{
    struct block_device device = make_device();
    struct efi_boot_environment environment;
    struct efi_fat32_esp esp;
    bool present = false;
    uint32_t size = 0U;
    static const char occupant[] = "\\EFI\\BOOT\\limine.conf";
    static const char loaded[] = "\\EFI\\BOOT\\BOOTX64.EFI";
    static const char preserved[] = "\\EFI\\HFOC\\LIMINE.PRE";

    fixture_init();
    /* \EFI\BOOT is cluster 5, its directory sector is LBA 99. */
    fixture_fat_set(5U, 0x0FFFFFFFU);
    fixture_dir_entry(97U, 3U, name_boot, 0x10U, 5U, 0U);
    fixture_dir_entry(
        99U, 0U, (const unsigned char *)".          ",
        0x10U, 5U, 0U);
    fixture_dir_entry(
        99U, 1U, (const unsigned char *)"..         ",
        0x10U, 3U, 0U);
    fixture_lfn_entry(99U, 2U, "limine.conf", name_limine);
    fixture_dir_entry(99U, 3U, name_limine, 0x20U, 6U, 13U);
    fixture_dir_entry(99U, 4U, name_bootx64, 0x20U, 4U, 11U);

    CHECK(fixture_environment(&environment, &device),
          "long-name fixture environment initializes");
    CHECK(efi_fat32_esp_bind(
              &environment, 0U, FIXTURE_BLOCK_COUNT, &esp) ==
              EFI_FAT32_ESP_OK,
          "long-name fixture ESP binds");
    present = false;
    size = 0U;
    CHECK(efi_fat32_esp_probe_file(
              &esp, occupant, &present, &size) == EFI_FAT32_ESP_OK &&
              present && size == 13U,
          "the occupant is found by its long name");
    CHECK(efi_fat32_esp_rename_file(
              &esp, occupant, preserved) == EFI_FAT32_ESP_OK,
          "the occupant moves to the documented .PRE name");
    present = true;
    size = 0U;
    CHECK(efi_fat32_esp_probe_file(
              &esp, occupant, &present, &size) == EFI_FAT32_ESP_OK &&
              !present,
          "the long name moved with the entry it named");
    present = false;
    size = 0U;
    CHECK(efi_fat32_esp_probe_file(
              &esp, loaded, &present, &size) == EFI_FAT32_ESP_OK &&
              present && size == 11U,
          "the entry that follows keeps its own name");
    present = false;
    size = 0U;
    CHECK(efi_fat32_esp_probe_file(
              &esp, preserved, &present, &size) == EFI_FAT32_ESP_OK &&
              present && size == 13U,
          "the moved entry is reachable under the .PRE name");
    CHECK(fixture_storage[99U * FIXTURE_BLOCK_SIZE + 2U * 32U] == 0xE5U,
          "the long-name entry was cleared with its entry");
    CHECK(fixture_storage[99U * FIXTURE_BLOCK_SIZE + 3U * 32U] == 0xE5U,
          "the moved entry's short name was cleared");
    CHECK(fixture_storage[99U * FIXTURE_BLOCK_SIZE + 4U * 32U + 0U] == 'B',
          "the following entry was not touched");
    CHECK(fixture_preserved_bytes(),
          "the move left every other byte of the volume alone");
}

int main(void)
{
    fixture_init();
    test_bind_and_path_contracts();
    test_volume_must_fit_its_partition();
    test_install_update_verify_and_preservation();
    test_move_takes_the_long_name_with_it();
    test_fixed_extent_file();
    test_range_probe_counts_the_work();
    test_created_directory_starts_empty();
    test_failure_isolation();
    test_status_strings();

    if (failures != 0) {
        fprintf(stderr,
                "EFI FAT32 ESP host tests: %d FAILURES\n",
                failures);
        return 1;
    }
    puts("EFI FAT32 ESP host tests: ALL OK");
    return 0;
}

#endif
