#ifndef HFOC_BACKUP_STREAM_H
#define HFOC_BACKUP_STREAM_H

#include "config_kv.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define BACKUP_STREAM_MAGIC_SIZE 8U
#define BACKUP_STREAM_FORMAT_VERSION 1U
#define BACKUP_STREAM_HEADER_SIZE 32U
#define BACKUP_STREAM_FRAME_HEADER_SIZE 12U
#define BACKUP_STREAM_DIGEST_SIZE 32U
#define BACKUP_STREAM_MAX_ENTRIES CONFIG_KV_CAPACITY_MAX
#define BACKUP_STREAM_MAX_FRAMED_LENGTH \
    (BACKUP_STREAM_HEADER_SIZE + BACKUP_STREAM_DIGEST_SIZE + \
     (BACKUP_STREAM_MAX_ENTRIES * (BACKUP_STREAM_FRAME_HEADER_SIZE + \
                                   CONFIG_KV_KEY_MAX + CONFIG_KV_VALUE_MAX)))
#define BACKUP_STREAM_MAGIC_BYTES \
    { 'H', 'F', 'O', 'C', 'B', 'K', 'U', 'P' }

enum backup_stream_status {
    BACKUP_STREAM_OK = 0,
    BACKUP_STREAM_NULL = 1,
    BACKUP_STREAM_BAD_STATE = 2,
    BACKUP_STREAM_TOO_SMALL = 3,
    BACKUP_STREAM_OVERFLOW = 4,
    BACKUP_STREAM_UNSUPPORTED = 5,
    BACKUP_STREAM_CORRUPT = 6,
};

struct backup_stream_counts {
    uint32_t config_count;
    uint32_t audit_count;
};

struct backup_stream_header {
    bool valid;
    uint8_t version;
    uint32_t config_count;
    uint32_t audit_count;
};

struct backup_stream_layout {
    bool valid;
    struct backup_stream_header header;
    size_t entry_count;
    size_t payload_length;
    size_t digest_offset;
    size_t stream_length;
};

enum backup_stream_status backup_stream_export(
    const struct config_kv *kv,
    void *buffer,
    size_t capacity,
    size_t *length_out);
enum backup_stream_status backup_stream_frame_validate(
    const void *stream,
    size_t length,
    struct backup_stream_layout *layout);
enum backup_stream_status backup_stream_import(
    const void *stream,
    size_t length,
    struct config_kv_entry *entries,
    size_t capacity,
    size_t *entry_count_out,
    struct backup_stream_counts *counts_out);
const char *backup_stream_status_string(enum backup_stream_status status);

#endif
