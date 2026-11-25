/*
 * ITEM-519 increment 5: the boot-medium guard's host proof.
 *
 * The kernel reads one sector from a candidate device and asks whether it is
 * the medium the loader booted from.  The rule itself - compare the loader's
 * MBR disk id and partition index with the device's own table, and refuse
 * everything that does not line up - is pure, so every shape it must accept or
 * refuse is driven here on synthetic sectors, including the shapes that would
 * make a writer overwrite the wrong device:
 *
 *   - the same disk id on another medium must not pass (NO_MATCH), or the
 *     journal could be pointed at an installed disk that happens to carry an
 *     id;
 *   - a loader with no disk id must not pass (NO_IDENTITY), because there is
 *     then nothing the device could be checked against;
 *   - a matching id with an unusable partition entry (index outside the table,
 *     empty entry, extent past the device's own capacity) must not pass, or the
 *     writer would receive an extent that is not the medium's;
 *   - the protective GPT MBR is not a layout (PROTECTIVE_MBR), and a sector
 *     with no 0x55AA is not a table at all (NO_TABLE).
 *
 * Every verdict carries the reason that decided it, so the record on a machine
 * whose loader reports something unexpected says what it saw.
 */
#ifdef HFOC_INSTALL_MEDIUM_HOST_TEST

#include "install_medium.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static int failures;

#define CHECK(cond, msg) do { \
    if (!(cond)) { \
        fprintf(stderr, "FAIL: %s\n", msg); \
        ++failures; \
    } else { \
        printf("PASS: %s\n", msg); \
    } \
} while (0)

static uint8_t sector[INSTALL_MEDIUM_SECTOR_BYTES];

static void put_le32(size_t offset, uint32_t value) {
    sector[offset] = (uint8_t)(value & 0xFFU);
    sector[offset + 1U] = (uint8_t)((value >> 8U) & 0xFFU);
    sector[offset + 2U] = (uint8_t)((value >> 16U) & 0xFFU);
    sector[offset + 3U] = (uint8_t)((value >> 24U) & 0xFFU);
}

/*
 * One table with a distinct signature for each entry, so a test can prove the
 * loader's partition index selects the entry it names rather than the first.
 */
static void build_table(uint32_t disk_id) {
    static const uint8_t types[INSTALL_MEDIUM_MBR_PARTITION_COUNT] = {
        0x0CU, 0x83U, 0x07U, 0xEFU,
    };

    memset(sector, 0, sizeof(sector));
    put_le32(INSTALL_MEDIUM_MBR_DISK_ID_OFFSET, disk_id);
    sector[INSTALL_MEDIUM_MBR_SIGNATURE_OFFSET] = 0x55U;
    sector[INSTALL_MEDIUM_MBR_SIGNATURE_OFFSET + 1U] = 0xAAU;
    for (uint32_t index = 0U; index < INSTALL_MEDIUM_MBR_PARTITION_COUNT;
         ++index) {
        const size_t offset = INSTALL_MEDIUM_MBR_PARTITION_OFFSET +
                              (size_t)index *
                                  INSTALL_MEDIUM_MBR_PARTITION_SIZE;

        sector[offset + 4U] = types[index];
        put_le32(offset + 8U, 2048U + index * 4096U);
        put_le32(offset + 12U, 4096U);
    }
}

static void set_partition(uint32_t index, uint8_t type, uint32_t first_lba,
                          uint32_t length_lba) {
    const size_t offset = INSTALL_MEDIUM_MBR_PARTITION_OFFSET +
                          ((size_t)index - 1U) *
                              INSTALL_MEDIUM_MBR_PARTITION_SIZE;

    sector[offset + 4U] = type;
    put_le32(offset + 8U, first_lba);
    put_le32(offset + 12U, length_lba);
}

static struct install_medium_boot_identity boot_identity(uint32_t disk_id,
                                                         uint32_t partition) {
    struct install_medium_boot_identity boot;

    memset(&boot, 0, sizeof(boot));
    boot.media_type = INSTALL_MEDIUM_MEDIA_GENERIC;
    boot.partition_index = partition;
    boot.mbr_disk_id = disk_id;
    boot.mbr_disk_id_valid = true;
    return boot;
}

static void test_match_names_the_partition(void) {
    const struct install_medium_boot_identity boot =
        boot_identity(0x12345678U, 1U);
    const struct install_medium_verdict verdict =
        install_medium_identify(&boot, sector, 100000U);

    CHECK(verdict.result == INSTALL_MEDIUM_MATCH,
          "a medium with the loader's disk id matches");
    CHECK(strcmp(verdict.reason, "OK") == 0,
          "a match names OK as its reason");
    CHECK(verdict.disk_id == 0x12345678U,
          "the verdict carries the device's own disk id");
    CHECK(verdict.partition_index == 1U && verdict.partition_type == 0x0CU,
          "the verdict names the loader's partition entry");
    CHECK(verdict.first_lba == 2048U && verdict.length_lba == 4096U,
          "the verdict carries the partition's extent");
}

static void test_the_loader_index_selects_its_entry(void) {
    const struct install_medium_boot_identity boot =
        boot_identity(0x12345678U, 3U);
    const struct install_medium_verdict verdict =
        install_medium_identify(&boot, sector, 100000U);

    CHECK(verdict.result == INSTALL_MEDIUM_MATCH,
          "the third partition entry is usable");
    CHECK(verdict.partition_index == 3U && verdict.partition_type == 0x07U,
          "the third entry is the one the loader named");
    CHECK(verdict.first_lba == 2048U + 2U * 4096U,
          "the extent is the third entry's, not the first's");
}

/*
 * An isohybrid medium reports no partition: the loader read the kernel through
 * a filesystem, so the whole device is the medium.  The guard still has the
 * disk id to compare, and the extent it hands on covers the whole device.
 */
static void test_a_loader_without_a_partition_names_the_whole_device(void) {
    const struct install_medium_boot_identity boot =
        boot_identity(0x12345678U, 0U);
    const struct install_medium_verdict verdict =
        install_medium_identify(&boot, sector, 100000U);

    CHECK(verdict.result == INSTALL_MEDIUM_MATCH,
          "a matching id with no partition index is the medium");
    CHECK(verdict.partition_index == 0U && verdict.partition_type == 0U,
          "the whole-device verdict names no partition");
    CHECK(verdict.first_lba == 0U && verdict.length_lba == 100000U,
          "the whole-device extent covers the device");
}

static void test_another_disk_is_refused(void) {
    const struct install_medium_boot_identity boot =
        boot_identity(0xDEADBEEFU, 1U);
    const struct install_medium_verdict verdict =
        install_medium_identify(&boot, sector, 100000U);

    CHECK(verdict.result == INSTALL_MEDIUM_NO_MATCH,
          "another disk's id is not the boot medium");
    CHECK(strcmp(verdict.reason, "DISK_ID") == 0,
          "the refusal names the disk id");
    CHECK(verdict.disk_id == 0x12345678U,
          "the refusal still publishes what the device carries");
}

static void test_no_loader_identity_is_refused(void) {
    struct install_medium_boot_identity boot = boot_identity(0U, 1U);

    boot.mbr_disk_id_valid = false;
    {
        const struct install_medium_verdict verdict =
            install_medium_identify(&boot, sector, 100000U);

        CHECK(verdict.result == INSTALL_MEDIUM_NO_IDENTITY,
              "a loader with no disk id identifies nothing");
        CHECK(strcmp(verdict.reason, "NO_LOADER_ID") == 0,
              "the refusal names the missing loader id");
        CHECK(verdict.disk_id == 0x12345678U,
              "the device's id is still published");
    }
    boot = boot_identity(0U, 1U);
    {
        const struct install_medium_verdict verdict =
            install_medium_identify(&boot, sector, 100000U);

        CHECK(verdict.result == INSTALL_MEDIUM_NO_IDENTITY,
              "a zero disk id is treated as missing");
    }
}

static void test_a_sector_without_a_table_is_refused(void) {
    const struct install_medium_boot_identity boot =
        boot_identity(0x12345678U, 1U);

    memset(sector, 0, sizeof(sector));
    {
        const struct install_medium_verdict verdict =
            install_medium_identify(&boot, sector, 100000U);

        CHECK(verdict.result == INSTALL_MEDIUM_NO_TABLE,
              "a sector without 0x55AA is not a table");
        CHECK(strcmp(verdict.reason, "SIGNATURE") == 0,
              "the refusal names the signature");
    }
}

static void test_a_bad_partition_entry_is_refused(void) {
    const struct install_medium_boot_identity boot =
        boot_identity(0x12345678U, 1U);

    build_table(0x12345678U);
    {
        struct install_medium_boot_identity bad = boot;

        bad.partition_index = 5U;
        CHECK(install_medium_identify(&bad, sector, 100000U).result ==
                  INSTALL_MEDIUM_MALFORMED,
              "a loader index past the table is not a partition");
        CHECK(strcmp(install_medium_identify(&bad, sector, 100000U).reason,
                     "PARTITION") == 0,
              "the refusal names the partition");
    }
    set_partition(1U, 0x00U, 2048U, 4096U);
    CHECK(install_medium_identify(&boot, sector, 100000U).result ==
              INSTALL_MEDIUM_MALFORMED,
          "an empty partition entry is not an extent");
    set_partition(1U, 0x0CU, 0U, 4096U);
    CHECK(install_medium_identify(&boot, sector, 100000U).result ==
              INSTALL_MEDIUM_MALFORMED,
          "a partition that starts at zero is not an extent");
    set_partition(1U, 0x0CU, 2048U, 0U);
    CHECK(install_medium_identify(&boot, sector, 100000U).result ==
              INSTALL_MEDIUM_MALFORMED,
          "a partition with no length is not an extent");
}

static void test_an_extent_past_the_device_is_refused(void) {
    const struct install_medium_boot_identity boot =
        boot_identity(0x12345678U, 1U);

    build_table(0x12345678U);
    set_partition(1U, 0x0CU, 4096U, 4096U);
    {
        const struct install_medium_verdict verdict =
            install_medium_identify(&boot, sector, 8192U);

        CHECK(verdict.result == INSTALL_MEDIUM_MATCH,
              "an extent that ends on the last block matches");
        CHECK(verdict.first_lba == 4096U && verdict.length_lba == 4096U,
              "the last-block extent is the table's");
    }
    {
        const struct install_medium_verdict verdict =
            install_medium_identify(&boot, sector, 8191U);

        CHECK(verdict.result == INSTALL_MEDIUM_MALFORMED,
              "an extent that runs past the device is refused");
        CHECK(strcmp(verdict.reason, "RANGE") == 0,
              "the refusal names the range");
    }
    set_partition(1U, 0x0CU, 8192U, 4096U);
    CHECK(install_medium_identify(&boot, sector, 8192U).result ==
              INSTALL_MEDIUM_MALFORMED,
          "a partition starting past the device is refused");
    set_partition(1U, 0x0CU, 0xFFFFFF00U, 0x200U);
    CHECK(install_medium_identify(&boot, sector, 8192U).result ==
              INSTALL_MEDIUM_MALFORMED,
          "an extent that would wrap the capacity is refused");
}

static void test_the_protective_mbr_is_not_a_layout(void) {
    const struct install_medium_boot_identity boot =
        boot_identity(0x12345678U, 1U);

    build_table(0x12345678U);
    set_partition(1U, INSTALL_MEDIUM_MBR_TYPE_PROTECTIVE, 1U, 0xFFFFFFFFU);
    {
        const struct install_medium_verdict verdict =
            install_medium_identify(&boot, sector, 8192U);

        CHECK(verdict.result == INSTALL_MEDIUM_PROTECTIVE_MBR,
              "the protective GPT MBR is not a usable layout");
        CHECK(strcmp(verdict.reason, "GPT") == 0,
              "the refusal names the GPT container");
    }
}

static void test_missing_inputs_are_refused(void) {
    const struct install_medium_boot_identity boot =
        boot_identity(0x12345678U, 1U);
    const struct install_medium_verdict verdict =
        install_medium_identify(&boot, NULL, 100000U);

    CHECK(verdict.result == INSTALL_MEDIUM_NO_IDENTITY,
          "a probe without a sector identifies nothing");
    CHECK(strcmp(verdict.reason, "NO_SECTOR") == 0,
          "the refusal names the missing sector");
    CHECK(install_medium_identify(NULL, sector, 100000U).result ==
              INSTALL_MEDIUM_NO_IDENTITY,
          "a probe without a loader identity identifies nothing");
    CHECK(strcmp(install_medium_identify(NULL, sector, 100000U).reason,
                 "NO_LOADER") == 0,
          "the refusal names the missing loader identity");
}

static void test_result_names_are_distinct(void) {
    CHECK(strcmp(install_medium_result_string(INSTALL_MEDIUM_MATCH),
                 "MATCH") == 0,
          "MATCH has its own name");
    CHECK(strcmp(install_medium_result_string(INSTALL_MEDIUM_NO_MATCH),
                 "NO_MATCH") == 0,
          "NO_MATCH has its own name");
    CHECK(strcmp(install_medium_result_string(INSTALL_MEDIUM_NO_TABLE),
                 "NO_TABLE") == 0,
          "NO_TABLE has its own name");
    CHECK(strcmp(install_medium_result_string(INSTALL_MEDIUM_NO_IDENTITY),
                 "NO_IDENTITY") == 0,
          "NO_IDENTITY has its own name");
    CHECK(strcmp(install_medium_result_string(INSTALL_MEDIUM_MALFORMED),
                 "MALFORMED") == 0,
          "MALFORMED has its own name");
    CHECK(strcmp(
              install_medium_result_string(INSTALL_MEDIUM_PROTECTIVE_MBR),
              "PROTECTIVE_MBR") == 0,
          "PROTECTIVE_MBR has its own name");
}

/* ITEM-582: the diagnostic stick is a different device from the boot medium,
   so it is identified by its own disk id and its FAT12 partition; a random FAT
   volume or the boot disk itself must never match. */
static void test_the_diagnostic_stick_has_its_own_identity(void) {
    struct install_medium_verdict verdict;

    build_table(INSTALL_MEDIUM_DIAGNOSTIC_DISK_ID);
    /* build_table uses one distinct type per entry; entry 1 is the FAT12 one in
       that fixture only if the test's type table says so, so write entry 1 as a
       FAT12 partition explicitly. */
    sector[INSTALL_MEDIUM_MBR_PARTITION_OFFSET + 4U] =
        INSTALL_MEDIUM_MBR_TYPE_FAT12;
    verdict = install_medium_diagnostic_identify(sector, 100000U);
    CHECK(verdict.result == INSTALL_MEDIUM_MATCH,
          "the diagnostic id on a FAT12 partition is the diagnostic medium");
    CHECK(strcmp(verdict.reason, "DIAGNOSTIC") == 0,
          "the diagnostic verdict names its own reason");
    CHECK(verdict.partition_index == 1U,
          "the diagnostic verdict carries the partition it found");
    CHECK(verdict.first_lba != 0U && verdict.length_lba != 0U,
          "the diagnostic verdict carries the partition extent");

    build_table(0x12345678U);
    verdict = install_medium_diagnostic_identify(sector, 100000U);
    CHECK(verdict.result == INSTALL_MEDIUM_NO_MATCH,
          "another disk id is not a diagnostic medium");
    CHECK(strcmp(verdict.reason, "DISK_ID") == 0,
          "the refusal names the disk id");

    build_table(INSTALL_MEDIUM_DIAGNOSTIC_DISK_ID);
    sector[INSTALL_MEDIUM_MBR_PARTITION_OFFSET + 4U] = 0x83U;
    sector[INSTALL_MEDIUM_MBR_PARTITION_OFFSET +
           INSTALL_MEDIUM_MBR_PARTITION_SIZE + 4U] = 0x83U;
    sector[INSTALL_MEDIUM_MBR_PARTITION_OFFSET +
           2U * INSTALL_MEDIUM_MBR_PARTITION_SIZE + 4U] = 0x83U;
    sector[INSTALL_MEDIUM_MBR_PARTITION_OFFSET +
           3U * INSTALL_MEDIUM_MBR_PARTITION_SIZE + 4U] = 0x83U;
    verdict = install_medium_diagnostic_identify(sector, 100000U);
    CHECK(verdict.result == INSTALL_MEDIUM_NO_MATCH,
          "the diagnostic id without a FAT12 partition is not a medium");
    CHECK(strcmp(verdict.reason, "NO_FAT12") == 0,
          "the refusal names the missing FAT12 partition");

    build_table(INSTALL_MEDIUM_DIAGNOSTIC_DISK_ID);
    sector[INSTALL_MEDIUM_MBR_PARTITION_OFFSET + 4U] =
        INSTALL_MEDIUM_MBR_TYPE_FAT12;
    put_le32(INSTALL_MEDIUM_MBR_PARTITION_OFFSET + 8U, 90000U);
    put_le32(INSTALL_MEDIUM_MBR_PARTITION_OFFSET + 12U, 20000U);
    verdict = install_medium_diagnostic_identify(sector, 100000U);
    CHECK(verdict.result == INSTALL_MEDIUM_MALFORMED,
          "a diagnostic extent past the device is refused");
    CHECK(strcmp(verdict.reason, "RANGE") == 0,
          "the refusal names the range");

    verdict = install_medium_diagnostic_identify(NULL, 100000U);
    CHECK(verdict.result == INSTALL_MEDIUM_NO_IDENTITY,
          "a diagnostic probe without a sector identifies nothing");
}

int main(void) {
    build_table(0x12345678U);

    test_match_names_the_partition();
    test_the_loader_index_selects_its_entry();
    test_a_loader_without_a_partition_names_the_whole_device();
    test_another_disk_is_refused();
    test_no_loader_identity_is_refused();
    test_a_sector_without_a_table_is_refused();
    test_a_bad_partition_entry_is_refused();
    test_an_extent_past_the_device_is_refused();
    test_the_protective_mbr_is_not_a_layout();
    test_missing_inputs_are_refused();
    test_result_names_are_distinct();
    test_the_diagnostic_stick_has_its_own_identity();

    if (failures != 0) {
        fprintf(stderr, "Install medium host tests: %d FAILURES\n", failures);
        return 1;
    }
    puts("Install medium host tests: ALL OK");
    return 0;
}

#endif
