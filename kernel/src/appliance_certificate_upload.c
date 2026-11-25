#include "appliance_certificate_upload.h"

/*
 * ITEM-503: the module needs the import gate and the certificate owner, both
 * of which are production-TLS (or host-proof) objects.  In a reduced kernel
 * profile that links neither, this translation unit compiles to nothing, the
 * same way the two objects it depends on do.
 */
#if defined(HFOC_PRODUCTION_TLS) || \
    defined(HFOC_APPLIANCE_CERTIFICATE_UPLOAD_HOST_TEST)

#include "appliance_certificate_import.h"
#include "pem_decode.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/*
 * ITEM-503: the bounded BYO upload path.
 *
 * The body is one `application/x-www-form-urlencoded` document.  Fields are
 * `certificate`, `chain` and `key`; an unknown field is ignored exactly as the
 * sign-in form ignores one, but a malformed field, a duplicated field or a
 * missing required field is refused by name.  Values are percent-decoded into
 * fixed PEM buffers - `+` is a space, `%XX` is a byte, anything else after a
 * `%` is malformed - and a value that would not fit its buffer is refused
 * rather than truncated, so a partial document never reaches the validator.
 *
 * The private key is decoded and validated here because the import gate needs
 * it, and it is zeroed by `rollback()` and after a successful `commit()`.
 * Nothing in this module logs, renders or copies the key anywhere else.
 */

#define APPLIANCE_CERTIFICATE_UPLOAD_PRIVATE_KEY_LABEL "PRIVATE KEY"
#define APPLIANCE_CERTIFICATE_UPLOAD_EC_KEY_LABEL "EC PRIVATE KEY"
#define APPLIANCE_CERTIFICATE_UPLOAD_CERTIFICATE_LABEL "CERTIFICATE"
#define APPLIANCE_CERTIFICATE_UPLOAD_CHAIN_BEGIN \
    "-----BEGIN CERTIFICATE-----"
#define APPLIANCE_CERTIFICATE_UPLOAD_CHAIN_BEGIN_LENGTH 27U

static void appliance_certificate_upload_zero(
    void *buffer,
    size_t length)
{
    volatile uint8_t *bytes = (volatile uint8_t *)buffer;
    size_t index;

    for (index = 0U; index < length; ++index) {
        bytes[index] = 0U;
    }
}

static int appliance_certificate_upload_hex(uint8_t character)
{
    if (character >= (uint8_t)'0' && character <= (uint8_t)'9') {
        return (int)(character - (uint8_t)'0');
    }
    if (character >= (uint8_t)'a' && character <= (uint8_t)'f') {
        return (int)(character - (uint8_t)'a') + 10;
    }
    if (character >= (uint8_t)'A' && character <= (uint8_t)'F') {
        return (int)(character - (uint8_t)'A') + 10;
    }
    return -1;
}

/*
 * Percent-decode one form value into `output`.  The output is NUL-terminated
 * inside the capacity; `output_length` does not count the terminator.  A
 * malformed escape and a value that would not fit are distinct refusals, so
 * the operator is told which one happened; a value is never truncated.
 */
static enum appliance_certificate_upload_status
appliance_certificate_upload_decode(
    const uint8_t *value,
    uint32_t value_length,
    char *output,
    size_t capacity,
    size_t *output_length)
{
    size_t written = 0U;
    uint32_t index = 0U;

    if (value == NULL || output == NULL || output_length == NULL ||
        capacity == 0U) {
        return APPLIANCE_CERTIFICATE_UPLOAD_NULL;
    }
    while (index < value_length) {
        uint8_t byte = value[index];

        if (byte == (uint8_t)'+') {
            byte = (uint8_t)' ';
            ++index;
        } else if (byte == (uint8_t)'%') {
            int high;
            int low;

            if (index + 2U >= value_length) {
                return APPLIANCE_CERTIFICATE_UPLOAD_MALFORMED;
            }
            high = appliance_certificate_upload_hex(value[index + 1U]);
            low = appliance_certificate_upload_hex(value[index + 2U]);
            if (high < 0 || low < 0) {
                return APPLIANCE_CERTIFICATE_UPLOAD_MALFORMED;
            }
            byte = (uint8_t)((high << 4) | low);
            index += 3U;
        } else {
            ++index;
        }
        if (written + 1U >= capacity) {
            return APPLIANCE_CERTIFICATE_UPLOAD_TOO_LARGE;
        }
        output[written++] = (char)byte;
    }
    output[written] = '\0';
    *output_length = written;
    return APPLIANCE_CERTIFICATE_UPLOAD_OK;
}

static bool appliance_certificate_upload_field(
    const uint8_t *name,
    uint32_t name_length,
    const char *expected)
{
    size_t expected_length = strlen(expected);

    return (size_t)name_length == expected_length &&
           memcmp(name, expected, expected_length) == 0;
}

/*
 * The first occurrence of `needle` at or after `from`, or SIZE_MAX.  Used only
 * to advance from one PEM block to the next inside a chain bundle; the PEM
 * decoder itself validates the block it is given.
 */
static size_t appliance_certificate_upload_find(
    const char *text,
    size_t text_length,
    const char *needle,
    size_t needle_length,
    size_t from)
{
    size_t index;

    if (text == NULL || needle == NULL || needle_length == 0U ||
        from >= text_length || needle_length > text_length - from) {
        return SIZE_MAX;
    }
    for (index = from; index + needle_length <= text_length; ++index) {
        if (memcmp(text + index, needle, needle_length) == 0) {
            return index;
        }
    }
    return SIZE_MAX;
}

static enum appliance_certificate_upload_status
appliance_certificate_upload_map_import(
    enum appliance_certificate_import_status status)
{
    switch (status) {
    case APPLIANCE_CERTIFICATE_IMPORT_OK:
        return APPLIANCE_CERTIFICATE_UPLOAD_OK;
    case APPLIANCE_CERTIFICATE_IMPORT_TOO_LARGE:
        return APPLIANCE_CERTIFICATE_UPLOAD_TOO_LARGE;
    case APPLIANCE_CERTIFICATE_IMPORT_BAD_CERTIFICATE:
        return APPLIANCE_CERTIFICATE_UPLOAD_BAD_CERTIFICATE;
    case APPLIANCE_CERTIFICATE_IMPORT_BAD_KEY:
        return APPLIANCE_CERTIFICATE_UPLOAD_BAD_KEY;
    case APPLIANCE_CERTIFICATE_IMPORT_UNSUPPORTED_KEY:
        return APPLIANCE_CERTIFICATE_UPLOAD_UNSUPPORTED_KEY;
    case APPLIANCE_CERTIFICATE_IMPORT_BAD_PAIR:
        return APPLIANCE_CERTIFICATE_UPLOAD_BAD_PAIR;
    case APPLIANCE_CERTIFICATE_IMPORT_BAD_CLOCK:
        return APPLIANCE_CERTIFICATE_UPLOAD_BAD_CLOCK;
    case APPLIANCE_CERTIFICATE_IMPORT_NOT_YET_VALID:
        return APPLIANCE_CERTIFICATE_UPLOAD_NOT_YET_VALID;
    case APPLIANCE_CERTIFICATE_IMPORT_EXPIRED:
        return APPLIANCE_CERTIFICATE_UPLOAD_EXPIRED;
    case APPLIANCE_CERTIFICATE_IMPORT_NO_SUBJECT_ALT_NAME:
        return APPLIANCE_CERTIFICATE_UPLOAD_NO_SUBJECT_ALT_NAME;
    case APPLIANCE_CERTIFICATE_IMPORT_NAME_MISMATCH:
        return APPLIANCE_CERTIFICATE_UPLOAD_NAME_MISMATCH;
    case APPLIANCE_CERTIFICATE_IMPORT_BAD_CHAIN:
        return APPLIANCE_CERTIFICATE_UPLOAD_BAD_CHAIN;
    case APPLIANCE_CERTIFICATE_IMPORT_CHAIN_MISMATCH:
        return APPLIANCE_CERTIFICATE_UPLOAD_CHAIN_MISMATCH;
    case APPLIANCE_CERTIFICATE_IMPORT_NULL:
    default:
        return APPLIANCE_CERTIFICATE_UPLOAD_MALFORMED;
    }
}

enum appliance_certificate_upload_status appliance_certificate_upload_init(
    struct appliance_certificate_upload *upload,
    struct appliance_production_certificate *certificate,
    int (*f_rng)(void *context, unsigned char *output, size_t length),
    void *rng_context)
{
    if (upload == NULL || certificate == NULL || f_rng == NULL) {
        return APPLIANCE_CERTIFICATE_UPLOAD_NULL;
    }
    memset(upload, 0, sizeof(*upload));
    upload->initialized = true;
    upload->version = APPLIANCE_CERTIFICATE_UPLOAD_VERSION;
    upload->certificate = certificate;
    upload->f_rng = f_rng;
    upload->rng_context = rng_context;
    return APPLIANCE_CERTIFICATE_UPLOAD_OK;
}

enum appliance_certificate_upload_status appliance_certificate_upload_set_clock(
    struct appliance_certificate_upload *upload,
    uint32_t now_epoch_seconds)
{
    if (upload == NULL || !upload->initialized ||
        upload->version != APPLIANCE_CERTIFICATE_UPLOAD_VERSION) {
        return APPLIANCE_CERTIFICATE_UPLOAD_NULL;
    }
    upload->now_epoch_seconds = now_epoch_seconds;
    return APPLIANCE_CERTIFICATE_UPLOAD_OK;
}

static enum appliance_certificate_upload_status
appliance_certificate_upload_validate_candidate(
    struct appliance_certificate_upload *upload,
    struct appliance_certificate_import_result *result)
{
    enum appliance_certificate_import_status status;

    memset(result, 0, sizeof(*result));
    status = appliance_certificate_import_validate(
        upload->certificate_der,
        upload->certificate_length,
        upload->key_der,
        upload->key_length,
        upload->chain_length != 0U ? upload->chain_der : NULL,
        upload->chain_length,
        upload->now_epoch_seconds,
        upload->certificate->common_name,
        upload->certificate->common_name_length,
        upload->f_rng,
        upload->rng_context,
        result);
    return appliance_certificate_upload_map_import(status);
}

enum appliance_certificate_upload_status appliance_certificate_upload_stage(
    struct appliance_certificate_upload *upload,
    const uint8_t *content_type,
    uint32_t content_type_length,
    const uint8_t *body,
    uint32_t body_length)
{
    /*
     * The PEM buffers are file-scope: together they are 9 KiB and the kernel
     * boot stack is a fraction of that.  This path is single-threaded.
     */
    static char certificate_pem[
        APPLIANCE_CERTIFICATE_UPLOAD_CERTIFICATE_PEM_MAX];
    static char chain_pem[APPLIANCE_CERTIFICATE_UPLOAD_CHAIN_PEM_MAX];
    static char key_pem[APPLIANCE_CERTIFICATE_UPLOAD_KEY_PEM_MAX];
    struct appliance_certificate_import_result result;
    size_t certificate_pem_length = 0U;
    size_t chain_pem_length = 0U;
    size_t key_pem_length = 0U;
    bool certificate_seen = false;
    bool chain_seen = false;
    bool key_seen = false;
    uint32_t index = 0U;
    enum appliance_certificate_upload_status status;

    if (upload == NULL || !upload->initialized ||
        upload->version != APPLIANCE_CERTIFICATE_UPLOAD_VERSION) {
        return APPLIANCE_CERTIFICATE_UPLOAD_NULL;
    }
    upload->last_status = APPLIANCE_CERTIFICATE_UPLOAD_NULL;
    if ((content_type == NULL && content_type_length != 0U) ||
        (body == NULL && body_length != 0U)) {
        return APPLIANCE_CERTIFICATE_UPLOAD_NULL;
    }
    if (content_type_length !=
            APPLIANCE_CERTIFICATE_UPLOAD_CONTENT_TYPE_LENGTH ||
        memcmp(content_type,
               APPLIANCE_CERTIFICATE_UPLOAD_CONTENT_TYPE,
               APPLIANCE_CERTIFICATE_UPLOAD_CONTENT_TYPE_LENGTH) != 0) {
        upload->last_status =
            APPLIANCE_CERTIFICATE_UPLOAD_BAD_CONTENT_TYPE;
        return upload->last_status;
    }
    if (body_length > APPLIANCE_CERTIFICATE_UPLOAD_BODY_MAX) {
        upload->last_status = APPLIANCE_CERTIFICATE_UPLOAD_TOO_LARGE;
        return upload->last_status;
    }
    if (upload->certificate->common_name_length == 0U) {
        upload->last_status =
            APPLIANCE_CERTIFICATE_UPLOAD_NAME_UNAVAILABLE;
        return upload->last_status;
    }
    /*
     * A new candidate replaces the previous one; the old key material is
     * zeroed before anything else is written.
     */
    (void)appliance_certificate_upload_rollback(upload);

    memset(certificate_pem, 0, sizeof(certificate_pem));
    memset(chain_pem, 0, sizeof(chain_pem));
    memset(key_pem, 0, sizeof(key_pem));
    while (index < body_length) {
        uint32_t field_end = index;
        uint32_t separator = index;
        const uint8_t *name;
        uint32_t name_length;
        const uint8_t *value;
        uint32_t value_length;

        while (field_end < body_length &&
               body[field_end] != (uint8_t)'&') {
            ++field_end;
        }
        while (separator < field_end &&
               body[separator] != (uint8_t)'=') {
            ++separator;
        }
        if (separator >= field_end) {
            status = APPLIANCE_CERTIFICATE_UPLOAD_MALFORMED;
            goto refuse;
        }
        name = body + index;
        name_length = separator - index;
        value = body + separator + 1U;
        value_length = field_end - separator - 1U;

        if (appliance_certificate_upload_field(
                name, name_length, "certificate")) {
            if (certificate_seen) {
                status = APPLIANCE_CERTIFICATE_UPLOAD_MALFORMED;
                goto refuse;
            }
            status = appliance_certificate_upload_decode(
                value,
                value_length,
                certificate_pem,
                sizeof(certificate_pem),
                &certificate_pem_length);
            if (status != APPLIANCE_CERTIFICATE_UPLOAD_OK) {
                goto refuse;
            }
            certificate_seen = true;
        } else if (appliance_certificate_upload_field(
                       name, name_length, "chain")) {
            if (chain_seen) {
                status = APPLIANCE_CERTIFICATE_UPLOAD_MALFORMED;
                goto refuse;
            }
            status = appliance_certificate_upload_decode(
                value,
                value_length,
                chain_pem,
                sizeof(chain_pem),
                &chain_pem_length);
            if (status != APPLIANCE_CERTIFICATE_UPLOAD_OK) {
                goto refuse;
            }
            chain_seen = true;
        } else if (appliance_certificate_upload_field(
                       name, name_length, "key")) {
            if (key_seen) {
                status = APPLIANCE_CERTIFICATE_UPLOAD_MALFORMED;
                goto refuse;
            }
            status = appliance_certificate_upload_decode(
                value,
                value_length,
                key_pem,
                sizeof(key_pem),
                &key_pem_length);
            if (status != APPLIANCE_CERTIFICATE_UPLOAD_OK) {
                goto refuse;
            }
            key_seen = true;
        } else if (name_length == 0U) {
            status = APPLIANCE_CERTIFICATE_UPLOAD_MALFORMED;
            goto refuse;
        }
        index = field_end + 1U;
    }
    if (!certificate_seen || !key_seen) {
        status = APPLIANCE_CERTIFICATE_UPLOAD_MISSING_FIELD;
        goto refuse;
    }

    {
        size_t decoded = 0U;
        enum pem_decode_status pem_status;

        pem_status = pem_decode_block(
            certificate_pem,
            certificate_pem_length,
            APPLIANCE_CERTIFICATE_UPLOAD_CERTIFICATE_LABEL,
            upload->certificate_der,
            sizeof(upload->certificate_der),
            &decoded);
        if (pem_status != PEM_DECODE_OK) {
            status = pem_status == PEM_DECODE_TOO_LARGE
                ? APPLIANCE_CERTIFICATE_UPLOAD_TOO_LARGE
                : APPLIANCE_CERTIFICATE_UPLOAD_BAD_CERTIFICATE;
            goto refuse;
        }
        upload->certificate_length = (uint32_t)decoded;
    }
    {
        size_t decoded = 0U;
        enum pem_decode_status pem_status;

        /*
         * The key file is PKCS#8 (`PRIVATE KEY`) or SEC1 (`EC PRIVATE KEY`);
         * both are what `openssl` writes, and the import gate parses either.
         * The label is tried in that order and a block with a different label
         * is not touched.
         */
        pem_status = pem_decode_block(
            key_pem,
            key_pem_length,
            APPLIANCE_CERTIFICATE_UPLOAD_PRIVATE_KEY_LABEL,
            upload->key_der,
            sizeof(upload->key_der),
            &decoded);
        if (pem_status == PEM_DECODE_NO_BLOCK) {
            pem_status = pem_decode_block(
                key_pem,
                key_pem_length,
                APPLIANCE_CERTIFICATE_UPLOAD_EC_KEY_LABEL,
                upload->key_der,
                sizeof(upload->key_der),
                &decoded);
        }
        if (pem_status != PEM_DECODE_OK) {
            status = pem_status == PEM_DECODE_TOO_LARGE
                ? APPLIANCE_CERTIFICATE_UPLOAD_TOO_LARGE
                : APPLIANCE_CERTIFICATE_UPLOAD_BAD_KEY;
            goto refuse;
        }
        upload->key_length = (uint32_t)decoded;
    }
    if (chain_seen && chain_pem_length != 0U) {
        size_t offset = 0U;

        while (offset < chain_pem_length) {
            const size_t position = appliance_certificate_upload_find(
                chain_pem,
                chain_pem_length,
                APPLIANCE_CERTIFICATE_UPLOAD_CHAIN_BEGIN,
                APPLIANCE_CERTIFICATE_UPLOAD_CHAIN_BEGIN_LENGTH,
                offset);
            size_t decoded = 0U;
            enum pem_decode_status pem_status;

            if (position == SIZE_MAX) {
                break;
            }
            pem_status = pem_decode_block(
                chain_pem + position,
                chain_pem_length - position,
                APPLIANCE_CERTIFICATE_UPLOAD_CERTIFICATE_LABEL,
                upload->chain_der + upload->chain_length,
                sizeof(upload->chain_der) - upload->chain_length,
                &decoded);
            if (pem_status != PEM_DECODE_OK) {
                status = pem_status == PEM_DECODE_TOO_LARGE
                    ? APPLIANCE_CERTIFICATE_UPLOAD_TOO_LARGE
                    : APPLIANCE_CERTIFICATE_UPLOAD_BAD_CHAIN;
                goto refuse;
            }
            upload->chain_length += (uint32_t)decoded;
            ++upload->chain_count;
            offset = position +
                     APPLIANCE_CERTIFICATE_UPLOAD_CHAIN_BEGIN_LENGTH;
        }
    }

    status = appliance_certificate_upload_validate_candidate(
        upload, &result);
    if (status != APPLIANCE_CERTIFICATE_UPLOAD_OK) {
        goto refuse;
    }
    memcpy(upload->fingerprint, result.fingerprint,
           sizeof(upload->fingerprint));
    upload->name_length = result.name_length <=
            APPLIANCE_PRODUCTION_CERTIFICATE_COMMON_NAME_MAX
        ? result.name_length
        : APPLIANCE_PRODUCTION_CERTIFICATE_COMMON_NAME_MAX;
    memcpy(upload->name, result.name, upload->name_length);
    upload->name[upload->name_length] = '\0';
    upload->not_before_epoch = result.not_before_epoch;
    upload->not_after_epoch = result.not_after_epoch;
    upload->chain_count = result.chain_count;
    upload->staged = true;
    upload->last_status = APPLIANCE_CERTIFICATE_UPLOAD_OK;
    ++upload->stage_count;
    appliance_certificate_upload_zero(certificate_pem,
                                      sizeof(certificate_pem));
    appliance_certificate_upload_zero(chain_pem, sizeof(chain_pem));
    appliance_certificate_upload_zero(key_pem, sizeof(key_pem));
    return APPLIANCE_CERTIFICATE_UPLOAD_OK;

refuse:
    appliance_certificate_upload_zero(certificate_pem,
                                      sizeof(certificate_pem));
    appliance_certificate_upload_zero(chain_pem, sizeof(chain_pem));
    appliance_certificate_upload_zero(key_pem, sizeof(key_pem));
    (void)appliance_certificate_upload_rollback(upload);
    upload->last_status = status;
    ++upload->refused_count;
    return status;
}

enum appliance_certificate_upload_status appliance_certificate_upload_commit(
    struct appliance_certificate_upload *upload)
{
    struct appliance_certificate_import_result result;
    enum appliance_certificate_upload_status status;
    enum appliance_production_certificate_status certificate_status;
    uint8_t fingerprint[SHA256_DIGEST_SIZE];

    if (upload == NULL || !upload->initialized ||
        upload->version != APPLIANCE_CERTIFICATE_UPLOAD_VERSION) {
        return APPLIANCE_CERTIFICATE_UPLOAD_NULL;
    }
    if (!upload->staged) {
        upload->last_status = APPLIANCE_CERTIFICATE_UPLOAD_NO_CANDIDATE;
        return upload->last_status;
    }
    /*
     * The clock may have moved since staging, so the candidate is validated
     * again before anything durable is written; a certificate that expired in
     * between is refused here, not persisted.
     */
    status = appliance_certificate_upload_validate_candidate(
        upload, &result);
    if (status != APPLIANCE_CERTIFICATE_UPLOAD_OK) {
        (void)appliance_certificate_upload_rollback(upload);
        upload->last_status = status;
        ++upload->refused_count;
        return status;
    }
    certificate_status = appliance_production_certificate_adopt(
        upload->certificate,
        upload->certificate_der,
        upload->certificate_length,
        upload->chain_length != 0U ? upload->chain_der : NULL,
        upload->chain_length,
        upload->key_der,
        upload->key_length,
        fingerprint);
    if (certificate_status != APPLIANCE_PRODUCTION_CERTIFICATE_OK) {
        if (certificate_status ==
                APPLIANCE_PRODUCTION_CERTIFICATE_STORE_UNAVAILABLE ||
            certificate_status ==
                APPLIANCE_PRODUCTION_CERTIFICATE_STORE_SEALED) {
            status = APPLIANCE_CERTIFICATE_UPLOAD_STORE_UNAVAILABLE;
        } else {
            status = APPLIANCE_CERTIFICATE_UPLOAD_STORE_FAILED;
        }
        /*
         * The candidate is kept only when the store refused transiently?  No:
         * a failed commit is a failed import, and the caller confirms again
         * from scratch.  The key material is zeroed either way.
         */
        (void)appliance_certificate_upload_rollback(upload);
        upload->last_status = status;
        ++upload->refused_count;
        return status;
    }
    memcpy(upload->fingerprint, fingerprint, sizeof(upload->fingerprint));
    upload->staged = false;
    ++upload->commit_count;
    upload->last_status = APPLIANCE_CERTIFICATE_UPLOAD_OK;
    appliance_certificate_upload_zero(
        upload->key_der, sizeof(upload->key_der));
    appliance_certificate_upload_zero(
        upload->certificate_der, sizeof(upload->certificate_der));
    appliance_certificate_upload_zero(
        upload->chain_der, sizeof(upload->chain_der));
    upload->key_length = 0U;
    upload->certificate_length = 0U;
    upload->chain_length = 0U;
    return APPLIANCE_CERTIFICATE_UPLOAD_OK;
}

enum appliance_certificate_upload_status appliance_certificate_upload_rollback(
    struct appliance_certificate_upload *upload)
{
    if (upload == NULL || !upload->initialized ||
        upload->version != APPLIANCE_CERTIFICATE_UPLOAD_VERSION) {
        return APPLIANCE_CERTIFICATE_UPLOAD_NULL;
    }
    appliance_certificate_upload_zero(
        upload->key_der, sizeof(upload->key_der));
    appliance_certificate_upload_zero(
        upload->certificate_der, sizeof(upload->certificate_der));
    appliance_certificate_upload_zero(
        upload->chain_der, sizeof(upload->chain_der));
    upload->key_length = 0U;
    upload->certificate_length = 0U;
    upload->chain_length = 0U;
    upload->chain_count = 0U;
    upload->staged = false;
    return APPLIANCE_CERTIFICATE_UPLOAD_OK;
}

bool appliance_certificate_upload_staged(
    const struct appliance_certificate_upload *upload)
{
    return upload != NULL && upload->initialized &&
           upload->staged;
}

enum appliance_certificate_upload_status
appliance_certificate_upload_last_status(
    const struct appliance_certificate_upload *upload)
{
    return upload != NULL
        ? upload->last_status
        : APPLIANCE_CERTIFICATE_UPLOAD_NULL;
}

const uint8_t *appliance_certificate_upload_fingerprint(
    const struct appliance_certificate_upload *upload)
{
    if (upload == NULL || !upload->initialized) {
        return NULL;
    }
    return upload->fingerprint;
}

const char *appliance_certificate_upload_name(
    const struct appliance_certificate_upload *upload)
{
    if (upload == NULL || !upload->initialized ||
        upload->name_length == 0U) {
        return NULL;
    }
    return upload->name;
}

uint32_t appliance_certificate_upload_chain_count(
    const struct appliance_certificate_upload *upload)
{
    return upload != NULL && upload->initialized
        ? upload->chain_count
        : 0U;
}

const char *appliance_certificate_upload_status_string(
    enum appliance_certificate_upload_status status)
{
    switch (status) {
    case APPLIANCE_CERTIFICATE_UPLOAD_OK:
        return "OK";
    case APPLIANCE_CERTIFICATE_UPLOAD_NULL:
        return "NULL";
    case APPLIANCE_CERTIFICATE_UPLOAD_NO_CANDIDATE:
        return "NO_CANDIDATE";
    case APPLIANCE_CERTIFICATE_UPLOAD_BAD_CONTENT_TYPE:
        return "BAD_CONTENT_TYPE";
    case APPLIANCE_CERTIFICATE_UPLOAD_MALFORMED:
        return "MALFORMED";
    case APPLIANCE_CERTIFICATE_UPLOAD_MISSING_FIELD:
        return "MISSING_FIELD";
    case APPLIANCE_CERTIFICATE_UPLOAD_TOO_LARGE:
        return "TOO_LARGE";
    case APPLIANCE_CERTIFICATE_UPLOAD_NAME_UNAVAILABLE:
        return "NAME_UNAVAILABLE";
    case APPLIANCE_CERTIFICATE_UPLOAD_BAD_CERTIFICATE:
        return "BAD_CERTIFICATE";
    case APPLIANCE_CERTIFICATE_UPLOAD_BAD_KEY:
        return "BAD_KEY";
    case APPLIANCE_CERTIFICATE_UPLOAD_UNSUPPORTED_KEY:
        return "UNSUPPORTED_KEY";
    case APPLIANCE_CERTIFICATE_UPLOAD_BAD_PAIR:
        return "BAD_PAIR";
    case APPLIANCE_CERTIFICATE_UPLOAD_BAD_CLOCK:
        return "BAD_CLOCK";
    case APPLIANCE_CERTIFICATE_UPLOAD_NOT_YET_VALID:
        return "NOT_YET_VALID";
    case APPLIANCE_CERTIFICATE_UPLOAD_EXPIRED:
        return "EXPIRED";
    case APPLIANCE_CERTIFICATE_UPLOAD_NO_SUBJECT_ALT_NAME:
        return "NO_SUBJECT_ALT_NAME";
    case APPLIANCE_CERTIFICATE_UPLOAD_NAME_MISMATCH:
        return "NAME_MISMATCH";
    case APPLIANCE_CERTIFICATE_UPLOAD_BAD_CHAIN:
        return "BAD_CHAIN";
    case APPLIANCE_CERTIFICATE_UPLOAD_CHAIN_MISMATCH:
        return "CHAIN_MISMATCH";
    case APPLIANCE_CERTIFICATE_UPLOAD_STORE_UNAVAILABLE:
        return "STORE_UNAVAILABLE";
    case APPLIANCE_CERTIFICATE_UPLOAD_STORE_FAILED:
        return "STORE_FAILED";
    default:
        return "UNKNOWN";
    }
}

#else

typedef int appliance_certificate_upload_translation_unit_guard;

#endif
