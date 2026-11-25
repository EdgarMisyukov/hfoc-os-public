#include "appliance_host_seed.h"

#include "appliance_state_crypto.h"
#include "sha256.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define APPLIANCE_HOST_SEED_MEDIA_MAGIC "HFOCSED1"
#define APPLIANCE_HOST_SEED_STATE_MAGIC "HFOCES01"
#define APPLIANCE_HOST_SEED_STATE_FLAG_REKEY 1U
#define APPLIANCE_HOST_SEED_MEDIA_DIGEST_OFFSET 88U
#define APPLIANCE_HOST_SEED_STATE_CIPHERTEXT_OFFSET 96U
#define APPLIANCE_HOST_SEED_STATE_TAG_OFFSET 128U

static const uint8_t appliance_host_seed_media_magic[8U] = {
    'H', 'F', 'O', 'C', 'S', 'E', 'D', '1'
};
static const uint8_t appliance_host_seed_state_magic[8U] = {
    'H', 'F', 'O', 'C', 'E', 'S', '0', '1'
};

/* ITEM-611: counters and caller-supplied cycle clock for the login trace. */
static appliance_host_seed_clock_fn accounting_clock;
static struct appliance_host_seed_accounting accounting;

static uint64_t accounting_now(void)
{
    return accounting_clock != NULL ? accounting_clock() : 0U;
}

static void accounting_add(uint64_t *field, uint64_t started)
{
    if (accounting_clock != NULL) {
        *field += accounting_now() - started;
    }
}

void appliance_host_seed_accounting_begin(appliance_host_seed_clock_fn clock)
{
    memset(&accounting, 0, sizeof(accounting));
    accounting_clock = clock;
}

void appliance_host_seed_accounting_read(
    struct appliance_host_seed_accounting *accounting_out)
{
    if (accounting_out != NULL) {
        *accounting_out = accounting;
    }
}

void appliance_host_seed_accounting_end(void)
{
    accounting_clock = NULL;
    memset(&accounting, 0, sizeof(accounting));
}

static void appliance_host_seed_store_le32(uint8_t *bytes, uint32_t value)
{
    bytes[0] = (uint8_t)value;
    bytes[1] = (uint8_t)(value >> 8U);
    bytes[2] = (uint8_t)(value >> 16U);
    bytes[3] = (uint8_t)(value >> 24U);
}

static uint32_t appliance_host_seed_load_le32(const uint8_t *bytes)
{
    return (uint32_t)bytes[0] |
           ((uint32_t)bytes[1] << 8U) |
           ((uint32_t)bytes[2] << 16U) |
           ((uint32_t)bytes[3] << 24U);
}

static void appliance_host_seed_store_le64(uint8_t *bytes, uint64_t value)
{
    size_t index;

    for (index = 0U; index < 8U; ++index) {
        bytes[index] = (uint8_t)(value >> (8U * index));
    }
}

static uint64_t appliance_host_seed_load_le64(const uint8_t *bytes)
{
    uint64_t value = 0U;
    size_t index;

    for (index = 0U; index < 8U; ++index) {
        value |= (uint64_t)bytes[index] << (8U * index);
    }
    return value;
}

static bool appliance_host_seed_slot_marks_rekey(const uint8_t *bytes)
{
    return appliance_host_seed_load_le32(bytes + 28U) ==
           APPLIANCE_HOST_SEED_STATE_FLAG_REKEY;
}

static void appliance_host_seed_zeroize(void *buffer, size_t length)
{
    appliance_state_crypto_zeroize(buffer, length);
}

static bool appliance_host_seed_zero_range(
    const uint8_t *bytes,
    size_t offset,
    size_t length)
{
    uint8_t difference = 0U;
    size_t index;

    for (index = 0U; index < length; ++index) {
        difference |= bytes[offset + index];
    }
    return difference == 0U;
}

static bool appliance_host_seed_target_character(char value)
{
    return (value >= 'A' && value <= 'Z') ||
           (value >= 'a' && value <= 'z') ||
           (value >= '0' && value <= '9') ||
           value == '-' || value == '_' || value == '.' || value == ':';
}

static bool appliance_host_seed_target_valid(const char *target)
{
    size_t length = 0U;

    if (target == NULL) {
        return false;
    }
    while (target[length] != '\0') {
        if (length >= APPLIANCE_HOST_SEED_TARGET_LENGTH ||
            !appliance_host_seed_target_character(target[length])) {
            return false;
        }
        ++length;
    }
    return length != 0U;
}

static bool appliance_host_seed_material_valid(const uint8_t *seed)
{
    uint8_t seen[256];
    size_t index;
    size_t distinct = 0U;
    bool repeated_halves = true;

    if (seed == NULL) {
        return false;
    }
    memset(seen, 0, sizeof(seen));
    for (index = 0U; index < APPLIANCE_HOST_SEED_LENGTH; ++index) {
        if (seen[seed[index]] == 0U) {
            seen[seed[index]] = 1U;
            distinct += 1U;
        }
        if (index >= APPLIANCE_HOST_SEED_LENGTH / 2U &&
            seed[index] != seed[index - APPLIANCE_HOST_SEED_LENGTH / 2U]) {
            repeated_halves = false;
        }
    }
    appliance_host_seed_zeroize(seen, sizeof(seen));
    return distinct >= 16U && !repeated_halves;
}

enum appliance_host_seed_status appliance_host_seed_media_parse(
    const uint8_t *bytes,
    size_t length,
    struct appliance_host_seed_media_record *record)
{
    uint8_t digest[SHA256_DIGEST_SIZE];
    size_t target_length = 0U;

    if (bytes == NULL || record == NULL) {
        return APPLIANCE_HOST_SEED_NULL;
    }
    memset(record, 0, sizeof(*record));
    if (length != APPLIANCE_HOST_SEED_MEDIA_RECORD_LENGTH) {
        return APPLIANCE_HOST_SEED_BAD_LENGTH;
    }
    if (memcmp(bytes, appliance_host_seed_media_magic,
               sizeof(appliance_host_seed_media_magic)) != 0) {
        return APPLIANCE_HOST_SEED_BAD_MAGIC;
    }
    record->version = appliance_host_seed_load_le32(bytes + 8U);
    record->domain = appliance_host_seed_load_le32(bytes + 12U);
    record->build_id = appliance_host_seed_load_le32(bytes + 16U);
    if (record->version != APPLIANCE_HOST_SEED_VERSION) {
        return APPLIANCE_HOST_SEED_BAD_VERSION;
    }
    if (record->domain != APPLIANCE_HOST_SEED_DOMAIN_INSTALL) {
        return APPLIANCE_HOST_SEED_BAD_DOMAIN;
    }
    if (record->build_id == 0U) {
        return APPLIANCE_HOST_SEED_BAD_BUILD;
    }
    if (!appliance_host_seed_zero_range(bytes, 20U, 4U)) {
        return APPLIANCE_HOST_SEED_BAD_RESERVED;
    }
    while (target_length < APPLIANCE_HOST_SEED_TARGET_LENGTH &&
           bytes[24U + target_length] != 0U) {
        const char value = (char)bytes[24U + target_length];

        if (!appliance_host_seed_target_character(value)) {
            return APPLIANCE_HOST_SEED_BAD_TARGET;
        }
        record->target[target_length] = value;
        ++target_length;
    }
    if (target_length == 0U ||
        !appliance_host_seed_zero_range(
            bytes, 24U + target_length,
            APPLIANCE_HOST_SEED_TARGET_LENGTH - target_length)) {
        return APPLIANCE_HOST_SEED_BAD_TARGET;
    }
    record->target[target_length] = '\0';
    memcpy(record->seed, bytes + 56U, sizeof(record->seed));
    if (!appliance_host_seed_material_valid(record->seed)) {
        appliance_host_seed_clear_media(record);
        return APPLIANCE_HOST_SEED_LOW_DIVERSITY;
    }
    memset(digest, 0, sizeof(digest));
    if (sha256_compute(
            bytes, APPLIANCE_HOST_SEED_MEDIA_DIGEST_OFFSET,
            digest) != SHA256_OK) {
        appliance_host_seed_clear_media(record);
        return APPLIANCE_HOST_SEED_UNSUPPORTED;
    }
    if (!appliance_state_crypto_equal(
            digest, bytes + APPLIANCE_HOST_SEED_MEDIA_DIGEST_OFFSET,
            sizeof(digest))) {
        appliance_host_seed_zeroize(digest, sizeof(digest));
        appliance_host_seed_clear_media(record);
        return APPLIANCE_HOST_SEED_BAD_INTEGRITY;
    }
    appliance_host_seed_zeroize(digest, sizeof(digest));
    return APPLIANCE_HOST_SEED_OK;
}

enum appliance_host_seed_status appliance_host_seed_media_build(
    const struct appliance_host_seed_media_record *record,
    uint8_t bytes[APPLIANCE_HOST_SEED_MEDIA_RECORD_LENGTH])
{
    size_t target_length;

    if (record == NULL || bytes == NULL) {
        return APPLIANCE_HOST_SEED_NULL;
    }
    target_length = strlen(record->target);
    if (record->version != APPLIANCE_HOST_SEED_VERSION) {
        return APPLIANCE_HOST_SEED_BAD_VERSION;
    }
    if (record->domain != APPLIANCE_HOST_SEED_DOMAIN_INSTALL) {
        return APPLIANCE_HOST_SEED_BAD_DOMAIN;
    }
    if (record->build_id == 0U) {
        return APPLIANCE_HOST_SEED_BAD_BUILD;
    }
    if (!appliance_host_seed_target_valid(record->target)) {
        return APPLIANCE_HOST_SEED_BAD_TARGET;
    }
    if (!appliance_host_seed_material_valid(record->seed)) {
        return APPLIANCE_HOST_SEED_LOW_DIVERSITY;
    }
    memset(bytes, 0, APPLIANCE_HOST_SEED_MEDIA_RECORD_LENGTH);
    memcpy(bytes, appliance_host_seed_media_magic,
           sizeof(appliance_host_seed_media_magic));
    appliance_host_seed_store_le32(bytes + 8U, record->version);
    appliance_host_seed_store_le32(bytes + 12U, record->domain);
    appliance_host_seed_store_le32(bytes + 16U, record->build_id);
    memcpy(bytes + 24U, record->target, target_length);
    memcpy(bytes + 56U, record->seed, APPLIANCE_HOST_SEED_LENGTH);
    if (sha256_compute(
            bytes, APPLIANCE_HOST_SEED_MEDIA_DIGEST_OFFSET,
            bytes + APPLIANCE_HOST_SEED_MEDIA_DIGEST_OFFSET) != SHA256_OK) {
        memset(bytes, 0, APPLIANCE_HOST_SEED_MEDIA_RECORD_LENGTH);
        return APPLIANCE_HOST_SEED_UNSUPPORTED;
    }
    return APPLIANCE_HOST_SEED_OK;
}

enum appliance_host_seed_status appliance_host_seed_media_match(
    const struct appliance_host_seed_media_record *record,
    const char *target,
    uint32_t build_id)
{
    if (record == NULL || target == NULL) {
        return APPLIANCE_HOST_SEED_NULL;
    }
    if (!appliance_host_seed_target_valid(target) ||
        !appliance_host_seed_target_valid(record->target) ||
        strcmp(record->target, target) != 0) {
        return APPLIANCE_HOST_SEED_BAD_TARGET;
    }
    if (build_id == 0U || record->build_id != build_id) {
        return APPLIANCE_HOST_SEED_BAD_BUILD;
    }
    return APPLIANCE_HOST_SEED_OK;
}

static enum appliance_host_seed_status appliance_host_seed_slot_derive(
    const uint8_t seed[APPLIANCE_HOST_SEED_LENGTH],
    uint64_t sequence,
    uint8_t domain,
    uint8_t output[16U])
{
    static const char label[] = "HFOC-HOST-SEED-SLOT-V1";
    struct sha256 hash;
    uint8_t digest[SHA256_DIGEST_SIZE];
    uint8_t sequence_bytes[8U];

    if (seed == NULL || output == NULL) {
        return APPLIANCE_HOST_SEED_NULL;
    }
    memset(&hash, 0, sizeof(hash));
    memset(digest, 0, sizeof(digest));
    memset(sequence_bytes, 0, sizeof(sequence_bytes));
    appliance_host_seed_store_le64(sequence_bytes, sequence);
    if (sha256_init(&hash) != SHA256_OK ||
        sha256_update(&hash, label, sizeof(label) - 1U) != SHA256_OK ||
        sha256_update(&hash, &domain, sizeof(domain)) != SHA256_OK ||
        sha256_update(&hash, seed, APPLIANCE_HOST_SEED_LENGTH) != SHA256_OK ||
        sha256_update(&hash, sequence_bytes,
                      sizeof(sequence_bytes)) != SHA256_OK ||
        sha256_final(&hash, digest) != SHA256_OK) {
        appliance_host_seed_zeroize(&hash, sizeof(hash));
        appliance_host_seed_zeroize(digest, sizeof(digest));
        appliance_host_seed_zeroize(sequence_bytes, sizeof(sequence_bytes));
        return APPLIANCE_HOST_SEED_UNSUPPORTED;
    }
    memcpy(output, digest, 16U);
    appliance_host_seed_zeroize(&hash, sizeof(hash));
    appliance_host_seed_zeroize(digest, sizeof(digest));
    appliance_host_seed_zeroize(sequence_bytes, sizeof(sequence_bytes));
    return APPLIANCE_HOST_SEED_OK;
}

static enum appliance_host_seed_status appliance_host_seed_slot_seal(
    const struct appliance_host_seed_media_record *record,
    uint64_t sequence,
    bool rekey,
    const char *password,
    size_t password_length,
    uint8_t bytes[APPLIANCE_HOST_SEED_STATE_SLOT_LENGTH])
{
    uint8_t keys[64U];
    uint8_t mac[SHA256_DIGEST_SIZE];
    enum appliance_state_crypto_status crypto_status;
    enum appliance_host_seed_status status;
    const uint64_t started = accounting_now();

    if (record == NULL || password == NULL || bytes == NULL) {
        return APPLIANCE_HOST_SEED_NULL;
    }
    ++accounting.seals;
    ++accounting.kdf_calls;
    accounting.kdf_iterations +=
        APPLIANCE_STATE_CRYPTO_PBKDF2_DEFAULT_ITERATIONS;
    memset(keys, 0, sizeof(keys));
    memset(mac, 0, sizeof(mac));
    memset(bytes, 0, APPLIANCE_HOST_SEED_STATE_SLOT_LENGTH);
    memcpy(bytes, appliance_host_seed_state_magic,
           sizeof(appliance_host_seed_state_magic));
    appliance_host_seed_store_le32(bytes + 8U, APPLIANCE_HOST_SEED_VERSION);
    appliance_host_seed_store_le32(
        bytes + 12U, APPLIANCE_STATE_CRYPTO_PBKDF2_DEFAULT_ITERATIONS);
    appliance_host_seed_store_le64(bytes + 16U, sequence);
    appliance_host_seed_store_le32(bytes + 24U, record->build_id);
    appliance_host_seed_store_le32(
        bytes + 28U,
        rekey ? APPLIANCE_HOST_SEED_STATE_FLAG_REKEY : 0U);
    memcpy(bytes + 32U, record->target, sizeof(record->target));
    status = appliance_host_seed_slot_derive(
        record->seed, sequence, 1U, bytes + 64U);
    if (status == APPLIANCE_HOST_SEED_OK) {
        status = appliance_host_seed_slot_derive(
            record->seed, sequence, 2U, bytes + 80U);
    }
    if (status != APPLIANCE_HOST_SEED_OK) {
        goto fail;
    }
    crypto_status = appliance_state_crypto_pbkdf2_sha256(
        (const uint8_t *)password, password_length, bytes + 64U,
        APPLIANCE_STATE_CRYPTO_PBKDF2_DEFAULT_ITERATIONS,
        keys, sizeof(keys));
    if (crypto_status != APPLIANCE_STATE_CRYPTO_OK) {
        status = APPLIANCE_HOST_SEED_RANDOM_FAILED;
        goto fail;
    }
    crypto_status = appliance_state_crypto_ctr(
        keys, bytes + 80U, record->seed,
        bytes + APPLIANCE_HOST_SEED_STATE_CIPHERTEXT_OFFSET,
        APPLIANCE_HOST_SEED_LENGTH);
    if (crypto_status != APPLIANCE_STATE_CRYPTO_OK) {
        status = APPLIANCE_HOST_SEED_UNSUPPORTED;
        goto fail;
    }
    crypto_status = appliance_state_crypto_hmac_sha256(
        keys + 32U, 32U, bytes,
        APPLIANCE_HOST_SEED_STATE_CIPHERTEXT_OFFSET +
            APPLIANCE_HOST_SEED_LENGTH,
        mac);
    if (crypto_status != APPLIANCE_STATE_CRYPTO_OK) {
        status = APPLIANCE_HOST_SEED_UNSUPPORTED;
        goto fail;
    }
    memcpy(bytes + APPLIANCE_HOST_SEED_STATE_TAG_OFFSET, mac, sizeof(mac));
    appliance_host_seed_zeroize(keys, sizeof(keys));
    appliance_host_seed_zeroize(mac, sizeof(mac));
    accounting_add(&accounting.seal_cycles, started);
    return APPLIANCE_HOST_SEED_OK;

fail:
    accounting_add(&accounting.seal_cycles, started);
    appliance_host_seed_zeroize(keys, sizeof(keys));
    appliance_host_seed_zeroize(mac, sizeof(mac));
    memset(bytes, 0, APPLIANCE_HOST_SEED_STATE_SLOT_LENGTH);
    return status;
}

static enum appliance_host_seed_status appliance_host_seed_slot_open(
    const uint8_t bytes[APPLIANCE_HOST_SEED_STATE_SLOT_LENGTH],
    const char *password,
    size_t password_length,
    struct appliance_host_seed_media_record *record,
    uint64_t *sequence_out)
{
    uint8_t keys[64U];
    uint8_t mac[SHA256_DIGEST_SIZE];
    enum appliance_state_crypto_status crypto_status;
    uint32_t iterations;
    uint8_t target_length = 0U;

    if (bytes == NULL || password == NULL || record == NULL ||
        sequence_out == NULL) {
        return APPLIANCE_HOST_SEED_NULL;
    }
    memset(record, 0, sizeof(*record));
    memset(keys, 0, sizeof(keys));
    memset(mac, 0, sizeof(mac));
    if (memcmp(bytes, appliance_host_seed_state_magic,
               sizeof(appliance_host_seed_state_magic)) != 0) {
        return APPLIANCE_HOST_SEED_MISSING;
    }
    if (appliance_host_seed_load_le32(bytes + 8U) !=
        APPLIANCE_HOST_SEED_VERSION) {
        return APPLIANCE_HOST_SEED_BAD_VERSION;
    }
    iterations = appliance_host_seed_load_le32(bytes + 12U);
    if (iterations < APPLIANCE_STATE_CRYPTO_PBKDF2_MIN_ITERATIONS ||
        iterations > APPLIANCE_STATE_CRYPTO_PBKDF2_MAX_ITERATIONS) {
        return APPLIANCE_HOST_SEED_BAD_VERSION;
    }
    *sequence_out = appliance_host_seed_load_le64(bytes + 16U);
    if (*sequence_out == 0U) {
        return APPLIANCE_HOST_SEED_CORRUPT;
    }
    record->version = APPLIANCE_HOST_SEED_VERSION;
    record->domain = APPLIANCE_HOST_SEED_DOMAIN_INSTALL;
    record->build_id = appliance_host_seed_load_le32(bytes + 24U);
    if (record->build_id == 0U ||
        (appliance_host_seed_load_le32(bytes + 28U) != 0U &&
         !appliance_host_seed_slot_marks_rekey(bytes))) {
        return APPLIANCE_HOST_SEED_CORRUPT;
    }
    while (target_length < APPLIANCE_HOST_SEED_TARGET_LENGTH &&
           bytes[32U + target_length] != 0U) {
        record->target[target_length] =
            (char)bytes[32U + target_length];
        ++target_length;
    }
    if (!appliance_host_seed_target_valid(record->target) ||
        !appliance_host_seed_zero_range(
            bytes, 32U + target_length,
            APPLIANCE_HOST_SEED_TARGET_LENGTH - target_length) ||
        !appliance_host_seed_zero_range(
            bytes,
            APPLIANCE_HOST_SEED_STATE_TAG_OFFSET + SHA256_DIGEST_SIZE,
            APPLIANCE_HOST_SEED_STATE_SLOT_LENGTH -
                APPLIANCE_HOST_SEED_STATE_TAG_OFFSET -
                SHA256_DIGEST_SIZE)) {
        appliance_host_seed_clear_media(record);
        return APPLIANCE_HOST_SEED_CORRUPT;
    }
    {
        const uint64_t started = accounting_now();

        ++accounting.opens;
        ++accounting.kdf_calls;
        accounting.kdf_iterations += iterations;
        crypto_status = appliance_state_crypto_pbkdf2_sha256(
            (const uint8_t *)password, password_length, bytes + 64U,
            iterations, keys, sizeof(keys));
        accounting_add(&accounting.open_cycles, started);
    }
    if (crypto_status != APPLIANCE_STATE_CRYPTO_OK) {
        return APPLIANCE_HOST_SEED_RANDOM_FAILED;
    }
    crypto_status = appliance_state_crypto_hmac_sha256(
        keys + 32U, 32U, bytes,
        APPLIANCE_HOST_SEED_STATE_CIPHERTEXT_OFFSET +
            APPLIANCE_HOST_SEED_LENGTH,
        mac);
    if (crypto_status != APPLIANCE_STATE_CRYPTO_OK) {
        appliance_host_seed_zeroize(keys, sizeof(keys));
        return APPLIANCE_HOST_SEED_UNSUPPORTED;
    }
    if (!appliance_state_crypto_equal(
            mac, bytes + APPLIANCE_HOST_SEED_STATE_TAG_OFFSET,
            sizeof(mac))) {
        appliance_host_seed_zeroize(keys, sizeof(keys));
        appliance_host_seed_zeroize(mac, sizeof(mac));
        return APPLIANCE_HOST_SEED_WRONG_PASSWORD;
    }
    crypto_status = appliance_state_crypto_ctr(
        keys, bytes + 80U,
        bytes + APPLIANCE_HOST_SEED_STATE_CIPHERTEXT_OFFSET,
        record->seed, APPLIANCE_HOST_SEED_LENGTH);
    appliance_host_seed_zeroize(keys, sizeof(keys));
    appliance_host_seed_zeroize(mac, sizeof(mac));
    if (crypto_status != APPLIANCE_STATE_CRYPTO_OK ||
        !appliance_host_seed_material_valid(record->seed)) {
        appliance_host_seed_clear_media(record);
        return APPLIANCE_HOST_SEED_CORRUPT;
    }
    return APPLIANCE_HOST_SEED_OK;
}

static enum appliance_host_seed_status appliance_host_seed_store_bind(
    struct block_device *partition_device,
    uint64_t layout_capacity,
    uint32_t slot,
    struct block_device *store_out,
    uint64_t *offset_out)
{
    uint64_t offset;

    if (partition_device == NULL || store_out == NULL || offset_out == NULL) {
        return APPLIANCE_HOST_SEED_NULL;
    }
    if (block_device_validate(partition_device) != BLOCK_DEVICE_OK ||
        partition_device->block_size != APPLIANCE_HOST_SEED_STATE_SLOT_LENGTH) {
        return APPLIANCE_HOST_SEED_BAD_DEVICE;
    }
    if (layout_capacity == 0U ||
        layout_capacity >
            partition_device->block_count -
                APPLIANCE_HOST_SEED_STATE_SLOT_COUNT ||
        slot >= APPLIANCE_HOST_SEED_STATE_SLOT_COUNT ||
        partition_device->block_count <
            APPLIANCE_HOST_SEED_STATE_SLOT_COUNT) {
        return APPLIANCE_HOST_SEED_BAD_LAYOUT;
    }
    offset = (layout_capacity + slot) * partition_device->block_size;
    *store_out = *partition_device;
    *offset_out = offset;
    return APPLIANCE_HOST_SEED_OK;
}

static enum appliance_host_seed_status appliance_host_seed_slot_read(
    struct block_device *partition_device,
    uint64_t layout_capacity,
    uint32_t slot,
    uint8_t bytes[APPLIANCE_HOST_SEED_STATE_SLOT_LENGTH])
{
    struct block_device store;
    uint64_t offset = 0U;
    const uint64_t started = accounting_now();
    enum appliance_host_seed_status status =
        appliance_host_seed_store_bind(
            partition_device, layout_capacity, slot, &store, &offset);

    ++accounting.slot_reads;
    if (status != APPLIANCE_HOST_SEED_OK) {
        accounting_add(&accounting.read_cycles, started);
        return status;
    }
    status = block_device_read(
                 &store, offset, bytes,
                 APPLIANCE_HOST_SEED_STATE_SLOT_LENGTH) == BLOCK_DEVICE_OK
        ? APPLIANCE_HOST_SEED_OK
        : APPLIANCE_HOST_SEED_IO_ERROR;
    accounting_add(&accounting.read_cycles, started);
    return status;
}

static enum appliance_host_seed_status appliance_host_seed_slot_write(
    struct block_device *partition_device,
    uint64_t layout_capacity,
    uint32_t slot,
    const uint8_t bytes[APPLIANCE_HOST_SEED_STATE_SLOT_LENGTH])
{
    struct block_device store;
    uint64_t offset = 0U;
    const uint64_t started = accounting_now();
    enum appliance_host_seed_status status =
        appliance_host_seed_store_bind(
            partition_device, layout_capacity, slot, &store, &offset);

    ++accounting.slot_writes;
    if (status != APPLIANCE_HOST_SEED_OK) {
        accounting_add(&accounting.write_cycles, started);
        return status;
    }
    if (block_device_write(
            &store, offset, bytes,
            APPLIANCE_HOST_SEED_STATE_SLOT_LENGTH) != BLOCK_DEVICE_OK ||
        block_device_flush(&store) != BLOCK_DEVICE_OK) {
        accounting_add(&accounting.write_cycles, started);
        return APPLIANCE_HOST_SEED_IO_ERROR;
    }
    accounting_add(&accounting.write_cycles, started);
    return APPLIANCE_HOST_SEED_OK;
}

enum appliance_host_seed_status appliance_host_seed_store_provision(
    struct block_device *partition_device,
    uint64_t layout_capacity,
    const struct appliance_host_seed_media_record *record,
    const char *password,
    size_t password_length)
{
    uint8_t cleared[APPLIANCE_HOST_SEED_STATE_SLOT_LENGTH];
    uint8_t slot0[APPLIANCE_HOST_SEED_STATE_SLOT_LENGTH];
    uint8_t readback[APPLIANCE_HOST_SEED_STATE_SLOT_LENGTH];
    struct appliance_host_seed_media_record opened;
    uint64_t sequence = 0U;
    enum appliance_host_seed_status status = APPLIANCE_HOST_SEED_OK;

    if (record == NULL || password == NULL) {
        return APPLIANCE_HOST_SEED_NULL;
    }
    memset(cleared, 0, sizeof(cleared));
    memset(slot0, 0, sizeof(slot0));
    memset(readback, 0, sizeof(readback));
    memset(&opened, 0, sizeof(opened));
    status = appliance_host_seed_slot_seal(
        record, 1U, false, password, password_length, slot0);
    if (status != APPLIANCE_HOST_SEED_OK) {
        goto finish;
    }
    /*
     * Explicit provisioning replaces any prior two-slot state.  Clear the
     * companion first, then publish sequence 1 in slot 0.  A power loss before
     * the new slot lands is retryable as MISSING; a completed write can never
     * pair the new seed with an older, higher-sequence companion.
     */
    status = appliance_host_seed_slot_write(
        partition_device, layout_capacity, 1U, cleared);
    if (status != APPLIANCE_HOST_SEED_OK) {
        goto finish;
    }
    status = appliance_host_seed_slot_write(
        partition_device, layout_capacity, 0U, slot0);
    if (status != APPLIANCE_HOST_SEED_OK) {
        goto finish;
    }
    memset(readback, 0, sizeof(readback));
    status = appliance_host_seed_slot_read(
        partition_device, layout_capacity, 0U, readback);
    if (status != APPLIANCE_HOST_SEED_OK) {
        goto finish;
    }
    status = appliance_host_seed_slot_open(
        readback, password, password_length, &opened, &sequence);
    if (status == APPLIANCE_HOST_SEED_OK &&
        (sequence != 1U ||
         memcmp(opened.seed, record->seed, sizeof(record->seed)) != 0 ||
         strcmp(opened.target, record->target) != 0 ||
         opened.build_id != record->build_id)) {
        status = APPLIANCE_HOST_SEED_CORRUPT;
        goto finish;
    }
    memset(readback, 0, sizeof(readback));
    status = appliance_host_seed_slot_read(
        partition_device, layout_capacity, 1U, readback);
    if (status == APPLIANCE_HOST_SEED_OK &&
        !appliance_host_seed_zero_range(
            readback, 0U, sizeof(readback))) {
        status = APPLIANCE_HOST_SEED_CORRUPT;
    }

finish:
    appliance_host_seed_clear_media(&opened);
    appliance_host_seed_zeroize(cleared, sizeof(cleared));
    appliance_host_seed_zeroize(slot0, sizeof(slot0));
    appliance_host_seed_zeroize(readback, sizeof(readback));
    return status;
}

enum appliance_host_seed_status appliance_host_seed_store_unlock(
    struct block_device *partition_device,
    uint64_t layout_capacity,
    const char *password,
    size_t password_length,
    uint8_t seed_out[APPLIANCE_HOST_SEED_LENGTH],
    uint64_t *epoch_out)
{
    uint8_t slots[APPLIANCE_HOST_SEED_STATE_SLOT_COUNT]
                 [APPLIANCE_HOST_SEED_STATE_SLOT_LENGTH];
    uint8_t next[APPLIANCE_HOST_SEED_STATE_SLOT_LENGTH];
    uint8_t readback[APPLIANCE_HOST_SEED_STATE_SLOT_LENGTH];
    struct appliance_host_seed_media_record candidates[
        APPLIANCE_HOST_SEED_STATE_SLOT_COUNT];
    struct appliance_host_seed_media_record verified;
    uint64_t sequences[APPLIANCE_HOST_SEED_STATE_SLOT_COUNT];
    bool wrong_passwords[APPLIANCE_HOST_SEED_STATE_SLOT_COUNT];
    uint32_t selected = 0U;
    uint32_t slot;
    bool present = false;
    enum appliance_host_seed_status status = APPLIANCE_HOST_SEED_MISSING;

    if (partition_device == NULL || password == NULL || seed_out == NULL ||
        epoch_out == NULL) {
        return APPLIANCE_HOST_SEED_NULL;
    }
    memset(slots, 0, sizeof(slots));
    memset(next, 0, sizeof(next));
    memset(readback, 0, sizeof(readback));
    memset(candidates, 0, sizeof(candidates));
    memset(&verified, 0, sizeof(verified));
    memset(sequences, 0, sizeof(sequences));
    memset(wrong_passwords, 0, sizeof(wrong_passwords));
    memset(seed_out, 0, APPLIANCE_HOST_SEED_LENGTH);
    *epoch_out = 0U;

    for (slot = 0U; slot < APPLIANCE_HOST_SEED_STATE_SLOT_COUNT; ++slot) {
        status = appliance_host_seed_slot_read(
            partition_device, layout_capacity, slot, slots[slot]);
        if (status != APPLIANCE_HOST_SEED_OK) {
            goto finish;
        }
        if (appliance_host_seed_zero_range(
                slots[slot], 0U, APPLIANCE_HOST_SEED_STATE_SLOT_LENGTH)) {
            continue;
        }
        present = true;
        status = appliance_host_seed_slot_open(
            slots[slot], password, password_length,
            &candidates[slot], &sequences[slot]);
        if (status == APPLIANCE_HOST_SEED_WRONG_PASSWORD) {
            wrong_passwords[slot] = true;
            continue;
        } else if (status == APPLIANCE_HOST_SEED_OK) {
            if (*epoch_out == 0U || sequences[slot] > *epoch_out) {
                *epoch_out = sequences[slot];
                selected = slot;
            }
        } else {
            /*
             * Once any byte is present, a malformed slot is not treated as an
             * absent copy.  Falling back to the older sequence after a torn
             * write could publish bytes that sequence had already used.
             */
            status = APPLIANCE_HOST_SEED_CORRUPT;
            goto finish;
        }
    }
    if (*epoch_out == 0U) {
        status =
            present ? APPLIANCE_HOST_SEED_WRONG_PASSWORD
                    : APPLIANCE_HOST_SEED_MISSING;
        goto finish;
    }
    for (slot = 0U; slot < APPLIANCE_HOST_SEED_STATE_SLOT_COUNT; ++slot) {
        /*
         * During a password transition one slot may be sealed under a
         * different password already accepted by the account verifier.  An
         * unmarked wrong-password slot is different: if it is at or above the
         * authenticated sequence, it is a torn or substituted advance and
         * cannot be skipped.
         */
        if (wrong_passwords[slot] &&
            !appliance_host_seed_slot_marks_rekey(slots[slot]) &&
            sequences[slot] >= *epoch_out) {
            status = APPLIANCE_HOST_SEED_CORRUPT;
            goto finish;
        }
    }
    /*
     * ITEM-611: the selected slot was already opened and authenticated by the
     * scan above.  Re-running the KDF on the same bytes proved nothing new;
     * reuse the verified record and its sequence.
     */
    verified = candidates[selected];
    *epoch_out = sequences[selected];
    status = APPLIANCE_HOST_SEED_OK;
    if (*epoch_out >= UINT64_MAX - 1U) {
        status = APPLIANCE_HOST_SEED_EPOCH_EXHAUSTED;
        goto finish;
    }
    status = appliance_host_seed_slot_seal(
        &verified, *epoch_out + 1U, false,
        password, password_length, next);
    if (status != APPLIANCE_HOST_SEED_OK) {
        goto finish;
    }
    {
        const uint32_t next_slot =
            (selected + 1U) % APPLIANCE_HOST_SEED_STATE_SLOT_COUNT;

        status = appliance_host_seed_slot_write(
            partition_device, layout_capacity, next_slot, next);
        if (status != APPLIANCE_HOST_SEED_OK) {
            goto finish;
        }
    }
    {
        const uint64_t readback_started = accounting_now();

        ++accounting.readbacks;
        status = appliance_host_seed_slot_read(
            partition_device, layout_capacity,
            (selected + 1U) % APPLIANCE_HOST_SEED_STATE_SLOT_COUNT,
            readback);
        accounting_add(&accounting.readback_cycles, readback_started);
    }
    if (status != APPLIANCE_HOST_SEED_OK ||
        memcmp(readback, next, sizeof(next)) != 0) {
        status = APPLIANCE_HOST_SEED_IO_ERROR;
        goto finish;
    }
    {
        struct appliance_host_seed_media_record reread;
        uint64_t reread_sequence = 0U;

        memset(&reread, 0, sizeof(reread));
        status = appliance_host_seed_slot_open(
            readback, password, password_length, &reread,
            &reread_sequence);
        if (status == APPLIANCE_HOST_SEED_OK &&
            (reread_sequence != *epoch_out + 1U ||
             memcmp(reread.seed, verified.seed,
                    sizeof(verified.seed)) != 0)) {
            status = APPLIANCE_HOST_SEED_CORRUPT;
        }
        appliance_host_seed_clear_media(&reread);
    }
    if (status != APPLIANCE_HOST_SEED_OK) {
        goto finish;
    }
    memcpy(seed_out, verified.seed, APPLIANCE_HOST_SEED_LENGTH);
    *epoch_out += 1U;

finish:
    if (status != APPLIANCE_HOST_SEED_OK) {
        memset(seed_out, 0, APPLIANCE_HOST_SEED_LENGTH);
        *epoch_out = 0U;
    }
    for (slot = 0U; slot < APPLIANCE_HOST_SEED_STATE_SLOT_COUNT; ++slot) {
        appliance_host_seed_clear_media(&candidates[slot]);
    }
    appliance_host_seed_clear_media(&verified);
    appliance_host_seed_zeroize(slots, sizeof(slots));
    appliance_host_seed_zeroize(next, sizeof(next));
    appliance_host_seed_zeroize(readback, sizeof(readback));
    return status;
}

enum appliance_host_seed_status appliance_host_seed_store_rekey(
    struct block_device *partition_device,
    uint64_t layout_capacity,
    const char *current_password,
    size_t current_password_length,
    const char *new_password,
    size_t new_password_length)
{
    uint8_t slots[APPLIANCE_HOST_SEED_STATE_SLOT_COUNT]
                 [APPLIANCE_HOST_SEED_STATE_SLOT_LENGTH];
    uint8_t next[APPLIANCE_HOST_SEED_STATE_SLOT_LENGTH];
    uint8_t readback[APPLIANCE_HOST_SEED_STATE_SLOT_LENGTH];
    struct appliance_host_seed_media_record candidates[
        APPLIANCE_HOST_SEED_STATE_SLOT_COUNT];
    struct appliance_host_seed_media_record verified;
    uint64_t sequences[APPLIANCE_HOST_SEED_STATE_SLOT_COUNT];
    bool wrong_passwords[APPLIANCE_HOST_SEED_STATE_SLOT_COUNT];
    uint64_t selected_sequence = 0U;
    uint32_t selected = 0U;
    uint32_t slot;
    bool present = false;
    enum appliance_host_seed_status status = APPLIANCE_HOST_SEED_MISSING;

    if (partition_device == NULL || current_password == NULL ||
        new_password == NULL) {
        return APPLIANCE_HOST_SEED_NULL;
    }
    if (current_password_length == 0U || new_password_length == 0U) {
        return APPLIANCE_HOST_SEED_WRONG_PASSWORD;
    }
    memset(slots, 0, sizeof(slots));
    memset(next, 0, sizeof(next));
    memset(readback, 0, sizeof(readback));
    memset(candidates, 0, sizeof(candidates));
    memset(&verified, 0, sizeof(verified));
    memset(sequences, 0, sizeof(sequences));
    memset(wrong_passwords, 0, sizeof(wrong_passwords));

    for (slot = 0U; slot < APPLIANCE_HOST_SEED_STATE_SLOT_COUNT; ++slot) {
        status = appliance_host_seed_slot_read(
            partition_device, layout_capacity, slot, slots[slot]);
        if (status != APPLIANCE_HOST_SEED_OK) {
            goto finish;
        }
        if (appliance_host_seed_zero_range(
                slots[slot], 0U, APPLIANCE_HOST_SEED_STATE_SLOT_LENGTH)) {
            continue;
        }
        present = true;
        status = appliance_host_seed_slot_open(
            slots[slot], current_password, current_password_length,
            &candidates[slot], &sequences[slot]);
        if (status == APPLIANCE_HOST_SEED_WRONG_PASSWORD) {
            wrong_passwords[slot] = true;
            continue;
        }
        if (status != APPLIANCE_HOST_SEED_OK) {
            status = APPLIANCE_HOST_SEED_CORRUPT;
            goto finish;
        }
        if (selected_sequence == 0U ||
            sequences[slot] > selected_sequence) {
            selected_sequence = sequences[slot];
            selected = slot;
        }
    }
    if (selected_sequence == 0U) {
        status =
            present ? APPLIANCE_HOST_SEED_WRONG_PASSWORD
                    : APPLIANCE_HOST_SEED_MISSING;
        goto finish;
    }
    for (slot = 0U; slot < APPLIANCE_HOST_SEED_STATE_SLOT_COUNT; ++slot) {
        if (wrong_passwords[slot] &&
            !appliance_host_seed_slot_marks_rekey(slots[slot]) &&
            sequences[slot] >= selected_sequence) {
            status = APPLIANCE_HOST_SEED_CORRUPT;
            goto finish;
        }
    }
    status = appliance_host_seed_slot_open(
        slots[selected], current_password, current_password_length,
        &verified, &selected_sequence);
    if (status != APPLIANCE_HOST_SEED_OK) {
        goto finish;
    }
    if (selected_sequence >= UINT64_MAX - 1U) {
        status = APPLIANCE_HOST_SEED_EPOCH_EXHAUSTED;
        goto finish;
    }
    status = appliance_host_seed_slot_seal(
        &verified, selected_sequence + 1U, true, new_password,
        new_password_length, next);
    if (status != APPLIANCE_HOST_SEED_OK) {
        goto finish;
    }
    status = appliance_host_seed_slot_write(
        partition_device, layout_capacity,
        (selected + 1U) % APPLIANCE_HOST_SEED_STATE_SLOT_COUNT, next);
    if (status != APPLIANCE_HOST_SEED_OK) {
        goto finish;
    }
    status = appliance_host_seed_slot_read(
        partition_device, layout_capacity,
        (selected + 1U) % APPLIANCE_HOST_SEED_STATE_SLOT_COUNT, readback);
    if (status != APPLIANCE_HOST_SEED_OK ||
        memcmp(readback, next, sizeof(next)) != 0) {
        status = APPLIANCE_HOST_SEED_IO_ERROR;
        goto finish;
    }
    {
        struct appliance_host_seed_media_record reread;
        uint64_t reread_sequence = 0U;

        memset(&reread, 0, sizeof(reread));
        status = appliance_host_seed_slot_open(
            readback, new_password, new_password_length, &reread,
            &reread_sequence);
        if (status == APPLIANCE_HOST_SEED_OK &&
            (reread_sequence != selected_sequence + 1U ||
             memcmp(reread.seed, verified.seed,
                    sizeof(verified.seed)) != 0)) {
            status = APPLIANCE_HOST_SEED_CORRUPT;
        }
        appliance_host_seed_clear_media(&reread);
    }

finish:
    for (slot = 0U; slot < APPLIANCE_HOST_SEED_STATE_SLOT_COUNT; ++slot) {
        appliance_host_seed_clear_media(&candidates[slot]);
    }
    appliance_host_seed_clear_media(&verified);
    appliance_host_seed_zeroize(slots, sizeof(slots));
    appliance_host_seed_zeroize(next, sizeof(next));
    appliance_host_seed_zeroize(readback, sizeof(readback));
    return status;
}

enum appliance_host_seed_status appliance_host_seed_store_present(
    struct block_device *partition_device,
    uint64_t layout_capacity,
    bool *present_out)
{
    uint8_t slot[APPLIANCE_HOST_SEED_STATE_SLOT_LENGTH];
    uint32_t index;
    enum appliance_host_seed_status status;

    if (present_out == NULL) {
        return APPLIANCE_HOST_SEED_NULL;
    }
    *present_out = false;
    if (block_device_validate(partition_device) != BLOCK_DEVICE_OK ||
        partition_device->block_size !=
            APPLIANCE_HOST_SEED_STATE_SLOT_LENGTH) {
        return APPLIANCE_HOST_SEED_BAD_DEVICE;
    }
    if (layout_capacity == 0U ||
        layout_capacity > partition_device->block_count) {
        return APPLIANCE_HOST_SEED_BAD_LAYOUT;
    }
    /*
     * A legacy partition can consume the whole device.  Its capacity by
     * definition leaves no room for the host-seed companion slots, so absence
     * of the feature is distinguishable from a damaged existing store.
     */
    if (partition_device->block_count - layout_capacity <
        APPLIANCE_HOST_SEED_STATE_SLOT_COUNT) {
        return APPLIANCE_HOST_SEED_OK;
    }
    memset(slot, 0, sizeof(slot));
    for (index = 0U; index < APPLIANCE_HOST_SEED_STATE_SLOT_COUNT; ++index) {
        status = appliance_host_seed_slot_read(
            partition_device, layout_capacity, index, slot);
        if (status != APPLIANCE_HOST_SEED_OK) {
            appliance_host_seed_zeroize(slot, sizeof(slot));
            return status;
        }
        if (!appliance_host_seed_zero_range(slot, 0U, sizeof(slot))) {
            *present_out = true;
            break;
        }
    }
    appliance_host_seed_zeroize(slot, sizeof(slot));
    return APPLIANCE_HOST_SEED_OK;
}

enum appliance_host_seed_status appliance_host_seed_derive(
    const uint8_t seed[APPLIANCE_HOST_SEED_LENGTH],
    uint64_t epoch,
    uint32_t rotation,
    uint8_t output[APPLIANCE_HOST_SEED_LENGTH])
{
    static const char label[] = "HFOC-HOST-SEED-BOOT-V1";
    struct sha256 hash;
    uint8_t epoch_bytes[8U];
    uint8_t rotation_bytes[4U];

    if (seed == NULL || output == NULL) {
        return APPLIANCE_HOST_SEED_NULL;
    }
    memset(&hash, 0, sizeof(hash));
    memset(output, 0, APPLIANCE_HOST_SEED_LENGTH);
    memset(epoch_bytes, 0, sizeof(epoch_bytes));
    memset(rotation_bytes, 0, sizeof(rotation_bytes));
    appliance_host_seed_store_le64(epoch_bytes, epoch);
    appliance_host_seed_store_le32(rotation_bytes, rotation);
    if (sha256_init(&hash) != SHA256_OK ||
        sha256_update(&hash, label, sizeof(label) - 1U) != SHA256_OK ||
        sha256_update(&hash, seed, APPLIANCE_HOST_SEED_LENGTH) != SHA256_OK ||
        sha256_update(&hash, epoch_bytes, sizeof(epoch_bytes)) != SHA256_OK ||
        sha256_update(&hash, rotation_bytes,
                      sizeof(rotation_bytes)) != SHA256_OK ||
        sha256_final(&hash, output) != SHA256_OK) {
        appliance_host_seed_zeroize(&hash, sizeof(hash));
        appliance_host_seed_zeroize(epoch_bytes, sizeof(epoch_bytes));
        appliance_host_seed_zeroize(rotation_bytes, sizeof(rotation_bytes));
        memset(output, 0, APPLIANCE_HOST_SEED_LENGTH);
        return APPLIANCE_HOST_SEED_UNSUPPORTED;
    }
    appliance_host_seed_zeroize(&hash, sizeof(hash));
    appliance_host_seed_zeroize(epoch_bytes, sizeof(epoch_bytes));
    appliance_host_seed_zeroize(rotation_bytes, sizeof(rotation_bytes));
    return APPLIANCE_HOST_SEED_OK;
}

void appliance_host_seed_clear_media(
    struct appliance_host_seed_media_record *record)
{
    if (record != NULL) {
        appliance_host_seed_zeroize(record, sizeof(*record));
    }
}

const char *appliance_host_seed_status_string(
    enum appliance_host_seed_status status)
{
    switch (status) {
    case APPLIANCE_HOST_SEED_OK:
        return "OK";
    case APPLIANCE_HOST_SEED_NULL:
        return "NULL";
    case APPLIANCE_HOST_SEED_BAD_LENGTH:
        return "BAD_LENGTH";
    case APPLIANCE_HOST_SEED_BAD_MAGIC:
        return "BAD_MAGIC";
    case APPLIANCE_HOST_SEED_BAD_VERSION:
        return "BAD_VERSION";
    case APPLIANCE_HOST_SEED_BAD_DOMAIN:
        return "BAD_DOMAIN";
    case APPLIANCE_HOST_SEED_BAD_RESERVED:
        return "BAD_RESERVED";
    case APPLIANCE_HOST_SEED_BAD_TARGET:
        return "BAD_TARGET";
    case APPLIANCE_HOST_SEED_BAD_BUILD:
        return "BAD_BUILD";
    case APPLIANCE_HOST_SEED_BAD_INTEGRITY:
        return "BAD_INTEGRITY";
    case APPLIANCE_HOST_SEED_LOW_DIVERSITY:
        return "LOW_DIVERSITY";
    case APPLIANCE_HOST_SEED_BAD_DEVICE:
        return "BAD_DEVICE";
    case APPLIANCE_HOST_SEED_BAD_LAYOUT:
        return "BAD_LAYOUT";
    case APPLIANCE_HOST_SEED_MISSING:
        return "MISSING";
    case APPLIANCE_HOST_SEED_CORRUPT:
        return "CORRUPT";
    case APPLIANCE_HOST_SEED_WRONG_PASSWORD:
        return "WRONG_PASSWORD";
    case APPLIANCE_HOST_SEED_EPOCH_EXHAUSTED:
        return "EPOCH_EXHAUSTED";
    case APPLIANCE_HOST_SEED_IO_ERROR:
        return "IO_ERROR";
    case APPLIANCE_HOST_SEED_RANDOM_FAILED:
        return "RANDOM_FAILED";
    case APPLIANCE_HOST_SEED_UNSUPPORTED:
        return "UNSUPPORTED";
    default:
        return "UNKNOWN";
    }
}
