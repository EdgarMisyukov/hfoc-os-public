#include "backup_producer.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

static const unsigned char backup_producer_magic[BACKUP_STREAM_MAGIC_SIZE] =
    BACKUP_STREAM_MAGIC_BYTES;

static void backup_producer_zero_bytes(unsigned char *bytes, size_t length)
{
    size_t index;

    for (index = 0U; index < length; ++index) {
        bytes[index] = 0U;
    }
}

static void backup_producer_copy_bytes(unsigned char *destination,
                                       const unsigned char *source,
                                       size_t length)
{
    size_t index;

    for (index = 0U; index < length; ++index) {
        destination[index] = source[index];
    }
}

static bool backup_producer_bytes_equal(const unsigned char *left,
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

static void backup_producer_store_le16(unsigned char *bytes, uint16_t value)
{
    bytes[0] = (unsigned char)(value);
    bytes[1] = (unsigned char)(value >> 8U);
}

static void backup_producer_store_le32(unsigned char *bytes, uint32_t value)
{
    bytes[0] = (unsigned char)(value);
    bytes[1] = (unsigned char)(value >> 8U);
    bytes[2] = (unsigned char)(value >> 16U);
    bytes[3] = (unsigned char)(value >> 24U);
}

static void backup_producer_store_le64(unsigned char *bytes, uint64_t value)
{
    size_t index;

    for (index = 0U; index < 8U; ++index) {
        bytes[index] = (unsigned char)(value >> (8U * index));
    }
}

static bool backup_producer_entry_valid(
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

static bool backup_producer_entry_matches(
    const struct config_kv_entry *entry,
    uint8_t kind,
    const unsigned char *key,
    size_t key_length)
{
    return entry->occupied &&
           entry->kind == kind &&
           entry->key_length == key_length &&
           backup_producer_bytes_equal(entry->key, key, key_length);
}

static bool backup_producer_kv_valid(const struct config_kv *kv)
{
    size_t index;

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
        size_t previous;

        if (!entry->occupied) {
            continue;
        }
        if (!backup_producer_entry_valid(entry)) {
            return false;
        }
        for (previous = 0U; previous < index; ++previous) {
            if (backup_producer_entry_matches(
                    &kv->entries[previous], entry->kind, entry->key,
                    entry->key_length)) {
                return false;
            }
        }
    }
    return true;
}

static bool backup_producer_entry_less(
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

static void backup_producer_sort_refs(
    const struct config_kv_entry **refs,
    size_t count)
{
    size_t index;

    for (index = 1U; index < count; ++index) {
        const struct config_kv_entry *entry = refs[index];
        size_t position = index;

        while (position > 0U &&
               backup_producer_entry_less(entry,
                                          refs[position - 1U])) {
            refs[position] = refs[position - 1U];
            --position;
        }
        refs[position] = entry;
    }
}

enum backup_producer_status backup_producer_init(
    struct backup_producer *producer,
    const struct config_kv *kv)
{
    size_t count = 0U;
    size_t config_count = 0U;
    size_t audit_count = 0U;
    size_t index;

    if (producer == NULL || kv == NULL) {
        return BACKUP_PRODUCER_NULL;
    }
    if (!backup_producer_kv_valid(kv)) {
        return BACKUP_PRODUCER_BAD_STATE;
    }

    memset(producer, 0, sizeof(*producer));
    for (index = 0U; index < kv->capacity; ++index) {
        const struct config_kv_entry *entry = &kv->entries[index];

        if (!entry->occupied) {
            continue;
        }
        producer->refs[count] = entry;
        ++count;
        if (entry->kind == CONFIG_KV_KIND_CONFIG) {
            ++config_count;
        } else {
            ++audit_count;
        }
    }
    if (count != kv->count ||
        count > BACKUP_PRODUCER_MAX_ENTRIES ||
        config_count > BACKUP_PRODUCER_MAX_ENTRIES ||
        audit_count > BACKUP_PRODUCER_MAX_ENTRIES) {
        return BACKUP_PRODUCER_BAD_STATE;
    }

    backup_producer_sort_refs(producer->refs, count);
    producer->kv = kv;
    producer->count = count;
    producer->config_count = (uint32_t)config_count;
    producer->audit_count = (uint32_t)audit_count;
    producer->active = true;
    if (sha256_init(&producer->digest) != SHA256_OK) {
        memset(producer, 0, sizeof(*producer));
        return BACKUP_PRODUCER_BAD_STATE;
    }
    return BACKUP_PRODUCER_OK;
}

static void backup_producer_write_header(
    unsigned char *unit,
    const struct backup_producer *producer)
{
    backup_producer_zero_bytes(unit, BACKUP_PRODUCER_HEADER_SIZE);
    backup_producer_copy_bytes(unit, backup_producer_magic,
                               BACKUP_STREAM_MAGIC_SIZE);
    unit[BACKUP_STREAM_MAGIC_SIZE] = BACKUP_STREAM_FORMAT_VERSION;
    backup_producer_store_le32(unit + 16U, producer->config_count);
    backup_producer_store_le32(unit + 20U, producer->audit_count);
}

static size_t backup_producer_frame_size(
    const struct config_kv_entry *entry)
{
    return BACKUP_STREAM_FRAME_HEADER_SIZE +
           entry->key_length + entry->value_length;
}

static void backup_producer_write_frame(
    unsigned char *unit,
    const struct config_kv_entry *entry)
{
    backup_producer_zero_bytes(unit, BACKUP_PRODUCER_MAX_FRAME_SIZE);
    unit[0] = entry->kind;
    unit[1] = entry->key_length;
    backup_producer_store_le16(unit + 2U,
                               (uint16_t)entry->value_length);
    backup_producer_store_le64(unit + 4U, entry->revision);
    backup_producer_copy_bytes(unit + BACKUP_STREAM_FRAME_HEADER_SIZE,
                               entry->key, entry->key_length);
    backup_producer_copy_bytes(
        unit + BACKUP_STREAM_FRAME_HEADER_SIZE + entry->key_length,
        entry->value, entry->value_length);
}

enum backup_producer_status backup_producer_next(
    struct backup_producer *producer,
    void *buffer,
    size_t capacity,
    size_t *length_out)
{
    unsigned char unit[BACKUP_PRODUCER_MAX_FRAME_SIZE];
    unsigned char digest[SHA256_DIGEST_SIZE];
    struct sha256 final_context;
    size_t unit_length = 0U;

    if (producer == NULL || buffer == NULL || length_out == NULL) {
        return BACKUP_PRODUCER_NULL;
    }
    if (producer->closed) {
        return producer->done ? BACKUP_PRODUCER_DONE :
                               BACKUP_PRODUCER_BAD_STATE;
    }
    if (!producer->active ||
        producer->kv == NULL ||
        producer->count > BACKUP_PRODUCER_MAX_ENTRIES ||
        producer->next_frame > producer->count) {
        return BACKUP_PRODUCER_BAD_STATE;
    }
    if (producer->done) {
        return BACKUP_PRODUCER_DONE;
    }

    if (!producer->header_emitted) {
        unit_length = BACKUP_PRODUCER_HEADER_SIZE;
        if (capacity < unit_length) {
            return BACKUP_PRODUCER_TOO_SMALL;
        }
        backup_producer_write_header(unit, producer);
        if (sha256_update(&producer->digest, unit,
                          unit_length) != SHA256_OK) {
            return BACKUP_PRODUCER_BAD_STATE;
        }
        producer->header_emitted = true;
    } else if (producer->next_frame < producer->count) {
        const struct config_kv_entry *entry =
            producer->refs[producer->next_frame];

        unit_length = backup_producer_frame_size(entry);
        if (capacity < unit_length) {
            return BACKUP_PRODUCER_TOO_SMALL;
        }
        backup_producer_write_frame(unit, entry);
        if (sha256_update(&producer->digest, unit,
                          unit_length) != SHA256_OK) {
            return BACKUP_PRODUCER_BAD_STATE;
        }
        ++producer->next_frame;
    } else {
        unit_length = BACKUP_PRODUCER_DIGEST_SIZE;
        if (capacity < unit_length) {
            return BACKUP_PRODUCER_TOO_SMALL;
        }
        final_context = producer->digest;
        if (sha256_final(&final_context, digest) != SHA256_OK) {
            return BACKUP_PRODUCER_BAD_STATE;
        }
        backup_producer_copy_bytes(unit, digest, sizeof(digest));
        producer->done = true;
    }

    backup_producer_copy_bytes((unsigned char *)buffer, unit, unit_length);
    *length_out = unit_length;
    return BACKUP_PRODUCER_OK;
}

enum backup_producer_status backup_producer_close(
    struct backup_producer *producer)
{
    if (producer == NULL) {
        return BACKUP_PRODUCER_NULL;
    }
    producer->kv = NULL;
    producer->count = 0U;
    producer->next_frame = 0U;
    producer->config_count = 0U;
    producer->audit_count = 0U;
    producer->active = false;
    producer->closed = true;
    return BACKUP_PRODUCER_OK;
}

const char *backup_producer_status_string(
    enum backup_producer_status status)
{
    switch (status) {
    case BACKUP_PRODUCER_OK:
        return "BACKUP_PRODUCER_OK";
    case BACKUP_PRODUCER_NULL:
        return "BACKUP_PRODUCER_NULL";
    case BACKUP_PRODUCER_BAD_STATE:
        return "BACKUP_PRODUCER_BAD_STATE";
    case BACKUP_PRODUCER_TOO_SMALL:
        return "BACKUP_PRODUCER_TOO_SMALL";
    case BACKUP_PRODUCER_DONE:
        return "BACKUP_PRODUCER_DONE";
    default:
        return "BACKUP_PRODUCER_UNKNOWN";
    }
}
