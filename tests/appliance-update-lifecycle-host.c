/*
 * ITEM-376 focused host proof: the boot-side update lifecycle -- resolve,
 * attempt budget, slot integrity, accept, rollback and downgrade admission.
 */

#ifdef HFOC_APPLIANCE_UPDATE_LIFECYCLE_HOST_TEST

#include "appliance_update_lifecycle.h"

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
    uint32_t torn_write_bytes;
    bool drop_flush;
    /* The slot the fixture currently claims to hold. */
    uint32_t intact_slot_id;
    bool intact;
};

static bool host_read(void *context, uint32_t copy, uint8_t *block,
                      size_t length)
{
    struct host_store *store = (struct host_store *)context;

    if (copy >= COPIES || length != BLOCK || !store->valid[copy]) {
        return false;
    }
    memcpy(block, store->blocks[copy], BLOCK);
    return true;
}

static bool host_write(void *context, uint32_t copy, const uint8_t *block,
                       size_t length)
{
    struct host_store *store = (struct host_store *)context;

    if (copy >= COPIES || length != BLOCK) {
        return false;
    }
    store->writes += 1U;
    if (store->torn_write_bytes != 0U) {
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
    struct host_store *store = (struct host_store *)context;

    store->flushes += 1U;
    return !store->drop_flush;
}

static bool host_slot_intact(
    void *context,
    const struct appliance_update_record_entry *entry)
{
    struct host_store *store = (struct host_store *)context;

    return store->intact && entry != NULL &&
           entry->slot_id == store->intact_slot_id;
}

static struct host_store host_store(void)
{
    struct host_store store;

    memset(&store, 0, sizeof(store));
    memset(store.blocks, 0x5A, sizeof(store.blocks));
    store.intact = true;
    return store;
}

static struct appliance_update_lifecycle_io host_io(struct host_store *store)
{
    struct appliance_update_lifecycle_io io;

    memset(&io, 0, sizeof(io));
    io.context = store;
    io.record.context = store;
    io.record.read = host_read;
    io.record.write = host_write;
    io.record.flush = host_flush;
    io.slot_intact = host_slot_intact;
    return io;
}

static struct appliance_update_record_entry host_entry(uint32_t slot_id,
                                                       uint8_t digest_value)
{
    struct appliance_update_record_entry entry;
    size_t index;

    memset(&entry, 0, sizeof(entry));
    entry.slot_id = slot_id;
    entry.generation = slot_id + 1U;
    for (index = 0U;
         index < APPLIANCE_UPDATE_RECORD_DIGEST_SIZE;
         ++index) {
        entry.digest[index] = (uint8_t)(digest_value + (uint8_t)index);
    }
    return entry;
}

/* 1. First boot publishes the record and resolves to the known good slot. */
static void host_first_boot(void)
{
    struct host_store store = host_store();
    struct appliance_update_lifecycle_io io = host_io(&store);
    struct appliance_update_lifecycle lifecycle;
    struct appliance_update_lifecycle_snapshot snapshot;
    struct appliance_update_record_entry known_good =
        host_entry(1U, 0x10U);
    char rendered[APPLIANCE_UPDATE_LIFECYCLE_RENDER_MAX];
    size_t length = 0U;

    CHECK(appliance_update_lifecycle_init(
              &lifecycle, 1U, &known_good, &io) ==
              APPLIANCE_UPDATE_LIFECYCLE_OK,
          "the first boot initialises the lifecycle");
    CHECK(lifecycle.state == APPLIANCE_UPDATE_LIFECYCLE_STATE_IDLE &&
              lifecycle.record.installed_image_id == 1U &&
              lifecycle.active.slot_id == 1U,
          "the first boot resolves to the known good slot");
    CHECK(appliance_update_lifecycle_resolve(&lifecycle, &io) ==
              APPLIANCE_UPDATE_LIFECYCLE_OK &&
              lifecycle.state == APPLIANCE_UPDATE_LIFECYCLE_STATE_IDLE &&
              lifecycle.active.slot_id == 1U &&
              !lifecycle.rollback_selected,
          "a resolve without a pending target keeps the known good slot");
    CHECK(appliance_update_lifecycle_active(&lifecycle) != NULL &&
              appliance_update_lifecycle_active(&lifecycle)->slot_id == 1U,
          "the active slot is published");

    CHECK(appliance_update_lifecycle_snapshot(&lifecycle, &snapshot) ==
              APPLIANCE_UPDATE_LIFECYCLE_OK,
          "the lifecycle snapshots");
    CHECK(snapshot.published &&
              snapshot.outcome == APPLIANCE_UPDATE_RECORD_OUTCOME_NONE &&
              snapshot.installed_image_id == 1U &&
              snapshot.active_slot_id == 1U && !snapshot.pending &&
              !snapshot.rollback_selected,
          "the snapshot carries the installed image and slot");
    CHECK(appliance_update_lifecycle_render(
              &snapshot, rendered, sizeof(rendered), &length) ==
              APPLIANCE_UPDATE_LIFECYCLE_OK &&
              strstr(rendered, "update_lifecycle v=1 state=IDLE") != NULL &&
              strstr(rendered, "image=1 slot=1") != NULL &&
              strstr(rendered, "rollback=NONE") != NULL,
          "the rendered record is exact");
    CHECK(strcmp(appliance_update_lifecycle_state_string(
                     APPLIANCE_UPDATE_LIFECYCLE_STATE_ROLLED_BACK),
                 "ROLLED_BACK") == 0 &&
              strcmp(appliance_update_lifecycle_status_string(
                         APPLIANCE_UPDATE_LIFECYCLE_DOWNGRADE_REFUSED),
                     "DOWNGRADE_REFUSED") == 0,
          "lifecycle strings are exact");
}

/* 2. A pending target is handed over and counted on every boot. */
static void host_handover_and_accept(void)
{
    struct host_store store = host_store();
    struct appliance_update_lifecycle_io io = host_io(&store);
    struct appliance_update_lifecycle lifecycle;
    struct appliance_update_record_entry known_good =
        host_entry(1U, 0x10U);
    struct appliance_update_record_entry target =
        host_entry(2U, 0x40U);

    CHECK(appliance_update_lifecycle_init(
              &lifecycle, 1U, &known_good, &io) ==
              APPLIANCE_UPDATE_LIFECYCLE_OK,
          "the handover fixture initialises");
    CHECK(appliance_update_record_begin_pending(
              &lifecycle.record, &target, &known_good, &io.record) ==
              APPLIANCE_UPDATE_RECORD_OK,
          "the management side arms a pending target");

    store.intact_slot_id = 2U;
    CHECK(appliance_update_lifecycle_resolve(&lifecycle, &io) ==
              APPLIANCE_UPDATE_LIFECYCLE_OK,
          "the pending target resolves");
    CHECK(lifecycle.state == APPLIANCE_UPDATE_LIFECYCLE_STATE_PENDING &&
              lifecycle.active.slot_id == 2U &&
              lifecycle.record.attempts == 1U &&
              lifecycle.slot_verified && !lifecycle.rollback_selected,
          "the target is handed over with a durable attempt count");

    CHECK(appliance_update_lifecycle_accept(&lifecycle, 2U, &io) ==
              APPLIANCE_UPDATE_LIFECYCLE_OK,
          "a confirmed target is accepted");
    CHECK(lifecycle.state == APPLIANCE_UPDATE_LIFECYCLE_STATE_ACCEPTED &&
              lifecycle.active.slot_id == 2U &&
              lifecycle.record.installed_image_id == 2U &&
              lifecycle.record.attempts == 0U &&
              !appliance_update_record_pending(&lifecycle.record),
          "acceptance promotes the target and advances the image floor");
    CHECK(lifecycle.accept_count == 1U,
          "the acceptance is counted");

    /* The promoted image is the new floor. */
    CHECK(appliance_update_lifecycle_admit(&lifecycle, 3U, 2U) ==
              APPLIANCE_UPDATE_LIFECYCLE_OK,
          "a newer compatible bundle is admitted");
    CHECK(appliance_update_lifecycle_admit(&lifecycle, 2U, 2U) ==
              APPLIANCE_UPDATE_LIFECYCLE_DOWNGRADE_REFUSED,
          "re-installing the installed image is refused");
    CHECK(appliance_update_lifecycle_admit(&lifecycle, 1U, 2U) ==
              APPLIANCE_UPDATE_LIFECYCLE_DOWNGRADE_REFUSED,
          "a lower target is refused as a downgrade");
    CHECK(appliance_update_lifecycle_admit(&lifecycle, 3U, 1U) ==
              APPLIANCE_UPDATE_LIFECYCLE_SOURCE_MISMATCH,
          "a bundle built for another image is refused");
}

/* 3. A failed target rolls back and keeps the known good slot. */
static void host_failure_rolls_back(void)
{
    struct host_store store = host_store();
    struct appliance_update_lifecycle_io io = host_io(&store);
    struct appliance_update_lifecycle lifecycle;
    struct appliance_update_lifecycle reloaded;
    struct appliance_update_record_entry known_good =
        host_entry(1U, 0x10U);
    struct appliance_update_record_entry target =
        host_entry(2U, 0x40U);

    CHECK(appliance_update_lifecycle_init(
              &lifecycle, 1U, &known_good, &io) ==
              APPLIANCE_UPDATE_LIFECYCLE_OK &&
              appliance_update_record_begin_pending(
                  &lifecycle.record, &target, &known_good, &io.record) ==
                  APPLIANCE_UPDATE_RECORD_OK,
          "the rollback fixture arms a pending target");
    store.intact_slot_id = 2U;
    CHECK(appliance_update_lifecycle_resolve(&lifecycle, &io) ==
              APPLIANCE_UPDATE_LIFECYCLE_OK &&
              appliance_update_lifecycle_fail(&lifecycle, &io) ==
              APPLIANCE_UPDATE_LIFECYCLE_OK,
          "a target that did not confirm is failed");
    CHECK(lifecycle.state ==
              APPLIANCE_UPDATE_LIFECYCLE_STATE_ROLLED_BACK &&
              lifecycle.rollback_selected &&
              lifecycle.active.slot_id == 1U &&
              lifecycle.record.installed_image_id == 1U &&
              !appliance_update_record_pending(&lifecycle.record),
          "rollback keeps the known good slot and the image floor");
    CHECK(lifecycle.rollback_count == 1U,
          "the rollback is counted");

    /* The rollback is durable: a fresh boot agrees. */
    CHECK(appliance_update_lifecycle_init(
              &reloaded, 1U, &known_good, &io) ==
              APPLIANCE_UPDATE_LIFECYCLE_OK,
          "the next boot reloads the record");
    CHECK(reloaded.record.outcome ==
              APPLIANCE_UPDATE_RECORD_OUTCOME_ROLLED_BACK &&
              reloaded.active.slot_id == 1U &&
              !appliance_update_record_pending(&reloaded.record),
          "the next boot resolves the rolled-back record");

    CHECK(appliance_update_lifecycle_accept(&lifecycle, 3U, &io) ==
              APPLIANCE_UPDATE_LIFECYCLE_NO_PENDING &&
              appliance_update_lifecycle_fail(&lifecycle, &io) ==
              APPLIANCE_UPDATE_LIFECYCLE_NO_PENDING,
          "settling without a pending target is refused");
    CHECK(lifecycle.reject_count == 2U,
          "both refusals are counted");
}

/* 4. A damaged slot and an exhausted budget both roll back. */
static void host_slot_damage_and_budget(void)
{
    struct host_store store = host_store();
    struct appliance_update_lifecycle_io io = host_io(&store);
    struct appliance_update_lifecycle lifecycle;
    struct appliance_update_record_entry known_good =
        host_entry(1U, 0x10U);
    struct appliance_update_record_entry target =
        host_entry(2U, 0x40U);
    uint32_t attempt;

    CHECK(appliance_update_lifecycle_init(
              &lifecycle, 1U, &known_good, &io) ==
              APPLIANCE_UPDATE_LIFECYCLE_OK,
          "the slot-damage fixture initialises");

    /* The candidate's bytes no longer match: refuse it immediately. */
    CHECK(appliance_update_record_begin_pending(
              &lifecycle.record, &target, &known_good, &io.record) ==
              APPLIANCE_UPDATE_RECORD_OK,
          "a pending target is armed");
    store.intact = false;
    CHECK(appliance_update_lifecycle_resolve(&lifecycle, &io) ==
              APPLIANCE_UPDATE_LIFECYCLE_OK,
          "a damaged candidate still resolves to a boot decision");
    CHECK(lifecycle.state ==
              APPLIANCE_UPDATE_LIFECYCLE_STATE_ROLLED_BACK &&
              lifecycle.active.slot_id == 1U && !lifecycle.slot_verified &&
              lifecycle.record.attempts == 0U,
          "a damaged candidate is never handed over");

    /* The attempt budget bounds a candidate that never confirms. */
    store = host_store();
    io = host_io(&store);
    store.intact_slot_id = 2U;
    CHECK(appliance_update_lifecycle_init(
              &lifecycle, 1U, &known_good, &io) ==
              APPLIANCE_UPDATE_LIFECYCLE_OK &&
              appliance_update_record_begin_pending(
                  &lifecycle.record, &target, &known_good, &io.record) ==
                  APPLIANCE_UPDATE_RECORD_OK,
          "the budget fixture arms a pending target");
    for (attempt = 1U;
         attempt <= APPLIANCE_UPDATE_RECORD_MAX_ATTEMPTS;
         ++attempt) {
        /* Simulate a power cut before the target confirms. */
        CHECK(appliance_update_lifecycle_init(
                  &lifecycle, 1U, &known_good, &io) ==
                  APPLIANCE_UPDATE_LIFECYCLE_OK &&
                  appliance_update_lifecycle_resolve(&lifecycle, &io) ==
                  APPLIANCE_UPDATE_LIFECYCLE_OK &&
                  lifecycle.active.slot_id == 2U,
              "each boot inside the budget hands the target over");
    }
    CHECK(lifecycle.record.attempts ==
              APPLIANCE_UPDATE_RECORD_MAX_ATTEMPTS,
          "the budget is consumed exactly");
    CHECK(appliance_update_lifecycle_init(
              &lifecycle, 1U, &known_good, &io) ==
              APPLIANCE_UPDATE_LIFECYCLE_OK &&
              appliance_update_lifecycle_resolve(&lifecycle, &io) ==
              APPLIANCE_UPDATE_LIFECYCLE_OK,
          "the boot after the exhausted budget resolves");
    CHECK(lifecycle.state ==
              APPLIANCE_UPDATE_LIFECYCLE_STATE_ROLLED_BACK &&
              lifecycle.active.slot_id == 1U && lifecycle.slot_verified &&
              !appliance_update_record_pending(&lifecycle.record),
          "an exhausted budget rolls back to the known good slot");
}

/* 5. A power cut during the update still boots. */
static void host_power_cut_still_boots(void)
{
    struct host_store store = host_store();
    struct appliance_update_lifecycle_io io = host_io(&store);
    struct appliance_update_lifecycle lifecycle;
    struct appliance_update_record_entry known_good =
        host_entry(1U, 0x10U);
    struct appliance_update_record_entry target =
        host_entry(2U, 0x40U);

    CHECK(appliance_update_lifecycle_init(
              &lifecycle, 1U, &known_good, &io) ==
              APPLIANCE_UPDATE_LIFECYCLE_OK,
          "the power-cut fixture initialises");
    CHECK(lifecycle.record.sequence == 1U &&
              lifecycle.active.slot_id == 1U,
          "the committed record is the first sequence");

    /* The power fails while the pending record is being committed. */
    store.torn_write_bytes = APPLIANCE_UPDATE_RECORD_BLOCK_SIZE / 2U;
    CHECK(appliance_update_record_begin_pending(
              &lifecycle.record, &target, &known_good, &io.record) ==
              APPLIANCE_UPDATE_RECORD_OK,
          "the interrupted update reports success from the caller view");

    /* The next boot must still resolve, on the committed record. */
    CHECK(appliance_update_lifecycle_init(
              &lifecycle, 1U, &known_good, &io) ==
              APPLIANCE_UPDATE_LIFECYCLE_OK,
          "the boot after the power cut initialises");
    CHECK(lifecycle.record.sequence == 1U &&
              !appliance_update_record_pending(&lifecycle.record),
          "the torn pending record never became the head");
    CHECK(appliance_update_lifecycle_resolve(&lifecycle, &io) ==
              APPLIANCE_UPDATE_LIFECYCLE_OK,
          "the boot after the power cut resolves");
    CHECK(lifecycle.state == APPLIANCE_UPDATE_LIFECYCLE_STATE_IDLE &&
              lifecycle.active.slot_id == 1U &&
              !lifecycle.rollback_selected &&
              lifecycle.record.installed_image_id == 1U,
          "the appliance still boots the known good slot");

    /* A failed flush never reports a settled update. */
    store = host_store();
    io = host_io(&store);
    store.intact_slot_id = 2U;
    CHECK(appliance_update_lifecycle_init(
              &lifecycle, 1U, &known_good, &io) ==
              APPLIANCE_UPDATE_LIFECYCLE_OK &&
              appliance_update_record_begin_pending(
                  &lifecycle.record, &target, &known_good, &io.record) ==
                  APPLIANCE_UPDATE_RECORD_OK,
          "the flush-fault fixture arms a pending target");
    store.drop_flush = true;
    CHECK(appliance_update_lifecycle_resolve(&lifecycle, &io) ==
              APPLIANCE_UPDATE_LIFECYCLE_RECORD_FAILED,
          "a resolve that cannot be made durable is refused");
    CHECK(lifecycle.state == APPLIANCE_UPDATE_LIFECYCLE_STATE_FAILED &&
              lifecycle.reject_count == 1U,
          "a failed durable resolve publishes FAILED, never a settled "
          "state");
}

/* 6. Null and malformed input never publishes a decision. */
static void host_rejects_bad_input(void)
{
    struct host_store store = host_store();
    struct appliance_update_lifecycle_io io = host_io(&store);
    struct appliance_update_lifecycle lifecycle;
    struct appliance_update_lifecycle_snapshot snapshot;
    struct appliance_update_record_entry known_good =
        host_entry(1U, 0x10U);
    struct appliance_update_lifecycle_io broken = io;
    char rendered[APPLIANCE_UPDATE_LIFECYCLE_RENDER_MAX];
    size_t length = 0U;

    CHECK(appliance_update_lifecycle_init(
              NULL, 1U, &known_good, &io) ==
              APPLIANCE_UPDATE_LIFECYCLE_NULL &&
              appliance_update_lifecycle_init(
                  &lifecycle, 1U, NULL, &io) ==
                  APPLIANCE_UPDATE_LIFECYCLE_NULL &&
              appliance_update_lifecycle_init(
                  &lifecycle, 1U, &known_good, NULL) ==
                  APPLIANCE_UPDATE_LIFECYCLE_NULL,
          "the lifecycle refuses null arguments");
    broken.slot_intact = NULL;
    CHECK(appliance_update_lifecycle_init(
              &lifecycle, 1U, &known_good, &broken) ==
              APPLIANCE_UPDATE_LIFECYCLE_NULL,
          "a lifecycle without a slot readback is refused");
    broken = io;
    broken.record.flush = NULL;
    CHECK(appliance_update_lifecycle_init(
              &lifecycle, 1U, &known_good, &broken) ==
              APPLIANCE_UPDATE_LIFECYCLE_NULL,
          "a lifecycle without a durable flush is refused");

    CHECK(appliance_update_lifecycle_resolve(NULL, &io) ==
              APPLIANCE_UPDATE_LIFECYCLE_NULL &&
              appliance_update_lifecycle_accept(
                  NULL, 2U, &io) ==
                  APPLIANCE_UPDATE_LIFECYCLE_NULL &&
              appliance_update_lifecycle_fail(NULL, &io) ==
                  APPLIANCE_UPDATE_LIFECYCLE_NULL &&
              appliance_update_lifecycle_admit(NULL, 2U, 1U) ==
                  APPLIANCE_UPDATE_LIFECYCLE_NULL,
          "the operations refuse a null lifecycle");
    CHECK(appliance_update_lifecycle_active(NULL) == NULL &&
              appliance_update_lifecycle_snapshot(NULL, &snapshot) ==
                  APPLIANCE_UPDATE_LIFECYCLE_NULL,
          "the inspectors refuse a null lifecycle");

    CHECK(appliance_update_lifecycle_init(
              &lifecycle, 1U, &known_good, &io) ==
              APPLIANCE_UPDATE_LIFECYCLE_OK &&
              appliance_update_lifecycle_snapshot(&lifecycle, &snapshot) ==
                  APPLIANCE_UPDATE_LIFECYCLE_OK,
          "the render fixture initialises");
    CHECK(appliance_update_lifecycle_render(
              &snapshot, rendered, 4U, &length) ==
              APPLIANCE_UPDATE_LIFECYCLE_BUFFER_TOO_SMALL &&
              appliance_update_lifecycle_render(
                  &snapshot, NULL, sizeof(rendered), &length) ==
                  APPLIANCE_UPDATE_LIFECYCLE_NULL,
          "a short or null render buffer is refused");
    snapshot.published = false;
    CHECK(appliance_update_lifecycle_render(
              &snapshot, rendered, sizeof(rendered), &length) ==
              APPLIANCE_UPDATE_LIFECYCLE_BAD_STATE,
          "an unpublished snapshot is refused");
}

int main(void)
{
    host_first_boot();
    host_handover_and_accept();
    host_failure_rolls_back();
    host_slot_damage_and_budget();
    host_power_cut_still_boots();
    host_rejects_bad_input();

    if (failures != 0) {
        fprintf(stderr,
                "Appliance update lifecycle host tests: %d FAILED\n",
                failures);
        return 1;
    }
    printf("Appliance update lifecycle host tests: ALL OK (%d checks)\n",
           checks);
    return 0;
}

#else
int appliance_update_lifecycle_host_translation_unit_marker;
#endif
