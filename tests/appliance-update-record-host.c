/*
 * ITEM-376 focused host proof: the durable two-copy update record, its
 * torn-write tolerance, the downgrade floor and the attempt budget.
 */

#ifdef HFOC_APPLIANCE_UPDATE_RECORD_HOST_TEST

#include "appliance_update_record.h"
#include "appliance_update_store.h"
#include "block_device.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static int failures;
static int checks;

#define CHECK(condition, message)                                        \
    do {                                                                 \
        ++checks;                                                        \
        if (!(condition)) {                                              \
            ++failures;                                                  \
            fprintf(stderr, "FAIL: %s\n", (message));                    \
        }                                                                \
    } while (0)

#define COPIES APPLIANCE_UPDATE_RECORD_COPY_COUNT
#define BLOCK APPLIANCE_UPDATE_RECORD_BLOCK_SIZE

struct host_store {
    uint8_t blocks[COPIES][BLOCK];
    bool valid[COPIES];
    uint32_t writes;
    uint32_t flushes;
    /* Fault injection. */
    int fail_write_copy;
    uint32_t fail_write_call;
    uint32_t write_calls;
    /* Bytes of the next write that land before the power fails. */
    uint32_t torn_write_bytes;
    bool drop_flush;
};

static struct host_store *host_context(void *context)
{
    return (struct host_store *)context;
}

static bool host_read(
    void *context,
    uint32_t copy,
    uint8_t *block,
    size_t length)
{
    struct host_store *store = host_context(context);

    if (copy >= COPIES || length != BLOCK || !store->valid[copy]) {
        return false;
    }
    memcpy(block, store->blocks[copy], BLOCK);
    return true;
}

static bool host_write(
    void *context,
    uint32_t copy,
    const uint8_t *block,
    size_t length)
{
    struct host_store *store = host_context(context);

    store->write_calls += 1U;
    if (copy >= COPIES || length != BLOCK) {
        return false;
    }
    if (store->fail_write_copy == (int)copy &&
        store->fail_write_call == store->write_calls) {
        return false;
    }
    store->writes += 1U;
    if (store->torn_write_bytes != 0U) {
        /*
         * The power failed mid-write: only a prefix of the new block
         * landed and the rest still holds the previous media content, so
         * the block is readable but its checksum no longer matches.
         */
        memcpy(store->blocks[copy], block, store->torn_write_bytes);
        store->torn_write_bytes = 0U;
    } else {
        memcpy(store->blocks[copy], block, BLOCK);
    }
    store->valid[copy] = true;
    return true;
}

static bool host_flush(void *context)
{
    struct host_store *store = host_context(context);

    store->flushes += 1U;
    return !store->drop_flush;
}

static struct appliance_update_record_io host_io(struct host_store *store)
{
    struct appliance_update_record_io io;

    memset(&io, 0, sizeof(io));
    io.context = store;
    io.read = host_read;
    io.write = host_write;
    io.flush = host_flush;
    return io;
}

static struct appliance_update_record_entry host_entry(
    uint32_t slot_id,
    uint32_t generation,
    uint8_t digest_value)
{
    struct appliance_update_record_entry entry;
    size_t index;

    memset(&entry, 0, sizeof(entry));
    entry.slot_id = slot_id;
    entry.generation = generation;
    for (index = 0U;
         index < APPLIANCE_UPDATE_RECORD_DIGEST_SIZE;
         ++index) {
        entry.digest[index] = (uint8_t)(digest_value + (uint8_t)index);
    }
    return entry;
}

static struct host_store host_empty_store(void)
{
    struct host_store store;

    memset(&store, 0, sizeof(store));
    store.fail_write_copy = -1;
    /* Model non-zero media so an interrupted write is detectable. */
    memset(store.blocks, 0x5A, sizeof(store.blocks));
    return store;
}

/* 1. First boot publishes exactly one copy; a reload reads it back. */
static void host_init_and_reload(void)
{
    struct host_store store = host_empty_store();
    struct appliance_update_record_io io = host_io(&store);
    struct appliance_update_record record;
    struct appliance_update_record reloaded;
    struct appliance_update_record_entry known_good =
        host_entry(1U, 7U, 0x10U);

    CHECK(appliance_update_record_load(&record, &io) ==
              APPLIANCE_UPDATE_RECORD_NO_RECORD,
          "an empty device has no update record");
    CHECK(store.writes == 0U,
          "a refused load never writes");

    CHECK(appliance_update_record_init(
              &record, 1U, &known_good, &io) ==
              APPLIANCE_UPDATE_RECORD_OK,
          "the first boot publishes the initial record");
    CHECK(record.sequence == 1U &&
              record.outcome == APPLIANCE_UPDATE_RECORD_OUTCOME_NONE &&
              record.installed_image_id == 1U &&
              record.attempts == 0U &&
              record.known_good.slot_id == 1U &&
              !appliance_update_record_pending(&record),
          "the initial record carries the installed image and known good");
    CHECK(store.writes == 1U && store.flushes == 1U && store.valid[0] &&
              !store.valid[1],
          "the initial record commits one copy and flushes it");

    /* Re-initialising an existing record must not reset it. */
    CHECK(appliance_update_record_init(
              &record, 9U, &known_good, &io) ==
              APPLIANCE_UPDATE_RECORD_BAD_STATE,
          "a second init refuses to overwrite an existing record");

    CHECK(appliance_update_record_load(&reloaded, &io) ==
              APPLIANCE_UPDATE_RECORD_OK,
          "the published record loads");
    CHECK(reloaded.sequence == 1U &&
              reloaded.installed_image_id == 1U &&
              reloaded.known_good.slot_id == 1U &&
              memcmp(reloaded.known_good.digest,
                     record.known_good.digest,
                     APPLIANCE_UPDATE_RECORD_DIGEST_SIZE) == 0,
          "the reloaded record is identical to the published one");
    CHECK(strcmp(appliance_update_record_status_string(
                     APPLIANCE_UPDATE_RECORD_DOWNGRADE_REFUSED),
                 "DOWNGRADE_REFUSED") == 0 &&
              strcmp(appliance_update_record_outcome_string(
                         APPLIANCE_UPDATE_RECORD_OUTCOME_ROLLED_BACK),
                     "ROLLED_BACK") == 0,
          "record strings are exact");
}

/* 2. Every store alternates copies, so the previous commit stays intact. */
static void host_alternating_copies(void)
{
    struct host_store store = host_empty_store();
    struct appliance_update_record_io io = host_io(&store);
    struct appliance_update_record record;
    struct appliance_update_record reloaded;
    struct appliance_update_record_entry known_good =
        host_entry(1U, 7U, 0x10U);
    struct appliance_update_record_entry target =
        host_entry(2U, 8U, 0x40U);
    struct appliance_update_record_entry rollback =
        host_entry(1U, 7U, 0x10U);

    CHECK(appliance_update_record_init(
              &record, 1U, &known_good, &io) ==
              APPLIANCE_UPDATE_RECORD_OK,
          "the alternating fixture initialises");
    CHECK(appliance_update_record_begin_pending(
              &record, &target, &rollback, &io) ==
              APPLIANCE_UPDATE_RECORD_OK,
          "a pending target is armed");
    CHECK(store.valid[0] && store.valid[1],
          "the second commit lands in the other copy");
    CHECK(record.sequence == 2U &&
              appliance_update_record_pending(&record) &&
              record.pending.slot_id == 2U &&
              record.rollback.slot_id == 1U,
          "the pending record carries the target and rollback source");

    CHECK(appliance_update_record_note_attempt(&record, &io) ==
              APPLIANCE_UPDATE_RECORD_OK,
          "a boot of the pending target is counted");
    CHECK(record.attempts == 1U && record.sequence == 3U,
          "the attempt counter is durable and monotonic");

    CHECK(appliance_update_record_load(&reloaded, &io) ==
              APPLIANCE_UPDATE_RECORD_OK,
          "the newest copy loads");
    CHECK(reloaded.sequence == 3U && reloaded.attempts == 1U &&
              reloaded.pending.slot_id == 2U &&
              reloaded.installed_image_id == 1U,
          "the newest copy is the head");
}

/* 3. A torn or damaged head falls back to the last committed copy. */
static void host_torn_write_falls_back(void)
{
    struct host_store store = host_empty_store();
    struct appliance_update_record_io io = host_io(&store);
    struct appliance_update_record record;
    struct appliance_update_record reloaded;
    struct appliance_update_record_entry known_good =
        host_entry(1U, 7U, 0x10U);
    struct appliance_update_record_entry target =
        host_entry(2U, 8U, 0x40U);
    struct appliance_update_record_entry rollback =
        host_entry(1U, 7U, 0x10U);

    CHECK(appliance_update_record_init(
              &record, 1U, &known_good, &io) ==
              APPLIANCE_UPDATE_RECORD_OK,
          "the torn-write fixture initialises");
    CHECK(record.sequence == 1U && record.installed_image_id == 1U,
          "the committed copy is the first sequence");

    /* The power fails while the second copy is being written. */
    store.torn_write_bytes = APPLIANCE_UPDATE_RECORD_BLOCK_SIZE / 2U;
    CHECK(appliance_update_record_begin_pending(
              &record, &target, &rollback, &io) ==
              APPLIANCE_UPDATE_RECORD_OK,
          "the torn write still reports success from the caller view");
    CHECK(appliance_update_record_load(&reloaded, &io) ==
              APPLIANCE_UPDATE_RECORD_OK,
          "the record still loads after a torn write");
    CHECK(reloaded.sequence == 1U && reloaded.installed_image_id == 1U &&
              !appliance_update_record_pending(&reloaded) &&
              reloaded.known_good.slot_id == 1U,
          "the torn head is ignored and the committed copy survives");
    CHECK(reloaded.attempts == 0U,
          "the torn attempt increment never became durable");

    /* A damaged head block is also ignored. */
    memcpy(store.blocks[1], store.blocks[0], BLOCK);
    store.blocks[1][0] ^= 0xFFU;
    CHECK(appliance_update_record_load(&reloaded, &io) ==
              APPLIANCE_UPDATE_RECORD_OK &&
              reloaded.sequence == 1U,
          "a corrupted head block falls back to the committed copy");

    /* Both copies damaged is a hard, honest failure. */
    store.blocks[0][0] ^= 0xFFU;
    CHECK(appliance_update_record_load(&reloaded, &io) ==
              APPLIANCE_UPDATE_RECORD_NO_RECORD,
          "two damaged copies publish no record");

    /* A write or flush fault is reported, never silently ignored. */
    store = host_empty_store();
    io = host_io(&store);
    CHECK(appliance_update_record_init(
              &record, 1U, &known_good, &io) ==
              APPLIANCE_UPDATE_RECORD_OK,
          "the write-fault fixture initialises");
    store.fail_write_copy = 1;
    store.fail_write_call = 2U; /* init already consumed the first call */
    CHECK(appliance_update_record_begin_pending(
              &record, &target, &rollback, &io) ==
              APPLIANCE_UPDATE_RECORD_WRITE_FAULT,
          "a failed copy write is reported as a write fault");
    CHECK(appliance_update_record_load(&reloaded, &io) ==
              APPLIANCE_UPDATE_RECORD_OK &&
              reloaded.sequence == 1U &&
              !appliance_update_record_pending(&reloaded),
          "a failed write leaves the committed copy as the head");
    store.drop_flush = true;
    CHECK(appliance_update_record_note_attempt(&reloaded, &io) ==
              APPLIANCE_UPDATE_RECORD_NO_PENDING,
          "a record without a pending target refuses an attempt");
    store.drop_flush = false;

    /* A failed flush on the very first commit is reported, never faked. */
    store = host_empty_store();
    io = host_io(&store);
    store.drop_flush = true;
    CHECK(appliance_update_record_init(
              &record, 1U, &known_good, &io) ==
              APPLIANCE_UPDATE_RECORD_WRITE_FAULT,
          "a failed flush is reported as a write fault");
    CHECK(store.writes == 1U && store.flushes == 1U,
          "the failed flush still attempted exactly one commit");
}

/* 4. Accept and rollback settle the record and the downgrade floor. */
static void host_accept_and_rollback(void)
{
    struct host_store store = host_empty_store();
    struct appliance_update_record_io io = host_io(&store);
    struct appliance_update_record record;
    struct appliance_update_record reloaded;
    struct appliance_update_record_entry known_good =
        host_entry(1U, 7U, 0x10U);
    struct appliance_update_record_entry target =
        host_entry(2U, 8U, 0x40U);
    struct appliance_update_record_entry rollback =
        host_entry(1U, 7U, 0x10U);

    CHECK(appliance_update_record_init(
              &record, 1U, &known_good, &io) ==
              APPLIANCE_UPDATE_RECORD_OK &&
              appliance_update_record_begin_pending(
                  &record, &target, &rollback, &io) ==
                  APPLIANCE_UPDATE_RECORD_OK,
          "the accept fixture arms a pending target");

    CHECK(appliance_update_record_accept(&record, 2U, &io) ==
              APPLIANCE_UPDATE_RECORD_OK,
          "a confirmed target is accepted");
    CHECK(record.outcome == APPLIANCE_UPDATE_RECORD_OUTCOME_ACCEPTED &&
              record.installed_image_id == 2U &&
              record.attempts == 0U &&
              !appliance_update_record_pending(&record) &&
              record.known_good.slot_id == 2U,
          "acceptance promotes the target and advances the downgrade floor");
    CHECK(appliance_update_record_load(&reloaded, &io) ==
              APPLIANCE_UPDATE_RECORD_OK &&
              reloaded.installed_image_id == 2U &&
              reloaded.known_good.slot_id == 2U,
          "the accepted record survives a reload");

    /* The accepted image is now the floor. */
    CHECK(appliance_update_record_check_upgrade(&reloaded, 2U, 2U) ==
              APPLIANCE_UPDATE_RECORD_DOWNGRADE_REFUSED,
          "re-installing the installed image is refused");
    CHECK(appliance_update_record_check_upgrade(&reloaded, 1U, 2U) ==
              APPLIANCE_UPDATE_RECORD_DOWNGRADE_REFUSED,
          "a lower target is refused as a downgrade");
    CHECK(appliance_update_record_check_upgrade(&reloaded, 3U, 1U) ==
              APPLIANCE_UPDATE_RECORD_SOURCE_MISMATCH,
          "a bundle built for another image is refused");
    CHECK(appliance_update_record_check_upgrade(&reloaded, 3U, 2U) ==
              APPLIANCE_UPDATE_RECORD_OK,
          "a strictly newer compatible target is admitted");

    /* A failed target rolls back and keeps the known-good slot. */
    target = host_entry(3U, 9U, 0x80U);
    CHECK(appliance_update_record_begin_pending(
              &reloaded, &target, &reloaded.known_good, &io) ==
              APPLIANCE_UPDATE_RECORD_OK,
          "a second update arms a pending target");
    CHECK(appliance_update_record_note_attempt(&reloaded, &io) ==
              APPLIANCE_UPDATE_RECORD_OK &&
              reloaded.attempts == 1U,
          "the second boot is counted");
    CHECK(appliance_update_record_roll_back(&reloaded, &io) ==
              APPLIANCE_UPDATE_RECORD_OK,
          "a failed target rolls back");
    CHECK(reloaded.outcome ==
              APPLIANCE_UPDATE_RECORD_OUTCOME_ROLLED_BACK &&
              !appliance_update_record_pending(&reloaded) &&
              reloaded.attempts == 0U &&
              reloaded.installed_image_id == 2U &&
              reloaded.known_good.slot_id == 2U,
          "rollback clears the pending target and keeps the known good");
    CHECK(appliance_update_record_load(&record, &io) ==
              APPLIANCE_UPDATE_RECORD_OK &&
              record.outcome ==
                  APPLIANCE_UPDATE_RECORD_OUTCOME_ROLLED_BACK,
          "the rollback survives a reload");

    CHECK(appliance_update_record_accept(&reloaded, 4U, &io) ==
              APPLIANCE_UPDATE_RECORD_NO_PENDING &&
              appliance_update_record_roll_back(&reloaded, &io) ==
              APPLIANCE_UPDATE_RECORD_NO_PENDING,
          "settling without a pending target is refused");
}

/* 5. The attempt budget is bounded and reports exhaustion. */
static void host_attempt_budget(void)
{
    struct host_store store = host_empty_store();
    struct appliance_update_record_io io = host_io(&store);
    struct appliance_update_record record;
    struct appliance_update_record_entry known_good =
        host_entry(1U, 7U, 0x10U);
    struct appliance_update_record_entry target =
        host_entry(2U, 8U, 0x40U);
    uint32_t attempt;

    CHECK(appliance_update_record_init(
              &record, 1U, &known_good, &io) ==
              APPLIANCE_UPDATE_RECORD_OK &&
              appliance_update_record_begin_pending(
                  &record, &target, &known_good, &io) ==
                  APPLIANCE_UPDATE_RECORD_OK,
          "the attempt fixture arms a pending target");
    CHECK(!appliance_update_record_attempts_exhausted(&record),
          "a fresh pending target has budget left");

    for (attempt = 1U;
         attempt <= APPLIANCE_UPDATE_RECORD_MAX_ATTEMPTS;
         ++attempt) {
        CHECK(appliance_update_record_note_attempt(&record, &io) ==
                  APPLIANCE_UPDATE_RECORD_OK &&
                  record.attempts == attempt,
              "each boot of the pending target is counted");
    }
    CHECK(appliance_update_record_attempts_exhausted(&record),
          "the attempt budget is exhausted at the declared bound");
    CHECK(record.attempts == APPLIANCE_UPDATE_RECORD_MAX_ATTEMPTS,
          "the attempt counter never exceeds the declared bound");
}

/* 6. Null and malformed input never publishes a record. */
static void host_rejects_bad_input(void)
{
    struct host_store store = host_empty_store();
    struct appliance_update_record_io io = host_io(&store);
    struct appliance_update_record record;
    struct appliance_update_record_entry known_good =
        host_entry(1U, 7U, 0x10U);
    struct appliance_update_record_io broken = io;

    CHECK(appliance_update_record_load(NULL, &io) ==
              APPLIANCE_UPDATE_RECORD_NULL &&
              appliance_update_record_load(&record, NULL) ==
              APPLIANCE_UPDATE_RECORD_NULL &&
              appliance_update_record_init(
                  &record, 1U, NULL, &io) ==
                  APPLIANCE_UPDATE_RECORD_NULL,
          "the record refuses null arguments");
    broken.read = NULL;
    CHECK(appliance_update_record_load(&record, &broken) ==
              APPLIANCE_UPDATE_RECORD_NULL,
          "a record without a read callback is refused");
    broken = io;
    broken.write = NULL;
    CHECK(appliance_update_record_init(
              &record, 1U, &known_good, &broken) ==
              APPLIANCE_UPDATE_RECORD_NULL,
          "a record without a write callback is refused");
    broken = io;
    broken.flush = NULL;
    CHECK(appliance_update_record_init(
              &record, 1U, &known_good, &broken) ==
              APPLIANCE_UPDATE_RECORD_NULL,
          "a record without a flush callback is refused");

    /* A device that answers with a wrong length is never trusted. */
    store.valid[0] = true;
    memset(store.blocks[0], 0, BLOCK);
    CHECK(appliance_update_record_load(&record, &io) ==
              APPLIANCE_UPDATE_RECORD_NO_RECORD,
          "a zeroed block is not a record");
    CHECK(appliance_update_record_check_upgrade(NULL, 2U, 1U) ==
              APPLIANCE_UPDATE_RECORD_NULL &&
              appliance_update_record_rollback_source(NULL) == NULL,
          "the inspectors refuse a null record");
}

/* 7. The production binding writes the record into two real device blocks. */
struct host_device {
    uint8_t bytes[8U * BLOCK];
    uint32_t block_size;
    uint64_t block_count;
    uint32_t flushes;
};

static enum block_device_status host_device_read(
    struct block_device *device,
    uint64_t offset,
    void *buffer,
    size_t length)
{
    struct host_device *media = (struct host_device *)device->context;

    if (offset + length > sizeof(media->bytes)) {
        return BLOCK_DEVICE_OUT_OF_RANGE;
    }
    memcpy(buffer, media->bytes + offset, length);
    return BLOCK_DEVICE_OK;
}

static enum block_device_status host_device_write(
    struct block_device *device,
    uint64_t offset,
    const void *buffer,
    size_t length)
{
    struct host_device *media = (struct host_device *)device->context;

    if (offset + length > sizeof(media->bytes)) {
        return BLOCK_DEVICE_OUT_OF_RANGE;
    }
    memcpy(media->bytes + offset, buffer, length);
    return BLOCK_DEVICE_OK;
}

static enum block_device_status host_device_flush(
    struct block_device *device)
{
    struct host_device *media = (struct host_device *)device->context;

    media->flushes += 1U;
    return BLOCK_DEVICE_OK;
}

static void host_store_binding(void)
{
    struct host_device media;
    struct block_device device;
    struct appliance_update_store store;
    const struct appliance_update_record_io *io;
    struct appliance_update_record record;
    struct appliance_update_record reloaded;
    struct appliance_update_record_entry known_good =
        host_entry(1U, 7U, 0x10U);
    struct appliance_update_record_entry target =
        host_entry(2U, 8U, 0x40U);

    memset(&media, 0, sizeof(media));
    memset(media.bytes, 0x5A, sizeof(media.bytes));
    media.block_size = APPLIANCE_UPDATE_RECORD_BLOCK_SIZE;
    media.block_count = 8U;
    memset(&device, 0, sizeof(device));
    device.block_size = media.block_size;
    device.block_count = media.block_count;
    device.context = &media;
    device.read_fn = host_device_read;
    device.write_fn = host_device_write;
    device.flush_fn = host_device_flush;

    CHECK(appliance_update_store_bind(&store, &device, 3U) ==
              APPLIANCE_UPDATE_STORE_OK &&
              appliance_update_store_is_bound(&store),
          "the store binds a two-block range on a real device");
    io = appliance_update_store_record_io(&store);
    CHECK(io != NULL && io->read != NULL && io->write != NULL &&
              io->flush != NULL,
          "the bound store publishes a complete record callback table");

    CHECK(appliance_update_record_init(
              &record, 1U, &known_good, io) ==
              APPLIANCE_UPDATE_RECORD_OK,
          "the record initialises through the device binding");
    CHECK(appliance_update_record_load(&reloaded, io) ==
              APPLIANCE_UPDATE_RECORD_OK &&
              reloaded.installed_image_id == 1U &&
              reloaded.known_good.slot_id == 1U,
          "the record reloads through the device binding");
    CHECK(appliance_update_record_begin_pending(
              &reloaded, &target, &known_good, io) ==
              APPLIANCE_UPDATE_RECORD_OK &&
              appliance_update_record_load(&record, io) ==
                  APPLIANCE_UPDATE_RECORD_OK &&
              appliance_update_record_pending(&record) &&
              record.pending.slot_id == 2U,
          "a second commit lands in the other device block");
    CHECK(media.flushes == 2U,
          "every commit went through the device flush");

    /* The record lives at the declared offset, not at the device start. */
    CHECK(media.bytes[0] == 0x5AU &&
              media.bytes[3U * BLOCK] != 0x5AU,
          "the record is written exactly at the bound range");

    /* A store that cannot be bound never produces a record. */
    CHECK(appliance_update_store_bind(&store, NULL, 0U) ==
              APPLIANCE_UPDATE_STORE_NULL &&
              appliance_update_store_bind(NULL, &device, 0U) ==
                  APPLIANCE_UPDATE_STORE_NULL,
          "the store refuses null arguments");
    CHECK(appliance_update_store_bind(&store, &device, 7U) ==
              APPLIANCE_UPDATE_STORE_BAD_RANGE,
          "a range that does not fit is refused");
    device.block_size = 1024U;
    CHECK(appliance_update_store_bind(&store, &device, 0U) ==
              APPLIANCE_UPDATE_STORE_BAD_DEVICE,
          "a device with the wrong block size is refused");
    CHECK(appliance_update_store_record_io(&store) == NULL &&
              !appliance_update_store_is_bound(&store),
          "an unbound store publishes no callback table");
    CHECK(strcmp(appliance_update_store_status_string(
                     APPLIANCE_UPDATE_STORE_BAD_RANGE),
                 "BAD_RANGE") == 0,
          "store status strings are exact");
}

int main(void)
{
    host_init_and_reload();
    host_alternating_copies();
    host_torn_write_falls_back();
    host_accept_and_rollback();
    host_attempt_budget();
    host_rejects_bad_input();
    host_store_binding();

    if (failures != 0) {
        fprintf(stderr,
                "Appliance update record host tests: %d FAILED\n",
                failures);
        return 1;
    }
    printf("Appliance update record host tests: ALL OK (%d checks)\n",
           checks);
    return 0;
}

#else
int appliance_update_record_host_translation_unit_marker;
#endif
