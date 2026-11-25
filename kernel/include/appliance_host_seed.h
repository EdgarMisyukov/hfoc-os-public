#ifndef HFOC_APPLIANCE_HOST_SEED_H
#define HFOC_APPLIANCE_HOST_SEED_H

#include "block_device.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * ITEM-598 host-provisioned entropy.
 *
 * The trusted provisioning host writes one bounded media record from its
 * kernel CSPRNG.  The installer rewraps the seed under the administrator
 * password before it reaches the installed persistence partition; only the
 * wrapped record is durable on the appliance.  A boot unlocks that record with
 * the ordinary password, advances the two-slot sequence before any random
 * output is published, and derives a domain-separated DRBG seed.
 *
 * The media-record SHA-256 detects corruption and binds the target/build
 * context.  It is not authentication: physical custody of a personalised
 * medium is the provisioning trust boundary.  The installed record is
 * authenticated by HMAC-SHA-256 under a PBKDF2 key derived from the password.
 */

#define APPLIANCE_HOST_SEED_VERSION 1U
#define APPLIANCE_HOST_SEED_LENGTH 32U
#define APPLIANCE_HOST_SEED_TARGET_LENGTH 32U
#define APPLIANCE_HOST_SEED_TARGET_TEXT_MAX \
    (APPLIANCE_HOST_SEED_TARGET_LENGTH + 1U)
#define APPLIANCE_HOST_SEED_MEDIA_RECORD_LENGTH 120U
#define APPLIANCE_HOST_SEED_STATE_SLOT_LENGTH 512U
#define APPLIANCE_HOST_SEED_STATE_SLOT_COUNT 2U
#define APPLIANCE_HOST_SEED_DOMAIN_INSTALL 1U

enum appliance_host_seed_status {
    APPLIANCE_HOST_SEED_OK = 0,
    APPLIANCE_HOST_SEED_NULL = 1,
    APPLIANCE_HOST_SEED_BAD_LENGTH = 2,
    APPLIANCE_HOST_SEED_BAD_MAGIC = 3,
    APPLIANCE_HOST_SEED_BAD_VERSION = 4,
    APPLIANCE_HOST_SEED_BAD_DOMAIN = 5,
    APPLIANCE_HOST_SEED_BAD_RESERVED = 6,
    APPLIANCE_HOST_SEED_BAD_TARGET = 7,
    APPLIANCE_HOST_SEED_BAD_BUILD = 8,
    APPLIANCE_HOST_SEED_BAD_INTEGRITY = 9,
    APPLIANCE_HOST_SEED_LOW_DIVERSITY = 10,
    APPLIANCE_HOST_SEED_BAD_DEVICE = 11,
    APPLIANCE_HOST_SEED_BAD_LAYOUT = 12,
    APPLIANCE_HOST_SEED_MISSING = 13,
    APPLIANCE_HOST_SEED_CORRUPT = 14,
    APPLIANCE_HOST_SEED_WRONG_PASSWORD = 15,
    APPLIANCE_HOST_SEED_EPOCH_EXHAUSTED = 16,
    APPLIANCE_HOST_SEED_IO_ERROR = 17,
    APPLIANCE_HOST_SEED_RANDOM_FAILED = 18,
    APPLIANCE_HOST_SEED_UNSUPPORTED = 19,
};

struct appliance_host_seed_media_record {
    uint32_t version;
    uint32_t domain;
    uint32_t build_id;
    char target[APPLIANCE_HOST_SEED_TARGET_TEXT_MAX];
    uint8_t seed[APPLIANCE_HOST_SEED_LENGTH];
};

/*
 * ITEM-611: the ordinary login's seed path reports what it actually did, so a
 * slow password check can be attributed instead of guessed.  The accounting
 * carries stage cycle counts and operation/KDF counters only; it never keeps
 * the password, seed, tag, key or a hash-derived value.  The clock is a
 * caller seam: host proofs pass NULL and keep the counters.
 */
typedef uint64_t (*appliance_host_seed_clock_fn)(void);

struct appliance_host_seed_accounting {
    uint64_t read_cycles;
    uint64_t open_cycles;
    uint64_t seal_cycles;
    uint64_t write_cycles;
    uint64_t readback_cycles;
    uint32_t slot_reads;
    uint32_t slot_writes;
    uint32_t opens;
    uint32_t seals;
    uint32_t readbacks;
    uint32_t kdf_calls;
    uint32_t kdf_iterations;
};

void appliance_host_seed_accounting_begin(appliance_host_seed_clock_fn clock);
void appliance_host_seed_accounting_read(
    struct appliance_host_seed_accounting *accounting_out);
void appliance_host_seed_accounting_end(void);

enum appliance_host_seed_status appliance_host_seed_media_parse(
    const uint8_t *bytes,
    size_t length,
    struct appliance_host_seed_media_record *record);
enum appliance_host_seed_status appliance_host_seed_media_build(
    const struct appliance_host_seed_media_record *record,
    uint8_t bytes[APPLIANCE_HOST_SEED_MEDIA_RECORD_LENGTH]);
enum appliance_host_seed_status appliance_host_seed_media_match(
    const struct appliance_host_seed_media_record *record,
    const char *target,
    uint32_t build_id);

/*
 * Provision the installed two-slot store.  Sequence 1 is written to slot 0,
 * flushed, read back and authenticated before success is returned.
 */
enum appliance_host_seed_status appliance_host_seed_store_provision(
    struct block_device *partition_device,
    uint64_t layout_capacity,
    const struct appliance_host_seed_media_record *record,
    const char *password,
    size_t password_length);

/*
 * Read the highest valid slot, authenticate it with the password, write the
 * next sequence to the other slot, flush, read back and authenticate again.
 * `epoch_out` is the new durable sequence.  No seed is returned until all
 * those steps succeed.
 */
enum appliance_host_seed_status appliance_host_seed_store_unlock(
    struct block_device *partition_device,
    uint64_t layout_capacity,
    const char *password,
    size_t password_length,
    uint8_t seed_out[APPLIANCE_HOST_SEED_LENGTH],
    uint64_t *epoch_out);
/*
 * Re-seal the current highest slot under a replacement administrator
 * password.  The old slot remains readable until the account owner commits
 * the new credential, so a failed account write can still boot with the old
 * pair.  Once the new record exists, either pair selects the matching slot;
 * the account verifier is the authority that decides which pair may log in.
 */
enum appliance_host_seed_status appliance_host_seed_store_rekey(
    struct block_device *partition_device,
    uint64_t layout_capacity,
    const char *current_password,
    size_t current_password_length,
    const char *new_password,
    size_t new_password_length);
/*
 * Read-only presence probe used by ordinary boot to decide whether the local
 * login conversation is applicable.  It does not authenticate or expose the
 * seed; a non-zero slot still has to pass store_unlock before it can be used.
 * A valid legacy partition with no room for the companion slots reports
 * `OK` with `present_out=false`; provision, unlock and rekey still reject it.
 */
enum appliance_host_seed_status appliance_host_seed_store_present(
    struct block_device *partition_device,
    uint64_t layout_capacity,
    bool *present_out);

/*
 * Derive the material for one DRBG generation.  This is an expansion of the
 * installed 256-bit seed, never a claim of fresh entropy on each boot.
 */
enum appliance_host_seed_status appliance_host_seed_derive(
    const uint8_t seed[APPLIANCE_HOST_SEED_LENGTH],
    uint64_t epoch,
    uint32_t rotation,
    uint8_t output[APPLIANCE_HOST_SEED_LENGTH]);

void appliance_host_seed_clear_media(
    struct appliance_host_seed_media_record *record);
const char *appliance_host_seed_status_string(
    enum appliance_host_seed_status status);

#endif
