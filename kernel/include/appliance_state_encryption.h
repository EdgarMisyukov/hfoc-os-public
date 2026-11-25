#ifndef HFOC_APPLIANCE_STATE_ENCRYPTION_H
#define HFOC_APPLIANCE_STATE_ENCRYPTION_H

#include "appliance_key_custody.h"
#include "appliance_state_crypto.h"
#include "appliance_state_layout.h"
#include "block_device.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * ITEM-373 sealed state volume.
 *
 * The state partition keeps its layout: seven fixed stores followed by one
 * unencrypted custody block at the last partition block.  Every store is
 * sealed per 512-byte block with AES-256-CTR and an HMAC-SHA-256 tag whose
 * first 12 bytes plus a 32-bit write counter form a 16-byte tag entry.  Tag
 * entries live in tag blocks appended to the store region, so a sealed store
 * with N physical blocks exposes N - ceil(N / 33) data blocks.
 *
 * A tag failure is fail-closed: the ciphertext is authenticated before any
 * plaintext byte is produced, a wrong key never yields plaintext, and a tampered
 * block reads as an I/O error instead of a fabricated value.
 */

#if defined(HFOC_PRODUCTION_STATE_ENCRYPTION) || \
    defined(HFOC_STATE_ENCRYPTION_HOST_TEST)
#define APPLIANCE_STATE_ENCRYPTION_ENABLED 1
#endif

#define APPLIANCE_STATE_ENCRYPTION_VERSION 1U
#define APPLIANCE_STATE_ENCRYPTION_BLOCK_SIZE 512U
#define APPLIANCE_STATE_ENCRYPTION_TAG_ENTRY_LENGTH 16U
#define APPLIANCE_STATE_ENCRYPTION_TAG_MAC_LENGTH 12U
#define APPLIANCE_STATE_ENCRYPTION_TAG_BLOCK_ENTRIES 32U
#define APPLIANCE_STATE_ENCRYPTION_MAX_DATA_BLOCKS 256U
#define APPLIANCE_STATE_ENCRYPTION_TAG_CACHE_LENGTH \
    (APPLIANCE_STATE_ENCRYPTION_MAX_DATA_BLOCKS * \
     APPLIANCE_STATE_ENCRYPTION_TAG_ENTRY_LENGTH)
#define APPLIANCE_STATE_ENCRYPTION_MIN_STORE_BLOCKS 3U
#define APPLIANCE_STATE_ENCRYPTION_RECOVERY_TEXT_LENGTH \
    APPLIANCE_KEY_CUSTODY_RECOVERY_TEXT_LENGTH

enum appliance_state_encryption_status {
    APPLIANCE_STATE_ENCRYPTION_OK = 0,
    APPLIANCE_STATE_ENCRYPTION_NULL = 1,
    APPLIANCE_STATE_ENCRYPTION_BAD_DEVICE = 2,
    APPLIANCE_STATE_ENCRYPTION_BAD_LAYOUT = 3,
    APPLIANCE_STATE_ENCRYPTION_BAD_CUSTODY = 4,
    APPLIANCE_STATE_ENCRYPTION_NOT_ENCRYPTED = 5,
    APPLIANCE_STATE_ENCRYPTION_KEY_REQUIRED = 6,
    APPLIANCE_STATE_ENCRYPTION_WRONG_KEY = 7,
    APPLIANCE_STATE_ENCRYPTION_TPM_UNAVAILABLE = 8,
    APPLIANCE_STATE_ENCRYPTION_CORRUPT = 9,
    APPLIANCE_STATE_ENCRYPTION_IO_ERROR = 10,
    APPLIANCE_STATE_ENCRYPTION_AUTH_FAILED = 11,
    APPLIANCE_STATE_ENCRYPTION_TOO_SMALL = 12,
    APPLIANCE_STATE_ENCRYPTION_RANDOM_FAILED = 13,
    APPLIANCE_STATE_ENCRYPTION_UNSUPPORTED = 14,
    APPLIANCE_STATE_ENCRYPTION_COUNTER_EXHAUSTED = 15,
    APPLIANCE_STATE_ENCRYPTION_BAD_RANGE = 16,
};

struct appliance_state_encryption_store {
    bool initialized;
    struct block_device parent;
    struct block_device device;
    enum appliance_persistent_storage_namespace_id namespace_id;
    uint32_t store_id;
    uint32_t physical_block_count;
    uint32_t data_block_count;
    uint32_t tag_block_count;
    uint8_t enc_key[APPLIANCE_STATE_CRYPTO_KEY_LENGTH];
    uint8_t mac_key[APPLIANCE_STATE_CRYPTO_KEY_LENGTH];
    uint8_t volume_id[APPLIANCE_KEY_CUSTODY_VOLUME_ID_LENGTH];
    uint8_t tag_cache[APPLIANCE_STATE_ENCRYPTION_TAG_CACHE_LENGTH];
    bool tag_cache_valid;
    bool dirty;
};

struct appliance_state_encryption_volume {
    bool initialized;
    bool sealed;
    bool unlocked;
    struct appliance_key_custody custody;
    struct appliance_key_custody_record record;
};

struct appliance_state_encryption_install_result {
    uint8_t dek_fingerprint[APPLIANCE_KEY_CUSTODY_FINGERPRINT_LENGTH];
    uint8_t recovery_fingerprint[
        APPLIANCE_KEY_CUSTODY_FINGERPRINT_LENGTH];
    char recovery_text[
        APPLIANCE_STATE_ENCRYPTION_RECOVERY_TEXT_LENGTH + 1U];
    uint32_t sealed_store_count;
};

enum appliance_state_encryption_status
appliance_state_encryption_volume_probe(
    struct appliance_state_encryption_volume *volume,
    struct block_device *partition_device);
enum appliance_state_encryption_status
appliance_state_encryption_volume_unlock(
    struct appliance_state_encryption_volume *volume,
    const struct appliance_key_custody_material *material);
enum appliance_state_encryption_status
appliance_state_encryption_store_bind(
    const struct appliance_state_encryption_volume *volume,
    enum appliance_persistent_storage_namespace_id namespace_id,
    uint32_t store_id,
    struct block_device *store_range,
    struct appliance_state_encryption_store *store);
enum appliance_state_encryption_status
appliance_state_encryption_install(
    struct block_device *partition_device,
    const struct appliance_state_layout *layout,
    const struct appliance_key_custody_material *passphrase,
    appliance_state_crypto_random_fn random,
    void *random_context,
    struct appliance_state_encryption_install_result *result);
bool appliance_state_encryption_volume_sealed(
    const struct appliance_state_encryption_volume *volume);
bool appliance_state_encryption_volume_unlocked(
    const struct appliance_state_encryption_volume *volume);
const struct block_device *appliance_state_encryption_store_device(
    const struct appliance_state_encryption_store *store);
const char *appliance_state_encryption_status_string(
    enum appliance_state_encryption_status status);

#endif
