#include "backup_stream.h"

#include "sha256.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

static const unsigned char backup_stream_magic[BACKUP_STREAM_MAGIC_SIZE] =
    BACKUP_STREAM_MAGIC_BYTES;

static void backup_stream_zero_bytes(unsigned char *bytes, size_t length)
{
    size_t index;

    for (index = 0U; index < length; ++index) {
        bytes[index] = 0U;
    }
}

static void backup_stream_copy_bytes(unsigned char *destination,
                                     const unsigned char *source,
                                     size_t length)
{
    size_t index;

    for (index = 0U; index < length; ++index) {
        destination[index] = source[index];
    }
}

static bool backup_stream_bytes_equal(const unsigned char *left,
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

static uint32_t backup_stream_load_le32(const unsigned char *bytes)
{
    return (uint32_t)bytes[0] |
           ((uint32_t)bytes[1] << 8U) |
           ((uint32_t)bytes[2] << 16U) |
           ((uint32_t)bytes[3] << 24U);
}

static uint16_t backup_stream_load_le16(const unsigned char *bytes)
{
    return (uint16_t)bytes[0] |
           ((uint16_t)bytes[1] << 8U);
}

static uint64_t backup_stream_load_le64(const unsigned char *bytes)
{
    uint64_t value = 0U;
    size_t index;

    for (index = 0U; index < 8U; ++index) {
        value |= (uint64_t)bytes[index] << (8U * index);
    }
    return value;
}

static void backup_stream_store_le32(unsigned char *bytes, uint32_t value)
{
    bytes[0] = (unsigned char)(value);
    bytes[1] = (unsigned char)(value >> 8U);
    bytes[2] = (unsigned char)(value >> 16U);
    bytes[3] = (unsigned char)(value >> 24U);
}

static void backup_stream_store_le16(unsigned char *bytes, uint16_t value)
{
    bytes[0] = (unsigned char)(value);
    bytes[1] = (unsigned char)(value >> 8U);
}

static void backup_stream_store_le64(unsigned char *bytes, uint64_t value)
{
    size_t index;

    for (index = 0U; index < 8U; ++index) {
        bytes[index] = (unsigned char)(value >> (8U * index));
    }
}

static bool backup_stream_zero_range(const unsigned char *bytes,
                                     size_t offset,
                                     size_t length)
{
    size_t index;

    for (index = 0U; index < length; ++index) {
        if (bytes[offset + index] != 0U) {
            return false;
        }
    }
    return true;
}

struct backup_stream_framing {
    uint32_t config_count;
    uint32_t audit_count;
    size_t entry_count;
    size_t digest_offset;
};

static enum backup_stream_status backup_stream_validate_framing(
    const unsigned char *bytes,
    size_t length,
    struct backup_stream_framing *framing)
{
    struct sha256 context;
    unsigned char digest[SHA256_DIGEST_SIZE];
    size_t offset;
    size_t index;
    size_t total_entries;
    uint32_t config_count;
    uint32_t audit_count;

    if (length < BACKUP_STREAM_HEADER_SIZE + BACKUP_STREAM_DIGEST_SIZE ||
        length > BACKUP_STREAM_MAX_FRAMED_LENGTH) {
        return BACKUP_STREAM_CORRUPT;
    }
    if (!backup_stream_bytes_equal(bytes, backup_stream_magic,
                                   BACKUP_STREAM_MAGIC_SIZE) ||
        !backup_stream_zero_range(bytes, BACKUP_STREAM_MAGIC_SIZE + 1U, 7U) ||
        !backup_stream_zero_range(bytes, 24U, 8U)) {
        return BACKUP_STREAM_CORRUPT;
    }
    if (bytes[BACKUP_STREAM_MAGIC_SIZE] != BACKUP_STREAM_FORMAT_VERSION) {
        return BACKUP_STREAM_UNSUPPORTED;
    }

    config_count = backup_stream_load_le32(bytes + 16U);
    audit_count = backup_stream_load_le32(bytes + 20U);
    if (config_count > BACKUP_STREAM_MAX_ENTRIES ||
        audit_count > BACKUP_STREAM_MAX_ENTRIES ||
        (size_t)config_count + audit_count > BACKUP_STREAM_MAX_ENTRIES) {
        return BACKUP_STREAM_CORRUPT;
    }
    total_entries = (size_t)config_count + audit_count;

    offset = BACKUP_STREAM_HEADER_SIZE;
    for (index = 0U; index < total_entries; ++index) {
        uint8_t kind;
        uint8_t key_length;
        uint16_t value_length;
        uint64_t revision;
        size_t frame_size;

        if (length - BACKUP_STREAM_DIGEST_SIZE - offset <
            BACKUP_STREAM_FRAME_HEADER_SIZE) {
            return BACKUP_STREAM_CORRUPT;
        }
        kind = bytes[offset];
        key_length = bytes[offset + 1U];
        value_length = backup_stream_load_le16(bytes + offset + 2U);
        revision = backup_stream_load_le64(bytes + offset + 4U);
        if (kind >= CONFIG_KV_KIND_COUNT ||
            key_length == 0U ||
            key_length > CONFIG_KV_KEY_MAX ||
            value_length > CONFIG_KV_VALUE_MAX ||
            revision == 0U) {
            return BACKUP_STREAM_CORRUPT;
        }
        frame_size = BACKUP_STREAM_FRAME_HEADER_SIZE +
                     key_length + value_length;
        if (length - BACKUP_STREAM_DIGEST_SIZE - offset < frame_size) {
            return BACKUP_STREAM_CORRUPT;
        }
        offset += frame_size;
    }

    if (offset != length - BACKUP_STREAM_DIGEST_SIZE) {
        return BACKUP_STREAM_CORRUPT;
    }
    if (sha256_init(&context) != SHA256_OK ||
        sha256_update(&context, bytes,
                      length - BACKUP_STREAM_DIGEST_SIZE) != SHA256_OK ||
        sha256_final(&context, digest) != SHA256_OK) {
        return BACKUP_STREAM_BAD_STATE;
    }
    if (!backup_stream_bytes_equal(
            digest, bytes + length - BACKUP_STREAM_DIGEST_SIZE,
            SHA256_DIGEST_SIZE)) {
        return BACKUP_STREAM_CORRUPT;
    }

    framing->config_count = config_count;
    framing->audit_count = audit_count;
    framing->entry_count = total_entries;
    framing->digest_offset = length - BACKUP_STREAM_DIGEST_SIZE;
    return BACKUP_STREAM_OK;
}

static bool backup_stream_kv_valid(const struct config_kv *kv)
{
    return kv != NULL &&
           kv->initialized &&
           kv->entries != NULL &&
           kv->capacity > 0U &&
           kv->capacity <= CONFIG_KV_CAPACITY_MAX &&
           kv->count <= kv->capacity;
}

static bool backup_stream_entry_valid(const struct config_kv_entry *entry)
{
    return entry != NULL &&
           entry->occupied &&
           entry->kind < CONFIG_KV_KIND_COUNT &&
           entry->key_length > 0U &&
           entry->key_length <= CONFIG_KV_KEY_MAX &&
           entry->value_length <= CONFIG_KV_VALUE_MAX &&
           entry->revision != 0U;
}

static bool backup_stream_entry_less(const struct config_kv_entry *left,
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

static void backup_stream_sort_refs(
    const struct config_kv_entry **refs,
    size_t count)
{
    size_t index;

    for (index = 1U; index < count; ++index) {
        const struct config_kv_entry *entry = refs[index];
        size_t position = index;

        while (position > 0U &&
               backup_stream_entry_less(entry, refs[position - 1U])) {
            refs[position] = refs[position - 1U];
            --position;
        }
        refs[position] = entry;
    }
}

enum backup_stream_status backup_stream_export(
    const struct config_kv *kv,
    void *buffer,
    size_t capacity,
    size_t *length_out)
{
    const struct config_kv_entry *refs[BACKUP_STREAM_MAX_ENTRIES];
    struct sha256 context;
    unsigned char digest[SHA256_DIGEST_SIZE];
    unsigned char *bytes;
    size_t count = 0U;
    size_t offset;
    size_t required;
    size_t index;
    uint32_t config_count = 0U;
    uint32_t audit_count = 0U;

    if (kv == NULL || buffer == NULL || length_out == NULL) {
        return BACKUP_STREAM_NULL;
    }
    if (!backup_stream_kv_valid(kv)) {
        return BACKUP_STREAM_BAD_STATE;
    }

    for (index = 0U; index < kv->capacity; ++index) {
        const struct config_kv_entry *entry = &kv->entries[index];

        if (!entry->occupied) {
            continue;
        }
        if (!backup_stream_entry_valid(entry)) {
            return BACKUP_STREAM_BAD_STATE;
        }
        refs[count] = entry;
        ++count;
        if (entry->kind == CONFIG_KV_KIND_CONFIG) {
            ++config_count;
        } else {
            ++audit_count;
        }
    }
    if (count != kv->count ||
        config_count > BACKUP_STREAM_MAX_ENTRIES ||
        audit_count > BACKUP_STREAM_MAX_ENTRIES ||
        count > BACKUP_STREAM_MAX_ENTRIES) {
        return BACKUP_STREAM_BAD_STATE;
    }

    backup_stream_sort_refs(refs, count);
    required = BACKUP_STREAM_HEADER_SIZE + BACKUP_STREAM_DIGEST_SIZE;
    for (index = 0U; index < count; ++index) {
        required += BACKUP_STREAM_FRAME_HEADER_SIZE +
                    refs[index]->key_length + refs[index]->value_length;
    }
    if (capacity < required) {
        return BACKUP_STREAM_TOO_SMALL;
    }

    bytes = (unsigned char *)buffer;
    backup_stream_zero_bytes(bytes, BACKUP_STREAM_HEADER_SIZE);
    backup_stream_copy_bytes(bytes, backup_stream_magic,
                             BACKUP_STREAM_MAGIC_SIZE);
    bytes[BACKUP_STREAM_MAGIC_SIZE] = BACKUP_STREAM_FORMAT_VERSION;
    backup_stream_store_le32(bytes + 16U, config_count);
    backup_stream_store_le32(bytes + 20U, audit_count);

    offset = BACKUP_STREAM_HEADER_SIZE;
    for (index = 0U; index < count; ++index) {
        const struct config_kv_entry *entry = refs[index];

        bytes[offset] = entry->kind;
        bytes[offset + 1U] = entry->key_length;
        backup_stream_store_le16(bytes + offset + 2U,
                                 (uint16_t)entry->value_length);
        backup_stream_store_le64(bytes + offset + 4U, entry->revision);
        backup_stream_copy_bytes(bytes + offset +
                                     BACKUP_STREAM_FRAME_HEADER_SIZE,
                                 entry->key, entry->key_length);
        offset += BACKUP_STREAM_FRAME_HEADER_SIZE + entry->key_length;
        backup_stream_copy_bytes(bytes + offset,
                                 entry->value, entry->value_length);
        offset += entry->value_length;
    }

    if (sha256_init(&context) != SHA256_OK ||
        sha256_update(&context, bytes, offset) != SHA256_OK ||
        sha256_final(&context, digest) != SHA256_OK) {
        return BACKUP_STREAM_BAD_STATE;
    }
    backup_stream_copy_bytes(bytes + offset, digest, SHA256_DIGEST_SIZE);
    *length_out = offset + SHA256_DIGEST_SIZE;
    return BACKUP_STREAM_OK;
}

enum backup_stream_status backup_stream_frame_validate(
    const void *stream,
    size_t length,
    struct backup_stream_layout *layout)
{
    struct backup_stream_framing framing;
    enum backup_stream_status status;
    const unsigned char *bytes;

    if (stream == NULL || layout == NULL) {
        return BACKUP_STREAM_NULL;
    }
    bytes = (const unsigned char *)stream;
    status = backup_stream_validate_framing(bytes, length, &framing);
    if (status != BACKUP_STREAM_OK) {
        return status;
    }

    layout->valid = true;
    layout->header.valid = true;
    layout->header.version = BACKUP_STREAM_FORMAT_VERSION;
    layout->header.config_count = framing.config_count;
    layout->header.audit_count = framing.audit_count;
    layout->entry_count = framing.entry_count;
    layout->payload_length =
        framing.digest_offset - BACKUP_STREAM_HEADER_SIZE;
    layout->digest_offset = framing.digest_offset;
    layout->stream_length = length;
    return BACKUP_STREAM_OK;
}

struct backup_stream_key_ref {
    uint8_t kind;
    uint8_t key_length;
    unsigned char key[CONFIG_KV_KEY_MAX];
};

static bool backup_stream_key_ref_matches(
    const struct backup_stream_key_ref *ref,
    uint8_t kind,
    uint8_t key_length,
    const unsigned char *key)
{
    size_t index;

    if (ref->kind != kind || ref->key_length != key_length) {
        return false;
    }
    for (index = 0U; index < key_length; ++index) {
        if (ref->key[index] != key[index]) {
            return false;
        }
    }
    return true;
}

enum backup_stream_status backup_stream_import(
    const void *stream,
    size_t length,
    struct config_kv_entry *entries,
    size_t capacity,
    size_t *entry_count_out,
    struct backup_stream_counts *counts_out)
{
    struct backup_stream_key_ref seen[BACKUP_STREAM_MAX_ENTRIES];
    struct backup_stream_counts counts;
    struct backup_stream_framing framing;
    const unsigned char *bytes;
    enum backup_stream_status status;
    size_t seen_count = 0U;
    size_t total_entries;
    size_t offset;
    size_t index;

    if (stream == NULL || entry_count_out == NULL || counts_out == NULL) {
        return BACKUP_STREAM_NULL;
    }
    if ((entries == NULL) != (capacity == 0U)) {
        return BACKUP_STREAM_NULL;
    }

    bytes = (const unsigned char *)stream;
    status = backup_stream_validate_framing(bytes, length, &framing);
    if (status != BACKUP_STREAM_OK) {
        return status;
    }
    total_entries = framing.entry_count;

    offset = BACKUP_STREAM_HEADER_SIZE;
    for (index = 0U; index < total_entries; ++index) {
        struct backup_stream_key_ref *candidate;
        uint8_t kind;
        uint8_t key_length;
        uint16_t value_length;
        size_t duplicate_index;

        kind = bytes[offset];
        key_length = bytes[offset + 1U];
        value_length = backup_stream_load_le16(bytes + offset + 2U);

        for (duplicate_index = 0U;
             duplicate_index < seen_count;
             ++duplicate_index) {
            if (backup_stream_key_ref_matches(
                    &seen[duplicate_index], kind, key_length,
                    bytes + offset + BACKUP_STREAM_FRAME_HEADER_SIZE)) {
                return BACKUP_STREAM_CORRUPT;
            }
        }

        candidate = &seen[seen_count];
        candidate->kind = kind;
        candidate->key_length = key_length;
        backup_stream_copy_bytes(
            candidate->key,
            bytes + offset + BACKUP_STREAM_FRAME_HEADER_SIZE,
            key_length);
        ++seen_count;
        offset += BACKUP_STREAM_FRAME_HEADER_SIZE +
                  key_length + value_length;
    }

    counts.config_count = framing.config_count;
    counts.audit_count = framing.audit_count;
    if (entries != NULL && capacity < total_entries) {
        return BACKUP_STREAM_OVERFLOW;
    }

    if (entries != NULL) {
        size_t output_index = 0U;

        offset = BACKUP_STREAM_HEADER_SIZE;
        for (index = 0U; index < total_entries; ++index) {
            struct config_kv_entry *entry = &entries[output_index];
            uint8_t key_length = bytes[offset + 1U];
            uint16_t value_length =
                backup_stream_load_le16(bytes + offset + 2U);

            entry->occupied = true;
            entry->kind = bytes[offset];
            entry->key_length = key_length;
            backup_stream_copy_bytes(
                entry->key,
                bytes + offset + BACKUP_STREAM_FRAME_HEADER_SIZE,
                key_length);
            entry->value_length = value_length;
            backup_stream_copy_bytes(
                entry->value,
                bytes + offset + BACKUP_STREAM_FRAME_HEADER_SIZE +
                    key_length,
                value_length);
            entry->revision = backup_stream_load_le64(
                bytes + offset + 4U);
            offset += BACKUP_STREAM_FRAME_HEADER_SIZE +
                      key_length + value_length;
            ++output_index;
        }
    }

    *entry_count_out = total_entries;
    *counts_out = counts;
    return BACKUP_STREAM_OK;
}

const char *backup_stream_status_string(enum backup_stream_status status)
{
    switch (status) {
    case BACKUP_STREAM_OK:
        return "BACKUP_STREAM_OK";
    case BACKUP_STREAM_NULL:
        return "BACKUP_STREAM_NULL";
    case BACKUP_STREAM_BAD_STATE:
        return "BACKUP_STREAM_BAD_STATE";
    case BACKUP_STREAM_TOO_SMALL:
        return "BACKUP_STREAM_TOO_SMALL";
    case BACKUP_STREAM_OVERFLOW:
        return "BACKUP_STREAM_OVERFLOW";
    case BACKUP_STREAM_UNSUPPORTED:
        return "BACKUP_STREAM_UNSUPPORTED";
    case BACKUP_STREAM_CORRUPT:
        return "BACKUP_STREAM_CORRUPT";
    default:
        return "BACKUP_STREAM_UNKNOWN";
    }
}
