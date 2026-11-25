#include "appliance_acme_trust.h"

/*
 * The module is compiled into the production kernel (HFOC_PRODUCTION_TLS) and
 * into its host proof.  A fixture kernel has neither the TLS profile nor the
 * mbedTLS headers, so it compiles the unit empty - the same boundary
 * appliance_certificate_import.c and appliance_production_serving.c carry.
 */
#if defined(HFOC_PRODUCTION_TLS) || \
    defined(HFOC_APPLIANCE_ACME_TRUST_HOST_TEST)

#include "mbedtls/x509.h"
#include "mbedtls/x509_crt.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/*
 * ITEM-508 increment 2: the ACME client role's single trust decision.
 *
 * The presented chain is parsed from the DER a TLS peer sends - leaf first,
 * then the intermediates, concatenated exactly as the certificate_list of a
 * Certificate message carries them - and every certificate is bounded before
 * it is parsed.  The named anchor is parsed once and cached, so a handshake
 * loop does not re-parse the root it already trusts.
 *
 * The caches are static for the same reason the name store's record buffer is:
 * an mbedTLS certificate context is several kilobytes, and this module runs on
 * the request path where the kernel stack is a measured resource.  There is
 * one caller - the ACME module - so one cache is one trust decision.
 */

#define APPLIANCE_ACME_TRUST_VERSION_CURRENT 1U
/* "CN=" prefix, one exact name, terminator: the verify API wants a C string. */
#define APPLIANCE_ACME_TRUST_NAME_STORAGE \
    (APPLIANCE_ACME_TRUST_NAME_MAX + 1U)

static mbedtls_x509_crt appliance_acme_trust_anchor_cache;
static mbedtls_x509_crt appliance_acme_trust_chain_cache;
static const unsigned char *appliance_acme_trust_anchor_source;
static size_t appliance_acme_trust_anchor_source_length;
static bool appliance_acme_trust_anchor_ready;
static char appliance_acme_trust_name[APPLIANCE_ACME_TRUST_NAME_STORAGE];

static enum appliance_acme_trust_status appliance_acme_trust_refuse(
    struct appliance_acme_trust *trust,
    enum appliance_acme_trust_status status)
{
    if (trust != NULL) {
        trust->refusal_count += 1U;
        trust->last_status = status;
        /*
         * A refusal never leaves an expiry behind: the caller feeds the
         * renewal policy only from a chain this call accepted.
         */
        trust->leaf_not_after_epoch_seconds = 0U;
    }
    return status;
}

/*
 * The total length of one DER certificate: the outer SEQUENCE header plus its
 * contents.  The list is self-delimiting, so the boundary of each member is
 * read here instead of trusting a count from the peer.
 */
static bool appliance_acme_trust_certificate_length(
    const unsigned char *bytes,
    size_t available,
    size_t *length_out)
{
    size_t header = 2U;
    size_t content = 0U;
    size_t index;
    unsigned char first;

    if (bytes == NULL || length_out == NULL || available < 4U ||
        bytes[0] != 0x30U) {
        return false;
    }
    first = bytes[1];
    if ((first & 0x80U) == 0U) {
        content = first;
    } else {
        const size_t octets = (size_t)(first & 0x7FU);

        if (octets == 0U || octets > 3U || available < 2U + octets) {
            return false;
        }
        for (index = 0U; index < octets; ++index) {
            content = (content << 8U) | bytes[2U + index];
        }
        header = 2U + octets;
    }
    if (content == 0U || content > available - header) {
        return false;
    }
    *length_out = header + content;
    return true;
}

/*
 * One exact DNS name: lower-case letters, digits and hyphens in labels of 1 to
 * 63 characters, separated by single dots, at most 253 characters in total, no
 * wildcard, no trailing dot and no empty label.  A name that is not exact can
 * never be verified, and refusing it as a name problem keeps it apart from a
 * chain problem in the record.
 */
static bool appliance_acme_trust_name_valid(
    const char *hostname,
    size_t hostname_length)
{
    size_t index;
    size_t label = 0U;
    bool hyphen = false;

    if (hostname == NULL || hostname_length == 0U ||
        hostname_length > APPLIANCE_ACME_TRUST_NAME_MAX) {
        return false;
    }
    for (index = 0U; index < hostname_length; ++index) {
        const unsigned char value = (unsigned char)hostname[index];

        if (value == (unsigned char)'.') {
            if (label == 0U || hyphen) {
                return false;
            }
            label = 0U;
            continue;
        }
        if (!((value >= (unsigned char)'a' && value <= (unsigned char)'z') ||
              (value >= (unsigned char)'0' && value <= (unsigned char)'9') ||
              value == (unsigned char)'-')) {
            return false;
        }
        if (value == (unsigned char)'-' && label == 0U) {
            return false;
        }
        hyphen = value == (unsigned char)'-';
        ++label;
        if (label > 63U) {
            return false;
        }
    }
    return label != 0U && !hyphen;
}

static bool appliance_acme_trust_epoch(
    const mbedtls_x509_time *time,
    uint32_t *epoch_out)
{
    static const uint8_t month_lengths[12U] = {
        31U, 28U, 31U, 30U, 31U, 30U, 31U, 31U, 30U, 31U, 30U, 31U
    };
    uint32_t days = 0U;
    uint32_t year;
    uint32_t month;
    bool leap;

    if (time == NULL || epoch_out == NULL || time->year < 1970 ||
        time->year > 9999 || time->mon < 1 || time->mon > 12 ||
        time->day < 1 || time->day > 31 || time->hour > 23 ||
        time->min > 59 || time->sec > 60) {
        return false;
    }
    for (year = 1970U; year < (uint32_t)time->year; ++year) {
        leap = (year % 4U == 0U && year % 100U != 0U) || year % 400U == 0U;
        days += leap ? 366U : 365U;
    }
    leap = ((uint32_t)time->year % 4U == 0U &&
            (uint32_t)time->year % 100U != 0U) ||
           (uint32_t)time->year % 400U == 0U;
    for (month = 1U; month < (uint32_t)time->mon; ++month) {
        days += month_lengths[month - 1U];
        if (month == 2U && leap) {
            days += 1U;
        }
    }
    days += (uint32_t)time->day - 1U;
    *epoch_out = days * 86400U + (uint32_t)time->hour * 3600U +
                 (uint32_t)time->min * 60U + (uint32_t)time->sec;
    return true;
}

static bool appliance_acme_trust_windows(
    const mbedtls_x509_crt *chain,
    uint32_t now_epoch_seconds,
    uint32_t *leaf_not_after_out)
{
    const mbedtls_x509_crt *member;
    uint32_t members = 0U;

    if (chain == NULL) {
        return false;
    }
    if (leaf_not_after_out != NULL) {
        *leaf_not_after_out = 0U;
    }
    for (member = chain; member != NULL; member = member->next) {
        uint32_t not_before = 0U;
        uint32_t not_after = 0U;

        if (!appliance_acme_trust_epoch(&member->valid_from, &not_before) ||
            !appliance_acme_trust_epoch(&member->valid_to, &not_after) ||
            not_before >= not_after ||
            now_epoch_seconds < not_before ||
            now_epoch_seconds > not_after) {
            if (leaf_not_after_out != NULL) {
                *leaf_not_after_out = 0U;
            }
            return false;
        }
        /*
         * ITEM-508 increment 35: the first member is the leaf, and its own
         * `notAfter` is the one expiry the caller may feed the renewal policy.
         * It is recorded only after the whole chain passed the walk, so a
         * chain refused for an issuer's window leaves the leaf expiry at zero.
         */
        if (members == 0U && leaf_not_after_out != NULL) {
            *leaf_not_after_out = not_after;
        }
        ++members;
    }
    if (members == 0U && leaf_not_after_out != NULL) {
        *leaf_not_after_out = 0U;
    }
    return members != 0U;
}

static bool appliance_acme_trust_anchor_parse(
    struct appliance_acme_trust *trust)
{
    if (appliance_acme_trust_anchor_ready &&
        appliance_acme_trust_anchor_source == trust->anchor_der &&
        appliance_acme_trust_anchor_source_length == trust->anchor_length) {
        return true;
    }
    appliance_acme_trust_anchor_ready = false;
    if (appliance_acme_trust_anchor_source != NULL) {
        mbedtls_x509_crt_free(&appliance_acme_trust_anchor_cache);
    }
    mbedtls_x509_crt_init(&appliance_acme_trust_anchor_cache);
    appliance_acme_trust_anchor_source = trust->anchor_der;
    appliance_acme_trust_anchor_source_length = trust->anchor_length;
    if (mbedtls_x509_crt_parse_der(
            &appliance_acme_trust_anchor_cache, trust->anchor_der,
            trust->anchor_length) != 0) {
        return false;
    }
    appliance_acme_trust_anchor_ready = true;
    return true;
}

enum appliance_acme_trust_status appliance_acme_trust_init(
    struct appliance_acme_trust *trust,
    const unsigned char *anchor_der,
    size_t anchor_length)
{
    if (trust == NULL) {
        return APPLIANCE_ACME_TRUST_NULL;
    }
    memset(trust, 0, sizeof(*trust));
    trust->version = APPLIANCE_ACME_TRUST_VERSION_CURRENT;
    trust->initialized = true;
    trust->anchor_der = anchor_der;
    trust->anchor_length = anchor_length;
    trust->last_status = APPLIANCE_ACME_TRUST_OK;
    return APPLIANCE_ACME_TRUST_OK;
}

enum appliance_acme_trust_status appliance_acme_trust_verify(
    struct appliance_acme_trust *trust,
    const unsigned char *chain_der,
    size_t chain_length,
    const char *hostname,
    size_t hostname_length,
    uint32_t now_epoch_seconds)
{
    const unsigned char *cursor = chain_der;
    size_t remaining = chain_length;
    uint32_t members = 0U;
    uint32_t flags = 0U;
    int result;

    if (trust == NULL || chain_der == NULL || hostname == NULL) {
        return APPLIANCE_ACME_TRUST_NULL;
    }
    if (!trust->initialized ||
        trust->version != APPLIANCE_ACME_TRUST_VERSION_CURRENT) {
        return APPLIANCE_ACME_TRUST_BAD_STATE;
    }
    if (trust->anchor_der == NULL || trust->anchor_length == 0U) {
        return appliance_acme_trust_refuse(
            trust, APPLIANCE_ACME_TRUST_ANCHOR_UNAVAILABLE);
    }
    /*
     * The item's point 6: an unvalidated clock starts nothing.  No order, no
     * chain, no name - the refusal comes first.
     */
    if (now_epoch_seconds == 0U) {
        return appliance_acme_trust_refuse(
            trust, APPLIANCE_ACME_TRUST_BAD_CLOCK);
    }
    if (!appliance_acme_trust_name_valid(hostname, hostname_length)) {
        return appliance_acme_trust_refuse(
            trust, APPLIANCE_ACME_TRUST_BAD_NAME);
    }
    if (chain_length == 0U || chain_length > APPLIANCE_ACME_TRUST_CHAIN_MAX) {
        return appliance_acme_trust_refuse(
            trust, APPLIANCE_ACME_TRUST_CHAIN_PARSE_FAILED);
    }
    if (!appliance_acme_trust_anchor_parse(trust)) {
        return appliance_acme_trust_refuse(
            trust, APPLIANCE_ACME_TRUST_ANCHOR_UNAVAILABLE);
    }

    memset(appliance_acme_trust_name, 0, sizeof(appliance_acme_trust_name));
    memcpy(appliance_acme_trust_name, hostname, hostname_length);

    mbedtls_x509_crt_free(&appliance_acme_trust_chain_cache);
    mbedtls_x509_crt_init(&appliance_acme_trust_chain_cache);
    while (remaining != 0U) {
        size_t certificate_length = 0U;

        if (members == APPLIANCE_ACME_TRUST_CHAIN_MEMBERS_MAX ||
            !appliance_acme_trust_certificate_length(
                cursor, remaining, &certificate_length) ||
            mbedtls_x509_crt_parse_der(
                &appliance_acme_trust_chain_cache, cursor,
                certificate_length) != 0) {
            mbedtls_x509_crt_free(&appliance_acme_trust_chain_cache);
            mbedtls_x509_crt_init(&appliance_acme_trust_chain_cache);
            return appliance_acme_trust_refuse(
                trust, APPLIANCE_ACME_TRUST_CHAIN_PARSE_FAILED);
        }
        cursor += certificate_length;
        remaining -= certificate_length;
        ++members;
    }
    if (!appliance_acme_trust_windows(
            &appliance_acme_trust_chain_cache, now_epoch_seconds,
            &trust->leaf_not_after_epoch_seconds)) {
        trust->leaf_not_after_epoch_seconds = 0U;
        mbedtls_x509_crt_free(&appliance_acme_trust_chain_cache);
        mbedtls_x509_crt_init(&appliance_acme_trust_chain_cache);
        return appliance_acme_trust_refuse(
            trust, APPLIANCE_ACME_TRUST_CHAIN_WINDOW_REJECTED);
    }
    result = mbedtls_x509_crt_verify(
        &appliance_acme_trust_chain_cache, &appliance_acme_trust_anchor_cache,
        NULL, appliance_acme_trust_name, &flags, NULL, NULL);
    trust->last_flags = flags;
    mbedtls_x509_crt_free(&appliance_acme_trust_chain_cache);
    mbedtls_x509_crt_init(&appliance_acme_trust_chain_cache);
    if (result != 0 || flags != 0U) {
        if ((flags & MBEDTLS_X509_BADCERT_CN_MISMATCH) != 0U) {
            return appliance_acme_trust_refuse(
                trust, APPLIANCE_ACME_TRUST_NAME_REJECTED);
        }
        return appliance_acme_trust_refuse(
            trust, APPLIANCE_ACME_TRUST_CHAIN_REJECTED);
    }
    trust->verify_count += 1U;
    trust->last_status = APPLIANCE_ACME_TRUST_OK;
    return APPLIANCE_ACME_TRUST_OK;
}


uint32_t appliance_acme_trust_leaf_not_after(
    const struct appliance_acme_trust *trust)
{
    return trust != NULL && trust->initialized
               ? trust->leaf_not_after_epoch_seconds
               : 0U;
}

const char *appliance_acme_trust_status_string(
    enum appliance_acme_trust_status status)
{
    switch (status) {
    case APPLIANCE_ACME_TRUST_OK:
        return "OK";
    case APPLIANCE_ACME_TRUST_NULL:
        return "NULL";
    case APPLIANCE_ACME_TRUST_BAD_STATE:
        return "BAD_STATE";
    case APPLIANCE_ACME_TRUST_BAD_CLOCK:
        return "BAD_CLOCK";
    case APPLIANCE_ACME_TRUST_BAD_NAME:
        return "BAD_NAME";
    case APPLIANCE_ACME_TRUST_ANCHOR_UNAVAILABLE:
        return "ANCHOR_UNAVAILABLE";
    case APPLIANCE_ACME_TRUST_CHAIN_PARSE_FAILED:
        return "CHAIN_PARSE_FAILED";
    case APPLIANCE_ACME_TRUST_CHAIN_WINDOW_REJECTED:
        return "CHAIN_WINDOW_REJECTED";
    case APPLIANCE_ACME_TRUST_CHAIN_REJECTED:
        return "CHAIN_REJECTED";
    case APPLIANCE_ACME_TRUST_NAME_REJECTED:
        return "NAME_REJECTED";
    default:
        break;
    }
    return "UNKNOWN";
}

#endif /* HFOC_PRODUCTION_TLS || HFOC_APPLIANCE_ACME_TRUST_HOST_TEST */
