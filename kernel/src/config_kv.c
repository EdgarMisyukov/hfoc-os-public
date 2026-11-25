#include "config_kv.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

static void config_kv_zero_bytes(unsigned char *bytes, size_t length)
{
    size_t index;

    for (index = 0U; index < length; ++index) {
        bytes[index] = 0U;
    }
}

static void config_kv_copy_bytes(unsigned char *destination,
                                 const unsigned char *source,
                                 size_t length)
{
    size_t index;

    for (index = 0U; index < length; ++index) {
        destination[index] = source[index];
    }
}

static bool config_kv_bytes_equal(const unsigned char *left,
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

static void config_kv_zero_entry(struct config_kv_entry *entry)
{
    config_kv_zero_bytes((unsigned char *)entry,
                         sizeof(*entry));
}

static bool config_kv_key_valid(uint8_t kind,
                                const void *key,
                                size_t key_length)
{
    return kind < CONFIG_KV_KIND_COUNT &&
           key != NULL &&
           key_length > 0U &&
           key_length <= CONFIG_KV_KEY_MAX;
}

static bool config_kv_entry_matches(const struct config_kv_entry *entry,
                                    uint8_t kind,
                                    const unsigned char *key,
                                    size_t key_length)
{
    return entry->occupied &&
           entry->kind == kind &&
           entry->key_length == key_length &&
           config_kv_bytes_equal(entry->key, key, key_length);
}

static struct config_kv_entry *config_kv_find(
    struct config_kv *kv,
    uint8_t kind,
    const unsigned char *key,
    size_t key_length)
{
    size_t index;

    for (index = 0U; index < kv->capacity; ++index) {
        if (config_kv_entry_matches(&kv->entries[index], kind,
                                    key, key_length)) {
            return &kv->entries[index];
        }
    }
    return NULL;
}

enum config_kv_status config_kv_init(
    struct config_kv *kv,
    struct config_kv_entry *entries,
    size_t capacity)
{
    size_t index;

    if (kv == NULL || entries == NULL) {
        return CONFIG_KV_NULL;
    }
    if (capacity == 0U || capacity > CONFIG_KV_CAPACITY_MAX) {
        return CONFIG_KV_BAD_CAPACITY;
    }
    for (index = 0U; index < capacity; ++index) {
        config_kv_zero_entry(&entries[index]);
    }
    kv->entries = entries;
    kv->capacity = capacity;
    kv->count = 0U;
    kv->initialized = true;
    return CONFIG_KV_OK;
}

enum config_kv_status config_kv_put(
    struct config_kv *kv,
    uint8_t kind,
    const void *key,
    size_t key_length,
    const void *value,
    size_t value_length,
    bool *replaced)
{
    struct config_kv_entry *entry;
    struct config_kv_entry *empty;
    size_t index;

    if (kv == NULL || replaced == NULL) {
        return CONFIG_KV_NULL;
    }
    if (!kv->initialized || kv->entries == NULL ||
        kv->capacity == 0U || kv->capacity > CONFIG_KV_CAPACITY_MAX ||
        kv->count > kv->capacity) {
        return CONFIG_KV_BAD_STATE;
    }
    if (!config_kv_key_valid(kind, key, key_length)) {
        return kind >= CONFIG_KV_KIND_COUNT ? CONFIG_KV_BAD_KIND :
               key == NULL ? CONFIG_KV_NULL :
               key_length == 0U ? CONFIG_KV_BAD_KEY :
                                  CONFIG_KV_BAD_KEY;
    }
    if (value == NULL) {
        return CONFIG_KV_NULL;
    }
    if (value_length > CONFIG_KV_VALUE_MAX) {
        return CONFIG_KV_BAD_VALUE;
    }

    *replaced = false;
    entry = config_kv_find(kv, kind, (const unsigned char *)key,
                           key_length);
    if (entry != NULL) {
        *replaced = true;
        entry->value_length = (uint16_t)value_length;
        config_kv_copy_bytes(entry->value, (const unsigned char *)value,
                             value_length);
        ++entry->revision;
        return CONFIG_KV_OK;
    }

    if (kv->count >= kv->capacity) {
        return CONFIG_KV_FULL;
    }
    empty = NULL;
    for (index = 0U; index < kv->capacity; ++index) {
        if (!kv->entries[index].occupied) {
            empty = &kv->entries[index];
            break;
        }
    }
    if (empty == NULL) {
        return CONFIG_KV_FULL;
    }

    empty->occupied = true;
    empty->kind = kind;
    empty->key_length = (uint8_t)key_length;
    config_kv_copy_bytes(empty->key, (const unsigned char *)key,
                         key_length);
    empty->value_length = (uint16_t)value_length;
    config_kv_copy_bytes(empty->value, (const unsigned char *)value,
                         value_length);
    empty->revision = 1U;
    ++kv->count;
    return CONFIG_KV_OK;
}

enum config_kv_status config_kv_get(
    const struct config_kv *kv,
    uint8_t kind,
    const void *key,
    size_t key_length,
    void *value,
    size_t capacity,
    size_t *value_length,
    uint64_t *revision)
{
    struct config_kv_entry *entry;

    if (kv == NULL || value == NULL || value_length == NULL ||
        revision == NULL) {
        return CONFIG_KV_NULL;
    }
    if (!kv->initialized || kv->entries == NULL ||
        kv->capacity == 0U || kv->capacity > CONFIG_KV_CAPACITY_MAX ||
        kv->count > kv->capacity) {
        return CONFIG_KV_BAD_STATE;
    }
    if (!config_kv_key_valid(kind, key, key_length)) {
        return kind >= CONFIG_KV_KIND_COUNT ? CONFIG_KV_BAD_KIND :
               key == NULL ? CONFIG_KV_NULL :
                             CONFIG_KV_BAD_KEY;
    }

    entry = config_kv_find((struct config_kv *)kv, kind,
                           (const unsigned char *)key, key_length);
    if (entry == NULL) {
        return CONFIG_KV_NOT_FOUND;
    }
    if (capacity < entry->value_length) {
        return CONFIG_KV_TRUNCATED;
    }
    *value_length = entry->value_length;
    *revision = entry->revision;
    config_kv_copy_bytes((unsigned char *)value, entry->value,
                         entry->value_length);
    return CONFIG_KV_OK;
}

enum config_kv_status config_kv_remove(
    struct config_kv *kv,
    uint8_t kind,
    const void *key,
    size_t key_length,
    bool *removed)
{
    struct config_kv_entry *entry;

    if (kv == NULL || removed == NULL) {
        return CONFIG_KV_NULL;
    }
    if (!kv->initialized || kv->entries == NULL ||
        kv->capacity == 0U || kv->capacity > CONFIG_KV_CAPACITY_MAX ||
        kv->count > kv->capacity) {
        return CONFIG_KV_BAD_STATE;
    }
    if (!config_kv_key_valid(kind, key, key_length)) {
        return kind >= CONFIG_KV_KIND_COUNT ? CONFIG_KV_BAD_KIND :
               key == NULL ? CONFIG_KV_NULL :
                             CONFIG_KV_BAD_KEY;
    }

    entry = config_kv_find(kv, kind, (const unsigned char *)key,
                           key_length);
    if (entry == NULL) {
        *removed = false;
        return CONFIG_KV_NOT_FOUND;
    }
    *removed = true;
    config_kv_zero_entry(entry);
    --kv->count;
    return CONFIG_KV_OK;
}

enum config_kv_status config_kv_count(
    const struct config_kv *kv,
    size_t *count)
{
    if (kv == NULL || count == NULL) {
        return CONFIG_KV_NULL;
    }
    if (!kv->initialized || kv->entries == NULL ||
        kv->capacity == 0U || kv->capacity > CONFIG_KV_CAPACITY_MAX ||
        kv->count > kv->capacity) {
        return CONFIG_KV_BAD_STATE;
    }
    *count = kv->count;
    return CONFIG_KV_OK;
}

const char *config_kv_status_string(enum config_kv_status status)
{
    switch (status) {
    case CONFIG_KV_OK:
        return "CONFIG_KV_OK";
    case CONFIG_KV_NULL:
        return "CONFIG_KV_NULL";
    case CONFIG_KV_BAD_CAPACITY:
        return "CONFIG_KV_BAD_CAPACITY";
    case CONFIG_KV_BAD_KIND:
        return "CONFIG_KV_BAD_KIND";
    case CONFIG_KV_BAD_KEY:
        return "CONFIG_KV_BAD_KEY";
    case CONFIG_KV_BAD_VALUE:
        return "CONFIG_KV_BAD_VALUE";
    case CONFIG_KV_BAD_STATE:
        return "CONFIG_KV_BAD_STATE";
    case CONFIG_KV_NOT_FOUND:
        return "CONFIG_KV_NOT_FOUND";
    case CONFIG_KV_FULL:
        return "CONFIG_KV_FULL";
    case CONFIG_KV_TRUNCATED:
        return "CONFIG_KV_TRUNCATED";
    default:
        return "CONFIG_KV_UNKNOWN";
    }
}
