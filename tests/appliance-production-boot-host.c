#include "appliance_persistent_config.h"
#include "appliance_production_boot.h"
#include "appliance_state_block_device.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifdef HFOC_APPLIANCE_PRODUCTION_BOOT_HOST_TEST

#define TEST_BLOCK_SIZE 512U
#define TEST_BLOCK_COUNT 1024U
#define TEST_CAPACITY (TEST_BLOCK_SIZE * TEST_BLOCK_COUNT)
#define HFOC_FIRST_LBA 34U
#define HFOC_BLOCK_COUNT 512U
#define STORE_BLOCKS 64U
#define TEST_BACKEND_COUNT 2U
#define HOSTNAME "hfoc-production"

static int failures;

#define CHECK(cond, msg) do { \
    if (!(cond)) { \
        fprintf(stderr, "FAIL: %s\n", msg); \
        ++failures; \
    } else { \
        printf("PASS: %s\n", msg); \
    } \
} while (0)

struct disk_fixture {
    unsigned char storage[TEST_CAPACITY];
};

struct probe_fixture {
    struct disk_fixture *disks;
    size_t count;
    bool reverse;
    enum appliance_production_boot_backend_kind kind;
};

static struct disk_fixture test_disks[TEST_BACKEND_COUNT];

static const unsigned char hfoc_guid[16U] = {
    0xE1U, 0x44U, 0x01U, 0x4CU, 0x0DU, 0x6EU, 0x7AU, 0x4BU,
    0xB4U, 0x9DU, 0x22U, 0x10U, 0x33U, 0x3DU, 0x41U, 0x31U
};

static void store_le32(unsigned char *bytes, uint32_t value)
{
    bytes[0] = (unsigned char)(value & 0xFFU);
    bytes[1] = (unsigned char)((value >> 8U) & 0xFFU);
    bytes[2] = (unsigned char)((value >> 16U) & 0xFFU);
    bytes[3] = (unsigned char)((value >> 24U) & 0xFFU);
}

static void store_le64(unsigned char *bytes, uint64_t value)
{
    size_t index;

    for (index = 0U; index < 8U; ++index) {
        bytes[index] = (unsigned char)(value >> (8U * index));
    }
}

static uint32_t crc32_update(
    uint32_t crc,
    const unsigned char *bytes,
    size_t length)
{
    size_t byte_index;

    for (byte_index = 0U; byte_index < length; ++byte_index) {
        unsigned int bit;

        crc ^= bytes[byte_index];
        for (bit = 0U; bit < 8U; ++bit) {
            crc = (crc & 1U) != 0U
                ? (crc >> 1U) ^ 0xEDB88320U
                : crc >> 1U;
        }
    }
    return crc;
}

static uint32_t crc32(const unsigned char *bytes, size_t length)
{
    return crc32_update(0xFFFFFFFFU, bytes, length) ^ 0xFFFFFFFFU;
}

static enum block_device_status disk_read(
    struct block_device *device,
    uint64_t offset,
    void *buffer,
    size_t length)
{
    struct disk_fixture *disk = (struct disk_fixture *)device->context;

    if (offset > sizeof(disk->storage) ||
        length > sizeof(disk->storage) - (size_t)offset) {
        return BLOCK_DEVICE_IO_ERROR;
    }
    memcpy(buffer, disk->storage + (size_t)offset, length);
    return BLOCK_DEVICE_OK;
}

static enum block_device_status disk_write(
    struct block_device *device,
    uint64_t offset,
    const void *buffer,
    size_t length)
{
    struct disk_fixture *disk = (struct disk_fixture *)device->context;

    if (offset > sizeof(disk->storage) ||
        length > sizeof(disk->storage) - (size_t)offset) {
        return BLOCK_DEVICE_IO_ERROR;
    }
    memcpy(disk->storage + (size_t)offset, buffer, length);
    return BLOCK_DEVICE_OK;
}

static enum block_device_status disk_flush(struct block_device *device)
{
    (void)device;
    return BLOCK_DEVICE_OK;
}

static struct block_device disk_device(struct disk_fixture *disk)
{
    struct block_device device;

    memset(&device, 0, sizeof(device));
    device.block_size = TEST_BLOCK_SIZE;
    device.block_count = TEST_BLOCK_COUNT;
    device.context = disk;
    device.read_fn = disk_read;
    device.write_fn = disk_write;
    device.flush_fn = disk_flush;
    return device;
}

static void finalize_gpt(struct disk_fixture *disk)
{
    unsigned char *header = disk->storage + TEST_BLOCK_SIZE;
    unsigned char copy[TEST_BLOCK_SIZE];
    uint32_t entries_crc = 0xFFFFFFFFU;
    size_t index;

    for (index = 0U; index < 4U * 128U; ++index) {
        entries_crc = crc32_update(
            entries_crc, disk->storage + 2U * TEST_BLOCK_SIZE + index, 1U);
    }
    store_le32(header + 88U, entries_crc ^ 0xFFFFFFFFU);
    memset(copy, 0, sizeof(copy));
    memcpy(copy, header, 92U);
    memset(copy + 16U, 0, 4U);
    store_le32(header + 16U, crc32(copy, 92U));
}

static void install_store_header(
    struct disk_fixture *disk,
    uint64_t partition_block)
{
    static const unsigned char magic[8U] = {
        'H', 'F', 'O', 'C', 'D', 'A', 'T', 'A'
    };
    unsigned char *header =
        disk->storage +
        (HFOC_FIRST_LBA + partition_block) * TEST_BLOCK_SIZE;

    memset(header, 0, STORE_BLOCKS * TEST_BLOCK_SIZE);
    memcpy(header, magic, sizeof(magic));
    header[8U] = 1U;
    store_le64(header + 16U, 1U);
    store_le64(header + 24U, 16U);
}

static void install_gpt(struct disk_fixture *disk)
{
    unsigned char *header;
    unsigned char *entry;

    memset(disk, 0, sizeof(*disk));
    header = disk->storage + TEST_BLOCK_SIZE;
    memcpy(header, "EFI PART", 8U);
    store_le32(header + 8U, UINT32_C(0x00010000));
    store_le32(header + 12U, 92U);
    store_le64(header + 24U, 1U);
    store_le64(header + 32U, TEST_BLOCK_COUNT - 1U);
    store_le64(header + 40U, HFOC_FIRST_LBA);
    store_le64(header + 48U, TEST_BLOCK_COUNT - 1U);
    store_le64(header + 72U, 2U);
    store_le32(header + 80U, 4U);
    store_le32(header + 84U, 128U);

    entry = disk->storage + 2U * TEST_BLOCK_SIZE + 128U;
    memcpy(entry, hfoc_guid, sizeof(hfoc_guid));
    store_le64(entry + 16U, 2U);
    store_le64(entry + 32U, HFOC_FIRST_LBA);
    store_le64(
        entry + 40U, HFOC_FIRST_LBA + HFOC_BLOCK_COUNT - 1U);
    finalize_gpt(disk);

    install_store_header(disk, 0U);
    install_store_header(disk, 64U);
    install_store_header(disk, 128U);
    install_store_header(disk, 256U);
}

static void install_committed_hostname(struct disk_fixture *disk)
{
    struct block_device device = disk_device(disk);
    struct appliance_state_block_device store;
    struct appliance_persistent_config config;
    enum appliance_persistent_config_status load_status;
    enum appliance_persistent_config_status set_status;

    memset(&config, 0, sizeof(config));
    load_status = appliance_state_block_device_bind_range(
            &device, HFOC_FIRST_LBA, STORE_BLOCKS, &store) ==
        APPLIANCE_STATE_BLOCK_DEVICE_OK
        ? appliance_persistent_config_load(&config, &store.device)
        : APPLIANCE_PERSISTENT_CONFIG_BAD_DEVICE;
    set_status = load_status == APPLIANCE_PERSISTENT_CONFIG_OK
        ? appliance_persistent_config_set_hostname(
              &config, HOSTNAME, sizeof(HOSTNAME) - 1U)
        : load_status;
    CHECK(load_status == APPLIANCE_PERSISTENT_CONFIG_OK &&
              set_status == APPLIANCE_PERSISTENT_CONFIG_OK,
          "fixture commits hostname");
}

static enum appliance_production_boot_probe_status probe_fixture(
    void *context,
    struct appliance_production_boot_backend *backends,
    size_t capacity,
    size_t *count)
{
    struct probe_fixture *fixture = (struct probe_fixture *)context;
    size_t index;

    if (fixture->count > capacity) {
        return APPLIANCE_PRODUCTION_BOOT_PROBE_CAPACITY;
    }
    for (index = 0U; index < fixture->count; ++index) {
        const size_t source = fixture->reverse
            ? fixture->count - index - 1U
            : index;

        backends[index].kind =
            fixture->kind;
        backends[index].identity = source + 1U;
        backends[index].device = disk_device(&fixture->disks[source]);
    }
    *count = fixture->count;
    return APPLIANCE_PRODUCTION_BOOT_PROBE_OK;
}

static bool state_has_hostname(
    const struct appliance_production_boot *boot)
{
    const struct appliance_state_loader *loader =
        appliance_production_boot_state_loader(boot);
    char hostname[APPLIANCE_PERSISTENT_CONFIG_HOSTNAME_MAX + 1U];
    size_t length = 0U;

    return loader != NULL &&
           appliance_persistent_config_hostname(
               &loader->system_config, hostname, sizeof(hostname),
               &length) == APPLIANCE_PERSISTENT_CONFIG_OK &&
           length == sizeof(HOSTNAME) - 1U &&
           memcmp(hostname, HOSTNAME, length) == 0;
}

static void test_single_and_reload(void)
{
    struct probe_fixture fixture;
    struct appliance_production_boot boot;
    struct appliance_production_boot reload;

    install_gpt(&test_disks[0]);
    install_committed_hostname(&test_disks[0]);
    fixture.disks = &test_disks[0];
    fixture.count = 1U;
    fixture.reverse = false;
    fixture.kind = APPLIANCE_PRODUCTION_BOOT_BACKEND_NVME;

    CHECK(appliance_production_boot_init(&boot) ==
              APPLIANCE_PRODUCTION_BOOT_OK &&
              appliance_production_boot_run(
                  &boot, probe_fixture, &fixture) ==
                  APPLIANCE_PRODUCTION_BOOT_OK &&
              appliance_production_boot_readiness(&boot) ==
                  APPLIANCE_PRODUCTION_BOOT_READINESS_READY &&
              appliance_production_boot_mode(&boot) ==
                  APPLIANCE_PRODUCTION_BOOT_MODE_NORMAL &&
              boot.discovered_backend_count == 1U &&
              boot.valid_backend_count == 1U &&
              boot.selected_kind ==
                  APPLIANCE_PRODUCTION_BOOT_BACKEND_NVME &&
              state_has_hostname(&boot),
          "single production backend loads persistent state");

    CHECK(appliance_production_boot_init(&reload) ==
              APPLIANCE_PRODUCTION_BOOT_OK &&
              appliance_production_boot_run(
                  &reload, probe_fixture, &fixture) ==
                  APPLIANCE_PRODUCTION_BOOT_OK &&
              state_has_hostname(&reload),
          "reload selects same persistent state");
}

static void test_no_backend(void)
{
    struct probe_fixture fixture = {
        NULL, 0U, false, APPLIANCE_PRODUCTION_BOOT_BACKEND_AHCI
    };
    struct appliance_production_boot boot;

    CHECK(appliance_production_boot_init(&boot) ==
              APPLIANCE_PRODUCTION_BOOT_OK &&
              appliance_production_boot_run(
                  &boot, probe_fixture, &fixture) ==
                  APPLIANCE_PRODUCTION_BOOT_NO_BACKEND &&
              appliance_production_boot_readiness(&boot) ==
                  APPLIANCE_PRODUCTION_BOOT_READINESS_DEGRADED &&
              appliance_production_boot_mode(&boot) ==
                  APPLIANCE_PRODUCTION_BOOT_MODE_DEGRADED &&
              appliance_production_boot_state_loader(&boot) == NULL,
          "missing storage degrades without panic");
}

static void test_ambiguous_and_order(void)
{
    struct probe_fixture fixture;
    struct appliance_production_boot forward;
    struct appliance_production_boot reverse;

    install_gpt(&test_disks[0]);
    install_gpt(&test_disks[1]);
    fixture.disks = test_disks;
    fixture.count = 2U;
    fixture.kind = APPLIANCE_PRODUCTION_BOOT_BACKEND_AHCI;

    fixture.reverse = false;
    CHECK(appliance_production_boot_init(&forward) ==
              APPLIANCE_PRODUCTION_BOOT_OK &&
              appliance_production_boot_run(
                  &forward, probe_fixture, &fixture) ==
                  APPLIANCE_PRODUCTION_BOOT_AMBIGUOUS_BACKEND &&
              forward.valid_backend_count == 2U &&
              appliance_production_boot_readiness(&forward) ==
                  APPLIANCE_PRODUCTION_BOOT_READINESS_UNAVAILABLE,
          "multiple valid backends fail closed");
    fixture.reverse = true;
    CHECK(appliance_production_boot_init(&reverse) ==
              APPLIANCE_PRODUCTION_BOOT_OK &&
              appliance_production_boot_run(
                  &reverse, probe_fixture, &fixture) ==
                  APPLIANCE_PRODUCTION_BOOT_AMBIGUOUS_BACKEND &&
              reverse.valid_backend_count == 2U,
          "backend decision is independent of enumeration order");
}

static void test_invalid_candidate_does_not_create_ambiguity(void)
{
    struct probe_fixture fixture;
    struct appliance_production_boot boot;

    install_gpt(&test_disks[0]);
    memset(&test_disks[1], 0, sizeof(test_disks[1]));
    fixture.disks = test_disks;
    fixture.count = 2U;
    fixture.reverse = false;
    fixture.kind = APPLIANCE_PRODUCTION_BOOT_BACKEND_AHCI;

    CHECK(appliance_production_boot_init(&boot) ==
              APPLIANCE_PRODUCTION_BOOT_OK &&
              appliance_production_boot_run(
                  &boot, probe_fixture, &fixture) ==
                  APPLIANCE_PRODUCTION_BOOT_OK &&
              boot.discovered_backend_count == 2U &&
              boot.valid_backend_count == 1U &&
              boot.selected_identity == 1U,
          "invalid candidate is rejected without order tie-break");
}

int main(void)
{
    test_single_and_reload();
    test_no_backend();
    test_ambiguous_and_order();
    test_invalid_candidate_does_not_create_ambiguity();

    if (failures != 0) {
        fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    printf("appliance production boot host test: OK\n");
    return 0;
}

#else
int hfoc_appliance_production_boot_host_translation_unit_marker;
#endif
