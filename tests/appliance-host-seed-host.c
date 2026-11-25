#include "appliance_host_seed.h"
#include "appliance-host-seed-old-slot.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define TEST_PARTITION_BLOCKS 640U
#define TEST_LAYOUT_CAPACITY 512U

#define CHECK(condition) do { \
        if (!(condition)) { \
            fprintf(stderr, "FAIL %s:%d: %s\n", \
                    __FILE__, __LINE__, #condition); \
            return 1; \
        } \
    } while (0)

void mbedtls_platform_zeroize(void *buffer, size_t length)
{
    volatile uint8_t *bytes = buffer;

    while (bytes != NULL && length != 0U) {
        *bytes = 0U;
        ++bytes;
        --length;
    }
}

struct test_disk {
    uint8_t bytes[TEST_PARTITION_BLOCKS * 512U];
    bool fail_write;
    bool fail_flush;
    bool short_write;
};

static enum block_device_status test_read(
    struct block_device *device,
    uint64_t offset,
    void *buffer,
    size_t length)
{
    struct test_disk *disk = device != NULL ? device->context : NULL;

    if (disk == NULL || buffer == NULL ||
        offset > sizeof(disk->bytes) ||
        length > sizeof(disk->bytes) - (size_t)offset) {
        return BLOCK_DEVICE_OUT_OF_RANGE;
    }
    memcpy(buffer, disk->bytes + (size_t)offset, length);
    return BLOCK_DEVICE_OK;
}

static enum block_device_status test_write(
    struct block_device *device,
    uint64_t offset,
    const void *buffer,
    size_t length)
{
    struct test_disk *disk = device != NULL ? device->context : NULL;

    if (disk == NULL || buffer == NULL ||
        offset > sizeof(disk->bytes) ||
        length > sizeof(disk->bytes) - (size_t)offset) {
        return BLOCK_DEVICE_OUT_OF_RANGE;
    }
    if (disk->fail_write) {
        return BLOCK_DEVICE_IO_ERROR;
    }
    if (disk->short_write) {
        const size_t written = length / 4U;

        memcpy(disk->bytes + (size_t)offset, buffer, written);
        return BLOCK_DEVICE_IO_ERROR;
    }
    memcpy(disk->bytes + (size_t)offset, buffer, length);
    return BLOCK_DEVICE_OK;
}

static enum block_device_status test_flush(struct block_device *device)
{
    struct test_disk *disk = device != NULL ? device->context : NULL;

    if (disk == NULL) {
        return BLOCK_DEVICE_BAD_DEVICE;
    }
    return disk->fail_flush ? BLOCK_DEVICE_IO_ERROR : BLOCK_DEVICE_OK;
}

static struct block_device test_device(struct test_disk *disk)
{
    const struct block_device device = {
        .context = disk,
        .block_size = 512U,
        .block_count = TEST_PARTITION_BLOCKS,
        .read_fn = test_read,
        .write_fn = test_write,
        .flush_fn = test_flush,
    };

    return device;
}

static struct appliance_host_seed_media_record test_record(void)
{
    struct appliance_host_seed_media_record record;
    uint32_t index;

    memset(&record, 0, sizeof(record));
    record.version = APPLIANCE_HOST_SEED_VERSION;
    record.domain = APPLIANCE_HOST_SEED_DOMAIN_INSTALL;
    record.build_id = 1U;
    memcpy(record.target, "TESTNVME000001", sizeof("TESTNVME000001"));
    for (index = 0U; index < APPLIANCE_HOST_SEED_LENGTH; ++index) {
        record.seed[index] = (uint8_t)(index * 7U + 3U);
    }
    return record;
}

static int test_media_record(void)
{
    struct appliance_host_seed_media_record record = test_record();
    struct appliance_host_seed_media_record parsed;
    uint8_t bytes[APPLIANCE_HOST_SEED_MEDIA_RECORD_LENGTH];

    CHECK(appliance_host_seed_media_build(&record, bytes) ==
          APPLIANCE_HOST_SEED_OK);
    CHECK(appliance_host_seed_media_parse(bytes, sizeof(bytes), &parsed) ==
          APPLIANCE_HOST_SEED_OK);
    CHECK(parsed.build_id == record.build_id);
    CHECK(strcmp(parsed.target, record.target) == 0);
    CHECK(memcmp(parsed.seed, record.seed, sizeof(record.seed)) == 0);
    CHECK(appliance_host_seed_media_match(
              &parsed, "TESTNVME000001", record.build_id) ==
          APPLIANCE_HOST_SEED_OK);
    CHECK(appliance_host_seed_media_match(
              &parsed, "TESTNVME000002", record.build_id) ==
          APPLIANCE_HOST_SEED_BAD_TARGET);
    CHECK(appliance_host_seed_media_match(
              &parsed, record.target, record.build_id + 1U) ==
          APPLIANCE_HOST_SEED_BAD_BUILD);
    CHECK(appliance_host_seed_media_parse(
              bytes, sizeof(bytes) - 1U, &parsed) ==
          APPLIANCE_HOST_SEED_BAD_LENGTH);
    bytes[60U] ^= 1U;
    CHECK(appliance_host_seed_media_parse(bytes, sizeof(bytes), &parsed) ==
          APPLIANCE_HOST_SEED_BAD_INTEGRITY);
    CHECK(appliance_host_seed_media_build(&record, bytes) ==
          APPLIANCE_HOST_SEED_OK);
    bytes[0] ^= 1U;
    CHECK(appliance_host_seed_media_parse(bytes, sizeof(bytes), &parsed) ==
          APPLIANCE_HOST_SEED_BAD_MAGIC);
    record.domain = 2U;
    CHECK(appliance_host_seed_media_build(&record, bytes) ==
          APPLIANCE_HOST_SEED_BAD_DOMAIN);
    record.domain = APPLIANCE_HOST_SEED_DOMAIN_INSTALL;
    record.build_id = 0U;
    CHECK(appliance_host_seed_media_build(&record, bytes) ==
          APPLIANCE_HOST_SEED_BAD_BUILD);
    record = test_record();
    record.target[0] = '\0';
    CHECK(appliance_host_seed_media_build(&record, bytes) ==
          APPLIANCE_HOST_SEED_BAD_TARGET);
    record = test_record();
    memset(record.seed, 0, sizeof(record.seed));
    CHECK(appliance_host_seed_media_build(&record, bytes) ==
          APPLIANCE_HOST_SEED_LOW_DIVERSITY);
    appliance_host_seed_clear_media(&record);
    appliance_host_seed_clear_media(&parsed);
    return 0;
}

static int test_store_lifecycle(void)
{
    struct test_disk disk;
    struct block_device device;
    struct appliance_host_seed_media_record record = test_record();
    uint8_t first_seed[APPLIANCE_HOST_SEED_LENGTH];
    uint8_t second_seed[APPLIANCE_HOST_SEED_LENGTH];
    uint8_t derived_a[APPLIANCE_HOST_SEED_LENGTH];
    uint8_t derived_b[APPLIANCE_HOST_SEED_LENGTH];
    uint64_t epoch = 0U;
    bool present = true;
    static const char password[] = "correct horse battery staple";

    memset(&disk, 0, sizeof(disk));
    device = test_device(&disk);
    CHECK(appliance_host_seed_store_present(
              &device, TEST_LAYOUT_CAPACITY, &present) ==
              APPLIANCE_HOST_SEED_OK &&
          !present);
    CHECK(appliance_host_seed_store_unlock(
              &device, TEST_LAYOUT_CAPACITY, password,
              sizeof(password) - 1U, first_seed, &epoch) ==
          APPLIANCE_HOST_SEED_MISSING);
    CHECK(appliance_host_seed_store_provision(
              &device, TEST_LAYOUT_CAPACITY, &record,
              password, sizeof(password) - 1U) ==
          APPLIANCE_HOST_SEED_OK);
    CHECK(appliance_host_seed_store_present(
              &device, TEST_LAYOUT_CAPACITY, &present) ==
              APPLIANCE_HOST_SEED_OK &&
          present);
    CHECK(appliance_host_seed_store_unlock(
              &device, TEST_LAYOUT_CAPACITY, password,
              sizeof(password) - 1U, first_seed, &epoch) ==
          APPLIANCE_HOST_SEED_OK);
    CHECK(epoch == 2U);
    CHECK(memcmp(first_seed, record.seed, sizeof(first_seed)) == 0);
    CHECK(appliance_host_seed_store_unlock(
              &device, TEST_LAYOUT_CAPACITY, password,
              sizeof(password) - 1U, second_seed, &epoch) ==
          APPLIANCE_HOST_SEED_OK);
    CHECK(epoch == 3U);
    CHECK(memcmp(first_seed, second_seed, sizeof(first_seed)) == 0);
    CHECK(appliance_host_seed_store_unlock(
              &device, TEST_LAYOUT_CAPACITY, "wrong password",
              sizeof("wrong password") - 1U, second_seed, &epoch) ==
          APPLIANCE_HOST_SEED_WRONG_PASSWORD);
    CHECK(epoch == 0U);
    CHECK(appliance_host_seed_derive(
              first_seed, 2U, 0U, derived_a) == APPLIANCE_HOST_SEED_OK);
    CHECK(appliance_host_seed_derive(
              first_seed, 3U, 0U, derived_b) == APPLIANCE_HOST_SEED_OK);
    CHECK(memcmp(derived_a, derived_b, sizeof(derived_a)) != 0);
    CHECK(appliance_host_seed_derive(
              first_seed, 3U, 1U, derived_a) == APPLIANCE_HOST_SEED_OK);
    CHECK(memcmp(derived_a, derived_b, sizeof(derived_a)) != 0);
    /*
     * Slot 0 carries sequence 3 after the second unlock.  A torn record must
     * fail closed rather than fall back to sequence 2 and reuse its stream.
     */
    disk.bytes[TEST_LAYOUT_CAPACITY * 512U] ^= 1U;
    CHECK(appliance_host_seed_store_unlock(
              &device, TEST_LAYOUT_CAPACITY, password,
              sizeof(password) - 1U, second_seed, &epoch) ==
          APPLIANCE_HOST_SEED_CORRUPT);
    CHECK(epoch == 0U);
    appliance_host_seed_clear_media(&record);
    memset(first_seed, 0, sizeof(first_seed));
    memset(second_seed, 0, sizeof(second_seed));
    memset(derived_a, 0, sizeof(derived_a));
    memset(derived_b, 0, sizeof(derived_b));
    return 0;
}

static int test_legacy_store_without_seed_slots(void)
{
    struct test_disk disk;
    struct block_device device;
    uint8_t seed[APPLIANCE_HOST_SEED_LENGTH];
    uint64_t epoch = 0U;
    bool present = true;
    static const char old_password[] = "correct horse battery staple";
    static const char new_password[] = "new correct horse battery staple";

    memset(&disk, 0, sizeof(disk));
    memset(seed, 0, sizeof(seed));
    device = test_device(&disk);
    /*
     * The legacy layout ends exactly at the device boundary.  Its missing
     * companion slots mean "no host seed", not a corrupt seed store.
     */
    device.block_count = TEST_LAYOUT_CAPACITY;
    CHECK(appliance_host_seed_store_present(
              &device, TEST_LAYOUT_CAPACITY, &present) ==
              APPLIANCE_HOST_SEED_OK &&
          !present);
    /*
     * A real unlock or rekey still has to fail closed because there is no
     * durable slot available for the password-sealed record.
     */
    CHECK(appliance_host_seed_store_unlock(
              &device, TEST_LAYOUT_CAPACITY, old_password,
              sizeof(old_password) - 1U, seed, &epoch) ==
          APPLIANCE_HOST_SEED_BAD_LAYOUT);
    CHECK(appliance_host_seed_store_rekey(
              &device, TEST_LAYOUT_CAPACITY, old_password,
              sizeof(old_password) - 1U, new_password,
              sizeof(new_password) - 1U) ==
          APPLIANCE_HOST_SEED_BAD_LAYOUT);
    return 0;
}

static int test_store_reprovision(void)
{
    struct test_disk disk;
    struct block_device device;
    struct appliance_host_seed_media_record first = test_record();
    struct appliance_host_seed_media_record second = test_record();
    uint8_t seed[APPLIANCE_HOST_SEED_LENGTH];
    uint64_t epoch = 0U;
    static const char password[] = "correct horse battery staple";

    memset(&disk, 0, sizeof(disk));
    device = test_device(&disk);
    second.build_id = 2U;
    for (uint32_t index = 0U; index < APPLIANCE_HOST_SEED_LENGTH; ++index) {
        second.seed[index] = (uint8_t)(index * 11U + 9U);
    }
    CHECK(appliance_host_seed_store_provision(
              &device, TEST_LAYOUT_CAPACITY, &first,
              password, sizeof(password) - 1U) == APPLIANCE_HOST_SEED_OK);
    CHECK(appliance_host_seed_store_unlock(
              &device, TEST_LAYOUT_CAPACITY, password,
              sizeof(password) - 1U, seed, &epoch) == APPLIANCE_HOST_SEED_OK);
    CHECK(epoch == 2U);
    /*
     * A retried update can encounter a complete seed written by an earlier
     * attempt.  Provisioning is an explicit replacement operation: it writes
     * the new sequence-1 record and clears the stale companion slot.
     */
    CHECK(appliance_host_seed_store_provision(
              &device, TEST_LAYOUT_CAPACITY, &second,
              password, sizeof(password) - 1U) == APPLIANCE_HOST_SEED_OK);
    CHECK(appliance_host_seed_store_unlock(
              &device, TEST_LAYOUT_CAPACITY, password,
              sizeof(password) - 1U, seed, &epoch) == APPLIANCE_HOST_SEED_OK);
    CHECK(epoch == 2U);
    CHECK(memcmp(seed, second.seed, sizeof(seed)) == 0);
    appliance_host_seed_clear_media(&first);
    appliance_host_seed_clear_media(&second);
    memset(seed, 0, sizeof(seed));
    return 0;
}

static int test_store_password_change(void)
{
    struct test_disk disk;
    struct block_device device;
    struct appliance_host_seed_media_record record = test_record();
    uint8_t seed[APPLIANCE_HOST_SEED_LENGTH];
    uint64_t epoch = 0U;
    static const char old_password[] = "correct horse battery staple";
    static const char new_password[] = "new correct horse battery staple";

    memset(&disk, 0, sizeof(disk));
    device = test_device(&disk);
    CHECK(appliance_host_seed_store_provision(
              &device, TEST_LAYOUT_CAPACITY, &record,
              old_password, sizeof(old_password) - 1U) ==
          APPLIANCE_HOST_SEED_OK);
    CHECK(appliance_host_seed_store_unlock(
              &device, TEST_LAYOUT_CAPACITY, old_password,
              sizeof(old_password) - 1U, seed, &epoch) ==
          APPLIANCE_HOST_SEED_OK);
    CHECK(epoch == 2U);
    CHECK(appliance_host_seed_store_rekey(
              &device, TEST_LAYOUT_CAPACITY, "wrong password",
              sizeof("wrong password") - 1U,
              new_password, sizeof(new_password) - 1U) ==
          APPLIANCE_HOST_SEED_WRONG_PASSWORD);
    CHECK(appliance_host_seed_store_rekey(
              &device, TEST_LAYOUT_CAPACITY, old_password,
              sizeof(old_password) - 1U,
              new_password, sizeof(new_password) - 1U) ==
          APPLIANCE_HOST_SEED_OK);
    CHECK(appliance_host_seed_store_unlock(
              &device, TEST_LAYOUT_CAPACITY, new_password,
              sizeof(new_password) - 1U, seed, &epoch) ==
          APPLIANCE_HOST_SEED_OK);
    CHECK(epoch == 4U);
    CHECK(memcmp(seed, record.seed, sizeof(seed)) == 0);
    CHECK(appliance_host_seed_store_unlock(
              &device, TEST_LAYOUT_CAPACITY, old_password,
              sizeof(old_password) - 1U, seed, &epoch) ==
          APPLIANCE_HOST_SEED_WRONG_PASSWORD);
    CHECK(epoch == 0U);
    appliance_host_seed_clear_media(&record);
    memset(seed, 0, sizeof(seed));
    return 0;
}

/*
 * ITEM-611: a slot sealed by the old implementation still opens after the
 * prepared-HMAC change.  The format and derivation are unchanged; the test
 * would fail if the new code changed either.
 */
static int test_old_sealed_slot_compatibility(void)
{
    static const char password[] = "correct horse battery staple";
    struct test_disk disk;
    struct block_device device;
    const size_t slot0_offset = TEST_LAYOUT_CAPACITY * 512U;
    struct appliance_host_seed_media_record record = test_record();
    uint8_t seed[APPLIANCE_HOST_SEED_LENGTH];
    uint64_t epoch = 0U;

    memset(&disk, 0, sizeof(disk));
    memcpy(disk.bytes + slot0_offset, old_slot0, sizeof(old_slot0));
    device = test_device(&disk);
    CHECK(appliance_host_seed_store_unlock(
              &device, TEST_LAYOUT_CAPACITY, password,
              sizeof(password) - 1U, seed, &epoch) ==
              APPLIANCE_HOST_SEED_OK);
    CHECK(epoch == 2U);
    CHECK(memcmp(seed, record.seed, sizeof(record.seed)) == 0);
    return 0;
}

static int test_store_failures(void)
{
    struct test_disk disk;
    struct block_device device;
    struct appliance_host_seed_media_record record = test_record();
    uint8_t seed[APPLIANCE_HOST_SEED_LENGTH];
    uint64_t epoch = 0U;
    enum appliance_host_seed_status status;
    static const char password[] = "correct horse battery staple";

    memset(&disk, 0, sizeof(disk));
    device = test_device(&disk);
    disk.fail_write = true;
    CHECK(appliance_host_seed_store_provision(
              &device, TEST_LAYOUT_CAPACITY, &record,
              password, sizeof(password) - 1U) ==
          APPLIANCE_HOST_SEED_IO_ERROR);
    disk.fail_write = false;
    CHECK(appliance_host_seed_store_provision(
              &device, TEST_LAYOUT_CAPACITY, &record,
              password, sizeof(password) - 1U) ==
          APPLIANCE_HOST_SEED_OK);

    disk.fail_write = true;
    CHECK(appliance_host_seed_store_unlock(
              &device, TEST_LAYOUT_CAPACITY, password,
              sizeof(password) - 1U, seed, &epoch) ==
          APPLIANCE_HOST_SEED_IO_ERROR);
    CHECK(epoch == 0U);
    CHECK(memcmp(seed, (uint8_t[APPLIANCE_HOST_SEED_LENGTH]){0},
                 sizeof(seed)) == 0);
    disk.fail_write = false;
    CHECK(appliance_host_seed_store_unlock(
              &device, TEST_LAYOUT_CAPACITY, password,
              sizeof(password) - 1U, seed, &epoch) ==
          APPLIANCE_HOST_SEED_OK);
    CHECK(epoch == 2U);

    /*
     * A torn companion write fails closed.  Falling back to the older slot
     * after an attempted durable advance could reuse a stream.
     */
    disk.short_write = true;
    CHECK(appliance_host_seed_store_unlock(
              &device, TEST_LAYOUT_CAPACITY, password,
              sizeof(password) - 1U, seed, &epoch) ==
          APPLIANCE_HOST_SEED_IO_ERROR);
    disk.short_write = false;
    status = appliance_host_seed_store_unlock(
        &device, TEST_LAYOUT_CAPACITY, password,
        sizeof(password) - 1U, seed, &epoch);
    CHECK(status != APPLIANCE_HOST_SEED_OK);
    CHECK(epoch == 0U);
    CHECK(memcmp(seed, (uint8_t[APPLIANCE_HOST_SEED_LENGTH]){0},
                 sizeof(seed)) == 0);
    appliance_host_seed_clear_media(&record);
    return 0;
}

int main(void)
{
    CHECK(test_media_record() == 0);
    CHECK(test_store_lifecycle() == 0);
    CHECK(test_legacy_store_without_seed_slots() == 0);
    CHECK(test_store_reprovision() == 0);
    CHECK(test_store_password_change() == 0);
    CHECK(test_old_sealed_slot_compatibility() == 0);
    CHECK(test_store_failures() == 0);
    puts("appliance host seed host test: OK");
    return 0;
}
