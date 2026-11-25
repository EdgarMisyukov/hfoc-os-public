#include "hfoc_mbedtls_drbg.h"

#include "entropy_seed_fill.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#if defined(HFOC_MBEDTLS_DRBG_HOST_TEST) || \
    defined(HFOC_PRODUCTION_TLS)

#include "mbedtls/ctr_drbg.h"

typedef char hfoc_mbedtls_drbg_context_fits[
    sizeof(mbedtls_ctr_drbg_context) <=
        HFOC_MBEDTLS_DRBG_CONTEXT_STORAGE_BYTES ? 1 : -1];

static bool hfoc_mbedtls_drbg_generation_valid(uint32_t generation)
{
    return generation >= HFOC_MBEDTLS_DRBG_GENERATION_MIN &&
           generation <= HFOC_MBEDTLS_DRBG_GENERATION_MAX;
}

static bool hfoc_mbedtls_drbg_source_id_valid(uint32_t source_id)
{
    return source_id >= HFOC_MBEDTLS_DRBG_SOURCE_ID_MIN &&
           source_id <= HFOC_MBEDTLS_DRBG_SOURCE_ID_MAX;
}

static bool hfoc_mbedtls_drbg_purpose_valid(uint32_t purpose_id)
{
    return purpose_id >= HFOC_MBEDTLS_DRBG_PURPOSE_MIN &&
           purpose_id <= HFOC_MBEDTLS_DRBG_PURPOSE_MAX;
}

static bool hfoc_mbedtls_drbg_config_valid(
    const struct hfoc_mbedtls_drbg_config *config)
{
    return config != NULL &&
           config->version == HFOC_MBEDTLS_DRBG_VERSION &&
           hfoc_mbedtls_drbg_purpose_valid(config->purpose_id) &&
           hfoc_mbedtls_drbg_generation_valid(config->generation) &&
           hfoc_mbedtls_drbg_source_id_valid(config->source_id) &&
           config->required_credited_bytes >=
               HFOC_MBEDTLS_DRBG_REQUIRED_BYTES_MIN &&
           config->required_credited_bytes <=
               HFOC_MBEDTLS_DRBG_REQUIRED_BYTES_MAX &&
           config->fill_count_max >= HFOC_MBEDTLS_DRBG_FILL_COUNT_MIN &&
           config->fill_count_max <= HFOC_MBEDTLS_DRBG_FILL_COUNT_MAX &&
           config->reseed_interval >=
               HFOC_MBEDTLS_DRBG_RESEED_INTERVAL_MIN &&
           config->reseed_interval <=
               HFOC_MBEDTLS_DRBG_RESEED_INTERVAL_MAX;
}

static bool hfoc_mbedtls_drbg_state_valid(
    enum hfoc_mbedtls_drbg_state state)
{
    return state == HFOC_MBEDTLS_DRBG_STATE_UNAVAILABLE ||
           state == HFOC_MBEDTLS_DRBG_STATE_DEGRADED ||
           state == HFOC_MBEDTLS_DRBG_STATE_READY ||
           state == HFOC_MBEDTLS_DRBG_STATE_PUBLISHED ||
           state == HFOC_MBEDTLS_DRBG_STATE_FAILED;
}

static bool hfoc_mbedtls_drbg_object_valid(
    const struct hfoc_mbedtls_drbg *drbg)
{
    return drbg != NULL &&
           drbg->initialized &&
           drbg->version == HFOC_MBEDTLS_DRBG_VERSION &&
           hfoc_mbedtls_drbg_purpose_valid(drbg->purpose_id) &&
           (drbg->generation == 0U ||
            hfoc_mbedtls_drbg_generation_valid(drbg->generation)) &&
           (drbg->source_id == 0U ||
            hfoc_mbedtls_drbg_source_id_valid(drbg->source_id)) &&
           drbg->required_credited_bytes >=
               HFOC_MBEDTLS_DRBG_REQUIRED_BYTES_MIN &&
           drbg->required_credited_bytes <=
               HFOC_MBEDTLS_DRBG_REQUIRED_BYTES_MAX &&
           drbg->fill_count_max >= HFOC_MBEDTLS_DRBG_FILL_COUNT_MIN &&
           drbg->fill_count_max <= HFOC_MBEDTLS_DRBG_FILL_COUNT_MAX &&
           drbg->reseed_interval >=
               HFOC_MBEDTLS_DRBG_RESEED_INTERVAL_MIN &&
           drbg->reseed_interval <=
               HFOC_MBEDTLS_DRBG_RESEED_INTERVAL_MAX &&
           hfoc_mbedtls_drbg_state_valid(drbg->state);
}

static void hfoc_mbedtls_drbg_zeroize(
    void *object,
    size_t size)
{
    volatile uint8_t *bytes = (volatile uint8_t *)object;
    size_t index;

    for (index = 0U; index < size; ++index) {
        bytes[index] = 0U;
    }
}

static bool hfoc_mbedtls_drbg_ranges_overlap(
    const void *first,
    size_t first_length,
    const void *second,
    size_t second_length)
{
    uintptr_t first_start = (uintptr_t)first;
    uintptr_t second_start = (uintptr_t)second;
    uintptr_t first_end;
    uintptr_t second_end;

    if (first_start > (uintptr_t)-1 - first_length ||
        second_start > (uintptr_t)-1 - second_length) {
        return true;
    }
    first_end = first_start + first_length;
    second_end = second_start + second_length;
    return first_start < second_end && second_start < first_end;
}

static mbedtls_ctr_drbg_context *hfoc_mbedtls_drbg_context(
    struct hfoc_mbedtls_drbg *drbg)
{
    return (mbedtls_ctr_drbg_context *)(void *)drbg->context_storage;
}

static enum hfoc_mbedtls_drbg_status
hfoc_mbedtls_drbg_map_seed_fill_status(
    enum entropy_seed_fill_status status)
{
    switch (status) {
    case ENTROPY_SEED_FILL_GENERATION_MISMATCH:
        return HFOC_MBEDTLS_DRBG_GENERATION_MISMATCH;
    case ENTROPY_SEED_FILL_SOURCE_MISMATCH:
        return HFOC_MBEDTLS_DRBG_SOURCE_MISMATCH;
    case ENTROPY_SEED_FILL_BAD_VERSION:
        return HFOC_MBEDTLS_DRBG_BAD_VERSION;
    case ENTROPY_SEED_FILL_BAD_GENERATION:
        return HFOC_MBEDTLS_DRBG_BAD_GENERATION;
    case ENTROPY_SEED_FILL_BAD_SOURCE_ID:
        return HFOC_MBEDTLS_DRBG_BAD_SOURCE_ID;
    case ENTROPY_SEED_FILL_BAD_REQUIRED_BYTES:
        return HFOC_MBEDTLS_DRBG_BAD_REQUIRED_BYTES;
    case ENTROPY_SEED_FILL_BAD_FILL_COUNT:
        return HFOC_MBEDTLS_DRBG_BAD_FILL_COUNT;
    default:
        return HFOC_MBEDTLS_DRBG_BAD_STATE;
    }
}

static void hfoc_mbedtls_drbg_fail(
    struct hfoc_mbedtls_drbg *drbg,
    enum hfoc_mbedtls_drbg_fault fault)
{
    if (drbg->context_initialized) {
        mbedtls_ctr_drbg_free(
            hfoc_mbedtls_drbg_context(drbg));
        drbg->context_initialized = false;
    }
    hfoc_mbedtls_drbg_zeroize(
        drbg->context_storage,
        sizeof(drbg->context_storage));
    memset(&drbg->seed_fill, 0, sizeof(drbg->seed_fill));
    drbg->drbg_published = false;
    drbg->generation = 0U;
    drbg->source_id = 0U;
    drbg->accumulator = NULL;
    drbg->random_calls = 0U;
    drbg->state = HFOC_MBEDTLS_DRBG_STATE_FAILED;
    drbg->fault = fault;
}

static enum hfoc_mbedtls_drbg_status
hfoc_mbedtls_drbg_check_accumulator(
    struct hfoc_mbedtls_drbg *drbg,
    const struct entropy_accumulator *accumulator)
{
    if (accumulator == NULL ||
        !entropy_accumulator_initialized(accumulator) ||
        !entropy_accumulator_accumulating(accumulator)) {
        hfoc_mbedtls_drbg_fail(
            drbg,
            HFOC_MBEDTLS_DRBG_FAULT_ENTROPY_UNAVAILABLE);
        return HFOC_MBEDTLS_DRBG_ENTROPY_UNAVAILABLE;
    }
    if (entropy_accumulator_generation(accumulator) !=
        drbg->generation) {
        hfoc_mbedtls_drbg_fail(
            drbg,
            HFOC_MBEDTLS_DRBG_FAULT_GENERATION_MISMATCH);
        return HFOC_MBEDTLS_DRBG_GENERATION_MISMATCH;
    }
    if (entropy_accumulator_source_id(accumulator) !=
        drbg->source_id) {
        hfoc_mbedtls_drbg_fail(
            drbg,
            HFOC_MBEDTLS_DRBG_FAULT_SOURCE_MISMATCH);
        return HFOC_MBEDTLS_DRBG_SOURCE_MISMATCH;
    }
    if (entropy_accumulator_credited_bytes(accumulator) <
        drbg->required_credited_bytes) {
        hfoc_mbedtls_drbg_fail(
            drbg,
            HFOC_MBEDTLS_DRBG_FAULT_ENTROPY_DEGRADED);
        return HFOC_MBEDTLS_DRBG_ENTROPY_DEGRADED;
    }
    return HFOC_MBEDTLS_DRBG_OK;
}

static int hfoc_mbedtls_drbg_entropy_cb(
    void *opaque,
    unsigned char *output,
    size_t len)
{
    struct hfoc_mbedtls_drbg *drbg = opaque;
    enum entropy_seed_fill_status status;
    uint32_t bytes_written = 0U;

    if (drbg == NULL || output == NULL ||
        len != HFOC_MBEDTLS_DRBG_ENTROPY_LENGTH) {
        if (drbg != NULL) {
            hfoc_mbedtls_drbg_fail(
                drbg,
                HFOC_MBEDTLS_DRBG_FAULT_ENTROPY_REJECTED);
        }
        return MBEDTLS_ERR_CTR_DRBG_ENTROPY_SOURCE_FAILED;
    }

    if (!entropy_seed_fill_seed_published(&drbg->seed_fill)) {
        status = entropy_seed_fill_seed(
            &drbg->seed_fill,
            drbg->accumulator,
            output,
            (uint32_t)len,
            &bytes_written);
    } else {
        status = entropy_seed_fill_fill(
            &drbg->seed_fill,
            drbg->accumulator,
            output,
            (uint32_t)len,
            HFOC_MBEDTLS_DRBG_ENTROPY_LENGTH,
            &bytes_written);
    }

    if (status != ENTROPY_SEED_FILL_OK ||
        bytes_written != (uint32_t)len) {
        enum hfoc_mbedtls_drbg_fault fault;

        if (status == ENTROPY_SEED_FILL_GENERATION_MISMATCH) {
            fault = HFOC_MBEDTLS_DRBG_FAULT_GENERATION_MISMATCH;
        } else if (status == ENTROPY_SEED_FILL_SOURCE_MISMATCH) {
            fault = HFOC_MBEDTLS_DRBG_FAULT_SOURCE_MISMATCH;
        } else {
            fault = HFOC_MBEDTLS_DRBG_FAULT_ENTROPY_REJECTED;
        }
        hfoc_mbedtls_drbg_fail(drbg, fault);
        return MBEDTLS_ERR_CTR_DRBG_ENTROPY_SOURCE_FAILED;
    }
    return 0;
}

static void hfoc_mbedtls_drbg_encode_purpose(
    uint32_t purpose_id,
    uint8_t *purpose)
{
    uint32_t index;

    for (index = 0U; index < 8U; ++index) {
        purpose[index] = (uint8_t)(
            (purpose_id >> (index * 8U)) & 0xFFU);
    }
}

enum hfoc_mbedtls_drbg_status hfoc_mbedtls_drbg_init(
    struct hfoc_mbedtls_drbg *drbg,
    const struct hfoc_mbedtls_drbg_config *config,
    const struct entropy_accumulator *accumulator)
{
    struct entropy_seed_fill_config seed_config;
    enum entropy_seed_fill_status seed_status;

    if (drbg == NULL || config == NULL) {
        return HFOC_MBEDTLS_DRBG_NULL;
    }
    if (drbg->initialized) {
        return HFOC_MBEDTLS_DRBG_ALREADY_PUBLISHED;
    }
    if (!hfoc_mbedtls_drbg_config_valid(config)) {
        if (config->version != HFOC_MBEDTLS_DRBG_VERSION) {
            return HFOC_MBEDTLS_DRBG_BAD_VERSION;
        }
        if (!hfoc_mbedtls_drbg_purpose_valid(config->purpose_id)) {
            return HFOC_MBEDTLS_DRBG_BAD_PURPOSE;
        }
        if (!hfoc_mbedtls_drbg_generation_valid(
                config->generation)) {
            return HFOC_MBEDTLS_DRBG_BAD_GENERATION;
        }
        if (!hfoc_mbedtls_drbg_source_id_valid(
                config->source_id)) {
            return HFOC_MBEDTLS_DRBG_BAD_SOURCE_ID;
        }
        if (config->required_credited_bytes <
                HFOC_MBEDTLS_DRBG_REQUIRED_BYTES_MIN ||
            config->required_credited_bytes >
                HFOC_MBEDTLS_DRBG_REQUIRED_BYTES_MAX) {
            return HFOC_MBEDTLS_DRBG_BAD_REQUIRED_BYTES;
        }
        if (config->fill_count_max <
                HFOC_MBEDTLS_DRBG_FILL_COUNT_MIN ||
            config->fill_count_max >
                HFOC_MBEDTLS_DRBG_FILL_COUNT_MAX) {
            return HFOC_MBEDTLS_DRBG_BAD_FILL_COUNT;
        }
        return HFOC_MBEDTLS_DRBG_BAD_RESEED_INTERVAL;
    }

    memset(&seed_config, 0, sizeof(seed_config));
    seed_config.version = ENTROPY_SEED_FILL_VERSION;
    seed_config.generation = config->generation;
    seed_config.source_id = config->source_id;
    seed_config.required_credited_bytes =
        config->required_credited_bytes;
    seed_config.seed_length = HFOC_MBEDTLS_DRBG_ENTROPY_LENGTH;
    seed_config.fill_length_max =
        HFOC_MBEDTLS_DRBG_ENTROPY_LENGTH;
    seed_config.fill_count_max = config->fill_count_max;

    memset(drbg, 0, sizeof(*drbg));
    seed_status = entropy_seed_fill_init(
        &drbg->seed_fill, &seed_config, accumulator);
    if (seed_status != ENTROPY_SEED_FILL_OK) {
        return hfoc_mbedtls_drbg_map_seed_fill_status(seed_status);
    }

    drbg->initialized = true;
    drbg->version = HFOC_MBEDTLS_DRBG_VERSION;
    drbg->purpose_id = config->purpose_id;
    drbg->generation = config->generation;
    drbg->source_id = config->source_id;
    drbg->required_credited_bytes =
        config->required_credited_bytes;
    drbg->fill_count_max = config->fill_count_max;
    drbg->reseed_interval = config->reseed_interval;
    drbg->accumulator = accumulator;

    switch (entropy_seed_fill_state(&drbg->seed_fill)) {
    case ENTROPY_SEED_FILL_STATE_UNAVAILABLE:
        drbg->state = HFOC_MBEDTLS_DRBG_STATE_UNAVAILABLE;
        drbg->fault = HFOC_MBEDTLS_DRBG_FAULT_ENTROPY_UNAVAILABLE;
        break;
    case ENTROPY_SEED_FILL_STATE_DEGRADED:
        drbg->state = HFOC_MBEDTLS_DRBG_STATE_DEGRADED;
        drbg->fault = HFOC_MBEDTLS_DRBG_FAULT_ENTROPY_DEGRADED;
        break;
    case ENTROPY_SEED_FILL_STATE_READY:
        drbg->state = HFOC_MBEDTLS_DRBG_STATE_READY;
        drbg->fault = HFOC_MBEDTLS_DRBG_FAULT_NONE;
        break;
    default:
        drbg->state = HFOC_MBEDTLS_DRBG_STATE_FAILED;
        drbg->fault = HFOC_MBEDTLS_DRBG_FAULT_ENTROPY_REJECTED;
        break;
    }
    return HFOC_MBEDTLS_DRBG_OK;
}

enum hfoc_mbedtls_drbg_status hfoc_mbedtls_drbg_publish(
    struct hfoc_mbedtls_drbg *drbg,
    const struct entropy_accumulator *accumulator)
{
    mbedtls_ctr_drbg_context *context;
    uint8_t purpose[8];
    enum hfoc_mbedtls_drbg_status status;
    int result;

    if (drbg == NULL) {
        return HFOC_MBEDTLS_DRBG_NULL;
    }
    if (!hfoc_mbedtls_drbg_object_valid(drbg)) {
        return HFOC_MBEDTLS_DRBG_BAD_STATE;
    }
    if (drbg->drbg_published) {
        return HFOC_MBEDTLS_DRBG_ALREADY_PUBLISHED;
    }
    if (drbg->state == HFOC_MBEDTLS_DRBG_STATE_UNAVAILABLE) {
        return HFOC_MBEDTLS_DRBG_ENTROPY_UNAVAILABLE;
    }
    if (drbg->state == HFOC_MBEDTLS_DRBG_STATE_DEGRADED) {
        return HFOC_MBEDTLS_DRBG_ENTROPY_DEGRADED;
    }
    if (drbg->state != HFOC_MBEDTLS_DRBG_STATE_READY) {
        return HFOC_MBEDTLS_DRBG_BAD_STATE;
    }

    status = hfoc_mbedtls_drbg_check_accumulator(
        drbg, accumulator);
    if (status != HFOC_MBEDTLS_DRBG_OK) {
        return status;
    }
    drbg->accumulator = accumulator;

    context = hfoc_mbedtls_drbg_context(drbg);
    mbedtls_ctr_drbg_init(context);
    drbg->context_initialized = true;
    mbedtls_ctr_drbg_set_entropy_len(
        context, HFOC_MBEDTLS_DRBG_ENTROPY_LENGTH);
    (void)mbedtls_ctr_drbg_set_nonce_len(context, 0U);
    mbedtls_ctr_drbg_set_reseed_interval(
        context, (int)drbg->reseed_interval);
    hfoc_mbedtls_drbg_encode_purpose(
        drbg->purpose_id, purpose);

    result = mbedtls_ctr_drbg_seed(
        context,
        hfoc_mbedtls_drbg_entropy_cb,
        drbg,
        purpose,
        sizeof(purpose));
    if (result != 0) {
        if (drbg->state != HFOC_MBEDTLS_DRBG_STATE_FAILED) {
            hfoc_mbedtls_drbg_fail(
                drbg,
                HFOC_MBEDTLS_DRBG_FAULT_SEED_FAILED);
            return HFOC_MBEDTLS_DRBG_SEED_FAILED;
        }
        if (drbg->fault ==
            HFOC_MBEDTLS_DRBG_FAULT_GENERATION_MISMATCH) {
            return HFOC_MBEDTLS_DRBG_GENERATION_MISMATCH;
        }
        if (drbg->fault ==
            HFOC_MBEDTLS_DRBG_FAULT_SOURCE_MISMATCH) {
            return HFOC_MBEDTLS_DRBG_SOURCE_MISMATCH;
        }
        return HFOC_MBEDTLS_DRBG_ENTROPY_REJECTED;
    }

    drbg->drbg_published = true;
    drbg->random_calls = 0U;
    drbg->state = HFOC_MBEDTLS_DRBG_STATE_PUBLISHED;
    drbg->fault = HFOC_MBEDTLS_DRBG_FAULT_NONE;
    return HFOC_MBEDTLS_DRBG_OK;
}

enum hfoc_mbedtls_drbg_status hfoc_mbedtls_drbg_reseed(
    struct hfoc_mbedtls_drbg *drbg,
    const struct entropy_accumulator *accumulator)
{
    mbedtls_ctr_drbg_context *context;
    enum hfoc_mbedtls_drbg_status status;
    int result;

    if (drbg == NULL) {
        return HFOC_MBEDTLS_DRBG_NULL;
    }
    if (!hfoc_mbedtls_drbg_object_valid(drbg)) {
        return HFOC_MBEDTLS_DRBG_BAD_STATE;
    }
    if (!drbg->drbg_published) {
        return HFOC_MBEDTLS_DRBG_NOT_PUBLISHED;
    }
    status = hfoc_mbedtls_drbg_check_accumulator(
        drbg, accumulator);
    if (status != HFOC_MBEDTLS_DRBG_OK) {
        return status;
    }
    drbg->accumulator = accumulator;

    context = hfoc_mbedtls_drbg_context(drbg);
    result = mbedtls_ctr_drbg_reseed(context, NULL, 0U);
    if (result != 0) {
        if (drbg->state != HFOC_MBEDTLS_DRBG_STATE_FAILED) {
            hfoc_mbedtls_drbg_fail(
                drbg,
                HFOC_MBEDTLS_DRBG_FAULT_RESEED_FAILED);
            return HFOC_MBEDTLS_DRBG_RESEED_FAILED;
        }
        if (drbg->fault ==
            HFOC_MBEDTLS_DRBG_FAULT_GENERATION_MISMATCH) {
            return HFOC_MBEDTLS_DRBG_GENERATION_MISMATCH;
        }
        if (drbg->fault ==
            HFOC_MBEDTLS_DRBG_FAULT_SOURCE_MISMATCH) {
            return HFOC_MBEDTLS_DRBG_SOURCE_MISMATCH;
        }
        return HFOC_MBEDTLS_DRBG_ENTROPY_REJECTED;
    }
    return HFOC_MBEDTLS_DRBG_OK;
}

enum hfoc_mbedtls_drbg_status hfoc_mbedtls_drbg_generate(
    struct hfoc_mbedtls_drbg *drbg,
    const struct entropy_accumulator *accumulator,
    void *output,
    uint32_t output_capacity,
    uint32_t requested_length,
    uint32_t *bytes_written)
{
    mbedtls_ctr_drbg_context *context;
    enum hfoc_mbedtls_drbg_status status;
    int result;

    if (drbg == NULL || output == NULL || bytes_written == NULL) {
        return HFOC_MBEDTLS_DRBG_NULL;
    }
    if (!drbg->initialized) {
        return HFOC_MBEDTLS_DRBG_NOT_PUBLISHED;
    }
    if (!hfoc_mbedtls_drbg_object_valid(drbg)) {
        return HFOC_MBEDTLS_DRBG_BAD_STATE;
    }
    if (!drbg->drbg_published) {
        return HFOC_MBEDTLS_DRBG_NOT_PUBLISHED;
    }
    if (requested_length == 0U ||
        requested_length > HFOC_MBEDTLS_DRBG_RANDOM_MAX) {
        return HFOC_MBEDTLS_DRBG_LENGTH_OVERFLOW;
    }
    if (output_capacity < requested_length) {
        return HFOC_MBEDTLS_DRBG_OUTPUT_TOO_SMALL;
    }
    if (hfoc_mbedtls_drbg_ranges_overlap(
            drbg,
            sizeof(*drbg),
            output,
            requested_length) ||
        (accumulator != NULL &&
         hfoc_mbedtls_drbg_ranges_overlap(
            accumulator,
            sizeof(*accumulator),
            output,
            requested_length))) {
        return HFOC_MBEDTLS_DRBG_OUTPUT_OVERLAP;
    }

    status = hfoc_mbedtls_drbg_check_accumulator(
        drbg, accumulator);
    if (status != HFOC_MBEDTLS_DRBG_OK) {
        return status;
    }
    drbg->accumulator = accumulator;

    context = hfoc_mbedtls_drbg_context(drbg);
    result = mbedtls_ctr_drbg_random(
        context, (unsigned char *)output, requested_length);
    if (result != 0) {
        if (drbg->state != HFOC_MBEDTLS_DRBG_STATE_FAILED) {
            hfoc_mbedtls_drbg_fail(
                drbg,
                HFOC_MBEDTLS_DRBG_FAULT_RANDOM_FAILED);
            return HFOC_MBEDTLS_DRBG_RANDOM_FAILED;
        }
        if (drbg->fault ==
            HFOC_MBEDTLS_DRBG_FAULT_GENERATION_MISMATCH) {
            return HFOC_MBEDTLS_DRBG_GENERATION_MISMATCH;
        }
        if (drbg->fault ==
            HFOC_MBEDTLS_DRBG_FAULT_SOURCE_MISMATCH) {
            return HFOC_MBEDTLS_DRBG_SOURCE_MISMATCH;
        }
        return HFOC_MBEDTLS_DRBG_ENTROPY_REJECTED;
    }

    ++drbg->random_calls;
    *bytes_written = requested_length;
    return HFOC_MBEDTLS_DRBG_OK;
}

enum hfoc_mbedtls_drbg_status hfoc_mbedtls_drbg_invalidate(
    struct hfoc_mbedtls_drbg *drbg,
    uint32_t generation)
{
    if (drbg == NULL) {
        return HFOC_MBEDTLS_DRBG_NULL;
    }
    if (!hfoc_mbedtls_drbg_object_valid(drbg)) {
        return HFOC_MBEDTLS_DRBG_BAD_STATE;
    }
    if (!hfoc_mbedtls_drbg_generation_valid(generation)) {
        return HFOC_MBEDTLS_DRBG_BAD_GENERATION;
    }
    if (generation == drbg->generation) {
        return HFOC_MBEDTLS_DRBG_OK;
    }
    hfoc_mbedtls_drbg_fail(
        drbg,
        HFOC_MBEDTLS_DRBG_FAULT_GENERATION_MISMATCH);
    return HFOC_MBEDTLS_DRBG_GENERATION_MISMATCH;
}

void hfoc_mbedtls_drbg_shutdown(struct hfoc_mbedtls_drbg *drbg)
{
    if (drbg == NULL) {
        return;
    }
    if (drbg->context_initialized) {
        mbedtls_ctr_drbg_free(
            hfoc_mbedtls_drbg_context(drbg));
        drbg->context_initialized = false;
    }
    hfoc_mbedtls_drbg_zeroize(
        drbg->context_storage,
        sizeof(drbg->context_storage));
    memset(drbg, 0, sizeof(*drbg));
}

bool hfoc_mbedtls_drbg_initialized(
    const struct hfoc_mbedtls_drbg *drbg)
{
    return hfoc_mbedtls_drbg_object_valid(drbg);
}

bool hfoc_mbedtls_drbg_published(
    const struct hfoc_mbedtls_drbg *drbg)
{
    return hfoc_mbedtls_drbg_object_valid(drbg) &&
           drbg->drbg_published;
}

enum hfoc_mbedtls_drbg_state hfoc_mbedtls_drbg_state(
    const struct hfoc_mbedtls_drbg *drbg)
{
    if (!hfoc_mbedtls_drbg_object_valid(drbg)) {
        return HFOC_MBEDTLS_DRBG_STATE_UNINITIALIZED;
    }
    return drbg->state;
}

enum hfoc_mbedtls_drbg_fault hfoc_mbedtls_drbg_fault(
    const struct hfoc_mbedtls_drbg *drbg)
{
    if (!hfoc_mbedtls_drbg_object_valid(drbg)) {
        return HFOC_MBEDTLS_DRBG_FAULT_NONE;
    }
    return drbg->fault;
}

uint32_t hfoc_mbedtls_drbg_purpose_id(
    const struct hfoc_mbedtls_drbg *drbg)
{
    if (!hfoc_mbedtls_drbg_object_valid(drbg)) {
        return 0U;
    }
    return drbg->purpose_id;
}

uint32_t hfoc_mbedtls_drbg_generation(
    const struct hfoc_mbedtls_drbg *drbg)
{
    if (!hfoc_mbedtls_drbg_object_valid(drbg)) {
        return 0U;
    }
    return drbg->generation;
}

uint32_t hfoc_mbedtls_drbg_source_id(
    const struct hfoc_mbedtls_drbg *drbg)
{
    if (!hfoc_mbedtls_drbg_object_valid(drbg)) {
        return 0U;
    }
    return drbg->source_id;
}

uint32_t hfoc_mbedtls_drbg_fill_count(
    const struct hfoc_mbedtls_drbg *drbg)
{
    if (!hfoc_mbedtls_drbg_object_valid(drbg) ||
        !entropy_seed_fill_initialized(&drbg->seed_fill)) {
        return 0U;
    }
    return entropy_seed_fill_fill_count(&drbg->seed_fill);
}

uint32_t hfoc_mbedtls_drbg_random_calls(
    const struct hfoc_mbedtls_drbg *drbg)
{
    if (!hfoc_mbedtls_drbg_object_valid(drbg)) {
        return 0U;
    }
    return drbg->random_calls;
}

const char *hfoc_mbedtls_drbg_status_string(
    enum hfoc_mbedtls_drbg_status status)
{
    switch (status) {
    case HFOC_MBEDTLS_DRBG_OK:
        return "HFOC_MBEDTLS_DRBG_OK";
    case HFOC_MBEDTLS_DRBG_NULL:
        return "HFOC_MBEDTLS_DRBG_NULL";
    case HFOC_MBEDTLS_DRBG_BAD_STATE:
        return "HFOC_MBEDTLS_DRBG_BAD_STATE";
    case HFOC_MBEDTLS_DRBG_ALREADY_PUBLISHED:
        return "HFOC_MBEDTLS_DRBG_ALREADY_PUBLISHED";
    case HFOC_MBEDTLS_DRBG_BAD_VERSION:
        return "HFOC_MBEDTLS_DRBG_BAD_VERSION";
    case HFOC_MBEDTLS_DRBG_BAD_PURPOSE:
        return "HFOC_MBEDTLS_DRBG_BAD_PURPOSE";
    case HFOC_MBEDTLS_DRBG_BAD_GENERATION:
        return "HFOC_MBEDTLS_DRBG_BAD_GENERATION";
    case HFOC_MBEDTLS_DRBG_BAD_SOURCE_ID:
        return "HFOC_MBEDTLS_DRBG_BAD_SOURCE_ID";
    case HFOC_MBEDTLS_DRBG_BAD_REQUIRED_BYTES:
        return "HFOC_MBEDTLS_DRBG_BAD_REQUIRED_BYTES";
    case HFOC_MBEDTLS_DRBG_BAD_FILL_COUNT:
        return "HFOC_MBEDTLS_DRBG_BAD_FILL_COUNT";
    case HFOC_MBEDTLS_DRBG_BAD_RESEED_INTERVAL:
        return "HFOC_MBEDTLS_DRBG_BAD_RESEED_INTERVAL";
    case HFOC_MBEDTLS_DRBG_ENTROPY_UNAVAILABLE:
        return "HFOC_MBEDTLS_DRBG_ENTROPY_UNAVAILABLE";
    case HFOC_MBEDTLS_DRBG_ENTROPY_DEGRADED:
        return "HFOC_MBEDTLS_DRBG_ENTROPY_DEGRADED";
    case HFOC_MBEDTLS_DRBG_ENTROPY_REJECTED:
        return "HFOC_MBEDTLS_DRBG_ENTROPY_REJECTED";
    case HFOC_MBEDTLS_DRBG_GENERATION_MISMATCH:
        return "HFOC_MBEDTLS_DRBG_GENERATION_MISMATCH";
    case HFOC_MBEDTLS_DRBG_SOURCE_MISMATCH:
        return "HFOC_MBEDTLS_DRBG_SOURCE_MISMATCH";
    case HFOC_MBEDTLS_DRBG_SEED_FAILED:
        return "HFOC_MBEDTLS_DRBG_SEED_FAILED";
    case HFOC_MBEDTLS_DRBG_RESEED_FAILED:
        return "HFOC_MBEDTLS_DRBG_RESEED_FAILED";
    case HFOC_MBEDTLS_DRBG_RANDOM_FAILED:
        return "HFOC_MBEDTLS_DRBG_RANDOM_FAILED";
    case HFOC_MBEDTLS_DRBG_LENGTH_OVERFLOW:
        return "HFOC_MBEDTLS_DRBG_LENGTH_OVERFLOW";
    case HFOC_MBEDTLS_DRBG_OUTPUT_TOO_SMALL:
        return "HFOC_MBEDTLS_DRBG_OUTPUT_TOO_SMALL";
    case HFOC_MBEDTLS_DRBG_OUTPUT_OVERLAP:
        return "HFOC_MBEDTLS_DRBG_OUTPUT_OVERLAP";
    case HFOC_MBEDTLS_DRBG_NOT_PUBLISHED:
        return "HFOC_MBEDTLS_DRBG_NOT_PUBLISHED";
    case HFOC_MBEDTLS_DRBG_CONTEXT_STORAGE_OVERFLOW:
        return "HFOC_MBEDTLS_DRBG_CONTEXT_STORAGE_OVERFLOW";
    default:
        return NULL;
    }
}

const char *hfoc_mbedtls_drbg_state_string(
    enum hfoc_mbedtls_drbg_state state)
{
    switch (state) {
    case HFOC_MBEDTLS_DRBG_STATE_UNINITIALIZED:
        return "HFOC_MBEDTLS_DRBG_STATE_UNINITIALIZED";
    case HFOC_MBEDTLS_DRBG_STATE_UNAVAILABLE:
        return "HFOC_MBEDTLS_DRBG_STATE_UNAVAILABLE";
    case HFOC_MBEDTLS_DRBG_STATE_DEGRADED:
        return "HFOC_MBEDTLS_DRBG_STATE_DEGRADED";
    case HFOC_MBEDTLS_DRBG_STATE_READY:
        return "HFOC_MBEDTLS_DRBG_STATE_READY";
    case HFOC_MBEDTLS_DRBG_STATE_PUBLISHED:
        return "HFOC_MBEDTLS_DRBG_STATE_PUBLISHED";
    case HFOC_MBEDTLS_DRBG_STATE_FAILED:
        return "HFOC_MBEDTLS_DRBG_STATE_FAILED";
    default:
        return NULL;
    }
}

const char *hfoc_mbedtls_drbg_fault_string(
    enum hfoc_mbedtls_drbg_fault fault)
{
    switch (fault) {
    case HFOC_MBEDTLS_DRBG_FAULT_NONE:
        return "HFOC_MBEDTLS_DRBG_FAULT_NONE";
    case HFOC_MBEDTLS_DRBG_FAULT_ENTROPY_UNAVAILABLE:
        return "HFOC_MBEDTLS_DRBG_FAULT_ENTROPY_UNAVAILABLE";
    case HFOC_MBEDTLS_DRBG_FAULT_ENTROPY_DEGRADED:
        return "HFOC_MBEDTLS_DRBG_FAULT_ENTROPY_DEGRADED";
    case HFOC_MBEDTLS_DRBG_FAULT_ENTROPY_REJECTED:
        return "HFOC_MBEDTLS_DRBG_FAULT_ENTROPY_REJECTED";
    case HFOC_MBEDTLS_DRBG_FAULT_GENERATION_MISMATCH:
        return "HFOC_MBEDTLS_DRBG_FAULT_GENERATION_MISMATCH";
    case HFOC_MBEDTLS_DRBG_FAULT_SOURCE_MISMATCH:
        return "HFOC_MBEDTLS_DRBG_FAULT_SOURCE_MISMATCH";
    case HFOC_MBEDTLS_DRBG_FAULT_SEED_FAILED:
        return "HFOC_MBEDTLS_DRBG_FAULT_SEED_FAILED";
    case HFOC_MBEDTLS_DRBG_FAULT_RESEED_FAILED:
        return "HFOC_MBEDTLS_DRBG_FAULT_RESEED_FAILED";
    case HFOC_MBEDTLS_DRBG_FAULT_RANDOM_FAILED:
        return "HFOC_MBEDTLS_DRBG_FAULT_RANDOM_FAILED";
    case HFOC_MBEDTLS_DRBG_FAULT_OUTPUT_OVERLAP:
        return "HFOC_MBEDTLS_DRBG_FAULT_OUTPUT_OVERLAP";
    case HFOC_MBEDTLS_DRBG_FAULT_LENGTH_OVERFLOW:
        return "HFOC_MBEDTLS_DRBG_FAULT_LENGTH_OVERFLOW";
    case HFOC_MBEDTLS_DRBG_FAULT_PURPOSE_MISMATCH:
        return "HFOC_MBEDTLS_DRBG_FAULT_PURPOSE_MISMATCH";
    case HFOC_MBEDTLS_DRBG_FAULT_CONTEXT_STORAGE_OVERFLOW:
        return "HFOC_MBEDTLS_DRBG_FAULT_CONTEXT_STORAGE_OVERFLOW";
    case HFOC_MBEDTLS_DRBG_FAULT_CONTEXT_NOT_PUBLISHED:
        return "HFOC_MBEDTLS_DRBG_FAULT_CONTEXT_NOT_PUBLISHED";
    default:
        return NULL;
    }
}

#else

enum {
    HFOC_MBEDTLS_DRBG_PRODUCTION_BOUNDARY_UNUSED = 0
};

#endif
