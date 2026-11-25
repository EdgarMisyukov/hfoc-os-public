#include "appliance_ssh_authorized_key.h"

#include "appliance_state_block_device.h"
#include "appliance_state_layout.h"
#include "appliance_state_loader.h"
#include "hfoc_ssh_wire.h"
#include "sha256.h"

#include <string.h>

#define APPLIANCE_SSH_AUTHORIZED_MAGIC "HFSSHAUT"
#define APPLIANCE_SSH_AUTHORIZED_VERSION 1U
#define APPLIANCE_SSH_AUTHORIZED_LENGTH_OFFSET 16U
#define APPLIANCE_SSH_AUTHORIZED_BLOB_OFFSET 20U
#define APPLIANCE_SSH_AUTHORIZED_FINGERPRINT_OFFSET 212U
#define APPLIANCE_SSH_AUTHORIZED_CHECKSUM_OFFSET 244U
#define APPLIANCE_SSH_AUTHORIZED_CHECKSUM_BYTES 32U

static int appliance_ssh_authorized_base64_value(char value)
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

static bool appliance_ssh_authorized_base64_decode(
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
        decoded = appliance_ssh_authorized_base64_value(value);
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

static size_t appliance_ssh_authorized_base64_encode(
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

static void appliance_ssh_authorized_store_u32(uint8_t *bytes,
                                               uint32_t value)
{
    bytes[0] = (uint8_t)(value >> 24U);
    bytes[1] = (uint8_t)(value >> 16U);
    bytes[2] = (uint8_t)(value >> 8U);
    bytes[3] = (uint8_t)value;
}

static uint32_t appliance_ssh_authorized_load_u32(const uint8_t *bytes)
{
    return ((uint32_t)bytes[0] << 24U) |
           ((uint32_t)bytes[1] << 16U) |
           ((uint32_t)bytes[2] << 8U) |
           (uint32_t)bytes[3];
}

static bool appliance_ssh_authorized_checksum(
    const uint8_t *record, uint8_t digest[32])
{
    return sha256_compute(record, APPLIANCE_SSH_AUTHORIZED_CHECKSUM_OFFSET,
                          digest) == SHA256_OK;
}

static bool appliance_ssh_authorized_record_build(
    const uint8_t *blob, size_t blob_length,
    const uint8_t fingerprint[32],
    uint8_t record[APPLIANCE_SSH_AUTHORIZED_RECORD_BYTES])
{
    memset(record, 0, APPLIANCE_SSH_AUTHORIZED_RECORD_BYTES);
    memcpy(record, APPLIANCE_SSH_AUTHORIZED_MAGIC, 8U);
    appliance_ssh_authorized_store_u32(
        record + 8U, APPLIANCE_SSH_AUTHORIZED_VERSION);
    appliance_ssh_authorized_store_u32(record + 12U, 1U);
    appliance_ssh_authorized_store_u32(
        record + APPLIANCE_SSH_AUTHORIZED_LENGTH_OFFSET,
        (uint32_t)blob_length);
    memcpy(record + APPLIANCE_SSH_AUTHORIZED_BLOB_OFFSET, blob,
           blob_length);
    memcpy(record + APPLIANCE_SSH_AUTHORIZED_FINGERPRINT_OFFSET, fingerprint,
           32U);
    return appliance_ssh_authorized_checksum(
        record, record + APPLIANCE_SSH_AUTHORIZED_CHECKSUM_OFFSET);
}

static bool appliance_ssh_authorized_record_valid(
    const uint8_t *record, const uint8_t *blob, size_t blob_length,
    const uint8_t fingerprint[32])
{
    uint8_t digest[32];
    uint32_t stored_length;

    if (memcmp(record, APPLIANCE_SSH_AUTHORIZED_MAGIC, 8U) != 0 ||
        appliance_ssh_authorized_load_u32(record + 8U) !=
            APPLIANCE_SSH_AUTHORIZED_VERSION ||
        appliance_ssh_authorized_load_u32(record + 12U) != 1U) {
        return false;
    }
    stored_length = appliance_ssh_authorized_load_u32(
        record + APPLIANCE_SSH_AUTHORIZED_LENGTH_OFFSET);
    if (stored_length != blob_length ||
        memcmp(record + APPLIANCE_SSH_AUTHORIZED_BLOB_OFFSET, blob,
               blob_length) != 0 ||
        memcmp(record + APPLIANCE_SSH_AUTHORIZED_FINGERPRINT_OFFSET,
               fingerprint, 32U) != 0 ||
        !appliance_ssh_authorized_checksum(record, digest)) {
        return false;
    }
    return memcmp(digest,
                  record + APPLIANCE_SSH_AUTHORIZED_CHECKSUM_OFFSET,
                  APPLIANCE_SSH_AUTHORIZED_CHECKSUM_BYTES) == 0;
}

bool appliance_ssh_authorized_key_from_point(
    const char *text, size_t length,
    uint8_t blob[APPLIANCE_SSH_AUTHORIZED_BLOB_MAX], size_t *blob_length,
    uint8_t fingerprint[32])
{
    uint8_t point[65];
    size_t point_length = 0U;
    struct hfoc_ssh_writer writer;

    if (text == NULL || blob == NULL || blob_length == NULL ||
        fingerprint == NULL) {
        return false;
    }
    if (!appliance_ssh_authorized_base64_decode(
            text, length, point, sizeof(point), &point_length) ||
        point_length != sizeof(point) || point[0] != 0x04U) {
        return false;
    }
    hfoc_ssh_writer_init(&writer, blob,
                         APPLIANCE_SSH_AUTHORIZED_BLOB_MAX);
    (void)hfoc_ssh_write_cstring(&writer, "ecdsa-sha2-nistp256");
    (void)hfoc_ssh_write_cstring(&writer, "nistp256");
    (void)hfoc_ssh_write_string(&writer, point, point_length);
    if (writer.failed ||
        sha256_compute(blob, writer.length, fingerprint) != SHA256_OK) {
        return false;
    }
    *blob_length = writer.length;
    return true;
}

size_t appliance_ssh_authorized_key_fingerprint_text(
    const uint8_t fingerprint[32], char *output, size_t capacity)
{
    size_t written = 0U;
    size_t encoded;

    if (fingerprint == NULL || output == NULL || capacity < 8U) {
        return 0U;
    }
    memcpy(output, "SHA256:", 7U);
    written = 7U;
    encoded = appliance_ssh_authorized_base64_encode(
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

enum appliance_ssh_authorized_key_status
appliance_ssh_authorized_key_store(
    struct appliance_state_loader *loader,
    const uint8_t *blob, size_t blob_length)
{
    const struct appliance_state_layout_entry *entry = NULL;
    struct appliance_state_block_device store;
    uint8_t record[APPLIANCE_SSH_AUTHORIZED_RECORD_BYTES];
    uint8_t readback[APPLIANCE_SSH_AUTHORIZED_RECORD_BYTES];
    uint8_t fingerprint[32];

    if (loader == NULL || blob == NULL || blob_length == 0U ||
        blob_length > APPLIANCE_SSH_AUTHORIZED_BLOB_MAX) {
        return APPLIANCE_SSH_AUTHORIZED_KEY_NULL;
    }
    if (!loader->ready) {
        return APPLIANCE_SSH_AUTHORIZED_KEY_STORE_UNAVAILABLE;
    }
#if APPLIANCE_STATE_ENCRYPTION_ENABLED
    if (loader->encryption_sealed) {
        return APPLIANCE_SSH_AUTHORIZED_KEY_STORE_SEALED;
    }
#endif
    if (appliance_state_layout_find(
            &loader->layout,
            APPLIANCE_PERSISTENT_STORAGE_NAMESPACE_ID_CERTIFICATE,
            APPLIANCE_STATE_STORE_ID_CERTIFICATE, &entry) !=
            APPLIANCE_STATE_LAYOUT_OK ||
        entry == NULL) {
        return APPLIANCE_SSH_AUTHORIZED_KEY_STORE_UNAVAILABLE;
    }
    if (entry->block_count <
        APPLIANCE_SSH_AUTHORIZED_CERT_TAIL_BLOCK_OFFSET +
            APPLIANCE_SSH_AUTHORIZED_HOST_KEY_RECORD_BLOCKS +
            APPLIANCE_SSH_AUTHORIZED_RECORD_BLOCKS) {
        return APPLIANCE_SSH_AUTHORIZED_KEY_STORE_TOO_SMALL;
    }
    if (appliance_state_block_device_bind_range(
            &loader->partition.device,
            entry->block_offset +
                APPLIANCE_SSH_AUTHORIZED_CERT_TAIL_BLOCK_OFFSET +
                APPLIANCE_SSH_AUTHORIZED_HOST_KEY_RECORD_BLOCKS,
            APPLIANCE_SSH_AUTHORIZED_RECORD_BLOCKS, &store) !=
        APPLIANCE_STATE_BLOCK_DEVICE_OK) {
        return APPLIANCE_SSH_AUTHORIZED_KEY_STORE_UNAVAILABLE;
    }
    if (sha256_compute(blob, blob_length, fingerprint) != SHA256_OK) {
        return APPLIANCE_SSH_AUTHORIZED_KEY_RECORD_CORRUPT;
    }
    if (!appliance_ssh_authorized_record_build(
            blob, blob_length, fingerprint, record)) {
        return APPLIANCE_SSH_AUTHORIZED_KEY_RECORD_CORRUPT;
    }
    if (appliance_state_block_device_write(
            &store, 0U, record, sizeof(record)) != BLOCK_DEVICE_OK ||
        appliance_state_block_device_flush(&store) != BLOCK_DEVICE_OK) {
        return APPLIANCE_SSH_AUTHORIZED_KEY_STORE_WRITE_FAILED;
    }
    memset(readback, 0, sizeof(readback));
    if (appliance_state_block_device_read(
            &store, 0U, readback, sizeof(readback)) != BLOCK_DEVICE_OK) {
        return APPLIANCE_SSH_AUTHORIZED_KEY_STORE_READ_FAILED;
    }
    return appliance_ssh_authorized_record_valid(
               readback, blob, blob_length, fingerprint)
           ? APPLIANCE_SSH_AUTHORIZED_KEY_OK
           : APPLIANCE_SSH_AUTHORIZED_KEY_RECORD_CORRUPT;
}

const char *appliance_ssh_authorized_key_status_string(
    enum appliance_ssh_authorized_key_status status)
{
    switch (status) {
    case APPLIANCE_SSH_AUTHORIZED_KEY_OK:
        return "OK";
    case APPLIANCE_SSH_AUTHORIZED_KEY_NULL:
        return "NULL";
    case APPLIANCE_SSH_AUTHORIZED_KEY_BAD_POINT:
        return "BAD_POINT";
    case APPLIANCE_SSH_AUTHORIZED_KEY_STORE_UNAVAILABLE:
        return "STORE_UNAVAILABLE";
    case APPLIANCE_SSH_AUTHORIZED_KEY_STORE_SEALED:
        return "STORE_SEALED";
    case APPLIANCE_SSH_AUTHORIZED_KEY_STORE_TOO_SMALL:
        return "STORE_TOO_SMALL";
    case APPLIANCE_SSH_AUTHORIZED_KEY_STORE_READ_FAILED:
        return "STORE_READ_FAILED";
    case APPLIANCE_SSH_AUTHORIZED_KEY_STORE_WRITE_FAILED:
        return "STORE_WRITE_FAILED";
    case APPLIANCE_SSH_AUTHORIZED_KEY_RECORD_CORRUPT:
        return "RECORD_CORRUPT";
    default:
        return NULL;
    }
}
