#include "appliance_production_admin_enrollment.h"

#include "appliance_secret_protection.h"
#include "sha256.h"

#include <string.h>

/*
 * ITEM-401 administrator credential enrollment.
 *
 * The record contract is the one the ITEM-363 first-boot flow already writes:
 *
 *   verifier = SHA-256("HFOC-ADMIN-CREDENTIAL-V1:" || secret)
 *   record   = appliance_secret_protection_fingerprint(
 *                  ADMIN_CREDENTIAL, generation 1, verifier, 32)
 *
 * The module re-derives the verifier from the presented secret, proves the
 * fingerprint equals the persisted record, publishes the verifier into the
 * ITEM-365 request gate and issues one bounded local bearer session.  See the
 * header for the honesty rules.
 */

#define ADMIN_ENROLLMENT_CREDENTIAL_DOMAIN "HFOC-ADMIN-CREDENTIAL-V1:"
#define ADMIN_ENROLLMENT_TOKEN_DOMAIN "HFOC-ADMIN-TOKEN-V1:"

static const char admin_enrollment_token_alphabet[65U] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";

const uint8_t appliance_production_admin_enrollment_factory_credential
    [APPLIANCE_PRODUCTION_ADMIN_ENROLLMENT_FACTORY_CREDENTIAL_LENGTH] = {
    0x48U, 0x46U, 0x4FU, 0x43U, 0x2DU, 0x41U, 0x50U, 0x50U,
    0x4CU, 0x49U, 0x41U, 0x4EU, 0x43U, 0x45U, 0x2DU, 0x56U,
    0x31U, 0x2DU, 0x50U, 0x52U, 0x4FU, 0x56U, 0x49U, 0x53U,
    0x49U, 0x4FU, 0x4EU, 0x49U, 0x4EU, 0x47U, 0x2DU, 0x31U
};

static void admin_enrollment_zero(void *destination, size_t length)
{
    unsigned char *bytes = destination;
    size_t index;

    for (index = 0U; index < length; ++index) {
        bytes[index] = 0U;
    }
}

static bool admin_enrollment_initialized(
    const struct appliance_production_admin_enrollment *enrollment)
{
    return enrollment != NULL && enrollment->initialized &&
           enrollment->version ==
               APPLIANCE_PRODUCTION_ADMIN_ENROLLMENT_VERSION;
}

static bool admin_enrollment_secret_valid(
    const char *secret,
    size_t secret_length)
{
    return secret != NULL && secret_length != 0U &&
           secret_length <= APPLIANCE_PRODUCTION_ADMIN_ENROLLMENT_SECRET_MAX;
}

static bool admin_enrollment_derive_verifier(
    const char *secret,
    size_t secret_length,
    uint8_t verifier[APPLIANCE_ADMIN_AUTH_CREDENTIAL_LENGTH])
{
    struct sha256 context;
    enum sha256_status status;

    status = sha256_init(&context);
    if (status != SHA256_OK) {
        return false;
    }
    status = sha256_update(
        &context, ADMIN_ENROLLMENT_CREDENTIAL_DOMAIN,
        sizeof(ADMIN_ENROLLMENT_CREDENTIAL_DOMAIN) - 1U);
    if (status == SHA256_OK) {
        status = sha256_update(&context, secret, secret_length);
    }
    if (status == SHA256_OK) {
        status = sha256_final(&context, verifier);
    }
    admin_enrollment_zero(&context, sizeof(context));
    if (status != SHA256_OK) {
        admin_enrollment_zero(verifier,
                              APPLIANCE_ADMIN_AUTH_CREDENTIAL_LENGTH);
        return false;
    }
    return true;
}

static bool admin_enrollment_record_fingerprint(
    const uint8_t verifier[APPLIANCE_ADMIN_AUTH_CREDENTIAL_LENGTH],
    uint8_t digest[SHA256_DIGEST_SIZE])
{
    return appliance_secret_protection_fingerprint(
               APPLIANCE_SECRET_PROTECTION_KIND_ADMIN_CREDENTIAL,
               APPLIANCE_PRODUCTION_ADMIN_ENROLLMENT_GENERATION,
               verifier, APPLIANCE_ADMIN_AUTH_CREDENTIAL_LENGTH,
               digest) == APPLIANCE_SECRET_PROTECTION_OK;
}

static bool admin_enrollment_factory_fingerprint(
    uint8_t digest[SHA256_DIGEST_SIZE])
{
    return appliance_secret_protection_fingerprint(
               APPLIANCE_SECRET_PROTECTION_KIND_ADMIN_CREDENTIAL,
               APPLIANCE_PRODUCTION_ADMIN_ENROLLMENT_GENERATION,
               appliance_production_admin_enrollment_factory_credential,
               APPLIANCE_PRODUCTION_ADMIN_ENROLLMENT_FACTORY_CREDENTIAL_LENGTH,
               digest) == APPLIANCE_SECRET_PROTECTION_OK;
}

static bool admin_enrollment_persisted(
    const struct appliance_production_admin_enrollment *enrollment,
    uint8_t digest[SHA256_DIGEST_SIZE])
{
    return enrollment->secret_config != NULL &&
           enrollment->secret_config->initialized &&
           appliance_persistent_secret_config_admin(
               enrollment->secret_config, digest);
}

static bool admin_enrollment_persisted_is(
    const struct appliance_production_admin_enrollment *enrollment,
    const uint8_t digest[SHA256_DIGEST_SIZE])
{
    uint8_t stored[SHA256_DIGEST_SIZE];
    bool matches;

    admin_enrollment_zero(stored, sizeof(stored));
    if (!admin_enrollment_persisted(enrollment, stored)) {
        return false;
    }
    matches = memcmp(stored, digest, SHA256_DIGEST_SIZE) == 0;
    admin_enrollment_zero(stored, sizeof(stored));
    return matches;
}

static bool admin_enrollment_credential_set_internal(
    const struct appliance_production_admin_enrollment *enrollment)
{
    struct appliance_admin_credential credential;
    bool present;

    if (enrollment == NULL || enrollment->secret_config == NULL ||
        !enrollment->secret_config->initialized) {
        return false;
    }
    admin_enrollment_zero(&credential, sizeof(credential));
    present = appliance_persistent_secret_config_admin_credential(
                  enrollment->secret_config, &credential) ==
              APPLIANCE_PERSISTENT_SECRET_CONFIG_OK;
    /*
     * The read copies the salt and the PBKDF2 verifier - the material bearer
     * tokens derive from - so the copy is zeroed here, exactly like every
     * other local in this module.  This helper runs on every refresh and on
     * every authentication attempt, so it is the path where a copy would
     * linger most often.
     */
    admin_enrollment_zero(&credential, sizeof(credential));
    return present;
}

static void admin_enrollment_refresh(
    struct appliance_production_admin_enrollment *enrollment)
{
    uint8_t stored[SHA256_DIGEST_SIZE];
    uint8_t factory[SHA256_DIGEST_SIZE];

    if (!enrollment->initialized) {
        enrollment->state =
            APPLIANCE_PRODUCTION_ADMIN_ENROLLMENT_STATE_UNINITIALIZED;
        return;
    }
    admin_enrollment_zero(stored, sizeof(stored));
    admin_enrollment_zero(factory, sizeof(factory));
    if (enrollment->secret_config == NULL ||
        !enrollment->secret_config->initialized ||
        !admin_enrollment_factory_fingerprint(factory)) {
        enrollment->provisioned = false;
        enrollment->state =
            APPLIANCE_PRODUCTION_ADMIN_ENROLLMENT_STATE_UNAVAILABLE;
        return;
    }
    /*
     * ITEM-506: a structured credential at the head is the provisioned state,
     * whatever the legacy record behind it says.
     */
    if (admin_enrollment_credential_set_internal(enrollment)) {
        enrollment->provisioned = true;
        if (enrollment->active &&
            appliance_production_auth_available(enrollment->auth)) {
            enrollment->state =
                APPLIANCE_PRODUCTION_ADMIN_ENROLLMENT_STATE_ACTIVE;
            return;
        }
        enrollment->state =
            APPLIANCE_PRODUCTION_ADMIN_ENROLLMENT_STATE_AWAITING_SECRET;
        return;
    }
    if (!appliance_persistent_secret_config_admin(
            enrollment->secret_config, stored)) {
        /*
         * ITEM-403: ordinary first boot defers the administrator credential,
         * so an initialized store with no record is provisionable instead of
         * unavailable.  Legacy factory records are still refused below.
         */
        enrollment->provisioned = false;
        admin_enrollment_zero(factory, sizeof(factory));
        enrollment->state =
            APPLIANCE_PRODUCTION_ADMIN_ENROLLMENT_STATE_UNPROVISIONED;
        return;
    }
    enrollment->provisioned =
        memcmp(stored, factory, SHA256_DIGEST_SIZE) != 0;
    admin_enrollment_zero(stored, sizeof(stored));
    admin_enrollment_zero(factory, sizeof(factory));
    if (!enrollment->provisioned) {
        enrollment->state =
            APPLIANCE_PRODUCTION_ADMIN_ENROLLMENT_STATE_UNPROVISIONED;
        return;
    }
    if (enrollment->active &&
        appliance_production_auth_available(enrollment->auth)) {
        enrollment->state =
            APPLIANCE_PRODUCTION_ADMIN_ENROLLMENT_STATE_ACTIVE;
        return;
    }
    enrollment->state =
        APPLIANCE_PRODUCTION_ADMIN_ENROLLMENT_STATE_AWAITING_SECRET;
}

static enum appliance_production_admin_enrollment_status
admin_enrollment_publish(
    struct appliance_production_admin_enrollment *enrollment,
    const uint8_t verifier[APPLIANCE_ADMIN_AUTH_CREDENTIAL_LENGTH])
{
    struct appliance_admin_auth_credential credential;

    admin_enrollment_zero(&credential, sizeof(credential));
    credential.version = APPLIANCE_ADMIN_CREDENTIAL_VERSION;
    memcpy(credential.verifier, verifier, sizeof(credential.verifier));
    if (appliance_production_auth_publish_credential(
            enrollment->auth, &credential) != APPLIANCE_PRODUCTION_AUTH_OK) {
        admin_enrollment_zero(&credential, sizeof(credential));
        enrollment->state =
            APPLIANCE_PRODUCTION_ADMIN_ENROLLMENT_STATE_FAILED;
        enrollment->last_status =
            APPLIANCE_PRODUCTION_ADMIN_ENROLLMENT_PUBLISH_FAILED;
        return APPLIANCE_PRODUCTION_ADMIN_ENROLLMENT_PUBLISH_FAILED;
    }
    admin_enrollment_zero(&credential, sizeof(credential));
    enrollment->active = true;
    return APPLIANCE_PRODUCTION_ADMIN_ENROLLMENT_OK;
}

static void admin_enrollment_token(
    const uint8_t verifier[APPLIANCE_ADMIN_AUTH_CREDENTIAL_LENGTH],
    uint64_t salt,
    uint32_t counter,
    uint8_t token[APPLIANCE_ADMIN_AUTH_TOKEN_LENGTH])
{
    struct sha256 context;
    uint8_t digest[SHA256_DIGEST_SIZE];
    uint8_t numeric[12];
    size_t index;

    admin_enrollment_zero(digest, sizeof(digest));
    admin_enrollment_zero(numeric, sizeof(numeric));
    for (index = 0U; index < 8U; ++index) {
        numeric[index] = (uint8_t)(salt >> (8U * index));
    }
    for (index = 0U; index < 4U; ++index) {
        numeric[8U + index] = (uint8_t)(counter >> (8U * index));
    }
    if (sha256_init(&context) != SHA256_OK ||
        sha256_update(&context, ADMIN_ENROLLMENT_TOKEN_DOMAIN,
                      sizeof(ADMIN_ENROLLMENT_TOKEN_DOMAIN) - 1U) !=
            SHA256_OK ||
        sha256_update(&context, verifier,
                      APPLIANCE_ADMIN_AUTH_CREDENTIAL_LENGTH) != SHA256_OK ||
        sha256_update(&context, numeric, sizeof(numeric)) != SHA256_OK ||
        sha256_final(&context, digest) != SHA256_OK) {
        /*
         * A failed derivation must not fall back to a predictable token; the
         * caller refuses the session instead.
         */
        admin_enrollment_zero(token, APPLIANCE_ADMIN_AUTH_TOKEN_LENGTH);
        admin_enrollment_zero(&context, sizeof(context));
        admin_enrollment_zero(digest, sizeof(digest));
        return;
    }
    for (index = 0U; index < APPLIANCE_ADMIN_AUTH_TOKEN_LENGTH; ++index) {
        token[index] =
            (uint8_t)admin_enrollment_token_alphabet[digest[index] & 0x3fU];
    }
    admin_enrollment_zero(&context, sizeof(context));
    admin_enrollment_zero(digest, sizeof(digest));
    admin_enrollment_zero(numeric, sizeof(numeric));
}

enum appliance_production_admin_enrollment_status
appliance_production_admin_enrollment_init(
    struct appliance_production_admin_enrollment *enrollment,
    struct appliance_production_auth *auth,
    struct appliance_persistent_secret_config *secret_config)
{
    if (enrollment == NULL || auth == NULL) {
        return APPLIANCE_PRODUCTION_ADMIN_ENROLLMENT_NULL;
    }
    if (enrollment->initialized) {
        return APPLIANCE_PRODUCTION_ADMIN_ENROLLMENT_ALREADY_INITIALIZED;
    }
    admin_enrollment_zero(enrollment, sizeof(*enrollment));
    enrollment->version = APPLIANCE_PRODUCTION_ADMIN_ENROLLMENT_VERSION;
    enrollment->auth = auth;
    enrollment->secret_config = secret_config;
    enrollment->initialized = true;
    enrollment->last_status = APPLIANCE_PRODUCTION_ADMIN_ENROLLMENT_OK;
    admin_enrollment_refresh(enrollment);
    return APPLIANCE_PRODUCTION_ADMIN_ENROLLMENT_OK;
}

enum appliance_production_admin_enrollment_status
appliance_production_admin_enrollment_provision(
    struct appliance_production_admin_enrollment *enrollment,
    const char *secret,
    size_t secret_length)
{
    uint8_t verifier[APPLIANCE_ADMIN_AUTH_CREDENTIAL_LENGTH];
    uint8_t fingerprint[SHA256_DIGEST_SIZE];

    if (!admin_enrollment_initialized(enrollment)) {
        return enrollment == NULL
                   ? APPLIANCE_PRODUCTION_ADMIN_ENROLLMENT_NULL
                   : APPLIANCE_PRODUCTION_ADMIN_ENROLLMENT_BAD_STATE;
    }
    if (!admin_enrollment_secret_valid(secret, secret_length)) {
        enrollment->last_status =
            APPLIANCE_PRODUCTION_ADMIN_ENROLLMENT_BAD_SECRET;
        return enrollment->last_status;
    }
    admin_enrollment_refresh(enrollment);
    if (enrollment->state ==
        APPLIANCE_PRODUCTION_ADMIN_ENROLLMENT_STATE_UNAVAILABLE) {
        enrollment->last_status =
            APPLIANCE_PRODUCTION_ADMIN_ENROLLMENT_UNAVAILABLE;
        return enrollment->last_status;
    }
    if (admin_enrollment_credential_set_internal(enrollment)) {
        ++enrollment->denied_count;
        enrollment->last_status =
            APPLIANCE_PRODUCTION_ADMIN_ENROLLMENT_CREDENTIAL_REQUIRED;
        return enrollment->last_status;
    }
    if (enrollment->provisioned) {
        ++enrollment->denied_count;
        enrollment->last_status =
            APPLIANCE_PRODUCTION_ADMIN_ENROLLMENT_DENIED;
        return enrollment->last_status;
    }
    if (enrollment->locked_until_tick > 0U) {
        ++enrollment->throttled_count;
        enrollment->last_status =
            APPLIANCE_PRODUCTION_ADMIN_ENROLLMENT_THROTTLED;
        return enrollment->last_status;
    }
    admin_enrollment_zero(verifier, sizeof(verifier));
    admin_enrollment_zero(fingerprint, sizeof(fingerprint));
    if (!admin_enrollment_derive_verifier(
            secret, secret_length, verifier) ||
        !admin_enrollment_record_fingerprint(verifier, fingerprint)) {
        admin_enrollment_zero(verifier, sizeof(verifier));
        admin_enrollment_zero(fingerprint, sizeof(fingerprint));
        enrollment->last_status =
            APPLIANCE_PRODUCTION_ADMIN_ENROLLMENT_BAD_SECRET;
        return enrollment->last_status;
    }
    if (appliance_persistent_secret_config_commit_admin(
            enrollment->secret_config, fingerprint) !=
        APPLIANCE_PERSISTENT_SECRET_CONFIG_OK) {
        admin_enrollment_zero(verifier, sizeof(verifier));
        admin_enrollment_zero(fingerprint, sizeof(fingerprint));
        enrollment->state =
            APPLIANCE_PRODUCTION_ADMIN_ENROLLMENT_STATE_FAILED;
        enrollment->last_status =
            APPLIANCE_PRODUCTION_ADMIN_ENROLLMENT_PUBLISH_FAILED;
        return enrollment->last_status;
    }
    admin_enrollment_zero(fingerprint, sizeof(fingerprint));
    enrollment->provisioned = true;
    ++enrollment->provisioned_count;
    {
        const enum appliance_production_admin_enrollment_status status =
            admin_enrollment_publish(enrollment, verifier);
        admin_enrollment_zero(verifier, sizeof(verifier));
        admin_enrollment_refresh(enrollment);
        if (status != APPLIANCE_PRODUCTION_ADMIN_ENROLLMENT_OK) {
            return status;
        }
    }
    enrollment->last_status = APPLIANCE_PRODUCTION_ADMIN_ENROLLMENT_OK;
    return enrollment->last_status;
}

/*
 * ITEM-465: restore the deferred first-boot state from the local console.
 *
 * The persisted record is the source of truth: rewriting it to the factory
 * placeholder makes `refresh()` read the appliance as UNPROVISIONED and
 * `provision()` work again, exactly as if the operator's secret had never been
 * committed.  The request gate is reset afterwards, which closes every issued
 * session and clears the in-memory verifier, so a token copied out before the
 * reset stops authenticating.  The record is committed first on purpose: a
 * failure there leaves the gate untouched and is refused, while a gate reset
 * that somehow failed after the commit still leaves the disk unprovisioned and
 * fail-closed on the next boot.
 */
enum appliance_production_admin_enrollment_status
appliance_production_admin_enrollment_reset(
    struct appliance_production_admin_enrollment *enrollment)
{
    uint8_t factory[SHA256_DIGEST_SIZE];
    enum appliance_persistent_secret_config_status commit_status;

    if (enrollment == NULL) {
        return APPLIANCE_PRODUCTION_ADMIN_ENROLLMENT_NULL;
    }
    if (!admin_enrollment_initialized(enrollment)) {
        return APPLIANCE_PRODUCTION_ADMIN_ENROLLMENT_BAD_STATE;
    }
    admin_enrollment_refresh(enrollment);
    if (enrollment->state ==
        APPLIANCE_PRODUCTION_ADMIN_ENROLLMENT_STATE_UNAVAILABLE) {
        enrollment->last_status =
            APPLIANCE_PRODUCTION_ADMIN_ENROLLMENT_UNAVAILABLE;
        return enrollment->last_status;
    }
    admin_enrollment_zero(factory, sizeof(factory));
    if (!admin_enrollment_factory_fingerprint(factory)) {
        enrollment->last_status =
            APPLIANCE_PRODUCTION_ADMIN_ENROLLMENT_BAD_STATE;
        return enrollment->last_status;
    }
    commit_status = appliance_persistent_secret_config_commit_admin(
        enrollment->secret_config, factory);
    admin_enrollment_zero(factory, sizeof(factory));
    if (commit_status != APPLIANCE_PERSISTENT_SECRET_CONFIG_OK) {
        enrollment->state =
            APPLIANCE_PRODUCTION_ADMIN_ENROLLMENT_STATE_FAILED;
        enrollment->last_status =
            APPLIANCE_PRODUCTION_ADMIN_ENROLLMENT_PUBLISH_FAILED;
        return enrollment->last_status;
    }
    if (appliance_production_auth_available(enrollment->auth) &&
        appliance_production_auth_reset(enrollment->auth) !=
            APPLIANCE_PRODUCTION_AUTH_OK) {
        enrollment->active = false;
        enrollment->state =
            APPLIANCE_PRODUCTION_ADMIN_ENROLLMENT_STATE_FAILED;
        enrollment->last_status =
            APPLIANCE_PRODUCTION_ADMIN_ENROLLMENT_PUBLISH_FAILED;
        return enrollment->last_status;
    }
    enrollment->active = false;
    enrollment->provisioned = false;
    enrollment->failure_count = 0U;
    enrollment->locked_until_tick = 0U;
    admin_enrollment_refresh(enrollment);
    enrollment->last_status = APPLIANCE_PRODUCTION_ADMIN_ENROLLMENT_OK;
    return enrollment->last_status;
}

/*
 * ITEM-506: one failure count and one lockout decision for both entries (the
 * legacy secret-only path and the login+password path), so the console and the
 * panel cannot drift apart.
 */
static enum appliance_production_admin_enrollment_status
admin_enrollment_record_failure(
    struct appliance_production_admin_enrollment *enrollment,
    uint64_t now_tick)
{
    ++enrollment->denied_count;
    ++enrollment->failure_count;
    if (enrollment->failure_count >=
        APPLIANCE_PRODUCTION_ADMIN_ENROLLMENT_FAILURE_LIMIT) {
        if (now_tick >
            UINT64_MAX - APPLIANCE_PRODUCTION_ADMIN_ENROLLMENT_COOLDOWN_TICKS) {
            enrollment->last_status =
                APPLIANCE_PRODUCTION_ADMIN_ENROLLMENT_BAD_STATE;
            return enrollment->last_status;
        }
        enrollment->locked_until_tick =
            now_tick + APPLIANCE_PRODUCTION_ADMIN_ENROLLMENT_COOLDOWN_TICKS;
        ++enrollment->throttled_count;
        enrollment->last_status =
            APPLIANCE_PRODUCTION_ADMIN_ENROLLMENT_THROTTLED;
        return enrollment->last_status;
    }
    enrollment->last_status = APPLIANCE_PRODUCTION_ADMIN_ENROLLMENT_DENIED;
    return enrollment->last_status;
}

/*
 * ITEM-506: the shared tail of a successful credential check: reset the
 * failure state, publish the verifier into the request gate when it is not
 * live yet, derive the builder token and open one bounded local session.
 */
static enum appliance_production_admin_enrollment_status
admin_enrollment_finish_session(
    struct appliance_production_admin_enrollment *enrollment,
    const uint8_t verifier[APPLIANCE_ADMIN_AUTH_CREDENTIAL_LENGTH],
    uint64_t now_tick,
    uint32_t lifetime_ticks,
    uint8_t token_out[APPLIANCE_ADMIN_AUTH_TOKEN_LENGTH],
    uint32_t *session_id_out)
{
    struct appliance_admin_auth_proof proof;
    enum appliance_admin_auth_status session_status;

    enrollment->failure_count = 0U;
    enrollment->locked_until_tick = 0U;
    if (!enrollment->active ||
        !appliance_production_auth_available(enrollment->auth)) {
        const enum appliance_production_admin_enrollment_status publish_status =
            admin_enrollment_publish(enrollment, verifier);
        if (publish_status != APPLIANCE_PRODUCTION_ADMIN_ENROLLMENT_OK) {
            return publish_status;
        }
    }
    admin_enrollment_zero(&proof, sizeof(proof));
    proof.version = APPLIANCE_ADMIN_CREDENTIAL_VERSION;
    memcpy(proof.proof, verifier, sizeof(proof.proof));
    admin_enrollment_token(verifier, now_tick, enrollment->token_counter,
                           token_out);
    ++enrollment->token_counter;
    session_status = appliance_admin_auth_authenticate_local(
        &enrollment->auth->admin, &proof, token_out, now_tick,
        lifetime_ticks, session_id_out);
    admin_enrollment_zero(&proof, sizeof(proof));
    switch (session_status) {
    case APPLIANCE_ADMIN_AUTH_OK:
        ++enrollment->session_count;
        enrollment->state =
            APPLIANCE_PRODUCTION_ADMIN_ENROLLMENT_STATE_ACTIVE;
        enrollment->last_status = APPLIANCE_PRODUCTION_ADMIN_ENROLLMENT_OK;
        return enrollment->last_status;
    case APPLIANCE_ADMIN_AUTH_AUTH_THROTTLED:
        ++enrollment->throttled_count;
        enrollment->last_status =
            APPLIANCE_PRODUCTION_ADMIN_ENROLLMENT_THROTTLED;
        return enrollment->last_status;
    default:
        ++enrollment->denied_count;
        enrollment->last_status =
            APPLIANCE_PRODUCTION_ADMIN_ENROLLMENT_SESSION_FAILED;
        return enrollment->last_status;
    }
}

enum appliance_production_admin_enrollment_status
appliance_production_admin_enrollment_authenticate(
    struct appliance_production_admin_enrollment *enrollment,
    const char *secret,
    size_t secret_length,
    uint64_t now_tick,
    uint32_t lifetime_ticks,
    uint8_t token_out[APPLIANCE_ADMIN_AUTH_TOKEN_LENGTH],
    uint32_t *session_id_out)
{
    uint8_t verifier[APPLIANCE_ADMIN_AUTH_CREDENTIAL_LENGTH];
    uint8_t fingerprint[SHA256_DIGEST_SIZE];
    enum appliance_production_admin_enrollment_status status;

    if (enrollment == NULL || token_out == NULL || session_id_out == NULL) {
        return APPLIANCE_PRODUCTION_ADMIN_ENROLLMENT_NULL;
    }
    if (!admin_enrollment_initialized(enrollment)) {
        return APPLIANCE_PRODUCTION_ADMIN_ENROLLMENT_BAD_STATE;
    }
    if (!admin_enrollment_secret_valid(secret, secret_length) ||
        lifetime_ticks == 0U ||
        lifetime_ticks > APPLIANCE_ADMIN_AUTH_SESSION_LIFETIME_MAX) {
        enrollment->last_status =
            APPLIANCE_PRODUCTION_ADMIN_ENROLLMENT_BAD_SECRET;
        return enrollment->last_status;
    }
    admin_enrollment_refresh(enrollment);
    if (enrollment->state ==
        APPLIANCE_PRODUCTION_ADMIN_ENROLLMENT_STATE_UNAVAILABLE) {
        enrollment->last_status =
            APPLIANCE_PRODUCTION_ADMIN_ENROLLMENT_UNAVAILABLE;
        return enrollment->last_status;
    }
    /*
     * ITEM-506: once the head is a structured credential, the legacy
     * secret-only path must refuse instead of presenting a secret that is no
     * longer the account.
     */
    if (admin_enrollment_credential_set_internal(enrollment)) {
        ++enrollment->denied_count;
        enrollment->last_status =
            APPLIANCE_PRODUCTION_ADMIN_ENROLLMENT_CREDENTIAL_REQUIRED;
        return enrollment->last_status;
    }
    if (!enrollment->provisioned) {
        ++enrollment->denied_count;
        enrollment->last_status =
            APPLIANCE_PRODUCTION_ADMIN_ENROLLMENT_UNPROVISIONED;
        return enrollment->last_status;
    }
    if (enrollment->locked_until_tick > now_tick) {
        ++enrollment->throttled_count;
        enrollment->last_status =
            APPLIANCE_PRODUCTION_ADMIN_ENROLLMENT_THROTTLED;
        return enrollment->last_status;
    }
    admin_enrollment_zero(verifier, sizeof(verifier));
    admin_enrollment_zero(fingerprint, sizeof(fingerprint));
    if (!admin_enrollment_derive_verifier(
            secret, secret_length, verifier) ||
        !admin_enrollment_record_fingerprint(verifier, fingerprint)) {
        admin_enrollment_zero(verifier, sizeof(verifier));
        admin_enrollment_zero(fingerprint, sizeof(fingerprint));
        enrollment->last_status =
            APPLIANCE_PRODUCTION_ADMIN_ENROLLMENT_BAD_SECRET;
        return enrollment->last_status;
    }
    if (!admin_enrollment_persisted_is(enrollment, fingerprint)) {
        admin_enrollment_zero(verifier, sizeof(verifier));
        admin_enrollment_zero(fingerprint, sizeof(fingerprint));
        return admin_enrollment_record_failure(enrollment, now_tick);
    }
    admin_enrollment_zero(fingerprint, sizeof(fingerprint));
    status = admin_enrollment_finish_session(
        enrollment, verifier, now_tick, lifetime_ticks, token_out,
        session_id_out);
    admin_enrollment_zero(verifier, sizeof(verifier));
    return status;
}

enum appliance_production_admin_enrollment_status
appliance_production_admin_enrollment_set_random(
    struct appliance_production_admin_enrollment *enrollment,
    appliance_production_admin_enrollment_random_fn random,
    void *random_context)
{
    if (enrollment == NULL) {
        return APPLIANCE_PRODUCTION_ADMIN_ENROLLMENT_NULL;
    }
    if (!admin_enrollment_initialized(enrollment)) {
        return APPLIANCE_PRODUCTION_ADMIN_ENROLLMENT_BAD_STATE;
    }
    enrollment->random = random;
    enrollment->random_context = random_context;
    return APPLIANCE_PRODUCTION_ADMIN_ENROLLMENT_OK;
}

enum appliance_production_admin_enrollment_status
appliance_production_admin_enrollment_provision_credential(
    struct appliance_production_admin_enrollment *enrollment,
    const char *login,
    size_t login_length,
    const char *password,
    size_t password_length,
    uint64_t now_tick)
{
    struct appliance_admin_credential credential;
    enum appliance_admin_credential_status credential_status;
    enum appliance_production_admin_enrollment_status status;
    uint8_t salt[APPLIANCE_ADMIN_CREDENTIAL_SALT_LENGTH];

    if (enrollment == NULL || login == NULL || password == NULL) {
        return APPLIANCE_PRODUCTION_ADMIN_ENROLLMENT_NULL;
    }
    if (!admin_enrollment_initialized(enrollment)) {
        return APPLIANCE_PRODUCTION_ADMIN_ENROLLMENT_BAD_STATE;
    }
    credential_status = appliance_admin_credential_validate_login(
        login, login_length);
    if (credential_status != APPLIANCE_ADMIN_CREDENTIAL_OK) {
        enrollment->last_status =
            APPLIANCE_PRODUCTION_ADMIN_ENROLLMENT_BAD_LOGIN;
        return enrollment->last_status;
    }
    credential_status = appliance_admin_credential_validate_password(
        password, password_length, login, login_length);
    if (credential_status == APPLIANCE_ADMIN_CREDENTIAL_WEAK_PASSWORD) {
        enrollment->last_status =
            APPLIANCE_PRODUCTION_ADMIN_ENROLLMENT_WEAK_PASSWORD;
        return enrollment->last_status;
    }
    if (credential_status != APPLIANCE_ADMIN_CREDENTIAL_OK) {
        enrollment->last_status =
            APPLIANCE_PRODUCTION_ADMIN_ENROLLMENT_BAD_SECRET;
        return enrollment->last_status;
    }
    admin_enrollment_refresh(enrollment);
    if (enrollment->state ==
        APPLIANCE_PRODUCTION_ADMIN_ENROLLMENT_STATE_UNAVAILABLE) {
        enrollment->last_status =
            APPLIANCE_PRODUCTION_ADMIN_ENROLLMENT_UNAVAILABLE;
        return enrollment->last_status;
    }
    if (enrollment->locked_until_tick > now_tick) {
        ++enrollment->throttled_count;
        enrollment->last_status =
            APPLIANCE_PRODUCTION_ADMIN_ENROLLMENT_THROTTLED;
        return enrollment->last_status;
    }
    /*
     * No salt source means no credential: inventing one would make the record
     * weaker than the policy claims.
     */
    if (enrollment->random == NULL) {
        enrollment->last_status =
            APPLIANCE_PRODUCTION_ADMIN_ENROLLMENT_NO_RANDOM;
        return enrollment->last_status;
    }
    admin_enrollment_zero(salt, sizeof(salt));
    if (!enrollment->random(
            enrollment->random_context, salt, sizeof(salt))) {
        admin_enrollment_zero(salt, sizeof(salt));
        enrollment->last_status =
            APPLIANCE_PRODUCTION_ADMIN_ENROLLMENT_NO_RANDOM;
        return enrollment->last_status;
    }
    admin_enrollment_zero(&credential, sizeof(credential));
    credential_status = appliance_admin_credential_create(
        login, login_length, password, password_length, salt,
        APPLIANCE_ADMIN_CREDENTIAL_ITERATIONS_DEFAULT, &credential);
    admin_enrollment_zero(salt, sizeof(salt));
    if (credential_status != APPLIANCE_ADMIN_CREDENTIAL_OK) {
        admin_enrollment_zero(&credential, sizeof(credential));
        enrollment->last_status =
            APPLIANCE_PRODUCTION_ADMIN_ENROLLMENT_BAD_SECRET;
        return enrollment->last_status;
    }
    if (appliance_persistent_secret_config_commit_admin_credential(
            enrollment->secret_config, &credential) !=
        APPLIANCE_PERSISTENT_SECRET_CONFIG_OK) {
        admin_enrollment_zero(&credential, sizeof(credential));
        enrollment->state =
            APPLIANCE_PRODUCTION_ADMIN_ENROLLMENT_STATE_FAILED;
        enrollment->last_status =
            APPLIANCE_PRODUCTION_ADMIN_ENROLLMENT_PUBLISH_FAILED;
        return enrollment->last_status;
    }
    status = admin_enrollment_publish(enrollment, credential.verifier);
    admin_enrollment_zero(&credential, sizeof(credential));
    if (status != APPLIANCE_PRODUCTION_ADMIN_ENROLLMENT_OK) {
        admin_enrollment_refresh(enrollment);
        return status;
    }
    enrollment->provisioned = true;
    ++enrollment->provisioned_count;
    enrollment->failure_count = 0U;
    enrollment->locked_until_tick = 0U;
    admin_enrollment_refresh(enrollment);
    enrollment->last_status = APPLIANCE_PRODUCTION_ADMIN_ENROLLMENT_OK;
    return enrollment->last_status;
}

/*
 * ITEM-506: the shared check behind both credential entries (the session
 * issuer and the change conversation's verify-only step).  It reads the
 * structured record, answers the state refusals, honours the one lockout and
 * verifies the pair; on success the caller owns `credential` and must zero it.
 *
 * A NULL `login` means "the stored login": the change conversation asks only
 * for the current password, so the comparison is against the account's own
 * login rather than a second value the operator would have to type.
 */
static enum appliance_production_admin_enrollment_status
admin_enrollment_credential_check(
    struct appliance_production_admin_enrollment *enrollment,
    const char *login,
    size_t login_length,
    const char *password,
    size_t password_length,
    uint64_t now_tick,
    struct appliance_admin_credential *credential)
{
    enum appliance_persistent_secret_config_status store_status;
    enum appliance_admin_credential_status credential_status;

    admin_enrollment_refresh(enrollment);
    if (enrollment->state ==
        APPLIANCE_PRODUCTION_ADMIN_ENROLLMENT_STATE_UNAVAILABLE) {
        enrollment->last_status =
            APPLIANCE_PRODUCTION_ADMIN_ENROLLMENT_UNAVAILABLE;
        return enrollment->last_status;
    }
    /*
     * The lockout is answered before the record is even read, so a locked
     * appliance refuses the pair without disclosing whether the head is a
     * structured credential, a legacy fingerprint or absent: those three
     * states all answer THROTTLED while the window is open, exactly as the
     * session-issuing entry does.
     */
    if (enrollment->locked_until_tick > now_tick) {
        admin_enrollment_zero(credential, sizeof(*credential));
        ++enrollment->throttled_count;
        enrollment->last_status =
            APPLIANCE_PRODUCTION_ADMIN_ENROLLMENT_THROTTLED;
        return enrollment->last_status;
    }
    admin_enrollment_zero(credential, sizeof(*credential));
    store_status = appliance_persistent_secret_config_admin_credential(
        enrollment->secret_config, credential);
    if (store_status == APPLIANCE_PERSISTENT_SECRET_CONFIG_LEGACY_RECORD) {
        ++enrollment->denied_count;
        /*
         * The factory placeholder is the deferred first-boot state, not a
         * legacy account: it reports UNPROVISIONED, and only a real legacy
         * fingerprint is refused as LEGACY_RECORD.
         */
        enrollment->last_status = enrollment->provisioned
            ? APPLIANCE_PRODUCTION_ADMIN_ENROLLMENT_LEGACY_RECORD
            : APPLIANCE_PRODUCTION_ADMIN_ENROLLMENT_UNPROVISIONED;
        return enrollment->last_status;
    }
    if (store_status != APPLIANCE_PERSISTENT_SECRET_CONFIG_OK) {
        ++enrollment->denied_count;
        enrollment->last_status =
            APPLIANCE_PRODUCTION_ADMIN_ENROLLMENT_UNPROVISIONED;
        return enrollment->last_status;
    }
    if (login == NULL) {
        login = credential->login;
        login_length = strlen(credential->login);
    }
    credential_status = appliance_admin_credential_verify(
        credential, login, login_length, password, password_length);
    if (credential_status != APPLIANCE_ADMIN_CREDENTIAL_OK) {
        admin_enrollment_zero(credential, sizeof(*credential));
        return admin_enrollment_record_failure(enrollment, now_tick);
    }
    return APPLIANCE_PRODUCTION_ADMIN_ENROLLMENT_OK;
}

/*
 * ITEM-619: the cooperative half of the pair check.  It repeats exactly the
 * state/lockout/record refusals of `admin_enrollment_credential_check`, then
 * hands the job to the caller instead of deriving in one call.  A NULL login
 * still means the stored login, as in the verify-only entry.
 */
enum appliance_production_admin_enrollment_status
appliance_production_admin_enrollment_begin_credential_check(
    struct appliance_production_admin_enrollment *enrollment,
    const char *login,
    size_t login_length,
    const char *password,
    size_t password_length,
    uint64_t now_tick,
    struct appliance_admin_credential_check *check)
{
    struct appliance_admin_credential stored;
    enum appliance_persistent_secret_config_status store_status;
    enum appliance_admin_credential_status credential_status;

    if (enrollment == NULL || password == NULL || check == NULL) {
        return APPLIANCE_PRODUCTION_ADMIN_ENROLLMENT_NULL;
    }
    memset(check, 0, sizeof(*check));
    admin_enrollment_refresh(enrollment);
    if (enrollment->state ==
        APPLIANCE_PRODUCTION_ADMIN_ENROLLMENT_STATE_UNAVAILABLE) {
        enrollment->last_status =
            APPLIANCE_PRODUCTION_ADMIN_ENROLLMENT_UNAVAILABLE;
        return enrollment->last_status;
    }
    if (enrollment->locked_until_tick > now_tick) {
        ++enrollment->throttled_count;
        enrollment->last_status =
            APPLIANCE_PRODUCTION_ADMIN_ENROLLMENT_THROTTLED;
        return enrollment->last_status;
    }
    memset(&stored, 0, sizeof(stored));
    store_status = appliance_persistent_secret_config_admin_credential(
        enrollment->secret_config, &stored);
    if (store_status == APPLIANCE_PERSISTENT_SECRET_CONFIG_LEGACY_RECORD) {
        ++enrollment->denied_count;
        admin_enrollment_zero(&stored, sizeof(stored));
        enrollment->last_status = enrollment->provisioned
            ? APPLIANCE_PRODUCTION_ADMIN_ENROLLMENT_LEGACY_RECORD
            : APPLIANCE_PRODUCTION_ADMIN_ENROLLMENT_UNPROVISIONED;
        return enrollment->last_status;
    }
    if (store_status != APPLIANCE_PERSISTENT_SECRET_CONFIG_OK) {
        ++enrollment->denied_count;
        admin_enrollment_zero(&stored, sizeof(stored));
        enrollment->last_status =
            APPLIANCE_PRODUCTION_ADMIN_ENROLLMENT_UNPROVISIONED;
        return enrollment->last_status;
    }
    if (login == NULL) {
        login = stored.login;
        login_length = strlen(stored.login);
    }
    credential_status = appliance_admin_credential_check_begin(
        check, &stored, login, login_length, password, password_length);
    admin_enrollment_zero(&stored, sizeof(stored));
    if (credential_status == APPLIANCE_ADMIN_CREDENTIAL_PENDING) {
        return APPLIANCE_PRODUCTION_ADMIN_ENROLLMENT_OK;
    }
    return admin_enrollment_record_failure(enrollment, now_tick);
}

/*
 * ITEM-619: consume a finished job.  The caller advances the job first; this
 * entry only issues the session (OK) or counts the one failure (DENIED) and
 * always zeroes the job.  Any other result is reported as a session failure
 * without inventing a success.
 */
enum appliance_production_admin_enrollment_status
appliance_production_admin_enrollment_finish_credential_check(
    struct appliance_production_admin_enrollment *enrollment,
    struct appliance_admin_credential_check *check,
    uint64_t now_tick,
    uint32_t lifetime_ticks,
    uint8_t token_out[APPLIANCE_ADMIN_AUTH_TOKEN_LENGTH],
    uint32_t *session_id_out)
{
    enum appliance_admin_credential_status credential_status;
    enum appliance_production_admin_enrollment_status status;

    if (enrollment == NULL || check == NULL || token_out == NULL ||
        session_id_out == NULL) {
        return APPLIANCE_PRODUCTION_ADMIN_ENROLLMENT_NULL;
    }
    credential_status = appliance_admin_credential_check_step(check, 0U);
    if (credential_status == APPLIANCE_ADMIN_CREDENTIAL_PENDING) {
        return APPLIANCE_PRODUCTION_ADMIN_ENROLLMENT_BAD_STATE;
    }
    if (credential_status == APPLIANCE_ADMIN_CREDENTIAL_OK) {
        status = admin_enrollment_finish_session(
            enrollment, check->kdf.verifier, now_tick, lifetime_ticks,
            token_out, session_id_out);
    } else if (credential_status == APPLIANCE_ADMIN_CREDENTIAL_DENIED) {
        status = admin_enrollment_record_failure(enrollment, now_tick);
    } else {
        ++enrollment->denied_count;
        enrollment->last_status =
            APPLIANCE_PRODUCTION_ADMIN_ENROLLMENT_SESSION_FAILED;
        status = enrollment->last_status;
    }
    appliance_admin_credential_check_cancel(check);
    return status;
}

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
    uint32_t *session_id_out)
{
    struct appliance_admin_credential credential;
    enum appliance_production_admin_enrollment_status status;

    if (enrollment == NULL || login == NULL || password == NULL ||
        token_out == NULL || session_id_out == NULL) {
        return APPLIANCE_PRODUCTION_ADMIN_ENROLLMENT_NULL;
    }
    if (!admin_enrollment_initialized(enrollment)) {
        return APPLIANCE_PRODUCTION_ADMIN_ENROLLMENT_BAD_STATE;
    }
    if (password_length == 0U ||
        password_length > APPLIANCE_ADMIN_CREDENTIAL_PASSWORD_MAX ||
        lifetime_ticks == 0U ||
        lifetime_ticks > APPLIANCE_ADMIN_AUTH_SESSION_LIFETIME_MAX) {
        enrollment->last_status =
            APPLIANCE_PRODUCTION_ADMIN_ENROLLMENT_BAD_SECRET;
        return enrollment->last_status;
    }
    admin_enrollment_zero(&credential, sizeof(credential));
    status = admin_enrollment_credential_check(
        enrollment, login, login_length, password, password_length, now_tick,
        &credential);
    if (status != APPLIANCE_PRODUCTION_ADMIN_ENROLLMENT_OK) {
        return status;
    }
    status = admin_enrollment_finish_session(
        enrollment, credential.verifier, now_tick, lifetime_ticks,
        token_out, session_id_out);
    admin_enrollment_zero(&credential, sizeof(credential));
    return status;
}

enum appliance_production_admin_enrollment_status
appliance_production_admin_enrollment_verify_credential(
    struct appliance_production_admin_enrollment *enrollment,
    const char *password,
    size_t password_length,
    uint64_t now_tick)
{
    struct appliance_admin_credential credential;
    enum appliance_production_admin_enrollment_status status;

    if (enrollment == NULL || password == NULL) {
        return APPLIANCE_PRODUCTION_ADMIN_ENROLLMENT_NULL;
    }
    if (!admin_enrollment_initialized(enrollment)) {
        return APPLIANCE_PRODUCTION_ADMIN_ENROLLMENT_BAD_STATE;
    }
    if (password_length == 0U ||
        password_length > APPLIANCE_ADMIN_CREDENTIAL_PASSWORD_MAX) {
        enrollment->last_status =
            APPLIANCE_PRODUCTION_ADMIN_ENROLLMENT_BAD_SECRET;
        return enrollment->last_status;
    }
    admin_enrollment_zero(&credential, sizeof(credential));
    status = admin_enrollment_credential_check(
        enrollment, NULL, 0U, password, password_length, now_tick,
        &credential);
    admin_enrollment_zero(&credential, sizeof(credential));
    return status;
}

void appliance_production_admin_enrollment_verification_clear(
    struct appliance_production_admin_enrollment_verification *verification)
{
    if (verification != NULL) {
        admin_enrollment_zero(verification, sizeof(*verification));
    }
}

enum appliance_production_admin_enrollment_status
appliance_production_admin_enrollment_verify_credential_pair(
    struct appliance_production_admin_enrollment *enrollment,
    const char *login,
    size_t login_length,
    const char *password,
    size_t password_length,
    uint64_t now_tick,
    struct appliance_production_admin_enrollment_verification
        *verification_out)
{
    struct appliance_admin_credential credential;
    enum appliance_production_admin_enrollment_status status;

    if (verification_out != NULL) {
        admin_enrollment_zero(verification_out, sizeof(*verification_out));
    }
    if (enrollment == NULL || login == NULL || password == NULL ||
        verification_out == NULL) {
        return APPLIANCE_PRODUCTION_ADMIN_ENROLLMENT_NULL;
    }
    if (!admin_enrollment_initialized(enrollment)) {
        return APPLIANCE_PRODUCTION_ADMIN_ENROLLMENT_BAD_STATE;
    }
    if (password_length == 0U ||
        password_length > APPLIANCE_ADMIN_CREDENTIAL_PASSWORD_MAX) {
        enrollment->last_status =
            APPLIANCE_PRODUCTION_ADMIN_ENROLLMENT_BAD_SECRET;
        return enrollment->last_status;
    }
    admin_enrollment_zero(&credential, sizeof(credential));
    status = admin_enrollment_credential_check(
        enrollment, login, login_length, password, password_length, now_tick,
        &credential);
    if (status == APPLIANCE_PRODUCTION_ADMIN_ENROLLMENT_OK) {
        /*
         * This no-session path has no later finish_session() tail to clear the
         * shared failure state.  The account proof itself succeeded, so mirror
         * that tail even if the following seed operation refuses.
         */
        enrollment->failure_count = 0U;
        enrollment->locked_until_tick = 0U;
        verification_out->valid = true;
        verification_out->iterations = credential.iterations;
        memcpy(verification_out->verifier, credential.verifier,
               sizeof(verification_out->verifier));
        enrollment->last_status = APPLIANCE_PRODUCTION_ADMIN_ENROLLMENT_OK;
    }
    admin_enrollment_zero(&credential, sizeof(credential));
    return status;
}

enum appliance_production_admin_enrollment_status
appliance_production_admin_enrollment_publish_verification(
    struct appliance_production_admin_enrollment *enrollment,
    struct appliance_production_admin_enrollment_verification
        *verification)
{
    struct appliance_admin_credential stored;
    enum appliance_persistent_secret_config_status store_status;
    enum appliance_production_admin_enrollment_status status;

    if (verification == NULL) {
        return APPLIANCE_PRODUCTION_ADMIN_ENROLLMENT_NULL;
    }
    if (enrollment == NULL) {
        appliance_production_admin_enrollment_verification_clear(verification);
        return APPLIANCE_PRODUCTION_ADMIN_ENROLLMENT_NULL;
    }
    if (!admin_enrollment_initialized(enrollment) || !verification->valid) {
        appliance_production_admin_enrollment_verification_clear(verification);
        if (admin_enrollment_initialized(enrollment)) {
            enrollment->last_status =
                APPLIANCE_PRODUCTION_ADMIN_ENROLLMENT_BAD_STATE;
            return enrollment->last_status;
        }
        return APPLIANCE_PRODUCTION_ADMIN_ENROLLMENT_BAD_STATE;
    }
    admin_enrollment_refresh(enrollment);
    if (enrollment->state ==
        APPLIANCE_PRODUCTION_ADMIN_ENROLLMENT_STATE_UNAVAILABLE) {
        appliance_production_admin_enrollment_verification_clear(verification);
        enrollment->last_status =
            APPLIANCE_PRODUCTION_ADMIN_ENROLLMENT_UNAVAILABLE;
        return enrollment->last_status;
    }
    admin_enrollment_zero(&stored, sizeof(stored));
    store_status = appliance_persistent_secret_config_admin_credential(
        enrollment->secret_config, &stored);
    if (store_status == APPLIANCE_PERSISTENT_SECRET_CONFIG_LEGACY_RECORD) {
        admin_enrollment_zero(&stored, sizeof(stored));
        appliance_production_admin_enrollment_verification_clear(verification);
        enrollment->last_status =
            APPLIANCE_PRODUCTION_ADMIN_ENROLLMENT_LEGACY_RECORD;
        return enrollment->last_status;
    }
    if (store_status != APPLIANCE_PERSISTENT_SECRET_CONFIG_OK) {
        admin_enrollment_zero(&stored, sizeof(stored));
        appliance_production_admin_enrollment_verification_clear(verification);
        enrollment->last_status =
            APPLIANCE_PRODUCTION_ADMIN_ENROLLMENT_UNPROVISIONED;
        return enrollment->last_status;
    }
    if (memcmp(stored.verifier, verification->verifier,
               sizeof(verification->verifier)) != 0) {
        admin_enrollment_zero(&stored, sizeof(stored));
        appliance_production_admin_enrollment_verification_clear(verification);
        enrollment->last_status =
            APPLIANCE_PRODUCTION_ADMIN_ENROLLMENT_DENIED;
        return enrollment->last_status;
    }
    status = !enrollment->active ||
                     !appliance_production_auth_available(enrollment->auth)
                 ? admin_enrollment_publish(enrollment, stored.verifier)
                 : APPLIANCE_PRODUCTION_ADMIN_ENROLLMENT_OK;
    admin_enrollment_zero(&stored, sizeof(stored));
    appliance_production_admin_enrollment_verification_clear(verification);
    if (status != APPLIANCE_PRODUCTION_ADMIN_ENROLLMENT_OK) {
        return status;
    }
    enrollment->provisioned = true;
    admin_enrollment_refresh(enrollment);
    enrollment->last_status = APPLIANCE_PRODUCTION_ADMIN_ENROLLMENT_OK;
    return enrollment->last_status;
}

bool appliance_production_admin_enrollment_credential_set(
    const struct appliance_production_admin_enrollment *enrollment)
{
    return admin_enrollment_initialized(enrollment) &&
           admin_enrollment_credential_set_internal(enrollment);
}

/*
 * ITEM-506 increment 5: the serving gate asks exactly the credential reader.
 * Keeping it a named predicate rather than an inline call keeps the meaning of
 * "may serve" in one place: only a structured account counts, and a legacy
 * head needs the documented reset gesture before it does.
 */
bool appliance_production_admin_enrollment_serving_allowed(
    const struct appliance_production_admin_enrollment *enrollment)
{
    return appliance_production_admin_enrollment_credential_set(enrollment);
}

bool appliance_production_admin_enrollment_published(
    const struct appliance_production_admin_enrollment *enrollment)
{
    return admin_enrollment_initialized(enrollment) && enrollment->active &&
           appliance_production_auth_available(enrollment->auth);
}

/*
 * ITEM-516: the boot-time publication of the after-boot policy.
 *
 * The record the operator wrote says "publish without a local login"; the
 * structured credential holds the same verifier a successful login would
 * publish, so the boot publishes that verifier and nothing else.  A store that
 * is absent, still the factory placeholder or a pre-ITEM-506 legacy
 * fingerprint has no verifier to publish and refuses by name, which the boot
 * reports as `gate=CLOSED reason=AWAITING_LOCAL_SECRET` instead of pretending
 * the policy is in force.
 */
enum appliance_production_admin_enrollment_status
appliance_production_admin_enrollment_publish_policy(
    struct appliance_production_admin_enrollment *enrollment)
{
    struct appliance_admin_credential credential;
    enum appliance_persistent_secret_config_status store_status;
    enum appliance_production_admin_enrollment_status status;

    if (enrollment == NULL) {
        return APPLIANCE_PRODUCTION_ADMIN_ENROLLMENT_NULL;
    }
    if (!admin_enrollment_initialized(enrollment)) {
        return APPLIANCE_PRODUCTION_ADMIN_ENROLLMENT_BAD_STATE;
    }
    admin_enrollment_refresh(enrollment);
    if (enrollment->state ==
        APPLIANCE_PRODUCTION_ADMIN_ENROLLMENT_STATE_UNAVAILABLE) {
        enrollment->last_status =
            APPLIANCE_PRODUCTION_ADMIN_ENROLLMENT_UNAVAILABLE;
        return enrollment->last_status;
    }
    admin_enrollment_zero(&credential, sizeof(credential));
    store_status = appliance_persistent_secret_config_admin_credential(
        enrollment->secret_config, &credential);
    if (store_status == APPLIANCE_PERSISTENT_SECRET_CONFIG_LEGACY_RECORD) {
        admin_enrollment_zero(&credential, sizeof(credential));
        enrollment->last_status =
            APPLIANCE_PRODUCTION_ADMIN_ENROLLMENT_LEGACY_RECORD;
        return enrollment->last_status;
    }
    if (store_status != APPLIANCE_PERSISTENT_SECRET_CONFIG_OK) {
        admin_enrollment_zero(&credential, sizeof(credential));
        enrollment->last_status =
            APPLIANCE_PRODUCTION_ADMIN_ENROLLMENT_UNPROVISIONED;
        return enrollment->last_status;
    }
    status = admin_enrollment_publish(enrollment, credential.verifier);
    admin_enrollment_zero(&credential, sizeof(credential));
    if (status != APPLIANCE_PRODUCTION_ADMIN_ENROLLMENT_OK) {
        return status;
    }
    enrollment->provisioned = true;
    admin_enrollment_refresh(enrollment);
    enrollment->last_status = APPLIANCE_PRODUCTION_ADMIN_ENROLLMENT_OK;
    return enrollment->last_status;
}

enum appliance_production_admin_enrollment_state
appliance_production_admin_enrollment_state(
    const struct appliance_production_admin_enrollment *enrollment)
{
    if (!admin_enrollment_initialized(enrollment)) {
        return APPLIANCE_PRODUCTION_ADMIN_ENROLLMENT_STATE_NONE;
    }
    return enrollment->state;
}

enum appliance_production_admin_enrollment_status
appliance_production_admin_enrollment_last_status(
    const struct appliance_production_admin_enrollment *enrollment)
{
    if (!admin_enrollment_initialized(enrollment)) {
        return APPLIANCE_PRODUCTION_ADMIN_ENROLLMENT_BAD_STATE;
    }
    return enrollment->last_status;
}

enum appliance_production_admin_enrollment_status
appliance_production_admin_enrollment_credential_fingerprint(
    const char *secret,
    size_t secret_length,
    uint8_t digest_out[SHA256_DIGEST_SIZE])
{
    uint8_t verifier[APPLIANCE_ADMIN_AUTH_CREDENTIAL_LENGTH];
    bool derived;

    if (digest_out == NULL) {
        return APPLIANCE_PRODUCTION_ADMIN_ENROLLMENT_NULL;
    }
    admin_enrollment_zero(digest_out, SHA256_DIGEST_SIZE);
    if (!admin_enrollment_secret_valid(secret, secret_length)) {
        return APPLIANCE_PRODUCTION_ADMIN_ENROLLMENT_BAD_SECRET;
    }
    admin_enrollment_zero(verifier, sizeof(verifier));
    derived = admin_enrollment_derive_verifier(secret, secret_length, verifier);
    if (derived) {
        derived = admin_enrollment_record_fingerprint(verifier, digest_out);
    }
    admin_enrollment_zero(verifier, sizeof(verifier));
    if (!derived) {
        admin_enrollment_zero(digest_out, SHA256_DIGEST_SIZE);
        return APPLIANCE_PRODUCTION_ADMIN_ENROLLMENT_BAD_SECRET;
    }
    return APPLIANCE_PRODUCTION_ADMIN_ENROLLMENT_OK;
}

const char *appliance_production_admin_enrollment_state_string(
    enum appliance_production_admin_enrollment_state state)
{
    switch (state) {
    case APPLIANCE_PRODUCTION_ADMIN_ENROLLMENT_STATE_NONE:
        return "NONE";
    case APPLIANCE_PRODUCTION_ADMIN_ENROLLMENT_STATE_UNINITIALIZED:
        return "UNINITIALIZED";
    case APPLIANCE_PRODUCTION_ADMIN_ENROLLMENT_STATE_UNAVAILABLE:
        return "UNAVAILABLE";
    case APPLIANCE_PRODUCTION_ADMIN_ENROLLMENT_STATE_UNPROVISIONED:
        return "UNPROVISIONED";
    case APPLIANCE_PRODUCTION_ADMIN_ENROLLMENT_STATE_AWAITING_SECRET:
        return "AWAITING_SECRET";
    case APPLIANCE_PRODUCTION_ADMIN_ENROLLMENT_STATE_ACTIVE:
        return "ACTIVE";
    case APPLIANCE_PRODUCTION_ADMIN_ENROLLMENT_STATE_FAILED:
        return "FAILED";
    default:
        return NULL;
    }
}

const char *appliance_production_admin_enrollment_status_string(
    enum appliance_production_admin_enrollment_status status)
{
    switch (status) {
    case APPLIANCE_PRODUCTION_ADMIN_ENROLLMENT_OK:
        return "APPLIANCE_PRODUCTION_ADMIN_ENROLLMENT_OK";
    case APPLIANCE_PRODUCTION_ADMIN_ENROLLMENT_NULL:
        return "APPLIANCE_PRODUCTION_ADMIN_ENROLLMENT_NULL";
    case APPLIANCE_PRODUCTION_ADMIN_ENROLLMENT_BAD_STATE:
        return "APPLIANCE_PRODUCTION_ADMIN_ENROLLMENT_BAD_STATE";
    case APPLIANCE_PRODUCTION_ADMIN_ENROLLMENT_BAD_VERSION:
        return "APPLIANCE_PRODUCTION_ADMIN_ENROLLMENT_BAD_VERSION";
    case APPLIANCE_PRODUCTION_ADMIN_ENROLLMENT_ALREADY_INITIALIZED:
        return "APPLIANCE_PRODUCTION_ADMIN_ENROLLMENT_ALREADY_INITIALIZED";
    case APPLIANCE_PRODUCTION_ADMIN_ENROLLMENT_UNAVAILABLE:
        return "APPLIANCE_PRODUCTION_ADMIN_ENROLLMENT_UNAVAILABLE";
    case APPLIANCE_PRODUCTION_ADMIN_ENROLLMENT_BAD_SECRET:
        return "APPLIANCE_PRODUCTION_ADMIN_ENROLLMENT_BAD_SECRET";
    case APPLIANCE_PRODUCTION_ADMIN_ENROLLMENT_UNPROVISIONED:
        return "APPLIANCE_PRODUCTION_ADMIN_ENROLLMENT_UNPROVISIONED";
    case APPLIANCE_PRODUCTION_ADMIN_ENROLLMENT_DENIED:
        return "APPLIANCE_PRODUCTION_ADMIN_ENROLLMENT_DENIED";
    case APPLIANCE_PRODUCTION_ADMIN_ENROLLMENT_THROTTLED:
        return "APPLIANCE_PRODUCTION_ADMIN_ENROLLMENT_THROTTLED";
    case APPLIANCE_PRODUCTION_ADMIN_ENROLLMENT_PUBLISH_FAILED:
        return "APPLIANCE_PRODUCTION_ADMIN_ENROLLMENT_PUBLISH_FAILED";
    case APPLIANCE_PRODUCTION_ADMIN_ENROLLMENT_SESSION_FAILED:
        return "APPLIANCE_PRODUCTION_ADMIN_ENROLLMENT_SESSION_FAILED";
    case APPLIANCE_PRODUCTION_ADMIN_ENROLLMENT_BUFFER_TOO_SMALL:
        return "APPLIANCE_PRODUCTION_ADMIN_ENROLLMENT_BUFFER_TOO_SMALL";
    case APPLIANCE_PRODUCTION_ADMIN_ENROLLMENT_BAD_LOGIN:
        return "APPLIANCE_PRODUCTION_ADMIN_ENROLLMENT_BAD_LOGIN";
    case APPLIANCE_PRODUCTION_ADMIN_ENROLLMENT_WEAK_PASSWORD:
        return "APPLIANCE_PRODUCTION_ADMIN_ENROLLMENT_WEAK_PASSWORD";
    case APPLIANCE_PRODUCTION_ADMIN_ENROLLMENT_LEGACY_RECORD:
        return "APPLIANCE_PRODUCTION_ADMIN_ENROLLMENT_LEGACY_RECORD";
    case APPLIANCE_PRODUCTION_ADMIN_ENROLLMENT_CREDENTIAL_REQUIRED:
        return "APPLIANCE_PRODUCTION_ADMIN_ENROLLMENT_CREDENTIAL_REQUIRED";
    case APPLIANCE_PRODUCTION_ADMIN_ENROLLMENT_NO_RANDOM:
        return "APPLIANCE_PRODUCTION_ADMIN_ENROLLMENT_NO_RANDOM";
    default:
        return NULL;
    }
}

static bool admin_enrollment_append_text(
    char *buffer,
    size_t capacity,
    size_t *offset,
    const char *text)
{
    size_t index = 0U;

    if (text == NULL) {
        return false;
    }
    while (text[index] != '\0') {
        if (*offset + 1U >= capacity) {
            return false;
        }
        buffer[*offset] = text[index];
        ++(*offset);
        ++index;
    }
    buffer[*offset] = '\0';
    return true;
}

static bool admin_enrollment_append_u32(
    char *buffer,
    size_t capacity,
    size_t *offset,
    uint32_t value)
{
    char digits[10];
    size_t length = 0U;

    if (value == 0U) {
        return admin_enrollment_append_text(buffer, capacity, offset, "0");
    }
    while (value != 0U && length < sizeof(digits)) {
        digits[length] = (char)('0' + (value % 10U));
        value /= 10U;
        ++length;
    }
    while (length != 0U) {
        char single[2];

        single[0] = digits[length - 1U];
        single[1] = '\0';
        if (!admin_enrollment_append_text(buffer, capacity, offset, single)) {
            return false;
        }
        --length;
    }
    return true;
}

enum appliance_production_admin_enrollment_status
appliance_production_admin_enrollment_render(
    const struct appliance_production_admin_enrollment *enrollment,
    char *buffer,
    size_t buffer_size,
    size_t *length_out)
{
    size_t offset = 0U;
    const char *state;

    if (enrollment == NULL || buffer == NULL || length_out == NULL) {
        return APPLIANCE_PRODUCTION_ADMIN_ENROLLMENT_NULL;
    }
    if (!admin_enrollment_initialized(enrollment)) {
        return APPLIANCE_PRODUCTION_ADMIN_ENROLLMENT_BAD_STATE;
    }
    if (buffer_size == 0U) {
        return APPLIANCE_PRODUCTION_ADMIN_ENROLLMENT_BUFFER_TOO_SMALL;
    }
    buffer[0] = '\0';
    state = appliance_production_admin_enrollment_state_string(
        enrollment->state);
    if (state == NULL ||
        !admin_enrollment_append_text(
            buffer, buffer_size, &offset,
            "[ADMIN-ENROLLMENT] v=1 state=") ||
        !admin_enrollment_append_text(buffer, buffer_size, &offset, state) ||
        !admin_enrollment_append_text(
            buffer, buffer_size, &offset, " gate=") ||
        !admin_enrollment_append_text(
            buffer, buffer_size, &offset,
            appliance_production_admin_enrollment_published(enrollment)
                ? "PUBLISHED"
                : "CLOSED") ||
        !admin_enrollment_append_text(
            buffer, buffer_size, &offset, " provisioned=") ||
        !admin_enrollment_append_u32(
            buffer, buffer_size, &offset,
            enrollment->provisioned ? 1U : 0U) ||
        !admin_enrollment_append_text(
            buffer, buffer_size, &offset, " sessions=") ||
        !admin_enrollment_append_u32(
            buffer, buffer_size, &offset, enrollment->session_count) ||
        !admin_enrollment_append_text(
            buffer, buffer_size, &offset, " denied=") ||
        !admin_enrollment_append_u32(
            buffer, buffer_size, &offset, enrollment->denied_count) ||
        /*
         * ITEM-460: the record names the last refusal, so a run that lost the
         * console answer to a stream of timer records still explains itself
         * from the durable log alone.  A successful operation has no refusal
         * and says so instead of leaving the field out.
         */
        !admin_enrollment_append_text(
            buffer, buffer_size, &offset, " last_refusal=") ||
        !admin_enrollment_append_text(
            buffer, buffer_size, &offset,
            enrollment->last_status ==
                    APPLIANCE_PRODUCTION_ADMIN_ENROLLMENT_OK
                ? "NONE"
                : appliance_production_admin_enrollment_status_string(
                      enrollment->last_status)) ||
        !admin_enrollment_append_text(
            buffer, buffer_size, &offset, " throttled=") ||
        !admin_enrollment_append_u32(
            buffer, buffer_size, &offset, enrollment->throttled_count) ||
        !admin_enrollment_append_text(
            buffer, buffer_size, &offset, " unavailable=") ||
        !admin_enrollment_append_u32(
            buffer, buffer_size, &offset,
            enrollment->state ==
                    APPLIANCE_PRODUCTION_ADMIN_ENROLLMENT_STATE_UNAVAILABLE
                ? 1U
                : 0U)) {
        buffer[0] = '\0';
        *length_out = 0U;
        return APPLIANCE_PRODUCTION_ADMIN_ENROLLMENT_BUFFER_TOO_SMALL;
    }
    *length_out = offset;
    return APPLIANCE_PRODUCTION_ADMIN_ENROLLMENT_OK;
}
