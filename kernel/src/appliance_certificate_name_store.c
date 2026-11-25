#include "appliance_certificate_name_store.h"

#include "appliance_route_table.h"
#include "sha256.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/*
 * ITEM-507 increment 2: the name -> certificate/key model and its durable
 * record.  The model reuses the route table's public DNS-name normalizer, so a
 * name a route carries and a name a certificate is declared for are the same
 * name by construction; the store is the same shape as the ITEM-504 route
 * store, with the private key treated as bytes that are never rendered.
 */

static void appliance_certificate_name_store_zero(
    void *buffer,
    size_t length)
{
    memset(buffer, 0, length);
}

static void appliance_certificate_name_store_put_u16(
    uint8_t *output,
    uint16_t value)
{
    output[0] = (uint8_t)(value & 0xFFU);
    output[1] = (uint8_t)((value >> 8U) & 0xFFU);
}

static void appliance_certificate_name_store_put_u32(
    uint8_t *output,
    uint32_t value)
{
    output[0] = (uint8_t)(value & 0xFFU);
    output[1] = (uint8_t)((value >> 8U) & 0xFFU);
    output[2] = (uint8_t)((value >> 16U) & 0xFFU);
    output[3] = (uint8_t)((value >> 24U) & 0xFFU);
}

static uint16_t appliance_certificate_name_store_get_u16(
    const uint8_t *input)
{
    return (uint16_t)((uint16_t)input[0] |
                      ((uint16_t)input[1] << 8U));
}

static uint32_t appliance_certificate_name_store_get_u32(
    const uint8_t *input)
{
    return (uint32_t)input[0] |
           ((uint32_t)input[1] << 8U) |
           ((uint32_t)input[2] << 16U) |
           ((uint32_t)input[3] << 24U);
}

static bool appliance_certificate_name_store_record_is_blank(
    const uint8_t *record)
{
    size_t index;

    for (index = 0U;
         index < APPLIANCE_CERTIFICATE_NAME_STORE_RECORD_BYTES;
         ++index) {
        if (record[index] != 0U) {
            return false;
        }
    }
    return true;
}

static enum appliance_certificate_name_status
appliance_certificate_name_name_status(
    enum appliance_route_status status)
{
    switch (status) {
    case APPLIANCE_ROUTE_OK:
        return APPLIANCE_CERTIFICATE_NAME_OK;
    case APPLIANCE_ROUTE_NULL:
        return APPLIANCE_CERTIFICATE_NAME_NULL;
    case APPLIANCE_ROUTE_NAME_TOO_LONG:
        return APPLIANCE_CERTIFICATE_NAME_NAME_TOO_LONG;
    case APPLIANCE_ROUTE_BUFFER_TOO_SMALL:
        return APPLIANCE_CERTIFICATE_NAME_BUFFER_TOO_SMALL;
    case APPLIANCE_ROUTE_BAD_NAME:
    default:
        return APPLIANCE_CERTIFICATE_NAME_BAD_NAME;
    }
}

/*
 * Normalize one bare name with the route table's own rule (lower case, one
 * trailing dot removed, labels 1..63 of `[a-z0-9-]`, no leading or trailing
 * hyphen, no wildcard), so the certificate table and the route table agree on
 * what a name is.
 */
static enum appliance_certificate_name_status
appliance_certificate_name_normalize(
    const char *name,
    size_t name_length,
    char *out,
    size_t capacity,
    size_t *length_out)
{
    return appliance_certificate_name_name_status(
        appliance_route_name_normalize(
            name, name_length, out, capacity, length_out));
}

void appliance_certificate_name_table_init(
    struct appliance_certificate_name_table *table)
{
    if (table == NULL) {
        return;
    }
    appliance_certificate_name_store_zero(table, sizeof(*table));
    table->version = APPLIANCE_CERTIFICATE_NAME_STORE_VERSION;
    table->default_mode = APPLIANCE_CERTIFICATE_NAME_DEFAULT_REFUSE;
}

enum appliance_certificate_name_status
appliance_certificate_name_table_set_default(
    struct appliance_certificate_name_table *table,
    enum appliance_certificate_name_default default_mode)
{
    if (table == NULL) {
        return APPLIANCE_CERTIFICATE_NAME_NULL;
    }
    if (table->version != APPLIANCE_CERTIFICATE_NAME_STORE_VERSION) {
        return APPLIANCE_CERTIFICATE_NAME_BAD_VERSION;
    }
    if (default_mode != APPLIANCE_CERTIFICATE_NAME_DEFAULT_REFUSE &&
        default_mode != APPLIANCE_CERTIFICATE_NAME_DEFAULT_CERTIFICATE) {
        return APPLIANCE_CERTIFICATE_NAME_BAD_NAME;
    }
    table->default_mode = (uint32_t)default_mode;
    return APPLIANCE_CERTIFICATE_NAME_OK;
}

static bool appliance_certificate_name_payload_valid(
    const uint8_t *certificate_der,
    size_t certificate_length,
    const uint8_t *key_der,
    size_t key_length)
{
    return certificate_der != NULL && certificate_length != 0U &&
           certificate_length <= APPLIANCE_CERTIFICATE_NAME_DER_MAX &&
           key_der != NULL && key_length != 0U &&
           key_length <= APPLIANCE_CERTIFICATE_NAME_KEY_DER_MAX;
}

enum appliance_certificate_name_status appliance_certificate_name_table_add(
    struct appliance_certificate_name_table *table,
    const char *name,
    size_t name_length,
    const uint8_t *certificate_der,
    size_t certificate_length,
    const uint8_t *key_der,
    size_t key_length)
{
    char normalized[APPLIANCE_CERTIFICATE_NAME_MAX + 1U];
    size_t normalized_length = 0U;
    enum appliance_certificate_name_status status;
    uint32_t index;

    if (table == NULL) {
        return APPLIANCE_CERTIFICATE_NAME_NULL;
    }
    if (table->version != APPLIANCE_CERTIFICATE_NAME_STORE_VERSION) {
        return APPLIANCE_CERTIFICATE_NAME_BAD_VERSION;
    }
    status = appliance_certificate_name_normalize(
        name, name_length, normalized, sizeof(normalized),
        &normalized_length);
    if (status != APPLIANCE_CERTIFICATE_NAME_OK) {
        return status;
    }
    if (!appliance_certificate_name_payload_valid(
            certificate_der, certificate_length, key_der, key_length)) {
        if (certificate_der == NULL || certificate_length == 0U ||
            certificate_length > APPLIANCE_CERTIFICATE_NAME_DER_MAX) {
            return APPLIANCE_CERTIFICATE_NAME_BAD_CERTIFICATE;
        }
        return APPLIANCE_CERTIFICATE_NAME_BAD_KEY;
    }
    /*
     * The stored names are already normalized, so a byte comparison is a
     * case-insensitive comparison: `Example.COM` and `example.com` cannot both
     * be declared.
     */
    for (index = 0U; index < table->count; ++index) {
        if (table->entries[index].name_length == normalized_length &&
            memcmp(table->entries[index].name, normalized,
                   normalized_length) == 0) {
            return APPLIANCE_CERTIFICATE_NAME_DUPLICATE_NAME;
        }
    }
    if (table->count >= APPLIANCE_CERTIFICATE_NAME_MAX_COUNT) {
        return APPLIANCE_CERTIFICATE_NAME_TABLE_FULL;
    }
    {
        struct appliance_certificate_name *entry =
            &table->entries[table->count];

        appliance_certificate_name_store_zero(entry, sizeof(*entry));
        memcpy(entry->name, normalized, normalized_length);
        entry->name[normalized_length] = '\0';
        entry->name_length = (uint32_t)normalized_length;
        memcpy(entry->certificate_der, certificate_der, certificate_length);
        entry->certificate_length = (uint32_t)certificate_length;
        memcpy(entry->key_der, key_der, key_length);
        entry->key_length = (uint32_t)key_length;
    }
    ++table->count;
    return APPLIANCE_CERTIFICATE_NAME_OK;
}

enum appliance_certificate_name_status
appliance_certificate_name_table_remove(
    struct appliance_certificate_name_table *table,
    const char *name,
    size_t name_length)
{
    char normalized[APPLIANCE_CERTIFICATE_NAME_MAX + 1U];
    size_t normalized_length = 0U;
    enum appliance_certificate_name_status status;
    uint32_t index;

    if (table == NULL) {
        return APPLIANCE_CERTIFICATE_NAME_NULL;
    }
    if (table->version != APPLIANCE_CERTIFICATE_NAME_STORE_VERSION) {
        return APPLIANCE_CERTIFICATE_NAME_BAD_VERSION;
    }
    status = appliance_certificate_name_normalize(
        name, name_length, normalized, sizeof(normalized),
        &normalized_length);
    if (status != APPLIANCE_CERTIFICATE_NAME_OK) {
        return status;
    }
    for (index = 0U; index < table->count; ++index) {
        if (table->entries[index].name_length != normalized_length ||
            memcmp(table->entries[index].name, normalized,
                   normalized_length) != 0) {
            continue;
        }
        /*
         * The removed slot is zeroed before the tail moves down and the last
         * slot is zeroed again: a deleted key never stays in the table's
         * memory under a name that no longer exists.
         */
        appliance_certificate_name_store_zero(
            &table->entries[index], sizeof(table->entries[index]));
        if (index + 1U < table->count) {
            memmove(&table->entries[index],
                    &table->entries[index + 1U],
                    (size_t)(table->count - index - 1U) *
                        sizeof(table->entries[0]));
        }
        appliance_certificate_name_store_zero(
            &table->entries[table->count - 1U],
            sizeof(table->entries[0]));
        --table->count;
        return APPLIANCE_CERTIFICATE_NAME_OK;
    }
    return APPLIANCE_CERTIFICATE_NAME_NOT_FOUND;
}

enum appliance_certificate_name_status
appliance_certificate_name_table_lookup(
    const struct appliance_certificate_name_table *table,
    const char *name,
    size_t name_length,
    const struct appliance_certificate_name **entry_out)
{
    char normalized[APPLIANCE_CERTIFICATE_NAME_MAX + 1U];
    size_t normalized_length = 0U;
    enum appliance_certificate_name_status status;
    uint32_t index;

    if (entry_out != NULL) {
        *entry_out = NULL;
    }
    if (table == NULL) {
        return APPLIANCE_CERTIFICATE_NAME_NULL;
    }
    if (table->version != APPLIANCE_CERTIFICATE_NAME_STORE_VERSION) {
        return APPLIANCE_CERTIFICATE_NAME_BAD_VERSION;
    }
    status = appliance_certificate_name_normalize(
        name, name_length, normalized, sizeof(normalized),
        &normalized_length);
    if (status != APPLIANCE_CERTIFICATE_NAME_OK) {
        return status;
    }
    for (index = 0U; index < table->count; ++index) {
        if (table->entries[index].name_length == normalized_length &&
            memcmp(table->entries[index].name, normalized,
                   normalized_length) == 0) {
            if (entry_out != NULL) {
                *entry_out = &table->entries[index];
            }
            return APPLIANCE_CERTIFICATE_NAME_OK;
        }
    }
    return APPLIANCE_CERTIFICATE_NAME_NOT_FOUND;
}

enum appliance_certificate_name_status
appliance_certificate_name_table_validate(
    const struct appliance_certificate_name_table *table)
{
    uint32_t index;
    uint32_t other;

    if (table == NULL) {
        return APPLIANCE_CERTIFICATE_NAME_NULL;
    }
    if (table->version != APPLIANCE_CERTIFICATE_NAME_STORE_VERSION) {
        return APPLIANCE_CERTIFICATE_NAME_BAD_VERSION;
    }
    if (table->default_mode !=
            APPLIANCE_CERTIFICATE_NAME_DEFAULT_REFUSE &&
        table->default_mode !=
            APPLIANCE_CERTIFICATE_NAME_DEFAULT_CERTIFICATE) {
        return APPLIANCE_CERTIFICATE_NAME_BAD_NAME;
    }
    if (table->count > APPLIANCE_CERTIFICATE_NAME_MAX_COUNT) {
        return APPLIANCE_CERTIFICATE_NAME_TABLE_FULL;
    }
    for (index = 0U; index < table->count; ++index) {
        const struct appliance_certificate_name *entry =
            &table->entries[index];
        char normalized[APPLIANCE_CERTIFICATE_NAME_MAX + 1U];
        size_t normalized_length = 0U;
        enum appliance_certificate_name_status status;

        if (entry->name_length == 0U ||
            entry->name_length > APPLIANCE_CERTIFICATE_NAME_MAX ||
            entry->name[entry->name_length] != '\0') {
            return APPLIANCE_CERTIFICATE_NAME_BAD_NAME;
        }
        if (!appliance_certificate_name_payload_valid(
                entry->certificate_der, entry->certificate_length,
                entry->key_der, entry->key_length)) {
            if (entry->certificate_length == 0U ||
                entry->certificate_length >
                    APPLIANCE_CERTIFICATE_NAME_DER_MAX) {
                return APPLIANCE_CERTIFICATE_NAME_BAD_CERTIFICATE;
            }
            return APPLIANCE_CERTIFICATE_NAME_BAD_KEY;
        }
        status = appliance_certificate_name_normalize(
            entry->name, entry->name_length, normalized,
            sizeof(normalized), &normalized_length);
        if (status != APPLIANCE_CERTIFICATE_NAME_OK) {
            return status;
        }
        if (normalized_length != entry->name_length ||
            memcmp(normalized, entry->name, normalized_length) != 0) {
            /*
             * The record stores names normalized; a name that normalizes to
             * something else is a record this model did not write.
             */
            return APPLIANCE_CERTIFICATE_NAME_BAD_NAME;
        }
        for (other = 0U; other < index; ++other) {
            if (table->entries[other].name_length == entry->name_length &&
                memcmp(table->entries[other].name, entry->name,
                       entry->name_length) == 0) {
                return APPLIANCE_CERTIFICATE_NAME_DUPLICATE_NAME;
            }
        }
    }
    return APPLIANCE_CERTIFICATE_NAME_OK;
}

static size_t appliance_certificate_name_render_append(
    char *buffer,
    size_t offset,
    const char *text)
{
    size_t index = 0U;

    while (text[index] != '\0') {
        buffer[offset + index] = text[index];
        ++index;
    }
    return offset + index;
}

static size_t appliance_certificate_name_render_number(
    char *buffer,
    size_t offset,
    uint32_t value)
{
    char digits[10];
    size_t count = 0U;
    size_t index;

    if (value == 0U) {
        buffer[offset] = '0';
        return offset + 1U;
    }
    while (value != 0U && count < sizeof(digits)) {
        digits[count] = (char)('0' + (value % 10U));
        value /= 10U;
        ++count;
    }
    for (index = 0U; index < count; ++index) {
        buffer[offset + index] = digits[count - 1U - index];
    }
    return offset + count;
}

enum appliance_certificate_name_status appliance_certificate_name_render(
    const struct appliance_certificate_name *entry,
    char *buffer,
    size_t buffer_size,
    size_t *length_out)
{
    size_t offset;

    if (entry == NULL || buffer == NULL || length_out == NULL) {
        return APPLIANCE_CERTIFICATE_NAME_NULL;
    }
    *length_out = 0U;
    offset = appliance_certificate_name_render_append(buffer, 0U, "name=");
    if (offset + entry->name_length >= buffer_size) {
        return APPLIANCE_CERTIFICATE_NAME_BUFFER_TOO_SMALL;
    }
    memcpy(buffer + offset, entry->name, entry->name_length);
    offset += entry->name_length;
    offset = appliance_certificate_name_render_append(
        buffer, offset, " certificate_bytes=");
    offset = appliance_certificate_name_render_number(
        buffer, offset, entry->certificate_length);
    offset = appliance_certificate_name_render_append(
        buffer, offset, " key=present");
    if (offset >= buffer_size) {
        return APPLIANCE_CERTIFICATE_NAME_BUFFER_TOO_SMALL;
    }
    buffer[offset] = '\0';
    *length_out = offset;
    return APPLIANCE_CERTIFICATE_NAME_OK;
}

enum appliance_certificate_name_status appliance_certificate_name_select(
    const struct appliance_certificate_name_table *table,
    const char *name,
    size_t name_length,
    bool name_is_routed,
    enum appliance_certificate_name_selection *selection_out,
    const struct appliance_certificate_name **entry_out)
{
    enum appliance_certificate_name_status status;

    if (entry_out != NULL) {
        *entry_out = NULL;
    }
    if (selection_out != NULL) {
        *selection_out = APPLIANCE_CERTIFICATE_NAME_SELECTION_REFUSED;
    }
    if (table == NULL || selection_out == NULL) {
        return APPLIANCE_CERTIFICATE_NAME_NULL;
    }
    if (table->version != APPLIANCE_CERTIFICATE_NAME_STORE_VERSION) {
        return APPLIANCE_CERTIFICATE_NAME_BAD_VERSION;
    }
    /*
     * A client that presents no name is not asking for a name the table does
     * not carry: it gets the appliance's own declared identity.  Only a name
     * the operator did not declare is refused.
     */
    if (name == NULL || name_length == 0U) {
        *selection_out = APPLIANCE_CERTIFICATE_NAME_SELECTION_DEFAULT;
        return APPLIANCE_CERTIFICATE_NAME_OK;
    }
    status = appliance_certificate_name_table_lookup(
        table, name, name_length, entry_out);
    if (status == APPLIANCE_CERTIFICATE_NAME_OK) {
        *selection_out = APPLIANCE_CERTIFICATE_NAME_SELECTION_EXACT;
        return status;
    }
    if (status == APPLIANCE_CERTIFICATE_NAME_NOT_FOUND ||
        status == APPLIANCE_CERTIFICATE_NAME_BAD_NAME) {
        /*
         * ITEM-507 increment 4: a name the facade routes to a backend must
         * have a certificate of its own.  A default certificate is not a
         * substitute for a routed name, so the handshake is refused and the
         * reason names the missing declaration.
         */
        if (name_is_routed) {
            *selection_out = APPLIANCE_CERTIFICATE_NAME_SELECTION_REFUSED;
            return APPLIANCE_CERTIFICATE_NAME_ROUTE_WITHOUT_CERTIFICATE;
        }
        *selection_out =
            table->default_mode ==
                    APPLIANCE_CERTIFICATE_NAME_DEFAULT_CERTIFICATE
                ? APPLIANCE_CERTIFICATE_NAME_SELECTION_DEFAULT
                : APPLIANCE_CERTIFICATE_NAME_SELECTION_REFUSED;
        return status;
    }
    return status;
}

void appliance_certificate_name_store_init(
    struct appliance_certificate_name_store *store)
{
    if (store == NULL) {
        return;
    }
    appliance_certificate_name_store_zero(store, sizeof(*store));
    store->initialized = true;
    store->version = APPLIANCE_CERTIFICATE_NAME_STORE_VERSION;
}

bool appliance_certificate_name_store_available(
    const struct appliance_certificate_name_store *store)
{
    return store != NULL && store->initialized && store->bound;
}

enum appliance_certificate_name_store_source
appliance_certificate_name_store_source(
    const struct appliance_certificate_name_store *store)
{
    if (store == NULL || !store->initialized) {
        return APPLIANCE_CERTIFICATE_NAME_STORE_SOURCE_NONE;
    }
    return store->source;
}

enum appliance_certificate_name_store_status
appliance_certificate_name_store_last_status(
    const struct appliance_certificate_name_store *store)
{
    if (store == NULL || !store->initialized) {
        return APPLIANCE_CERTIFICATE_NAME_STORE_NULL;
    }
    return store->last_status;
}

uint32_t appliance_certificate_name_store_save_count(
    const struct appliance_certificate_name_store *store)
{
    if (store == NULL || !store->initialized) {
        return 0U;
    }
    return store->save_count;
}

/*
 * The record sits after the twelve blocks of the ITEM-503 certificate record
 * in the same CERTIFICATE-namespace store.  A sealed volume is refused for the
 * same reason the ITEM-503 and ITEM-504 stores refuse it: a private key must
 * never be written in the clear to a volume the appliance claims to protect.
 */
static bool appliance_certificate_name_store_bind(
    struct appliance_certificate_name_store *store,
    struct appliance_state_loader *loader)
{
    const struct appliance_state_layout_entry *entry = NULL;

    if (loader == NULL || !loader->ready) {
        store->last_status =
            APPLIANCE_CERTIFICATE_NAME_STORE_STORE_UNAVAILABLE;
        return false;
    }
#if APPLIANCE_STATE_ENCRYPTION_ENABLED
    if (loader->encryption_sealed) {
        store->last_status =
            APPLIANCE_CERTIFICATE_NAME_STORE_STORE_SEALED;
        return false;
    }
#endif
    if (appliance_state_layout_find(
            &loader->layout,
            APPLIANCE_PERSISTENT_STORAGE_NAMESPACE_ID_CERTIFICATE,
            APPLIANCE_STATE_STORE_ID_CERTIFICATE,
            &entry) != APPLIANCE_STATE_LAYOUT_OK ||
        entry == NULL) {
        store->last_status =
            APPLIANCE_CERTIFICATE_NAME_STORE_STORE_UNAVAILABLE;
        return false;
    }
    if (entry->block_count <
        (uint64_t)APPLIANCE_CERTIFICATE_NAME_STORE_BLOCK_OFFSET +
            APPLIANCE_CERTIFICATE_NAME_STORE_RECORD_BLOCKS) {
        store->last_status =
            APPLIANCE_CERTIFICATE_NAME_STORE_STORE_UNAVAILABLE;
        return false;
    }
    if (appliance_state_block_device_bind_range(
            &loader->partition.device,
            entry->block_offset +
                APPLIANCE_CERTIFICATE_NAME_STORE_BLOCK_OFFSET,
            APPLIANCE_CERTIFICATE_NAME_STORE_RECORD_BLOCKS,
            &store->store) != APPLIANCE_STATE_BLOCK_DEVICE_OK) {
        store->last_status =
            APPLIANCE_CERTIFICATE_NAME_STORE_STORE_UNAVAILABLE;
        return false;
    }
    store->bound = true;
    return true;
}

static enum appliance_certificate_name_store_status
appliance_certificate_name_store_encode(
    const struct appliance_certificate_name_table *table,
    uint8_t *record,
    size_t *length_out)
{
    uint8_t digest[SHA256_DIGEST_SIZE];
    size_t offset =
        APPLIANCE_CERTIFICATE_NAME_STORE_HEADER_BYTES +
        APPLIANCE_CERTIFICATE_NAME_STORE_BODY_FLAGS_BYTES;
    uint32_t index;

    if (appliance_certificate_name_table_validate(table) !=
        APPLIANCE_CERTIFICATE_NAME_OK) {
        return APPLIANCE_CERTIFICATE_NAME_STORE_TABLE_REFUSED;
    }
    appliance_certificate_name_store_zero(
        record, APPLIANCE_CERTIFICATE_NAME_STORE_RECORD_BYTES);
    appliance_certificate_name_store_put_u32(
        record + APPLIANCE_CERTIFICATE_NAME_STORE_HEADER_BYTES,
        table->default_mode);
    for (index = 0U; index < table->count; ++index) {
        const struct appliance_certificate_name *entry =
            &table->entries[index];

        if (offset + APPLIANCE_CERTIFICATE_NAME_STORE_ENTRY_FIXED_BYTES +
                (size_t)entry->name_length +
                (size_t)entry->certificate_length +
                (size_t)entry->key_length >
            APPLIANCE_CERTIFICATE_NAME_STORE_RECORD_BYTES) {
            return APPLIANCE_CERTIFICATE_NAME_STORE_TABLE_TOO_LARGE;
        }
        appliance_certificate_name_store_put_u16(
            record + offset, (uint16_t)entry->name_length);
        appliance_certificate_name_store_put_u16(
            record + offset + 2U, (uint16_t)entry->certificate_length);
        appliance_certificate_name_store_put_u16(
            record + offset + 4U, (uint16_t)entry->key_length);
        appliance_certificate_name_store_put_u16(
            record + offset + 6U, 0U);
        memcpy(record + offset +
                   APPLIANCE_CERTIFICATE_NAME_STORE_ENTRY_FIXED_BYTES,
               entry->name, entry->name_length);
        memcpy(record + offset +
                   APPLIANCE_CERTIFICATE_NAME_STORE_ENTRY_FIXED_BYTES +
                   entry->name_length,
               entry->certificate_der, entry->certificate_length);
        memcpy(record + offset +
                   APPLIANCE_CERTIFICATE_NAME_STORE_ENTRY_FIXED_BYTES +
                   entry->name_length + entry->certificate_length,
               entry->key_der, entry->key_length);
        offset += APPLIANCE_CERTIFICATE_NAME_STORE_ENTRY_FIXED_BYTES +
                  (size_t)entry->name_length +
                  (size_t)entry->certificate_length +
                  (size_t)entry->key_length;
    }
    if (sha256_compute(
            record + APPLIANCE_CERTIFICATE_NAME_STORE_HEADER_BYTES,
            offset - APPLIANCE_CERTIFICATE_NAME_STORE_HEADER_BYTES,
            digest) != SHA256_OK) {
        return APPLIANCE_CERTIFICATE_NAME_STORE_RECORD_CORRUPT;
    }
    appliance_certificate_name_store_put_u32(
        record, APPLIANCE_CERTIFICATE_NAME_STORE_MAGIC);
    appliance_certificate_name_store_put_u32(
        record + 4U, APPLIANCE_CERTIFICATE_NAME_STORE_RECORD_VERSION);
    appliance_certificate_name_store_put_u32(
        record + 8U, table->count);
    appliance_certificate_name_store_put_u32(
        record + 12U,
        appliance_certificate_name_store_get_u32(digest));
    *length_out = offset;
    return APPLIANCE_CERTIFICATE_NAME_STORE_OK;
}

static enum appliance_certificate_name_store_status
appliance_certificate_name_store_decode(
    const uint8_t *record,
    struct appliance_certificate_name_table *table_out)
{
    uint8_t digest[SHA256_DIGEST_SIZE];
    size_t offset =
        APPLIANCE_CERTIFICATE_NAME_STORE_HEADER_BYTES +
        APPLIANCE_CERTIFICATE_NAME_STORE_BODY_FLAGS_BYTES;
    uint32_t count;
    uint32_t index;

    if (appliance_certificate_name_store_get_u32(record) !=
            APPLIANCE_CERTIFICATE_NAME_STORE_MAGIC ||
        appliance_certificate_name_store_get_u32(record + 4U) !=
            APPLIANCE_CERTIFICATE_NAME_STORE_RECORD_VERSION) {
        return APPLIANCE_CERTIFICATE_NAME_STORE_RECORD_CORRUPT;
    }
    count = appliance_certificate_name_store_get_u32(record + 8U);
    if (count > APPLIANCE_CERTIFICATE_NAME_MAX_COUNT) {
        return APPLIANCE_CERTIFICATE_NAME_STORE_RECORD_CORRUPT;
    }
    /*
     * The body is walked by its own lengths first, so the checksum covers
     * exactly the bytes the count and the lengths claim, and a truncated
     * record is refused instead of hashing into the neighbouring blocks.
     */
    for (index = 0U; index < count; ++index) {
        uint16_t name_length;
        uint16_t certificate_length;
        uint16_t key_length;
        size_t entry_bytes;

        if (offset + APPLIANCE_CERTIFICATE_NAME_STORE_ENTRY_FIXED_BYTES >
            APPLIANCE_CERTIFICATE_NAME_STORE_RECORD_BYTES) {
            return APPLIANCE_CERTIFICATE_NAME_STORE_RECORD_CORRUPT;
        }
        name_length = appliance_certificate_name_store_get_u16(
            record + offset);
        certificate_length = appliance_certificate_name_store_get_u16(
            record + offset + 2U);
        key_length = appliance_certificate_name_store_get_u16(
            record + offset + 4U);
        if (name_length == 0U ||
            name_length > APPLIANCE_CERTIFICATE_NAME_MAX ||
            certificate_length == 0U ||
            certificate_length > APPLIANCE_CERTIFICATE_NAME_DER_MAX ||
            key_length == 0U ||
            key_length > APPLIANCE_CERTIFICATE_NAME_KEY_DER_MAX) {
            return APPLIANCE_CERTIFICATE_NAME_STORE_RECORD_CORRUPT;
        }
        entry_bytes =
            APPLIANCE_CERTIFICATE_NAME_STORE_ENTRY_FIXED_BYTES +
            (size_t)name_length + (size_t)certificate_length +
            (size_t)key_length;
        if (entry_bytes >
            APPLIANCE_CERTIFICATE_NAME_STORE_RECORD_BYTES - offset) {
            return APPLIANCE_CERTIFICATE_NAME_STORE_RECORD_CORRUPT;
        }
        offset += entry_bytes;
    }
    if (sha256_compute(
            record + APPLIANCE_CERTIFICATE_NAME_STORE_HEADER_BYTES,
            offset - APPLIANCE_CERTIFICATE_NAME_STORE_HEADER_BYTES,
            digest) != SHA256_OK) {
        return APPLIANCE_CERTIFICATE_NAME_STORE_RECORD_CORRUPT;
    }
    if (appliance_certificate_name_store_get_u32(digest) !=
        appliance_certificate_name_store_get_u32(record + 12U)) {
        return APPLIANCE_CERTIFICATE_NAME_STORE_RECORD_CHECKSUM;
    }
    appliance_certificate_name_table_init(table_out);
    table_out->default_mode = appliance_certificate_name_store_get_u32(
        record + APPLIANCE_CERTIFICATE_NAME_STORE_HEADER_BYTES);
    offset = APPLIANCE_CERTIFICATE_NAME_STORE_HEADER_BYTES +
             APPLIANCE_CERTIFICATE_NAME_STORE_BODY_FLAGS_BYTES;
    for (index = 0U; index < count; ++index) {
        struct appliance_certificate_name *entry =
            &table_out->entries[index];
        uint16_t name_length = appliance_certificate_name_store_get_u16(
            record + offset);
        uint16_t certificate_length =
            appliance_certificate_name_store_get_u16(record + offset + 2U);
        uint16_t key_length =
            appliance_certificate_name_store_get_u16(record + offset + 4U);

        memcpy(
            entry->name,
            record + offset +
                APPLIANCE_CERTIFICATE_NAME_STORE_ENTRY_FIXED_BYTES,
            name_length);
        entry->name[name_length] = '\0';
        entry->name_length = name_length;
        memcpy(
            entry->certificate_der,
            record + offset +
                APPLIANCE_CERTIFICATE_NAME_STORE_ENTRY_FIXED_BYTES +
                name_length,
            certificate_length);
        entry->certificate_length = certificate_length;
        memcpy(
            entry->key_der,
            record + offset +
                APPLIANCE_CERTIFICATE_NAME_STORE_ENTRY_FIXED_BYTES +
                name_length + certificate_length,
            key_length);
        entry->key_length = key_length;
        offset += APPLIANCE_CERTIFICATE_NAME_STORE_ENTRY_FIXED_BYTES +
                  (size_t)name_length + (size_t)certificate_length +
                  (size_t)key_length;
    }
    table_out->count = count;
    if (appliance_certificate_name_table_validate(table_out) !=
        APPLIANCE_CERTIFICATE_NAME_OK) {
        appliance_certificate_name_table_init(table_out);
        return APPLIANCE_CERTIFICATE_NAME_STORE_TABLE_REFUSED;
    }
    return APPLIANCE_CERTIFICATE_NAME_STORE_OK;
}

enum appliance_certificate_name_store_status
appliance_certificate_name_store_load(
    struct appliance_certificate_name_store *store,
    struct appliance_state_loader *loader,
    struct appliance_certificate_name_table *table_out)
{
    static uint8_t record[APPLIANCE_CERTIFICATE_NAME_STORE_RECORD_BYTES];
    enum appliance_certificate_name_store_status status;

    if (table_out == NULL) {
        return APPLIANCE_CERTIFICATE_NAME_STORE_NULL;
    }
    appliance_certificate_name_table_init(table_out);
    if (store == NULL) {
        return APPLIANCE_CERTIFICATE_NAME_STORE_NULL;
    }
    if (!store->initialized ||
        store->version != APPLIANCE_CERTIFICATE_NAME_STORE_VERSION) {
        store->last_status = APPLIANCE_CERTIFICATE_NAME_STORE_BAD_STATE;
        return store->last_status;
    }
    store->source =
        APPLIANCE_CERTIFICATE_NAME_STORE_SOURCE_DEFAULT_UNAVAILABLE;
    if (!store->bound && !appliance_certificate_name_store_bind(store, loader)) {
        return store->last_status;
    }
    appliance_certificate_name_store_zero(record, sizeof(record));
    if (appliance_state_block_device_read(
            &store->store, 0U, record, sizeof(record)) != BLOCK_DEVICE_OK) {
        store->source =
            APPLIANCE_CERTIFICATE_NAME_STORE_SOURCE_DEFAULT_INVALID;
        store->last_status = APPLIANCE_CERTIFICATE_NAME_STORE_READ_FAILED;
        return store->last_status;
    }
    if (appliance_certificate_name_store_record_is_blank(record)) {
        /* Nothing was ever written here; that is an empty table, not a fault. */
        appliance_certificate_name_store_zero(record, sizeof(record));
        store->source =
            APPLIANCE_CERTIFICATE_NAME_STORE_SOURCE_DEFAULT_MISSING;
        store->last_status = APPLIANCE_CERTIFICATE_NAME_STORE_OK;
        ++store->load_count;
        return APPLIANCE_CERTIFICATE_NAME_STORE_OK;
    }
    status = appliance_certificate_name_store_decode(record, table_out);
    /*
     * The static record buffer held private keys while it was decoded; it is
     * zeroed before anything else can see it, so a later reader of this
     * module's memory does not find the keys of a table it did not load.
     */
    appliance_certificate_name_store_zero(record, sizeof(record));
    store->last_status = status;
    if (status != APPLIANCE_CERTIFICATE_NAME_STORE_OK) {
        store->source =
            APPLIANCE_CERTIFICATE_NAME_STORE_SOURCE_DEFAULT_INVALID;
        return status;
    }
    store->source = APPLIANCE_CERTIFICATE_NAME_STORE_SOURCE_COMMITTED;
    ++store->load_count;
    return APPLIANCE_CERTIFICATE_NAME_STORE_OK;
}

enum appliance_certificate_name_store_status
appliance_certificate_name_store_save(
    struct appliance_certificate_name_store *store,
    const struct appliance_certificate_name_table *table)
{
    static uint8_t record[APPLIANCE_CERTIFICATE_NAME_STORE_RECORD_BYTES];
    static uint8_t verify[APPLIANCE_CERTIFICATE_NAME_STORE_RECORD_BYTES];
    enum appliance_certificate_name_store_status status;
    size_t length = 0U;

    if (store == NULL || table == NULL) {
        return APPLIANCE_CERTIFICATE_NAME_STORE_NULL;
    }
    if (!store->initialized ||
        store->version != APPLIANCE_CERTIFICATE_NAME_STORE_VERSION) {
        store->last_status = APPLIANCE_CERTIFICATE_NAME_STORE_BAD_STATE;
        return store->last_status;
    }
    if (!store->bound) {
        store->last_status =
            APPLIANCE_CERTIFICATE_NAME_STORE_STORE_UNAVAILABLE;
        return store->last_status;
    }
    status = appliance_certificate_name_store_encode(table, record, &length);
    if (status != APPLIANCE_CERTIFICATE_NAME_STORE_OK ||
        length > APPLIANCE_CERTIFICATE_NAME_STORE_RECORD_BYTES) {
        store->last_status = status != APPLIANCE_CERTIFICATE_NAME_STORE_OK
            ? status
            : APPLIANCE_CERTIFICATE_NAME_STORE_TABLE_TOO_LARGE;
        return store->last_status;
    }
    if (appliance_state_block_device_write(
            &store->store, 0U, record, sizeof(record)) != BLOCK_DEVICE_OK ||
        appliance_state_block_device_flush(&store->store) != BLOCK_DEVICE_OK) {
        store->last_status = APPLIANCE_CERTIFICATE_NAME_STORE_WRITE_FAILED;
        return store->last_status;
    }
    /*
     * Read back before reporting success: the panel must not show a name the
     * next boot would not carry.
     */
    appliance_certificate_name_store_zero(verify, sizeof(verify));
    if (appliance_state_block_device_read(
            &store->store, 0U, verify, sizeof(verify)) != BLOCK_DEVICE_OK ||
        memcmp(verify, record, sizeof(record)) != 0) {
        /* Both buffers held private keys; neither keeps them. */
        appliance_certificate_name_store_zero(record, sizeof(record));
        appliance_certificate_name_store_zero(verify, sizeof(verify));
        store->last_status = APPLIANCE_CERTIFICATE_NAME_STORE_WRITE_FAILED;
        return store->last_status;
    }
    appliance_certificate_name_store_zero(record, sizeof(record));
    appliance_certificate_name_store_zero(verify, sizeof(verify));
    store->last_status = APPLIANCE_CERTIFICATE_NAME_STORE_OK;
    store->source = APPLIANCE_CERTIFICATE_NAME_STORE_SOURCE_COMMITTED;
    ++store->save_count;
    return APPLIANCE_CERTIFICATE_NAME_STORE_OK;
}

const char *appliance_certificate_name_status_string(
    enum appliance_certificate_name_status status)
{
    switch (status) {
    case APPLIANCE_CERTIFICATE_NAME_OK:
        return "OK";
    case APPLIANCE_CERTIFICATE_NAME_NULL:
        return "NULL";
    case APPLIANCE_CERTIFICATE_NAME_BAD_VERSION:
        return "BAD_VERSION";
    case APPLIANCE_CERTIFICATE_NAME_BAD_NAME:
        return "BAD_NAME";
    case APPLIANCE_CERTIFICATE_NAME_NAME_TOO_LONG:
        return "NAME_TOO_LONG";
    case APPLIANCE_CERTIFICATE_NAME_BAD_CERTIFICATE:
        return "BAD_CERTIFICATE";
    case APPLIANCE_CERTIFICATE_NAME_BAD_KEY:
        return "BAD_KEY";
    case APPLIANCE_CERTIFICATE_NAME_DUPLICATE_NAME:
        return "DUPLICATE_NAME";
    case APPLIANCE_CERTIFICATE_NAME_TABLE_FULL:
        return "TABLE_FULL";
    case APPLIANCE_CERTIFICATE_NAME_NOT_FOUND:
        return "NOT_FOUND";
    case APPLIANCE_CERTIFICATE_NAME_BUFFER_TOO_SMALL:
        return "BUFFER_TOO_SMALL";
    case APPLIANCE_CERTIFICATE_NAME_ROUTE_WITHOUT_CERTIFICATE:
        return "ROUTE_WITHOUT_CERTIFICATE";
    }
    return "UNKNOWN";
}

const char *appliance_certificate_name_store_source_string(
    enum appliance_certificate_name_store_source source)
{
    switch (source) {
    case APPLIANCE_CERTIFICATE_NAME_STORE_SOURCE_NONE:
        return "NONE";
    case APPLIANCE_CERTIFICATE_NAME_STORE_SOURCE_COMMITTED:
        return "COMMITTED";
    case APPLIANCE_CERTIFICATE_NAME_STORE_SOURCE_DEFAULT_MISSING:
        return "DEFAULT_MISSING";
    case APPLIANCE_CERTIFICATE_NAME_STORE_SOURCE_DEFAULT_INVALID:
        return "DEFAULT_INVALID";
    case APPLIANCE_CERTIFICATE_NAME_STORE_SOURCE_DEFAULT_UNAVAILABLE:
        return "DEFAULT_UNAVAILABLE";
    }
    return "UNKNOWN";
}

const char *appliance_certificate_name_store_status_string(
    enum appliance_certificate_name_store_status status)
{
    switch (status) {
    case APPLIANCE_CERTIFICATE_NAME_STORE_OK:
        return "OK";
    case APPLIANCE_CERTIFICATE_NAME_STORE_NULL:
        return "NULL";
    case APPLIANCE_CERTIFICATE_NAME_STORE_BAD_STATE:
        return "BAD_STATE";
    case APPLIANCE_CERTIFICATE_NAME_STORE_STORE_UNAVAILABLE:
        return "STORE_UNAVAILABLE";
    case APPLIANCE_CERTIFICATE_NAME_STORE_STORE_SEALED:
        return "STORE_SEALED";
    case APPLIANCE_CERTIFICATE_NAME_STORE_READ_FAILED:
        return "READ_FAILED";
    case APPLIANCE_CERTIFICATE_NAME_STORE_WRITE_FAILED:
        return "WRITE_FAILED";
    case APPLIANCE_CERTIFICATE_NAME_STORE_RECORD_CORRUPT:
        return "RECORD_CORRUPT";
    case APPLIANCE_CERTIFICATE_NAME_STORE_RECORD_CHECKSUM:
        return "RECORD_CHECKSUM";
    case APPLIANCE_CERTIFICATE_NAME_STORE_TABLE_REFUSED:
        return "TABLE_REFUSED";
    case APPLIANCE_CERTIFICATE_NAME_STORE_TABLE_TOO_LARGE:
        return "TABLE_TOO_LARGE";
    }
    return "UNKNOWN";
}
