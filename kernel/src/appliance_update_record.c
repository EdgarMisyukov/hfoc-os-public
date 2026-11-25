#include "appliance_update_record.h"

#include "sha256.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/*
 * ITEM-376 durable update record.
 *
 * Two 512-byte copies with a monotonic sequence and a SHA-256 checksum.  A
 * commit writes the copy that is not the current head and only then flushes,
 * so an interrupted commit can never destroy the record that was already
 * committed: the loader skips an unreadable, torn or damaged copy and falls
 * back to the older one.
 */

#define APPLIANCE_UPDATE_RECORD_MAGIC_OFFSET 0U
#define APPLIANCE_UPDATE_RECORD_VERSION_OFFSET 8U
#define APPLIANCE_UPDATE_RECORD_SEQUENCE_OFFSET 12U
#define APPLIANCE_UPDATE_RECORD_LENGTH_OFFSET 16U
#define APPLIANCE_UPDATE_RECORD_CHECKSUM_OFFSET 32U

#define APPLIANCE_UPDATE_RECORD_BODY_VERSION_OFFSET 0U
#define APPLIANCE_UPDATE_RECORD_BODY_OUTCOME_OFFSET 4U
#define APPLIANCE_UPDATE_RECORD_BODY_IMAGE_OFFSET 8U
#define APPLIANCE_UPDATE_RECORD_BODY_ATTEMPTS_OFFSET 12U
#define APPLIANCE_UPDATE_RECORD_BODY_KNOWN_GOOD_OFFSET 16U
#define APPLIANCE_UPDATE_RECORD_BODY_PENDING_OFFSET 56U
#define APPLIANCE_UPDATE_RECORD_BODY_ROLLBACK_OFFSET 96U
#define APPLIANCE_UPDATE_RECORD_BODY_SIZE 136U

#define APPLIANCE_UPDATE_RECORD_ENTRY_SIZE 40U

static const uint8_t appliance_update_record_magic[
    APPLIANCE_UPDATE_RECORD_MAGIC_SIZE] =
    APPLIANCE_UPDATE_RECORD_MAGIC_BYTES;

static void appliance_update_record_zero(void *destination, size_t length)
{
    unsigned char *bytes = destination;
    size_t index;

    for (index = 0U; index < length; ++index) {
        bytes[index] = 0U;
    }
}

static void appliance_update_record_store_le32(uint8_t *destination,
                                               uint32_t value)
{
    destination[0] = (uint8_t)(value & 0xFFU);
    destination[1] = (uint8_t)((value >> 8U) & 0xFFU);
    destination[2] = (uint8_t)((value >> 16U) & 0xFFU);
    destination[3] = (uint8_t)((value >> 24U) & 0xFFU);
}

static uint32_t appliance_update_record_load_le32(const uint8_t *source)
{
    return (uint32_t)source[0] |
           ((uint32_t)source[1] << 8U) |
           ((uint32_t)source[2] << 16U) |
           ((uint32_t)source[3] << 24U);
}

static bool appliance_update_record_bytes_equal(const uint8_t *left,
                                                const uint8_t *right,
                                                size_t length)
{
    size_t index;

    for (index = 0U; index < length; ++index) {
        if (left[index] != right[index]) {
            return false;
        }
    }
    return true;
}

static bool appliance_update_record_outcome_valid(
    enum appliance_update_record_outcome outcome)
{
    return outcome >= APPLIANCE_UPDATE_RECORD_OUTCOME_NONE &&
           outcome <= APPLIANCE_UPDATE_RECORD_OUTCOME_ROLLED_BACK;
}

static bool appliance_update_record_io_valid(
    const struct appliance_update_record_io *io)
{
    return io != NULL && io->read != NULL && io->write != NULL &&
           io->flush != NULL;
}

/*
 * The checksum covers the whole block except the checksum field itself, so a
 * torn write anywhere -- header, body or tail -- never validates and the
 * loader falls back to the untouched copy.
 */
static enum appliance_update_record_status appliance_update_record_checksum(
    const uint8_t block[APPLIANCE_UPDATE_RECORD_BLOCK_SIZE],
    uint8_t digest[APPLIANCE_UPDATE_RECORD_DIGEST_SIZE])
{
    struct sha256 context;

    if (sha256_init(&context) != SHA256_OK ||
        sha256_update(&context, block,
                      APPLIANCE_UPDATE_RECORD_CHECKSUM_OFFSET) !=
            SHA256_OK ||
        sha256_update(
            &context,
            block + APPLIANCE_UPDATE_RECORD_BODY_OFFSET,
            APPLIANCE_UPDATE_RECORD_BLOCK_SIZE -
                APPLIANCE_UPDATE_RECORD_BODY_OFFSET) != SHA256_OK ||
        sha256_final(&context, digest) != SHA256_OK) {
        return APPLIANCE_UPDATE_RECORD_CORRUPT;
    }
    return APPLIANCE_UPDATE_RECORD_OK;
}

static void appliance_update_record_encode_entry(
    uint8_t *destination,
    const struct appliance_update_record_entry *entry)
{
    appliance_update_record_store_le32(destination, entry->slot_id);
    appliance_update_record_store_le32(destination + 4U, entry->generation);
    memcpy(destination + 8U, entry->digest,
           APPLIANCE_UPDATE_RECORD_DIGEST_SIZE);
}

static void appliance_update_record_decode_entry(
    const uint8_t *source,
    struct appliance_update_record_entry *entry)
{
    entry->slot_id = appliance_update_record_load_le32(source);
    entry->generation = appliance_update_record_load_le32(source + 4U);
    memcpy(entry->digest, source + 8U,
           APPLIANCE_UPDATE_RECORD_DIGEST_SIZE);
}

static uint32_t appliance_update_record_encode(
    const struct appliance_update_record *record,
    uint8_t block[APPLIANCE_UPDATE_RECORD_BLOCK_SIZE])
{
    uint8_t *body = block + APPLIANCE_UPDATE_RECORD_BODY_OFFSET;

    appliance_update_record_zero(block,
                                 APPLIANCE_UPDATE_RECORD_BLOCK_SIZE);
    memcpy(block + APPLIANCE_UPDATE_RECORD_MAGIC_OFFSET,
           appliance_update_record_magic,
           APPLIANCE_UPDATE_RECORD_MAGIC_SIZE);
    appliance_update_record_store_le32(
        block + APPLIANCE_UPDATE_RECORD_VERSION_OFFSET,
        APPLIANCE_UPDATE_RECORD_VERSION);
    appliance_update_record_store_le32(
        block + APPLIANCE_UPDATE_RECORD_SEQUENCE_OFFSET,
        record->sequence);
    appliance_update_record_store_le32(
        block + APPLIANCE_UPDATE_RECORD_LENGTH_OFFSET,
        APPLIANCE_UPDATE_RECORD_BODY_SIZE);

    appliance_update_record_store_le32(
        body + APPLIANCE_UPDATE_RECORD_BODY_VERSION_OFFSET,
        record->version);
    appliance_update_record_store_le32(
        body + APPLIANCE_UPDATE_RECORD_BODY_OUTCOME_OFFSET,
        (uint32_t)record->outcome);
    appliance_update_record_store_le32(
        body + APPLIANCE_UPDATE_RECORD_BODY_IMAGE_OFFSET,
        record->installed_image_id);
    appliance_update_record_store_le32(
        body + APPLIANCE_UPDATE_RECORD_BODY_ATTEMPTS_OFFSET,
        record->attempts);
    appliance_update_record_encode_entry(
        body + APPLIANCE_UPDATE_RECORD_BODY_KNOWN_GOOD_OFFSET,
        &record->known_good);
    appliance_update_record_encode_entry(
        body + APPLIANCE_UPDATE_RECORD_BODY_PENDING_OFFSET,
        &record->pending);
    appliance_update_record_encode_entry(
        body + APPLIANCE_UPDATE_RECORD_BODY_ROLLBACK_OFFSET,
        &record->rollback);
    return APPLIANCE_UPDATE_RECORD_BODY_SIZE;
}

static enum appliance_update_record_status appliance_update_record_decode(
    const uint8_t block[APPLIANCE_UPDATE_RECORD_BLOCK_SIZE],
    struct appliance_update_record *record)
{
    const uint8_t *body = block + APPLIANCE_UPDATE_RECORD_BODY_OFFSET;
    enum appliance_update_record_status status;
    uint8_t expected[APPLIANCE_UPDATE_RECORD_DIGEST_SIZE];
    uint32_t body_length;

    if (!appliance_update_record_bytes_equal(
            block + APPLIANCE_UPDATE_RECORD_MAGIC_OFFSET,
            appliance_update_record_magic,
            APPLIANCE_UPDATE_RECORD_MAGIC_SIZE)) {
        return APPLIANCE_UPDATE_RECORD_CORRUPT;
    }
    if (appliance_update_record_load_le32(
            block + APPLIANCE_UPDATE_RECORD_VERSION_OFFSET) !=
        APPLIANCE_UPDATE_RECORD_VERSION) {
        return APPLIANCE_UPDATE_RECORD_BAD_VERSION;
    }
    body_length = appliance_update_record_load_le32(
        block + APPLIANCE_UPDATE_RECORD_LENGTH_OFFSET);
    if (body_length != APPLIANCE_UPDATE_RECORD_BODY_SIZE) {
        return APPLIANCE_UPDATE_RECORD_CORRUPT;
    }
    status = appliance_update_record_checksum(block, expected);
    if (status != APPLIANCE_UPDATE_RECORD_OK) {
        return status;
    }
    if (!appliance_update_record_bytes_equal(
            expected,
            block + APPLIANCE_UPDATE_RECORD_CHECKSUM_OFFSET,
            APPLIANCE_UPDATE_RECORD_DIGEST_SIZE)) {
        return APPLIANCE_UPDATE_RECORD_CORRUPT;
    }
    if (appliance_update_record_load_le32(
            body + APPLIANCE_UPDATE_RECORD_BODY_VERSION_OFFSET) !=
        APPLIANCE_UPDATE_RECORD_VERSION) {
        return APPLIANCE_UPDATE_RECORD_BAD_VERSION;
    }

    record->version = APPLIANCE_UPDATE_RECORD_VERSION;
    record->sequence = appliance_update_record_load_le32(
        block + APPLIANCE_UPDATE_RECORD_SEQUENCE_OFFSET);
    record->outcome = (enum appliance_update_record_outcome)
        appliance_update_record_load_le32(
            body + APPLIANCE_UPDATE_RECORD_BODY_OUTCOME_OFFSET);
    record->installed_image_id = appliance_update_record_load_le32(
        body + APPLIANCE_UPDATE_RECORD_BODY_IMAGE_OFFSET);
    record->attempts = appliance_update_record_load_le32(
        body + APPLIANCE_UPDATE_RECORD_BODY_ATTEMPTS_OFFSET);
    appliance_update_record_decode_entry(
        body + APPLIANCE_UPDATE_RECORD_BODY_KNOWN_GOOD_OFFSET,
        &record->known_good);
    appliance_update_record_decode_entry(
        body + APPLIANCE_UPDATE_RECORD_BODY_PENDING_OFFSET,
        &record->pending);
    appliance_update_record_decode_entry(
        body + APPLIANCE_UPDATE_RECORD_BODY_ROLLBACK_OFFSET,
        &record->rollback);

    if (!appliance_update_record_outcome_valid(record->outcome)) {
        return APPLIANCE_UPDATE_RECORD_CORRUPT;
    }
    if (record->outcome != APPLIANCE_UPDATE_RECORD_OUTCOME_PENDING &&
        record->pending.slot_id != 0U) {
        return APPLIANCE_UPDATE_RECORD_CORRUPT;
    }
    if (record->outcome == APPLIANCE_UPDATE_RECORD_OUTCOME_PENDING &&
        record->pending.slot_id == 0U) {
        return APPLIANCE_UPDATE_RECORD_CORRUPT;
    }
    return APPLIANCE_UPDATE_RECORD_OK;
}

/*
 * Commit the record into the copy that is not the current head and flush it.
 * The in-memory sequence is only advanced once the commit succeeded, so the
 * caller never observes a state the device did not accept.
 */
static enum appliance_update_record_status appliance_update_record_commit(
    struct appliance_update_record *record,
    const struct appliance_update_record_io *io)
{
    uint8_t block[APPLIANCE_UPDATE_RECORD_BLOCK_SIZE];
    uint8_t digest[APPLIANCE_UPDATE_RECORD_DIGEST_SIZE];
    uint32_t copy;
    uint32_t previous_sequence = record->sequence;

    copy = record->sequence == 0U
               ? 0U
               : (record->head_copy + 1U) %
                     APPLIANCE_UPDATE_RECORD_COPY_COUNT;
    record->sequence = previous_sequence + 1U;
    (void)appliance_update_record_encode(record, block);
    if (appliance_update_record_checksum(block, digest) !=
        APPLIANCE_UPDATE_RECORD_OK) {
        record->sequence = previous_sequence;
        return APPLIANCE_UPDATE_RECORD_WRITE_FAULT;
    }
    memcpy(block + APPLIANCE_UPDATE_RECORD_CHECKSUM_OFFSET, digest,
           APPLIANCE_UPDATE_RECORD_DIGEST_SIZE);
    if (!io->write(io->context, copy, block,
                   APPLIANCE_UPDATE_RECORD_BLOCK_SIZE) ||
        !io->flush(io->context)) {
        record->sequence = previous_sequence;
        return APPLIANCE_UPDATE_RECORD_WRITE_FAULT;
    }
    record->head_copy = copy;
    return APPLIANCE_UPDATE_RECORD_OK;
}

enum appliance_update_record_status appliance_update_record_load(
    struct appliance_update_record *record,
    const struct appliance_update_record_io *io)
{
    struct appliance_update_record candidate;
    enum appliance_update_record_status status;
    uint8_t block[APPLIANCE_UPDATE_RECORD_BLOCK_SIZE];
    uint32_t copy;
    uint32_t head_copy = 0U;
    bool found = false;

    if (record == NULL) {
        return APPLIANCE_UPDATE_RECORD_NULL;
    }
    if (!appliance_update_record_io_valid(io)) {
        return APPLIANCE_UPDATE_RECORD_NULL;
    }
    for (copy = 0U; copy < APPLIANCE_UPDATE_RECORD_COPY_COUNT; ++copy) {
        if (!io->read(io->context, copy, block,
                      APPLIANCE_UPDATE_RECORD_BLOCK_SIZE)) {
            continue;
        }
        status = appliance_update_record_decode(block, &candidate);
        if (status == APPLIANCE_UPDATE_RECORD_BAD_VERSION) {
            return status;
        }
        if (status != APPLIANCE_UPDATE_RECORD_OK) {
            continue;
        }
        if (!found || candidate.sequence > record->sequence) {
            *record = candidate;
            head_copy = copy;
            found = true;
        }
    }
    if (!found) {
        return APPLIANCE_UPDATE_RECORD_NO_RECORD;
    }
    record->head_copy = head_copy;
    return APPLIANCE_UPDATE_RECORD_OK;
}

enum appliance_update_record_status appliance_update_record_init(
    struct appliance_update_record *record,
    uint32_t installed_image_id,
    const struct appliance_update_record_entry *known_good,
    const struct appliance_update_record_io *io)
{
    struct appliance_update_record existing;
    enum appliance_update_record_status status;

    if (record == NULL || known_good == NULL) {
        return APPLIANCE_UPDATE_RECORD_NULL;
    }
    if (!appliance_update_record_io_valid(io)) {
        return APPLIANCE_UPDATE_RECORD_NULL;
    }
    status = appliance_update_record_load(&existing, io);
    if (status == APPLIANCE_UPDATE_RECORD_OK) {
        return APPLIANCE_UPDATE_RECORD_BAD_STATE;
    }
    if (status != APPLIANCE_UPDATE_RECORD_NO_RECORD) {
        return status;
    }

    appliance_update_record_zero(record, sizeof(*record));
    record->version = APPLIANCE_UPDATE_RECORD_VERSION;
    record->outcome = APPLIANCE_UPDATE_RECORD_OUTCOME_NONE;
    record->installed_image_id = installed_image_id;
    record->known_good = *known_good;
    return appliance_update_record_commit(record, io);
}

enum appliance_update_record_status appliance_update_record_begin_pending(
    struct appliance_update_record *record,
    const struct appliance_update_record_entry *target,
    const struct appliance_update_record_entry *rollback,
    const struct appliance_update_record_io *io)
{
    if (record == NULL || target == NULL || rollback == NULL) {
        return APPLIANCE_UPDATE_RECORD_NULL;
    }
    if (!appliance_update_record_io_valid(io)) {
        return APPLIANCE_UPDATE_RECORD_NULL;
    }
    if (record->version != APPLIANCE_UPDATE_RECORD_VERSION ||
        record->outcome == APPLIANCE_UPDATE_RECORD_OUTCOME_PENDING) {
        return APPLIANCE_UPDATE_RECORD_BAD_STATE;
    }
    record->outcome = APPLIANCE_UPDATE_RECORD_OUTCOME_PENDING;
    record->pending = *target;
    record->rollback = *rollback;
    record->attempts = 0U;
    return appliance_update_record_commit(record, io);
}

enum appliance_update_record_status appliance_update_record_note_attempt(
    struct appliance_update_record *record,
    const struct appliance_update_record_io *io)
{
    if (record == NULL) {
        return APPLIANCE_UPDATE_RECORD_NULL;
    }
    if (!appliance_update_record_io_valid(io)) {
        return APPLIANCE_UPDATE_RECORD_NULL;
    }
    if (record->version != APPLIANCE_UPDATE_RECORD_VERSION ||
        record->outcome != APPLIANCE_UPDATE_RECORD_OUTCOME_PENDING) {
        return APPLIANCE_UPDATE_RECORD_NO_PENDING;
    }
    record->attempts += 1U;
    return appliance_update_record_commit(record, io);
}

enum appliance_update_record_status appliance_update_record_accept(
    struct appliance_update_record *record,
    uint32_t accepted_image_id,
    const struct appliance_update_record_io *io)
{
    if (record == NULL) {
        return APPLIANCE_UPDATE_RECORD_NULL;
    }
    if (!appliance_update_record_io_valid(io)) {
        return APPLIANCE_UPDATE_RECORD_NULL;
    }
    if (record->version != APPLIANCE_UPDATE_RECORD_VERSION ||
        record->outcome != APPLIANCE_UPDATE_RECORD_OUTCOME_PENDING) {
        return APPLIANCE_UPDATE_RECORD_NO_PENDING;
    }
    record->known_good = record->pending;
    appliance_update_record_zero(&record->pending,
                                 sizeof(record->pending));
    appliance_update_record_zero(&record->rollback,
                                 sizeof(record->rollback));
    record->attempts = 0U;
    record->installed_image_id = accepted_image_id;
    record->outcome = APPLIANCE_UPDATE_RECORD_OUTCOME_ACCEPTED;
    return appliance_update_record_commit(record, io);
}

enum appliance_update_record_status appliance_update_record_roll_back(
    struct appliance_update_record *record,
    const struct appliance_update_record_io *io)
{
    if (record == NULL) {
        return APPLIANCE_UPDATE_RECORD_NULL;
    }
    if (!appliance_update_record_io_valid(io)) {
        return APPLIANCE_UPDATE_RECORD_NULL;
    }
    if (record->version != APPLIANCE_UPDATE_RECORD_VERSION ||
        record->outcome != APPLIANCE_UPDATE_RECORD_OUTCOME_PENDING) {
        return APPLIANCE_UPDATE_RECORD_NO_PENDING;
    }
    appliance_update_record_zero(&record->pending,
                                 sizeof(record->pending));
    record->attempts = 0U;
    record->outcome = APPLIANCE_UPDATE_RECORD_OUTCOME_ROLLED_BACK;
    return appliance_update_record_commit(record, io);
}

enum appliance_update_record_status appliance_update_record_check_upgrade(
    const struct appliance_update_record *record,
    uint32_t target_image_id,
    uint32_t source_image_id)
{
    if (record == NULL) {
        return APPLIANCE_UPDATE_RECORD_NULL;
    }
    if (record->version != APPLIANCE_UPDATE_RECORD_VERSION) {
        return APPLIANCE_UPDATE_RECORD_BAD_VERSION;
    }
    if (target_image_id <= record->installed_image_id) {
        return APPLIANCE_UPDATE_RECORD_DOWNGRADE_REFUSED;
    }
    if (source_image_id != record->installed_image_id) {
        return APPLIANCE_UPDATE_RECORD_SOURCE_MISMATCH;
    }
    return APPLIANCE_UPDATE_RECORD_OK;
}

bool appliance_update_record_pending(
    const struct appliance_update_record *record)
{
    return record != NULL &&
           record->version == APPLIANCE_UPDATE_RECORD_VERSION &&
           record->outcome == APPLIANCE_UPDATE_RECORD_OUTCOME_PENDING &&
           record->pending.slot_id != 0U;
}

bool appliance_update_record_attempts_exhausted(
    const struct appliance_update_record *record)
{
    return appliance_update_record_pending(record) &&
           record->attempts >= APPLIANCE_UPDATE_RECORD_MAX_ATTEMPTS;
}

const struct appliance_update_record_entry *
appliance_update_record_rollback_source(
    const struct appliance_update_record *record)
{
    if (!appliance_update_record_pending(record)) {
        return NULL;
    }
    return &record->rollback;
}

const char *appliance_update_record_outcome_string(
    enum appliance_update_record_outcome outcome)
{
    switch (outcome) {
    case APPLIANCE_UPDATE_RECORD_OUTCOME_NONE:
        return "NONE";
    case APPLIANCE_UPDATE_RECORD_OUTCOME_PENDING:
        return "PENDING";
    case APPLIANCE_UPDATE_RECORD_OUTCOME_ACCEPTED:
        return "ACCEPTED";
    case APPLIANCE_UPDATE_RECORD_OUTCOME_ROLLED_BACK:
        return "ROLLED_BACK";
    default:
        return NULL;
    }
}

const char *appliance_update_record_status_string(
    enum appliance_update_record_status status)
{
    switch (status) {
    case APPLIANCE_UPDATE_RECORD_OK:
        return "OK";
    case APPLIANCE_UPDATE_RECORD_NULL:
        return "NULL";
    case APPLIANCE_UPDATE_RECORD_BAD_STATE:
        return "BAD_STATE";
    case APPLIANCE_UPDATE_RECORD_BAD_VERSION:
        return "BAD_VERSION";
    case APPLIANCE_UPDATE_RECORD_NO_RECORD:
        return "NO_RECORD";
    case APPLIANCE_UPDATE_RECORD_IO_ERROR:
        return "IO_ERROR";
    case APPLIANCE_UPDATE_RECORD_CORRUPT:
        return "CORRUPT";
    case APPLIANCE_UPDATE_RECORD_WRITE_FAULT:
        return "WRITE_FAULT";
    case APPLIANCE_UPDATE_RECORD_DOWNGRADE_REFUSED:
        return "DOWNGRADE_REFUSED";
    case APPLIANCE_UPDATE_RECORD_SOURCE_MISMATCH:
        return "SOURCE_MISMATCH";
    case APPLIANCE_UPDATE_RECORD_NO_PENDING:
        return "NO_PENDING";
    default:
        return NULL;
    }
}
