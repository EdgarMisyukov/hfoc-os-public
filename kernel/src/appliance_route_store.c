/*
 * ITEM-504 increment 6: the route table as durable state.
 *
 * The record format and the fail-closed rules are documented in
 * `appliance_route_store.h`.  This file owns the binding to the state volume,
 * the encoding, the decoding and the write-with-read-back.  It never decides
 * what a route means: the model's own `appliance_route_table_validate` is the
 * only judge, and it is applied to every table read from the disk.
 */
#include "appliance_route_store.h"

#include "data_region.h"
#include "sha256.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

static void appliance_route_store_zero(void *buffer, size_t length)
{
    if (buffer == NULL) {
        return;
    }
    memset(buffer, 0, length);
}

static void appliance_route_store_put_u16(uint8_t *output, uint16_t value)
{
    output[0] = (uint8_t)(value & UINT16_C(0x00FF));
    output[1] = (uint8_t)((value >> 8) & UINT16_C(0x00FF));
}

static void appliance_route_store_put_u32(uint8_t *output, uint32_t value)
{
    output[0] = (uint8_t)(value & UINT32_C(0x000000FF));
    output[1] = (uint8_t)((value >> 8) & UINT32_C(0x000000FF));
    output[2] = (uint8_t)((value >> 16) & UINT32_C(0x000000FF));
    output[3] = (uint8_t)((value >> 24) & UINT32_C(0x000000FF));
}

static uint16_t appliance_route_store_get_u16(const uint8_t *input)
{
    return (uint16_t)((uint16_t)input[0] |
                      (uint16_t)((uint16_t)input[1] << 8));
}

static uint32_t appliance_route_store_get_u32(const uint8_t *input)
{
    return (uint32_t)input[0] |
           ((uint32_t)input[1] << 8) |
           ((uint32_t)input[2] << 16) |
           ((uint32_t)input[3] << 24);
}

static bool appliance_route_store_record_is_blank(const uint8_t *record)
{
    size_t index;

    for (index = 0U; index < APPLIANCE_ROUTE_STORE_RECORD_BYTES; ++index) {
        if (record[index] != 0U) {
            return false;
        }
    }
    return true;
}

void appliance_route_store_init(struct appliance_route_store *store)
{
    if (store == NULL) {
        return;
    }
    memset(store, 0, sizeof(*store));
    store->version = APPLIANCE_ROUTE_STORE_VERSION;
    store->source = APPLIANCE_ROUTE_STORE_SOURCE_NONE;
    store->last_status = APPLIANCE_ROUTE_STORE_OK;
    store->initialized = true;
}

bool appliance_route_store_available(const struct appliance_route_store *store)
{
    return store != NULL && store->initialized && store->bound;
}

enum appliance_route_store_source appliance_route_store_source(
    const struct appliance_route_store *store)
{
    if (store == NULL) {
        return APPLIANCE_ROUTE_STORE_SOURCE_NONE;
    }
    return store->source;
}

enum appliance_route_store_status appliance_route_store_last_status(
    const struct appliance_route_store *store)
{
    if (store == NULL) {
        return APPLIANCE_ROUTE_STORE_NULL;
    }
    return store->last_status;
}

uint32_t appliance_route_store_save_count(
    const struct appliance_route_store *store)
{
    if (store == NULL) {
        return 0U;
    }
    return store->save_count;
}

/*
 * The store lives in the SYSTEM_CONFIG namespace next to the hostname and the
 * network configuration.  It is bound from the loader's layout exactly as the
 * certificate store is, and a sealed volume is refused for the same reason:
 * the plaintext record must never be written to a volume the appliance claims
 * to protect.
 */
static bool appliance_route_store_bind(
    struct appliance_route_store *store,
    struct appliance_state_loader *loader)
{
    const struct appliance_state_layout_entry *entry = NULL;

    if (loader == NULL || !loader->ready) {
        store->last_status = APPLIANCE_ROUTE_STORE_STORE_UNAVAILABLE;
        return false;
    }
#if APPLIANCE_STATE_ENCRYPTION_ENABLED
    if (loader->encryption_sealed) {
        store->last_status = APPLIANCE_ROUTE_STORE_STORE_SEALED;
        return false;
    }
#endif
    if (appliance_state_layout_find(
            &loader->layout,
            APPLIANCE_PERSISTENT_STORAGE_NAMESPACE_ID_SYSTEM_CONFIG,
            APPLIANCE_STATE_STORE_ID_SYSTEM_ROUTES,
            &entry) != APPLIANCE_STATE_LAYOUT_OK ||
        entry == NULL) {
        store->last_status = APPLIANCE_ROUTE_STORE_STORE_UNAVAILABLE;
        return false;
    }
    if (entry->block_count < APPLIANCE_ROUTE_STORE_RECORD_BLOCKS) {
        store->last_status = APPLIANCE_ROUTE_STORE_STORE_UNAVAILABLE;
        return false;
    }
    if (appliance_state_block_device_bind(
            &loader->partition.device, entry, &store->store) !=
        APPLIANCE_STATE_BLOCK_DEVICE_OK) {
        store->last_status = APPLIANCE_ROUTE_STORE_STORE_UNAVAILABLE;
        return false;
    }
    store->bound = true;
    return true;
}

static enum appliance_route_store_status appliance_route_store_encode(
    const struct appliance_route_table *table,
    uint8_t *record,
    size_t *length_out)
{
    uint8_t digest[SHA256_DIGEST_SIZE];
    size_t offset = APPLIANCE_ROUTE_STORE_HEADER_BYTES;
    uint32_t index;

    if (appliance_route_table_validate(table) != APPLIANCE_ROUTE_OK) {
        return APPLIANCE_ROUTE_STORE_TABLE_REFUSED;
    }
    appliance_route_store_zero(record, APPLIANCE_ROUTE_STORE_RECORD_BYTES);
    for (index = 0U; index < table->count; ++index) {
        const struct appliance_route *route = &table->routes[index];

        if (route->name_length == 0U ||
            route->name_length > APPLIANCE_ROUTE_NAME_MAX ||
            offset + APPLIANCE_ROUTE_STORE_ENTRY_FIXED_BYTES +
                    (size_t)route->name_length >
                APPLIANCE_ROUTE_STORE_RECORD_BYTES) {
            return APPLIANCE_ROUTE_STORE_TABLE_TOO_LARGE;
        }
        appliance_route_store_put_u16(
            record + offset, (uint16_t)route->name_length);
        memcpy(record + offset + 2U, route->backend,
               APPLIANCE_ROUTE_BACKEND_ADDRESS_LENGTH);
        appliance_route_store_put_u16(
            record + offset + 6U, route->backend_port);
        record[offset + 8U] = route->tls_to_backend ? 1U : 0U;
        record[offset + 9U] = (uint8_t)route->source;
        memcpy(record + offset + APPLIANCE_ROUTE_STORE_ENTRY_FIXED_BYTES,
               route->name, route->name_length);
        offset += APPLIANCE_ROUTE_STORE_ENTRY_FIXED_BYTES +
                  (size_t)route->name_length;
    }
    if (sha256_compute(
            record + APPLIANCE_ROUTE_STORE_HEADER_BYTES,
            offset - APPLIANCE_ROUTE_STORE_HEADER_BYTES, digest) !=
        SHA256_OK) {
        return APPLIANCE_ROUTE_STORE_RECORD_CORRUPT;
    }
    appliance_route_store_put_u32(record, APPLIANCE_ROUTE_STORE_MAGIC);
    appliance_route_store_put_u32(
        record + 4U, APPLIANCE_ROUTE_STORE_RECORD_VERSION);
    appliance_route_store_put_u32(record + 8U, table->count);
    appliance_route_store_put_u32(
        record + 12U, appliance_route_store_get_u32(digest));
    *length_out = offset;
    return APPLIANCE_ROUTE_STORE_OK;
}

static enum appliance_route_store_status appliance_route_store_decode(
    const uint8_t *record,
    struct appliance_route_table *table_out)
{
    uint8_t digest[SHA256_DIGEST_SIZE];
    size_t offset = APPLIANCE_ROUTE_STORE_HEADER_BYTES;
    uint32_t count;
    uint32_t index;

    if (appliance_route_store_get_u32(record) !=
            APPLIANCE_ROUTE_STORE_MAGIC ||
        appliance_route_store_get_u32(record + 4U) !=
            APPLIANCE_ROUTE_STORE_RECORD_VERSION) {
        return APPLIANCE_ROUTE_STORE_RECORD_CORRUPT;
    }
    count = appliance_route_store_get_u32(record + 8U);
    if (count > APPLIANCE_ROUTE_TABLE_MAX) {
        return APPLIANCE_ROUTE_STORE_RECORD_CORRUPT;
    }
    /*
     * The body is walked by its own lengths first, so the checksum covers
     * exactly the bytes the count and the lengths claim, and a truncated
     * record is refused instead of hashing into the neighbouring blocks.
     */
    for (index = 0U; index < count; ++index) {
        uint16_t name_length;

        if (offset + APPLIANCE_ROUTE_STORE_ENTRY_FIXED_BYTES >
            APPLIANCE_ROUTE_STORE_RECORD_BYTES) {
            return APPLIANCE_ROUTE_STORE_RECORD_CORRUPT;
        }
        name_length = appliance_route_store_get_u16(record + offset);
        if (name_length == 0U ||
            name_length > APPLIANCE_ROUTE_NAME_MAX ||
            offset + APPLIANCE_ROUTE_STORE_ENTRY_FIXED_BYTES +
                    (size_t)name_length >
                APPLIANCE_ROUTE_STORE_RECORD_BYTES) {
            return APPLIANCE_ROUTE_STORE_RECORD_CORRUPT;
        }
        offset += APPLIANCE_ROUTE_STORE_ENTRY_FIXED_BYTES +
                  (size_t)name_length;
    }
    if (sha256_compute(
            record + APPLIANCE_ROUTE_STORE_HEADER_BYTES,
            offset - APPLIANCE_ROUTE_STORE_HEADER_BYTES, digest) !=
        SHA256_OK) {
        return APPLIANCE_ROUTE_STORE_RECORD_CORRUPT;
    }
    if (appliance_route_store_get_u32(digest) !=
        appliance_route_store_get_u32(record + 12U)) {
        return APPLIANCE_ROUTE_STORE_RECORD_CHECKSUM;
    }
    appliance_route_table_init(table_out);
    offset = APPLIANCE_ROUTE_STORE_HEADER_BYTES;
    for (index = 0U; index < count; ++index) {
        struct appliance_route *route = &table_out->routes[index];
        uint16_t name_length =
            appliance_route_store_get_u16(record + offset);

        memcpy(route->backend, record + offset + 2U,
               APPLIANCE_ROUTE_BACKEND_ADDRESS_LENGTH);
        route->backend_port =
            appliance_route_store_get_u16(record + offset + 6U);
        route->tls_to_backend = record[offset + 8U] != 0U;
        route->source = record[offset + 9U];
        memcpy(route->name,
               record + offset + APPLIANCE_ROUTE_STORE_ENTRY_FIXED_BYTES,
               name_length);
        route->name[name_length] = '\0';
        route->name_length = name_length;
        offset += APPLIANCE_ROUTE_STORE_ENTRY_FIXED_BYTES +
                  (size_t)name_length;
    }
    table_out->count = count;
    if (appliance_route_table_validate(table_out) != APPLIANCE_ROUTE_OK) {
        appliance_route_table_init(table_out);
        return APPLIANCE_ROUTE_STORE_TABLE_REFUSED;
    }
    return APPLIANCE_ROUTE_STORE_OK;
}

enum appliance_route_store_status appliance_route_store_load(
    struct appliance_route_store *store,
    struct appliance_state_loader *loader,
    struct appliance_route_table *table_out)
{
    static uint8_t record[APPLIANCE_ROUTE_STORE_RECORD_BYTES];
    enum appliance_route_store_status status;

    if (table_out == NULL) {
        return APPLIANCE_ROUTE_STORE_NULL;
    }
    appliance_route_table_init(table_out);
    if (store == NULL) {
        return APPLIANCE_ROUTE_STORE_NULL;
    }
    if (!store->initialized ||
        store->version != APPLIANCE_ROUTE_STORE_VERSION) {
        store->last_status = APPLIANCE_ROUTE_STORE_BAD_STATE;
        return store->last_status;
    }
    store->source = APPLIANCE_ROUTE_STORE_SOURCE_DEFAULT_UNAVAILABLE;
    if (!store->bound && !appliance_route_store_bind(store, loader)) {
        return store->last_status;
    }
    appliance_route_store_zero(record, sizeof(record));
    if (appliance_state_block_device_read(
            &store->store, 0U, record, sizeof(record)) != BLOCK_DEVICE_OK) {
        store->source = APPLIANCE_ROUTE_STORE_SOURCE_DEFAULT_INVALID;
        store->last_status = APPLIANCE_ROUTE_STORE_READ_FAILED;
        return store->last_status;
    }
    if (appliance_route_store_record_is_blank(record)) {
        /* Nothing was ever written here; that is an empty table, not a fault. */
        store->source = APPLIANCE_ROUTE_STORE_SOURCE_DEFAULT_MISSING;
        store->last_status = APPLIANCE_ROUTE_STORE_OK;
        ++store->load_count;
        return APPLIANCE_ROUTE_STORE_OK;
    }
    /*
     * Every store the installer or the ITEM-502 erase rebuilt carries the
     * empty data-region header, and a store that never held a route table
     * still carries it: a committed record overwrites block 0 with the route
     * magic, so a valid region header means "no routes yet" rather than a
     * corrupt record.  A record that carries the route magic and fails its
     * checksum stays corruption.
     */
    {
        struct data_region region;

        if (data_region_discover(&store->store.device, &region) ==
            DATA_REGION_OK) {
            store->source = APPLIANCE_ROUTE_STORE_SOURCE_DEFAULT_MISSING;
            store->last_status = APPLIANCE_ROUTE_STORE_OK;
            ++store->load_count;
            return APPLIANCE_ROUTE_STORE_OK;
        }
    }
    status = appliance_route_store_decode(record, table_out);
    store->last_status = status;
    if (status != APPLIANCE_ROUTE_STORE_OK) {
        store->source = APPLIANCE_ROUTE_STORE_SOURCE_DEFAULT_INVALID;
        return status;
    }
    store->source = APPLIANCE_ROUTE_STORE_SOURCE_COMMITTED;
    ++store->load_count;
    return APPLIANCE_ROUTE_STORE_OK;
}

enum appliance_route_store_status appliance_route_store_save(
    struct appliance_route_store *store,
    const struct appliance_route_table *table)
{
    static uint8_t record[APPLIANCE_ROUTE_STORE_RECORD_BYTES];
    static uint8_t verify[APPLIANCE_ROUTE_STORE_RECORD_BYTES];
    enum appliance_route_store_status status;
    size_t length = 0U;

    if (store == NULL || table == NULL) {
        return APPLIANCE_ROUTE_STORE_NULL;
    }
    if (!store->initialized ||
        store->version != APPLIANCE_ROUTE_STORE_VERSION) {
        store->last_status = APPLIANCE_ROUTE_STORE_BAD_STATE;
        return store->last_status;
    }
    if (!store->bound) {
        store->last_status = APPLIANCE_ROUTE_STORE_STORE_UNAVAILABLE;
        return store->last_status;
    }
    status = appliance_route_store_encode(table, record, &length);
    if (status != APPLIANCE_ROUTE_STORE_OK ||
        length > APPLIANCE_ROUTE_STORE_RECORD_BYTES) {
        store->last_status = status != APPLIANCE_ROUTE_STORE_OK
            ? status
            : APPLIANCE_ROUTE_STORE_TABLE_TOO_LARGE;
        return store->last_status;
    }
    if (appliance_state_block_device_write(
            &store->store, 0U, record, sizeof(record)) != BLOCK_DEVICE_OK ||
        appliance_state_block_device_flush(&store->store) != BLOCK_DEVICE_OK) {
        store->last_status = APPLIANCE_ROUTE_STORE_WRITE_FAILED;
        return store->last_status;
    }
    /*
     * Read back before reporting success: the panel must not show a route the
     * next boot would not carry.
     */
    appliance_route_store_zero(verify, sizeof(verify));
    if (appliance_state_block_device_read(
            &store->store, 0U, verify, sizeof(verify)) != BLOCK_DEVICE_OK ||
        memcmp(verify, record, sizeof(record)) != 0) {
        store->last_status = APPLIANCE_ROUTE_STORE_WRITE_FAILED;
        return store->last_status;
    }
    store->last_status = APPLIANCE_ROUTE_STORE_OK;
    store->source = APPLIANCE_ROUTE_STORE_SOURCE_COMMITTED;
    ++store->save_count;
    return APPLIANCE_ROUTE_STORE_OK;
}

const char *appliance_route_store_source_string(
    enum appliance_route_store_source source)
{
    switch (source) {
    case APPLIANCE_ROUTE_STORE_SOURCE_NONE:
        return "NONE";
    case APPLIANCE_ROUTE_STORE_SOURCE_COMMITTED:
        return "COMMITTED";
    case APPLIANCE_ROUTE_STORE_SOURCE_DEFAULT_MISSING:
        return "DEFAULT_MISSING";
    case APPLIANCE_ROUTE_STORE_SOURCE_DEFAULT_INVALID:
        return "DEFAULT_INVALID";
    case APPLIANCE_ROUTE_STORE_SOURCE_DEFAULT_UNAVAILABLE:
        return "DEFAULT_UNAVAILABLE";
    }
    return "UNKNOWN";
}

const char *appliance_route_store_status_string(
    enum appliance_route_store_status status)
{
    switch (status) {
    case APPLIANCE_ROUTE_STORE_OK:
        return "OK";
    case APPLIANCE_ROUTE_STORE_NULL:
        return "NULL";
    case APPLIANCE_ROUTE_STORE_BAD_STATE:
        return "BAD_STATE";
    case APPLIANCE_ROUTE_STORE_STORE_UNAVAILABLE:
        return "STORE_UNAVAILABLE";
    case APPLIANCE_ROUTE_STORE_STORE_SEALED:
        return "STORE_SEALED";
    case APPLIANCE_ROUTE_STORE_READ_FAILED:
        return "READ_FAILED";
    case APPLIANCE_ROUTE_STORE_WRITE_FAILED:
        return "WRITE_FAILED";
    case APPLIANCE_ROUTE_STORE_RECORD_CORRUPT:
        return "RECORD_CORRUPT";
    case APPLIANCE_ROUTE_STORE_RECORD_CHECKSUM:
        return "RECORD_CHECKSUM";
    case APPLIANCE_ROUTE_STORE_TABLE_REFUSED:
        return "TABLE_REFUSED";
    case APPLIANCE_ROUTE_STORE_TABLE_TOO_LARGE:
        return "TABLE_TOO_LARGE";
    }
    return "UNKNOWN";
}
