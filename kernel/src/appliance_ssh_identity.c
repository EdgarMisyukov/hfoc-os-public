#ifdef HFOC_PRODUCTION_TLS

#include "appliance_ssh_identity.h"

#include "hfoc_ssh_wire.h"

#include "appliance_ssh_authorized_key.h"

#include "mbedtls/sha256.h"

#include <string.h>

#define APPLIANCE_SSH_RECORD_VERSION 1U
#define APPLIANCE_SSH_RECORD_MAGIC_BYTES 8U
#define APPLIANCE_SSH_HOST_SCALAR_OFFSET 16U
#define APPLIANCE_SSH_HOST_LENGTH_OFFSET 48U
#define APPLIANCE_SSH_HOST_BLOB_OFFSET 52U
#define APPLIANCE_SSH_HOST_FINGERPRINT_OFFSET 212U
#define APPLIANCE_SSH_AUTH_LENGTH_OFFSET 16U
#define APPLIANCE_SSH_AUTH_BLOB_OFFSET 20U
#define APPLIANCE_SSH_AUTH_FINGERPRINT_OFFSET 212U
#define APPLIANCE_SSH_CHECKSUM_OFFSET 244U
#define APPLIANCE_SSH_CHECKSUM_BYTES 32U

static void appliance_ssh_identity_zero(void *data, size_t length)
{
    unsigned char *bytes = data;
    size_t index;

    for (index = 0U; index < length; ++index) {
        bytes[index] = 0U;
    }
}

static void appliance_ssh_identity_store_u32(uint8_t *bytes, uint32_t value)
{
    bytes[0] = (uint8_t)(value >> 24U);
    bytes[1] = (uint8_t)(value >> 16U);
    bytes[2] = (uint8_t)(value >> 8U);
    bytes[3] = (uint8_t)value;
}

static uint32_t appliance_ssh_identity_load_u32(const uint8_t *bytes)
{
    return ((uint32_t)bytes[0] << 24U) |
           ((uint32_t)bytes[1] << 16U) |
           ((uint32_t)bytes[2] << 8U) |
           (uint32_t)bytes[3];
}

static bool appliance_ssh_identity_all_zero(const uint8_t *bytes,
                                            size_t length)
{
    size_t index;

    for (index = 0U; index < length; ++index) {
        if (bytes[index] != 0U) {
            return false;
        }
    }
    return true;
}

static bool appliance_ssh_identity_checksum_valid(const uint8_t *record)
{
    uint8_t digest[32];

    if (mbedtls_sha256(record, APPLIANCE_SSH_CHECKSUM_OFFSET, digest, 0) != 0) {
        return false;
    }
    return memcmp(digest, record + APPLIANCE_SSH_CHECKSUM_OFFSET,
                  APPLIANCE_SSH_CHECKSUM_BYTES) == 0;
}

static bool appliance_ssh_identity_checksum_write(uint8_t *record)
{
    uint8_t digest[32];

    if (mbedtls_sha256(record, APPLIANCE_SSH_CHECKSUM_OFFSET, digest, 0) != 0) {
        return false;
    }
    memcpy(record + APPLIANCE_SSH_CHECKSUM_OFFSET, digest, sizeof(digest));
    return true;
}

/*
 * The magic is an eight-byte ASCII tag, not two endian words: write it
 * directly and compare it directly so a record never depends on the host's
 * byte order.
 */
static void appliance_ssh_identity_magic_write(uint8_t *record,
                                               const char *magic)
{
    memcpy(record, magic, APPLIANCE_SSH_RECORD_MAGIC_BYTES);
}

static bool appliance_ssh_identity_magic_is_text(const uint8_t *record,
                                                 const char *magic)
{
    return memcmp(record, magic, APPLIANCE_SSH_RECORD_MAGIC_BYTES) == 0;
}

static enum appliance_ssh_identity_status
appliance_ssh_identity_bind(struct appliance_ssh_identity *identity,
                            struct appliance_state_loader *loader)
{
    const struct appliance_state_layout_entry *entry = NULL;

    if (loader == NULL || !loader->ready) {
        return APPLIANCE_SSH_IDENTITY_STORE_UNAVAILABLE;
    }
#if APPLIANCE_STATE_ENCRYPTION_ENABLED
    if (loader->encryption_sealed) {
        return APPLIANCE_SSH_IDENTITY_STORE_SEALED;
    }
#endif
    if (appliance_state_layout_find(
            &loader->layout,
            APPLIANCE_PERSISTENT_STORAGE_NAMESPACE_ID_CERTIFICATE,
            APPLIANCE_STATE_STORE_ID_CERTIFICATE,
            &entry) != APPLIANCE_STATE_LAYOUT_OK ||
        entry == NULL) {
        return APPLIANCE_SSH_IDENTITY_STORE_UNAVAILABLE;
    }
    if (entry->block_count <
        APPLIANCE_SSH_CERT_TAIL_BLOCK_OFFSET +
            APPLIANCE_SSH_HOST_KEY_RECORD_BLOCKS +
            APPLIANCE_SSH_AUTHORIZED_KEY_RECORD_BLOCKS) {
        return APPLIANCE_SSH_IDENTITY_STORE_TOO_SMALL;
    }
    if (appliance_state_block_device_bind_range(
            &loader->partition.device,
            entry->block_offset + APPLIANCE_SSH_CERT_TAIL_BLOCK_OFFSET,
            APPLIANCE_SSH_HOST_KEY_RECORD_BLOCKS,
            &identity->host_store) != APPLIANCE_STATE_BLOCK_DEVICE_OK) {
        return APPLIANCE_SSH_IDENTITY_STORE_UNAVAILABLE;
    }
    if (appliance_state_block_device_bind_range(
            &loader->partition.device,
            entry->block_offset + APPLIANCE_SSH_CERT_TAIL_BLOCK_OFFSET +
                APPLIANCE_SSH_HOST_KEY_RECORD_BLOCKS,
            APPLIANCE_SSH_AUTHORIZED_KEY_RECORD_BLOCKS,
            &identity->authorized_store) != APPLIANCE_STATE_BLOCK_DEVICE_OK) {
        return APPLIANCE_SSH_IDENTITY_STORE_UNAVAILABLE;
    }
    identity->store_bound = true;
    return APPLIANCE_SSH_IDENTITY_OK;
}

static enum appliance_ssh_identity_status
appliance_ssh_identity_write_record(
    struct appliance_state_block_device *store,
    const uint8_t *record)
{
    uint8_t readback[APPLIANCE_SSH_RECORD_BYTES];

    if (appliance_state_block_device_write(store, 0U, record,
                                           APPLIANCE_SSH_RECORD_BYTES) !=
            BLOCK_DEVICE_OK ||
        appliance_state_block_device_flush(store) != BLOCK_DEVICE_OK) {
        return APPLIANCE_SSH_IDENTITY_STORE_WRITE_FAILED;
    }
    appliance_ssh_identity_zero(readback, sizeof(readback));
    if (appliance_state_block_device_read(store, 0U, readback,
                                          sizeof(readback)) != BLOCK_DEVICE_OK ||
        memcmp(readback, record, APPLIANCE_SSH_CHECKSUM_OFFSET +
                                     APPLIANCE_SSH_CHECKSUM_BYTES) != 0) {
        return APPLIANCE_SSH_IDENTITY_STORE_READ_FAILED;
    }
    return APPLIANCE_SSH_IDENTITY_OK;
}

static enum appliance_ssh_identity_status
appliance_ssh_identity_host_load(struct appliance_ssh_identity *identity,
                                 hfoc_ssh_rng_fn rng, void *rng_context)
{
    uint8_t record[APPLIANCE_SSH_RECORD_BYTES];
    uint8_t blob[HFOC_SSH_CRYPTO_HOST_BLOB_MAX];
    uint8_t fingerprint[32];
    size_t blob_length = 0U;
    uint32_t scalar_length;
    enum appliance_ssh_identity_status status;

    appliance_ssh_identity_zero(record, sizeof(record));
    if (appliance_state_block_device_read(
            &identity->host_store, 0U, record, sizeof(record)) !=
        BLOCK_DEVICE_OK) {
        return APPLIANCE_SSH_IDENTITY_STORE_READ_FAILED;
    }
    if (appliance_ssh_identity_all_zero(record, sizeof(record))) {
        /* A genuinely blank record: generate once and persist it. */
        if (!hfoc_ssh_crypto_host_key_create(
                identity->host_scalar, identity->host_blob,
                &identity->host_blob_length, identity->host_fingerprint,
                rng, rng_context)) {
            return APPLIANCE_SSH_IDENTITY_KEY_GENERATE_FAILED;
        }
        appliance_ssh_identity_zero(record, sizeof(record));
        appliance_ssh_identity_magic_write(record, "HFSSHKEY");
        appliance_ssh_identity_store_u32(record + 8U,
                                         APPLIANCE_SSH_RECORD_VERSION);
        appliance_ssh_identity_store_u32(record + 12U, 1U);
        memcpy(record + APPLIANCE_SSH_HOST_SCALAR_OFFSET,
               identity->host_scalar, 32U);
        appliance_ssh_identity_store_u32(
            record + APPLIANCE_SSH_HOST_LENGTH_OFFSET,
            (uint32_t)identity->host_blob_length);
        memcpy(record + APPLIANCE_SSH_HOST_BLOB_OFFSET,
               identity->host_blob, identity->host_blob_length);
        memcpy(record + APPLIANCE_SSH_HOST_FINGERPRINT_OFFSET,
               identity->host_fingerprint, 32U);
        if (!appliance_ssh_identity_checksum_write(record)) {
            return APPLIANCE_SSH_IDENTITY_KEY_GENERATE_FAILED;
        }
        status = appliance_ssh_identity_write_record(
            &identity->host_store, record);
        if (status != APPLIANCE_SSH_IDENTITY_OK) {
            return status;
        }
        identity->host_ready = true;
        return APPLIANCE_SSH_IDENTITY_OK;
    }
    if (!appliance_ssh_identity_magic_is_text(record, "HFSSHKEY") ||
        appliance_ssh_identity_load_u32(record + 8U) !=
            APPLIANCE_SSH_RECORD_VERSION ||
        !appliance_ssh_identity_checksum_valid(record)) {
        return APPLIANCE_SSH_IDENTITY_RECORD_CORRUPT;
    }
    scalar_length = appliance_ssh_identity_load_u32(
        record + APPLIANCE_SSH_HOST_LENGTH_OFFSET);
    if (scalar_length == 0U ||
        scalar_length > HFOC_SSH_CRYPTO_HOST_BLOB_MAX) {
        return APPLIANCE_SSH_IDENTITY_RECORD_CORRUPT;
    }
    memcpy(identity->host_scalar,
           record + APPLIANCE_SSH_HOST_SCALAR_OFFSET, 32U);
    if (!hfoc_ssh_crypto_host_key_derive(
            identity->host_scalar, blob, &blob_length, fingerprint, rng,
            rng_context) ||
        blob_length != scalar_length ||
        memcmp(blob, record + APPLIANCE_SSH_HOST_BLOB_OFFSET,
               blob_length) != 0 ||
        memcmp(fingerprint,
               record + APPLIANCE_SSH_HOST_FINGERPRINT_OFFSET, 32U) != 0) {
        return APPLIANCE_SSH_IDENTITY_KEY_INVALID;
    }
    memcpy(identity->host_blob, blob, blob_length);
    identity->host_blob_length = blob_length;
    memcpy(identity->host_fingerprint, fingerprint, 32U);
    identity->host_ready = true;
    return APPLIANCE_SSH_IDENTITY_OK;
}

static enum appliance_ssh_identity_status
appliance_ssh_identity_authorized_load(
    struct appliance_ssh_identity *identity)
{
    uint8_t record[APPLIANCE_SSH_RECORD_BYTES];
    uint8_t fingerprint[32];
    uint32_t blob_length;

    appliance_ssh_identity_zero(record, sizeof(record));
    if (appliance_state_block_device_read(
            &identity->authorized_store, 0U, record, sizeof(record)) !=
        BLOCK_DEVICE_OK) {
        return APPLIANCE_SSH_IDENTITY_STORE_READ_FAILED;
    }
    if (appliance_ssh_identity_all_zero(record, sizeof(record))) {
        identity->authorized_length = 0U;
        identity->authorized_ready = false;
        return APPLIANCE_SSH_IDENTITY_AUTHORIZED_MISSING;
    }
    if (!appliance_ssh_identity_magic_is_text(record, "HFSSHAUT") ||
        appliance_ssh_identity_load_u32(record + 8U) !=
            APPLIANCE_SSH_RECORD_VERSION ||
        !appliance_ssh_identity_checksum_valid(record)) {
        return APPLIANCE_SSH_IDENTITY_RECORD_CORRUPT;
    }
    blob_length = appliance_ssh_identity_load_u32(
        record + APPLIANCE_SSH_AUTH_LENGTH_OFFSET);
    if (blob_length == 0U || blob_length > sizeof(identity->authorized_blob) ||
        !hfoc_ssh_crypto_public_blob_validate(
            record + APPLIANCE_SSH_AUTH_BLOB_OFFSET, blob_length) ||
        !hfoc_ssh_crypto_public_blob_fingerprint(
            record + APPLIANCE_SSH_AUTH_BLOB_OFFSET, blob_length,
            fingerprint) ||
        memcmp(fingerprint,
               record + APPLIANCE_SSH_AUTH_FINGERPRINT_OFFSET, 32U) != 0) {
        return APPLIANCE_SSH_IDENTITY_AUTHORIZED_INVALID;
    }
    memcpy(identity->authorized_blob,
           record + APPLIANCE_SSH_AUTH_BLOB_OFFSET, blob_length);
    identity->authorized_length = blob_length;
    identity->authorized_ready = true;
    return APPLIANCE_SSH_IDENTITY_OK;
}

void appliance_ssh_identity_init(struct appliance_ssh_identity *identity)
{
    if (identity == NULL) {
        return;
    }
    appliance_ssh_identity_zero(identity, sizeof(*identity));
    identity->initialized = true;
    identity->version = APPLIANCE_SSH_IDENTITY_VERSION;
    identity->last_status = APPLIANCE_SSH_IDENTITY_OK;
}

enum appliance_ssh_identity_status appliance_ssh_identity_prepare(
    struct appliance_ssh_identity *identity,
    struct appliance_state_loader *loader,
    hfoc_ssh_rng_fn rng,
    void *rng_context)
{
    enum appliance_ssh_identity_status status;

    if (identity == NULL || !identity->initialized ||
        identity->version != APPLIANCE_SSH_IDENTITY_VERSION) {
        return APPLIANCE_SSH_IDENTITY_NULL;
    }
    if (!identity->store_bound) {
        status = appliance_ssh_identity_bind(identity, loader);
        if (status != APPLIANCE_SSH_IDENTITY_OK) {
            identity->last_status = status;
            return status;
        }
    }
    if (!identity->host_ready) {
        status = appliance_ssh_identity_host_load(identity, rng, rng_context);
        if (status != APPLIANCE_SSH_IDENTITY_OK) {
            identity->last_status = status;
            return status;
        }
    }
    /*
     * A missing or corrupt authorized-key record leaves the host identity
     * usable but authorizes nobody.  The caller must refuse authentication
     * until a key is enrolled; it must never accept the first key it sees.
     */
    status = appliance_ssh_identity_authorized_load(identity);
    if (status != APPLIANCE_SSH_IDENTITY_OK) {
        identity->last_status = status;
        identity->authorized_ready = false;
        identity->authorized_length = 0U;
        return APPLIANCE_SSH_IDENTITY_OK;
    }
    identity->last_status = APPLIANCE_SSH_IDENTITY_OK;
    return APPLIANCE_SSH_IDENTITY_OK;
}

enum appliance_ssh_identity_status appliance_ssh_identity_authorized_key_add(
    struct appliance_ssh_identity *identity,
    const uint8_t *blob,
    size_t blob_length)
{
    uint8_t record[APPLIANCE_SSH_RECORD_BYTES];
    uint8_t fingerprint[32];
    enum appliance_ssh_identity_status status;

    if (identity == NULL || blob == NULL ||
        !identity->initialized ||
        identity->version != APPLIANCE_SSH_IDENTITY_VERSION) {
        return APPLIANCE_SSH_IDENTITY_NULL;
    }
    if (!identity->store_bound) {
        return APPLIANCE_SSH_IDENTITY_BAD_STATE;
    }
    if (blob_length == 0U || blob_length > sizeof(identity->authorized_blob) ||
        !hfoc_ssh_crypto_public_blob_validate(blob, blob_length) ||
        !hfoc_ssh_crypto_public_blob_fingerprint(blob, blob_length,
                                                 fingerprint)) {
        identity->last_status =
            APPLIANCE_SSH_IDENTITY_AUTHORIZED_INVALID;
        return identity->last_status;
    }
    appliance_ssh_identity_zero(record, sizeof(record));
    appliance_ssh_identity_magic_write(record, "HFSSHAUT");
    appliance_ssh_identity_store_u32(record + 8U,
                                     APPLIANCE_SSH_RECORD_VERSION);
    appliance_ssh_identity_store_u32(record + 12U, 1U);
    appliance_ssh_identity_store_u32(
        record + APPLIANCE_SSH_AUTH_LENGTH_OFFSET, (uint32_t)blob_length);
    memcpy(record + APPLIANCE_SSH_AUTH_BLOB_OFFSET, blob, blob_length);
    memcpy(record + APPLIANCE_SSH_AUTH_FINGERPRINT_OFFSET, fingerprint,
           32U);
    if (!appliance_ssh_identity_checksum_write(record)) {
        return APPLIANCE_SSH_IDENTITY_AUTHORIZED_INVALID;
    }
    status = appliance_ssh_identity_write_record(
        &identity->authorized_store, record);
    if (status != APPLIANCE_SSH_IDENTITY_OK) {
        identity->last_status = status;
        return status;
    }
    memcpy(identity->authorized_blob, blob, blob_length);
    identity->authorized_length = blob_length;
    identity->authorized_ready = true;
    identity->last_status = APPLIANCE_SSH_IDENTITY_OK;
    return APPLIANCE_SSH_IDENTITY_OK;
}

enum appliance_ssh_identity_status
appliance_ssh_identity_authorized_key_remove(
    struct appliance_ssh_identity *identity)
{
    uint8_t record[APPLIANCE_SSH_RECORD_BYTES];
    enum appliance_ssh_identity_status status;

    if (identity == NULL || !identity->initialized ||
        identity->version != APPLIANCE_SSH_IDENTITY_VERSION) {
        return APPLIANCE_SSH_IDENTITY_NULL;
    }
    if (!identity->store_bound) {
        return APPLIANCE_SSH_IDENTITY_BAD_STATE;
    }
    /*
     * ITEM-603: revocation is fail-closed in memory even when the durable
     * clear cannot be written.  The old key must not stay authorized for the
     * rest of this boot because a flush failed; the caller keeps the listener
     * stopped and reports the failed durable clear.
     */
    appliance_ssh_identity_zero(identity->authorized_blob,
                                sizeof(identity->authorized_blob));
    identity->authorized_length = 0U;
    identity->authorized_ready = false;
    appliance_ssh_identity_zero(record, sizeof(record));
    status = appliance_ssh_identity_write_record(
        &identity->authorized_store, record);
    identity->last_status = status;
    return status;
}

/*
 * ITEM-606: the installer/medium import path.  The record writer already
 * flushes and reads its own bytes back; this wrapper adds the installed
 * system's record parse on top, so a medium cannot claim a key that the next
 * boot would reject.  The host key is not generated here: only the authorized
 * four-block tail is bound and written.
 */
enum appliance_ssh_identity_status appliance_ssh_identity_authorized_key_store(
    struct appliance_state_loader *loader,
    const uint8_t *blob, size_t blob_length)
{
    /*
     * ITEM-606: the record writer lives in the mbedTLS-free module so the
     * installer kernel can use it.  This wrapper keeps the identity-facing
     * API for the console/host proofs and maps the named result.
     */
    switch (appliance_ssh_authorized_key_store(loader, blob, blob_length)) {
    case APPLIANCE_SSH_AUTHORIZED_KEY_OK:
        return APPLIANCE_SSH_IDENTITY_OK;
    case APPLIANCE_SSH_AUTHORIZED_KEY_NULL:
        return APPLIANCE_SSH_IDENTITY_NULL;
    case APPLIANCE_SSH_AUTHORIZED_KEY_BAD_POINT:
        return APPLIANCE_SSH_IDENTITY_AUTHORIZED_INVALID;
    case APPLIANCE_SSH_AUTHORIZED_KEY_STORE_UNAVAILABLE:
        return APPLIANCE_SSH_IDENTITY_STORE_UNAVAILABLE;
    case APPLIANCE_SSH_AUTHORIZED_KEY_STORE_SEALED:
        return APPLIANCE_SSH_IDENTITY_STORE_SEALED;
    case APPLIANCE_SSH_AUTHORIZED_KEY_STORE_TOO_SMALL:
        return APPLIANCE_SSH_IDENTITY_STORE_TOO_SMALL;
    case APPLIANCE_SSH_AUTHORIZED_KEY_STORE_READ_FAILED:
        return APPLIANCE_SSH_IDENTITY_STORE_READ_FAILED;
    case APPLIANCE_SSH_AUTHORIZED_KEY_STORE_WRITE_FAILED:
        return APPLIANCE_SSH_IDENTITY_STORE_WRITE_FAILED;
    case APPLIANCE_SSH_AUTHORIZED_KEY_RECORD_CORRUPT:
    default:
        return APPLIANCE_SSH_IDENTITY_RECORD_CORRUPT;
    }
}

bool appliance_ssh_identity_host_ready(
    const struct appliance_ssh_identity *identity)
{
    return identity != NULL && identity->host_ready;
}

bool appliance_ssh_identity_authorized_ready(
    const struct appliance_ssh_identity *identity)
{
    return identity != NULL && identity->authorized_ready;
}

const uint8_t *appliance_ssh_identity_host_scalar(
    const struct appliance_ssh_identity *identity)
{
    return identity != NULL && identity->host_ready
        ? identity->host_scalar
        : NULL;
}

const uint8_t *appliance_ssh_identity_host_blob(
    const struct appliance_ssh_identity *identity,
    size_t *length)
{
    if (identity == NULL || !identity->host_ready || length == NULL) {
        return NULL;
    }
    *length = identity->host_blob_length;
    return identity->host_blob;
}

const uint8_t *appliance_ssh_identity_host_fingerprint(
    const struct appliance_ssh_identity *identity)
{
    return identity != NULL && identity->host_ready
        ? identity->host_fingerprint
        : NULL;
}

const uint8_t *appliance_ssh_identity_authorized_blob(
    const struct appliance_ssh_identity *identity,
    size_t *length)
{
    if (identity == NULL || !identity->authorized_ready || length == NULL) {
        return NULL;
    }
    *length = identity->authorized_length;
    return identity->authorized_blob;
}

static int appliance_ssh_identity_base64_value(char value)
{
    if (value >= 'A' && value <= 'Z') {
        return value - 'A';
    }
    if (value >= 'a' && value <= 'z') {
        return value - 'a' + 26;
    }
    if (value >= '0' && value <= '9') {
        return value - '0' + 52;
    }
    if (value == '+') {
        return 62;
    }
    if (value == '/') {
        return 63;
    }
    return -1;
}

static bool appliance_ssh_identity_base64_decode(
    const char *text, size_t length, uint8_t *output, size_t capacity,
    size_t *output_length)
{
    size_t used = 0U;
    unsigned accumulator = 0U;
    int bits = 0;
    size_t index;

    for (index = 0U; index < length; ++index) {
        const char value = text[index];
        int decoded;

        if (value == '=' || value == '\n' || value == '\r') {
            continue;
        }
        decoded = appliance_ssh_identity_base64_value(value);
        if (decoded < 0) {
            return false;
        }
        accumulator = (accumulator << 6U) | (unsigned)decoded;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            if (used >= capacity) {
                return false;
            }
            output[used++] = (uint8_t)(accumulator >> bits);
        }
    }
    *output_length = used;
    return true;
}

static size_t appliance_ssh_identity_base64_encode(
    const uint8_t *bytes, size_t length, char *output, size_t capacity)
{
    static const char alphabet[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t written = 0U;
    size_t index;

    for (index = 0U; index < length; index += 3U) {
        const size_t remaining = length - index;
        uint32_t value = (uint32_t)bytes[index] << 16U;
        char encoded[4];

        if (remaining > 1U) {
            value |= (uint32_t)bytes[index + 1U] << 8U;
        }
        if (remaining > 2U) {
            value |= bytes[index + 2U];
        }
        encoded[0] = alphabet[(value >> 18U) & 0x3FU];
        encoded[1] = alphabet[(value >> 12U) & 0x3FU];
        encoded[2] = remaining > 1U ? alphabet[(value >> 6U) & 0x3FU] : '=';
        encoded[3] = remaining > 2U ? alphabet[value & 0x3FU] : '=';
        for (size_t part = 0U; part < sizeof(encoded); ++part) {
            if (written + 1U >= capacity) {
                return 0U;
            }
            output[written++] = encoded[part];
        }
    }
    if (written < capacity) {
        output[written] = '\0';
    }
    return written;
}

bool appliance_ssh_identity_blob_from_point_base64(
    const char *text, size_t length,
    uint8_t blob[192], size_t *blob_length, uint8_t fingerprint[32])
{
    uint8_t point[65];
    size_t point_length = 0U;
    struct hfoc_ssh_writer writer;

    if (text == NULL || blob == NULL || blob_length == NULL ||
        fingerprint == NULL) {
        return false;
    }
    if (!appliance_ssh_identity_base64_decode(
            text, length, point, sizeof(point), &point_length) ||
        point_length != sizeof(point) || point[0] != 0x04U) {
        return false;
    }
    hfoc_ssh_writer_init(&writer, blob, 192U);
    (void)hfoc_ssh_write_cstring(&writer, "ecdsa-sha2-nistp256");
    (void)hfoc_ssh_write_cstring(&writer, "nistp256");
    (void)hfoc_ssh_write_string(&writer, point, point_length);
    if (writer.failed ||
        !hfoc_ssh_crypto_public_blob_validate(blob, writer.length) ||
        !hfoc_ssh_crypto_public_blob_fingerprint(blob, writer.length,
                                                 fingerprint)) {
        return false;
    }
    *blob_length = writer.length;
    return true;
}

size_t appliance_ssh_identity_fingerprint_text(
    const uint8_t fingerprint[32], char *output, size_t capacity)
{
    size_t written = 0U;
    size_t encoded;

    if (fingerprint == NULL || output == NULL || capacity == 0U) {
        return 0U;
    }
    if (capacity < 8U) {
        return 0U;
    }
    memcpy(output, "SHA256:", 7U);
    written = 7U;
    encoded = appliance_ssh_identity_base64_encode(
        fingerprint, 32U, output + written, capacity - written);
    if (encoded == 0U) {
        return 0U;
    }
    /* OpenSSH's `SHA256:` form omits the base64 padding. */
    while (encoded != 0U && output[written + encoded - 1U] == '=') {
        output[written + encoded - 1U] = '\0';
        --encoded;
    }
    return written + encoded;
}

const char *appliance_ssh_identity_status_string(
    enum appliance_ssh_identity_status status)
{
    switch (status) {
    case APPLIANCE_SSH_IDENTITY_OK:
        return "OK";
    case APPLIANCE_SSH_IDENTITY_NULL:
        return "NULL";
    case APPLIANCE_SSH_IDENTITY_BAD_STATE:
        return "BAD_STATE";
    case APPLIANCE_SSH_IDENTITY_STORE_UNAVAILABLE:
        return "STORE_UNAVAILABLE";
    case APPLIANCE_SSH_IDENTITY_STORE_SEALED:
        return "STORE_SEALED";
    case APPLIANCE_SSH_IDENTITY_STORE_TOO_SMALL:
        return "STORE_TOO_SMALL";
    case APPLIANCE_SSH_IDENTITY_STORE_READ_FAILED:
        return "STORE_READ_FAILED";
    case APPLIANCE_SSH_IDENTITY_STORE_WRITE_FAILED:
        return "STORE_WRITE_FAILED";
    case APPLIANCE_SSH_IDENTITY_RECORD_CORRUPT:
        return "RECORD_CORRUPT";
    case APPLIANCE_SSH_IDENTITY_KEY_GENERATE_FAILED:
        return "KEY_GENERATE_FAILED";
    case APPLIANCE_SSH_IDENTITY_KEY_INVALID:
        return "KEY_INVALID";
    case APPLIANCE_SSH_IDENTITY_AUTHORIZED_MISSING:
        return "AUTHORIZED_MISSING";
    case APPLIANCE_SSH_IDENTITY_AUTHORIZED_INVALID:
        return "AUTHORIZED_INVALID";
    case APPLIANCE_SSH_IDENTITY_BUFFER_TOO_SMALL:
        return "BUFFER_TOO_SMALL";
    default:
        return NULL;
    }
}

#else
typedef int appliance_ssh_identity_translation_unit_guard;
#endif
