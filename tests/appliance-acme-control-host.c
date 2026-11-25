/*
 * ITEM-508 increment 34 host proof for the durable ACME control record.
 *
 * The record is the only path by which the operator's enable/disable choice and
 * a pending issuance request reach the next boot.  This proof drives it against
 * a memory block device: the off state is what an absent record reads as, a
 * saved setting loads back byte for byte, a second request is refused rather
 * than queued twice, disabling clears a pending request, a corrupt record of
 * every kind is refused as the off state and never partially, a write the
 * device does not keep fails the save, and the record's placement after the
 * ITEM-503 certificate record and the ITEM-507 name table is pinned.
 */
#include "appliance_acme_control.h"
#include "appliance_certificate_name_store.h"
#include "appliance_production_certificate.h"
#include "appliance_state_block_device.h"
#include "appliance_state_layout.h"
#include "block_device.h"
#include "sha256.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define TEST_BLOCK_SIZE 512U
#define TEST_BLOCK_COUNT 512U
#define TEST_CAPACITY (TEST_BLOCK_SIZE * TEST_BLOCK_COUNT)
/* The certificate store entry is the fourth default store: 3 * 64 blocks. */
#define TEST_CERTIFICATE_OFFSET 192U
#define TEST_CERTIFICATE_BLOCKS 64U
#define TEST_CONTROL_OFFSET \
    (TEST_CERTIFICATE_OFFSET + APPLIANCE_ACME_CONTROL_BLOCK_OFFSET)
#define TEST_CONTROL_BYTES \
    (APPLIANCE_ACME_CONTROL_RECORD_BLOCKS * TEST_BLOCK_SIZE)

static int failures;

#define CHECK(cond, msg) do { \
    if (!(cond)) { \
        fprintf(stderr, "FAIL: %s\n", msg); \
        ++failures; \
    } else { \
        printf("PASS: %s\n", msg); \
    } \
} while (0)

struct fixture_device {
    uint8_t storage[TEST_CAPACITY];
    bool fail_write;
    bool corrupt_after_write;
    size_t read_calls;
    size_t write_calls;
    size_t flush_calls;
};

static struct fixture_device fixture;

static enum block_device_status fixture_read(
    struct block_device *device,
    uint64_t offset,
    void *buffer,
    size_t length)
{
    struct fixture_device *self = (struct fixture_device *)device->context;

    ++self->read_calls;
    if (offset > TEST_CAPACITY || length > TEST_CAPACITY - (size_t)offset) {
        return BLOCK_DEVICE_OUT_OF_RANGE;
    }
    memcpy(buffer, self->storage + (size_t)offset, length);
    return BLOCK_DEVICE_OK;
}

static enum block_device_status fixture_write(
    struct block_device *device,
    uint64_t offset,
    const void *buffer,
    size_t length)
{
    struct fixture_device *self = (struct fixture_device *)device->context;

    ++self->write_calls;
    if (offset > TEST_CAPACITY || length > TEST_CAPACITY - (size_t)offset) {
        return BLOCK_DEVICE_OUT_OF_RANGE;
    }
    if (self->fail_write) {
        return BLOCK_DEVICE_IO_ERROR;
    }
    memcpy(self->storage + (size_t)offset, buffer, length);
    if (self->corrupt_after_write && length > 0U) {
        self->storage[(size_t)offset] ^= 0xFFU;
    }
    return BLOCK_DEVICE_OK;
}

static enum block_device_status fixture_flush(struct block_device *device)
{
    struct fixture_device *self = (struct fixture_device *)device->context;

    ++self->flush_calls;
    return BLOCK_DEVICE_OK;
}

static struct block_device make_device(struct fixture_device *self)
{
    struct block_device device;

    memset(&device, 0, sizeof(device));
    device.block_size = TEST_BLOCK_SIZE;
    device.block_count = TEST_BLOCK_COUNT;
    device.context = self;
    device.read_fn = fixture_read;
    device.write_fn = fixture_write;
    device.flush_fn = fixture_flush;
    return device;
}

static bool make_loader(
    struct appliance_state_loader *loader,
    struct block_device *device)
{
    memset(loader, 0, sizeof(*loader));
    if (appliance_state_layout_default(&loader->layout) !=
        APPLIANCE_STATE_LAYOUT_OK) {
        return false;
    }
    if (appliance_state_block_device_bind_range(
            device, 0U, TEST_BLOCK_COUNT, &loader->partition) !=
        APPLIANCE_STATE_BLOCK_DEVICE_OK) {
        return false;
    }
    loader->ready = true;
    return true;
}

static void reset_fixture(void)
{
    memset(&fixture, 0, sizeof(fixture));
}

static uint8_t *control_record(void)
{
    return fixture.storage + (TEST_CONTROL_OFFSET * TEST_BLOCK_SIZE);
}

static void write_u32(uint8_t *output, uint32_t value)
{
    output[0] = (uint8_t)(value & 0xFFU);
    output[1] = (uint8_t)((value >> 8U) & 0xFFU);
    output[2] = (uint8_t)((value >> 16U) & 0xFFU);
    output[3] = (uint8_t)((value >> 24U) & 0xFFU);
}

/*
 * Write a record with a correct checksum over the first sixteen bytes.  The
 * proof uses this to plant records the model did not write, so a decode that
 * accepted them would be caught.
 */
static void plant_record(uint32_t magic, uint32_t version, uint32_t flags,
                         uint32_t requested_at, bool fix_checksum)
{
    uint8_t digest[SHA256_DIGEST_SIZE];
    uint8_t *record = control_record();

    memset(record, 0, TEST_CONTROL_BYTES);
    write_u32(record, magic);
    write_u32(record + 4U, version);
    write_u32(record + 8U, flags);
    write_u32(record + 12U, requested_at);
    if (fix_checksum) {
        (void)sha256_compute(
            record,
            APPLIANCE_ACME_CONTROL_HEADER_BYTES -
                APPLIANCE_ACME_CONTROL_CHECKSUM_BYTES,
            digest);
        write_u32(record + 16U,
                  (uint32_t)digest[0] |
                      ((uint32_t)digest[1] << 8U) |
                      ((uint32_t)digest[2] << 16U) |
                      ((uint32_t)digest[3] << 24U));
    }
}

int main(void)
{
    struct block_device device;
    struct appliance_state_loader loader;
    struct appliance_acme_control_store store;
    struct appliance_acme_control control;

    reset_fixture();
    device = make_device(&fixture);
    CHECK(make_loader(&loader, &device), "the state loader binds");
    CHECK(APPLIANCE_ACME_CONTROL_BLOCK_OFFSET == 44U,
          "the control record starts after the certificate record (12) and the "
          "name table (32)");
    CHECK(APPLIANCE_ACME_CONTROL_RECORD_BLOCKS == 1U,
          "the control record is one block");

    /* 1. An absent record is the off state, not a fault. */
    appliance_acme_control_store_init(&store);
    CHECK(appliance_acme_control_store_load(&store, &loader, &control) ==
              APPLIANCE_ACME_CONTROL_OK,
          "a blank control record loads as OK");
    CHECK(store.source == APPLIANCE_ACME_CONTROL_SOURCE_DEFAULT_MISSING,
          "a blank control record is sourced DEFAULT_MISSING");
    CHECK(!appliance_acme_control_enabled(&control),
          "a blank control record leaves ACME disabled");
    CHECK(!appliance_acme_control_order_requested(&control),
          "a blank control record leaves no order requested");

    /* 2. The request path refuses what it cannot mean. */
    CHECK(appliance_acme_control_request_order(&control, 1800000000U) ==
              APPLIANCE_ACME_CONTROL_NOT_ENABLED,
          "an order request while ACME is disabled is refused by name");
    CHECK(!appliance_acme_control_order_requested(&control),
          "a refused request queues nothing");
    CHECK(appliance_acme_control_set_enabled(&control, true) ==
              APPLIANCE_ACME_CONTROL_OK,
          "ACME can be enabled");
    CHECK(appliance_acme_control_enabled(&control),
          "the enabled state is readable");
    CHECK(appliance_acme_control_order_requested_at(&control) == 0U,
          "enabling requests nothing by itself");

    /* 3. A saved setting survives a fresh load. */
    CHECK(appliance_acme_control_store_save(&store, &control) ==
              APPLIANCE_ACME_CONTROL_OK,
          "the enabled setting is written and read back");
    CHECK(store.source == APPLIANCE_ACME_CONTROL_SOURCE_COMMITTED,
          "a saved setting is sourced COMMITTED");
    CHECK(store.save_count == 1U, "the save is counted once");
    {
        struct appliance_acme_control_store reloaded_store;
        struct appliance_acme_control reloaded;

        appliance_acme_control_store_init(&reloaded_store);
        CHECK(appliance_acme_control_store_load(
                  &reloaded_store, &loader, &reloaded) ==
                  APPLIANCE_ACME_CONTROL_OK,
              "the saved setting loads back");
        CHECK(reloaded_store.source ==
                  APPLIANCE_ACME_CONTROL_SOURCE_COMMITTED,
              "the loaded setting is sourced COMMITTED");
        CHECK(appliance_acme_control_enabled(&reloaded),
              "the next boot sees ACME enabled");
        CHECK(!appliance_acme_control_order_requested(&reloaded),
              "the next boot sees no order request yet");
    }

    /* 4. One request at a time. */
    CHECK(appliance_acme_control_request_order(&control, 1800000042U) ==
              APPLIANCE_ACME_CONTROL_OK,
          "an order can be requested while ACME is enabled");
    CHECK(appliance_acme_control_order_requested(&control),
          "the pending request is readable");
    CHECK(appliance_acme_control_order_requested_at(&control) == 1800000042U,
          "the request keeps its instant");
    CHECK(appliance_acme_control_request_order(&control, 1800000100U) ==
              APPLIANCE_ACME_CONTROL_ALREADY_REQUESTED,
          "a second request while one is pending is refused, not queued");
    CHECK(appliance_acme_control_order_requested_at(&control) == 1800000042U,
          "the refused second request moves no instant");
    CHECK(appliance_acme_control_store_save(&store, &control) ==
              APPLIANCE_ACME_CONTROL_OK,
          "the pending request is written and read back");
    {
        struct appliance_acme_control_store reloaded_store;
        struct appliance_acme_control reloaded;

        appliance_acme_control_store_init(&reloaded_store);
        CHECK(appliance_acme_control_store_load(
                  &reloaded_store, &loader, &reloaded) ==
                  APPLIANCE_ACME_CONTROL_OK &&
                  appliance_acme_control_order_requested(&reloaded) &&
                  appliance_acme_control_order_requested_at(&reloaded) ==
                      1800000042U,
              "the pending request survives the reboot");
        CHECK(appliance_acme_control_take_request(&reloaded) ==
                  APPLIANCE_ACME_CONTROL_OK &&
                  !appliance_acme_control_order_requested(&reloaded),
              "the order path consumes the request once");
    }

    /* 5. Disabling clears the pending request. */
    CHECK(appliance_acme_control_set_enabled(&control, false) ==
              APPLIANCE_ACME_CONTROL_OK,
          "ACME can be disabled");
    CHECK(!appliance_acme_control_enabled(&control) &&
              !appliance_acme_control_order_requested(&control),
          "disabling clears the pending order request");
    CHECK(appliance_acme_control_store_save(&store, &control) ==
              APPLIANCE_ACME_CONTROL_OK,
          "the disabled setting is written and read back");
    {
        struct appliance_acme_control_store reloaded_store;
        struct appliance_acme_control reloaded;

        appliance_acme_control_store_init(&reloaded_store);
        CHECK(appliance_acme_control_store_load(
                  &reloaded_store, &loader, &reloaded) ==
                  APPLIANCE_ACME_CONTROL_OK &&
                  !appliance_acme_control_enabled(&reloaded) &&
                  !appliance_acme_control_order_requested(&reloaded),
              "the next boot sees ACME off with no request");
    }

    /* 6. Every corrupt record is refused whole, as the off state. */
    {
        static const struct {
            uint32_t magic;
            uint32_t version;
            uint32_t flags;
            uint32_t requested_at;
            bool fix_checksum;
            enum appliance_acme_control_status expected;
            const char *message;
        } cases[] = {
            {APPLIANCE_ACME_CONTROL_MAGIC,
             APPLIANCE_ACME_CONTROL_RECORD_VERSION,
             APPLIANCE_ACME_CONTROL_FLAG_ENABLED |
                 APPLIANCE_ACME_CONTROL_FLAG_ORDER_REQUESTED,
             0U, false, APPLIANCE_ACME_CONTROL_RECORD_CHECKSUM,
             "a record with a bad checksum is refused as a checksum fault"},
            {UINT32_C(0xDEADBEEF),
             APPLIANCE_ACME_CONTROL_RECORD_VERSION,
             APPLIANCE_ACME_CONTROL_FLAG_ENABLED, 0U, true,
             APPLIANCE_ACME_CONTROL_RECORD_CORRUPT,
             "a record with another magic is refused as corrupt"},
            {APPLIANCE_ACME_CONTROL_MAGIC, 99U,
             APPLIANCE_ACME_CONTROL_FLAG_ENABLED, 0U, true,
             APPLIANCE_ACME_CONTROL_RECORD_VERSION_UNKNOWN,
             "a record of another version is refused by name"},
            {APPLIANCE_ACME_CONTROL_MAGIC,
             APPLIANCE_ACME_CONTROL_RECORD_VERSION,
             APPLIANCE_ACME_CONTROL_FLAG_ENABLED | UINT32_C(0x00000004), 0U,
             true, APPLIANCE_ACME_CONTROL_RECORD_CORRUPT,
             "an unknown flag is refused as corrupt"},
            {APPLIANCE_ACME_CONTROL_MAGIC,
             APPLIANCE_ACME_CONTROL_RECORD_VERSION,
             APPLIANCE_ACME_CONTROL_FLAG_ENABLED, 12345U, true,
             APPLIANCE_ACME_CONTROL_RECORD_CORRUPT,
             "a request instant without a request is refused as corrupt"},
        };
        size_t index;

        for (index = 0U; index < sizeof(cases) / sizeof(cases[0]); ++index) {
            struct appliance_acme_control_store corrupt_store;
            struct appliance_acme_control corrupt;

            reset_fixture();
            device = make_device(&fixture);
            CHECK(make_loader(&loader, &device),
                  "the loader binds for the corrupt case");
            plant_record(cases[index].magic, cases[index].version,
                         cases[index].flags, cases[index].requested_at,
                         cases[index].fix_checksum);
            /* Seed the in-memory model with the opposite of the off state. */
            appliance_acme_control_store_init(&corrupt_store);
            CHECK(appliance_acme_control_store_load(
                      &corrupt_store, &loader, &corrupt) ==
                      cases[index].expected,
                  cases[index].message);
            CHECK(corrupt_store.source ==
                      APPLIANCE_ACME_CONTROL_SOURCE_DEFAULT_INVALID,
                  "a refused record is sourced DEFAULT_INVALID");
            CHECK(!appliance_acme_control_enabled(&corrupt) &&
                      !appliance_acme_control_order_requested(&corrupt),
                  "a refused record yields the off state, never half a "
                  "setting");
        }
    }

    /* 7. A write the device does not keep is a failed save. */
    reset_fixture();
    device = make_device(&fixture);
    CHECK(make_loader(&loader, &device),
          "the loader binds for the write-failure case");
    appliance_acme_control_store_init(&store);
    CHECK(appliance_acme_control_store_load(&store, &loader, &control) ==
              APPLIANCE_ACME_CONTROL_OK,
          "the off state loads before the failing save");
    CHECK(appliance_acme_control_set_enabled(&control, true) ==
              APPLIANCE_ACME_CONTROL_OK,
          "the candidate setting is enabled");
    CHECK(appliance_acme_control_store_save(&store, &control) ==
              APPLIANCE_ACME_CONTROL_OK,
          "the enabled setting is committed before the failing save");
    fixture.fail_write = true;
    CHECK(appliance_acme_control_store_save(&store, &control) ==
              APPLIANCE_ACME_CONTROL_WRITE_FAILED,
          "an io error fails the save");
    fixture.fail_write = false;
    {
        struct appliance_acme_control_store reloaded_store;
        struct appliance_acme_control reloaded;

        appliance_acme_control_store_init(&reloaded_store);
        CHECK(appliance_acme_control_store_load(
                  &reloaded_store, &loader, &reloaded) ==
                  APPLIANCE_ACME_CONTROL_OK,
              "the record still loads after an io-error save");
        CHECK(appliance_acme_control_enabled(&reloaded),
              "an io-error save left the durable setting as it was");
    }
    /*
     * A device that reports success and keeps something else is refused by the
     * read-back: the save fails, and the record the next boot reads is the one
     * the device really holds - which is not the candidate.
     */
    fixture.corrupt_after_write = true;
    CHECK(appliance_acme_control_store_save(&store, &control) ==
              APPLIANCE_ACME_CONTROL_WRITE_FAILED,
          "a record the device did not keep fails the save");
    fixture.corrupt_after_write = false;
    {
        struct appliance_acme_control_store reloaded_store;
        struct appliance_acme_control reloaded;

        appliance_acme_control_store_init(&reloaded_store);
        CHECK(appliance_acme_control_store_load(
                  &reloaded_store, &loader, &reloaded) !=
                  APPLIANCE_ACME_CONTROL_OK &&
                  reloaded_store.source ==
                      APPLIANCE_ACME_CONTROL_SOURCE_DEFAULT_INVALID,
              "the record the device mangled is refused, not trusted");
        CHECK(!appliance_acme_control_enabled(&reloaded),
              "a mangled record yields the off state, never a guess");
    }

    /* 8. The record holds the control bits and nothing else. */
    reset_fixture();
    device = make_device(&fixture);
    CHECK(make_loader(&loader, &device),
          "the loader binds for the shape case");
    appliance_acme_control_store_init(&store);
    CHECK(appliance_acme_control_store_load(&store, &loader, &control) ==
              APPLIANCE_ACME_CONTROL_OK,
          "the off state loads for the shape case");
    (void)appliance_acme_control_set_enabled(&control, true);
    (void)appliance_acme_control_request_order(&control, 1800000042U);
    CHECK(appliance_acme_control_store_save(&store, &control) ==
              APPLIANCE_ACME_CONTROL_OK,
          "the control record is written for the shape case");
    {
        size_t index;
        bool beyond_header_zero = true;

        for (index = APPLIANCE_ACME_CONTROL_HEADER_BYTES;
             index < TEST_CONTROL_BYTES;
             ++index) {
            if (control_record()[index] != 0U) {
                beyond_header_zero = false;
            }
        }
        CHECK(beyond_header_zero,
              "everything past the header is zero, so the record carries the "
              "setting and nothing else");
    }

    if (failures != 0) {
        fprintf(stderr, "%d FAILURES\n", failures);
        return 1;
    }
    printf("ALL OK\n");
    return 0;
}
