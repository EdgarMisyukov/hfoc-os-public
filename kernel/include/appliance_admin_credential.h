#ifndef HFOC_APPLIANCE_ADMIN_CREDENTIAL_H
#define HFOC_APPLIANCE_ADMIN_CREDENTIAL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "sha256.h"

/*
 * ITEM-506 administrator credential model: one login and one password.
 *
 * The appliance has exactly one account, and the owner's contract fixes its
 * shape here, in one place, so the installer, the console `credentials`
 * command and the remote panel cannot drift apart:
 *
 *   * login    1..32 characters of `A-Za-z0-9._-`, compared exactly, case
 *              significant; it is not a secret and may appear in records;
 *   * password 12..128 printable ASCII characters including the space,
 *              compared exactly with no trimming; refused when it is all
 *              spaces, equal to the login, or one of a bounded documented
 *              weak list; there are deliberately no composition rules;
 *   * storage  a PBKDF2-HMAC-SHA256 verifier with a per-account 16-byte salt
 *              and the iteration count written next to the verifier, so the
 *              cost can be raised later without breaking existing records;
 *   * the password itself is never stored, rendered, logged or copied out of
 *              the caller's buffer; derived material is zeroed.
 *
 * The record value this module owns is the part of the persistent secret
 * record after the `v1:secret.admin.credential=` key:
 *
 *   pbkdf2-sha256$<iterations>$<salt-hex>$<verifier-hex>$<login>
 *
 * A record whose value is the legacy 64-character SHA-256 fingerprint is
 * recognised explicitly by `appliance_admin_credential_record_is_legacy()`
 * and is refused as a credential; migration is a new provisioning call, not a
 * silent reinterpretation of an old record.
 *
 * The persistent store keeps this value and never consults it directly:
 * everything about the value format and the policy lives here.
 *
 * The KDF is built on the in-tree SHA-256 the same way
 * `appliance_state_crypto_pbkdf2_sha256()` is.  The frozen TLS mbedTLS
 * profile deliberately does not carry MBEDTLS_PKCS5_C, and enabling it would
 * change the profile the TLS proofs pin; the algorithm is PBKDF2-HMAC-SHA256
 * all the same, and the host proof checks it against the published vectors.
 */

#define APPLIANCE_ADMIN_CREDENTIAL_VERSION 1U
#define APPLIANCE_ADMIN_CREDENTIAL_LOGIN_MIN 1U
#define APPLIANCE_ADMIN_CREDENTIAL_LOGIN_MAX 32U
#define APPLIANCE_ADMIN_CREDENTIAL_PASSWORD_MIN 12U
#define APPLIANCE_ADMIN_CREDENTIAL_PASSWORD_MAX 128U
#define APPLIANCE_ADMIN_CREDENTIAL_SALT_LENGTH 16U
#define APPLIANCE_ADMIN_CREDENTIAL_VERIFIER_LENGTH 32U
#define APPLIANCE_ADMIN_CREDENTIAL_HEX_SALT_LENGTH \
    (APPLIANCE_ADMIN_CREDENTIAL_SALT_LENGTH * 2U)
#define APPLIANCE_ADMIN_CREDENTIAL_HEX_VERIFIER_LENGTH \
    (APPLIANCE_ADMIN_CREDENTIAL_VERIFIER_LENGTH * 2U)
/*
 * The iteration bounds are a downgrade guard, not a preference: a record that
 * claims fewer than the floor is refused, so nobody can rewrite the store to
 * make the KDF cheap.  The default was measured on the build host for this
 * item: 200,000 iterations of the in-tree HMAC-SHA-256 cost about 155 ms
 * there, inside the contract's 100-250 ms window.  CI-SPEEDUP-4 kept every
 * parameter and halved the work per iteration (the pads are hashed once per
 * derivation, not once per round), so the same 200,000 iterations now measure
 * about 89 ms there - the security parameter is unchanged, the cost of
 * spending it fell.  The appliance CPU check belongs to the hardware
 * acceptance and the count is stored per record, so raising it later does not
 * invalidate existing credentials.
 */
#define APPLIANCE_ADMIN_CREDENTIAL_ITERATIONS_MIN 10000U
#define APPLIANCE_ADMIN_CREDENTIAL_ITERATIONS_MAX 2000000U
#define APPLIANCE_ADMIN_CREDENTIAL_ITERATIONS_DEFAULT 200000U
/*
 * The persistent record format and the largest value it can produce:
 * label(13) + 5 separators + decimal iterations(<=10) + salt hex(32) +
 * verifier hex(64) + login(<=32).
 */
#define APPLIANCE_ADMIN_CREDENTIAL_RECORD_LABEL "pbkdf2-sha256"
#define APPLIANCE_ADMIN_CREDENTIAL_RECORD_SEPARATOR '$'
#define APPLIANCE_ADMIN_CREDENTIAL_RECORD_VALUE_MAX 160U
/*
 * ITEM-506 policy: five failed attempts lock the account for a bounded
 * cooldown.  The counting itself lives with the callers that own a clock
 * (`appliance_production_admin_enrollment` today); the number is fixed here
 * so both entries cannot drift apart.
 */
#define APPLIANCE_ADMIN_CREDENTIAL_FAILURE_LIMIT 5U

enum appliance_admin_credential_status {
    APPLIANCE_ADMIN_CREDENTIAL_OK = 0,
    APPLIANCE_ADMIN_CREDENTIAL_NULL = 1,
    APPLIANCE_ADMIN_CREDENTIAL_BAD_LOGIN = 2,
    APPLIANCE_ADMIN_CREDENTIAL_BAD_PASSWORD = 3,
    APPLIANCE_ADMIN_CREDENTIAL_WEAK_PASSWORD = 4,
    APPLIANCE_ADMIN_CREDENTIAL_BAD_ITERATIONS = 5,
    APPLIANCE_ADMIN_CREDENTIAL_BAD_SALT = 6,
    APPLIANCE_ADMIN_CREDENTIAL_BAD_VERIFIER = 7,
    APPLIANCE_ADMIN_CREDENTIAL_DENIED = 8,
    APPLIANCE_ADMIN_CREDENTIAL_LEGACY_RECORD = 9,
    APPLIANCE_ADMIN_CREDENTIAL_BAD_RECORD = 10,
    APPLIANCE_ADMIN_CREDENTIAL_BUFFER_TOO_SMALL = 11,
    APPLIANCE_ADMIN_CREDENTIAL_UNAVAILABLE = 12,
    APPLIANCE_ADMIN_CREDENTIAL_BAD_STATE = 13,
    /* ITEM-619: the job is not finished yet; call `step` again. */
    APPLIANCE_ADMIN_CREDENTIAL_PENDING = 14,
    /* ITEM-619: the job was abandoned and its secret state zeroed. */
    APPLIANCE_ADMIN_CREDENTIAL_CANCELLED = 15,
};

struct appliance_admin_credential {
    uint32_t version;
    char login[APPLIANCE_ADMIN_CREDENTIAL_LOGIN_MAX + 1U];
    uint8_t salt[APPLIANCE_ADMIN_CREDENTIAL_SALT_LENGTH];
    uint32_t iterations;
    uint8_t verifier[APPLIANCE_ADMIN_CREDENTIAL_VERIFIER_LENGTH];
};

/*
 * Login policy: 1..32 bytes, exactly the characters the model fixes, no
 * leading or trailing space (spaces are not in the alphabet at all).
 */
enum appliance_admin_credential_status
appliance_admin_credential_validate_login(
    const char *login,
    size_t login_length);
/*
 * Password policy: 12..128 printable ASCII bytes including interior spaces,
 * no trimming, refused when all spaces or equal (case-insensitively) to the
 * login.  `login` may be NULL only when `login_length` is zero.
 */
enum appliance_admin_credential_status
appliance_admin_credential_validate_password(
    const char *password,
    size_t password_length,
    const char *login,
    size_t login_length);
/*
 * True when the password is in the bounded weak list.  Exposed so the check
 * can be proved and pinned without duplicating the list.
 */
bool appliance_admin_credential_password_weak(
    const char *password,
    size_t password_length);
/*
 * PBKDF2-HMAC-SHA256 (one 32-byte block).  The password is the caller's
 * exact bytes; iterations are validated against the bounds above.
 */
enum appliance_admin_credential_status
appliance_admin_credential_derive(
    const char *password,
    size_t password_length,
    const uint8_t salt[APPLIANCE_ADMIN_CREDENTIAL_SALT_LENGTH],
    uint32_t iterations,
    uint8_t verifier_out[APPLIANCE_ADMIN_CREDENTIAL_VERIFIER_LENGTH]);
/*
 * Validate the policy and derive the verifier in one call.  `credential_out`
 * is zeroed first and left zeroed on every refusal.
 */
enum appliance_admin_credential_status
appliance_admin_credential_create(
    const char *login,
    size_t login_length,
    const char *password,
    size_t password_length,
    const uint8_t salt[APPLIANCE_ADMIN_CREDENTIAL_SALT_LENGTH],
    uint32_t iterations,
    struct appliance_admin_credential *credential_out);
/*
 * One credential check: constant-time login comparison, one derivation with
 * the stored parameters, constant-time verifier comparison.  A wrong login
 * and a wrong password are both `DENIED` and cost the same derivation, so the
 * refusal does not say which half failed.
 */
enum appliance_admin_credential_status
appliance_admin_credential_verify(
    const struct appliance_admin_credential *stored,
    const char *login,
    size_t login_length,
    const char *password,
    size_t password_length);
/*
 * Record value codec.  `encode` writes a NUL-terminated canonical value and
 * requires room for it; `decode` accepts exactly the canonical shape and
 * refuses a legacy fingerprint with `LEGACY_RECORD`.
 */
enum appliance_admin_credential_status
appliance_admin_credential_record_encode(
    const struct appliance_admin_credential *credential,
    char *buffer,
    size_t buffer_size,
    size_t *length_out);
enum appliance_admin_credential_status
appliance_admin_credential_record_decode(
    const char *value,
    size_t value_length,
    struct appliance_admin_credential *credential_out);
bool appliance_admin_credential_record_is_legacy(
    const char *value,
    size_t value_length);
/*
 * Full validity check of a record value: true when it is a well-formed legacy
 * fingerprint or a value the decoder accepts.  It parses, it is not a shape
 * test; the secret-persistence catalogue keeps its own looser shared-shape
 * check because the exact grammar belongs here.
 */
bool appliance_admin_credential_record_value_valid(
    const uint8_t *value,
    size_t value_length);
/*
 * ITEM-619: the credential check as a resumable job.
 *
 * The derivation above spends its whole iteration count in one call.  That is
 * correct for the hash but wrong for the event loop: the single caller in the
 * production serving poll cannot service ARP/ICMP/SSH or lwIP timers until it
 * returns, which is the measured 21.4 s panel stall.  The same derivation is
 * therefore exposed as a fixed-size job the caller advances in bounded
 * slices.  No parameter, salt, verifier or comparison changes: a drained job
 * produces exactly the bytes `derive()` produces, and the drain loop below is
 * what `derive()` and `verify()` now use internally.
 *
 * The caller owns the storage (no heap), drives the slices from its own loop
 * and must call `cancel()` on every error, disconnect or deadline.  The job
 * holds secret-derived state, never logs it, and zeroizes on cancel.  On
 * completion the derived verifier is kept only for the caller's immediate
 * session issuance (or for the synchronous `verify()` wrapper, which cancels
 * before it returns).  A slice is a count of PBKDF2 rounds, so the time one
 * step costs is bounded by the same work the old loop did per round; the
 * caller chooses the count from its measured budget.
 */
/*
 * ITEM-619: 64 rounds is ~12 ms on the measured Toshiba cycle and ~150 ms
 * under the TCG production proof.  The old 256-round slice took ~600 ms under
 * TCG, so a second TLS handshake could not finish inside the two-second
 * liveness bound while the sign-in job was pending.  This value is the yield
 * granularity only: the stored iteration count is never shortened.
 */
#define APPLIANCE_ADMIN_CREDENTIAL_CHECK_ROUNDS_DEFAULT 64U
/*
 * ITEM-650: one fixed 64-round slice per PIT wake made the 200000-round check
 * wait 3125 wakes before it could answer, even though the derivation itself
 * costs a fraction of that wall time.  The serving loop now measures the last
 * slice with the project's calibrated cycle clock and asks for the next one
 * that spends this bounded share of a turn.  The lower bound stays the proven
 * 64 rounds, so a slow/TCG machine that cannot afford more keeps the exact
 * liveness behavior ITEM-619 proved; the upper bound keeps one turn from
 * becoming an unbounded stall on a very fast machine.  This changes only the
 * yield granularity: every stored iteration is still derived.
 */
#define APPLIANCE_ADMIN_CREDENTIAL_CHECK_SLICE_TARGET_US 40000U
#define APPLIANCE_ADMIN_CREDENTIAL_CHECK_SLICE_MAX_ROUNDS 4096U

struct appliance_admin_credential_hmac_key {
    struct sha256 inner;
    struct sha256 outer;
    bool ready;
};

struct appliance_admin_credential_kdf {
    struct appliance_admin_credential_hmac_key prepared;
    uint8_t block_input[APPLIANCE_ADMIN_CREDENTIAL_SALT_LENGTH + 4U];
    uint8_t u[APPLIANCE_ADMIN_CREDENTIAL_VERIFIER_LENGTH];
    uint8_t t[APPLIANCE_ADMIN_CREDENTIAL_VERIFIER_LENGTH];
    uint8_t verifier[APPLIANCE_ADMIN_CREDENTIAL_VERIFIER_LENGTH];
    uint32_t iterations_total;
    uint32_t iterations_done;
    enum appliance_admin_credential_status result;
    bool started;
    bool finished;
};

struct appliance_admin_credential_check {
    struct appliance_admin_credential_kdf kdf;
    uint8_t stored_verifier[APPLIANCE_ADMIN_CREDENTIAL_VERIFIER_LENGTH];
    bool login_matches;
    bool finished;
    enum appliance_admin_credential_status result;
};

/*
 * Start a derivation and run PBKDF2 round 1.  Returns `PENDING` while rounds
 * remain or `OK` when the whole derivation already finished (one iteration).
 * Every other return is a refusal and leaves no secret state behind.
 */
enum appliance_admin_credential_status
appliance_admin_credential_kdf_begin(
    struct appliance_admin_credential_kdf *kdf,
    const char *password,
    size_t password_length,
    const uint8_t salt[APPLIANCE_ADMIN_CREDENTIAL_SALT_LENGTH],
    uint32_t iterations);
/*
 * Advance at most `rounds` PBKDF2 rounds.  Returns `PENDING` while more rounds
 * remain, `OK` when the derivation is complete and the verifier is ready, or
 * a stable refusal.  `rounds == 0` makes no progress and is not an error.
 */
enum appliance_admin_credential_status
appliance_admin_credential_kdf_step(
    struct appliance_admin_credential_kdf *kdf,
    uint32_t rounds);
void appliance_admin_credential_kdf_cancel(
    struct appliance_admin_credential_kdf *kdf);
bool appliance_admin_credential_kdf_pending(
    const struct appliance_admin_credential_kdf *kdf);
uint32_t appliance_admin_credential_kdf_iterations_done(
    const struct appliance_admin_credential_kdf *kdf);
/*
 * The complete credential check as a job: constant-time login comparison, the
 * resumable derivation, then one constant-time verifier comparison.  The
 * result is `OK`, `DENIED` or a refusal; `PENDING` means "call step again".
 */
enum appliance_admin_credential_status
appliance_admin_credential_check_begin(
    struct appliance_admin_credential_check *check,
    const struct appliance_admin_credential *stored,
    const char *login,
    size_t login_length,
    const char *password,
    size_t password_length);
enum appliance_admin_credential_status
appliance_admin_credential_check_step(
    struct appliance_admin_credential_check *check,
    uint32_t rounds);
/*
 * Choose the next cooperative slice from the measured cost of the previous
 * one.  `rounds_done` is what that slice actually advanced and `elapsed_us`
 * is its measured time; either being zero means "not measured yet" and keeps
 * the proven minimum.  The answer is clamped to
 * [CHECK_ROUNDS_DEFAULT, CHECK_SLICE_MAX_ROUNDS] and never changes the
 * derivation.  Pure arithmetic, so the host proof can pin it.
 */
uint32_t appliance_admin_credential_check_next_slice(uint32_t rounds_done,
                                                     uint32_t elapsed_us);
void appliance_admin_credential_check_cancel(
    struct appliance_admin_credential_check *check);
bool appliance_admin_credential_check_pending(
    const struct appliance_admin_credential_check *check);
uint32_t appliance_admin_credential_check_iterations_done(
    const struct appliance_admin_credential_check *check);
uint32_t appliance_admin_credential_check_iterations_total(
    const struct appliance_admin_credential_check *check);

const char *appliance_admin_credential_status_string(
    enum appliance_admin_credential_status status);

#endif
