#include "boot_measurements.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

static const char *const boot_measurements_status_names[] = {
    "OK",
    "NULL",
    "BAD_STATE",
    "BAD_INDEX",
    "CAPACITY",
    "DIGEST_FAILED",
};

static void boot_measurements_write_u32(
    unsigned char *buffer,
    uint32_t value)
{
    buffer[0] = (unsigned char)(value & 0xFFU);
    buffer[1] = (unsigned char)((value >> 8U) & 0xFFU);
    buffer[2] = (unsigned char)((value >> 16U) & 0xFFU);
    buffer[3] = (unsigned char)((value >> 24U) & 0xFFU);
}

static void boot_measurements_write_u64(
    unsigned char *buffer,
    uint64_t value)
{
    unsigned int index;

    for (index = 0U; index < 8U; ++index) {
        buffer[index] =
            (unsigned char)((value >> (index * 8U)) & 0xFFU);
    }
}

enum boot_measurements_status boot_measurements_reset(
    struct boot_measurements *measurements)
{
    if (measurements == NULL) {
        return BOOT_MEASUREMENTS_NULL;
    }
    memset(measurements, 0, sizeof(*measurements));
    measurements->version = BOOT_MEASUREMENTS_VERSION;
    measurements->register_count = BOOT_MEASUREMENTS_REGISTER_COUNT;
    return BOOT_MEASUREMENTS_OK;
}

enum boot_measurements_status boot_measurements_fail(
    struct boot_measurements *measurements)
{
    if (measurements == NULL) {
        return BOOT_MEASUREMENTS_NULL;
    }
    if (measurements->version != BOOT_MEASUREMENTS_VERSION) {
        return BOOT_MEASUREMENTS_BAD_STATE;
    }
    measurements->failed = true;
    measurements->published = false;
    measurements->aggregate_published = false;
    return BOOT_MEASUREMENTS_OK;
}

enum boot_measurements_status boot_measurements_extend(
    struct boot_measurements *measurements,
    uint32_t register_index,
    const unsigned char digest[SHA256_DIGEST_SIZE])
{
    struct sha256 hash;
    unsigned char extended[SHA256_DIGEST_SIZE];

    if (measurements == NULL || digest == NULL) {
        return BOOT_MEASUREMENTS_NULL;
    }
    if (measurements->version != BOOT_MEASUREMENTS_VERSION) {
        return BOOT_MEASUREMENTS_BAD_STATE;
    }
    if (measurements->published) {
        return BOOT_MEASUREMENTS_BAD_STATE;
    }
    if (register_index >= BOOT_MEASUREMENTS_REGISTER_COUNT) {
        return BOOT_MEASUREMENTS_BAD_INDEX;
    }

    /* register = SHA-256(register || digest) */
    sha256_init(&hash);
    sha256_update(&hash, measurements->registers[register_index],
                  SHA256_DIGEST_SIZE);
    sha256_update(&hash, digest, SHA256_DIGEST_SIZE);
    if (sha256_final(&hash, extended) != SHA256_OK) {
        (void)boot_measurements_fail(measurements);
        return BOOT_MEASUREMENTS_DIGEST_FAILED;
    }
    memcpy(measurements->registers[register_index], extended,
           SHA256_DIGEST_SIZE);
    return BOOT_MEASUREMENTS_OK;
}

enum boot_measurements_status boot_measurements_record(
    struct boot_measurements *measurements,
    uint32_t register_index,
    uint32_t component_kind,
    uint64_t component_length,
    const unsigned char digest[SHA256_DIGEST_SIZE])
{
    enum boot_measurements_status status;

    if (measurements == NULL || digest == NULL) {
        return BOOT_MEASUREMENTS_NULL;
    }
    if (measurements->version != BOOT_MEASUREMENTS_VERSION) {
        return BOOT_MEASUREMENTS_BAD_STATE;
    }
    if (measurements->published) {
        return BOOT_MEASUREMENTS_BAD_STATE;
    }
    if (measurements->log_count >= BOOT_MEASUREMENTS_LOG_CAPACITY) {
        (void)boot_measurements_fail(measurements);
        return BOOT_MEASUREMENTS_CAPACITY;
    }

    status = boot_measurements_extend(measurements, register_index, digest);
    if (status != BOOT_MEASUREMENTS_OK) {
        (void)boot_measurements_fail(measurements);
        return status;
    }

    measurements->log[measurements->log_count].component_kind =
        component_kind;
    measurements->log[measurements->log_count].reserved = 0U;
    measurements->log[measurements->log_count].component_length =
        component_length;
    memcpy(measurements->log[measurements->log_count].digest, digest,
           SHA256_DIGEST_SIZE);
    ++measurements->log_count;
    return BOOT_MEASUREMENTS_OK;
}

enum boot_measurements_status boot_measurements_measure(
    struct boot_measurements *measurements,
    uint32_t register_index,
    uint32_t component_kind,
    const void *data,
    size_t data_length)
{
    unsigned char digest[SHA256_DIGEST_SIZE];

    if (measurements == NULL || data == NULL) {
        return BOOT_MEASUREMENTS_NULL;
    }
    if (measurements->version != BOOT_MEASUREMENTS_VERSION) {
        return BOOT_MEASUREMENTS_BAD_STATE;
    }
    if (measurements->published) {
        return BOOT_MEASUREMENTS_BAD_STATE;
    }
    if (data_length > (size_t)SHA256_MAX_INPUT_BYTES) {
        (void)boot_measurements_fail(measurements);
        return BOOT_MEASUREMENTS_DIGEST_FAILED;
    }
    if (sha256_compute(data, data_length, digest) != SHA256_OK) {
        (void)boot_measurements_fail(measurements);
        return BOOT_MEASUREMENTS_DIGEST_FAILED;
    }
    return boot_measurements_record(measurements, register_index,
                                    component_kind,
                                    (uint64_t)data_length, digest);
}

enum boot_measurements_status boot_measurements_publish(
    struct boot_measurements *measurements)
{
    struct sha256 hash;
    unsigned char encoded[8];
    unsigned char aggregate[SHA256_DIGEST_SIZE];
    uint32_t index;

    if (measurements == NULL) {
        return BOOT_MEASUREMENTS_NULL;
    }
    if (measurements->version != BOOT_MEASUREMENTS_VERSION ||
        measurements->failed) {
        return BOOT_MEASUREMENTS_BAD_STATE;
    }
    if (measurements->published) {
        return BOOT_MEASUREMENTS_BAD_STATE;
    }

    sha256_init(&hash);
    boot_measurements_write_u32(encoded, measurements->register_count);
    sha256_update(&hash, encoded, sizeof(encoded));
    for (index = 0U; index < measurements->register_count; ++index) {
        sha256_update(&hash, measurements->registers[index],
                      SHA256_DIGEST_SIZE);
    }
    boot_measurements_write_u32(encoded, measurements->log_count);
    sha256_update(&hash, encoded, sizeof(encoded));
    for (index = 0U; index < measurements->log_count; ++index) {
        boot_measurements_write_u32(
            encoded, measurements->log[index].component_kind);
        sha256_update(&hash, encoded, sizeof(encoded));
        boot_measurements_write_u64(
            encoded, measurements->log[index].component_length);
        sha256_update(&hash, encoded, sizeof(encoded));
        sha256_update(&hash, measurements->log[index].digest,
                      SHA256_DIGEST_SIZE);
    }
    if (sha256_final(&hash, aggregate) != SHA256_OK) {
        (void)boot_measurements_fail(measurements);
        return BOOT_MEASUREMENTS_DIGEST_FAILED;
    }
    memcpy(measurements->aggregate, aggregate, SHA256_DIGEST_SIZE);
    measurements->aggregate_published = true;
    measurements->published = true;
    return BOOT_MEASUREMENTS_OK;
}

bool boot_measurements_is_published(
    const struct boot_measurements *measurements)
{
    return measurements != NULL &&
           measurements->version == BOOT_MEASUREMENTS_VERSION &&
           measurements->published && !measurements->failed;
}

bool boot_measurements_failed(
    const struct boot_measurements *measurements)
{
    return measurements != NULL &&
           measurements->version == BOOT_MEASUREMENTS_VERSION &&
           measurements->failed;
}

uint32_t boot_measurements_register_count(
    const struct boot_measurements *measurements)
{
    if (measurements == NULL ||
        measurements->version != BOOT_MEASUREMENTS_VERSION) {
        return 0U;
    }
    return measurements->register_count;
}

enum boot_measurements_status boot_measurements_read(
    const struct boot_measurements *measurements,
    uint32_t register_index,
    unsigned char digest_out[SHA256_DIGEST_SIZE])
{
    if (measurements == NULL || digest_out == NULL) {
        return BOOT_MEASUREMENTS_NULL;
    }
    if (measurements->version != BOOT_MEASUREMENTS_VERSION ||
        !measurements->published) {
        return BOOT_MEASUREMENTS_BAD_STATE;
    }
    if (register_index >= BOOT_MEASUREMENTS_REGISTER_COUNT) {
        return BOOT_MEASUREMENTS_BAD_INDEX;
    }
    memcpy(digest_out, measurements->registers[register_index],
           SHA256_DIGEST_SIZE);
    return BOOT_MEASUREMENTS_OK;
}

uint32_t boot_measurements_log_count(
    const struct boot_measurements *measurements)
{
    if (measurements == NULL ||
        measurements->version != BOOT_MEASUREMENTS_VERSION) {
        return 0U;
    }
    return measurements->log_count;
}

const struct boot_measurements_log_entry *boot_measurements_log_entry(
    const struct boot_measurements *measurements,
    uint32_t index)
{
    if (measurements == NULL ||
        measurements->version != BOOT_MEASUREMENTS_VERSION ||
        index >= measurements->log_count) {
        return NULL;
    }
    return &measurements->log[index];
}

enum boot_measurements_status boot_measurements_aggregate(
    const struct boot_measurements *measurements,
    unsigned char digest_out[SHA256_DIGEST_SIZE])
{
    if (measurements == NULL || digest_out == NULL) {
        return BOOT_MEASUREMENTS_NULL;
    }
    if (measurements->version != BOOT_MEASUREMENTS_VERSION ||
        !measurements->aggregate_published || measurements->failed) {
        return BOOT_MEASUREMENTS_BAD_STATE;
    }
    memcpy(digest_out, measurements->aggregate, SHA256_DIGEST_SIZE);
    return BOOT_MEASUREMENTS_OK;
}

const char *boot_measurements_status_string(
    enum boot_measurements_status status)
{
    if ((size_t)status >=
        sizeof(boot_measurements_status_names) /
            sizeof(boot_measurements_status_names[0])) {
        return "UNKNOWN";
    }
    return boot_measurements_status_names[status];
}
