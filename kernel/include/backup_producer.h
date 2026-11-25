#ifndef HFOC_BACKUP_PRODUCER_H
#define HFOC_BACKUP_PRODUCER_H

#include "backup_stream.h"
#include "config_kv.h"
#include "sha256.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define BACKUP_PRODUCER_MAX_ENTRIES BACKUP_STREAM_MAX_ENTRIES
#define BACKUP_PRODUCER_HEADER_SIZE BACKUP_STREAM_HEADER_SIZE
#define BACKUP_PRODUCER_DIGEST_SIZE BACKUP_STREAM_DIGEST_SIZE
#define BACKUP_PRODUCER_MAX_FRAME_SIZE \
    (BACKUP_STREAM_FRAME_HEADER_SIZE + CONFIG_KV_KEY_MAX + \
     CONFIG_KV_VALUE_MAX)
#define BACKUP_PRODUCER_MIN_CHUNK_SIZE BACKUP_PRODUCER_MAX_FRAME_SIZE

enum backup_producer_status {
    BACKUP_PRODUCER_OK = 0,
    BACKUP_PRODUCER_NULL = 1,
    BACKUP_PRODUCER_BAD_STATE = 2,
    BACKUP_PRODUCER_TOO_SMALL = 3,
    BACKUP_PRODUCER_DONE = 4,
};

struct backup_producer {
    const struct config_kv *kv;
    const struct config_kv_entry *refs[BACKUP_STREAM_MAX_ENTRIES];
    size_t count;
    size_t next_frame;
    uint32_t config_count;
    uint32_t audit_count;
    struct sha256 digest;
    bool header_emitted;
    bool active;
    bool done;
    bool closed;
};

enum backup_producer_status backup_producer_init(
    struct backup_producer *producer,
    const struct config_kv *kv);
enum backup_producer_status backup_producer_next(
    struct backup_producer *producer,
    void *buffer,
    size_t capacity,
    size_t *length_out);
enum backup_producer_status backup_producer_close(
    struct backup_producer *producer);
const char *backup_producer_status_string(
    enum backup_producer_status status);

#endif
