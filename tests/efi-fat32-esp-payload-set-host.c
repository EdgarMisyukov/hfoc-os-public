#include "efi_boot_environment.h"
#include "efi_fat32_esp.h"
#include "sha256.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifdef HFOC_EFI_FAT32_ESP_PAYLOAD_SET_HOST_TEST

#define FIXTURE_BLOCK_SIZE 512U
#define FIXTURE_BLOCK_COUNT 8192U
#define FIXTURE_RESERVED_SECTORS 32U
#define FIXTURE_FAT_COUNT 2U
#define FIXTURE_SECTORS_PER_FAT 64U
#define FIXTURE_SECTORS_PER_CLUSTER 1U
#define FIXTURE_ROOT_CLUSTER 2U
#define FIXTURE_DATA_START \
    (FIXTURE_RESERVED_SECTORS + \
     FIXTURE_FAT_COUNT * FIXTURE_SECTORS_PER_FAT)
#define FIXTURE_SMALL_BLOCK_COUNT 1024U
#define FIXTURE_SMALL_SECTORS_PER_FAT 16U
#define FIXTURE_KERNEL_BYTES (512U * 1024U)
#define FIXTURE_LOADER_BYTES 4096U
#define FIXTURE_PAYLOAD_BYTES 1024U
#define FIXTURE_CONFIG_BYTES 128U
#define FIXTURE_MANIFEST_BYTES 288U
#define FIXTURE_ENTRY_CAPACITY 64U

/*
 * ITEM-397: the ESP payload set.  The declared product ESP (one contracted
 * image slot, 32768 sectors) is sized against the *installed kernel* bound,
 * not against the fixture below; the fixture stays small so the proof runs in
 * bounded host memory, and the requirement arithmetic is checked separately
 * at the real bound.
 */
#define CONTRACTED_ESP_SECTORS UINT64_C(32768)
#define LEGACY_ESP_SECTORS UINT64_C(4096)
#define KERNEL_BOUND_BYTES (4U * 1024U * 1024U + 262144U)

struct fixture_disk {
    unsigned char *storage;
    uint64_t block_count;
    uint64_t write_calls;
    uint64_t fail_write_call;
    bool fail_read;
    uint32_t sectors_per_fat;
};

static unsigned char fixture_storage[
    FIXTURE_BLOCK_COUNT * FIXTURE_BLOCK_SIZE];
static unsigned char fixture_small_storage[
    FIXTURE_SMALL_BLOCK_COUNT * FIXTURE_BLOCK_SIZE];
static unsigned char fixture_kernel[FIXTURE_KERNEL_BYTES];
static unsigned char fixture_loader[FIXTURE_LOADER_BYTES];
static unsigned char fixture_payload[FIXTURE_PAYLOAD_BYTES];
static unsigned char fixture_config[FIXTURE_CONFIG_BYTES];
static unsigned char fixture_manifest[FIXTURE_MANIFEST_BYTES];
static int failures;

#define CHECK(cond, msg) do { \
    if (!(cond)) { \
        fprintf(stderr, "FAIL: %s\n", msg); \
        ++failures; \
    } else { \
        printf("PASS: %s\n", msg); \
    } \
} while (0)

static enum efi_boot_variable_io_status fixture_variable_read(
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

static enum efi_boot_variable_io_status fixture_variable_write(
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

static enum block_device_status fixture_read(
    struct block_device *device,
    uint64_t offset,
    void *buffer,
    size_t length)
{
    struct fixture_disk *disk = (struct fixture_disk *)device->context;

    if (disk == NULL || disk->fail_read ||
        offset > disk->block_count * FIXTURE_BLOCK_SIZE ||
        length > disk->block_count * FIXTURE_BLOCK_SIZE - offset) {
        return BLOCK_DEVICE_IO_ERROR;
    }
    memcpy(buffer, disk->storage + offset, length);
    return BLOCK_DEVICE_OK;
}

static enum block_device_status fixture_write(
    struct block_device *device,
    uint64_t offset,
    const void *buffer,
    size_t length)
{
    struct fixture_disk *disk = (struct fixture_disk *)device->context;

    if (disk == NULL ||
        offset > disk->block_count * FIXTURE_BLOCK_SIZE ||
        length > disk->block_count * FIXTURE_BLOCK_SIZE - offset) {
        return BLOCK_DEVICE_IO_ERROR;
    }
    ++disk->write_calls;
    if (disk->fail_write_call != 0U &&
        disk->write_calls == disk->fail_write_call) {
        disk->fail_write_call = 0U;
        return BLOCK_DEVICE_IO_ERROR;
    }
    memcpy(disk->storage + offset, buffer, length);
    return BLOCK_DEVICE_OK;
}

static enum block_device_status fixture_flush(
    struct block_device *device)
{
    (void)device;
    return BLOCK_DEVICE_OK;
}

static struct fixture_disk fixture_main_disk;
static struct fixture_disk fixture_small_disk;

static void fixture_format(
    unsigned char *storage,
    uint64_t block_count,
    uint32_t sectors_per_fat)
{
    unsigned char boot_sector[FIXTURE_BLOCK_SIZE];
    uint64_t fat_lba;
    uint64_t index;

    memset(storage, 0, (size_t)block_count * FIXTURE_BLOCK_SIZE);
    memset(boot_sector, 0, sizeof(boot_sector));
    fixture_store_le16(boot_sector + 11U, FIXTURE_BLOCK_SIZE);
    boot_sector[13U] = FIXTURE_SECTORS_PER_CLUSTER;
    fixture_store_le16(boot_sector + 14U, FIXTURE_RESERVED_SECTORS);
    boot_sector[16U] = FIXTURE_FAT_COUNT;
    /* ITEM-405: the volume declares its own extent. */
    fixture_store_le16(boot_sector + 19U, 0U);
    fixture_store_le32(boot_sector + 32U, (uint32_t)block_count);
    fixture_store_le32(boot_sector + 36U, sectors_per_fat);
    fixture_store_le32(boot_sector + 44U, FIXTURE_ROOT_CLUSTER);
    boot_sector[510U] = 0x55U;
    boot_sector[511U] = 0xAAU;
    memcpy(storage, boot_sector, sizeof(boot_sector));

    memset(boot_sector, 0, sizeof(boot_sector));
    for (index = 0U; index < FIXTURE_FAT_COUNT; ++index) {
        fat_lba = FIXTURE_RESERVED_SECTORS +
                  index * sectors_per_fat;
        fixture_store_le32(boot_sector, 0x0FFFFFF8U);
        fixture_store_le32(boot_sector + 4U, 0x0FFFFFFFU);
        fixture_store_le32(
            boot_sector + FIXTURE_ROOT_CLUSTER *
                EFI_FAT32_ESP_FAT_ENTRY_BYTES,
            0x0FFFFFFFU);
        memcpy(storage + fat_lba * FIXTURE_BLOCK_SIZE,
               boot_sector, sizeof(boot_sector));
    }
}

static struct block_device fixture_device(
    struct fixture_disk *disk,
    unsigned char *storage,
    uint64_t block_count)
{
    struct block_device device;

    memset(&device, 0, sizeof(device));
    memset(disk, 0, sizeof(*disk));
    disk->storage = storage;
    disk->block_count = block_count;
    device.block_size = FIXTURE_BLOCK_SIZE;
    device.block_count = block_count;
    device.context = disk;
    device.read_fn = fixture_read;
    device.write_fn = fixture_write;
    device.flush_fn = fixture_flush;
    return device;
}

static void fixture_reset(void)
{
    static const unsigned char payload_path[] =
        "\\EFI\\HFOC\\BOOTX64.EFI";
    static const unsigned char loader_path[] =
        "\\EFI\\BOOT\\BOOTX64.EFI";
    static const unsigned char kernel_path[] =
        "\\boot\\hfoc-kernel.elf";
    static const unsigned char config_path[] =
        "\\boot\\limine\\limine.conf";
    static const unsigned char manifest_path[] =
        "\\boot\\hfoc-boot.man";
    size_t index;

    (void)payload_path;
    (void)loader_path;
    (void)kernel_path;
    (void)config_path;
    (void)manifest_path;
    fixture_format(fixture_storage, FIXTURE_BLOCK_COUNT,
                   FIXTURE_SECTORS_PER_FAT);
    fixture_format(fixture_small_storage, FIXTURE_SMALL_BLOCK_COUNT,
                   FIXTURE_SMALL_SECTORS_PER_FAT);
    for (index = 0U; index < sizeof(fixture_kernel); ++index) {
        fixture_kernel[index] = (unsigned char)(
            (index * 31U + (index >> 8U) * 7U) & 0xFFU);
    }
    for (index = 0U; index < sizeof(fixture_loader); ++index) {
        fixture_loader[index] = (unsigned char)(0xA5U ^ (index & 0xFFU));
    }
    for (index = 0U; index < sizeof(fixture_payload); ++index) {
        fixture_payload[index] = (unsigned char)(index & 0xFFU);
    }
    memset(fixture_config, 0, sizeof(fixture_config));
    memcpy(fixture_config,
           "timeout: 3\n\n/HFOC OS 0.2\n    protocol: limine\n"
           "    path: boot():/boot/hfoc-kernel.elf\n",
           76U);
    memset(fixture_manifest, 0, sizeof(fixture_manifest));
    memcpy(fixture_manifest, "HFOCBOOT", 8U);
}

static struct efi_fat32_esp_payload fixture_set[5U];

static void fixture_build_set(size_t kernel_bytes)
{
    memset(fixture_set, 0, sizeof(fixture_set));
    fixture_set[0U].path = "\\EFI\\HFOC\\BOOTX64.EFI";
    fixture_set[0U].data = fixture_payload;
    fixture_set[0U].size = sizeof(fixture_payload);
    fixture_set[1U].path = "\\boot\\limine\\limine.conf";
    fixture_set[1U].data = fixture_config;
    fixture_set[1U].size = 76U;
    fixture_set[2U].path = "\\boot\\hfoc-kernel.elf";
    fixture_set[2U].data = fixture_kernel;
    fixture_set[2U].size = kernel_bytes;
    fixture_set[3U].path = "\\EFI\\BOOT\\BOOTX64.EFI";
    fixture_set[3U].data = fixture_loader;
    fixture_set[3U].size = sizeof(fixture_loader);
    fixture_set[4U].path = "\\boot\\hfoc-boot.man";
    fixture_set[4U].data = fixture_manifest;
    fixture_set[4U].size = sizeof(fixture_manifest);
}

static bool fixture_read_directory_entry(
    const unsigned char *storage,
    uint64_t cluster,
    size_t entry_index,
    unsigned char out[32U])
{
    const uint64_t lba = FIXTURE_DATA_START + (cluster - 2U);

    if (entry_index >= 16U) {
        return false;
    }
    memcpy(out, storage + lba * FIXTURE_BLOCK_SIZE +
                    entry_index * 32U, 32U);
    return true;
}

/*
 * Walk one directory cluster for an 8.3 short name, the way a loader walks it:
 * long-name entries are skipped, a deleted entry is skipped, and a zero entry
 * ends the directory.  The matching entry's index and first cluster are
 * returned, so the raw long-name chain in front of it can be reconstructed.
 */
static bool fixture_find_entry(
    const unsigned char *storage,
    uint64_t cluster,
    const char *short_name,
    size_t *index_out,
    uint32_t *first_cluster_out)
{
    size_t index;
    unsigned char entry[32U];

    for (index = 0U; index < 16U; ++index) {
        if (!fixture_read_directory_entry(
                storage, cluster, index, entry)) {
            return false;
        }
        if (entry[0U] == 0x00U) {
            return false;
        }
        if (entry[0U] == 0xE5U || entry[11U] == 0x0FU ||
            (entry[11U] & 0x08U) != 0U) {
            continue;
        }
        if (memcmp(entry, short_name, 11U) != 0) {
            continue;
        }
        if (index_out != NULL) {
            *index_out = index;
        }
        if (first_cluster_out != NULL) {
            *first_cluster_out =
                (uint32_t)entry[20U] << 24U |
                (uint32_t)entry[21U] << 16U |
                (uint32_t)entry[26U] |
                (uint32_t)entry[27U] << 8U;
        }
        return true;
    }
    return false;
}

/*
 * Rebuild the long name a chain of LFN entries in front of `short_entry`
 * carries, the way a FAT driver does, so the proof checks the bytes a
 * firmware or Limine would actually read.
 */
static bool fixture_lfn_reconstruct(
    const unsigned char *storage,
    uint64_t cluster,
    size_t short_index,
    char *name,
    size_t capacity,
    size_t *length)
{
    unsigned char entries[EFI_FAT32_ESP_LFN_ENTRIES_MAX][32U];
    size_t count = 0U;
    size_t index;
    unsigned char checksum;
    unsigned char short_name[11U];

    if (short_index == 0U) {
        return false;
    }
    if (!fixture_read_directory_entry(
            storage, cluster, short_index, entries[0U])) {
        return false;
    }
    memcpy(short_name, entries[0U], 11U);
    checksum = 0U;
    for (index = 0U; index < 11U; ++index) {
        checksum = (unsigned char)(
            ((checksum & 1U) << 7U) + (checksum >> 1U) +
            short_name[index]);
    }
    while (count < EFI_FAT32_ESP_LFN_ENTRIES_MAX &&
           count < short_index) {
        unsigned char entry[32U];
        const size_t position = short_index - 1U - count;

        if (!fixture_read_directory_entry(
                storage, cluster, position, entry) ||
            entry[11U] != 0x0FU ||
            entry[13U] != checksum) {
            break;
        }
        memcpy(entries[count], entry, 32U);
        ++count;
    }
    if (count == 0U) {
        return false;
    }
    memset(name, 0, capacity);
    *length = 0U;
    for (index = 0U; index < count; ++index) {
        const size_t part =
            (size_t)(entries[index][0U] & 0x1FU);
        size_t character;

        for (character = 0U; character < 13U; ++character) {
            const size_t slot = character < 5U
                ? 1U + character * 2U
                : (character < 11U
                       ? 14U + (character - 5U) * 2U
                       : 28U + (character - 11U) * 2U);
            const unsigned int value =
                (unsigned int)entries[index][slot] |
                ((unsigned int)entries[index][slot + 1U] << 8U);
            size_t name_index;

            if (value == 0x0000U || value == 0xFFFFU) {
                continue;
            }
            name_index = (part - 1U) * 13U + character;
            if (name_index + 1U >= capacity ||
                value > 0x7FU) {
                return false;
            }
            name[name_index] = (char)value;
            if (name_index + 1U > *length) {
                *length = name_index + 1U;
            }
        }
    }
    return *length != 0U;
}

static void test_requirement_bounds(void)
{
    struct efi_fat32_esp_requirement requirement;
    static struct efi_fat32_esp_payload set[5U];

    fixture_build_set(KERNEL_BOUND_BYTES);
    set[0U] = fixture_set[0U];
    set[1U] = fixture_set[1U];
    set[2U] = fixture_set[2U];
    set[3U] = fixture_set[3U];
    set[4U] = fixture_set[4U];
    set[2U].size = KERNEL_BOUND_BYTES;
    CHECK(efi_fat32_esp_payload_requirement(
              set, 5U, 1U, &requirement) == EFI_FAT32_ESP_OK,
          "payload set requirement is computed");
    CHECK(requirement.payload_bytes ==
              KERNEL_BOUND_BYTES + FIXTURE_PAYLOAD_BYTES + 76U +
              FIXTURE_LOADER_BYTES + FIXTURE_MANIFEST_BYTES,
          "requirement counts the whole set");
    CHECK(requirement.required_sectors <= CONTRACTED_ESP_SECTORS,
          "the contracted ESP holds the kernel at its declared bound");
    CHECK(requirement.required_sectors > LEGACY_ESP_SECTORS,
          "the legacy 2 MiB ESP cannot hold the installed kernel");

    CHECK(efi_fat32_esp_payload_requirement(
              set, 5U, 3U, &requirement) != EFI_FAT32_ESP_OK,
          "a non power-of-two cluster size is refused");
    CHECK(efi_fat32_esp_payload_requirement(
              set, 0U, 1U, &requirement) == EFI_FAT32_ESP_NULL,
          "an empty set is refused");
    CHECK(efi_fat32_esp_validate_payload_path(
              "\\boot\\limine\\limine.conf") ==
              EFI_FAT32_ESP_OK &&
          efi_fat32_esp_validate_payload_path(
              "\\EFI\\HFOC\\BOOTX64.EFI") == EFI_FAT32_ESP_OK,
          "canonical installed paths are accepted");
    CHECK(efi_fat32_esp_validate_payload_path(
              "relative\\path") == EFI_FAT32_ESP_PATH_DENIED &&
          efi_fat32_esp_validate_payload_path(
              "\\boot\\") == EFI_FAT32_ESP_PATH_DENIED &&
          efi_fat32_esp_validate_payload_path(
              "\\boot\\bad*name.elf") == EFI_FAT32_ESP_PATH_DENIED &&
          efi_fat32_esp_validate_payload_path(NULL) ==
              EFI_FAT32_ESP_NULL,
          "a path outside the canonical grammar is denied");
}

static void test_refusal_before_first_write(void)
{
    struct efi_boot_environment environment;
    struct efi_fat32_esp esp;
    struct block_device device;
    void *context = NULL;

    fixture_build_set(sizeof(fixture_kernel));
    device = fixture_device(&fixture_small_disk,
                            fixture_small_storage,
                            FIXTURE_SMALL_BLOCK_COUNT);
    memset(&environment, 0, sizeof(environment));
    CHECK(efi_boot_environment_init(
              &environment, EFI_BOOT_FIRMWARE_UEFI, &device,
              context, fixture_variable_read,
              fixture_variable_write) == EFI_BOOT_ENVIRONMENT_OK,
          "small ESP environment initializes");
    CHECK(efi_fat32_esp_bind(
              &environment, 0U, FIXTURE_SMALL_BLOCK_COUNT,
              &esp) == EFI_FAT32_ESP_OK,
          "small ESP binds");
    fixture_small_disk.write_calls = 0U;
    CHECK(efi_fat32_esp_install_payload_set(
              &esp, fixture_set, 5U) == EFI_FAT32_ESP_CAPACITY,
          "an ESP too small for the set is refused");
    CHECK(fixture_small_disk.write_calls == 0U,
          "the refusal happens before the first write");
    CHECK(efi_fat32_esp_install_payload_set(
              &esp, fixture_set, 0U) == EFI_FAT32_ESP_NULL,
          "an empty set performs no write");
    CHECK(fixture_small_disk.write_calls == 0U,
          "an invalid call performs no write");
}

static void test_conflict_and_bounds(void)
{
    struct efi_boot_environment environment;
    struct efi_fat32_esp esp;
    struct block_device device;
    void *context = NULL;
    static struct efi_fat32_esp_payload set[2U];

    fixture_build_set(sizeof(fixture_kernel));
    device = fixture_device(&fixture_main_disk,
                            fixture_storage,
                            FIXTURE_BLOCK_COUNT);
    memset(&environment, 0, sizeof(environment));
    CHECK(efi_boot_environment_init(
              &environment, EFI_BOOT_FIRMWARE_UEFI, &device,
              context, fixture_variable_read,
              fixture_variable_write) == EFI_BOOT_ENVIRONMENT_OK &&
          efi_fat32_esp_bind(
              &environment, 0U, FIXTURE_BLOCK_COUNT, &esp) ==
              EFI_FAT32_ESP_OK,
          "conflict fixture binds");
    memset(set, 0, sizeof(set));
    set[0U].path = "\\boot\\hfoc-kernel.elf";
    set[0U].data = fixture_kernel;
    set[0U].size = 16U;
    set[1U].path = "\\boot\\hfoc-kernel.elf";
    set[1U].data = fixture_kernel;
    set[1U].size = 16U;
    fixture_main_disk.write_calls = 0U;
    CHECK(efi_fat32_esp_install_payload_set(
              &esp, set, 2U) == EFI_FAT32_ESP_PATH_CONFLICT,
          "two entries that resolve to one name are refused");
    CHECK(fixture_main_disk.write_calls == 0U,
          "a set conflict performs no write");

    set[1U].path = "\\boot\\hfoc-kernel.elfb";
    set[1U].size = EFI_FAT32_ESP_FILE_MAX + 1U;
    CHECK(efi_fat32_esp_install_payload_set(
              &esp, set, 2U) == EFI_FAT32_ESP_SET_TOO_LARGE,
          "a file above the per-file bound is refused");
    set[1U].size = 16U;
    set[0U].size = EFI_FAT32_ESP_PAYLOAD_SET_BYTES_MAX;
    set[1U].size = EFI_FAT32_ESP_PAYLOAD_SET_BYTES_MAX;
    CHECK(efi_fat32_esp_install_payload_set(
              &esp, set, 2U) == EFI_FAT32_ESP_SET_TOO_LARGE,
          "a set above the declared total bound is refused");
    CHECK(fixture_main_disk.write_calls == 0U,
          "bound refusals perform no write");
}

static void test_install_verify_and_long_names(void)
{
    struct efi_boot_environment environment;
    struct efi_fat32_esp esp;
    struct block_device device;
    void *context = NULL;
    unsigned char digest[SHA256_DIGEST_SIZE];
    unsigned char expected[SHA256_DIGEST_SIZE];
    unsigned char kernel_digest[SHA256_DIGEST_SIZE];
    unsigned char readback[FIXTURE_PAYLOAD_BYTES];
    struct sha256 hasher;
    size_t readback_size = 0U;
    uint64_t sealed_length = 0U;
    char name[EFI_FAT32_ESP_ELEMENT_MAX + 1U];
    size_t name_length = 0U;

    fixture_build_set(sizeof(fixture_kernel));
    device = fixture_device(&fixture_main_disk,
                            fixture_storage,
                            FIXTURE_BLOCK_COUNT);
    memset(&environment, 0, sizeof(environment));
    CHECK(efi_boot_environment_init(
              &environment, EFI_BOOT_FIRMWARE_UEFI, &device,
              context, fixture_variable_read,
              fixture_variable_write) == EFI_BOOT_ENVIRONMENT_OK &&
          efi_fat32_esp_bind(
              &environment, 0U, FIXTURE_BLOCK_COUNT, &esp) ==
              EFI_FAT32_ESP_OK,
          "payload set fixture binds");
    CHECK(efi_fat32_esp_install_payload_set(
              &esp, fixture_set, 5U) == EFI_FAT32_ESP_OK,
          "the payload set is installed");
    CHECK(efi_fat32_esp_verify_payload_set(
              &esp, fixture_set, 5U, 3U, digest,
              &sealed_length) == EFI_FAT32_ESP_OK,
          "the staged set verifies byte for byte");
    CHECK(sealed_length ==
              FIXTURE_PAYLOAD_BYTES + 76U +
              sizeof(fixture_kernel),
          "the sealed prefix is the declared source length");
    CHECK(sha256_init(&hasher) == SHA256_OK &&
          sha256_update(&hasher, fixture_payload,
                        sizeof(fixture_payload)) == SHA256_OK &&
          sha256_update(&hasher, fixture_config, 76U) == SHA256_OK &&
          sha256_update(&hasher, fixture_kernel,
                        sizeof(fixture_kernel)) == SHA256_OK &&
          sha256_final(&hasher, expected) == SHA256_OK,
          "the sealed media digest is reproduced");
    CHECK(memcmp(digest, expected, SHA256_DIGEST_SIZE) == 0,
          "the staged set matches the sealed media digest");

    CHECK(efi_fat32_esp_read_file(
              &esp, "\\EFI\\HFOC\\BOOTX64.EFI", readback,
              sizeof(readback), &readback_size) ==
              EFI_FAT32_ESP_OK &&
          readback_size == sizeof(fixture_payload) &&
          memcmp(readback, fixture_payload,
                 sizeof(fixture_payload)) == 0,
          "the canonical HFOC payload reads back");
    readback_size = 0U;
    CHECK(efi_fat32_esp_digest_file(
              &esp, "\\boot\\hfoc-kernel.elf", kernel_digest,
              &readback_size) == EFI_FAT32_ESP_OK &&
          readback_size == sizeof(fixture_kernel) &&
          sha256_compute(fixture_kernel, sizeof(fixture_kernel),
                         expected) == SHA256_OK &&
          memcmp(kernel_digest, expected, SHA256_DIGEST_SIZE) == 0,
          "the staged kernel digest is the declared kernel digest");
    CHECK(efi_fat32_esp_read_file(
              &esp, "\\boot\\hfoc-kernel.elfb", readback,
              sizeof(readback), &readback_size) ==
              EFI_FAT32_ESP_NOT_FOUND,
          "a name outside the set is not found");

    /*
     * The installed tree must be reachable the way a loader reaches it: the
     * root directory holds the `EFI` and `boot` entries, and the kernel sits
     * behind a long-name chain that reconstructs `hfoc-kernel.elf` from the
     * raw directory bytes.
     */
    {
        uint32_t boot_cluster = 0U;
        uint32_t limine_cluster = 0U;
        uint32_t hfoc_cluster = 0U;
        size_t kernel_index = 0U;

        CHECK(fixture_find_entry(
                  fixture_storage, FIXTURE_ROOT_CLUSTER,
                  "BOOT       ", NULL, &boot_cluster) &&
              fixture_find_entry(
                  fixture_storage, FIXTURE_ROOT_CLUSTER,
                  "EFI        ", NULL, &hfoc_cluster),
              "the installed root directory holds EFI and boot");
        CHECK(fixture_find_entry(
                  fixture_storage, boot_cluster, "LIMINE     ",
                  NULL, &limine_cluster),
              "the boot directory holds the limine directory");
        CHECK(fixture_find_entry(
                  fixture_storage, limine_cluster, "LIMINE~1CON",
                  &kernel_index, NULL) &&
              fixture_lfn_reconstruct(
                  fixture_storage, limine_cluster, kernel_index,
                  name, sizeof(name), &name_length) &&
              name_length == 11U &&
              memcmp(name, "limine.conf", 11U) == 0,
              "the boot configuration long name reconstructs its name");
        CHECK(fixture_find_entry(
                  fixture_storage, boot_cluster, "HFOC-K~1ELF",
                  &kernel_index, NULL) &&
              fixture_lfn_reconstruct(
                  fixture_storage, boot_cluster, kernel_index,
                  name, sizeof(name), &name_length) &&
              name_length == 15U &&
              memcmp(name, "hfoc-kernel.elf", 15U) == 0,
              "the kernel long name reconstructs its name");
        CHECK(fixture_find_entry(
                  fixture_storage, boot_cluster, "HFOC-B~1MAN",
                  &kernel_index, NULL) &&
              fixture_lfn_reconstruct(
                  fixture_storage, boot_cluster, kernel_index,
                  name, sizeof(name), &name_length) &&
              name_length == 13U &&
              memcmp(name, "hfoc-boot.man", 13U) == 0,
              "the boot manifest long name reconstructs its name");
        {
            size_t dump_index;
            unsigned char dump_entry[32U];
            uint64_t dump_cluster;

            for (dump_cluster = 2U; dump_cluster < 8U;
                 ++dump_cluster) {
                fprintf(stderr, "-- cluster %llu\n",
                        (unsigned long long)dump_cluster);
                for (dump_index = 0U; dump_index < 16U;
                     ++dump_index) {
                    if (!fixture_read_directory_entry(
                            fixture_storage, dump_cluster,
                            dump_index, dump_entry) ||
                        dump_entry[0U] == 0x00U) {
                        break;
                    }
                    fprintf(stderr,
                            "   [%zu] attr=%02X name=%.11s first=%u\n",
                            dump_index, dump_entry[11U], dump_entry,
                            (unsigned)(
                                (unsigned)dump_entry[20U] << 24U |
                                (unsigned)dump_entry[21U] << 16U |
                                (unsigned)dump_entry[26U] |
                                (unsigned)dump_entry[27U] << 8U));
                }
            }
            fprintf(stderr, "boot_cluster=%u hfoc=%u limine=%u\n",
                    boot_cluster, hfoc_cluster, limine_cluster);
        }
        CHECK(fixture_find_entry(
                  fixture_storage, boot_cluster,
                  "BOOTX64 EFI", NULL, NULL) == false,
              "the loader is not staged outside its own directory");
    }
}

static void test_reinstall_replaces_and_failure_isolates(void)
{
    struct efi_boot_environment environment;
    struct efi_fat32_esp esp;
    struct block_device device;
    void *context = NULL;
    unsigned char readback[FIXTURE_PAYLOAD_BYTES];
    size_t readback_size = 0U;
    uint64_t writes_after_first = 0U;

    fixture_build_set(sizeof(fixture_kernel));
    device = fixture_device(&fixture_main_disk,
                            fixture_storage,
                            FIXTURE_BLOCK_COUNT);
    memset(&environment, 0, sizeof(environment));
    CHECK(efi_boot_environment_init(
              &environment, EFI_BOOT_FIRMWARE_UEFI, &device,
              context, fixture_variable_read,
              fixture_variable_write) == EFI_BOOT_ENVIRONMENT_OK &&
          efi_fat32_esp_bind(
              &environment, 0U, FIXTURE_BLOCK_COUNT, &esp) ==
              EFI_FAT32_ESP_OK &&
          efi_fat32_esp_install_payload_set(
              &esp, fixture_set, 5U) == EFI_FAT32_ESP_OK,
          "the first install succeeds");
    writes_after_first = fixture_main_disk.write_calls;
    CHECK(writes_after_first != 0U,
          "the first install wrote to the volume");

    fixture_set[0U].data = fixture_loader;
    fixture_set[0U].size = 64U;
    {
        enum efi_fat32_esp_status debug_status;
        size_t dump_index;
        uint64_t dump_cluster;

        for (dump_cluster = 2U; dump_cluster < 10U; ++dump_cluster) {
            unsigned char dump_entry[32U];

            fprintf(stderr, "-- before call2 cluster %llu\n",
                    (unsigned long long)dump_cluster);
            for (dump_index = 0U; dump_index < 16U; ++dump_index) {
                if (!fixture_read_directory_entry(
                        fixture_storage, dump_cluster, dump_index,
                        dump_entry) || dump_entry[0U] == 0x00U) {
                    break;
                }
                fprintf(stderr, "   [%zu] attr=%02X name=%.11s first=%u size=%u\n",
                        dump_index, dump_entry[11U], dump_entry,
                        (unsigned)((unsigned)dump_entry[20U] << 24U |
                                   (unsigned)dump_entry[21U] << 16U |
                                   (unsigned)dump_entry[26U] |
                                   (unsigned)dump_entry[27U] << 8U),
                        (unsigned)((unsigned)dump_entry[28U] |
                                   (unsigned)dump_entry[29U] << 8U |
                                   (unsigned)dump_entry[30U] << 16U |
                                   (unsigned)dump_entry[31U] << 24U));
            }
        }
        fprintf(stderr, "DEBUG call2 begin\n");
        fflush(stderr);
        debug_status = efi_fat32_esp_install_payload_set(
                &esp, fixture_set, 5U);
        fprintf(stderr, "DEBUG call2 end int=%d\n", (int)debug_status);
        fflush(stderr);
        CHECK(debug_status == EFI_FAT32_ESP_OK,
              "a re-install replaces the set");
    }
    CHECK(efi_fat32_esp_read_file(
              &esp, "\\EFI\\HFOC\\BOOTX64.EFI", readback,
              sizeof(readback), &readback_size) ==
              EFI_FAT32_ESP_OK &&
          readback_size == 64U &&
          memcmp(readback, fixture_loader, 64U) == 0,
          "the re-installed file reads back with its new length");
    CHECK(efi_fat32_esp_verify_payload_set(
              &esp, fixture_set, 5U, 0U, NULL, &(uint64_t){ 0U }) ==
              EFI_FAT32_ESP_OK,
          "the re-installed set verifies");

    /*
     * A failure in the middle of a set must leave the volume with the set it
     * had: the previous payload is still readable and the staged names still
     * resolve.
     */
    {
        static struct efi_fat32_esp_payload previous_set[5U];
        size_t index;

        for (index = 0U; index < 5U; ++index) {
            previous_set[index] = fixture_set[index];
        }
        fixture_set[0U].data = fixture_payload;
        fixture_set[0U].size = sizeof(fixture_payload);
        fixture_main_disk.fail_write_call =
            fixture_main_disk.write_calls + 40U;
        CHECK(efi_fat32_esp_install_payload_set(
                  &esp, fixture_set, 5U) != EFI_FAT32_ESP_OK,
              "an injected write failure fails the set install");
        CHECK(efi_fat32_esp_verify_payload_set(
                  &esp, previous_set, 5U, 0U, NULL,
                  &(uint64_t){ 0U }) == EFI_FAT32_ESP_OK,
              "the volume still holds the previous set after the failure");
    }
    CHECK(efi_fat32_esp_read_file(
              &esp, "\\EFI\\HFOC\\BOOTX64.EFI", readback,
              sizeof(readback), &readback_size) ==
              EFI_FAT32_ESP_OK &&
          readback_size == 64U,
          "the file the failed install was replacing is intact");
}

static void test_status_strings(void)
{
    CHECK(strcmp(efi_fat32_esp_status_string(
                     EFI_FAT32_ESP_PATH_CONFLICT),
                 "EFI_FAT32_ESP_PATH_CONFLICT") == 0,
          "path conflict status string is exact");
    CHECK(strcmp(efi_fat32_esp_status_string(
                     EFI_FAT32_ESP_SET_TOO_LARGE),
                 "EFI_FAT32_ESP_SET_TOO_LARGE") == 0,
          "set-too-large status string is exact");
}

int main(void)
{
    fixture_reset();
    test_requirement_bounds();
    test_refusal_before_first_write();
    test_conflict_and_bounds();
    test_install_verify_and_long_names();
    test_reinstall_replaces_and_failure_isolates();
    test_status_strings();

    if (failures != 0) {
        fprintf(stderr,
                "EFI FAT32 ESP payload set host tests: %d FAILURES\n",
                failures);
        return 1;
    }
    puts("EFI FAT32 ESP payload set host tests: ALL OK");
    return 0;
}

#endif
