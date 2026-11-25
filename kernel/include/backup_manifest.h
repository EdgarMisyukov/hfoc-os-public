#ifndef HFOC_BACKUP_MANIFEST_H
#define HFOC_BACKUP_MANIFEST_H

#include "config_kv.h"
#include "journal_compaction.h"
#include "state_schema.h"

#include <stddef.h>
#include <stdint.h>

#define BACKUP_MANIFEST_FORMAT_VERSION 1U
#define BACKUP_MANIFEST_MAGIC_SIZE 8U
#define BACKUP_MANIFEST_SIZE 160U
#define BACKUP_MANIFEST_DIGEST_SIZE 32U
#define BACKUP_MANIFEST_SCHEMA_RECORD_SIZE 40U

#define BACKUP_MANIFEST_MAGIC_BYTES \
    { 'H', 'F', 'O', 'C', 'B', 'M', 'N', 'F' }

enum backup_manifest_status {
    BACKUP_MANIFEST_OK = 0,
    BACKUP_MANIFEST_NULL = 1,
    BACKUP_MANIFEST_BAD_STATE = 2,
    BACKUP_MANIFEST_TOO_SMALL = 3,
};

struct backup_manifest_source {
    const struct config_kv *checkpoint;
    const struct state_schema_registry *registry;
    const struct journal_compaction_generation *generation;
};

enum backup_manifest_status backup_manifest_export(
    const struct backup_manifest_source *source,
    void *buffer,
    size_t capacity,
    size_t *length_out);
const char *backup_manifest_status_string(
    enum backup_manifest_status status);

#endif
