#include "appliance_acme_control.h"

#include "sha256.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/*
 * ITEM-508 increment 34: the durable ACME control record.  The module is
 * transport-free and socket-free: it encodes, writes back and reads one small
 * setting, and it never touches a certificate, a key or the network.
 */

static void appliance_acme_control_zero(
    void *buffer,
    size_t length)
{
    memset(buffer, 0, length);
}

static void appliance_acme_control_put_u32(
    uint8_t *output,
    uint32_t value)
{
    output[0] = (uint8_t)(value & 0xFFU);
    output[1] = (uint8_t)((value >> 8U) & 0xFFU);
    output[2] = (uint8_t)((value >> 16U) & 0xFFU);
    output[3] = (uint8_t)((value >> 24U) & 0xFFU);
}

static uint32_t appliance_acme_control_get_u32(
    const uint8_t *input)
{
    return (uint32_t)input[0] |
           ((uint32_t)input[1] << 8U) |
           ((uint32_t)input[2] << 16U) |
           ((uint32_t)input[3] << 24U);
}

static bool appliance_acme_control_record_is_blank(
    const uint8_t *record,
    size_t length)
{
    size_t index;

    for (index = 0U; index < length; ++index) {
        if (record[index] != 0U) {
            return false;
        }
    }
    return true;
}

/*
 * The checksum covers the header without its own four bytes: magic, version,
 * flags and the request instant.  A record whose bytes are right but whose
 * header was not written through this encoder fails the checksum, so a
 * half-written record is never read as a setting.
 */
static uint32_t appliance_acme_control_checksum(
    const uint8_t *record)
{
    uint8_t digest[SHA256_DIGEST_SIZE];

    if (sha256_compute(record,
                       APPLIANCE_ACME_CONTROL_HEADER_BYTES -
                           APPLIANCE_ACME_CONTROL_CHECKSUM_BYTES,
                       digest) != SHA256_OK) {
        return 0U;
    }
    return appliance_acme_control_get_u32(digest);
}

static enum appliance_acme_control_status
appliance_acme_control_encode(
    const struct appliance_acme_control *control,
    uint8_t *record)
{
    uint32_t flags = 0U;

    if (control == NULL || record == NULL) {
        return APPLIANCE_ACME_CONTROL_NULL;
    }
    if (!control->initialized ||
        control->version != APPLIANCE_ACME_CONTROL_VERSION) {
        return APPLIANCE_ACME_CONTROL_BAD_STATE;
    }
    if (control->enabled) {
        flags |= APPLIANCE_ACME_CONTROL_FLAG_ENABLED;
    }
    if (control->order_requested) {
        flags |= APPLIANCE_ACME_CONTROL_FLAG_ORDER_REQUESTED;
    }
    appliance_acme_control_zero(record,
                               APPLIANCE_ACME_CONTROL_RECORD_BYTES);
    appliance_acme_control_put_u32(record, APPLIANCE_ACME_CONTROL_MAGIC);
    appliance_acme_control_put_u32(
        record + 4U, APPLIANCE_ACME_CONTROL_RECORD_VERSION);
    appliance_acme_control_put_u32(record + 8U, flags);
    appliance_acme_control_put_u32(
        record + 12U, control->order_requested_at);
    appliance_acme_control_put_u32(
        record + 16U, appliance_acme_control_checksum(record));
    return APPLIANCE_ACME_CONTROL_OK;
}

static enum appliance_acme_control_status
appliance_acme_control_decode(
    const uint8_t *record,
    struct appliance_acme_control *control_out)
{
    uint32_t flags;

    if (control_out == NULL) {
        return APPLIANCE_ACME_CONTROL_NULL;
    }
    appliance_acme_control_init(control_out);
    if (appliance_acme_control_get_u32(record) !=
        APPLIANCE_ACME_CONTROL_MAGIC) {
        return APPLIANCE_ACME_CONTROL_RECORD_CORRUPT;
    }
    if (appliance_acme_control_get_u32(record + 4U) !=
        APPLIANCE_ACME_CONTROL_RECORD_VERSION) {
        return APPLIANCE_ACME_CONTROL_RECORD_VERSION_UNKNOWN;
    }
    if (appliance_acme_control_get_u32(record + 16U) !=
        appliance_acme_control_checksum(record)) {
        return APPLIANCE_ACME_CONTROL_RECORD_CHECKSUM;
    }
    flags = appliance_acme_control_get_u32(record + 8U);
    if ((flags & ~(APPLIANCE_ACME_CONTROL_FLAG_ENABLED |
                   APPLIANCE_ACME_CONTROL_FLAG_ORDER_REQUESTED)) != 0U) {
        /* An unknown flag is a record this build does not understand. */
        return APPLIANCE_ACME_CONTROL_RECORD_CORRUPT;
    }
    control_out->enabled =
        (flags & APPLIANCE_ACME_CONTROL_FLAG_ENABLED) != 0U;
    control_out->order_requested =
        (flags & APPLIANCE_ACME_CONTROL_FLAG_ORDER_REQUESTED) != 0U;
    /*
     * A request instant without a request is not a setting this build wrote;
     * refuse it rather than publish an instant nothing asked for.
     */
    if (!control_out->order_requested &&
        appliance_acme_control_get_u32(record + 12U) != 0U) {
        return APPLIANCE_ACME_CONTROL_RECORD_CORRUPT;
    }
    control_out->order_requested_at =
        appliance_acme_control_get_u32(record + 12U);
    return APPLIANCE_ACME_CONTROL_OK;
}

void appliance_acme_control_init(
    struct appliance_acme_control *control)
{
    if (control == NULL) {
        return;
    }
    memset(control, 0, sizeof(*control));
    control->version = APPLIANCE_ACME_CONTROL_VERSION;
    control->initialized = true;
}

void appliance_acme_control_store_init(
    struct appliance_acme_control_store *store)
{
    if (store == NULL) {
        return;
    }
    memset(store, 0, sizeof(*store));
    store->version = APPLIANCE_ACME_CONTROL_VERSION;
    store->initialized = true;
    store->source = APPLIANCE_ACME_CONTROL_SOURCE_NONE;
    store->last_status = APPLIANCE_ACME_CONTROL_OK;
}

static bool appliance_acme_control_store_bind(
    struct appliance_acme_control_store *store,
    struct appliance_state_loader *loader)
{
    const struct appliance_state_layout_entry *entry = NULL;

    if (loader == NULL || !loader->ready) {
        store->last_status = APPLIANCE_ACME_CONTROL_STORE_UNAVAILABLE;
        return false;
    }
#if APPLIANCE_STATE_ENCRYPTION_ENABLED
    if (loader->encryption_sealed) {
        store->last_status = APPLIANCE_ACME_CONTROL_STORE_SEALED;
        return false;
    }
#endif
    if (appliance_state_layout_find(
            &loader->layout,
            APPLIANCE_PERSISTENT_STORAGE_NAMESPACE_ID_CERTIFICATE,
            APPLIANCE_STATE_STORE_ID_CERTIFICATE,
            &entry) != APPLIANCE_STATE_LAYOUT_OK ||
        entry == NULL) {
        store->last_status = APPLIANCE_ACME_CONTROL_STORE_UNAVAILABLE;
        return false;
    }
    if (entry->block_count <
        (uint64_t)APPLIANCE_ACME_CONTROL_BLOCK_OFFSET +
            APPLIANCE_ACME_CONTROL_RECORD_BLOCKS) {
        store->last_status = APPLIANCE_ACME_CONTROL_STORE_UNAVAILABLE;
        return false;
    }
    if (appliance_state_block_device_bind_range(
            &loader->partition.device,
            entry->block_offset + APPLIANCE_ACME_CONTROL_BLOCK_OFFSET,
            APPLIANCE_ACME_CONTROL_RECORD_BLOCKS,
            &store->store) != APPLIANCE_STATE_BLOCK_DEVICE_OK) {
        store->last_status = APPLIANCE_ACME_CONTROL_STORE_UNAVAILABLE;
        return false;
    }
    store->bound = true;
    return true;
}

enum appliance_acme_control_status appliance_acme_control_store_load(
    struct appliance_acme_control_store *store,
    struct appliance_state_loader *loader,
    struct appliance_acme_control *control_out)
{
    uint8_t record[APPLIANCE_ACME_CONTROL_RECORD_BYTES];
    enum appliance_acme_control_status status;

    if (control_out == NULL) {
        return APPLIANCE_ACME_CONTROL_NULL;
    }
    appliance_acme_control_init(control_out);
    if (store == NULL) {
        return APPLIANCE_ACME_CONTROL_NULL;
    }
    if (!store->initialized ||
        store->version != APPLIANCE_ACME_CONTROL_VERSION) {
        store->last_status = APPLIANCE_ACME_CONTROL_BAD_STATE;
        return store->last_status;
    }
    store->source = APPLIANCE_ACME_CONTROL_SOURCE_DEFAULT_UNAVAILABLE;
    if (!store->bound && !appliance_acme_control_store_bind(store, loader)) {
        return store->last_status;
    }
    appliance_acme_control_zero(record, sizeof(record));
    if (appliance_state_block_device_read(
            &store->store, 0U, record, sizeof(record)) != BLOCK_DEVICE_OK) {
        store->source = APPLIANCE_ACME_CONTROL_SOURCE_DEFAULT_INVALID;
        store->last_status = APPLIANCE_ACME_CONTROL_READ_FAILED;
        return store->last_status;
    }
    if (appliance_acme_control_record_is_blank(record, sizeof(record))) {
        /* Nothing was ever written here; ACME is off, and that is not a fault. */
        store->source = APPLIANCE_ACME_CONTROL_SOURCE_DEFAULT_MISSING;
        store->last_status = APPLIANCE_ACME_CONTROL_OK;
        ++store->load_count;
        return APPLIANCE_ACME_CONTROL_OK;
    }
    status = appliance_acme_control_decode(record, control_out);
    appliance_acme_control_zero(record, sizeof(record));
    store->last_status = status;
    if (status != APPLIANCE_ACME_CONTROL_OK) {
        appliance_acme_control_init(control_out);
        store->source = APPLIANCE_ACME_CONTROL_SOURCE_DEFAULT_INVALID;
        return status;
    }
    store->source = APPLIANCE_ACME_CONTROL_SOURCE_COMMITTED;
    ++store->load_count;
    return APPLIANCE_ACME_CONTROL_OK;
}

enum appliance_acme_control_status appliance_acme_control_store_save(
    struct appliance_acme_control_store *store,
    const struct appliance_acme_control *control)
{
    static uint8_t record[APPLIANCE_ACME_CONTROL_RECORD_BYTES];
    static uint8_t verify[APPLIANCE_ACME_CONTROL_RECORD_BYTES];
    enum appliance_acme_control_status status;

    if (store == NULL || control == NULL) {
        return APPLIANCE_ACME_CONTROL_NULL;
    }
    if (!store->initialized ||
        store->version != APPLIANCE_ACME_CONTROL_VERSION) {
        store->last_status = APPLIANCE_ACME_CONTROL_BAD_STATE;
        return store->last_status;
    }
    if (!store->bound) {
        store->last_status = APPLIANCE_ACME_CONTROL_STORE_UNAVAILABLE;
        return store->last_status;
    }
    status = appliance_acme_control_encode(control, record);
    if (status != APPLIANCE_ACME_CONTROL_OK) {
        store->last_status = status;
        return status;
    }
    if (appliance_state_block_device_write(
            &store->store, 0U, record, sizeof(record)) != BLOCK_DEVICE_OK ||
        appliance_state_block_device_flush(&store->store) != BLOCK_DEVICE_OK) {
        appliance_acme_control_zero(record, sizeof(record));
        store->last_status = APPLIANCE_ACME_CONTROL_WRITE_FAILED;
        return store->last_status;
    }
    /*
     * Read back before reporting success: the panel must not show a setting
     * the next boot would not carry.
     */
    appliance_acme_control_zero(verify, sizeof(verify));
    if (appliance_state_block_device_read(
            &store->store, 0U, verify, sizeof(verify)) != BLOCK_DEVICE_OK ||
        memcmp(verify, record, sizeof(record)) != 0) {
        appliance_acme_control_zero(record, sizeof(record));
        appliance_acme_control_zero(verify, sizeof(verify));
        store->last_status = APPLIANCE_ACME_CONTROL_WRITE_FAILED;
        return store->last_status;
    }
    appliance_acme_control_zero(record, sizeof(record));
    appliance_acme_control_zero(verify, sizeof(verify));
    store->last_status = APPLIANCE_ACME_CONTROL_OK;
    store->source = APPLIANCE_ACME_CONTROL_SOURCE_COMMITTED;
    ++store->save_count;
    return APPLIANCE_ACME_CONTROL_OK;
}

enum appliance_acme_control_status appliance_acme_control_set_enabled(
    struct appliance_acme_control *control,
    bool enabled)
{
    if (control == NULL) {
        return APPLIANCE_ACME_CONTROL_NULL;
    }
    if (!control->initialized ||
        control->version != APPLIANCE_ACME_CONTROL_VERSION) {
        return APPLIANCE_ACME_CONTROL_BAD_STATE;
    }
    control->enabled = enabled;
    if (!enabled) {
        /*
         * An operator who turned ACME off did not ask for an issuance to
         * happen later, so the pending request goes with it.
         */
        control->order_requested = false;
        control->order_requested_at = 0U;
    }
    return APPLIANCE_ACME_CONTROL_OK;
}

enum appliance_acme_control_status appliance_acme_control_request_order(
    struct appliance_acme_control *control,
    uint32_t now_epoch_seconds)
{
    if (control == NULL) {
        return APPLIANCE_ACME_CONTROL_NULL;
    }
    if (!control->initialized ||
        control->version != APPLIANCE_ACME_CONTROL_VERSION) {
        return APPLIANCE_ACME_CONTROL_BAD_STATE;
    }
    if (!control->enabled) {
        return APPLIANCE_ACME_CONTROL_NOT_ENABLED;
    }
    if (control->order_requested) {
        return APPLIANCE_ACME_CONTROL_ALREADY_REQUESTED;
    }
    control->order_requested = true;
    control->order_requested_at = now_epoch_seconds;
    return APPLIANCE_ACME_CONTROL_OK;
}

enum appliance_acme_control_status appliance_acme_control_take_request(
    struct appliance_acme_control *control)
{
    if (control == NULL) {
        return APPLIANCE_ACME_CONTROL_NULL;
    }
    if (!control->initialized ||
        control->version != APPLIANCE_ACME_CONTROL_VERSION) {
        return APPLIANCE_ACME_CONTROL_BAD_STATE;
    }
    control->order_requested = false;
    control->order_requested_at = 0U;
    return APPLIANCE_ACME_CONTROL_OK;
}

bool appliance_acme_control_enabled(
    const struct appliance_acme_control *control)
{
    return control != NULL && control->initialized && control->enabled;
}

bool appliance_acme_control_order_requested(
    const struct appliance_acme_control *control)
{
    return control != NULL && control->initialized &&
           control->order_requested;
}

uint32_t appliance_acme_control_order_requested_at(
    const struct appliance_acme_control *control)
{
    return control != NULL && control->initialized
               ? control->order_requested_at
               : 0U;
}

enum appliance_acme_control_source appliance_acme_control_store_source(
    const struct appliance_acme_control_store *store)
{
    return store != NULL ? store->source
                         : APPLIANCE_ACME_CONTROL_SOURCE_NONE;
}

const char *appliance_acme_control_status_string(
    enum appliance_acme_control_status status)
{
    switch (status) {
    case APPLIANCE_ACME_CONTROL_OK:
        return "OK";
    case APPLIANCE_ACME_CONTROL_NULL:
        return "NULL";
    case APPLIANCE_ACME_CONTROL_BAD_STATE:
        return "BAD_STATE";
    case APPLIANCE_ACME_CONTROL_BAD_VERSION:
        return "BAD_VERSION";
    case APPLIANCE_ACME_CONTROL_STORE_UNAVAILABLE:
        return "STORE_UNAVAILABLE";
    case APPLIANCE_ACME_CONTROL_STORE_SEALED:
        return "STORE_SEALED";
    case APPLIANCE_ACME_CONTROL_READ_FAILED:
        return "READ_FAILED";
    case APPLIANCE_ACME_CONTROL_WRITE_FAILED:
        return "WRITE_FAILED";
    case APPLIANCE_ACME_CONTROL_RECORD_CORRUPT:
        return "RECORD_CORRUPT";
    case APPLIANCE_ACME_CONTROL_RECORD_CHECKSUM:
        return "RECORD_CHECKSUM";
    case APPLIANCE_ACME_CONTROL_RECORD_VERSION_UNKNOWN:
        return "RECORD_VERSION_UNKNOWN";
    case APPLIANCE_ACME_CONTROL_NOT_ENABLED:
        return "NOT_ENABLED";
    case APPLIANCE_ACME_CONTROL_ALREADY_REQUESTED:
        return "ALREADY_REQUESTED";
    }
    return "UNKNOWN";
}

const char *appliance_acme_control_source_string(
    enum appliance_acme_control_source source)
{
    switch (source) {
    case APPLIANCE_ACME_CONTROL_SOURCE_NONE:
        return "NONE";
    case APPLIANCE_ACME_CONTROL_SOURCE_COMMITTED:
        return "COMMITTED";
    case APPLIANCE_ACME_CONTROL_SOURCE_DEFAULT_MISSING:
        return "DEFAULT_MISSING";
    case APPLIANCE_ACME_CONTROL_SOURCE_DEFAULT_INVALID:
        return "DEFAULT_INVALID";
    case APPLIANCE_ACME_CONTROL_SOURCE_DEFAULT_UNAVAILABLE:
        return "DEFAULT_UNAVAILABLE";
    }
    return "UNKNOWN";
}
