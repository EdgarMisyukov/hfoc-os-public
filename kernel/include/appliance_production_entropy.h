#ifndef HFOC_APPLIANCE_PRODUCTION_ENTROPY_H
#define HFOC_APPLIANCE_PRODUCTION_ENTROPY_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/*
 * ITEM-386 ordinary-boot entropy owner for the production TLS listener.
 *
 * Before ITEM-386 the production kernel had no entropy owner at all: the
 * activation record was called with a literal `entropy_ready = false`, the
 * management TLS listener did not exist, and the only generator in the tree
 * was the deterministic fake RNG of the ITEM-270 QEMU TLS fixture.  This
 * module is the real owner.
 *
 * The source is the CPU itself: RDSEED when CPUID advertises it, otherwise
 * RDRAND when CPUID advertises that, otherwise nothing.  The module never
 * substitutes a deterministic generator, never falls back to a lower-quality
 * source without saying so, and never pretends to be ready:
 *
 *   NONE                  no hardware source; state UNAVAILABLE and the
 *                         serving path must not start a listener;
 *   RDSEED / RDRAND       a hardware block was accepted; state READY only
 *                         after the block was mixed into the ITEM-268
 *                         accumulator and the ITEM-269 seed controller and
 *                         the ITEM-264 CTR-DRBG accepted the seed.
 *   OPERATOR              a local operator supplied 32 bytes from a trusted
 *                         CSPRNG through the hidden console prompt; this is
 *                         the explicit fallback for a CPU with neither
 *                         instruction, never a time- or constant-derived
 *                         substitute.
 *   HOST_PROVISIONED      a 256-bit seed provisioned from the trusted host
 *                         CSPRNG, sealed under the administrator password on
 *                         the installed persistence partition, and expanded
 *                         with a durable boot epoch.  It is the ordinary path
 *                         on a CPU with neither hardware instruction; OPERATOR
 *                         remains an explicit recovery source.
 *
 * The collected material is mixed through the existing accumulator and
 * seeding path (`entropy_accumulator`, `entropy_health`,
 * `entropy_seed_fill`, `hfoc_mbedtls_drbg`), so the frozen entropy contracts
 * stay the only owners of credited-byte accounting, health observation and
 * DRBG reseeding.  This module owns no generator of its own.
 *
 * The generator material pool is bounded: one seed controller can publish a
 * fixed number of DRBG reseeds before the pool is exhausted.  When that
 * happens the module rotates the whole chain -- a fresh accumulator, health
 * observer, seed controller and DRBG with the next generation -- and mixes a
 * fresh hardware block.  Rotation is bounded by
 * APPLIANCE_PRODUCTION_ENTROPY_ROTATION_MAX so the failure mode is explicit
 * rather than an unbounded retry loop; a rotation that cannot be completed
 * fails the module closed and the serving path refuses to start TLS work.
 */

#define APPLIANCE_PRODUCTION_ENTROPY_VERSION 1U

#define APPLIANCE_PRODUCTION_ENTROPY_GENERATION_MIN 1U
#define APPLIANCE_PRODUCTION_ENTROPY_GENERATION_MAX UINT32_C(0xFFFFFFFE)
#define APPLIANCE_PRODUCTION_ENTROPY_SOURCE_ID 1U

/* One accepted hardware block, and how many are mixed before publishing. */
#define APPLIANCE_PRODUCTION_ENTROPY_BLOCK_BYTES 32U
#define APPLIANCE_PRODUCTION_ENTROPY_BLOCK_COUNT 16U
#define APPLIANCE_PRODUCTION_ENTROPY_REQUIRED_CREDITED_BYTES 256U

/* Bounded retry count for a single rdseed/rdrand instruction. */
#define APPLIANCE_PRODUCTION_ENTROPY_INSTRUCTION_RETRY 32U

#define APPLIANCE_PRODUCTION_ENTROPY_RESEED_INTERVAL 1000U
#define APPLIANCE_PRODUCTION_ENTROPY_FILL_COUNT_MAX 64U
#define APPLIANCE_PRODUCTION_ENTROPY_FILL_LENGTH_MAX 48U
#define APPLIANCE_PRODUCTION_ENTROPY_SEED_LENGTH 48U
#define APPLIANCE_PRODUCTION_ENTROPY_RANDOM_CHUNK_MAX 256U

/*
 * Upper bound on entropy-pool rotations in one boot.  One rotation publishes
 * APPLIANCE_PRODUCTION_ENTROPY_FILL_COUNT_MAX DRBG reseeds, and one reseed
 * covers APPLIANCE_PRODUCTION_ENTROPY_RESEED_INTERVAL generator calls, so the
 * ceiling is a documented number of random bytes rather than an open retry.
 */
#define APPLIANCE_PRODUCTION_ENTROPY_ROTATION_MAX 1024U

/*
 * ITEM-585: the operator fallback.  A CPU without RDRAND/RDSEED (the installed
 * Toshiba reports both zero and stops at DETECT) has no hardware generator this
 * kernel can own.  The safe answer is an explicit local seed from a trusted
 * CSPRNG, typed through a hidden console line: 32 bytes are 256 bits, which is
 * the DRBG's seed size, and the material goes through the same accumulator,
 * health, seed-fill and CTR-DRBG chain as a hardware block.  The seed is never
 * a timestamp, a build constant, or a value derived from one, and it is never
 * echoed, journaled or stored.
 */
#define APPLIANCE_PRODUCTION_ENTROPY_OPERATOR_SEED_BYTES 32U
#define APPLIANCE_PRODUCTION_ENTROPY_OPERATOR_SEED_HEX_CHARS 64U
#define APPLIANCE_PRODUCTION_ENTROPY_OPERATOR_SEED_MIN_DISTINCT 16U

enum appliance_production_entropy_source {
    APPLIANCE_PRODUCTION_ENTROPY_SOURCE_NONE = 0,
    APPLIANCE_PRODUCTION_ENTROPY_SOURCE_RDSEED = 1,
    APPLIANCE_PRODUCTION_ENTROPY_SOURCE_RDRAND = 2,
    APPLIANCE_PRODUCTION_ENTROPY_SOURCE_OPERATOR = 3,
    APPLIANCE_PRODUCTION_ENTROPY_SOURCE_HOST_PROVISIONED = 4,
};

enum appliance_production_entropy_state {
    APPLIANCE_PRODUCTION_ENTROPY_STATE_UNINITIALIZED = 0,
    APPLIANCE_PRODUCTION_ENTROPY_STATE_UNAVAILABLE = 1,
    APPLIANCE_PRODUCTION_ENTROPY_STATE_DEGRADED = 2,
    APPLIANCE_PRODUCTION_ENTROPY_STATE_READY = 3,
    APPLIANCE_PRODUCTION_ENTROPY_STATE_FAILED = 4,
};

/*
 * ITEM-581: where the generator stopped when it could not reach READY.  The
 * owner's installed machine refused `credentials` with NO_RANDOM and nothing
 * said whether the CPU advertised a source at all, whether the source was
 * asked and failed, or whether the accumulator/health/seed/DRBG/PSA stage
 * refused the block.  The stage is a fixed word set once, before the failing
 * step, so the next run names the cause instead of the symptom.
 */
enum appliance_production_entropy_stage {
    APPLIANCE_PRODUCTION_ENTROPY_STAGE_NONE = 0,
    /* No CPUID-advertised source on this CPU. */
    APPLIANCE_PRODUCTION_ENTROPY_STAGE_DETECT = 1,
    /* The source was asked and the instruction never carried. */
    APPLIANCE_PRODUCTION_ENTROPY_STAGE_INSTRUCTION = 2,
    APPLIANCE_PRODUCTION_ENTROPY_STAGE_HEALTH = 3,
    APPLIANCE_PRODUCTION_ENTROPY_STAGE_ACCUMULATOR = 4,
    APPLIANCE_PRODUCTION_ENTROPY_STAGE_SEED = 5,
    APPLIANCE_PRODUCTION_ENTROPY_STAGE_DRBG = 6,
    APPLIANCE_PRODUCTION_ENTROPY_STAGE_PSA = 7,
};

/*
 * The source the CPUID capabilities select: RDSEED first when both are
 * advertised, RDRAND otherwise, NONE when neither is.  Pure, so the host proof
 * can pin the three CPU profiles the production boot must handle.
 */
static inline enum appliance_production_entropy_source
appliance_production_entropy_source_for_cpuid(uint32_t max_leaf,
                                              bool rdrand,
                                              bool rdseed)
{
    if (max_leaf >= 7U && rdseed) {
        return APPLIANCE_PRODUCTION_ENTROPY_SOURCE_RDSEED;
    }
    if (max_leaf >= 1U && rdrand) {
        return APPLIANCE_PRODUCTION_ENTROPY_SOURCE_RDRAND;
    }
    return APPLIANCE_PRODUCTION_ENTROPY_SOURCE_NONE;
}

/*
 * ITEM-585: why the operator seed was accepted or refused.  The console prints
 * the word, never the material.
 */
enum appliance_production_entropy_seed_status {
    APPLIANCE_PRODUCTION_ENTROPY_SEED_OK = 0,
    APPLIANCE_PRODUCTION_ENTROPY_SEED_NULL_ARGUMENT = 1,
    APPLIANCE_PRODUCTION_ENTROPY_SEED_WRONG_LENGTH = 2,
    APPLIANCE_PRODUCTION_ENTROPY_SEED_LOW_DIVERSITY = 3,
    APPLIANCE_PRODUCTION_ENTROPY_SEED_NOT_HEX = 4,
    APPLIANCE_PRODUCTION_ENTROPY_SEED_ALREADY_READY = 5,
    APPLIANCE_PRODUCTION_ENTROPY_SEED_NOT_AVAILABLE = 6,
    APPLIANCE_PRODUCTION_ENTROPY_SEED_FAILED = 7,
};

/*
 * The pure half of the operator path, so the host proof can pin the exact
 * acceptance rule: exactly 64 hex characters, exactly 32 bytes, at least 16
 * distinct byte values and not two identical halves.  These are rejection
 * rules, not a statistical certification; the caller still has to obtain the
 * bytes from a trusted CSPRNG.
 */
static inline int appliance_production_entropy_hex_nibble(char character)
{
    if (character >= '0' && character <= '9') {
        return character - '0';
    }
    if (character >= 'a' && character <= 'f') {
        return character - 'a' + 10;
    }
    if (character >= 'A' && character <= 'F') {
        return character - 'A' + 10;
    }
    return -1;
}

static inline bool appliance_production_entropy_operator_seed_valid(
    const uint8_t *seed,
    size_t length)
{
    uint8_t seen[256];
    size_t index;
    size_t distinct = 0U;
    bool repeated_halves = true;

    if (seed == NULL ||
        length != APPLIANCE_PRODUCTION_ENTROPY_OPERATOR_SEED_BYTES) {
        return false;
    }
    memset(seen, 0, sizeof(seen));
    for (index = 0U; index < length; ++index) {
        if (seen[seed[index]] == 0U) {
            seen[seed[index]] = 1U;
            ++distinct;
        }
        if (index >= length / 2U &&
            seed[index] != seed[index - length / 2U]) {
            repeated_halves = false;
        }
    }
    return distinct >=
               APPLIANCE_PRODUCTION_ENTROPY_OPERATOR_SEED_MIN_DISTINCT &&
           !repeated_halves;
}

static inline enum appliance_production_entropy_seed_status
appliance_production_entropy_operator_seed_status_from_hex(
    const char *text,
    size_t text_length,
    uint8_t seed[APPLIANCE_PRODUCTION_ENTROPY_OPERATOR_SEED_BYTES])
{
    size_t index;

    if (text == NULL || seed == NULL) {
        return APPLIANCE_PRODUCTION_ENTROPY_SEED_NULL_ARGUMENT;
    }
    if (text_length !=
        APPLIANCE_PRODUCTION_ENTROPY_OPERATOR_SEED_HEX_CHARS) {
        return APPLIANCE_PRODUCTION_ENTROPY_SEED_WRONG_LENGTH;
    }
    for (index = 0U;
         index < APPLIANCE_PRODUCTION_ENTROPY_OPERATOR_SEED_BYTES;
         ++index) {
        const int high =
            appliance_production_entropy_hex_nibble(text[index * 2U]);
        const int low =
            appliance_production_entropy_hex_nibble(text[index * 2U + 1U]);

        if (high < 0 || low < 0) {
            memset(seed, 0,
                   APPLIANCE_PRODUCTION_ENTROPY_OPERATOR_SEED_BYTES);
            return APPLIANCE_PRODUCTION_ENTROPY_SEED_NOT_HEX;
        }
        seed[index] = (uint8_t)(((unsigned int)high << 4U) |
                                (unsigned int)low);
    }
    if (!appliance_production_entropy_operator_seed_valid(
            seed, APPLIANCE_PRODUCTION_ENTROPY_OPERATOR_SEED_BYTES)) {
        memset(seed, 0,
               APPLIANCE_PRODUCTION_ENTROPY_OPERATOR_SEED_BYTES);
        return APPLIANCE_PRODUCTION_ENTROPY_SEED_LOW_DIVERSITY;
    }
    return APPLIANCE_PRODUCTION_ENTROPY_SEED_OK;
}

static inline bool appliance_production_entropy_operator_seed_from_hex(
    const char *text,
    size_t text_length,
    uint8_t seed[APPLIANCE_PRODUCTION_ENTROPY_OPERATOR_SEED_BYTES])
{
    return appliance_production_entropy_operator_seed_status_from_hex(
               text, text_length, seed) ==
           APPLIANCE_PRODUCTION_ENTROPY_SEED_OK;
}

/*
 * Publish the hardware source.  Returns true when the module owns a published
 * state (READY or UNAVAILABLE); the caller must consult
 * appliance_production_entropy_ready() before it starts any TLS listener.
 */
bool appliance_production_entropy_init(uint32_t now_ms);

/*
 * ITEM-585: replace an UNAVAILABLE hardware owner with one operator-supplied
 * seed.  The seed is mixed through the same chain; it is not stored.  The
 * call is refused when the hardware source already owns a READY or FAILED
 * state, so an operator can never overwrite working hardware entropy.
 */
enum appliance_production_entropy_seed_status
appliance_production_entropy_seed_operator(
    const uint8_t *seed,
    size_t length,
    uint32_t now_ms);
/*
 * ITEM-598: publish the password-unlocked host seed.  The owner derives the
 * DRBG material from the durable epoch before accepting it.  The derived
 * material seeds one bounded pool; it is not recredited as fresh entropy when
 * that pool is exhausted.  The next boot advances the durable epoch instead.
 */
enum appliance_production_entropy_seed_status
appliance_production_entropy_seed_host(
    const uint8_t *seed,
    size_t length,
    uint64_t boot_epoch,
    uint32_t now_ms);
const char *appliance_production_entropy_seed_status_string(
    enum appliance_production_entropy_seed_status status);

bool appliance_production_entropy_initialized(void);
bool appliance_production_entropy_ready(void);
enum appliance_production_entropy_state
appliance_production_entropy_state(void);
enum appliance_production_entropy_source
appliance_production_entropy_source(void);
uint32_t appliance_production_entropy_random_calls(void);
uint32_t appliance_production_entropy_generation(void);
uint32_t appliance_production_entropy_rotations(void);
uint64_t appliance_production_entropy_credited_bytes(void);

const char *appliance_production_entropy_source_string(void);
const char *appliance_production_entropy_state_string(
    enum appliance_production_entropy_state state);

/*
 * ITEM-581: the CPU capabilities the detection actually read, and the stage
 * the generator stopped at when it did not reach READY.  The values only
 * characterise the failure; no password, token or random material is here.
 */
void appliance_production_entropy_cpu_flags(uint32_t *max_leaf_out,
                                            bool *rdrand_out,
                                            bool *rdseed_out);
enum appliance_production_entropy_stage
appliance_production_entropy_failure_stage(void);
const char *appliance_production_entropy_stage_string(
    enum appliance_production_entropy_stage stage);

/*
 * mbedTLS RNG callback (f_rng).  Returns 0 on success and a non-zero value on
 * failure, exactly as mbedTLS expects; a failure is terminal for the caller
 * and must never be retried into a deterministic output.
 */
int appliance_production_entropy_random(
    void *context,
    unsigned char *output,
    size_t length);

#endif
