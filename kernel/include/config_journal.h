#ifndef HFOC_CONFIG_JOURNAL_H
#define HFOC_CONFIG_JOURNAL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "block_device.h"

#define CONFIG_JOURNAL_BLOCK_SIZE 512U
#define CONFIG_JOURNAL_MAGIC_SIZE 8U
#define CONFIG_JOURNAL_FORMAT_VERSION 2U
#define CONFIG_JOURNAL_CHECKSUM_OFFSET 32U
#define CONFIG_JOURNAL_CHECKSUM_SIZE 32U
#define CONFIG_JOURNAL_DATA_OFFSET 64U
#define CONFIG_JOURNAL_DATA_MAX 384U
#define CONFIG_JOURNAL_RECORD_SIZE \
    (CONFIG_JOURNAL_DATA_OFFSET + CONFIG_JOURNAL_DATA_MAX)
#define CONFIG_JOURNAL_COMMIT_OFFSET CONFIG_JOURNAL_RECORD_SIZE
#define CONFIG_JOURNAL_COMMIT_SIZE \
    (CONFIG_JOURNAL_BLOCK_SIZE - CONFIG_JOURNAL_COMMIT_OFFSET)
#define CONFIG_JOURNAL_COMMIT_MAGIC_SIZE 8U
#define CONFIG_JOURNAL_COMMIT_SEQUENCE_OFFSET \
    (CONFIG_JOURNAL_COMMIT_OFFSET + CONFIG_JOURNAL_COMMIT_MAGIC_SIZE)
#define CONFIG_JOURNAL_COMMIT_SEQUENCE_SIZE 8U
#define CONFIG_JOURNAL_REGION_BLOCK_COUNT_MAX 4096U
#define CONFIG_JOURNAL_MAGIC_BYTES \
    { 'H', 'F', 'O', 'C', 'J', 'R', 'N', 'L' }
#define CONFIG_JOURNAL_COMMIT_MAGIC_BYTES \
    { 'H', 'F', 'O', 'C', 'C', 'O', 'M', 'M' }

enum config_journal_status {
    CONFIG_JOURNAL_OK = 0,
    CONFIG_JOURNAL_NULL = 1,
    CONFIG_JOURNAL_BAD_DEVICE = 2,
    CONFIG_JOURNAL_BAD_REGION = 3,
    CONFIG_JOURNAL_IO_ERROR = 4,
    CONFIG_JOURNAL_CORRUPT = 5,
    CONFIG_JOURNAL_BAD_STATE = 6,
    CONFIG_JOURNAL_BAD_RECORD = 7,
    CONFIG_JOURNAL_TOO_SMALL = 8,
    CONFIG_JOURNAL_EMPTY = 9,
    CONFIG_JOURNAL_FULL = 10,
};

struct config_journal {
    struct block_device *device;
    uint64_t block_offset;
    uint64_t block_count;
    uint64_t next_block;
    uint64_t next_sequence;
    bool initialized;
};

enum config_journal_status config_journal_init(
    struct config_journal *journal,
    struct block_device *device,
    uint64_t block_offset,
    uint64_t block_count);
enum config_journal_status config_journal_append(
    struct config_journal *journal,
    const void *data,
    size_t length);
enum config_journal_status config_journal_head(
    const struct config_journal *journal,
    void *buffer,
    size_t capacity,
    size_t *length);
const char *config_journal_status_string(enum config_journal_status status);

#endif
