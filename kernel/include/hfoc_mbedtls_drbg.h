#ifndef HFOC_MBEDTLS_DRBG_H
#define HFOC_MBEDTLS_DRBG_H

#include "entropy_seed_fill.h"

#include <stdbool.h>
#include <stdint.h>

#define HFOC_MBEDTLS_DRBG_VERSION 1U

#define HFOC_MBEDTLS_DRBG_PURPOSE_TLS_HANDSHAKE 1U
#define HFOC_MBEDTLS_DRBG_PURPOSE_MIN 1U
#define HFOC_MBEDTLS_DRBG_PURPOSE_MAX UINT32_C(0xFFFFFFFE)

#define HFOC_MBEDTLS_DRBG_GENERATION_MIN 1U
#define HFOC_MBEDTLS_DRBG_GENERATION_MAX UINT32_C(0xFFFFFFFE)
#define HFOC_MBEDTLS_DRBG_SOURCE_ID_MIN 1U
#define HFOC_MBEDTLS_DRBG_SOURCE_ID_MAX UINT32_C(0xFFFFFFFE)

#define HFOC_MBEDTLS_DRBG_REQUIRED_BYTES_MIN \
    ENTROPY_SEED_FILL_REQUIRED_BYTES_MIN
#define HFOC_MBEDTLS_DRBG_REQUIRED_BYTES_MAX \
    ENTROPY_SEED_FILL_REQUIRED_BYTES_MAX
#define HFOC_MBEDTLS_DRBG_ENTROPY_LENGTH 48U
#define HFOC_MBEDTLS_DRBG_FILL_COUNT_MIN 1U
#define HFOC_MBEDTLS_DRBG_FILL_COUNT_MAX \
    ENTROPY_SEED_FILL_FILL_COUNT_MAX
#define HFOC_MBEDTLS_DRBG_RESEED_INTERVAL_MIN 1U
#define HFOC_MBEDTLS_DRBG_RESEED_INTERVAL_MAX 1000U
#define HFOC_MBEDTLS_DRBG_RANDOM_MAX 256U
#define HFOC_MBEDTLS_DRBG_CONTEXT_STORAGE_BYTES 512U

enum hfoc_mbedtls_drbg_state {
    HFOC_MBEDTLS_DRBG_STATE_UNINITIALIZED = 0,
    HFOC_MBEDTLS_DRBG_STATE_UNAVAILABLE = 1,
    HFOC_MBEDTLS_DRBG_STATE_DEGRADED = 2,
    HFOC_MBEDTLS_DRBG_STATE_READY = 3,
    HFOC_MBEDTLS_DRBG_STATE_PUBLISHED = 4,
    HFOC_MBEDTLS_DRBG_STATE_FAILED = 5,
};

enum hfoc_mbedtls_drbg_fault {
    HFOC_MBEDTLS_DRBG_FAULT_NONE = 0,
    HFOC_MBEDTLS_DRBG_FAULT_ENTROPY_UNAVAILABLE = 1,
    HFOC_MBEDTLS_DRBG_FAULT_ENTROPY_DEGRADED = 2,
    HFOC_MBEDTLS_DRBG_FAULT_ENTROPY_REJECTED = 3,
    HFOC_MBEDTLS_DRBG_FAULT_GENERATION_MISMATCH = 4,
    HFOC_MBEDTLS_DRBG_FAULT_SOURCE_MISMATCH = 5,
    HFOC_MBEDTLS_DRBG_FAULT_SEED_FAILED = 6,
    HFOC_MBEDTLS_DRBG_FAULT_RESEED_FAILED = 7,
    HFOC_MBEDTLS_DRBG_FAULT_RANDOM_FAILED = 8,
    HFOC_MBEDTLS_DRBG_FAULT_OUTPUT_OVERLAP = 9,
    HFOC_MBEDTLS_DRBG_FAULT_LENGTH_OVERFLOW = 10,
    HFOC_MBEDTLS_DRBG_FAULT_PURPOSE_MISMATCH = 11,
    HFOC_MBEDTLS_DRBG_FAULT_CONTEXT_STORAGE_OVERFLOW = 12,
    HFOC_MBEDTLS_DRBG_FAULT_CONTEXT_NOT_PUBLISHED = 13,
};

struct hfoc_mbedtls_drbg_config {
    uint32_t version;
    uint32_t purpose_id;
    uint32_t generation;
    uint32_t source_id;
    uint32_t required_credited_bytes;
    uint32_t fill_count_max;
    uint32_t reseed_interval;
};

struct hfoc_mbedtls_drbg {
    bool initialized;
    bool drbg_published;
    bool context_initialized;
    uint32_t version;
    uint32_t purpose_id;
    uint32_t generation;
    uint32_t source_id;
    uint32_t required_credited_bytes;
    uint32_t fill_count_max;
    uint32_t reseed_interval;
    uint32_t random_calls;
    enum hfoc_mbedtls_drbg_state state;
    enum hfoc_mbedtls_drbg_fault fault;
    struct entropy_seed_fill seed_fill;
    const struct entropy_accumulator *accumulator;
    _Alignas(64) uint8_t
        context_storage[HFOC_MBEDTLS_DRBG_CONTEXT_STORAGE_BYTES];
};

enum hfoc_mbedtls_drbg_status {
    HFOC_MBEDTLS_DRBG_OK = 0,
    HFOC_MBEDTLS_DRBG_NULL = 1,
    HFOC_MBEDTLS_DRBG_BAD_STATE = 2,
    HFOC_MBEDTLS_DRBG_ALREADY_PUBLISHED = 3,
    HFOC_MBEDTLS_DRBG_BAD_VERSION = 4,
    HFOC_MBEDTLS_DRBG_BAD_PURPOSE = 5,
    HFOC_MBEDTLS_DRBG_BAD_GENERATION = 6,
    HFOC_MBEDTLS_DRBG_BAD_SOURCE_ID = 7,
    HFOC_MBEDTLS_DRBG_BAD_REQUIRED_BYTES = 8,
    HFOC_MBEDTLS_DRBG_BAD_FILL_COUNT = 9,
    HFOC_MBEDTLS_DRBG_BAD_RESEED_INTERVAL = 10,
    HFOC_MBEDTLS_DRBG_ENTROPY_UNAVAILABLE = 11,
    HFOC_MBEDTLS_DRBG_ENTROPY_DEGRADED = 12,
    HFOC_MBEDTLS_DRBG_ENTROPY_REJECTED = 13,
    HFOC_MBEDTLS_DRBG_GENERATION_MISMATCH = 14,
    HFOC_MBEDTLS_DRBG_SOURCE_MISMATCH = 15,
    HFOC_MBEDTLS_DRBG_SEED_FAILED = 16,
    HFOC_MBEDTLS_DRBG_RESEED_FAILED = 17,
    HFOC_MBEDTLS_DRBG_RANDOM_FAILED = 18,
    HFOC_MBEDTLS_DRBG_LENGTH_OVERFLOW = 19,
    HFOC_MBEDTLS_DRBG_OUTPUT_TOO_SMALL = 20,
    HFOC_MBEDTLS_DRBG_OUTPUT_OVERLAP = 21,
    HFOC_MBEDTLS_DRBG_NOT_PUBLISHED = 22,
    HFOC_MBEDTLS_DRBG_CONTEXT_STORAGE_OVERFLOW = 23,
};

enum hfoc_mbedtls_drbg_status hfoc_mbedtls_drbg_init(
    struct hfoc_mbedtls_drbg *drbg,
    const struct hfoc_mbedtls_drbg_config *config,
    const struct entropy_accumulator *accumulator);
enum hfoc_mbedtls_drbg_status hfoc_mbedtls_drbg_publish(
    struct hfoc_mbedtls_drbg *drbg,
    const struct entropy_accumulator *accumulator);
enum hfoc_mbedtls_drbg_status hfoc_mbedtls_drbg_reseed(
    struct hfoc_mbedtls_drbg *drbg,
    const struct entropy_accumulator *accumulator);
enum hfoc_mbedtls_drbg_status hfoc_mbedtls_drbg_generate(
    struct hfoc_mbedtls_drbg *drbg,
    const struct entropy_accumulator *accumulator,
    void *output,
    uint32_t output_capacity,
    uint32_t requested_length,
    uint32_t *bytes_written);
enum hfoc_mbedtls_drbg_status hfoc_mbedtls_drbg_invalidate(
    struct hfoc_mbedtls_drbg *drbg,
    uint32_t generation);
void hfoc_mbedtls_drbg_shutdown(struct hfoc_mbedtls_drbg *drbg);

bool hfoc_mbedtls_drbg_initialized(
    const struct hfoc_mbedtls_drbg *drbg);
bool hfoc_mbedtls_drbg_published(
    const struct hfoc_mbedtls_drbg *drbg);
enum hfoc_mbedtls_drbg_state hfoc_mbedtls_drbg_state(
    const struct hfoc_mbedtls_drbg *drbg);
enum hfoc_mbedtls_drbg_fault hfoc_mbedtls_drbg_fault(
    const struct hfoc_mbedtls_drbg *drbg);
uint32_t hfoc_mbedtls_drbg_purpose_id(
    const struct hfoc_mbedtls_drbg *drbg);
uint32_t hfoc_mbedtls_drbg_generation(
    const struct hfoc_mbedtls_drbg *drbg);
uint32_t hfoc_mbedtls_drbg_source_id(
    const struct hfoc_mbedtls_drbg *drbg);
uint32_t hfoc_mbedtls_drbg_fill_count(
    const struct hfoc_mbedtls_drbg *drbg);
uint32_t hfoc_mbedtls_drbg_random_calls(
    const struct hfoc_mbedtls_drbg *drbg);
const char *hfoc_mbedtls_drbg_status_string(
    enum hfoc_mbedtls_drbg_status status);
const char *hfoc_mbedtls_drbg_state_string(
    enum hfoc_mbedtls_drbg_state state);
const char *hfoc_mbedtls_drbg_fault_string(
    enum hfoc_mbedtls_drbg_fault fault);

#endif
