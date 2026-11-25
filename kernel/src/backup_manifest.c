#include "backup_manifest.h"

#include "backup_stream.h"
#include "sha256.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

static const unsigned char backup_manifest_magic[
    BACKUP_MANIFEST_MAGIC_SIZE] = BACKUP_MANIFEST_MAGIC_BYTES;

static void backup_manifest_zero_bytes(unsigned char *bytes, size_t length)
{
    size_t index;

    for (index = 0U; index < length; ++index) {
        bytes[index] = 0U;
    }
}

static void backup_manifest_copy_bytes(unsigned char *destination,
                                       const unsigned char *source,
                                       size_t length)
{
    size_t index;

    for (index = 0U; index < length; ++index) {
        destination[index] = source[index];
    }
}

static bool backup_manifest_bytes_equal(const unsigned char *left,
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

static void backup_manifest_store_le32(unsigned char *bytes, uint32_t value)
{
    bytes[0] = (unsigned char)(value);
    bytes[1] = (unsigned char)(value >> 8U);
    bytes[2] = (unsigned char)(value >> 16U);
    bytes[3] = (unsigned char)(value >> 24U);
}

static void backup_manifest_store_le16(unsigned char *bytes, uint16_t value)
{
    bytes[0] = (unsigned char)(value);
    bytes[1] = (unsigned char)(value >> 8U);
}

static void backup_manifest_store_le64(unsigned char *bytes, uint64_t value)
{
    size_t index;

    for (index = 0U; index < 8U; ++index) {
        bytes[index] = (unsigned char)(value >> (8U * index));
    }
}

static bool backup_manifest_entry_valid(
    const struct config_kv_entry *entry)
{
    return entry != NULL &&
           entry->occupied &&
           entry->kind < CONFIG_KV_KIND_COUNT &&
           entry->key_length > 0U &&
           entry->key_length <= CONFIG_KV_KEY_MAX &&
           entry->value_length <= CONFIG_KV_VALUE_MAX &&
           entry->revision != 0U;
}

static bool backup_manifest_entry_matches(
    const struct config_kv_entry *entry,
    uint8_t kind,
    const unsigned char *key,
    size_t key_length)
{
    return entry->occupied &&
           entry->kind == kind &&
           entry->key_length == key_length &&
           backup_manifest_bytes_equal(entry->key, key, key_length);
}

static bool backup_manifest_checkpoint_valid(
    const struct config_kv *kv,
    size_t *config_count_out,
    size_t *audit_count_out)
{
    size_t config_count = 0U;
    size_t audit_count = 0U;
    size_t index;
    size_t previous;

    if (kv == NULL ||
        !kv->initialized ||
        kv->entries == NULL ||
        kv->capacity == 0U ||
        kv->capacity > CONFIG_KV_CAPACITY_MAX ||
        kv->count > kv->capacity) {
        return false;
    }
    for (index = 0U; index < kv->capacity; ++index) {
        const struct config_kv_entry *entry = &kv->entries[index];

        if (!entry->occupied) {
            continue;
        }
        if (!backup_manifest_entry_valid(entry)) {
            return false;
        }
        for (previous = 0U; previous < index; ++previous) {
            if (backup_manifest_entry_matches(
                    &kv->entries[previous], entry->kind, entry->key,
                    entry->key_length)) {
                return false;
            }
        }
        if (entry->kind == CONFIG_KV_KIND_CONFIG) {
            ++config_count;
        } else {
            ++audit_count;
        }
    }
    if (config_count + audit_count != kv->count) {
        return false;
    }
    if (config_count_out != NULL) {
        *config_count_out = config_count;
    }
    if (audit_count_out != NULL) {
        *audit_count_out = audit_count;
    }
    return true;
}

static bool backup_manifest_entry_less(
    const struct config_kv_entry *left,
    const struct config_kv_entry *right)
{
    size_t index;

    if (left->kind != right->kind) {
        return left->kind < right->kind;
    }
    if (left->key_length != right->key_length) {
        return left->key_length < right->key_length;
    }
    for (index = 0U; index < left->key_length; ++index) {
        if (left->key[index] != right->key[index]) {
            return left->key[index] < right->key[index];
        }
    }
    return left->revision < right->revision;
}

static void backup_manifest_sort_refs(
    const struct config_kv_entry **refs,
    size_t count)
{
    size_t index;

    for (index = 1U; index < count; ++index) {
        const struct config_kv_entry *entry = refs[index];
        size_t position = index;

        while (position > 0U &&
               backup_manifest_entry_less(entry,
                                          refs[position - 1U])) {
            refs[position] = refs[position - 1U];
            --position;
        }
        refs[position] = entry;
    }
}

static enum backup_manifest_status backup_manifest_checkpoint_digest(
    const struct config_kv *kv,
    uint8_t kind,
    unsigned char digest[SHA256_DIGEST_SIZE])
{
    const struct config_kv_entry *refs[BACKUP_STREAM_MAX_ENTRIES];
    struct sha256 context;
    unsigned char frame_header[BACKUP_STREAM_FRAME_HEADER_SIZE];
    unsigned char result[SHA256_DIGEST_SIZE];
    size_t count = 0U;
    size_t index;

    for (index = 0U; index < kv->capacity; ++index) {
        const struct config_kv_entry *entry = &kv->entries[index];

        if (entry->occupied && entry->kind == kind) {
            refs[count] = entry;
            ++count;
        }
    }
    backup_manifest_sort_refs(refs, count);

    if (sha256_init(&context) != SHA256_OK) {
        return BACKUP_MANIFEST_BAD_STATE;
    }
    for (index = 0U; index < count; ++index) {
        const struct config_kv_entry *entry = refs[index];

        backup_manifest_zero_bytes(frame_header,
                                   sizeof(frame_header));
        frame_header[0] = entry->kind;
        frame_header[1] = entry->key_length;
        backup_manifest_store_le16(frame_header + 2U,
                                   (uint16_t)entry->value_length);
        backup_manifest_store_le64(frame_header + 4U, entry->revision);
        if (sha256_update(&context, frame_header,
                          sizeof(frame_header)) != SHA256_OK ||
            sha256_update(&context, entry->key,
                          entry->key_length) != SHA256_OK ||
            sha256_update(&context, entry->value,
                          entry->value_length) != SHA256_OK) {
            return BACKUP_MANIFEST_BAD_STATE;
        }
    }
    if (sha256_final(&context, result) != SHA256_OK) {
        return BACKUP_MANIFEST_BAD_STATE;
    }
    backup_manifest_copy_bytes(digest, result, sizeof(result));
    return BACKUP_MANIFEST_OK;
}

static bool backup_manifest_id_char_valid(unsigned char value)
{
    return (value >= (unsigned char)'a' && value <= (unsigned char)'z') ||
           (value >= (unsigned char)'A' && value <= (unsigned char)'Z') ||
           (value >= (unsigned char)'0' && value <= (unsigned char)'9') ||
           value == (unsigned char)'_';
}

static bool backup_manifest_descriptor_valid(
    const struct state_schema_descriptor *descriptor)
{
    size_t index;

    if (descriptor->kind >= STATE_SCHEMA_KIND_COUNT) {
        return false;
    }
    if (descriptor->kind == STATE_SCHEMA_KIND_JOURNAL) {
        if (descriptor->target_kind != STATE_SCHEMA_TARGET_JOURNAL) {
            return false;
        }
    } else if (descriptor->target_kind >= CONFIG_KV_KIND_COUNT) {
        return false;
    }
    if (descriptor->id_length == 0U ||
        descriptor->id_length > STATE_SCHEMA_ID_MAX) {
        return false;
    }
    for (index = 0U; index < descriptor->id_length; ++index) {
        if (!backup_manifest_id_char_valid(descriptor->id[index])) {
            return false;
        }
    }
    if (descriptor->version == 0U ||
        descriptor->minimum_version == 0U ||
        descriptor->minimum_version > descriptor->maximum_version ||
        descriptor->version < descriptor->minimum_version ||
        descriptor->version > descriptor->maximum_version) {
        return false;
    }
    return descriptor->minimum_length <= descriptor->maximum_length &&
           descriptor->maximum_length <= STATE_SCHEMA_RECORD_LENGTH_MAX;
}

static bool backup_manifest_descriptor_matches(
    const struct state_schema_descriptor *left,
    const struct state_schema_descriptor *right)
{
    return left->kind == right->kind &&
           left->id_length == right->id_length &&
           backup_manifest_bytes_equal(left->id, right->id,
                                       left->id_length);
}

static bool backup_manifest_registry_valid(
    const struct state_schema_registry *registry)
{
    size_t index;
    size_t previous;

    if (registry == NULL ||
        !registry->initialized ||
        registry->count == 0U ||
        registry->count > STATE_SCHEMA_REGISTRY_CAPACITY_MAX) {
        return false;
    }
    for (index = 0U; index < registry->count; ++index) {
        if (!backup_manifest_descriptor_valid(
                &registry->descriptors[index])) {
            return false;
        }
        for (previous = 0U; previous < index; ++previous) {
            if (backup_manifest_descriptor_matches(
                    &registry->descriptors[index],
                    &registry->descriptors[previous])) {
                return false;
            }
        }
    }
    return true;
}

static enum backup_manifest_status backup_manifest_schema_digest(
    const struct state_schema_registry *registry,
    unsigned char digest[SHA256_DIGEST_SIZE])
{
    struct sha256 context;
    unsigned char record[BACKUP_MANIFEST_SCHEMA_RECORD_SIZE];
    unsigned char result[SHA256_DIGEST_SIZE];
    size_t index;

    if (sha256_init(&context) != SHA256_OK) {
        return BACKUP_MANIFEST_BAD_STATE;
    }
    for (index = 0U; index < registry->count; ++index) {
        const struct state_schema_descriptor *descriptor =
            &registry->descriptors[index];

        backup_manifest_zero_bytes(record, sizeof(record));
        record[0] = descriptor->kind;
        record[1] = descriptor->target_kind;
        record[2] = descriptor->id_length;
        backup_manifest_copy_bytes(record + 4U,
                                   descriptor->id,
                                   descriptor->id_length);
        backup_manifest_store_le32(record + 20U, descriptor->version);
        backup_manifest_store_le32(record + 24U,
                                   descriptor->minimum_version);
        backup_manifest_store_le32(record + 28U,
                                   descriptor->maximum_version);
        backup_manifest_store_le16(record + 32U,
                                   descriptor->minimum_length);
        backup_manifest_store_le16(record + 34U,
                                   descriptor->maximum_length);
        if (sha256_update(&context, record, sizeof(record)) != SHA256_OK) {
            return BACKUP_MANIFEST_BAD_STATE;
        }
    }
    if (sha256_final(&context, result) != SHA256_OK) {
        return BACKUP_MANIFEST_BAD_STATE;
    }
    backup_manifest_copy_bytes(digest, result, sizeof(result));
    return BACKUP_MANIFEST_OK;
}

static bool backup_manifest_generation_valid(
    const struct journal_compaction_generation *generation)
{
    size_t seen_config = 0U;
    size_t seen_audit = 0U;
    size_t index;
    size_t previous;

    if (generation == NULL ||
        !generation->valid ||
        !generation->active ||
        !generation->staged ||
        generation->interrupted ||
        generation->generation_id == 0U ||
        generation->source_sequence == 0U ||
        generation->count > JOURNAL_COMPACTION_MAX_ENTRIES ||
        generation->config_count > JOURNAL_COMPACTION_MAX_ENTRIES ||
        generation->audit_count > JOURNAL_COMPACTION_MAX_ENTRIES ||
        (uint64_t)generation->config_count +
            generation->audit_count != generation->count) {
        return false;
    }
    for (index = 0U; index < generation->count; ++index) {
        const struct config_kv_entry *entry = &generation->entries[index];

        if (!backup_manifest_entry_valid(entry)) {
            return false;
        }
        for (previous = 0U; previous < index; ++previous) {
            if (backup_manifest_entry_matches(
                    &generation->entries[previous], entry->kind,
                    entry->key, entry->key_length)) {
                return false;
            }
        }
        if (entry->kind == CONFIG_KV_KIND_CONFIG) {
            ++seen_config;
        } else {
            ++seen_audit;
        }
    }
    return seen_config == generation->config_count &&
           seen_audit == generation->audit_count;
}

enum backup_manifest_status backup_manifest_export(
    const struct backup_manifest_source *source,
    void *buffer,
    size_t capacity,
    size_t *length_out)
{
    unsigned char config_digest[SHA256_DIGEST_SIZE];
    unsigned char audit_digest[SHA256_DIGEST_SIZE];
    unsigned char schema_digest[SHA256_DIGEST_SIZE];
    enum backup_manifest_status status;
    size_t config_count = 0U;
    size_t audit_count = 0U;
    unsigned char *bytes;

    if (source == NULL || buffer == NULL || length_out == NULL) {
        return BACKUP_MANIFEST_NULL;
    }
    if (!backup_manifest_checkpoint_valid(
            source->checkpoint, &config_count, &audit_count) ||
        !backup_manifest_registry_valid(source->registry) ||
        !backup_manifest_generation_valid(source->generation)) {
        return BACKUP_MANIFEST_BAD_STATE;
    }
    status = backup_manifest_checkpoint_digest(
        source->checkpoint, CONFIG_KV_KIND_CONFIG, config_digest);
    if (status != BACKUP_MANIFEST_OK) {
        return status;
    }
    status = backup_manifest_checkpoint_digest(
        source->checkpoint, CONFIG_KV_KIND_AUDIT, audit_digest);
    if (status != BACKUP_MANIFEST_OK) {
        return status;
    }
    status = backup_manifest_schema_digest(
        source->registry, schema_digest);
    if (status != BACKUP_MANIFEST_OK) {
        return status;
    }
    if (capacity < BACKUP_MANIFEST_SIZE) {
        return BACKUP_MANIFEST_TOO_SMALL;
    }

    bytes = (unsigned char *)buffer;
    backup_manifest_zero_bytes(bytes, BACKUP_MANIFEST_SIZE);
    backup_manifest_copy_bytes(bytes, backup_manifest_magic,
                               BACKUP_MANIFEST_MAGIC_SIZE);
    bytes[8U] = BACKUP_MANIFEST_FORMAT_VERSION;
    backup_manifest_store_le32(bytes + 16U, (uint32_t)config_count);
    backup_manifest_store_le32(bytes + 20U, (uint32_t)audit_count);
    backup_manifest_copy_bytes(bytes + 24U, config_digest,
                               sizeof(config_digest));
    backup_manifest_copy_bytes(bytes + 56U, audit_digest,
                               sizeof(audit_digest));
    backup_manifest_copy_bytes(bytes + 88U, schema_digest,
                               sizeof(schema_digest));
    backup_manifest_store_le64(bytes + 120U,
                               source->generation->generation_id);
    backup_manifest_store_le64(bytes + 128U,
                               source->generation->source_sequence);
    *length_out = BACKUP_MANIFEST_SIZE;
    return BACKUP_MANIFEST_OK;
}

const char *backup_manifest_status_string(
    enum backup_manifest_status status)
{
    switch (status) {
    case BACKUP_MANIFEST_OK:
        return "BACKUP_MANIFEST_OK";
    case BACKUP_MANIFEST_NULL:
        return "BACKUP_MANIFEST_NULL";
    case BACKUP_MANIFEST_BAD_STATE:
        return "BACKUP_MANIFEST_BAD_STATE";
    case BACKUP_MANIFEST_TOO_SMALL:
        return "BACKUP_MANIFEST_TOO_SMALL";
    default:
        return "BACKUP_MANIFEST_UNKNOWN";
    }
}
