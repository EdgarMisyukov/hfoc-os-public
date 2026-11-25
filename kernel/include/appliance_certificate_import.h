#ifndef HFOC_APPLIANCE_CERTIFICATE_IMPORT_H
#define HFOC_APPLIANCE_CERTIFICATE_IMPORT_H

#include <stddef.h>
#include <stdint.h>

#include "sha256.h"

/*
 * ITEM-503: validate a certificate and its key before the appliance imports
 * them (BYO).  The product's own self-signed certificate goes through the
 * ITEM-375 lifecycle without this step because it was generated here; an
 * operator's certificate comes from outside and is checked first:
 *
 *   * the DER parses as a certificate and the key parses as a private key;
 *   * the key is an EC P-256 key - the frozen mbedTLS profile cannot serve any
 *     other family, so accepting one would promise a handshake that cannot
 *     happen;
 *   * the key really is the certificate's key (`mbedtls_pk_check_pair`);
 *   * the certificate is inside its validity window at the validated clock -
 *     a zero clock is refused, because "not yet checked" is not "valid";
 *   * the certificate names the appliance: one of its dNSName subjectAltNames
 *     equals the operator's declared name exactly, as the ITEM-375 lifecycle
 *     compares it (a chain certificate commonly carries several names, so
 *     every dNSName is offered, not only the first);
 *   * when a chain is supplied, every certificate in it parses and the chain
 *     really verifies the leaf - signatures, issuer linkage and the validity
 *     window of every member - with no verification flag set.  A chain that
 *     does not verify is refused by name and nothing is stored.  A chain is
 *     optional: an operator's self-signed certificate has none, and the
 *     appliance then makes no claim about how clients will trust it.
 *
 * Nothing here writes or touches a device: the caller stores the accepted
 * bytes through the lifecycle and the state volume.
 */

/* ITEM-503 follow-up: the chain parameter and the multi-name rule are v2. */
#define APPLIANCE_CERTIFICATE_IMPORT_VERSION 2U
/* The state record carries the certificate in APPLIANCE_PRODUCTION_* bounds. */
#define APPLIANCE_CERTIFICATE_IMPORT_DER_MAX 1024U
#define APPLIANCE_CERTIFICATE_IMPORT_KEY_DER_MAX 512U
#define APPLIANCE_CERTIFICATE_IMPORT_NAME_MAX 64U
/*
 * The chain is a concatenation of DER certificates, bounded so the upload
 * route can stage it in one buffer and so a hostile bundle cannot make the
 * appliance walk an unbounded list.
 */
#define APPLIANCE_CERTIFICATE_IMPORT_CHAIN_DER_MAX 4096U
#define APPLIANCE_CERTIFICATE_IMPORT_CHAIN_MAX 8U

enum appliance_certificate_import_status {
    APPLIANCE_CERTIFICATE_IMPORT_OK = 0,
    APPLIANCE_CERTIFICATE_IMPORT_NULL = 1,
    APPLIANCE_CERTIFICATE_IMPORT_TOO_LARGE = 2,
    APPLIANCE_CERTIFICATE_IMPORT_BAD_CERTIFICATE = 3,
    APPLIANCE_CERTIFICATE_IMPORT_BAD_KEY = 4,
    APPLIANCE_CERTIFICATE_IMPORT_UNSUPPORTED_KEY = 5,
    APPLIANCE_CERTIFICATE_IMPORT_BAD_PAIR = 6,
    APPLIANCE_CERTIFICATE_IMPORT_BAD_CLOCK = 7,
    APPLIANCE_CERTIFICATE_IMPORT_NOT_YET_VALID = 8,
    APPLIANCE_CERTIFICATE_IMPORT_EXPIRED = 9,
    APPLIANCE_CERTIFICATE_IMPORT_NO_SUBJECT_ALT_NAME = 10,
    APPLIANCE_CERTIFICATE_IMPORT_NAME_MISMATCH = 11,
    APPLIANCE_CERTIFICATE_IMPORT_BAD_CHAIN = 12,
    APPLIANCE_CERTIFICATE_IMPORT_CHAIN_MISMATCH = 13,
};

struct appliance_certificate_import_result {
    uint32_t version;
    /* The dNSName of the certificate that matched the declared name. */
    char name[APPLIANCE_CERTIFICATE_IMPORT_NAME_MAX + 1U];
    uint32_t name_length;
    uint32_t not_before_epoch;
    uint32_t not_after_epoch;
    /* How many certificates the verified chain carried; 0 when none was. */
    uint32_t chain_count;
    /* SHA-256 over the DER that was validated. */
    uint8_t fingerprint[SHA256_DIGEST_SIZE];
};

/*
 * `f_rng` is the platform random source: mbedTLS blinds the EC multiplication
 * the pair check performs, so it refuses to run without one.  The kernel
 * passes its entropy-backed generator and the host proof a deterministic stub.
 *
 * `chain_der`/`chain_length` are optional (NULL/0): when a chain is present it
 * must verify the certificate, and the caller stores the chain only after this
 * call returned OK.
 */
enum appliance_certificate_import_status
appliance_certificate_import_validate(
    const unsigned char *certificate_der,
    size_t certificate_length,
    const unsigned char *key_der,
    size_t key_length,
    const unsigned char *chain_der,
    size_t chain_length,
    uint32_t now_epoch_seconds,
    const char *expected_name,
    size_t expected_name_length,
    int (*f_rng)(void *context, unsigned char *output, size_t length),
    void *rng_context,
    struct appliance_certificate_import_result *result);

const char *appliance_certificate_import_status_string(
    enum appliance_certificate_import_status status);

#endif
