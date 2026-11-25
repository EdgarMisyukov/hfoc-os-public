#include "appliance_admin_credential.h"

#include "sha256.h"

#include <string.h>

/*
 * ITEM-506 credential model.  See the header for the contract; this file owns
 * the exact policy, the PBKDF2-HMAC-SHA256 derivation built on the in-tree
 * SHA-256, the canonical record value and the refusal names.
 */

#define APPLIANCE_ADMIN_CREDENTIAL_HMAC_BLOCK SHA256_BLOCK_SIZE

/*
 * The bounded weak list.  Every entry is stored lower case; the comparison
 * lower-cases the presented password, so `PASSWORD` is refused too.  The list
 * is deliberately short and documented (the contract asks for a small
 * built-in list, not a dictionary): the product name and its common spellings
 * with digits, the generic words every credential-stuffing list starts with,
 * and the classic numeric runs.
 */
static const char *const appliance_admin_credential_weak_passwords[] = {
    "password", "password1", "password123", "passw0rd",
    "123456", "1234567", "12345678", "123456789", "1234567890",
    "12345678901", "123456789012",
    "qwerty", "qwerty123", "qwertyuiop",
    "admin", "admin123", "administrator",
    "hfoc", "hfoc123", "hfoc-os", "hfoc-123", "hfoc_123", "hfocos",
    "letmein", "welcome", "iloveyou", "monkey", "dragon",
    "sunshine", "princess", "football", "baseball",
    "abc123", "111111", "000000",
};

static void appliance_admin_credential_zeroize(void *buffer, size_t length)
{
    volatile uint8_t *bytes = buffer;
    size_t index;

    for (index = 0U; index < length; ++index) {
        bytes[index] = 0U;
    }
}

static size_t appliance_admin_credential_bounded_length(
    const char *text,
    size_t maximum)
{
    size_t length = 0U;

    if (text == NULL) {
        return 0U;
    }
    while (length < maximum && text[length] != '\0') {
        ++length;
    }
    return length;
}

static bool appliance_admin_credential_login_character_valid(
    unsigned char value)
{
    return (value >= (unsigned char)'A' && value <= (unsigned char)'Z') ||
           (value >= (unsigned char)'a' && value <= (unsigned char)'z') ||
           (value >= (unsigned char)'0' && value <= (unsigned char)'9') ||
           value == (unsigned char)'.' ||
           value == (unsigned char)'_' ||
           value == (unsigned char)'-';
}

static bool appliance_admin_credential_printable_ascii(
    unsigned char value)
{
    return value >= 0x20U && value <= 0x7eU;
}

static unsigned char appliance_admin_credential_lower(unsigned char value)
{
    if (value >= (unsigned char)'A' && value <= (unsigned char)'Z') {
        return (unsigned char)(value + ((unsigned char)'a' -
                                        (unsigned char)'A'));
    }
    return value;
}

static bool appliance_admin_credential_hex_digit_valid(
    unsigned char value)
{
    return (value >= (unsigned char)'0' && value <= (unsigned char)'9') ||
           (value >= (unsigned char)'a' && value <= (unsigned char)'f');
}

static int8_t appliance_admin_credential_hex_value(unsigned char value)
{
    if (value >= (unsigned char)'0' && value <= (unsigned char)'9') {
        return (int8_t)(value - (unsigned char)'0');
    }
    if (value >= (unsigned char)'a' && value <= (unsigned char)'f') {
        return (int8_t)(value - (unsigned char)'a' + 10U);
    }
    return -1;
}

static char appliance_admin_credential_hex_digit(uint8_t value)
{
    if (value < 10U) {
        return (char)('0' + (int)value);
    }
    return (char)('a' + (int)(value - 10U));
}

static void appliance_admin_credential_encode_hex(
    const uint8_t *bytes,
    size_t length,
    char *output)
{
    size_t index;

    for (index = 0U; index < length; ++index) {
        output[index * 2U] = appliance_admin_credential_hex_digit(
            (uint8_t)(bytes[index] >> 4U));
        output[index * 2U + 1U] = appliance_admin_credential_hex_digit(
            (uint8_t)(bytes[index] & 0x0fU));
    }
}

static bool appliance_admin_credential_decode_hex(
    const char *hex,
    size_t hex_length,
    uint8_t *output,
    size_t output_length)
{
    size_t index;

    if (hex_length != output_length * 2U) {
        return false;
    }
    for (index = 0U; index < output_length; ++index) {
        const int8_t high = appliance_admin_credential_hex_value(
            (unsigned char)hex[index * 2U]);
        const int8_t low = appliance_admin_credential_hex_value(
            (unsigned char)hex[index * 2U + 1U]);

        if (high < 0 || low < 0) {
            return false;
        }
        output[index] = (uint8_t)(((uint8_t)high << 4U) | (uint8_t)low);
    }
    return true;
}

static bool appliance_admin_credential_bytes_all_zero(
    const uint8_t *bytes,
    size_t length)
{
    uint8_t nonzero = 0U;
    size_t index;

    for (index = 0U; index < length; ++index) {
        nonzero |= bytes[index];
    }
    return nonzero == 0U;
}

static bool appliance_admin_credential_bytes_equal_constant_time(
    const uint8_t *left,
    size_t left_length,
    const uint8_t *right,
    size_t right_length)
{
    volatile uint8_t mismatch = 0U;
    size_t longest = left_length > right_length ? left_length : right_length;
    size_t index;

    mismatch |= (uint8_t)((left_length ^ right_length) != 0U ? 1U : 0U);
    for (index = 0U; index < longest; ++index) {
        const uint8_t a = index < left_length ? left[index] : 0U;
        const uint8_t b = index < right_length ? right[index] : 0U;

        mismatch |= (uint8_t)(a ^ b);
    }
    return mismatch == 0U;
}

/*
 * CI-SPEEDUP-4: one prepared HMAC key per derivation.
 *
 * Every PBKDF2 round used to rebuild the key block and hash both pads again:
 * four SHA-256 compressions per round, plus a hash of the key itself on every
 * round once the password exceeded one block.  The pads only depend on the
 * key, so this struct keeps the two states *after* their pad block has been
 * absorbed and the counter has moved with it; a round copies both states (the
 * whole `struct sha256`, counter included) and pays one compression per hash
 * instead of two.  The prepared states are secret-derived material and are
 * zeroed on every exit path of the derivation.
 */
/*
 * Normalise the key once and absorb both pads.  A key longer than one block is
 * hashed here - once per derivation, not once per round.
 */
static enum appliance_admin_credential_status
appliance_admin_credential_hmac_key_prepare(
    const uint8_t *key,
    size_t key_length,
    struct appliance_admin_credential_hmac_key *prepared)
{
    uint8_t key_block[APPLIANCE_ADMIN_CREDENTIAL_HMAC_BLOCK];
    uint8_t pad[APPLIANCE_ADMIN_CREDENTIAL_HMAC_BLOCK];
    size_t index;
    enum appliance_admin_credential_status status =
        APPLIANCE_ADMIN_CREDENTIAL_UNAVAILABLE;

    if (key == NULL || prepared == NULL) {
        return APPLIANCE_ADMIN_CREDENTIAL_NULL;
    }
    memset(prepared, 0, sizeof(*prepared));
    memset(key_block, 0, sizeof(key_block));
    if (key_length > APPLIANCE_ADMIN_CREDENTIAL_HMAC_BLOCK) {
        if (sha256_compute(key, key_length, key_block) != SHA256_OK) {
            goto done;
        }
    } else {
        memcpy(key_block, key, key_length);
    }
    for (index = 0U; index < sizeof(pad); ++index) {
        pad[index] = (uint8_t)(key_block[index] ^ 0x36U);
    }
    if (sha256_init(&prepared->inner) != SHA256_OK ||
        sha256_update(&prepared->inner, pad, sizeof(pad)) != SHA256_OK ||
        sha256_absorb_block(&prepared->inner) != SHA256_OK) {
        goto done;
    }
    for (index = 0U; index < sizeof(pad); ++index) {
        pad[index] = (uint8_t)(key_block[index] ^ 0x5cU);
    }
    if (sha256_init(&prepared->outer) != SHA256_OK ||
        sha256_update(&prepared->outer, pad, sizeof(pad)) != SHA256_OK ||
        sha256_absorb_block(&prepared->outer) != SHA256_OK) {
        goto done;
    }
    prepared->ready = true;
    status = APPLIANCE_ADMIN_CREDENTIAL_OK;

done:
    appliance_admin_credential_zeroize(key_block, sizeof(key_block));
    appliance_admin_credential_zeroize(pad, sizeof(pad));
    if (status != APPLIANCE_ADMIN_CREDENTIAL_OK) {
        memset(prepared, 0, sizeof(*prepared));
    }
    return status;
}

/*
 * One HMAC over one message with the prepared key.  `data` and `mac` may be
 * the same buffer: the message is copied into the inner hash before anything
 * is written to `mac`.
 */
static enum appliance_admin_credential_status
appliance_admin_credential_hmac_sha256(
    const struct appliance_admin_credential_hmac_key *prepared,
    const uint8_t *data,
    size_t data_length,
    uint8_t mac[APPLIANCE_ADMIN_CREDENTIAL_VERIFIER_LENGTH])
{
    struct sha256 inner;
    struct sha256 outer;
    uint8_t digest[SHA256_DIGEST_SIZE];
    enum appliance_admin_credential_status status =
        APPLIANCE_ADMIN_CREDENTIAL_UNAVAILABLE;

    if (prepared == NULL || mac == NULL ||
        (data == NULL && data_length != 0U)) {
        return APPLIANCE_ADMIN_CREDENTIAL_NULL;
    }
    if (!prepared->ready) {
        return APPLIANCE_ADMIN_CREDENTIAL_BAD_STATE;
    }
    /*
     * The whole context travels: state, buffered bytes and the byte counter
     * the padding will describe.  That is what makes one prepared pair usable
     * for every round of the derivation.
     */
    inner = prepared->inner;
    outer = prepared->outer;
    if ((data_length != 0U &&
         sha256_update(&inner, data, data_length) != SHA256_OK) ||
        sha256_final(&inner, digest) != SHA256_OK) {
        goto done;
    }
    if (sha256_update(&outer, digest, sizeof(digest)) != SHA256_OK ||
        sha256_final(&outer, mac) != SHA256_OK) {
        goto done;
    }
    status = APPLIANCE_ADMIN_CREDENTIAL_OK;

done:
    appliance_admin_credential_zeroize(&inner, sizeof(inner));
    appliance_admin_credential_zeroize(&outer, sizeof(outer));
    appliance_admin_credential_zeroize(digest, sizeof(digest));
    return status;
}

enum appliance_admin_credential_status
appliance_admin_credential_validate_login(
    const char *login,
    size_t login_length)
{
    size_t index;

    if (login == NULL) {
        return APPLIANCE_ADMIN_CREDENTIAL_NULL;
    }
    if (login_length < APPLIANCE_ADMIN_CREDENTIAL_LOGIN_MIN ||
        login_length > APPLIANCE_ADMIN_CREDENTIAL_LOGIN_MAX) {
        return APPLIANCE_ADMIN_CREDENTIAL_BAD_LOGIN;
    }
    for (index = 0U; index < login_length; ++index) {
        if (!appliance_admin_credential_login_character_valid(
                (unsigned char)login[index])) {
            return APPLIANCE_ADMIN_CREDENTIAL_BAD_LOGIN;
        }
    }
    return APPLIANCE_ADMIN_CREDENTIAL_OK;
}

bool appliance_admin_credential_password_weak(
    const char *password,
    size_t password_length)
{
    size_t index;

    if (password == NULL || password_length == 0U) {
        return false;
    }
    for (index = 0U;
         index < sizeof(appliance_admin_credential_weak_passwords) /
                     sizeof(appliance_admin_credential_weak_passwords[0]);
         ++index) {
        const char *entry =
            appliance_admin_credential_weak_passwords[index];
        const size_t entry_length = strlen(entry);
        size_t position;
        bool equal = entry_length == password_length;

        for (position = 0U; equal && position < entry_length; ++position) {
            equal = appliance_admin_credential_lower(
                        (unsigned char)password[position]) ==
                    (unsigned char)entry[position];
        }
        if (equal) {
            return true;
        }
    }
    return false;
}

enum appliance_admin_credential_status
appliance_admin_credential_validate_password(
    const char *password,
    size_t password_length,
    const char *login,
    size_t login_length)
{
    size_t index;
    bool all_spaces = true;
    bool equals_login = login_length == password_length;

    if (password == NULL || (login == NULL && login_length != 0U)) {
        return APPLIANCE_ADMIN_CREDENTIAL_NULL;
    }
    if (password_length == 0U ||
        password_length > APPLIANCE_ADMIN_CREDENTIAL_PASSWORD_MAX) {
        return APPLIANCE_ADMIN_CREDENTIAL_BAD_PASSWORD;
    }
    for (index = 0U; index < password_length; ++index) {
        const unsigned char value = (unsigned char)password[index];

        if (!appliance_admin_credential_printable_ascii(value)) {
            return APPLIANCE_ADMIN_CREDENTIAL_BAD_PASSWORD;
        }
        if (value != (unsigned char)' ') {
            all_spaces = false;
        }
        if (equals_login && index < login_length) {
            equals_login =
                appliance_admin_credential_lower(value) ==
                appliance_admin_credential_lower(
                    (unsigned char)login[index]);
        }
    }
    if (all_spaces) {
        return APPLIANCE_ADMIN_CREDENTIAL_BAD_PASSWORD;
    }
    if (equals_login && login_length != 0U) {
        return APPLIANCE_ADMIN_CREDENTIAL_BAD_PASSWORD;
    }
    /*
     * The weak list is checked before the minimum length on purpose: an
     * eight-character `password` is refused as a listed weak value, which is
     * the more useful name in the audit record, and still refused either way.
     */
    if (appliance_admin_credential_password_weak(
            password, password_length)) {
        return APPLIANCE_ADMIN_CREDENTIAL_WEAK_PASSWORD;
    }
    if (password_length < APPLIANCE_ADMIN_CREDENTIAL_PASSWORD_MIN) {
        return APPLIANCE_ADMIN_CREDENTIAL_BAD_PASSWORD;
    }
    return APPLIANCE_ADMIN_CREDENTIAL_OK;
}

static enum appliance_admin_credential_status
appliance_admin_credential_kdf_finish(
    struct appliance_admin_credential_kdf *kdf)
{
    memcpy(kdf->verifier, kdf->t,
           APPLIANCE_ADMIN_CREDENTIAL_VERIFIER_LENGTH);
    kdf->finished = true;
    kdf->result = APPLIANCE_ADMIN_CREDENTIAL_OK;
    /* `u`, `t`, the pads and the block input are secret-derived; drop them. */
    appliance_admin_credential_zeroize(&kdf->prepared, sizeof(kdf->prepared));
    appliance_admin_credential_zeroize(kdf->u, sizeof(kdf->u));
    appliance_admin_credential_zeroize(kdf->t, sizeof(kdf->t));
    appliance_admin_credential_zeroize(
        kdf->block_input, sizeof(kdf->block_input));
    return APPLIANCE_ADMIN_CREDENTIAL_OK;
}

enum appliance_admin_credential_status
appliance_admin_credential_kdf_begin(
    struct appliance_admin_credential_kdf *kdf,
    const char *password,
    size_t password_length,
    const uint8_t salt[APPLIANCE_ADMIN_CREDENTIAL_SALT_LENGTH],
    uint32_t iterations)
{
    enum appliance_admin_credential_status status;

    if (kdf == NULL || password == NULL || salt == NULL) {
        return APPLIANCE_ADMIN_CREDENTIAL_NULL;
    }
    memset(kdf, 0, sizeof(*kdf));
    kdf->result = APPLIANCE_ADMIN_CREDENTIAL_BAD_STATE;
    if (iterations < APPLIANCE_ADMIN_CREDENTIAL_ITERATIONS_MIN ||
        iterations > APPLIANCE_ADMIN_CREDENTIAL_ITERATIONS_MAX) {
        return APPLIANCE_ADMIN_CREDENTIAL_BAD_ITERATIONS;
    }
    if (password_length == 0U ||
        password_length > APPLIANCE_ADMIN_CREDENTIAL_PASSWORD_MAX) {
        return APPLIANCE_ADMIN_CREDENTIAL_BAD_PASSWORD;
    }
    if (appliance_admin_credential_bytes_all_zero(
            salt, APPLIANCE_ADMIN_CREDENTIAL_SALT_LENGTH)) {
        return APPLIANCE_ADMIN_CREDENTIAL_BAD_SALT;
    }

    memcpy(kdf->block_input, salt, APPLIANCE_ADMIN_CREDENTIAL_SALT_LENGTH);
    /* PBKDF2 INT(1), big endian. */
    kdf->block_input[APPLIANCE_ADMIN_CREDENTIAL_SALT_LENGTH + 3U] = 1U;
    kdf->iterations_total = iterations;
    status = appliance_admin_credential_hmac_key_prepare(
        (const uint8_t *)password, password_length, &kdf->prepared);
    if (status != APPLIANCE_ADMIN_CREDENTIAL_OK) {
        appliance_admin_credential_zeroize(
            kdf->block_input, sizeof(kdf->block_input));
        return status;
    }
    status = appliance_admin_credential_hmac_sha256(
        &kdf->prepared, kdf->block_input, sizeof(kdf->block_input), kdf->u);
    if (status != APPLIANCE_ADMIN_CREDENTIAL_OK) {
        appliance_admin_credential_zeroize(
            &kdf->prepared, sizeof(kdf->prepared));
        appliance_admin_credential_zeroize(
            kdf->block_input, sizeof(kdf->block_input));
        return status;
    }
    memcpy(kdf->t, kdf->u, sizeof(kdf->t));
    kdf->iterations_done = 1U;
    kdf->started = true;
    if (kdf->iterations_done >= kdf->iterations_total) {
        return appliance_admin_credential_kdf_finish(kdf);
    }
    kdf->result = APPLIANCE_ADMIN_CREDENTIAL_PENDING;
    return APPLIANCE_ADMIN_CREDENTIAL_PENDING;
}

enum appliance_admin_credential_status
appliance_admin_credential_kdf_step(
    struct appliance_admin_credential_kdf *kdf,
    uint32_t rounds)
{
    enum appliance_admin_credential_status status;

    if (kdf == NULL) {
        return APPLIANCE_ADMIN_CREDENTIAL_NULL;
    }
    if (!kdf->started) {
        return APPLIANCE_ADMIN_CREDENTIAL_BAD_STATE;
    }
    if (kdf->finished) {
        return kdf->result;
    }
    {
        /*
         * `iterations` is the record's full count and is never shortened;
         * `rounds` only says how much of it this call may spend before it
         * returns to the loop that owns the socket and the timers.
         */
        const uint32_t iterations = kdf->iterations_total;
        const uint32_t first = kdf->iterations_done;
        uint32_t round;

        for (round = first;
             round < iterations && round - first < rounds;
             ++round) {
            size_t index;

            /* `u` is both the message and the output of this round. */
            status = appliance_admin_credential_hmac_sha256(
                &kdf->prepared, kdf->u, sizeof(kdf->u), kdf->u);
            if (status != APPLIANCE_ADMIN_CREDENTIAL_OK) {
                kdf->finished = true;
                kdf->result = status;
                appliance_admin_credential_zeroize(
                    &kdf->prepared, sizeof(kdf->prepared));
                appliance_admin_credential_zeroize(kdf->u, sizeof(kdf->u));
                appliance_admin_credential_zeroize(kdf->t, sizeof(kdf->t));
                appliance_admin_credential_zeroize(
                    kdf->block_input, sizeof(kdf->block_input));
                return status;
            }
            for (index = 0U; index < sizeof(kdf->t); ++index) {
                kdf->t[index] ^= kdf->u[index];
            }
            kdf->iterations_done = round + 1U;
        }
    }
    if (kdf->iterations_done >= kdf->iterations_total) {
        return appliance_admin_credential_kdf_finish(kdf);
    }
    return APPLIANCE_ADMIN_CREDENTIAL_PENDING;
}

void appliance_admin_credential_kdf_cancel(
    struct appliance_admin_credential_kdf *kdf)
{
    if (kdf == NULL) {
        return;
    }
    appliance_admin_credential_zeroize(kdf, sizeof(*kdf));
    kdf->finished = true;
    kdf->result = APPLIANCE_ADMIN_CREDENTIAL_CANCELLED;
}

bool appliance_admin_credential_kdf_pending(
    const struct appliance_admin_credential_kdf *kdf)
{
    return kdf != NULL && kdf->started && !kdf->finished &&
        kdf->result == APPLIANCE_ADMIN_CREDENTIAL_PENDING;
}

uint32_t appliance_admin_credential_kdf_iterations_done(
    const struct appliance_admin_credential_kdf *kdf)
{
    return kdf == NULL ? 0U : kdf->iterations_done;
}

enum appliance_admin_credential_status
appliance_admin_credential_derive(
    const char *password,
    size_t password_length,
    const uint8_t salt[APPLIANCE_ADMIN_CREDENTIAL_SALT_LENGTH],
    uint32_t iterations,
    uint8_t verifier_out[APPLIANCE_ADMIN_CREDENTIAL_VERIFIER_LENGTH])
{
    struct appliance_admin_credential_kdf kdf;
    enum appliance_admin_credential_status status;

    if (verifier_out == NULL) {
        return APPLIANCE_ADMIN_CREDENTIAL_NULL;
    }
    memset(verifier_out, 0, APPLIANCE_ADMIN_CREDENTIAL_VERIFIER_LENGTH);
    status = appliance_admin_credential_kdf_begin(
        &kdf, password, password_length, salt, iterations);
    while (status == APPLIANCE_ADMIN_CREDENTIAL_PENDING) {
        status = appliance_admin_credential_kdf_step(
            &kdf, APPLIANCE_ADMIN_CREDENTIAL_CHECK_ROUNDS_DEFAULT);
    }
    if (status == APPLIANCE_ADMIN_CREDENTIAL_OK) {
        memcpy(verifier_out, kdf.verifier,
               APPLIANCE_ADMIN_CREDENTIAL_VERIFIER_LENGTH);
    }
    appliance_admin_credential_kdf_cancel(&kdf);
    return status;
}

enum appliance_admin_credential_status
appliance_admin_credential_create(
    const char *login,
    size_t login_length,
    const char *password,
    size_t password_length,
    const uint8_t salt[APPLIANCE_ADMIN_CREDENTIAL_SALT_LENGTH],
    uint32_t iterations,
    struct appliance_admin_credential *credential_out)
{
    enum appliance_admin_credential_status status;

    if (credential_out == NULL) {
        return APPLIANCE_ADMIN_CREDENTIAL_NULL;
    }
    appliance_admin_credential_zeroize(
        credential_out, sizeof(*credential_out));
    status = appliance_admin_credential_validate_login(login, login_length);
    if (status != APPLIANCE_ADMIN_CREDENTIAL_OK) {
        return status;
    }
    status = appliance_admin_credential_validate_password(
        password, password_length, login, login_length);
    if (status != APPLIANCE_ADMIN_CREDENTIAL_OK) {
        return status;
    }
    if (salt == NULL) {
        return APPLIANCE_ADMIN_CREDENTIAL_NULL;
    }
    status = appliance_admin_credential_derive(
        password, password_length, salt, iterations,
        credential_out->verifier);
    if (status != APPLIANCE_ADMIN_CREDENTIAL_OK) {
        appliance_admin_credential_zeroize(
            credential_out, sizeof(*credential_out));
        return status;
    }
    credential_out->version = APPLIANCE_ADMIN_CREDENTIAL_VERSION;
    memcpy(credential_out->login, login, login_length);
    credential_out->login[login_length] = '\0';
    memcpy(credential_out->salt, salt,
           APPLIANCE_ADMIN_CREDENTIAL_SALT_LENGTH);
    credential_out->iterations = iterations;
    return APPLIANCE_ADMIN_CREDENTIAL_OK;
}

enum appliance_admin_credential_status
appliance_admin_credential_check_begin(
    struct appliance_admin_credential_check *check,
    const struct appliance_admin_credential *stored,
    const char *login,
    size_t login_length,
    const char *password,
    size_t password_length)
{
    enum appliance_admin_credential_status status;
    size_t stored_login_length;

    if (check == NULL || stored == NULL || password == NULL) {
        return APPLIANCE_ADMIN_CREDENTIAL_NULL;
    }
    if (login == NULL && login_length != 0U) {
        return APPLIANCE_ADMIN_CREDENTIAL_NULL;
    }
    memset(check, 0, sizeof(*check));
    check->result = APPLIANCE_ADMIN_CREDENTIAL_BAD_STATE;
    if (stored->version != APPLIANCE_ADMIN_CREDENTIAL_VERSION) {
        return APPLIANCE_ADMIN_CREDENTIAL_BAD_STATE;
    }
    stored_login_length = appliance_admin_credential_bounded_length(
        stored->login, APPLIANCE_ADMIN_CREDENTIAL_LOGIN_MAX);
    /*
     * Derive even when the login already mismatched: the refusal then costs
     * the same as a wrong password and does not say which half failed.  The
     * answer is kept in the job, not returned early.
     */
    check->login_matches = appliance_admin_credential_bytes_equal_constant_time(
        (const uint8_t *)stored->login, stored_login_length,
        (const uint8_t *)login, login == NULL ? 0U : login_length);
    memcpy(check->stored_verifier, stored->verifier,
           APPLIANCE_ADMIN_CREDENTIAL_VERIFIER_LENGTH);
    status = appliance_admin_credential_kdf_begin(
        &check->kdf, password, password_length, stored->salt,
        stored->iterations);
    if (status == APPLIANCE_ADMIN_CREDENTIAL_OK) {
        /* One iteration already finished; only the comparison remains. */
        check->result = APPLIANCE_ADMIN_CREDENTIAL_PENDING;
        return APPLIANCE_ADMIN_CREDENTIAL_PENDING;
    }
    if (status != APPLIANCE_ADMIN_CREDENTIAL_PENDING) {
        appliance_admin_credential_check_cancel(check);
        check->result = status;
        return status;
    }
    check->result = APPLIANCE_ADMIN_CREDENTIAL_PENDING;
    return APPLIANCE_ADMIN_CREDENTIAL_PENDING;
}

enum appliance_admin_credential_status
appliance_admin_credential_check_step(
    struct appliance_admin_credential_check *check,
    uint32_t rounds)
{
    enum appliance_admin_credential_status status;

    if (check == NULL) {
        return APPLIANCE_ADMIN_CREDENTIAL_NULL;
    }
    if (check->finished) {
        return check->result;
    }
    status = appliance_admin_credential_kdf_step(&check->kdf, rounds);
    if (status == APPLIANCE_ADMIN_CREDENTIAL_PENDING) {
        return APPLIANCE_ADMIN_CREDENTIAL_PENDING;
    }
    if (status == APPLIANCE_ADMIN_CREDENTIAL_OK) {
        const bool verifier_matches =
            appliance_admin_credential_bytes_equal_constant_time(
                check->stored_verifier,
                APPLIANCE_ADMIN_CREDENTIAL_VERIFIER_LENGTH,
                check->kdf.verifier,
                APPLIANCE_ADMIN_CREDENTIAL_VERIFIER_LENGTH);

        check->result = check->login_matches && verifier_matches
            ? APPLIANCE_ADMIN_CREDENTIAL_OK
            : APPLIANCE_ADMIN_CREDENTIAL_DENIED;
    } else {
        check->result = status;
    }
    check->finished = true;
    /*
     * `kdf_step` already zeroed the pads, `u`, `t` and the block input.  The
     * stored verifier is zeroed here; the derived verifier stays until the
     * caller's immediate session issuance (or `cancel()`), so a cooperative
     * caller can finish the login it just proved without deriving again.
     * `cancel()` zeroes the whole job on every other path, including the
     * synchronous `verify()` wrapper.
     */
    appliance_admin_credential_zeroize(
        check->stored_verifier, sizeof(check->stored_verifier));
    check->kdf.finished = true;
    check->kdf.result = check->result;
    return check->result;
}

uint32_t appliance_admin_credential_check_next_slice(uint32_t rounds_done,
                                                     uint32_t elapsed_us)
{
    uint64_t target;

    if (rounds_done == 0U || elapsed_us == 0U) {
        return APPLIANCE_ADMIN_CREDENTIAL_CHECK_ROUNDS_DEFAULT;
    }
    target = ((uint64_t)APPLIANCE_ADMIN_CREDENTIAL_CHECK_SLICE_TARGET_US *
              (uint64_t)rounds_done) / (uint64_t)elapsed_us;
    if (target < APPLIANCE_ADMIN_CREDENTIAL_CHECK_ROUNDS_DEFAULT) {
        return APPLIANCE_ADMIN_CREDENTIAL_CHECK_ROUNDS_DEFAULT;
    }
    if (target > APPLIANCE_ADMIN_CREDENTIAL_CHECK_SLICE_MAX_ROUNDS) {
        return APPLIANCE_ADMIN_CREDENTIAL_CHECK_SLICE_MAX_ROUNDS;
    }
    return (uint32_t)target;
}

void appliance_admin_credential_check_cancel(
    struct appliance_admin_credential_check *check)
{
    if (check == NULL) {
        return;
    }
    appliance_admin_credential_zeroize(check, sizeof(*check));
    check->finished = true;
    check->result = APPLIANCE_ADMIN_CREDENTIAL_CANCELLED;
}

bool appliance_admin_credential_check_pending(
    const struct appliance_admin_credential_check *check)
{
    return check != NULL && !check->finished &&
        check->result == APPLIANCE_ADMIN_CREDENTIAL_PENDING;
}

uint32_t appliance_admin_credential_check_iterations_done(
    const struct appliance_admin_credential_check *check)
{
    return check == NULL
        ? 0U
        : appliance_admin_credential_kdf_iterations_done(&check->kdf);
}

uint32_t appliance_admin_credential_check_iterations_total(
    const struct appliance_admin_credential_check *check)
{
    return check == NULL ? 0U : check->kdf.iterations_total;
}

enum appliance_admin_credential_status
appliance_admin_credential_verify(
    const struct appliance_admin_credential *stored,
    const char *login,
    size_t login_length,
    const char *password,
    size_t password_length)
{
    struct appliance_admin_credential_check check = {0};
    enum appliance_admin_credential_status status;

    status = appliance_admin_credential_check_begin(
        &check, stored, login, login_length, password, password_length);
    while (status == APPLIANCE_ADMIN_CREDENTIAL_PENDING) {
        status = appliance_admin_credential_check_step(
            &check, APPLIANCE_ADMIN_CREDENTIAL_CHECK_ROUNDS_DEFAULT);
    }
    appliance_admin_credential_check_cancel(&check);
    return status;
}

bool appliance_admin_credential_record_is_legacy(
    const char *value,
    size_t value_length)
{
    size_t index;

    if (value == NULL || value_length != SHA256_DIGEST_SIZE * 2U) {
        return false;
    }
    for (index = 0U; index < value_length; ++index) {
        if (!appliance_admin_credential_hex_digit_valid(
                (unsigned char)value[index])) {
            return false;
        }
    }
    return true;
}

enum appliance_admin_credential_status
appliance_admin_credential_record_encode(
    const struct appliance_admin_credential *credential,
    char *buffer,
    size_t buffer_size,
    size_t *length_out)
{
    static const char label[] = APPLIANCE_ADMIN_CREDENTIAL_RECORD_LABEL;
    const size_t label_length = sizeof(label) - 1U;
    char iterations_text[12];
    size_t login_length;
    size_t iterations_length = 0U;
    size_t offset = 0U;
    uint32_t value;

    if (credential == NULL || buffer == NULL || length_out == NULL) {
        return APPLIANCE_ADMIN_CREDENTIAL_NULL;
    }
    *length_out = 0U;
    if (buffer_size != 0U) {
        buffer[0] = '\0';
    }
    if (credential->version != APPLIANCE_ADMIN_CREDENTIAL_VERSION) {
        return APPLIANCE_ADMIN_CREDENTIAL_BAD_STATE;
    }
    login_length = appliance_admin_credential_bounded_length(
        credential->login, APPLIANCE_ADMIN_CREDENTIAL_LOGIN_MAX);
    {
        const enum appliance_admin_credential_status status =
            appliance_admin_credential_validate_login(
                credential->login, login_length);

        if (status != APPLIANCE_ADMIN_CREDENTIAL_OK) {
            return status;
        }
    }
    if (credential->iterations <
            APPLIANCE_ADMIN_CREDENTIAL_ITERATIONS_MIN ||
        credential->iterations >
            APPLIANCE_ADMIN_CREDENTIAL_ITERATIONS_MAX) {
        return APPLIANCE_ADMIN_CREDENTIAL_BAD_ITERATIONS;
    }
    if (appliance_admin_credential_bytes_all_zero(
            credential->salt, APPLIANCE_ADMIN_CREDENTIAL_SALT_LENGTH)) {
        return APPLIANCE_ADMIN_CREDENTIAL_BAD_SALT;
    }
    if (appliance_admin_credential_bytes_all_zero(
            credential->verifier,
            APPLIANCE_ADMIN_CREDENTIAL_VERIFIER_LENGTH)) {
        return APPLIANCE_ADMIN_CREDENTIAL_BAD_VERIFIER;
    }

    value = credential->iterations;
    while (value != 0U) {
        iterations_text[iterations_length] =
            (char)('0' + (int)(value % 10U));
        value /= 10U;
        ++iterations_length;
    }
    /* Canonical, so no leading zero; reverse into the output. */
    if (buffer_size < label_length +
                          iterations_length +
                          APPLIANCE_ADMIN_CREDENTIAL_HEX_SALT_LENGTH +
                          APPLIANCE_ADMIN_CREDENTIAL_HEX_VERIFIER_LENGTH +
                          login_length + 5U) {
        return APPLIANCE_ADMIN_CREDENTIAL_BUFFER_TOO_SMALL;
    }
    memcpy(buffer + offset, label, label_length);
    offset += label_length;
    buffer[offset] = APPLIANCE_ADMIN_CREDENTIAL_RECORD_SEPARATOR;
    ++offset;
    {
        size_t index;

        for (index = 0U; index < iterations_length; ++index) {
            buffer[offset + index] =
                iterations_text[iterations_length - 1U - index];
        }
        offset += iterations_length;
    }
    buffer[offset] = APPLIANCE_ADMIN_CREDENTIAL_RECORD_SEPARATOR;
    ++offset;
    appliance_admin_credential_encode_hex(
        credential->salt, APPLIANCE_ADMIN_CREDENTIAL_SALT_LENGTH,
        buffer + offset);
    offset += APPLIANCE_ADMIN_CREDENTIAL_HEX_SALT_LENGTH;
    buffer[offset] = APPLIANCE_ADMIN_CREDENTIAL_RECORD_SEPARATOR;
    ++offset;
    appliance_admin_credential_encode_hex(
        credential->verifier, APPLIANCE_ADMIN_CREDENTIAL_VERIFIER_LENGTH,
        buffer + offset);
    offset += APPLIANCE_ADMIN_CREDENTIAL_HEX_VERIFIER_LENGTH;
    buffer[offset] = APPLIANCE_ADMIN_CREDENTIAL_RECORD_SEPARATOR;
    ++offset;
    memcpy(buffer + offset, credential->login, login_length);
    offset += login_length;
    buffer[offset] = '\0';
    *length_out = offset;
    return APPLIANCE_ADMIN_CREDENTIAL_OK;
}

enum appliance_admin_credential_status
appliance_admin_credential_record_decode(
    const char *value,
    size_t value_length,
    struct appliance_admin_credential *credential_out)
{
    static const char label[] = APPLIANCE_ADMIN_CREDENTIAL_RECORD_LABEL;
    const size_t label_length = sizeof(label) - 1U;
    struct appliance_admin_credential decoded;
    size_t offset;
    size_t field_start;
    uint32_t iterations = 0U;
    size_t iterations_length = 0U;
    size_t login_length;

    if (value == NULL || credential_out == NULL) {
        return APPLIANCE_ADMIN_CREDENTIAL_NULL;
    }
    /*
     * Parse into a local and copy out only on success: a refused value must
     * never leave a partially filled credential behind, because the store's
     * load path keeps the struct it passed in.
     */
    appliance_admin_credential_zeroize(
        credential_out, sizeof(*credential_out));
    appliance_admin_credential_zeroize(&decoded, sizeof(decoded));
    if (appliance_admin_credential_record_is_legacy(value, value_length)) {
        return APPLIANCE_ADMIN_CREDENTIAL_LEGACY_RECORD;
    }
    if (value_length <= label_length ||
        memcmp(value, label, label_length) != 0 ||
        value[label_length] != APPLIANCE_ADMIN_CREDENTIAL_RECORD_SEPARATOR) {
        return APPLIANCE_ADMIN_CREDENTIAL_BAD_RECORD;
    }

    /* Iterations: 1..10 decimal digits, canonical (no leading zero). */
    field_start = label_length + 1U;
    offset = field_start;
    while (offset < value_length &&
           value[offset] != APPLIANCE_ADMIN_CREDENTIAL_RECORD_SEPARATOR) {
        const uint32_t digit = (uint32_t)(value[offset] - '0');

        if (value[offset] < '0' || value[offset] > '9' ||
            iterations_length >= 10U ||
            iterations > (UINT32_MAX - digit) / 10U) {
            return APPLIANCE_ADMIN_CREDENTIAL_BAD_RECORD;
        }
        iterations = iterations * 10U + digit;
        ++iterations_length;
        ++offset;
    }
    if (iterations_length == 0U ||
        (iterations_length > 1U && value[field_start] == '0') ||
        offset >= value_length ||
        value[offset] != APPLIANCE_ADMIN_CREDENTIAL_RECORD_SEPARATOR) {
        return APPLIANCE_ADMIN_CREDENTIAL_BAD_RECORD;
    }
    if (iterations < APPLIANCE_ADMIN_CREDENTIAL_ITERATIONS_MIN ||
        iterations > APPLIANCE_ADMIN_CREDENTIAL_ITERATIONS_MAX) {
        return APPLIANCE_ADMIN_CREDENTIAL_BAD_ITERATIONS;
    }

    /* Salt hex. */
    ++offset;
    if (value_length - offset <
        APPLIANCE_ADMIN_CREDENTIAL_HEX_SALT_LENGTH + 1U) {
        return APPLIANCE_ADMIN_CREDENTIAL_BAD_RECORD;
    }
    if (!appliance_admin_credential_decode_hex(
            value + offset, APPLIANCE_ADMIN_CREDENTIAL_HEX_SALT_LENGTH,
            decoded.salt, APPLIANCE_ADMIN_CREDENTIAL_SALT_LENGTH)) {
        return APPLIANCE_ADMIN_CREDENTIAL_BAD_RECORD;
    }
    offset += APPLIANCE_ADMIN_CREDENTIAL_HEX_SALT_LENGTH;
    if (value[offset] != APPLIANCE_ADMIN_CREDENTIAL_RECORD_SEPARATOR) {
        return APPLIANCE_ADMIN_CREDENTIAL_BAD_RECORD;
    }

    /* Verifier hex. */
    ++offset;
    if (value_length - offset <
        APPLIANCE_ADMIN_CREDENTIAL_HEX_VERIFIER_LENGTH + 1U) {
        return APPLIANCE_ADMIN_CREDENTIAL_BAD_RECORD;
    }
    if (!appliance_admin_credential_decode_hex(
            value + offset,
            APPLIANCE_ADMIN_CREDENTIAL_HEX_VERIFIER_LENGTH,
            decoded.verifier,
            APPLIANCE_ADMIN_CREDENTIAL_VERIFIER_LENGTH)) {
        return APPLIANCE_ADMIN_CREDENTIAL_BAD_RECORD;
    }
    offset += APPLIANCE_ADMIN_CREDENTIAL_HEX_VERIFIER_LENGTH;
    if (value[offset] != APPLIANCE_ADMIN_CREDENTIAL_RECORD_SEPARATOR) {
        return APPLIANCE_ADMIN_CREDENTIAL_BAD_RECORD;
    }

    /* Login: the rest of the value, policy-checked exactly. */
    ++offset;
    login_length = value_length - offset;
    if (appliance_admin_credential_validate_login(
            value + offset, login_length) !=
        APPLIANCE_ADMIN_CREDENTIAL_OK) {
        return APPLIANCE_ADMIN_CREDENTIAL_BAD_LOGIN;
    }
    if (appliance_admin_credential_bytes_all_zero(
            decoded.salt, APPLIANCE_ADMIN_CREDENTIAL_SALT_LENGTH)) {
        return APPLIANCE_ADMIN_CREDENTIAL_BAD_SALT;
    }
    if (appliance_admin_credential_bytes_all_zero(
            decoded.verifier, APPLIANCE_ADMIN_CREDENTIAL_VERIFIER_LENGTH)) {
        return APPLIANCE_ADMIN_CREDENTIAL_BAD_VERIFIER;
    }
    decoded.version = APPLIANCE_ADMIN_CREDENTIAL_VERSION;
    memcpy(decoded.login, value + offset, login_length);
    decoded.login[login_length] = '\0';
    decoded.iterations = iterations;
    *credential_out = decoded;
    appliance_admin_credential_zeroize(&decoded, sizeof(decoded));
    return APPLIANCE_ADMIN_CREDENTIAL_OK;
}

bool appliance_admin_credential_record_value_valid(
    const uint8_t *value,
    size_t value_length)
{
    struct appliance_admin_credential credential;

    if (value == NULL) {
        return false;
    }
    if (appliance_admin_credential_record_is_legacy(
            (const char *)value, value_length)) {
        return true;
    }
    if (value_length == 0U ||
        value_length > APPLIANCE_ADMIN_CREDENTIAL_RECORD_VALUE_MAX) {
        return false;
    }
    return appliance_admin_credential_record_decode(
               (const char *)value, value_length, &credential) ==
           APPLIANCE_ADMIN_CREDENTIAL_OK;
}

const char *appliance_admin_credential_status_string(
    enum appliance_admin_credential_status status)
{
    switch (status) {
    case APPLIANCE_ADMIN_CREDENTIAL_OK:
        return "APPLIANCE_ADMIN_CREDENTIAL_OK";
    case APPLIANCE_ADMIN_CREDENTIAL_NULL:
        return "APPLIANCE_ADMIN_CREDENTIAL_NULL";
    case APPLIANCE_ADMIN_CREDENTIAL_BAD_LOGIN:
        return "APPLIANCE_ADMIN_CREDENTIAL_BAD_LOGIN";
    case APPLIANCE_ADMIN_CREDENTIAL_BAD_PASSWORD:
        return "APPLIANCE_ADMIN_CREDENTIAL_BAD_PASSWORD";
    case APPLIANCE_ADMIN_CREDENTIAL_WEAK_PASSWORD:
        return "APPLIANCE_ADMIN_CREDENTIAL_WEAK_PASSWORD";
    case APPLIANCE_ADMIN_CREDENTIAL_BAD_ITERATIONS:
        return "APPLIANCE_ADMIN_CREDENTIAL_BAD_ITERATIONS";
    case APPLIANCE_ADMIN_CREDENTIAL_BAD_SALT:
        return "APPLIANCE_ADMIN_CREDENTIAL_BAD_SALT";
    case APPLIANCE_ADMIN_CREDENTIAL_BAD_VERIFIER:
        return "APPLIANCE_ADMIN_CREDENTIAL_BAD_VERIFIER";
    case APPLIANCE_ADMIN_CREDENTIAL_DENIED:
        return "APPLIANCE_ADMIN_CREDENTIAL_DENIED";
    case APPLIANCE_ADMIN_CREDENTIAL_LEGACY_RECORD:
        return "APPLIANCE_ADMIN_CREDENTIAL_LEGACY_RECORD";
    case APPLIANCE_ADMIN_CREDENTIAL_BAD_RECORD:
        return "APPLIANCE_ADMIN_CREDENTIAL_BAD_RECORD";
    case APPLIANCE_ADMIN_CREDENTIAL_BUFFER_TOO_SMALL:
        return "APPLIANCE_ADMIN_CREDENTIAL_BUFFER_TOO_SMALL";
    case APPLIANCE_ADMIN_CREDENTIAL_UNAVAILABLE:
        return "APPLIANCE_ADMIN_CREDENTIAL_UNAVAILABLE";
    case APPLIANCE_ADMIN_CREDENTIAL_BAD_STATE:
        return "APPLIANCE_ADMIN_CREDENTIAL_BAD_STATE";
    case APPLIANCE_ADMIN_CREDENTIAL_PENDING:
        return "APPLIANCE_ADMIN_CREDENTIAL_PENDING";
    case APPLIANCE_ADMIN_CREDENTIAL_CANCELLED:
        return "APPLIANCE_ADMIN_CREDENTIAL_CANCELLED";
    default:
        return NULL;
    }
}
