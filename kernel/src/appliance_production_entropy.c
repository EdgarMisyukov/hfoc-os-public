#include "appliance_production_entropy.h"

#ifdef HFOC_PRODUCTION_TLS

#include "entropy_accumulator.h"
#include "entropy_health.h"
#include "entropy_seed_fill.h"
#include "hfoc_mbedtls_alloc.h"
#include "hfoc_mbedtls_drbg.h"
#include "appliance_host_seed.h"

#include "psa/crypto.h"
#include "psa/crypto_extra.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

struct appliance_production_entropy {
    bool initialized;
    bool crypto_published;
    uint32_t version;
    enum appliance_production_entropy_state state;
    enum appliance_production_entropy_source source;
    /* ITEM-581: the CPUID facts the source selection read, and the stage the
       first failure stopped at. */
    uint32_t cpu_max_leaf;
    bool cpu_rdrand;
    bool cpu_rdseed;
    enum appliance_production_entropy_stage failure_stage;
    uint32_t generation;
    uint32_t random_calls;
    uint32_t rotations;
    struct entropy_accumulator accumulator;
    struct entropy_health health;
    struct entropy_seed_fill seed_fill;
    struct hfoc_mbedtls_drbg drbg;
};

static struct appliance_production_entropy appliance_production_entropy_owner;

/* ------------------------------------------------------------------ CPUID */

static void appliance_production_entropy_cpuid(
    uint32_t leaf,
    uint32_t subleaf,
    uint32_t *eax,
    uint32_t *ebx,
    uint32_t *ecx,
    uint32_t *edx)
{
    uint32_t value_a;
    uint32_t value_b;
    uint32_t value_c;
    uint32_t value_d;

    __asm__ volatile (
        "cpuid\n"
        : "=a"(value_a), "=b"(value_b), "=c"(value_c), "=d"(value_d)
        : "a"(leaf), "c"(subleaf)
        : "memory");
    if (eax != NULL) {
        *eax = value_a;
    }
    if (ebx != NULL) {
        *ebx = value_b;
    }
    if (ecx != NULL) {
        *ecx = value_c;
    }
    if (edx != NULL) {
        *edx = value_d;
    }
}

/*
 * CPUID leaf 7 sub-leaf 0, EBX bit 18 is RDSEED; CPUID leaf 1, ECX bit 30 is
 * RDRAND.  Both are read from the running CPU, so a machine that does not
 * advertise them is never asked to execute them and the module reports
 * UNAVAILABLE instead of fabricating entropy.
 */
static enum appliance_production_entropy_source
appliance_production_entropy_detect(void)
{
    struct appliance_production_entropy *entropy =
        &appliance_production_entropy_owner;
    uint32_t max_leaf = 0U;
    uint32_t ebx = 0U;
    uint32_t ecx = 0U;
    bool rdrand = false;
    bool rdseed = false;

    appliance_production_entropy_cpuid(0U, 0U, &max_leaf, NULL, NULL, NULL);
    if (max_leaf >= 7U) {
        appliance_production_entropy_cpuid(7U, 0U, NULL, &ebx, NULL, NULL);
        rdseed = (ebx & (UINT32_C(1) << 18U)) != 0U;
    }
    if (max_leaf >= 1U) {
        appliance_production_entropy_cpuid(1U, 0U, NULL, NULL, &ecx, NULL);
        rdrand = (ecx & (UINT32_C(1) << 30U)) != 0U;
    }
    entropy->cpu_max_leaf = max_leaf;
    entropy->cpu_rdrand = rdrand;
    entropy->cpu_rdseed = rdseed;
    return appliance_production_entropy_source_for_cpuid(max_leaf, rdrand,
                                                         rdseed);
}

static bool appliance_production_entropy_rdseed(uint64_t *value)
{
    uint64_t result = 0U;
    unsigned char carried = 0U;

    __asm__ volatile (
        "rdseed %0\n"
        "setc %1\n"
        : "=r"(result), "=qm"(carried)
        :
        : "cc");
    if (carried == 0U) {
        return false;
    }
    *value = result;
    return true;
}

static bool appliance_production_entropy_rdrand(uint64_t *value)
{
    uint64_t result = 0U;
    unsigned char carried = 0U;

    __asm__ volatile (
        "rdrand %0\n"
        "setc %1\n"
        : "=r"(result), "=qm"(carried)
        :
        : "cc");
    if (carried == 0U) {
        return false;
    }
    *value = result;
    return true;
}

/* --------------------------------------------------------------- entropy */

static bool appliance_production_entropy_read_word(
    enum appliance_production_entropy_source source,
    uint64_t *value)
{
    uint32_t attempt;

    for (attempt = 0U;
         attempt < APPLIANCE_PRODUCTION_ENTROPY_INSTRUCTION_RETRY;
         ++attempt) {
        if (source == APPLIANCE_PRODUCTION_ENTROPY_SOURCE_RDSEED) {
            if (appliance_production_entropy_rdseed(value)) {
                return true;
            }
        } else if (source ==
                   APPLIANCE_PRODUCTION_ENTROPY_SOURCE_RDRAND) {
            if (appliance_production_entropy_rdrand(value)) {
                return true;
            }
        } else {
            return false;
        }
    }
    return false;
}

static void appliance_production_entropy_zeroize(
    void *buffer,
    size_t length)
{
    volatile uint8_t *bytes = (volatile uint8_t *)buffer;
    size_t index;

    for (index = 0U; index < length; ++index) {
        bytes[index] = 0U;
    }
}

/*
 * Mix one bounded hardware block of
 * APPLIANCE_PRODUCTION_ENTROPY_BLOCK_COUNT *
 * APPLIANCE_PRODUCTION_ENTROPY_BLOCK_BYTES bytes and publish the seed
 * controller and the CTR-DRBG.  Every contract call is checked: a rejected
 * block, a health fault or a refused seed leaves the module DEGRADED or
 * FAILED instead of quietly continuing with less material.
 */
static bool appliance_production_entropy_publish_pool(uint32_t now_ms)
{
    struct appliance_production_entropy *entropy =
        &appliance_production_entropy_owner;
    struct entropy_accumulator_config accumulator_config;
    struct entropy_health_config health_config;
    struct entropy_seed_fill_config seed_config;
    struct hfoc_mbedtls_drbg_config drbg_config;
    uint32_t block;

    memset(&accumulator_config, 0, sizeof(accumulator_config));
    accumulator_config.version = ENTROPY_ACCUMULATOR_VERSION;
    accumulator_config.generation = entropy->generation;
    accumulator_config.source_id = APPLIANCE_PRODUCTION_ENTROPY_SOURCE_ID;
    accumulator_config.block_length_min =
        APPLIANCE_PRODUCTION_ENTROPY_BLOCK_BYTES;
    accumulator_config.block_length_max =
        APPLIANCE_PRODUCTION_ENTROPY_BLOCK_BYTES;
    /*
     * The whole pool is mixed in one bounded batch, so the mix-stall watchdog
     * cannot elapse inside it; the maximum keeps a future batched caller from
     * tripping the watchdog on a legitimate gap.  Source liveness is observed
     * by the health contract below, and the serving path re-reads the module
     * state before every handshake.
     */
    accumulator_config.max_stall_ticks =
        ENTROPY_ACCUMULATOR_STALL_LIMIT_MAX;
    if (entropy_accumulator_init(
            &entropy->accumulator, &accumulator_config, now_ms) !=
        ENTROPY_ACCUMULATOR_OK) {
        entropy->failure_stage = APPLIANCE_PRODUCTION_ENTROPY_STAGE_ACCUMULATOR;
        return false;
    }

    memset(&health_config, 0, sizeof(health_config));
    health_config.version = ENTROPY_HEALTH_VERSION;
    health_config.block_length_min =
        APPLIANCE_PRODUCTION_ENTROPY_BLOCK_BYTES;
    health_config.block_length_max =
        APPLIANCE_PRODUCTION_ENTROPY_BLOCK_BYTES;
    health_config.max_stall_ticks = ENTROPY_HEALTH_STALL_LIMIT_MAX;
    if (entropy_health_init(
            &entropy->health,
            &health_config,
            entropy->generation,
            now_ms) != ENTROPY_HEALTH_OK) {
        entropy->failure_stage = APPLIANCE_PRODUCTION_ENTROPY_STAGE_HEALTH;
        return false;
    }

    for (block = 0U;
         block < APPLIANCE_PRODUCTION_ENTROPY_BLOCK_COUNT;
         ++block) {
        uint8_t material[APPLIANCE_PRODUCTION_ENTROPY_BLOCK_BYTES];
        uint32_t word_index;

        for (word_index = 0U; word_index < 4U; ++word_index) {
            uint64_t word = 0U;

            if (!appliance_production_entropy_read_word(
                    entropy->source, &word)) {
                appliance_production_entropy_zeroize(
                    material, sizeof(material));
                entropy->failure_stage =
                    APPLIANCE_PRODUCTION_ENTROPY_STAGE_INSTRUCTION;
                return false;
            }
            memcpy(
                &material[word_index * sizeof(word)],
                &word,
                sizeof(word));
        }
        if (entropy_health_observe(
                &entropy->health,
                entropy->generation,
                material,
                (uint32_t)sizeof(material),
                now_ms) != ENTROPY_HEALTH_OK) {
            appliance_production_entropy_zeroize(
                material, sizeof(material));
            entropy->failure_stage = APPLIANCE_PRODUCTION_ENTROPY_STAGE_HEALTH;
            return false;
        }
        if (entropy_accumulator_mix(
                &entropy->accumulator,
                entropy->generation,
                APPLIANCE_PRODUCTION_ENTROPY_SOURCE_ID,
                material,
                (uint32_t)sizeof(material),
                now_ms) != ENTROPY_ACCUMULATOR_OK) {
            appliance_production_entropy_zeroize(
                material, sizeof(material));
            return false;
        }
        appliance_production_entropy_zeroize(
            material, sizeof(material));
    }

    memset(&seed_config, 0, sizeof(seed_config));
    seed_config.version = ENTROPY_SEED_FILL_VERSION;
    seed_config.generation = entropy->generation;
    seed_config.source_id = APPLIANCE_PRODUCTION_ENTROPY_SOURCE_ID;
    seed_config.required_credited_bytes =
        APPLIANCE_PRODUCTION_ENTROPY_REQUIRED_CREDITED_BYTES;
    seed_config.seed_length =
        APPLIANCE_PRODUCTION_ENTROPY_SEED_LENGTH;
    seed_config.fill_length_max =
        APPLIANCE_PRODUCTION_ENTROPY_FILL_LENGTH_MAX;
    seed_config.fill_count_max =
        APPLIANCE_PRODUCTION_ENTROPY_FILL_COUNT_MAX;
    if (entropy_seed_fill_init(
            &entropy->seed_fill, &seed_config,
            &entropy->accumulator) != ENTROPY_SEED_FILL_OK) {
        return false;
    }
    if (!entropy_seed_fill_ready(&entropy->seed_fill)) {
        return false;
    }

    memset(&drbg_config, 0, sizeof(drbg_config));
    drbg_config.version = HFOC_MBEDTLS_DRBG_VERSION;
    drbg_config.purpose_id = HFOC_MBEDTLS_DRBG_PURPOSE_TLS_HANDSHAKE;
    drbg_config.generation = entropy->generation;
    drbg_config.source_id = APPLIANCE_PRODUCTION_ENTROPY_SOURCE_ID;
    drbg_config.required_credited_bytes =
        APPLIANCE_PRODUCTION_ENTROPY_REQUIRED_CREDITED_BYTES;
    drbg_config.fill_count_max =
        APPLIANCE_PRODUCTION_ENTROPY_FILL_COUNT_MAX;
    drbg_config.reseed_interval =
        APPLIANCE_PRODUCTION_ENTROPY_RESEED_INTERVAL;
    if (hfoc_mbedtls_drbg_init(
            &entropy->drbg, &drbg_config,
            &entropy->accumulator) != HFOC_MBEDTLS_DRBG_OK) {
        entropy->failure_stage = APPLIANCE_PRODUCTION_ENTROPY_STAGE_DRBG;
        return false;
    }
    if (hfoc_mbedtls_drbg_publish(
            &entropy->drbg, &entropy->accumulator) !=
        HFOC_MBEDTLS_DRBG_OK) {
        entropy->failure_stage = APPLIANCE_PRODUCTION_ENTROPY_STAGE_SEED;
        return false;
    }
    if (!hfoc_mbedtls_drbg_published(&entropy->drbg)) {
        return false;
    }
    /*
     * The production TLS profile runs mbedTLS with MBEDTLS_USE_PSA_CRYPTO, so
     * the PSA subsystem must be initialized before any key generation,
     * certificate signing or handshake key exchange happens.  The only PSA
     * entropy source in this image is
     * mbedtls_psa_external_get_random() below, which reads the CTR-DRBG this
     * function just published, so a failed initialization is a hard failure
     * and never a silent fallback.
     */
    if (!hfoc_mbedtls_alloc_ready() && !hfoc_mbedtls_alloc_init()) {
        entropy->failure_stage = APPLIANCE_PRODUCTION_ENTROPY_STAGE_PSA;
        return false;
    }
    if (psa_crypto_init() != PSA_SUCCESS) {
        entropy->failure_stage = APPLIANCE_PRODUCTION_ENTROPY_STAGE_PSA;
        return false;
    }
    entropy->crypto_published = true;
    entropy->failure_stage = APPLIANCE_PRODUCTION_ENTROPY_STAGE_NONE;
    return true;
}

/*
 * Publish an externally seeded external block through the same chain as a
 * hardware block.  OPERATOR material is the explicit recovery input;
 * HOST_PROVISIONED material is the durable host seed expanded for this boot.
 * The password is never used as entropy, and every byte still passes the
 * accumulator, health, seed-fill and DRBG owners.
 */
static bool appliance_production_entropy_publish_external(
    enum appliance_production_entropy_source source,
    uint32_t now_ms,
    const uint8_t *seed,
    size_t length)
{
    struct appliance_production_entropy *entropy =
        &appliance_production_entropy_owner;
    struct entropy_accumulator_config accumulator_config;
    struct entropy_health_config health_config;
    struct entropy_seed_fill_config seed_config;
    struct hfoc_mbedtls_drbg_config drbg_config;
    uint8_t material[APPLIANCE_PRODUCTION_ENTROPY_OPERATOR_SEED_BYTES];

    if (seed == NULL ||
        (source != APPLIANCE_PRODUCTION_ENTROPY_SOURCE_OPERATOR &&
         source !=
             APPLIANCE_PRODUCTION_ENTROPY_SOURCE_HOST_PROVISIONED) ||
        length != APPLIANCE_PRODUCTION_ENTROPY_OPERATOR_SEED_BYTES) {
        entropy->failure_stage = APPLIANCE_PRODUCTION_ENTROPY_STAGE_DETECT;
        return false;
    }
    memcpy(material, seed, length);
    hfoc_mbedtls_drbg_shutdown(&entropy->drbg);
    memset(&entropy->accumulator, 0, sizeof(entropy->accumulator));
    memset(&entropy->health, 0, sizeof(entropy->health));
    memset(&entropy->seed_fill, 0, sizeof(entropy->seed_fill));
    memset(&entropy->drbg, 0, sizeof(entropy->drbg));
    if (entropy->generation >=
        APPLIANCE_PRODUCTION_ENTROPY_GENERATION_MAX) {
        entropy->generation =
            APPLIANCE_PRODUCTION_ENTROPY_GENERATION_MIN;
    } else {
        ++entropy->generation;
    }
    entropy->source = source;
    entropy->failure_stage = APPLIANCE_PRODUCTION_ENTROPY_STAGE_NONE;

    memset(&accumulator_config, 0, sizeof(accumulator_config));
    accumulator_config.version = ENTROPY_ACCUMULATOR_VERSION;
    accumulator_config.generation = entropy->generation;
    accumulator_config.source_id = APPLIANCE_PRODUCTION_ENTROPY_SOURCE_ID;
    accumulator_config.block_length_min =
        APPLIANCE_PRODUCTION_ENTROPY_OPERATOR_SEED_BYTES;
    accumulator_config.block_length_max =
        APPLIANCE_PRODUCTION_ENTROPY_OPERATOR_SEED_BYTES;
    accumulator_config.max_stall_ticks =
        ENTROPY_ACCUMULATOR_STALL_LIMIT_MAX;
    if (entropy_accumulator_init(
            &entropy->accumulator, &accumulator_config, now_ms) !=
        ENTROPY_ACCUMULATOR_OK) {
        appliance_production_entropy_zeroize(material, sizeof(material));
        entropy->failure_stage = APPLIANCE_PRODUCTION_ENTROPY_STAGE_ACCUMULATOR;
        return false;
    }

    memset(&health_config, 0, sizeof(health_config));
    health_config.version = ENTROPY_HEALTH_VERSION;
    health_config.block_length_min =
        APPLIANCE_PRODUCTION_ENTROPY_OPERATOR_SEED_BYTES;
    health_config.block_length_max =
        APPLIANCE_PRODUCTION_ENTROPY_OPERATOR_SEED_BYTES;
    health_config.max_stall_ticks = ENTROPY_HEALTH_STALL_LIMIT_MAX;
    if (entropy_health_init(
            &entropy->health,
            &health_config,
            entropy->generation,
            now_ms) != ENTROPY_HEALTH_OK) {
        appliance_production_entropy_zeroize(material, sizeof(material));
        entropy->failure_stage = APPLIANCE_PRODUCTION_ENTROPY_STAGE_HEALTH;
        return false;
    }
    if (entropy_health_observe(
            &entropy->health,
            entropy->generation,
            material,
            (uint32_t)sizeof(material),
            now_ms) != ENTROPY_HEALTH_OK) {
        appliance_production_entropy_zeroize(material, sizeof(material));
        entropy->failure_stage = APPLIANCE_PRODUCTION_ENTROPY_STAGE_HEALTH;
        return false;
    }
    if (entropy_accumulator_mix(
            &entropy->accumulator,
            entropy->generation,
            APPLIANCE_PRODUCTION_ENTROPY_SOURCE_ID,
            material,
            (uint32_t)sizeof(material),
            now_ms) != ENTROPY_ACCUMULATOR_OK) {
        appliance_production_entropy_zeroize(material, sizeof(material));
        entropy->failure_stage = APPLIANCE_PRODUCTION_ENTROPY_STAGE_ACCUMULATOR;
        return false;
    }
    appliance_production_entropy_zeroize(material, sizeof(material));

    memset(&seed_config, 0, sizeof(seed_config));
    seed_config.version = ENTROPY_SEED_FILL_VERSION;
    seed_config.generation = entropy->generation;
    seed_config.source_id = APPLIANCE_PRODUCTION_ENTROPY_SOURCE_ID;
    seed_config.required_credited_bytes =
        APPLIANCE_PRODUCTION_ENTROPY_OPERATOR_SEED_BYTES;
    seed_config.seed_length = APPLIANCE_PRODUCTION_ENTROPY_SEED_LENGTH;
    seed_config.fill_length_max =
        APPLIANCE_PRODUCTION_ENTROPY_FILL_LENGTH_MAX;
    seed_config.fill_count_max =
        APPLIANCE_PRODUCTION_ENTROPY_FILL_COUNT_MAX;
    if (entropy_seed_fill_init(
            &entropy->seed_fill, &seed_config,
            &entropy->accumulator) != ENTROPY_SEED_FILL_OK) {
        entropy->failure_stage = APPLIANCE_PRODUCTION_ENTROPY_STAGE_SEED;
        return false;
    }
    if (!entropy_seed_fill_ready(&entropy->seed_fill)) {
        entropy->failure_stage = APPLIANCE_PRODUCTION_ENTROPY_STAGE_SEED;
        return false;
    }

    memset(&drbg_config, 0, sizeof(drbg_config));
    drbg_config.version = HFOC_MBEDTLS_DRBG_VERSION;
    drbg_config.purpose_id = HFOC_MBEDTLS_DRBG_PURPOSE_TLS_HANDSHAKE;
    drbg_config.generation = entropy->generation;
    drbg_config.source_id = APPLIANCE_PRODUCTION_ENTROPY_SOURCE_ID;
    drbg_config.required_credited_bytes =
        APPLIANCE_PRODUCTION_ENTROPY_OPERATOR_SEED_BYTES;
    drbg_config.fill_count_max =
        APPLIANCE_PRODUCTION_ENTROPY_FILL_COUNT_MAX;
    drbg_config.reseed_interval =
        APPLIANCE_PRODUCTION_ENTROPY_RESEED_INTERVAL;
    if (hfoc_mbedtls_drbg_init(
            &entropy->drbg, &drbg_config,
            &entropy->accumulator) != HFOC_MBEDTLS_DRBG_OK) {
        entropy->failure_stage = APPLIANCE_PRODUCTION_ENTROPY_STAGE_DRBG;
        return false;
    }
    if (hfoc_mbedtls_drbg_publish(
            &entropy->drbg, &entropy->accumulator) !=
        HFOC_MBEDTLS_DRBG_OK) {
        entropy->failure_stage = APPLIANCE_PRODUCTION_ENTROPY_STAGE_SEED;
        return false;
    }
    if (!hfoc_mbedtls_drbg_published(&entropy->drbg)) {
        entropy->failure_stage = APPLIANCE_PRODUCTION_ENTROPY_STAGE_SEED;
        return false;
    }
    if (!hfoc_mbedtls_alloc_ready() && !hfoc_mbedtls_alloc_init()) {
        entropy->failure_stage = APPLIANCE_PRODUCTION_ENTROPY_STAGE_PSA;
        return false;
    }
    if (psa_crypto_init() != PSA_SUCCESS) {
        entropy->failure_stage = APPLIANCE_PRODUCTION_ENTROPY_STAGE_PSA;
        return false;
    }
    entropy->crypto_published = true;
    entropy->failure_stage = APPLIANCE_PRODUCTION_ENTROPY_STAGE_NONE;
    entropy->state = APPLIANCE_PRODUCTION_ENTROPY_STATE_READY;
    return true;
}

/*
 * Rotate the whole entropy chain onto the next generation.  The previous
 * CTR-DRBG is shut down first so the mbedTLS arena is released, and every
 * structure is zeroed before it is re-initialized, so a rotation can never
 * accumulate state.  The rotation counter is bounded; the caller fails closed
 * once the bound is reached.
 */
static bool appliance_production_entropy_rotate(uint32_t now_ms)
{
    struct appliance_production_entropy *entropy =
        &appliance_production_entropy_owner;

    /*
     * An operator or host-provisioned seed starts one bounded pool.  Rotating
     * would mix bytes derived from the same long-term seed and falsely count
     * them as newly observed entropy, so exhaustion fails closed until reboot
     * unlocks the next durable epoch instead.
     */
    if (entropy->source == APPLIANCE_PRODUCTION_ENTROPY_SOURCE_OPERATOR ||
        entropy->source ==
            APPLIANCE_PRODUCTION_ENTROPY_SOURCE_HOST_PROVISIONED) {
        return false;
    }
    if (entropy->rotations >=
        APPLIANCE_PRODUCTION_ENTROPY_ROTATION_MAX) {
        return false;
    }
    hfoc_mbedtls_drbg_shutdown(&entropy->drbg);
    memset(&entropy->drbg, 0, sizeof(entropy->drbg));
    memset(&entropy->seed_fill, 0, sizeof(entropy->seed_fill));
    memset(&entropy->accumulator, 0, sizeof(entropy->accumulator));
    memset(&entropy->health, 0, sizeof(entropy->health));
    entropy->generation =
        entropy->generation >=
                APPLIANCE_PRODUCTION_ENTROPY_GENERATION_MAX
            ? APPLIANCE_PRODUCTION_ENTROPY_GENERATION_MIN
            : entropy->generation + 1U;
    ++entropy->rotations;
    if (!appliance_production_entropy_publish_pool(now_ms)) {
        return false;
    }
    entropy->state = APPLIANCE_PRODUCTION_ENTROPY_STATE_READY;
    return true;
}

bool appliance_production_entropy_init(uint32_t now_ms)
{
    struct appliance_production_entropy *entropy =
        &appliance_production_entropy_owner;

    if (entropy->initialized) {
        return true;
    }
    memset(entropy, 0, sizeof(*entropy));
    entropy->version = APPLIANCE_PRODUCTION_ENTROPY_VERSION;
    entropy->generation = APPLIANCE_PRODUCTION_ENTROPY_GENERATION_MIN;
    entropy->initialized = true;
    entropy->source = appliance_production_entropy_detect();
    if (entropy->source == APPLIANCE_PRODUCTION_ENTROPY_SOURCE_NONE) {
        /*
         * Fail closed and say so.  The honest answer on a CPU with no
         * hardware generator is that the appliance has no entropy owner, and
         * the serving path must keep the listener down and the console up.
         */
        entropy->state = APPLIANCE_PRODUCTION_ENTROPY_STATE_UNAVAILABLE;
        entropy->failure_stage = APPLIANCE_PRODUCTION_ENTROPY_STAGE_DETECT;
        return true;
    }
    if (!appliance_production_entropy_publish_pool(now_ms)) {
        entropy->state = APPLIANCE_PRODUCTION_ENTROPY_STATE_FAILED;
        return true;
    }
    entropy->state = APPLIANCE_PRODUCTION_ENTROPY_STATE_READY;
    return true;
}

enum appliance_production_entropy_seed_status
appliance_production_entropy_seed_operator(
    const uint8_t *seed,
    size_t length,
    uint32_t now_ms)
{
    struct appliance_production_entropy *entropy =
        &appliance_production_entropy_owner;

    if (seed == NULL) {
        return APPLIANCE_PRODUCTION_ENTROPY_SEED_NULL_ARGUMENT;
    }
    if (length != APPLIANCE_PRODUCTION_ENTROPY_OPERATOR_SEED_BYTES) {
        return APPLIANCE_PRODUCTION_ENTROPY_SEED_WRONG_LENGTH;
    }
    if (!appliance_production_entropy_operator_seed_valid(seed, length)) {
        return APPLIANCE_PRODUCTION_ENTROPY_SEED_LOW_DIVERSITY;
    }
    if (!entropy->initialized) {
        return APPLIANCE_PRODUCTION_ENTROPY_SEED_NOT_AVAILABLE;
    }
    if (entropy->state == APPLIANCE_PRODUCTION_ENTROPY_STATE_READY) {
        return APPLIANCE_PRODUCTION_ENTROPY_SEED_ALREADY_READY;
    }
    if (entropy->state != APPLIANCE_PRODUCTION_ENTROPY_STATE_UNAVAILABLE &&
        entropy->state != APPLIANCE_PRODUCTION_ENTROPY_STATE_FAILED) {
        return APPLIANCE_PRODUCTION_ENTROPY_SEED_NOT_AVAILABLE;
    }
    if (!appliance_production_entropy_publish_external(
            APPLIANCE_PRODUCTION_ENTROPY_SOURCE_OPERATOR,
            now_ms, seed, length)) {
        entropy->state = APPLIANCE_PRODUCTION_ENTROPY_STATE_FAILED;
        return APPLIANCE_PRODUCTION_ENTROPY_SEED_FAILED;
    }
    return APPLIANCE_PRODUCTION_ENTROPY_SEED_OK;
}

enum appliance_production_entropy_seed_status
appliance_production_entropy_seed_host(
    const uint8_t *seed,
    size_t length,
    uint64_t boot_epoch,
    uint32_t now_ms)
{
    struct appliance_production_entropy *entropy =
        &appliance_production_entropy_owner;
    uint8_t derived[APPLIANCE_HOST_SEED_LENGTH];
    enum appliance_host_seed_status host_status;

    if (seed == NULL) {
        return APPLIANCE_PRODUCTION_ENTROPY_SEED_NULL_ARGUMENT;
    }
    if (length != APPLIANCE_HOST_SEED_LENGTH) {
        return APPLIANCE_PRODUCTION_ENTROPY_SEED_WRONG_LENGTH;
    }
    if (!appliance_production_entropy_operator_seed_valid(seed, length) ||
        boot_epoch == 0U) {
        return APPLIANCE_PRODUCTION_ENTROPY_SEED_LOW_DIVERSITY;
    }
    if (!entropy->initialized) {
        return APPLIANCE_PRODUCTION_ENTROPY_SEED_NOT_AVAILABLE;
    }
    if (entropy->state == APPLIANCE_PRODUCTION_ENTROPY_STATE_READY) {
        return APPLIANCE_PRODUCTION_ENTROPY_SEED_ALREADY_READY;
    }
    if (entropy->source != APPLIANCE_PRODUCTION_ENTROPY_SOURCE_NONE ||
        (entropy->state != APPLIANCE_PRODUCTION_ENTROPY_STATE_UNAVAILABLE &&
         entropy->state != APPLIANCE_PRODUCTION_ENTROPY_STATE_FAILED)) {
        return APPLIANCE_PRODUCTION_ENTROPY_SEED_NOT_AVAILABLE;
    }
    memset(derived, 0, sizeof(derived));
    host_status = appliance_host_seed_derive(
        seed, boot_epoch, 0U, derived);
    if (host_status != APPLIANCE_HOST_SEED_OK) {
        memset(derived, 0, sizeof(derived));
        entropy->state = APPLIANCE_PRODUCTION_ENTROPY_STATE_FAILED;
        return APPLIANCE_PRODUCTION_ENTROPY_SEED_FAILED;
    }
    if (!appliance_production_entropy_publish_external(
            APPLIANCE_PRODUCTION_ENTROPY_SOURCE_HOST_PROVISIONED,
            now_ms, derived, sizeof(derived))) {
        memset(derived, 0, sizeof(derived));
        entropy->state = APPLIANCE_PRODUCTION_ENTROPY_STATE_FAILED;
        return APPLIANCE_PRODUCTION_ENTROPY_SEED_FAILED;
    }
    memset(derived, 0, sizeof(derived));
    return APPLIANCE_PRODUCTION_ENTROPY_SEED_OK;
}

const char *appliance_production_entropy_seed_status_string(
    enum appliance_production_entropy_seed_status status)
{
    switch (status) {
    case APPLIANCE_PRODUCTION_ENTROPY_SEED_OK:
        return "OK";
    case APPLIANCE_PRODUCTION_ENTROPY_SEED_NULL_ARGUMENT:
        return "NULL_ARGUMENT";
    case APPLIANCE_PRODUCTION_ENTROPY_SEED_WRONG_LENGTH:
        return "WRONG_LENGTH";
    case APPLIANCE_PRODUCTION_ENTROPY_SEED_LOW_DIVERSITY:
        return "LOW_DIVERSITY";
    case APPLIANCE_PRODUCTION_ENTROPY_SEED_NOT_HEX:
        return "NOT_HEX";
    case APPLIANCE_PRODUCTION_ENTROPY_SEED_ALREADY_READY:
        return "ALREADY_READY";
    case APPLIANCE_PRODUCTION_ENTROPY_SEED_NOT_AVAILABLE:
        return "NOT_AVAILABLE";
    case APPLIANCE_PRODUCTION_ENTROPY_SEED_FAILED:
        return "FAILED";
    default:
        return "UNKNOWN";
    }
}

bool appliance_production_entropy_initialized(void)
{
    return appliance_production_entropy_owner.initialized;
}

bool appliance_production_entropy_ready(void)
{
    return appliance_production_entropy_owner.initialized &&
           appliance_production_entropy_owner.state ==
               APPLIANCE_PRODUCTION_ENTROPY_STATE_READY &&
           hfoc_mbedtls_drbg_published(
               &appliance_production_entropy_owner.drbg);
}

enum appliance_production_entropy_state
appliance_production_entropy_state(void)
{
    return appliance_production_entropy_owner.state;
}

enum appliance_production_entropy_source
appliance_production_entropy_source(void)
{
    return appliance_production_entropy_owner.source;
}

uint32_t appliance_production_entropy_random_calls(void)
{
    return appliance_production_entropy_owner.random_calls;
}

uint32_t appliance_production_entropy_generation(void)
{
    return appliance_production_entropy_owner.generation;
}

uint32_t appliance_production_entropy_rotations(void)
{
    return appliance_production_entropy_owner.rotations;
}

void appliance_production_entropy_cpu_flags(uint32_t *max_leaf_out,
                                            bool *rdrand_out,
                                            bool *rdseed_out)
{
    if (max_leaf_out != NULL) {
        *max_leaf_out = appliance_production_entropy_owner.cpu_max_leaf;
    }
    if (rdrand_out != NULL) {
        *rdrand_out = appliance_production_entropy_owner.cpu_rdrand;
    }
    if (rdseed_out != NULL) {
        *rdseed_out = appliance_production_entropy_owner.cpu_rdseed;
    }
}

enum appliance_production_entropy_stage
appliance_production_entropy_failure_stage(void)
{
    return appliance_production_entropy_owner.failure_stage;
}

const char *appliance_production_entropy_stage_string(
    enum appliance_production_entropy_stage stage)
{
    switch (stage) {
    case APPLIANCE_PRODUCTION_ENTROPY_STAGE_NONE:
        return "NONE";
    case APPLIANCE_PRODUCTION_ENTROPY_STAGE_DETECT:
        return "DETECT";
    case APPLIANCE_PRODUCTION_ENTROPY_STAGE_INSTRUCTION:
        return "INSTRUCTION";
    case APPLIANCE_PRODUCTION_ENTROPY_STAGE_HEALTH:
        return "HEALTH";
    case APPLIANCE_PRODUCTION_ENTROPY_STAGE_ACCUMULATOR:
        return "ACCUMULATOR";
    case APPLIANCE_PRODUCTION_ENTROPY_STAGE_SEED:
        return "SEED";
    case APPLIANCE_PRODUCTION_ENTROPY_STAGE_DRBG:
        return "DRBG";
    case APPLIANCE_PRODUCTION_ENTROPY_STAGE_PSA:
        return "PSA";
    default:
        return "UNKNOWN";
    }
}

uint64_t appliance_production_entropy_credited_bytes(void)
{
    return entropy_accumulator_credited_bytes(
        &appliance_production_entropy_owner.accumulator);
}

int appliance_production_entropy_random(
    void *context,
    unsigned char *output,
    size_t length)
{
    struct appliance_production_entropy *entropy =
        &appliance_production_entropy_owner;
    size_t offset = 0U;

    (void)context;
    if (output == NULL || length == 0U) {
        return -1;
    }
    if (!appliance_production_entropy_ready()) {
        return -1;
    }
    while (offset < length) {
        size_t remaining = length - offset;
        uint32_t chunk = remaining >
                APPLIANCE_PRODUCTION_ENTROPY_RANDOM_CHUNK_MAX
            ? APPLIANCE_PRODUCTION_ENTROPY_RANDOM_CHUNK_MAX
            : (uint32_t)remaining;
        uint32_t written = 0U;

        if (hfoc_mbedtls_drbg_generate(
                &entropy->drbg,
                &entropy->accumulator,
                &output[offset],
                chunk,
                chunk,
                &written) != HFOC_MBEDTLS_DRBG_OK ||
            written != chunk) {
            /*
             * The bounded seed pool is the only expected terminal failure.
             * Rotate once onto a fresh pool with fresh hardware material; if
             * that is not possible the module fails closed and every later
             * request is refused instead of being answered with stale bytes.
             */
            if (!appliance_production_entropy_rotate(0U)) {
                entropy->state =
                    APPLIANCE_PRODUCTION_ENTROPY_STATE_FAILED;
                return -1;
            }
            continue;
        }
        offset += (size_t)written;
        ++entropy->random_calls;
    }
    return 0;
}

const char *appliance_production_entropy_source_string(void)
{
    switch (appliance_production_entropy_owner.source) {
    case APPLIANCE_PRODUCTION_ENTROPY_SOURCE_RDSEED:
        return "RDSEED";
    case APPLIANCE_PRODUCTION_ENTROPY_SOURCE_RDRAND:
        return "RDRAND";
    case APPLIANCE_PRODUCTION_ENTROPY_SOURCE_OPERATOR:
        return "OPERATOR";
    case APPLIANCE_PRODUCTION_ENTROPY_SOURCE_HOST_PROVISIONED:
        return "HOST_PROVISIONED";
    case APPLIANCE_PRODUCTION_ENTROPY_SOURCE_NONE:
    default:
        return "NONE";
    }
}

const char *appliance_production_entropy_state_string(
    enum appliance_production_entropy_state state)
{
    switch (state) {
    case APPLIANCE_PRODUCTION_ENTROPY_STATE_UNINITIALIZED:
        return "UNINITIALIZED";
    case APPLIANCE_PRODUCTION_ENTROPY_STATE_UNAVAILABLE:
        return "AWAITING_ENTROPY";
    case APPLIANCE_PRODUCTION_ENTROPY_STATE_DEGRADED:
        return "DEGRADED";
    case APPLIANCE_PRODUCTION_ENTROPY_STATE_READY:
        return "READY";
    case APPLIANCE_PRODUCTION_ENTROPY_STATE_FAILED:
        return "FAILED";
    default:
        return NULL;
    }
}

/*
 * ITEM-386 PSA entropy boundary.  The production TLS profile compiles PSA
 * crypto with MBEDTLS_PSA_CRYPTO_EXTERNAL_RNG, so every PSA random request --
 * including the ephemeral key generation of the TLS 1.3 handshake -- comes
 * back here.  The bytes come from the same accumulator/seed/CTR-DRBG chain as
 * the mbedTLS f_rng callback, never from the deterministic fixture generator,
 * and an unavailable or failed chain reports PSA_ERROR_INSUFFICIENT_ENTROPY so
 * PSA refuses to proceed.
 */
psa_status_t mbedtls_psa_external_get_random(
    mbedtls_psa_external_random_context_t *context,
    uint8_t *output,
    size_t output_size,
    size_t *output_length)
{
    (void)context;
    if (output_length == NULL || (output == NULL && output_size != 0U)) {
        return PSA_ERROR_INVALID_ARGUMENT;
    }
    if (output_size == 0U) {
        *output_length = 0U;
        return PSA_SUCCESS;
    }
    if (appliance_production_entropy_random(NULL, output, output_size) !=
        0) {
        return PSA_ERROR_INSUFFICIENT_ENTROPY;
    }
    *output_length = output_size;
    return PSA_SUCCESS;
}

#else

typedef int appliance_production_entropy_translation_unit_guard;

#endif
