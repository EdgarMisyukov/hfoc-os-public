#include "config_journal.h"

#include "sha256.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

static const unsigned char config_journal_magic[CONFIG_JOURNAL_MAGIC_SIZE] =
    CONFIG_JOURNAL_MAGIC_BYTES;
static const unsigned char config_journal_commit_magic[
    CONFIG_JOURNAL_COMMIT_MAGIC_SIZE] =
    CONFIG_JOURNAL_COMMIT_MAGIC_BYTES;

static void config_journal_zero_bytes(unsigned char *destination,
                                      size_t length)
{
    size_t index;

    for (index = 0U; index < length; ++index) {
        destination[index] = 0U;
    }
}

static void config_journal_copy_bytes(unsigned char *destination,
                                      const unsigned char *source,
                                      size_t length)
{
    size_t index;

    for (index = 0U; index < length; ++index) {
        destination[index] = source[index];
    }
}

static bool config_journal_bytes_equal(const unsigned char *left,
                                       const unsigned char *right,
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

static uint64_t config_journal_load_le64(const unsigned char *bytes) {
    uint64_t value = 0U;
    size_t index;

    for (index = 0U; index < 8U; ++index) {
        value |= (uint64_t)bytes[index] << (8U * index);
    }
    return value;
}

static uint32_t config_journal_load_le32(const unsigned char *bytes) {
    return (uint32_t)bytes[0] |
           ((uint32_t)bytes[1] << 8U) |
           ((uint32_t)bytes[2] << 16U) |
           ((uint32_t)bytes[3] << 24U);
}

static void config_journal_store_le64(unsigned char *bytes, uint64_t value) {
    size_t index;

    for (index = 0U; index < 8U; ++index) {
        bytes[index] = (unsigned char)(value >> (8U * index));
    }
}

static void config_journal_store_le32(unsigned char *bytes, uint32_t value) {
    bytes[0] = (unsigned char)(value);
    bytes[1] = (unsigned char)(value >> 8U);
    bytes[2] = (unsigned char)(value >> 16U);
    bytes[3] = (unsigned char)(value >> 24U);
}

static bool config_journal_zero_range(const unsigned char *bytes,
                                      size_t offset,
                                      size_t size)
{
    size_t index;

    for (index = 0U; index < size; ++index) {
        if (bytes[offset + index] != 0U) {
            return false;
        }
    }
    return true;
}

static bool config_journal_magic_valid(const unsigned char *block) {
    size_t index;

    for (index = 0U; index < CONFIG_JOURNAL_MAGIC_SIZE; ++index) {
        if (block[index] != config_journal_magic[index]) {
            return false;
        }
    }
    return true;
}

static bool config_journal_commit_marker_valid(const unsigned char *block,
                                               uint64_t sequence)
{
    size_t index;

    for (index = 0U; index < CONFIG_JOURNAL_COMMIT_MAGIC_SIZE; ++index) {
        if (block[CONFIG_JOURNAL_COMMIT_OFFSET + index] !=
            config_journal_commit_magic[index]) {
            return false;
        }
    }
    if (config_journal_load_le64(
            block + CONFIG_JOURNAL_COMMIT_SEQUENCE_OFFSET) != sequence) {
        return false;
    }
    return config_journal_zero_range(
        block,
        CONFIG_JOURNAL_COMMIT_SEQUENCE_OFFSET +
            CONFIG_JOURNAL_COMMIT_SEQUENCE_SIZE,
        CONFIG_JOURNAL_COMMIT_SIZE -
            CONFIG_JOURNAL_COMMIT_MAGIC_SIZE -
            CONFIG_JOURNAL_COMMIT_SEQUENCE_SIZE);
}

static bool config_journal_block_all_zero(const unsigned char *block) {
    return config_journal_zero_range(block, 0U, CONFIG_JOURNAL_BLOCK_SIZE);
}

static bool config_journal_record_valid(
    const unsigned char *block,
    uint64_t *sequence,
    uint32_t *data_length,
    const unsigned char **data)
{
    uint8_t digest[SHA256_DIGEST_SIZE];
    struct sha256 context;
    uint64_t candidate_sequence;
    uint32_t candidate_length;

    if (!config_journal_magic_valid(block) ||
        block[8U] != CONFIG_JOURNAL_FORMAT_VERSION ||
        !config_journal_zero_range(block, 9U, 7U) ||
        !config_journal_zero_range(block, 28U, 4U)) {
        return false;
    }

    candidate_sequence = config_journal_load_le64(block + 16U);
    candidate_length = config_journal_load_le32(block + 24U);
    if (candidate_sequence == 0U ||
        candidate_length == 0U ||
        candidate_length > CONFIG_JOURNAL_DATA_MAX ||
        !config_journal_commit_marker_valid(block, candidate_sequence)) {
        return false;
    }

    if (sha256_init(&context) != SHA256_OK ||
        sha256_update(&context, block, 32U) != SHA256_OK ||
        sha256_update(&context, block + CONFIG_JOURNAL_DATA_OFFSET,
                      candidate_length) != SHA256_OK ||
        sha256_update(&context, block + CONFIG_JOURNAL_COMMIT_OFFSET,
                      CONFIG_JOURNAL_COMMIT_SIZE) != SHA256_OK ||
        sha256_final(&context, digest) != SHA256_OK) {
        return false;
    }
    if (!config_journal_bytes_equal(
            digest, block + CONFIG_JOURNAL_CHECKSUM_OFFSET,
            SHA256_DIGEST_SIZE)) {
        return false;
    }

    *sequence = candidate_sequence;
    *data_length = candidate_length;
    *data = block + CONFIG_JOURNAL_DATA_OFFSET;
    return true;
}

static uint64_t config_journal_block_offset(
    const struct config_journal *journal,
    uint64_t slot)
{
    return (journal->block_offset + slot) * CONFIG_JOURNAL_BLOCK_SIZE;
}

enum config_journal_status config_journal_init(
    struct config_journal *journal,
    struct block_device *device,
    uint64_t block_offset,
    uint64_t block_count)
{
    enum block_device_status io_status;
    unsigned char block[CONFIG_JOURNAL_BLOCK_SIZE];
    bool found = false;
    uint64_t head_block = 0U;
    uint64_t head_sequence = 0U;
    uint64_t slot;

    if (journal == NULL || device == NULL) {
        return CONFIG_JOURNAL_NULL;
    }
    if (block_device_validate(device) != BLOCK_DEVICE_OK ||
        device->block_size != CONFIG_JOURNAL_BLOCK_SIZE) {
        return CONFIG_JOURNAL_BAD_DEVICE;
    }
    if (block_count == 0U ||
        block_count > CONFIG_JOURNAL_REGION_BLOCK_COUNT_MAX ||
        block_offset > device->block_count ||
        block_count > device->block_count - block_offset) {
        return CONFIG_JOURNAL_BAD_REGION;
    }

    journal->device = device;
    journal->block_offset = block_offset;
    journal->block_count = block_count;
    journal->next_block = 0U;
    journal->next_sequence = 1U;
    journal->initialized = false;

    for (slot = block_count; slot > 0U; --slot) {
        const uint64_t current_slot = slot - 1U;
        uint64_t sequence;
        uint32_t data_length;
        const unsigned char *data;

        io_status = block_device_read(
            device, config_journal_block_offset(journal, current_slot),
            block, sizeof(block));
        if (io_status != BLOCK_DEVICE_OK) {
            return CONFIG_JOURNAL_IO_ERROR;
        }
        if (config_journal_block_all_zero(block)) {
            if (found) {
                return CONFIG_JOURNAL_CORRUPT;
            }
            continue;
        }
        if (config_journal_record_valid(
                block, &sequence, &data_length, &data)) {
            if (sequence != current_slot + 1U) {
                return CONFIG_JOURNAL_CORRUPT;
            }
            if (!found) {
                found = true;
                head_block = current_slot;
                head_sequence = sequence;
            }
        } else if (found) {
            return CONFIG_JOURNAL_CORRUPT;
        }
    }

    if (found) {
        journal->next_block = head_block + 1U;
        journal->next_sequence = head_sequence + 1U;
    }
    journal->initialized = true;
    return CONFIG_JOURNAL_OK;
}

enum config_journal_status config_journal_append(
    struct config_journal *journal,
    const void *data,
    size_t length)
{
    enum block_device_status io_status;
    unsigned char block[CONFIG_JOURNAL_BLOCK_SIZE];
    unsigned char digest[SHA256_DIGEST_SIZE];
    struct sha256 context;

    if (journal == NULL || data == NULL) {
        return CONFIG_JOURNAL_NULL;
    }
    if (!journal->initialized || journal->device == NULL ||
        block_device_validate(journal->device) != BLOCK_DEVICE_OK ||
        journal->device->block_size != CONFIG_JOURNAL_BLOCK_SIZE) {
        return CONFIG_JOURNAL_BAD_STATE;
    }
    if (length == 0U || length > CONFIG_JOURNAL_DATA_MAX) {
        return CONFIG_JOURNAL_BAD_RECORD;
    }
    if (journal->next_block >= journal->block_count) {
        return CONFIG_JOURNAL_FULL;
    }

    config_journal_zero_bytes(block, sizeof(block));
    config_journal_copy_bytes(
        block, config_journal_magic, sizeof(config_journal_magic));
    block[8U] = CONFIG_JOURNAL_FORMAT_VERSION;
    config_journal_store_le64(block + 16U, journal->next_sequence);
    config_journal_store_le32(block + 24U, (uint32_t)length);
    config_journal_copy_bytes(
        block + CONFIG_JOURNAL_COMMIT_OFFSET,
        config_journal_commit_magic,
        sizeof(config_journal_commit_magic));
    config_journal_store_le64(
        block + CONFIG_JOURNAL_COMMIT_SEQUENCE_OFFSET,
        journal->next_sequence);
    config_journal_copy_bytes(
        block + CONFIG_JOURNAL_DATA_OFFSET,
        (const unsigned char *)data, length);

    if (sha256_init(&context) != SHA256_OK ||
        sha256_update(&context, block, 32U) != SHA256_OK ||
        sha256_update(&context, block + CONFIG_JOURNAL_DATA_OFFSET,
                      length) != SHA256_OK ||
        sha256_update(&context, block + CONFIG_JOURNAL_COMMIT_OFFSET,
                      CONFIG_JOURNAL_COMMIT_SIZE) != SHA256_OK ||
        sha256_final(&context, digest) != SHA256_OK) {
        return CONFIG_JOURNAL_BAD_STATE;
    }
    config_journal_copy_bytes(
        block + CONFIG_JOURNAL_CHECKSUM_OFFSET, digest,
        SHA256_DIGEST_SIZE);

    io_status = block_device_write(
        journal->device, config_journal_block_offset(journal,
                                                     journal->next_block),
        block, sizeof(block));
    if (io_status != BLOCK_DEVICE_OK) {
        return CONFIG_JOURNAL_IO_ERROR;
    }
    ++journal->next_block;
    ++journal->next_sequence;
    return CONFIG_JOURNAL_OK;
}

enum config_journal_status config_journal_head(
    const struct config_journal *journal,
    void *buffer,
    size_t capacity,
    size_t *length)
{
    enum block_device_status io_status;
    unsigned char block[CONFIG_JOURNAL_BLOCK_SIZE];
    uint64_t sequence;
    uint32_t data_length;
    const unsigned char *data;

    if (journal == NULL || buffer == NULL || length == NULL) {
        return CONFIG_JOURNAL_NULL;
    }
    if (!journal->initialized || journal->device == NULL ||
        block_device_validate(journal->device) != BLOCK_DEVICE_OK ||
        journal->device->block_size != CONFIG_JOURNAL_BLOCK_SIZE) {
        return CONFIG_JOURNAL_BAD_STATE;
    }
    if (journal->next_block == 0U) {
        return CONFIG_JOURNAL_EMPTY;
    }
    if (journal->next_block > journal->block_count) {
        return CONFIG_JOURNAL_BAD_STATE;
    }

    io_status = block_device_read(
        journal->device,
        config_journal_block_offset(journal, journal->next_block - 1U),
        block, sizeof(block));
    if (io_status != BLOCK_DEVICE_OK) {
        return CONFIG_JOURNAL_IO_ERROR;
    }
    if (!config_journal_record_valid(
            block, &sequence, &data_length, &data)) {
        return CONFIG_JOURNAL_CORRUPT;
    }
    *length = data_length;
    if (capacity < data_length) {
        return CONFIG_JOURNAL_TOO_SMALL;
    }
    config_journal_copy_bytes((unsigned char *)buffer, data, data_length);
    return CONFIG_JOURNAL_OK;
}

const char *config_journal_status_string(enum config_journal_status status) {
    switch (status) {
    case CONFIG_JOURNAL_OK:
        return "CONFIG_JOURNAL_OK";
    case CONFIG_JOURNAL_NULL:
        return "CONFIG_JOURNAL_NULL";
    case CONFIG_JOURNAL_BAD_DEVICE:
        return "CONFIG_JOURNAL_BAD_DEVICE";
    case CONFIG_JOURNAL_BAD_REGION:
        return "CONFIG_JOURNAL_BAD_REGION";
    case CONFIG_JOURNAL_IO_ERROR:
        return "CONFIG_JOURNAL_IO_ERROR";
    case CONFIG_JOURNAL_CORRUPT:
        return "CONFIG_JOURNAL_CORRUPT";
    case CONFIG_JOURNAL_BAD_STATE:
        return "CONFIG_JOURNAL_BAD_STATE";
    case CONFIG_JOURNAL_BAD_RECORD:
        return "CONFIG_JOURNAL_BAD_RECORD";
    case CONFIG_JOURNAL_TOO_SMALL:
        return "CONFIG_JOURNAL_TOO_SMALL";
    case CONFIG_JOURNAL_EMPTY:
        return "CONFIG_JOURNAL_EMPTY";
    case CONFIG_JOURNAL_FULL:
        return "CONFIG_JOURNAL_FULL";
    default:
        return "CONFIG_JOURNAL_UNKNOWN";
    }
}
