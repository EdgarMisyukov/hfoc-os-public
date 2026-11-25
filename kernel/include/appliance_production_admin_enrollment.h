#ifndef HFOC_APPLIANCE_PRODUCTION_ADMIN_ENROLLMENT_H
#define HFOC_APPLIANCE_PRODUCTION_ADMIN_ENROLLMENT_H

#include "appliance_admin_auth.h"
#include "appliance_admin_credential.h"
#include "appliance_persistent_secret_config.h"
#include "appliance_production_auth.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * ITEM-401 administrator credential enrollment and local session issuance.
 *
 * The ITEM-363 first-boot flow commits only the SHA-256 fingerprint of an
 * administrator credential to the `secret.admin.credential` record, and the
 * ITEM-365 production request gate starts every boot with no verifier, so
 * every protected target answers 503.  This module is the missing owner
 * between them: the local administrator presents the secret once, the module
 * re-derives the 32-byte verifier in memory, proves the re-derived fingerprint
 * equals the persisted record, publishes the verifier into the request gate,
 * and issues one bounded bearer session for the local operator.
 *
 * Nothing here is persisted and nothing is fabricated:
 *
 *   * the presented secret is never stored, rendered, logged or kept after the
 *     call; only the derived verifier lives in the gate's credential slot;
 *   * a secret whose fingerprint does not match the persisted record is
 *     denied and never activates the gate;
 *   * the factory credential the ordinary first-boot flow currently commits is
 *     recognised as an unprovisioned placeholder and is never accepted as an
 *     authenticating credential; the first authenticated presentation replaces
 *     the placeholder record with the operator's fingerprint;
 *   * an appliance without a usable secret record stays UNAVAILABLE and the
 *     gate keeps refusing every protected target.
 *
 * The derivation is the documented production contract:
 *
 *   verifier  = SHA-256("HFOC-ADMIN-CREDENTIAL-V1:" || secret)
 *   record    = secret-protection fingerprint of the verifier with kind
 *               APPLIANCE_SECRET_PROTECTION_KIND_ADMIN_CREDENTIAL and
 *               generation 1, exactly as the first-boot flow computes it
 *
 * The bearer token is derived from the verifier with a per-boot salt and a
 * session counter and rendered into a 32-character URL-safe alphabet, so the
 * local operator can copy it into an `Authorization: Bearer` header while a
 * leaked token reveals nothing about the verifier.
 */

#define APPLIANCE_PRODUCTION_ADMIN_ENROLLMENT_VERSION 1U
#define APPLIANCE_PRODUCTION_ADMIN_ENROLLMENT_SECRET_MAX 64U
#define APPLIANCE_PRODUCTION_ADMIN_ENROLLMENT_RENDER_MAX 256U
#define APPLIANCE_PRODUCTION_ADMIN_ENROLLMENT_FACTORY_CREDENTIAL_LENGTH \
    APPLIANCE_ADMIN_AUTH_CREDENTIAL_LENGTH
#define APPLIANCE_PRODUCTION_ADMIN_ENROLLMENT_GENERATION 1U
#define APPLIANCE_PRODUCTION_ADMIN_ENROLLMENT_FAILURE_LIMIT \
    APPLIANCE_ADMIN_AUTH_FAILURE_LIMIT
#define APPLIANCE_PRODUCTION_ADMIN_ENROLLMENT_COOLDOWN_TICKS \
    APPLIANCE_ADMIN_AUTH_COOLDOWN_TICKS

/*
 * The credential the ordinary first-boot flow commits while no operator
 * secret exists yet.  It is a factory marker, not a credential: the
 * enrollment module refuses it and the request gate is never activated with
 * it.
 */
extern const uint8_t
    appliance_production_admin_enrollment_factory_credential
        [APPLIANCE_PRODUCTION_ADMIN_ENROLLMENT_FACTORY_CREDENTIAL_LENGTH];

enum appliance_production_admin_enrollment_state {
    APPLIANCE_PRODUCTION_ADMIN_ENROLLMENT_STATE_NONE = 0,
    APPLIANCE_PRODUCTION_ADMIN_ENROLLMENT_STATE_UNINITIALIZED = 1,
    APPLIANCE_PRODUCTION_ADMIN_ENROLLMENT_STATE_UNAVAILABLE = 2,
    APPLIANCE_PRODUCTION_ADMIN_ENROLLMENT_STATE_UNPROVISIONED = 3,
    APPLIANCE_PRODUCTION_ADMIN_ENROLLMENT_STATE_AWAITING_SECRET = 4,
    APPLIANCE_PRODUCTION_ADMIN_ENROLLMENT_STATE_ACTIVE = 5,
    APPLIANCE_PRODUCTION_ADMIN_ENROLLMENT_STATE_FAILED = 6,
};

enum appliance_production_admin_enrollment_status {
    APPLIANCE_PRODUCTION_ADMIN_ENROLLMENT_OK = 0,
    APPLIANCE_PRODUCTION_ADMIN_ENROLLMENT_NULL = 1,
    APPLIANCE_PRODUCTION_ADMIN_ENROLLMENT_BAD_STATE = 2,
    APPLIANCE_PRODUCTION_ADMIN_ENROLLMENT_BAD_VERSION = 3,
    APPLIANCE_PRODUCTION_ADMIN_ENROLLMENT_ALREADY_INITIALIZED = 4,
    APPLIANCE_PRODUCTION_ADMIN_ENROLLMENT_UNAVAILABLE = 5,
    APPLIANCE_PRODUCTION_ADMIN_ENROLLMENT_BAD_SECRET = 6,
    APPLIANCE_PRODUCTION_ADMIN_ENROLLMENT_UNPROVISIONED = 7,
    APPLIANCE_PRODUCTION_ADMIN_ENROLLMENT_DENIED = 8,
    APPLIANCE_PRODUCTION_ADMIN_ENROLLMENT_THROTTLED = 9,
    APPLIANCE_PRODUCTION_ADMIN_ENROLLMENT_PUBLISH_FAILED = 10,
    APPLIANCE_PRODUCTION_ADMIN_ENROLLMENT_SESSION_FAILED = 11,
    APPLIANCE_PRODUCTION_ADMIN_ENROLLMENT_BUFFER_TOO_SMALL = 12,
    /*
     * ITEM-506: the credential model's refusals and the shape of the store.
     * BAD_LOGIN and WEAK_PASSWORD come from the model's policy; LEGACY_RECORD
     * says the store holds the old single-SHA-256 record, which is not a
     * credential and is migrated by setting a new login and password;
     * CREDENTIAL_REQUIRED refuses the *legacy* secret-only API once the store
     * holds a structured credential, so nothing silently downgrades the
     * account; NO_RANDOM says no salt source is bound, and provisioning
     * refuses rather than inventing one.
     */
    APPLIANCE_PRODUCTION_ADMIN_ENROLLMENT_BAD_LOGIN = 13,
    APPLIANCE_PRODUCTION_ADMIN_ENROLLMENT_WEAK_PASSWORD = 14,
    APPLIANCE_PRODUCTION_ADMIN_ENROLLMENT_LEGACY_RECORD = 15,
    APPLIANCE_PRODUCTION_ADMIN_ENROLLMENT_CREDENTIAL_REQUIRED = 16,
    APPLIANCE_PRODUCTION_ADMIN_ENROLLMENT_NO_RANDOM = 17,
};

/*
 * ITEM-506: the salt source for provisioning.  The caller binds the one owner
 * that already has entropy (the production DRBG in the kernel, a
 * deterministic generator in a host proof); without a binding,
 * `provision_credential()` refuses with NO_RANDOM and nothing is written.
 * The callback returns true only when it filled the whole buffer.
 */
typedef bool (*appliance_production_admin_enrollment_random_fn)(
    void *context,
    uint8_t *output,
    size_t length);

struct appliance_production_admin_enrollment {
    bool initialized;
    uint32_t version;
    enum appliance_production_admin_enrollment_state state;
    enum appliance_production_admin_enrollment_status last_status;
    struct appliance_production_auth *auth;
    struct appliance_persistent_secret_config *secret_config;
    bool provisioned;
    bool active;
    uint32_t denied_count;
    uint32_t throttled_count;
    uint32_t provisioned_count;
    uint32_t session_count;
    uint32_t failure_count;
    uint32_t token_counter;
    uint64_t locked_until_tick;
    appliance_production_admin_enrollment_random_fn random;
    void *random_context;
};

enum appliance_production_admin_enrollment_status
appliance_production_admin_enrollment_init(
    struct appliance_production_admin_enrollment *enrollment,
    struct appliance_production_auth *auth,
    struct appliance_persistent_secret_config *secret_config);
enum appliance_production_admin_enrollment_status
appliance_production_admin_enrollment_provision(
    struct appliance_production_admin_enrollment *enrollment,
    const char *secret,
    size_t secret_length);
/*
 * ITEM-465: clear the administrator credential from the local console.
 *
 * The owner, having lost (or mistyped and forgotten) the secret, was locked
 * out for good: `provision` refuses once a real record exists and no reset
 * existed.  This restores the appliance to the deferred first-boot state -
 * the persisted record is rewritten to the factory placeholder, so
 * `provision` works again - and revokes every issued session by resetting the
 * request gate.  It never runs over the network: the console is the only
 * caller, and the command that reaches it is the two-line `credentials reset` /
 * `credentials reset confirm` gesture (ITEM-506 renamed the ITEM-465 gesture
 * into the one account vocabulary).
 *
 * Nothing is fabricated and nothing is left half-done quietly: a failure to
 * rewrite the record or to reset the gate is refused with a named status.
 */
enum appliance_production_admin_enrollment_status
appliance_production_admin_enrollment_reset(
    struct appliance_production_admin_enrollment *enrollment);
enum appliance_production_admin_enrollment_status
appliance_production_admin_enrollment_authenticate(
    struct appliance_production_admin_enrollment *enrollment,
    const char *secret,
    size_t secret_length,
    uint64_t now_tick,
    uint32_t lifetime_ticks,
    uint8_t token_out[APPLIANCE_ADMIN_AUTH_TOKEN_LENGTH],
    uint32_t *session_id_out);
/*
 * ITEM-506: the one-account API the console and the panel call.
 *
 * `set_random` binds the salt source; provisioning refuses with NO_RANDOM
 * without it and never invents a salt.
 *
 * `provision_credential` is the single setter of the account: it validates the
 * login and password with the credential model, derives a salted PBKDF2
 * verifier, commits it as the newest record and publishes that verifier into
 * the request gate.  It replaces an existing structured credential - the
 * caller is the one that proves the current password before a change (the
 * console `credentials` flow does) - and it is also the explicit migration of
 * a legacy fingerprint record, which cannot be verified and is therefore
 * replaced, not reinterpreted.
 *
 * `authenticate_credential` reads the stored record, compares the login and
 * the password with the model, counts failures and locks the account after
 * the model's limit, and issues the same bounded bearer session the legacy
 * path issues.  A legacy record is refused with LEGACY_RECORD (an explicit
 * `provision_credential` migrates it), and an absent record with
 * UNPROVISIONED.
 *
 * `verify_credential` is the change conversation's current-password step: it
 * verifies the password against the stored login without issuing a session,
 * counts a failure and honours the same lockout - which it answers before the
 * record is even read, so a locked appliance does not disclose which kind of
 * record it holds.  It exists so a change proves the current password with the
 * account's own verifier instead of presenting a secret that would open
 * another session; the caller that forgets it would turn `credentials` into an
 * unauthenticated account takeover.  A legacy record answers LEGACY_RECORD
 * rather than UNPROVISIONED: the operator did set that secret once, so the
 * change is refused and the explicit reset is required before it can be
 * replaced without proof.
 *
 * The legacy `provision`/`authenticate` remain for the factory placeholder and
 * for a store that only ever held the old record; once the head is a
 * structured credential they refuse with CREDENTIAL_REQUIRED instead of
 * silently accepting a secret that no longer exists.
 */
enum appliance_production_admin_enrollment_status
appliance_production_admin_enrollment_set_random(
    struct appliance_production_admin_enrollment *enrollment,
    appliance_production_admin_enrollment_random_fn random,
    void *random_context);
enum appliance_production_admin_enrollment_status
appliance_production_admin_enrollment_provision_credential(
    struct appliance_production_admin_enrollment *enrollment,
    const char *login,
    size_t login_length,
    const char *password,
    size_t password_length,
    uint64_t now_tick);
enum appliance_production_admin_enrollment_status
appliance_production_admin_enrollment_authenticate_credential(
    struct appliance_production_admin_enrollment *enrollment,
    const char *login,
    size_t login_length,
    const char *password,
    size_t password_length,
    uint64_t now_tick,
    uint32_t lifetime_ticks,
    uint8_t token_out[APPLIANCE_ADMIN_AUTH_TOKEN_LENGTH],
    uint32_t *session_id_out);
enum appliance_production_admin_enrollment_status
appliance_production_admin_enrollment_verify_credential(
    struct appliance_production_admin_enrollment *enrollment,
    const char *password,
    size_t password_length,
    uint64_t now_tick);
/*
 * ITEM-619: the same pair check as `authenticate_credential`, split so the
 * caller can advance the PBKDF2 job from the production event loop instead of
 * blocking it for the whole derivation.  `begin` performs every state,
 * lockout and record refusal, then starts the job.  `finish` issues the one
 * bounded session after the job completed (or records one failure), and
 * zeroes the job on both paths.  The KDF count, salt, verifier comparison and
 * lockout are the existing ones; only the blocking changes.
 */
enum appliance_production_admin_enrollment_status
appliance_production_admin_enrollment_begin_credential_check(
    struct appliance_production_admin_enrollment *enrollment,
    const char *login,
    size_t login_length,
    const char *password,
    size_t password_length,
    uint64_t now_tick,
    struct appliance_admin_credential_check *check);
enum appliance_production_admin_enrollment_status
appliance_production_admin_enrollment_finish_credential_check(
    struct appliance_production_admin_enrollment *enrollment,
    struct appliance_admin_credential_check *check,
    uint64_t now_tick,
    uint32_t lifetime_ticks,
    uint8_t token_out[APPLIANCE_ADMIN_AUTH_TOKEN_LENGTH],
    uint32_t *session_id_out);
/*
 * ITEM-598: prove a login/password pair without publishing the request gate or
 * opening a session.  The caller owns the bounded verification value on its
 * stack, must keep it private, and clears it after the follow-up operation.
 *
 * This split exists for the local login that unlocks the encrypted host seed:
 * the account must be proved before the seed store is advanced, but neither the
 * request gate nor a bearer session may become live until that seed has been
 * accepted.  Publication re-reads the structured record and refuses a value
 * that no longer matches it, so a credential change cannot be raced by an old
 * verification.
 */
struct appliance_production_admin_enrollment_verification {
    bool valid;
    /* ITEM-611: the stored account's PBKDF2 count, for the login trace. */
    uint32_t iterations;
    uint8_t verifier[APPLIANCE_ADMIN_AUTH_CREDENTIAL_LENGTH];
};
void appliance_production_admin_enrollment_verification_clear(
    struct appliance_production_admin_enrollment_verification *verification);
enum appliance_production_admin_enrollment_status
appliance_production_admin_enrollment_verify_credential_pair(
    struct appliance_production_admin_enrollment *enrollment,
    const char *login,
    size_t login_length,
    const char *password,
    size_t password_length,
    uint64_t now_tick,
    struct appliance_production_admin_enrollment_verification
        *verification_out);
enum appliance_production_admin_enrollment_status
appliance_production_admin_enrollment_publish_verification(
    struct appliance_production_admin_enrollment *enrollment,
    struct appliance_production_admin_enrollment_verification
        *verification);
bool appliance_production_admin_enrollment_credential_set(
    const struct appliance_production_admin_enrollment *enrollment);
/*
 * ITEM-506 increment 5: may the management plane serve with this enrollment?
 *
 * True only when the enrollment is initialized and its store head is the
 * structured account record (login + PBKDF2 verifier) - the same reader the
 * console and the panel authenticate against.  An absent store, the factory
 * placeholder and a pre-ITEM-506 legacy fingerprint all answer false: none of
 * them is an account the operator can sign in with, and the console
 * conversation is what sets one (a legacy head needs the documented
 * `credentials reset` gesture first).  The serving path asks this only while
 * its state volume is ready; with no ready state volume there is no durable
 * account and the caller keeps its documented per-boot-certificate behaviour.
 */
bool appliance_production_admin_enrollment_serving_allowed(
    const struct appliance_production_admin_enrollment *enrollment);
bool appliance_production_admin_enrollment_published(
    const struct appliance_production_admin_enrollment *enrollment);
/*
 * ITEM-516: publish the persisted credential at boot, without a local login.
 *
 * The boot calls this only when the system record says
 * `v1:security.panel.after-boot=open`.  The structured record already holds
 * the verifier (the PBKDF2 output the login path publishes after proving the
 * password), so the policy can publish exactly what a successful local login
 * would have published; the failure count, the lockout and the sessions are
 * untouched, and the decision is the operator's durable one.
 *
 * The honesty rules are the same as every other entry: an uninitialized owner,
 * an unavailable store, a legacy fingerprint or the factory placeholder refuse
 * by name and publish nothing, so the gate stays closed and the boot says so.
 */
enum appliance_production_admin_enrollment_status
appliance_production_admin_enrollment_publish_policy(
    struct appliance_production_admin_enrollment *enrollment);
/*
 * Derive the verifier from a presented secret and compute the exact record
 * fingerprint the first-boot flow commits (kind ADMIN_CREDENTIAL, generation
 * 1).  The test proof and any future provisioning caller use this instead of
 * a second derivation.
 */
enum appliance_production_admin_enrollment_status
appliance_production_admin_enrollment_credential_fingerprint(
    const char *secret,
    size_t secret_length,
    uint8_t digest_out[SHA256_DIGEST_SIZE]);
enum appliance_production_admin_enrollment_state
appliance_production_admin_enrollment_state(
    const struct appliance_production_admin_enrollment *enrollment);
enum appliance_production_admin_enrollment_status
appliance_production_admin_enrollment_last_status(
    const struct appliance_production_admin_enrollment *enrollment);
const char *
appliance_production_admin_enrollment_state_string(
    enum appliance_production_admin_enrollment_state state);
const char *
appliance_production_admin_enrollment_status_string(
    enum appliance_production_admin_enrollment_status status);
enum appliance_production_admin_enrollment_status
appliance_production_admin_enrollment_render(
    const struct appliance_production_admin_enrollment *enrollment,
    char *buffer,
    size_t buffer_size,
    size_t *length_out);

#endif
