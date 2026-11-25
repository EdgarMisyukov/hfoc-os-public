#ifndef HFOC_APPLIANCE_CERTIFICATE_UPLOAD_H
#define HFOC_APPLIANCE_CERTIFICATE_UPLOAD_H

#include "appliance_production_certificate.h"
#include "sha256.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * ITEM-503: the operator's BYO certificate as the panel form and the API
 * target submit it.
 *
 * The upload module is the bounded, allocation-free staging half of the
 * certificate import:
 *
 *   1. decode one `application/x-www-form-urlencoded` body into the three
 *      fields `certificate` (required), `chain` (optional, a bundle of
 *      certificates) and `key` (required);
 *   2. decode the PEM armour with the ITEM-503 decoder - a bundle is read
 *      block by block, never guessed at;
 *   3. validate the pair and the optional chain with
 *      `appliance_certificate_import_validate` against the appliance's own
 *      declared name and the validated wall clock;
 *   4. hold the accepted DER bytes as one candidate.  Staging writes nothing.
 *
 * `commit()` re-validates the candidate (the clock may have moved) and writes
 * it through `appliance_production_certificate_adopt`, which persists record
 * v2 and reads it back.  The active in-memory certificate stays untouched:
 * the listener's TLS configuration is built once, so an import applies on the
 * next boot and the caller publishes that honestly.  `rollback()` drops the
 * candidate and zeroes the key material.
 *
 * Nothing here authenticates or authorizes: the caller owns the two-phase
 * mutation, the session and the audit.  This module never writes the state
 * volume except inside `commit()`, and never logs private material.
 */

#define APPLIANCE_CERTIFICATE_UPLOAD_VERSION 1U
/*
 * The frozen request framing bound: a body larger than this never reaches the
 * module, and one that does is refused by name rather than truncated.
 */
#define APPLIANCE_CERTIFICATE_UPLOAD_BODY_MAX 8192U
/* PEM text bounds: the DER bounds with base64 expansion and armour. */
#define APPLIANCE_CERTIFICATE_UPLOAD_CERTIFICATE_PEM_MAX 2048U
#define APPLIANCE_CERTIFICATE_UPLOAD_CHAIN_PEM_MAX 6144U
#define APPLIANCE_CERTIFICATE_UPLOAD_KEY_PEM_MAX 1024U
#define APPLIANCE_CERTIFICATE_UPLOAD_CONTENT_TYPE \
    "application/x-www-form-urlencoded"
#define APPLIANCE_CERTIFICATE_UPLOAD_CONTENT_TYPE_LENGTH 33U

enum appliance_certificate_upload_status {
    APPLIANCE_CERTIFICATE_UPLOAD_OK = 0,
    APPLIANCE_CERTIFICATE_UPLOAD_NULL = 1,
    APPLIANCE_CERTIFICATE_UPLOAD_NO_CANDIDATE = 2,
    APPLIANCE_CERTIFICATE_UPLOAD_BAD_CONTENT_TYPE = 3,
    APPLIANCE_CERTIFICATE_UPLOAD_MALFORMED = 4,
    APPLIANCE_CERTIFICATE_UPLOAD_MISSING_FIELD = 5,
    APPLIANCE_CERTIFICATE_UPLOAD_TOO_LARGE = 6,
    APPLIANCE_CERTIFICATE_UPLOAD_NAME_UNAVAILABLE = 7,
    /* The import gate's own refusals, kept by name. */
    APPLIANCE_CERTIFICATE_UPLOAD_BAD_CERTIFICATE = 8,
    APPLIANCE_CERTIFICATE_UPLOAD_BAD_KEY = 9,
    APPLIANCE_CERTIFICATE_UPLOAD_UNSUPPORTED_KEY = 10,
    APPLIANCE_CERTIFICATE_UPLOAD_BAD_PAIR = 11,
    APPLIANCE_CERTIFICATE_UPLOAD_BAD_CLOCK = 12,
    APPLIANCE_CERTIFICATE_UPLOAD_NOT_YET_VALID = 13,
    APPLIANCE_CERTIFICATE_UPLOAD_EXPIRED = 14,
    APPLIANCE_CERTIFICATE_UPLOAD_NO_SUBJECT_ALT_NAME = 15,
    APPLIANCE_CERTIFICATE_UPLOAD_NAME_MISMATCH = 16,
    APPLIANCE_CERTIFICATE_UPLOAD_BAD_CHAIN = 17,
    APPLIANCE_CERTIFICATE_UPLOAD_CHAIN_MISMATCH = 18,
    /* The store refusals, distinct from a rejected certificate. */
    APPLIANCE_CERTIFICATE_UPLOAD_STORE_UNAVAILABLE = 19,
    APPLIANCE_CERTIFICATE_UPLOAD_STORE_FAILED = 20,
};

struct appliance_certificate_upload {
    bool initialized;
    uint32_t version;
    bool staged;
    /* Borrowed: the certificate owner supplies the store and the name. */
    struct appliance_production_certificate *certificate;
    int (*f_rng)(void *context, unsigned char *output, size_t length);
    void *rng_context;
    uint32_t now_epoch_seconds;
    /* One candidate. */
    uint8_t certificate_der[APPLIANCE_PRODUCTION_CERTIFICATE_DER_MAX];
    uint32_t certificate_length;
    uint8_t chain_der[APPLIANCE_PRODUCTION_CERTIFICATE_CHAIN_DER_MAX];
    uint32_t chain_length;
    uint8_t key_der[APPLIANCE_PRODUCTION_CERTIFICATE_KEY_DER_MAX];
    uint32_t key_length;
    char name[APPLIANCE_PRODUCTION_CERTIFICATE_COMMON_NAME_MAX + 1U];
    uint32_t name_length;
    uint8_t fingerprint[SHA256_DIGEST_SIZE];
    uint32_t not_before_epoch;
    uint32_t not_after_epoch;
    uint32_t chain_count;
    enum appliance_certificate_upload_status last_status;
    uint32_t stage_count;
    uint32_t commit_count;
    uint32_t refused_count;
};

enum appliance_certificate_upload_status appliance_certificate_upload_init(
    struct appliance_certificate_upload *upload,
    struct appliance_production_certificate *certificate,
    int (*f_rng)(void *context, unsigned char *output, size_t length),
    void *rng_context);
/* The validated wall clock epoch; zero keeps every import refused. */
enum appliance_certificate_upload_status appliance_certificate_upload_set_clock(
    struct appliance_certificate_upload *upload,
    uint32_t now_epoch_seconds);
/* Decode, validate and hold one candidate; writes nothing durable. */
enum appliance_certificate_upload_status appliance_certificate_upload_stage(
    struct appliance_certificate_upload *upload,
    const uint8_t *content_type,
    uint32_t content_type_length,
    const uint8_t *body,
    uint32_t body_length);
/* Re-validate the candidate and persist it as certificate record v2. */
enum appliance_certificate_upload_status appliance_certificate_upload_commit(
    struct appliance_certificate_upload *upload);
/* Drop the candidate and zero the private key material. */
enum appliance_certificate_upload_status appliance_certificate_upload_rollback(
    struct appliance_certificate_upload *upload);
bool appliance_certificate_upload_staged(
    const struct appliance_certificate_upload *upload);
enum appliance_certificate_upload_status
appliance_certificate_upload_last_status(
    const struct appliance_certificate_upload *upload);
const char *appliance_certificate_upload_status_string(
    enum appliance_certificate_upload_status status);
/* The accepted candidate's fingerprint; NULL when none was staged. */
const uint8_t *appliance_certificate_upload_fingerprint(
    const struct appliance_certificate_upload *upload);
/* The accepted candidate's matched dNSName, NUL-terminated, or NULL. */
const char *appliance_certificate_upload_name(
    const struct appliance_certificate_upload *upload);
uint32_t appliance_certificate_upload_chain_count(
    const struct appliance_certificate_upload *upload);

#endif
