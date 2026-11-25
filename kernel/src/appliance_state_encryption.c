#include "appliance_state_encryption.h"

#include "appliance_state_block_device.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define APPLIANCE_STATE_ENCRYPTION_CUSTODY_MAGIC "HFOCKEY1"
#define APPLIANCE_STATE_ENCRYPTION_INFO_LABEL_LENGTH 16U
#define APPLIANCE_STATE_ENCRYPTION_BLOCK_MAC_LABEL "HFOC-STATE-BLOCK"
#define APPLIANCE_STATE_ENCRYPTION_BLOCK_MAC_LABEL_LENGTH 16U
#define APPLIANCE_STATE_ENCRYPTION_BLOCK_MAC_METADATA_LENGTH 16U
#define APPLIANCE_STATE_ENCRYPTION_TAG_COUNTER_LENGTH 4U
#define APPLIANCE_STATE_ENCRYPTION_MAX_TAG_BYTES \
    (APPLIANCE_STATE_ENCRYPTION_TAG_CACHE_LENGTH)

static const uint8_t state_encryption_custody_magic[8U] = {
    'H', 'F', 'O', 'C', 'K', 'E', 'Y', '1'
};
static const uint8_t state_encryption_enc_label[] = "HFOC-STATE-ENC";
static const uint8_t state_encryption_mac_label[] = "HFOC-STATE-MAC";
static const uint8_t state_encryption_block_label[] =
    APPLIANCE_STATE_ENCRYPTION_BLOCK_MAC_LABEL;

static void state_encryption_store_le32(uint8_t *bytes, uint32_t value)
{
    bytes[0] = (uint8_t)(value & 0xFFU);
    bytes[1] = (uint8_t)((value >> 8U) & 0xFFU);
    bytes[2] = (uint8_t)((value >> 16U) & 0xFFU);
    bytes[3] = (uint8_t)((value >> 24U) & 0xFFU);
}

static uint32_t state_encryption_load_le32(const uint8_t *bytes)
{
    return (uint32_t)bytes[0] |
           ((uint32_t)bytes[1] << 8U) |
           ((uint32_t)bytes[2] << 16U) |
           ((uint32_t)bytes[3] << 24U);
}

static void state_encryption_store_le64(uint8_t *bytes, uint64_t value)
{
    size_t index;

    for (index = 0U; index < 8U; ++index) {
        bytes[index] = (uint8_t)(value >> (8U * index));
    }
}

static enum appliance_state_encryption_status
state_encryption_tag_geometry(
    uint32_t physical_block_count,
    uint32_t *data_block_count_out,
    uint32_t *tag_block_count_out)
{
    uint32_t tag_blocks;

    if (physical_block_count <
        APPLIANCE_STATE_ENCRYPTION_MIN_STORE_BLOCKS) {
        return APPLIANCE_STATE_ENCRYPTION_TOO_SMALL;
    }
    tag_blocks = (physical_block_count + 32U) / 33U;
    if (tag_blocks == 0U ||
        tag_blocks >= physical_block_count) {
        return APPLIANCE_STATE_ENCRYPTION_UNSUPPORTED;
    }
    if (physical_block_count - tag_blocks >
        APPLIANCE_STATE_ENCRYPTION_MAX_DATA_BLOCKS) {
        return APPLIANCE_STATE_ENCRYPTION_UNSUPPORTED;
    }
    *data_block_count_out = physical_block_count - tag_blocks;
    *tag_block_count_out = tag_blocks;
    return APPLIANCE_STATE_ENCRYPTION_OK;
}

static enum appliance_state_encryption_status
state_encryption_derive_store_key(
    const uint8_t dek[APPLIANCE_STATE_CRYPTO_KEY_LENGTH],
    const uint8_t *label,
    size_t label_length,
    enum appliance_persistent_storage_namespace_id namespace_id,
    uint32_t store_id,
    uint8_t key[APPLIANCE_STATE_CRYPTO_KEY_LENGTH])
{
    uint8_t info[APPLIANCE_STATE_ENCRYPTION_INFO_LABEL_LENGTH + 8U];
    size_t offset = 0U;

    if (label_length > APPLIANCE_STATE_ENCRYPTION_INFO_LABEL_LENGTH) {
        return APPLIANCE_STATE_ENCRYPTION_UNSUPPORTED;
    }
    memcpy(info, label, label_length);
    offset = label_length;
    state_encryption_store_le32(
        info + offset, (uint32_t)namespace_id);
    offset += 4U;
    state_encryption_store_le32(info + offset, store_id);
    offset += 4U;
    if (appliance_state_crypto_hmac_sha256(
            dek, APPLIANCE_STATE_CRYPTO_KEY_LENGTH, info, offset,
            key) != APPLIANCE_STATE_CRYPTO_OK) {
        return APPLIANCE_STATE_ENCRYPTION_UNSUPPORTED;
    }
    return APPLIANCE_STATE_ENCRYPTION_OK;
}

static enum appliance_state_encryption_status
state_encryption_block_mac(
    const struct appliance_state_encryption_store *store,
    uint32_t block_index,
    uint32_t counter,
    const uint8_t ciphertext[APPLIANCE_STATE_ENCRYPTION_BLOCK_SIZE],
    uint8_t mac[APPLIANCE_STATE_ENCRYPTION_TAG_MAC_LENGTH])
{
    uint8_t input[APPLIANCE_STATE_ENCRYPTION_BLOCK_MAC_LABEL_LENGTH +
                  APPLIANCE_STATE_ENCRYPTION_BLOCK_MAC_METADATA_LENGTH +
                  APPLIANCE_STATE_ENCRYPTION_BLOCK_SIZE];
    uint8_t full_mac[APPLIANCE_STATE_CRYPTO_MAC_LENGTH];
    size_t offset = 0U;

    memcpy(input + offset, state_encryption_block_label,
           APPLIANCE_STATE_ENCRYPTION_BLOCK_MAC_LABEL_LENGTH);
    offset += APPLIANCE_STATE_ENCRYPTION_BLOCK_MAC_LABEL_LENGTH;
    state_encryption_store_le32(
        input + offset, (uint32_t)store->namespace_id);
    offset += 4U;
    state_encryption_store_le32(input + offset, store->store_id);
    offset += 4U;
    state_encryption_store_le32(input + offset, block_index);
    offset += 4U;
    state_encryption_store_le32(input + offset, counter);
    offset += 4U;
    memcpy(input + offset, ciphertext,
           APPLIANCE_STATE_ENCRYPTION_BLOCK_SIZE);
    offset += APPLIANCE_STATE_ENCRYPTION_BLOCK_SIZE;
    if (appliance_state_crypto_hmac_sha256(
            store->mac_key, sizeof(store->mac_key), input, offset,
            full_mac) != APPLIANCE_STATE_CRYPTO_OK) {
        appliance_state_crypto_zeroize(input, sizeof(input));
        return APPLIANCE_STATE_ENCRYPTION_UNSUPPORTED;
    }
    memcpy(mac, full_mac, APPLIANCE_STATE_ENCRYPTION_TAG_MAC_LENGTH);
    appliance_state_crypto_zeroize(input, sizeof(input));
    appliance_state_crypto_zeroize(full_mac, sizeof(full_mac));
    return APPLIANCE_STATE_ENCRYPTION_OK;
}

static void state_encryption_block_nonce(
    const struct appliance_state_encryption_store *store,
    uint32_t block_index,
    uint32_t counter,
    uint8_t nonce[APPLIANCE_STATE_CRYPTO_NONCE_LENGTH])
{
    memcpy(nonce, store->volume_id, sizeof(store->volume_id));
    state_encryption_store_le32(
        nonce + sizeof(store->volume_id), block_index);
    state_encryption_store_le32(
        nonce + sizeof(store->volume_id) + 4U, counter);
}

static enum appliance_state_encryption_status
state_encryption_store_ensure_tags(
    struct appliance_state_encryption_store *store)
{
    uint64_t offset;
    size_t length;

    if (store->tag_cache_valid) {
        return APPLIANCE_STATE_ENCRYPTION_OK;
    }
    offset = (uint64_t)store->data_block_count *
             APPLIANCE_STATE_ENCRYPTION_BLOCK_SIZE;
    length = (size_t)store->tag_block_count *
             APPLIANCE_STATE_ENCRYPTION_BLOCK_SIZE;
    if (length > APPLIANCE_STATE_ENCRYPTION_MAX_TAG_BYTES) {
        return APPLIANCE_STATE_ENCRYPTION_UNSUPPORTED;
    }
    if (block_device_read(&store->parent, offset, store->tag_cache,
                          length) != BLOCK_DEVICE_OK) {
        return APPLIANCE_STATE_ENCRYPTION_IO_ERROR;
    }
    store->tag_cache_valid = true;
    return APPLIANCE_STATE_ENCRYPTION_OK;
}

static enum appliance_state_encryption_status
state_encryption_store_write_tags(
    struct appliance_state_encryption_store *store)
{
    uint64_t offset;
    size_t length;

    if (!store->dirty) {
        return APPLIANCE_STATE_ENCRYPTION_OK;
    }
    offset = (uint64_t)store->data_block_count *
             APPLIANCE_STATE_ENCRYPTION_BLOCK_SIZE;
    length = (size_t)store->tag_block_count *
             APPLIANCE_STATE_ENCRYPTION_BLOCK_SIZE;
    if (block_device_write(&store->parent, offset, store->tag_cache,
                           length) != BLOCK_DEVICE_OK) {
        return APPLIANCE_STATE_ENCRYPTION_IO_ERROR;
    }
    store->dirty = false;
    return APPLIANCE_STATE_ENCRYPTION_OK;
}

static enum appliance_state_encryption_status
state_encryption_store_write_block(
    struct appliance_state_encryption_store *store,
    uint32_t block_index,
    const uint8_t plaintext[APPLIANCE_STATE_ENCRYPTION_BLOCK_SIZE],
    bool durable,
    bool verify_existing)
{
    uint8_t *entry;
    uint8_t current[APPLIANCE_STATE_ENCRYPTION_BLOCK_SIZE];
    uint8_t ciphertext[APPLIANCE_STATE_ENCRYPTION_BLOCK_SIZE];
    uint8_t nonce[APPLIANCE_STATE_CRYPTO_NONCE_LENGTH];
    uint8_t mac[APPLIANCE_STATE_ENCRYPTION_TAG_MAC_LENGTH];
    uint32_t previous_counter;
    uint32_t counter;
    enum appliance_state_encryption_status status;

    if (block_index >= store->data_block_count) {
        return APPLIANCE_STATE_ENCRYPTION_BAD_RANGE;
    }
    status = state_encryption_store_ensure_tags(store);
    if (status != APPLIANCE_STATE_ENCRYPTION_OK) {
        return status;
    }
    entry = store->tag_cache +
            (size_t)block_index *
                APPLIANCE_STATE_ENCRYPTION_TAG_ENTRY_LENGTH;
    previous_counter = state_encryption_load_le32(entry);
    if (previous_counter == UINT32_MAX) {
        return APPLIANCE_STATE_ENCRYPTION_COUNTER_EXHAUSTED;
    }
    if (verify_existing) {
        /* Authenticate the current block before advancing its counter: a
         * tampered tag entry must not let the next write reuse a nonce. */
        if (block_device_read(
                &store->parent,
                (uint64_t)block_index *
                    APPLIANCE_STATE_ENCRYPTION_BLOCK_SIZE,
                current,
                APPLIANCE_STATE_ENCRYPTION_BLOCK_SIZE) !=
            BLOCK_DEVICE_OK) {
            return APPLIANCE_STATE_ENCRYPTION_IO_ERROR;
        }
        status = state_encryption_block_mac(
            store, block_index, previous_counter, current, mac);
        if (status != APPLIANCE_STATE_ENCRYPTION_OK) {
            appliance_state_crypto_zeroize(current, sizeof(current));
            return status;
        }
        if (!appliance_state_crypto_equal(
                mac,
                entry + APPLIANCE_STATE_ENCRYPTION_TAG_COUNTER_LENGTH,
                sizeof(mac))) {
            appliance_state_crypto_zeroize(current, sizeof(current));
            appliance_state_crypto_zeroize(mac, sizeof(mac));
            return APPLIANCE_STATE_ENCRYPTION_AUTH_FAILED;
        }
        appliance_state_crypto_zeroize(current, sizeof(current));
    }
    counter = previous_counter + 1U;
    state_encryption_block_nonce(store, block_index, counter, nonce);
    if (appliance_state_crypto_ctr(
            store->enc_key, nonce, plaintext, ciphertext,
            APPLIANCE_STATE_ENCRYPTION_BLOCK_SIZE) !=
        APPLIANCE_STATE_CRYPTO_OK) {
        return APPLIANCE_STATE_ENCRYPTION_UNSUPPORTED;
    }
    status = state_encryption_block_mac(
        store, block_index, counter, ciphertext, mac);
    if (status != APPLIANCE_STATE_ENCRYPTION_OK) {
        appliance_state_crypto_zeroize(ciphertext, sizeof(ciphertext));
        return status;
    }
    if (block_device_write(
            &store->parent,
            (uint64_t)block_index *
                APPLIANCE_STATE_ENCRYPTION_BLOCK_SIZE,
            ciphertext,
            APPLIANCE_STATE_ENCRYPTION_BLOCK_SIZE) != BLOCK_DEVICE_OK) {
        appliance_state_crypto_zeroize(ciphertext, sizeof(ciphertext));
        return APPLIANCE_STATE_ENCRYPTION_IO_ERROR;
    }
    state_encryption_store_le32(entry, counter);
    memcpy(entry + APPLIANCE_STATE_ENCRYPTION_TAG_COUNTER_LENGTH, mac,
           sizeof(mac));
    store->dirty = true;
    appliance_state_crypto_zeroize(ciphertext, sizeof(ciphertext));
    appliance_state_crypto_zeroize(nonce, sizeof(nonce));
    appliance_state_crypto_zeroize(mac, sizeof(mac));
    if (durable) {
        return state_encryption_store_write_tags(store);
    }
    return APPLIANCE_STATE_ENCRYPTION_OK;
}

static enum block_device_status state_encryption_store_read(
    struct block_device *device,
    uint64_t offset,
    void *buffer,
    size_t length)
{
    struct appliance_state_encryption_store *store =
        (struct appliance_state_encryption_store *)device->context;
    uint8_t ciphertext[APPLIANCE_STATE_ENCRYPTION_BLOCK_SIZE];
    uint8_t nonce[APPLIANCE_STATE_CRYPTO_NONCE_LENGTH];
    uint8_t mac[APPLIANCE_STATE_ENCRYPTION_TAG_MAC_LENGTH];
    uint8_t *output = (uint8_t *)buffer;
    uint32_t first_block;
    uint32_t block_count;
    uint32_t index;

    if (store == NULL || !store->initialized || buffer == NULL) {
        return BLOCK_DEVICE_BAD_DEVICE;
    }
    if (length == 0U ||
        (offset % APPLIANCE_STATE_ENCRYPTION_BLOCK_SIZE) != 0U ||
        (length % APPLIANCE_STATE_ENCRYPTION_BLOCK_SIZE) != 0U) {
        return BLOCK_DEVICE_UNALIGNED;
    }
    first_block =
        (uint32_t)(offset / APPLIANCE_STATE_ENCRYPTION_BLOCK_SIZE);
    block_count =
        (uint32_t)(length / APPLIANCE_STATE_ENCRYPTION_BLOCK_SIZE);
    if (first_block >= store->data_block_count ||
        block_count > store->data_block_count - first_block) {
        return BLOCK_DEVICE_OUT_OF_RANGE;
    }
    if (state_encryption_store_ensure_tags(store) !=
        APPLIANCE_STATE_ENCRYPTION_OK) {
        return BLOCK_DEVICE_IO_ERROR;
    }

    for (index = 0U; index < block_count; ++index) {
        const uint32_t block_index = first_block + index;
        const uint8_t *entry =
            store->tag_cache +
            (size_t)block_index *
                APPLIANCE_STATE_ENCRYPTION_TAG_ENTRY_LENGTH;
        const uint32_t counter = state_encryption_load_le32(entry);

        if (block_device_read(
                &store->parent,
                (uint64_t)block_index *
                    APPLIANCE_STATE_ENCRYPTION_BLOCK_SIZE,
                ciphertext,
                APPLIANCE_STATE_ENCRYPTION_BLOCK_SIZE) !=
            BLOCK_DEVICE_OK) {
            return BLOCK_DEVICE_IO_ERROR;
        }
        if (state_encryption_block_mac(
                store, block_index, counter, ciphertext, mac) !=
            APPLIANCE_STATE_ENCRYPTION_OK) {
            return BLOCK_DEVICE_IO_ERROR;
        }
        if (!appliance_state_crypto_equal(
                mac, entry + APPLIANCE_STATE_ENCRYPTION_TAG_COUNTER_LENGTH,
                sizeof(mac))) {
            appliance_state_crypto_zeroize(ciphertext,
                                           sizeof(ciphertext));
            return BLOCK_DEVICE_IO_ERROR;
        }
        state_encryption_block_nonce(store, block_index, counter, nonce);
        if (appliance_state_crypto_ctr(
                store->enc_key, nonce, ciphertext,
                output + (size_t)index *
                             APPLIANCE_STATE_ENCRYPTION_BLOCK_SIZE,
                APPLIANCE_STATE_ENCRYPTION_BLOCK_SIZE) !=
            APPLIANCE_STATE_CRYPTO_OK) {
            appliance_state_crypto_zeroize(ciphertext,
                                           sizeof(ciphertext));
            return BLOCK_DEVICE_IO_ERROR;
        }
    }
    appliance_state_crypto_zeroize(ciphertext, sizeof(ciphertext));
    appliance_state_crypto_zeroize(nonce, sizeof(nonce));
    appliance_state_crypto_zeroize(mac, sizeof(mac));
    return BLOCK_DEVICE_OK;
}

static enum block_device_status state_encryption_store_write(
    struct block_device *device,
    uint64_t offset,
    const void *buffer,
    size_t length)
{
    struct appliance_state_encryption_store *store =
        (struct appliance_state_encryption_store *)device->context;
    const uint8_t *input = (const uint8_t *)buffer;
    uint32_t first_block;
    uint32_t block_count;
    uint32_t index;

    if (store == NULL || !store->initialized || buffer == NULL) {
        return BLOCK_DEVICE_BAD_DEVICE;
    }
    if (length == 0U ||
        (offset % APPLIANCE_STATE_ENCRYPTION_BLOCK_SIZE) != 0U ||
        (length % APPLIANCE_STATE_ENCRYPTION_BLOCK_SIZE) != 0U) {
        return BLOCK_DEVICE_UNALIGNED;
    }
    first_block =
        (uint32_t)(offset / APPLIANCE_STATE_ENCRYPTION_BLOCK_SIZE);
    block_count =
        (uint32_t)(length / APPLIANCE_STATE_ENCRYPTION_BLOCK_SIZE);
    if (first_block >= store->data_block_count ||
        block_count > store->data_block_count - first_block) {
        return BLOCK_DEVICE_OUT_OF_RANGE;
    }

    for (index = 0U; index < block_count; ++index) {
        if (state_encryption_store_write_block(
                store, first_block + index,
                input + (size_t)index *
                            APPLIANCE_STATE_ENCRYPTION_BLOCK_SIZE,
                false, true) != APPLIANCE_STATE_ENCRYPTION_OK) {
            return BLOCK_DEVICE_IO_ERROR;
        }
    }
    if (state_encryption_store_write_tags(store) !=
        APPLIANCE_STATE_ENCRYPTION_OK) {
        return BLOCK_DEVICE_IO_ERROR;
    }
    return BLOCK_DEVICE_OK;
}

static enum block_device_status state_encryption_store_flush(
    struct block_device *device)
{
    struct appliance_state_encryption_store *store =
        (struct appliance_state_encryption_store *)device->context;

    if (store == NULL || !store->initialized) {
        return BLOCK_DEVICE_BAD_DEVICE;
    }
    if (state_encryption_store_write_tags(store) !=
        APPLIANCE_STATE_ENCRYPTION_OK) {
        return BLOCK_DEVICE_IO_ERROR;
    }
    if (block_device_flush(&store->parent) != BLOCK_DEVICE_OK) {
        return BLOCK_DEVICE_IO_ERROR;
    }
    return BLOCK_DEVICE_OK;
}

enum appliance_state_encryption_status
appliance_state_encryption_volume_probe(
    struct appliance_state_encryption_volume *volume,
    struct block_device *partition_device)
{
    uint8_t custody_bytes[APPLIANCE_KEY_CUSTODY_RECORD_LENGTH];

    if (volume == NULL || partition_device == NULL) {
        return APPLIANCE_STATE_ENCRYPTION_NULL;
    }
    memset(volume, 0, sizeof(*volume));
    volume->initialized = true;
    if (block_device_validate(partition_device) != BLOCK_DEVICE_OK ||
        partition_device->block_size !=
            APPLIANCE_STATE_ENCRYPTION_BLOCK_SIZE ||
        partition_device->block_count < 2U) {
        return APPLIANCE_STATE_ENCRYPTION_BAD_DEVICE;
    }
    if (block_device_read(
            partition_device,
            (partition_device->block_count - 1U) *
                APPLIANCE_STATE_ENCRYPTION_BLOCK_SIZE,
            custody_bytes, sizeof(custody_bytes)) != BLOCK_DEVICE_OK) {
        return APPLIANCE_STATE_ENCRYPTION_IO_ERROR;
    }
    if (memcmp(custody_bytes, state_encryption_custody_magic,
               sizeof(state_encryption_custody_magic)) != 0) {
        return APPLIANCE_STATE_ENCRYPTION_NOT_ENCRYPTED;
    }
    if (appliance_key_custody_record_decode(
            custody_bytes, &volume->record) !=
        APPLIANCE_KEY_CUSTODY_OK) {
        return APPLIANCE_STATE_ENCRYPTION_CORRUPT;
    }
    volume->sealed = true;
    return APPLIANCE_STATE_ENCRYPTION_OK;
}

enum appliance_state_encryption_status
appliance_state_encryption_volume_unlock(
    struct appliance_state_encryption_volume *volume,
    const struct appliance_key_custody_material *material)
{
    enum appliance_key_custody_status status;

    if (volume == NULL) {
        return APPLIANCE_STATE_ENCRYPTION_NULL;
    }
    if (!volume->initialized) {
        return APPLIANCE_STATE_ENCRYPTION_BAD_CUSTODY;
    }
    if (!volume->sealed) {
        return APPLIANCE_STATE_ENCRYPTION_NOT_ENCRYPTED;
    }
    if (volume->unlocked) {
        return APPLIANCE_STATE_ENCRYPTION_OK;
    }
    status = appliance_key_custody_open(
        &volume->custody, &volume->record, material);
    switch (status) {
    case APPLIANCE_KEY_CUSTODY_OK:
        volume->unlocked = true;
        return APPLIANCE_STATE_ENCRYPTION_OK;
    case APPLIANCE_KEY_CUSTODY_KEY_REQUIRED:
        return APPLIANCE_STATE_ENCRYPTION_KEY_REQUIRED;
    case APPLIANCE_KEY_CUSTODY_WRONG_KEY:
        return APPLIANCE_STATE_ENCRYPTION_WRONG_KEY;
    case APPLIANCE_KEY_CUSTODY_TPM_UNAVAILABLE:
        return APPLIANCE_STATE_ENCRYPTION_TPM_UNAVAILABLE;
    case APPLIANCE_KEY_CUSTODY_AUTH_FAILED:
    case APPLIANCE_KEY_CUSTODY_BAD_MAGIC:
    case APPLIANCE_KEY_CUSTODY_BAD_VERSION:
    case APPLIANCE_KEY_CUSTODY_BAD_BINDING:
    case APPLIANCE_KEY_CUSTODY_BAD_KDF:
    case APPLIANCE_KEY_CUSTODY_BAD_ITERATIONS:
        return APPLIANCE_STATE_ENCRYPTION_CORRUPT;
    default:
        return APPLIANCE_STATE_ENCRYPTION_BAD_CUSTODY;
    }
}

enum appliance_state_encryption_status
appliance_state_encryption_store_bind(
    const struct appliance_state_encryption_volume *volume,
    enum appliance_persistent_storage_namespace_id namespace_id,
    uint32_t store_id,
    struct block_device *store_range,
    struct appliance_state_encryption_store *store)
{
    enum appliance_state_encryption_status status;
    uint32_t data_block_count = 0U;
    uint32_t tag_block_count = 0U;

    if (volume == NULL || store_range == NULL || store == NULL) {
        return APPLIANCE_STATE_ENCRYPTION_NULL;
    }
    if (!volume->initialized || !volume->sealed || !volume->unlocked ||
        store_id == 0U) {
        return APPLIANCE_STATE_ENCRYPTION_BAD_CUSTODY;
    }
    if (block_device_validate(store_range) != BLOCK_DEVICE_OK ||
        store_range->block_size !=
            APPLIANCE_STATE_ENCRYPTION_BLOCK_SIZE) {
        return APPLIANCE_STATE_ENCRYPTION_BAD_DEVICE;
    }
    status = state_encryption_tag_geometry(
        (uint32_t)store_range->block_count, &data_block_count,
        &tag_block_count);
    if (status != APPLIANCE_STATE_ENCRYPTION_OK) {
        return status;
    }

    memset(store, 0, sizeof(*store));
    store->parent = *store_range;
    store->namespace_id = namespace_id;
    store->store_id = store_id;
    store->physical_block_count = (uint32_t)store_range->block_count;
    store->data_block_count = data_block_count;
    store->tag_block_count = tag_block_count;
    memcpy(store->volume_id, volume->custody.volume_id,
           sizeof(store->volume_id));
    status = state_encryption_derive_store_key(
        volume->custody.dek, state_encryption_enc_label,
        sizeof(state_encryption_enc_label) - 1U, namespace_id, store_id,
        store->enc_key);
    if (status != APPLIANCE_STATE_ENCRYPTION_OK) {
        memset(store, 0, sizeof(*store));
        return status;
    }
    status = state_encryption_derive_store_key(
        volume->custody.dek, state_encryption_mac_label,
        sizeof(state_encryption_mac_label) - 1U, namespace_id, store_id,
        store->mac_key);
    if (status != APPLIANCE_STATE_ENCRYPTION_OK) {
        memset(store, 0, sizeof(*store));
        return status;
    }
    store->device.block_size = APPLIANCE_STATE_ENCRYPTION_BLOCK_SIZE;
    store->device.block_count = data_block_count;
    store->device.context = store;
    store->device.read_fn = state_encryption_store_read;
    store->device.write_fn = state_encryption_store_write;
    store->device.flush_fn = state_encryption_store_flush;
    store->initialized = true;
    return APPLIANCE_STATE_ENCRYPTION_OK;
}

enum appliance_state_encryption_status appliance_state_encryption_install(
    struct block_device *partition_device,
    const struct appliance_state_layout *layout,
    const struct appliance_key_custody_material *passphrase,
    appliance_state_crypto_random_fn random,
    void *random_context,
    struct appliance_state_encryption_install_result *result)
{
    struct appliance_state_encryption_volume volume;
    uint8_t custody_bytes[APPLIANCE_KEY_CUSTODY_RECORD_LENGTH];
    uint8_t zero_block[APPLIANCE_STATE_ENCRYPTION_BLOCK_SIZE];
    char recovery_text[APPLIANCE_KEY_CUSTODY_RECOVERY_TEXT_LENGTH + 1U];
    uint32_t index;

    if (partition_device == NULL || layout == NULL || passphrase == NULL ||
        random == NULL || result == NULL) {
        return APPLIANCE_STATE_ENCRYPTION_NULL;
    }
    memset(result, 0, sizeof(*result));
    memset(&volume, 0, sizeof(volume));
    memset(custody_bytes, 0, sizeof(custody_bytes));
    memset(zero_block, 0, sizeof(zero_block));
    memset(recovery_text, 0, sizeof(recovery_text));

    if (block_device_validate(partition_device) != BLOCK_DEVICE_OK ||
        partition_device->block_size !=
            APPLIANCE_STATE_ENCRYPTION_BLOCK_SIZE) {
        return APPLIANCE_STATE_ENCRYPTION_BAD_DEVICE;
    }
    if (appliance_state_layout_validate(layout) !=
        APPLIANCE_STATE_LAYOUT_OK) {
        return APPLIANCE_STATE_ENCRYPTION_BAD_LAYOUT;
    }
    if (partition_device->block_count <= layout->block_capacity) {
        return APPLIANCE_STATE_ENCRYPTION_TOO_SMALL;
    }

    if (appliance_key_custody_create(
            &volume.custody, random, random_context, passphrase,
            &volume.record, recovery_text) != APPLIANCE_KEY_CUSTODY_OK) {
        return APPLIANCE_STATE_ENCRYPTION_RANDOM_FAILED;
    }
    if (appliance_key_custody_record_encode(
            &volume.record, custody_bytes) != APPLIANCE_KEY_CUSTODY_OK) {
        appliance_key_custody_clear(&volume.custody);
        return APPLIANCE_STATE_ENCRYPTION_BAD_CUSTODY;
    }
    if (block_device_write(
            partition_device,
            (partition_device->block_count - 1U) *
                APPLIANCE_STATE_ENCRYPTION_BLOCK_SIZE,
            custody_bytes, sizeof(custody_bytes)) != BLOCK_DEVICE_OK) {
        appliance_key_custody_clear(&volume.custody);
        return APPLIANCE_STATE_ENCRYPTION_IO_ERROR;
    }
    volume.initialized = true;
    volume.sealed = true;
    volume.unlocked = true;

    for (index = 0U; index < layout->entry_count; ++index) {
        struct appliance_state_block_device range;
        struct appliance_state_encryption_store store;
        uint8_t header[APPLIANCE_STATE_ENCRYPTION_BLOCK_SIZE];
        uint32_t block_index;

        if (appliance_state_block_device_bind_range(
                partition_device, layout->entries[index].block_offset,
                layout->entries[index].block_count, &range) !=
            APPLIANCE_STATE_BLOCK_DEVICE_OK) {
            appliance_key_custody_clear(&volume.custody);
            return APPLIANCE_STATE_ENCRYPTION_BAD_LAYOUT;
        }
        if (appliance_state_encryption_store_bind(
                &volume, layout->entries[index].namespace_id,
                layout->entries[index].store_id, &range.device,
                &store) != APPLIANCE_STATE_ENCRYPTION_OK) {
            appliance_key_custody_clear(&volume.custody);
            return APPLIANCE_STATE_ENCRYPTION_UNSUPPORTED;
        }
        for (block_index = 0U;
             block_index < store.data_block_count; ++block_index) {
            if (state_encryption_store_write_block(
                    &store, block_index, zero_block, false, false) !=
                APPLIANCE_STATE_ENCRYPTION_OK) {
                appliance_key_custody_clear(&volume.custody);
                return APPLIANCE_STATE_ENCRYPTION_IO_ERROR;
            }
        }
        memset(header, 0, sizeof(header));
        memcpy(header, "HFOCDATA", 8U);
        header[8U] = 1U;
        state_encryption_store_le64(header + 16U, 1U);
        state_encryption_store_le64(
            header + 24U, (uint64_t)store.data_block_count - 1U);
        if (block_device_write(
                &store.device, 0U, header, sizeof(header)) !=
                BLOCK_DEVICE_OK ||
            state_encryption_store_write_tags(&store) !=
                APPLIANCE_STATE_ENCRYPTION_OK ||
            block_device_flush(&store.device) != BLOCK_DEVICE_OK) {
            appliance_key_custody_clear(&volume.custody);
            return APPLIANCE_STATE_ENCRYPTION_IO_ERROR;
        }
        ++result->sealed_store_count;
    }

    if (block_device_flush(partition_device) != BLOCK_DEVICE_OK) {
        appliance_key_custody_clear(&volume.custody);
        return APPLIANCE_STATE_ENCRYPTION_IO_ERROR;
    }
    memcpy(result->dek_fingerprint, volume.custody.dek_fingerprint,
           sizeof(result->dek_fingerprint));
    memcpy(result->recovery_fingerprint,
           volume.record.recovery_fingerprint,
           sizeof(result->recovery_fingerprint));
    memcpy(result->recovery_text, recovery_text,
           sizeof(result->recovery_text));
    appliance_key_custody_clear(&volume.custody);
    appliance_state_crypto_zeroize(recovery_text, sizeof(recovery_text));
    return APPLIANCE_STATE_ENCRYPTION_OK;
}

bool appliance_state_encryption_volume_sealed(
    const struct appliance_state_encryption_volume *volume)
{
    return volume != NULL && volume->initialized && volume->sealed;
}

bool appliance_state_encryption_volume_unlocked(
    const struct appliance_state_encryption_volume *volume)
{
    return volume != NULL && volume->initialized && volume->sealed &&
           volume->unlocked;
}

const struct block_device *appliance_state_encryption_store_device(
    const struct appliance_state_encryption_store *store)
{
    if (store == NULL || !store->initialized) {
        return NULL;
    }
    return &store->device;
}

const char *appliance_state_encryption_status_string(
    enum appliance_state_encryption_status status)
{
    switch (status) {
    case APPLIANCE_STATE_ENCRYPTION_OK:
        return "OK";
    case APPLIANCE_STATE_ENCRYPTION_NULL:
        return "NULL";
    case APPLIANCE_STATE_ENCRYPTION_BAD_DEVICE:
        return "BAD_DEVICE";
    case APPLIANCE_STATE_ENCRYPTION_BAD_LAYOUT:
        return "BAD_LAYOUT";
    case APPLIANCE_STATE_ENCRYPTION_BAD_CUSTODY:
        return "BAD_CUSTODY";
    case APPLIANCE_STATE_ENCRYPTION_NOT_ENCRYPTED:
        return "NOT_ENCRYPTED";
    case APPLIANCE_STATE_ENCRYPTION_KEY_REQUIRED:
        return "KEY_REQUIRED";
    case APPLIANCE_STATE_ENCRYPTION_WRONG_KEY:
        return "WRONG_KEY";
    case APPLIANCE_STATE_ENCRYPTION_TPM_UNAVAILABLE:
        return "TPM_UNAVAILABLE";
    case APPLIANCE_STATE_ENCRYPTION_CORRUPT:
        return "CORRUPT";
    case APPLIANCE_STATE_ENCRYPTION_IO_ERROR:
        return "IO_ERROR";
    case APPLIANCE_STATE_ENCRYPTION_AUTH_FAILED:
        return "AUTH_FAILED";
    case APPLIANCE_STATE_ENCRYPTION_TOO_SMALL:
        return "TOO_SMALL";
    case APPLIANCE_STATE_ENCRYPTION_RANDOM_FAILED:
        return "RANDOM_FAILED";
    case APPLIANCE_STATE_ENCRYPTION_UNSUPPORTED:
        return "UNSUPPORTED";
    case APPLIANCE_STATE_ENCRYPTION_COUNTER_EXHAUSTED:
        return "COUNTER_EXHAUSTED";
    case APPLIANCE_STATE_ENCRYPTION_BAD_RANGE:
        return "BAD_RANGE";
    default:
        return "UNKNOWN";
    }
}
