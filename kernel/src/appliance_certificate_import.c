#include "appliance_certificate_import.h"

#if defined(HFOC_PRODUCTION_TLS) || \
    defined(HFOC_APPLIANCE_CERTIFICATE_IMPORT_HOST_TEST)

#include "mbedtls/asn1.h"
#include "mbedtls/pk.h"
#include "mbedtls/sha256.h"
#include "mbedtls/x509_crt.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/*
 * ITEM-503: the BYO validation gate.  The rules are in the header; this file
 * is their mechanical form.  Every refusal is taken before the caller stores
 * anything, so a rejected upload leaves the previous certificate in place.
 */

/* Days from civil, valid for the uint32 epoch range (1970..2106). */
static int64_t appliance_certificate_import_month_days(
    int64_t year,
    int64_t month)
{
    static const int64_t lengths[12] = {
        31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31
    };

    if (month < 1 || month > 12) {
        return 0;
    }
    if (month == 2 &&
        ((year % 4 == 0 && year % 100 != 0) || year % 400 == 0)) {
        return 29;
    }
    return lengths[month - 1];
}

static bool appliance_certificate_import_epoch(
    const mbedtls_x509_time *time,
    uint32_t *epoch)
{
    int64_t year;
    int64_t month;
    int64_t day;
    int64_t era;
    int64_t year_of_era;
    int64_t day_of_year;
    int64_t day_of_era;
    int64_t days;
    int64_t seconds;
    int64_t month_days;

    if (time == NULL || epoch == NULL) {
        return false;
    }
    year = time->year;
    month = time->mon;
    day = time->day;
    if (year < 1970 || year > 2106 || month < 1 || month > 12 ||
        day < 1 || day > 31 || time->hour > 23 || time->min > 59 ||
        time->sec > 60) {
        return false;
    }
    /*
     * ITEM-503 follow-up: a date such as "31 February" would otherwise be
     * normalised into March and publish a validity window that is not the one
     * the certificate carries.  The calendar is checked before the conversion.
     */
    month_days = appliance_certificate_import_month_days(year, month);
    if (month_days == 0 || day > month_days) {
        return false;
    }
    year_of_era = year - (month <= 2 ? 1 : 0);
    era = (year_of_era >= 0 ? year_of_era : year_of_era - 399) / 400;
    year_of_era -= era * 400;
    day_of_year = (153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + day - 1;
    day_of_era =
        year_of_era * 365 + year_of_era / 4 - year_of_era / 100 +
        day_of_year;
    days = era * 146097 + day_of_era - 719468;
    seconds = days * 86400 + (int64_t)time->hour * 3600 +
              (int64_t)time->min * 60 + time->sec;
    if (seconds < 0 || seconds > (int64_t)UINT32_MAX) {
        return false;
    }
    *epoch = (uint32_t)seconds;
    return true;
}

/* The common name as text, when the subject carries exactly one CN. */
static bool appliance_certificate_import_common_name(
    const mbedtls_x509_crt *certificate,
    char *name,
    size_t capacity,
    size_t *name_length)
{
    size_t index;
    size_t written = 0U;
    const mbedtls_x509_name *entry;

    if (certificate == NULL || name == NULL || capacity == 0U ||
        name_length == NULL) {
        return false;
    }
    for (entry = &certificate->subject;
         entry != NULL;
         entry = entry->next) {
        if (entry->oid.p == NULL || entry->oid.len == 0U ||
            entry->val.p == NULL) {
            continue;
        }
        /* OID 2.5.4.3 (CN) is stored as the DER content 55 04 03. */
        if (entry->oid.len == 3U && entry->oid.p[0U] == 0x55U &&
            entry->oid.p[1U] == 0x04U && entry->oid.p[2U] == 0x03U) {
            for (index = 0U; index < entry->val.len; ++index) {
                if (written + 1U >= capacity) {
                    return false;
                }
                name[written++] = (char)entry->val.p[index];
            }
            name[written] = '\0';
            *name_length = written;
            return written != 0U;
        }
    }
    return false;
}

/*
 * ITEM-503 follow-up: every dNSName of the SAN extension is offered, and the
 * one that equals the operator's declared name is copied out.  A certificate
 * for an appliance commonly carries the public name beside other names (the
 * ITEM-375 request and the certificate a CA issues are compared exactly), so
 * comparing only the first dNSName refused valid uploads.  `saw_dns_name`
 * distinguishes "the certificate names no host at all" from "it names hosts,
 * but not this one", which are different refusals.
 */
static bool appliance_certificate_import_name_equal(
    const char *left,
    size_t left_length,
    const char *right,
    size_t right_length)
{
    return left != NULL && right != NULL && left_length == right_length &&
           left_length != 0U &&
           memcmp(left, right, left_length) == 0;
}

static bool appliance_certificate_import_dns_name(
    const mbedtls_x509_crt *certificate,
    const char *expected_name,
    size_t expected_name_length,
    char *name,
    size_t capacity,
    size_t *name_length,
    bool *saw_dns_name)
{
    const mbedtls_x509_sequence *entry;

    if (certificate == NULL || expected_name == NULL || name == NULL ||
        capacity == 0U || name_length == NULL || saw_dns_name == NULL) {
        return false;
    }
    *saw_dns_name = false;
    for (entry = &certificate->subject_alt_names;
         entry != NULL;
         entry = entry->next) {
        mbedtls_x509_subject_alternative_name san;
        int result;

        memset(&san, 0, sizeof(san));
        result = mbedtls_x509_parse_subject_alt_name(&entry->buf, &san);
        if (result != 0) {
            continue;
        }
        if (san.type == MBEDTLS_X509_SAN_DNS_NAME &&
            san.san.unstructured_name.len != 0U) {
            const size_t copy = san.san.unstructured_name.len;
            bool matches = false;

            *saw_dns_name = true;
            if (copy < capacity &&
                appliance_certificate_import_name_equal(
                    (const char *)san.san.unstructured_name.p, copy,
                    expected_name, expected_name_length)) {
                memcpy(name, san.san.unstructured_name.p, copy);
                name[copy] = '\0';
                *name_length = copy;
                matches = true;
            }
            mbedtls_x509_free_subject_alt_name(&san);
            if (matches) {
                return true;
            }
            continue;
        }
        mbedtls_x509_free_subject_alt_name(&san);
    }
    return false;
}

/*
 * ITEM-503 follow-up: the operator's chain is a concatenation of DER
 * certificates (the form a CA hands out, minus the PEM armour the frozen
 * profile cannot read).  Each certificate is walked by its outer SEQUENCE
 * length, so a truncated or oversized bundle is refused instead of being
 * handed to the parser as one blob.
 */
static bool appliance_certificate_import_chain_parse(
    mbedtls_x509_crt *chain,
    const unsigned char *der,
    size_t length,
    uint32_t *count)
{
    size_t offset = 0U;

    if (chain == NULL || der == NULL || count == NULL || length == 0U) {
        return false;
    }
    *count = 0U;
    while (offset < length) {
        const unsigned char *start = der + offset;
        const unsigned char *end = der + length;
        unsigned char *cursor = (unsigned char *)start;
        size_t body = 0U;
        size_t total;

        if (mbedtls_asn1_get_tag(
                &cursor, end, &body,
                MBEDTLS_ASN1_CONSTRUCTED | MBEDTLS_ASN1_SEQUENCE) != 0) {
            return false;
        }
        total = (size_t)(cursor - start) + body;
        if (total == 0U || total > length - offset ||
            *count >= APPLIANCE_CERTIFICATE_IMPORT_CHAIN_MAX) {
            return false;
        }
        if (mbedtls_x509_crt_parse_der(chain, start, total) != 0) {
            return false;
        }
        offset += total;
        ++*count;
    }
    return *count != 0U;
}

/*
 * The chain is the trust anchor list: mbedTLS builds the path from the leaf to
 * one of its members and sets a flag for every reason the path is not valid -
 * a broken signature, an issuer that does not link.  Any flag at all is a
 * refusal; a chain that verifies publishes no claim about a public CA, because
 * the profile has no root store.
 */
static bool appliance_certificate_import_chain_verify(
    mbedtls_x509_crt *certificate,
    mbedtls_x509_crt *chain)
{
    uint32_t flags = 0U;

    if (certificate == NULL || chain == NULL) {
        return false;
    }
    if (mbedtls_x509_crt_verify(
            certificate, chain, NULL, NULL, &flags, NULL, NULL) != 0) {
        return false;
    }
    return flags == 0U;
}

/*
 * ITEM-503 follow-up: mbedTLS runs its own validity checks only when the
 * profile carries a wall clock (MBEDTLS_HAVE_TIME_DATE), and the frozen HFOC
 * profile deliberately does not - the appliance validates its clock itself and
 * hands this gate a `now`.  Without this walk an expired issuer would be
 * accepted with no flag set, so every member's window is checked here against
 * the same validated instant the leaf is checked against.  The window is
 * inclusive at its end, as RFC 5280 defines it.
 */
static bool appliance_certificate_import_chain_windows(
    const mbedtls_x509_crt *chain,
    uint32_t now_epoch_seconds)
{
    const mbedtls_x509_crt *member;
    uint32_t members = 0U;

    if (chain == NULL) {
        return false;
    }
    for (member = chain; member != NULL; member = member->next) {
        uint32_t not_before = 0U;
        uint32_t not_after = 0U;

        if (!appliance_certificate_import_epoch(
                &member->valid_from, &not_before) ||
            !appliance_certificate_import_epoch(
                &member->valid_to, &not_after) ||
            not_before >= not_after ||
            now_epoch_seconds < not_before ||
            now_epoch_seconds > not_after) {
            return false;
        }
        ++members;
    }
    return members != 0U;
}

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
    struct appliance_certificate_import_result *result)
{
    mbedtls_x509_crt certificate;
    mbedtls_x509_crt chain;
    mbedtls_pk_context key;
    enum appliance_certificate_import_status status =
        APPLIANCE_CERTIFICATE_IMPORT_OK;
    uint32_t not_before = 0U;
    uint32_t not_after = 0U;
    uint32_t chain_count = 0U;
    char common_name[APPLIANCE_CERTIFICATE_IMPORT_NAME_MAX + 1U];
    char dns_name[APPLIANCE_CERTIFICATE_IMPORT_NAME_MAX + 1U];
    size_t common_name_length = 0U;
    size_t dns_name_length = 0U;
    bool saw_dns_name = false;

    if (certificate_der == NULL || key_der == NULL ||
        expected_name == NULL || result == NULL || f_rng == NULL ||
        (chain_der == NULL && chain_length != 0U)) {
        return APPLIANCE_CERTIFICATE_IMPORT_NULL;
    }
    memset(result, 0, sizeof(*result));
    if (certificate_length == 0U ||
        certificate_length > APPLIANCE_CERTIFICATE_IMPORT_DER_MAX ||
        key_length == 0U ||
        key_length > APPLIANCE_CERTIFICATE_IMPORT_KEY_DER_MAX ||
        chain_length > APPLIANCE_CERTIFICATE_IMPORT_CHAIN_DER_MAX) {
        return APPLIANCE_CERTIFICATE_IMPORT_TOO_LARGE;
    }
    if (expected_name_length == 0U ||
        expected_name_length > APPLIANCE_CERTIFICATE_IMPORT_NAME_MAX) {
        return APPLIANCE_CERTIFICATE_IMPORT_NAME_MISMATCH;
    }
    /*
     * "Not yet checked" is not "valid": a zero clock cannot prove the window,
     * exactly as the ITEM-375 lifecycle refuses it.
     */
    if (now_epoch_seconds == 0U) {
        return APPLIANCE_CERTIFICATE_IMPORT_BAD_CLOCK;
    }

    mbedtls_x509_crt_init(&certificate);
    mbedtls_x509_crt_init(&chain);
    mbedtls_pk_init(&key);

    if (mbedtls_x509_crt_parse_der(
            &certificate, certificate_der, certificate_length) != 0) {
        status = APPLIANCE_CERTIFICATE_IMPORT_BAD_CERTIFICATE;
        goto done;
    }
    if (mbedtls_pk_parse_key(
            &key, key_der, key_length, NULL, 0U, NULL, NULL) != 0) {
        status = APPLIANCE_CERTIFICATE_IMPORT_BAD_KEY;
        goto done;
    }
    /*
     * The frozen profile serves EC P-256 only; a key of any other family
     * cannot complete a handshake here and is refused by name instead of being
     * stored and failing later.
     */
    if (mbedtls_pk_get_type(&key) != MBEDTLS_PK_ECKEY ||
        mbedtls_pk_ec(key) == NULL ||
        mbedtls_pk_ec(key)->MBEDTLS_PRIVATE(grp).id !=
            MBEDTLS_ECP_DP_SECP256R1) {
        status = APPLIANCE_CERTIFICATE_IMPORT_UNSUPPORTED_KEY;
        goto done;
    }
    /*
     * mbedTLS blinds the EC multiplication behind the pair check, so the
     * platform generator is required here and a missing one is refused above.
     */
    if (mbedtls_pk_check_pair(
            &certificate.pk, &key, f_rng, rng_context) != 0) {
        status = APPLIANCE_CERTIFICATE_IMPORT_BAD_PAIR;
        goto done;
    }
    if (!appliance_certificate_import_epoch(
            &certificate.valid_from, &not_before) ||
        !appliance_certificate_import_epoch(
            &certificate.valid_to, &not_after)) {
        status = APPLIANCE_CERTIFICATE_IMPORT_BAD_CERTIFICATE;
        goto done;
    }
    if (now_epoch_seconds < not_before) {
        status = APPLIANCE_CERTIFICATE_IMPORT_NOT_YET_VALID;
        goto done;
    }
    if (now_epoch_seconds > not_after) {
        status = APPLIANCE_CERTIFICATE_IMPORT_EXPIRED;
        goto done;
    }
    if (!appliance_certificate_import_dns_name(
            &certificate, expected_name, expected_name_length,
            dns_name, sizeof(dns_name), &dns_name_length,
            &saw_dns_name)) {
        status = saw_dns_name
                     ? APPLIANCE_CERTIFICATE_IMPORT_NAME_MISMATCH
                     : APPLIANCE_CERTIFICATE_IMPORT_NO_SUBJECT_ALT_NAME;
        goto done;
    }
    /*
     * The lifecycle compares the common name and the SAN to the request it
     * was issued, so a certificate whose CN is a different name would be
     * accepted here and refused there; the mismatch is named here instead.
     */
    if (appliance_certificate_import_common_name(
            &certificate, common_name, sizeof(common_name),
            &common_name_length) &&
        !appliance_certificate_import_name_equal(
            common_name, common_name_length, expected_name,
            expected_name_length)) {
        status = APPLIANCE_CERTIFICATE_IMPORT_NAME_MISMATCH;
        goto done;
    }
    /*
     * ITEM-503 follow-up: a chain, when the operator supplies one, must really
     * verify the leaf before anything is stored.  An empty chain stays legal:
     * the operator's self-signed certificate has none.
     */
    if (chain_length != 0U) {
        if (!appliance_certificate_import_chain_parse(
                &chain, chain_der, chain_length, &chain_count)) {
            status = APPLIANCE_CERTIFICATE_IMPORT_BAD_CHAIN;
            goto done;
        }
        if (!appliance_certificate_import_chain_verify(
                &certificate, &chain) ||
            !appliance_certificate_import_chain_windows(
                &chain, now_epoch_seconds)) {
            status = APPLIANCE_CERTIFICATE_IMPORT_CHAIN_MISMATCH;
            goto done;
        }
    }

    result->version = APPLIANCE_CERTIFICATE_IMPORT_VERSION;
    memcpy(result->name, dns_name, dns_name_length + 1U);
    result->name_length = (uint32_t)dns_name_length;
    result->not_before_epoch = not_before;
    result->not_after_epoch = not_after;
    result->chain_count = chain_count;
    mbedtls_sha256(certificate_der, certificate_length,
                   result->fingerprint, 0);

done:
    mbedtls_x509_crt_free(&chain);
    mbedtls_pk_free(&key);
    mbedtls_x509_crt_free(&certificate);
    return status;
}

const char *appliance_certificate_import_status_string(
    enum appliance_certificate_import_status status)
{
    switch (status) {
    case APPLIANCE_CERTIFICATE_IMPORT_OK:
        return "APPLIANCE_CERTIFICATE_IMPORT_OK";
    case APPLIANCE_CERTIFICATE_IMPORT_NULL:
        return "APPLIANCE_CERTIFICATE_IMPORT_NULL";
    case APPLIANCE_CERTIFICATE_IMPORT_TOO_LARGE:
        return "APPLIANCE_CERTIFICATE_IMPORT_TOO_LARGE";
    case APPLIANCE_CERTIFICATE_IMPORT_BAD_CERTIFICATE:
        return "APPLIANCE_CERTIFICATE_IMPORT_BAD_CERTIFICATE";
    case APPLIANCE_CERTIFICATE_IMPORT_BAD_KEY:
        return "APPLIANCE_CERTIFICATE_IMPORT_BAD_KEY";
    case APPLIANCE_CERTIFICATE_IMPORT_UNSUPPORTED_KEY:
        return "APPLIANCE_CERTIFICATE_IMPORT_UNSUPPORTED_KEY";
    case APPLIANCE_CERTIFICATE_IMPORT_BAD_PAIR:
        return "APPLIANCE_CERTIFICATE_IMPORT_BAD_PAIR";
    case APPLIANCE_CERTIFICATE_IMPORT_BAD_CLOCK:
        return "APPLIANCE_CERTIFICATE_IMPORT_BAD_CLOCK";
    case APPLIANCE_CERTIFICATE_IMPORT_NOT_YET_VALID:
        return "APPLIANCE_CERTIFICATE_IMPORT_NOT_YET_VALID";
    case APPLIANCE_CERTIFICATE_IMPORT_EXPIRED:
        return "APPLIANCE_CERTIFICATE_IMPORT_EXPIRED";
    case APPLIANCE_CERTIFICATE_IMPORT_NO_SUBJECT_ALT_NAME:
        return "APPLIANCE_CERTIFICATE_IMPORT_NO_SUBJECT_ALT_NAME";
    case APPLIANCE_CERTIFICATE_IMPORT_NAME_MISMATCH:
        return "APPLIANCE_CERTIFICATE_IMPORT_NAME_MISMATCH";
    case APPLIANCE_CERTIFICATE_IMPORT_BAD_CHAIN:
        return "APPLIANCE_CERTIFICATE_IMPORT_BAD_CHAIN";
    case APPLIANCE_CERTIFICATE_IMPORT_CHAIN_MISMATCH:
    default:
        return "APPLIANCE_CERTIFICATE_IMPORT_CHAIN_MISMATCH";
    }
}

#endif
