#ifndef HFOC_APPLIANCE_UPDATE_RECORD_H
#define HFOC_APPLIANCE_UPDATE_RECORD_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * ITEM-376 durable update record.
 *
 * The candidate/rollback stack already existed but lived entirely in RAM, so
 * nothing about an update survived a reboot.  This module owns the durable
 * record that makes the update protocol atomic across a power cut:
 *
 *   - two independent 512-byte copies with a monotonic sequence number and a
 *     SHA-256 checksum over the header and body.  A store writes the copy that
 *     is not the current head and only then flushes, so an interrupted write
 *     can never invalidate the record that was already committed: the loader
 *     ignores an unreadable or damaged copy and falls back to the older one;
 *   - the record carries the installed image id (the downgrade floor), the
 *     attempt counter, the known-good slot, the pending target and the
 *     rollback source, so a boot can decide without any other state;
 *   - a downgrade is refused by an explicit ordered comparison, and a bundle
 *     is refused when it was produced for a different installed image.
 *
 * The module is allocation-free and does not touch a block device directly:
 * the caller supplies bounded copy reads, copy writes and a flush, which keeps
 * the record host-testable and lets production bind it to a raw block range or
 * to a sealed state store.
 */

#define APPLIANCE_UPDATE_RECORD_VERSION 1U
#define APPLIANCE_UPDATE_RECORD_BLOCK_SIZE 512U
#define APPLIANCE_UPDATE_RECORD_MAGIC_SIZE 8U
#define APPLIANCE_UPDATE_RECORD_DIGEST_SIZE 32U
#define APPLIANCE_UPDATE_RECORD_COPY_COUNT 2U
#define APPLIANCE_UPDATE_RECORD_BODY_OFFSET 64U
#define APPLIANCE_UPDATE_RECORD_BODY_MAX \
    (APPLIANCE_UPDATE_RECORD_BLOCK_SIZE - APPLIANCE_UPDATE_RECORD_BODY_OFFSET)
#define APPLIANCE_UPDATE_RECORD_MAX_ATTEMPTS 3U
#define APPLIANCE_UPDATE_RECORD_MAGIC_BYTES \
    { 'H', 'F', 'O', 'C', 'U', 'P', 'D', 'T' }

enum appliance_update_record_outcome {
    APPLIANCE_UPDATE_RECORD_OUTCOME_NONE = 0,
    APPLIANCE_UPDATE_RECORD_OUTCOME_PENDING = 1,
    APPLIANCE_UPDATE_RECORD_OUTCOME_ACCEPTED = 2,
    APPLIANCE_UPDATE_RECORD_OUTCOME_ROLLED_BACK = 3,
};

enum appliance_update_record_status {
    APPLIANCE_UPDATE_RECORD_OK = 0,
    APPLIANCE_UPDATE_RECORD_NULL = 1,
    APPLIANCE_UPDATE_RECORD_BAD_STATE = 2,
    APPLIANCE_UPDATE_RECORD_BAD_VERSION = 3,
    APPLIANCE_UPDATE_RECORD_NO_RECORD = 4,
    APPLIANCE_UPDATE_RECORD_IO_ERROR = 5,
    APPLIANCE_UPDATE_RECORD_CORRUPT = 6,
    APPLIANCE_UPDATE_RECORD_WRITE_FAULT = 7,
    APPLIANCE_UPDATE_RECORD_DOWNGRADE_REFUSED = 8,
    APPLIANCE_UPDATE_RECORD_SOURCE_MISMATCH = 9,
    APPLIANCE_UPDATE_RECORD_NO_PENDING = 10,
};

struct appliance_update_record_entry {
    uint32_t slot_id;
    uint32_t generation;
    uint8_t digest[APPLIANCE_UPDATE_RECORD_DIGEST_SIZE];
};

struct appliance_update_record {
    uint32_t version;
    uint32_t sequence;
    /* Which of the two copies holds this record; a commit alternates. */
    uint32_t head_copy;
    enum appliance_update_record_outcome outcome;
    uint32_t installed_image_id;
    uint32_t attempts;
    struct appliance_update_record_entry known_good;
    struct appliance_update_record_entry pending;
    struct appliance_update_record_entry rollback;
};

typedef bool (*appliance_update_record_read_fn)(
    void *context,
    uint32_t copy,
    uint8_t *block,
    size_t length);
typedef bool (*appliance_update_record_write_fn)(
    void *context,
    uint32_t copy,
    const uint8_t *block,
    size_t length);
typedef bool (*appliance_update_record_flush_fn)(void *context);

struct appliance_update_record_io {
    void *context;
    appliance_update_record_read_fn read;
    appliance_update_record_write_fn write;
    appliance_update_record_flush_fn flush;
};

/* First boot: publish the initial record only when no valid copy exists. */
enum appliance_update_record_status appliance_update_record_init(
    struct appliance_update_record *record,
    uint32_t installed_image_id,
    const struct appliance_update_record_entry *known_good,
    const struct appliance_update_record_io *io);
/* Read the newest intact copy; a damaged or torn copy is never trusted. */
enum appliance_update_record_status appliance_update_record_load(
    struct appliance_update_record *record,
    const struct appliance_update_record_io *io);
/* Arm a pending target with its rollback source, counted from this boot. */
enum appliance_update_record_status appliance_update_record_begin_pending(
    struct appliance_update_record *record,
    const struct appliance_update_record_entry *target,
    const struct appliance_update_record_entry *rollback,
    const struct appliance_update_record_io *io);
/* Count one boot of the pending target before it is handed control. */
enum appliance_update_record_status appliance_update_record_note_attempt(
    struct appliance_update_record *record,
    const struct appliance_update_record_io *io);
/* The pending target confirmed: promote it and advance the downgrade floor. */
enum appliance_update_record_status appliance_update_record_accept(
    struct appliance_update_record *record,
    uint32_t accepted_image_id,
    const struct appliance_update_record_io *io);
/* The pending target failed: drop it, keep the known-good slot. */
enum appliance_update_record_status appliance_update_record_roll_back(
    struct appliance_update_record *record,
    const struct appliance_update_record_io *io);
/* Ordered admission: a lower or equal target is a downgrade. */
enum appliance_update_record_status appliance_update_record_check_upgrade(
    const struct appliance_update_record *record,
    uint32_t target_image_id,
    uint32_t source_image_id);
bool appliance_update_record_pending(
    const struct appliance_update_record *record);
bool appliance_update_record_attempts_exhausted(
    const struct appliance_update_record *record);
const struct appliance_update_record_entry *
appliance_update_record_rollback_source(
    const struct appliance_update_record *record);
const char *appliance_update_record_outcome_string(
    enum appliance_update_record_outcome outcome);
const char *appliance_update_record_status_string(
    enum appliance_update_record_status status);

#endif
