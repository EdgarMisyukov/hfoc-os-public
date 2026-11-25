#include "appliance_production_certificate.h"

#ifdef HFOC_PRODUCTION_TLS

#include "appliance_identity.h"
#include "appliance_production_entropy.h"
#include "appliance_persistent_config.h"
#include "appliance_state_layout.h"

#include "mbedtls/ecp.h"
#include "mbedtls/oid.h"
#include "mbedtls/pk.h"
#include "mbedtls/x509_crt.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define APPLIANCE_PRODUCTION_CERTIFICATE_STORE_ID \
    APPLIANCE_STATE_STORE_ID_CERTIFICATE
#define APPLIANCE_PRODUCTION_CERTIFICATE_SUBJECT_OID "O=HFOC"

struct appliance_production_certificate_record_header {
    uint8_t magic[4];
    uint8_t version[4];
    uint8_t certificate_length[4];
    uint8_t key_length[4];
};

/*
 * ITEM-503 record v2: the same magic and version fields, then the leaf, the
 * optional chain and the key, each with its own length.  A v1 record (no
 * chain) is still read; every write is v2.
 */
struct appliance_production_certificate_record_header_2 {
    uint8_t magic[4];
    uint8_t version[4];
    uint8_t source[4];
    uint8_t certificate_length[4];
    uint8_t chain_length[4];
    uint8_t key_length[4];
};

/* ------------------------------------------------------------- small utils */

static void appliance_production_certificate_zero(
    void *buffer,
    size_t length)
{
    volatile uint8_t *bytes = (volatile uint8_t *)buffer;
    size_t index;

    for (index = 0U; index < length; ++index) {
        bytes[index] = 0U;
    }
}

static void appliance_production_certificate_store_u32(
    uint8_t *destination,
    uint32_t value)
{
    destination[0] = (uint8_t)(value >> 24U);
    destination[1] = (uint8_t)(value >> 16U);
    destination[2] = (uint8_t)(value >> 8U);
    destination[3] = (uint8_t)value;
}

static uint32_t appliance_production_certificate_load_u32(
    const uint8_t *source)
{
    return ((uint32_t)source[0] << 24U) |
           ((uint32_t)source[1] << 16U) |
           ((uint32_t)source[2] << 8U) |
           (uint32_t)source[3];
}

/*
 * Bounded civil-time conversion: the X.509 writer wants "YYYYMMDDhhmmss" and
 * the freestanding kernel links no libc time formatting.  The algorithm is the
 * standard days-to-civil conversion and needs no lookup table.
 */
static void appliance_production_certificate_format_time(
    uint32_t epoch_seconds,
    char output[APPLIANCE_PRODUCTION_CERTIFICATE_VALIDITY_LENGTH])
{
    int64_t days = (int64_t)(epoch_seconds / 86400U) + INT64_C(719468);
    int64_t era = (days >= 0 ? days : days - INT64_C(146096)) /
                  INT64_C(146097);
    uint32_t day_of_era =
        (uint32_t)(days - era * INT64_C(146097));
    uint32_t year_of_era =
        (day_of_era - day_of_era / 1460U + day_of_era / 36524U -
         day_of_era / 146096U) /
        365U;
    int64_t year = (int64_t)year_of_era + era * INT64_C(400);
    uint32_t day_of_year =
        day_of_era -
        (365U * year_of_era + year_of_era / 4U - year_of_era / 100U);
    uint32_t month_prime = (5U * day_of_year + 2U) / 153U;
    uint32_t day =
        day_of_year - (153U * month_prime + 2U) / 5U + 1U;
    uint32_t month =
        month_prime < 10U ? month_prime + 3U : month_prime - 9U;
    uint32_t hour = (epoch_seconds / 3600U) % 24U;
    uint32_t minute = (epoch_seconds / 60U) % 60U;
    uint32_t second = epoch_seconds % 60U;

    if (month <= 2U) {
        year += 1;
    }
    output[0] = (char)('0' + (year / 1000) % 10);
    output[1] = (char)('0' + (year / 100) % 10);
    output[2] = (char)('0' + (year / 10) % 10);
    output[3] = (char)('0' + year % 10);
    output[4] = (char)('0' + (month / 10) % 10);
    output[5] = (char)('0' + month % 10);
    output[6] = (char)('0' + (day / 10) % 10);
    output[7] = (char)('0' + day % 10);
    output[8] = (char)('0' + (hour / 10) % 10);
    output[9] = (char)('0' + hour % 10);
    output[10] = (char)('0' + (minute / 10) % 10);
    output[11] = (char)('0' + minute % 10);
    output[12] = (char)('0' + (second / 10) % 10);
    output[13] = (char)('0' + second % 10);
    output[14] = '\0';
}

/*
 * ITEM-404: the inverse conversion plus the X.509 metadata the management TLS
 * lifecycle needs.  mbedTLS parses its own DER back to the validity window;
 * the fingerprint is SHA-256 over the DER that is actually served, so the
 * lifecycle compares certificates by the bytes on the wire and not by a
 * caller-supplied identifier.
 */
static bool appliance_production_certificate_days_from_civil(
    int64_t year,
    uint32_t month,
    uint32_t day,
    int64_t *days_out)
{
    int64_t adjusted_year = year;
    uint32_t adjusted_month = month;
    int64_t era;
    uint32_t year_of_era;
    uint32_t day_of_year;
    uint32_t day_of_era;

    if (days_out == NULL || month == 0U || month > 12U ||
        day == 0U || day > 31U) {
        return false;
    }
    if (adjusted_month <= 2U) {
        adjusted_year -= 1;
    } else {
        adjusted_month -= 3U;
    }
    era = (adjusted_year >= 0 ? adjusted_year
                              : adjusted_year - INT64_C(399)) /
          INT64_C(400);
    year_of_era =
        (uint32_t)(adjusted_year - era * INT64_C(400));
    day_of_year =
        (153U * adjusted_month + 2U) / 5U + day - 1U;
    day_of_era = year_of_era * 365U + year_of_era / 4U -
                 year_of_era / 100U + day_of_year;
    *days_out = era * INT64_C(146097) + (int64_t)day_of_era -
                INT64_C(719468);
    return true;
}

static bool appliance_production_certificate_time_epoch(
    const mbedtls_x509_time *time,
    uint32_t *epoch_out)
{
    int64_t days = 0;
    int64_t seconds;

    if (time == NULL || epoch_out == NULL ||
        !appliance_production_certificate_days_from_civil(
            (int64_t)time->year, (uint32_t)time->mon,
            (uint32_t)time->day, &days)) {
        return false;
    }
    if (time->hour < 0 || time->hour > 23 ||
        time->min < 0 || time->min > 59 ||
        time->sec < 0 || time->sec > 60) {
        return false;
    }
    seconds = days * INT64_C(86400) + (int64_t)time->hour * 3600 +
              (int64_t)time->min * 60 + (int64_t)time->sec;
    if (seconds <= 0 || seconds > (int64_t)UINT32_MAX) {
        return false;
    }
    *epoch_out = (uint32_t)seconds;
    return true;
}

static bool appliance_production_certificate_metadata_from_der(
    struct appliance_production_certificate *certificate)
{
    mbedtls_x509_crt parsed;
    bool ready = false;

    if (certificate == NULL || certificate->certificate_length == 0U ||
        certificate->certificate_length >
            APPLIANCE_PRODUCTION_CERTIFICATE_DER_MAX) {
        return false;
    }
    certificate->metadata_ready = false;
    certificate->not_before_epoch = 0U;
    certificate->not_after_epoch = 0U;
    certificate->issuer[0] = '\0';
    certificate->issuer_length = 0U;
    memset(certificate->fingerprint, 0, sizeof(certificate->fingerprint));
    if (sha256_compute(
            certificate->certificate_der,
            certificate->certificate_length,
            certificate->fingerprint) != SHA256_OK) {
        return false;
    }
    mbedtls_x509_crt_init(&parsed);
    if (mbedtls_x509_crt_parse_der(
            &parsed,
            certificate->certificate_der,
            certificate->certificate_length) == 0 &&
        appliance_production_certificate_time_epoch(
            &parsed.valid_from, &certificate->not_before_epoch) &&
        appliance_production_certificate_time_epoch(
            &parsed.valid_to, &certificate->not_after_epoch) &&
        certificate->not_after_epoch >
            certificate->not_before_epoch) {
        /*
         * ITEM-503: the issuer name is read from the DER so an imported
         * operator certificate is never described as the appliance's own
         * self-signed one.  A name that does not fit is truncated by mbedTLS
         * exactly as every other rendered name is; an unreadable name stays
         * empty and the caller falls back to a named default.
         */
        const int issuer_length = mbedtls_x509_dn_gets(
            certificate->issuer,
            sizeof(certificate->issuer),
            &parsed.issuer);

        if (issuer_length > 0) {
            certificate->issuer_length = (uint32_t)issuer_length;
        }
        ready = true;
    }
    mbedtls_x509_crt_free(&parsed);
    if (!ready) {
        certificate->not_before_epoch = 0U;
        certificate->not_after_epoch = 0U;
        certificate->issuer[0] = '\0';
        certificate->issuer_length = 0U;
        memset(certificate->fingerprint, 0,
               sizeof(certificate->fingerprint));
        return false;
    }
    certificate->metadata_ready = true;
    return true;
}

/* ------------------------------------------------------------- subject name */

static bool appliance_production_certificate_identity(
    struct appliance_production_certificate *certificate,
    struct appliance_state_loader *loader)
{
    const char *hostname = APPLIANCE_IDENTITY_DEFAULT_HOSTNAME;
    size_t hostname_length = APPLIANCE_IDENTITY_DEFAULT_HOSTNAME_LENGTH;

    if (loader != NULL && loader->ready) {
        char stored[APPLIANCE_PERSISTENT_CONFIG_HOSTNAME_MAX + 1U];
        size_t stored_length = 0U;

        memset(stored, 0, sizeof(stored));
        if (appliance_persistent_config_hostname(
                &loader->system_config,
                stored,
                sizeof(stored),
                &stored_length) == APPLIANCE_PERSISTENT_CONFIG_OK &&
            stored_length != 0U &&
            stored_length <=
                APPLIANCE_PRODUCTION_CERTIFICATE_COMMON_NAME_MAX) {
            hostname = stored;
            hostname_length = stored_length;
        }
    }
    if (hostname_length == 0U ||
        hostname_length >
            APPLIANCE_PRODUCTION_CERTIFICATE_COMMON_NAME_MAX) {
        return false;
    }
    memcpy(certificate->common_name, hostname, hostname_length);
    certificate->common_name[hostname_length] = '\0';
    certificate->common_name_length = (uint32_t)hostname_length;
    return true;
}

/* --------------------------------------------------------------- the store */

static bool appliance_production_certificate_bind_store(
    struct appliance_production_certificate *certificate,
    struct appliance_state_loader *loader)
{
    const struct appliance_state_layout_entry *entry = NULL;

    if (loader == NULL || !loader->ready) {
        return false;
    }
    /*
     * A sealed state volume is never written in the clear.  The durable
     * update record takes the same decision, so the appliance degrades to a
     * per-boot certificate instead of leaking a private key onto a volume it
     * claims to protect.
     */
#if APPLIANCE_STATE_ENCRYPTION_ENABLED
    if (loader->encryption_sealed) {
        certificate->last_status =
            APPLIANCE_PRODUCTION_CERTIFICATE_STORE_SEALED;
        return false;
    }
#endif
    if (appliance_state_layout_find(
            &loader->layout,
            APPLIANCE_PERSISTENT_STORAGE_NAMESPACE_ID_CERTIFICATE,
            APPLIANCE_PRODUCTION_CERTIFICATE_STORE_ID,
            &entry) != APPLIANCE_STATE_LAYOUT_OK ||
        entry == NULL) {
        return false;
    }
    if (entry->block_count <
        APPLIANCE_PRODUCTION_CERTIFICATE_RECORD_BLOCKS) {
        return false;
    }
    if (appliance_state_block_device_bind(
            &loader->partition.device,
            entry,
            &certificate->store) != APPLIANCE_STATE_BLOCK_DEVICE_OK) {
        return false;
    }
    certificate->store_bound = true;
    return true;
}

static bool appliance_production_certificate_load_record(
    struct appliance_production_certificate *certificate)
{
    /*
     * ITEM-503: the record grew to twelve blocks, so the buffer is file-scope
     * rather than a 6 KiB kernel stack frame.  The management path is
     * single-threaded and the loader is re-entrant only through itself.
     */
    static uint8_t record[APPLIANCE_PRODUCTION_CERTIFICATE_RECORD_BYTES];
    struct appliance_production_certificate_record_header header;
    struct appliance_production_certificate_record_header_2 header_2;
    uint32_t version;
    uint32_t certificate_length;
    uint32_t chain_length = 0U;
    uint32_t key_length;
    size_t header_bytes;
    size_t key_offset;

    memset(record, 0, sizeof(record));
    memset(&header, 0, sizeof(header));
    memset(&header_2, 0, sizeof(header_2));
    if (appliance_state_block_device_read(
            &certificate->store, 0U, record, sizeof(record)) !=
        BLOCK_DEVICE_OK) {
        certificate->last_status =
            APPLIANCE_PRODUCTION_CERTIFICATE_STORE_READ_FAILED;
        return false;
    }
    memcpy(&header, record, sizeof(header));
    if (appliance_production_certificate_load_u32(header.magic) !=
        APPLIANCE_PRODUCTION_CERTIFICATE_MAGIC) {
        return false;
    }
    version = appliance_production_certificate_load_u32(header.version);
    if (version == APPLIANCE_PRODUCTION_CERTIFICATE_RECORD_VERSION_2) {
        uint32_t source;

        memcpy(&header_2, record, sizeof(header_2));
        source = appliance_production_certificate_load_u32(header_2.source);
        header_bytes = sizeof(header_2);
        /*
         * ITEM-508 increment 23: provenance is one of the three declared
         * values.  An unknown source is a corrupt record, never a silent
         * "generated": the serving record would otherwise describe an
         * unreadable record as the appliance's own self-signed identity.
         */
        if (source != APPLIANCE_PRODUCTION_CERTIFICATE_SOURCE_GENERATED &&
            source != APPLIANCE_PRODUCTION_CERTIFICATE_SOURCE_OPERATOR &&
            source != APPLIANCE_PRODUCTION_CERTIFICATE_SOURCE_ACME) {
            certificate->last_status =
                APPLIANCE_PRODUCTION_CERTIFICATE_RECORD_CORRUPT;
            return false;
        }
        certificate->imported =
            source == APPLIANCE_PRODUCTION_CERTIFICATE_SOURCE_OPERATOR;
        certificate->acme_issued =
            source == APPLIANCE_PRODUCTION_CERTIFICATE_SOURCE_ACME;
        certificate_length =
            appliance_production_certificate_load_u32(
                header_2.certificate_length);
        chain_length =
            appliance_production_certificate_load_u32(
                header_2.chain_length);
        key_length =
            appliance_production_certificate_load_u32(header_2.key_length);
    } else if (version == APPLIANCE_PRODUCTION_CERTIFICATE_RECORD_VERSION) {
        header_bytes = sizeof(header);
        certificate->imported = false;
        certificate->acme_issued = false;
        certificate_length =
            appliance_production_certificate_load_u32(
                header.certificate_length);
        key_length =
            appliance_production_certificate_load_u32(header.key_length);
    } else {
        return false;
    }
    if (certificate_length == 0U ||
        certificate_length >
            APPLIANCE_PRODUCTION_CERTIFICATE_DER_MAX ||
        chain_length > APPLIANCE_PRODUCTION_CERTIFICATE_CHAIN_DER_MAX ||
        key_length == 0U ||
        key_length > APPLIANCE_PRODUCTION_CERTIFICATE_KEY_DER_MAX ||
        header_bytes + (size_t)certificate_length +
                (size_t)chain_length + (size_t)key_length >
            sizeof(record)) {
        certificate->last_status =
            APPLIANCE_PRODUCTION_CERTIFICATE_RECORD_CORRUPT;
        return false;
    }
    memcpy(
        certificate->certificate_der,
        record + header_bytes,
        certificate_length);
    if (chain_length != 0U) {
        memcpy(
            certificate->chain_der,
            record + header_bytes + certificate_length,
            chain_length);
    }
    key_offset = header_bytes + (size_t)certificate_length +
                 (size_t)chain_length;
    memcpy(certificate->key_der, record + key_offset, key_length);
    certificate->certificate_length = certificate_length;
    certificate->chain_length = chain_length;
    certificate->key_length = key_length;
    /*
     * ITEM-404: a record whose DER cannot be parsed back into the metadata the
     * management TLS lifecycle validates is corrupt, not servable.  Treating
     * it as absent regenerates one certificate instead of serving a
     * certificate the lifecycle must reject.
     */
    if (!appliance_production_certificate_metadata_from_der(certificate)) {
        certificate->certificate_length = 0U;
        certificate->chain_length = 0U;
        certificate->key_length = 0U;
        certificate->last_status =
            APPLIANCE_PRODUCTION_CERTIFICATE_RECORD_CORRUPT;
        appliance_production_certificate_zero(record, sizeof(record));
        return false;
    }
    certificate->loaded_from_store = true;
    certificate->persisted = true;
    ++certificate->load_count;
    appliance_production_certificate_zero(record, sizeof(record));
    return true;
}

static bool appliance_production_certificate_write_record(
    struct appliance_production_certificate *certificate)
{
    static uint8_t record[APPLIANCE_PRODUCTION_CERTIFICATE_RECORD_BYTES];
    struct appliance_production_certificate_record_header_2 header;

    if (sizeof(header) + (size_t)certificate->certificate_length +
            (size_t)certificate->chain_length +
            (size_t)certificate->key_length >
        sizeof(record)) {
        certificate->last_status =
            APPLIANCE_PRODUCTION_CERTIFICATE_RECORD_TOO_LARGE;
        return false;
    }
    memset(record, 0, sizeof(record));
    memset(&header, 0, sizeof(header));
    appliance_production_certificate_store_u32(
        header.magic, APPLIANCE_PRODUCTION_CERTIFICATE_MAGIC);
    appliance_production_certificate_store_u32(
        header.version,
        APPLIANCE_PRODUCTION_CERTIFICATE_RECORD_VERSION_2);
    appliance_production_certificate_store_u32(
        header.source,
        certificate->acme_issued
            ? APPLIANCE_PRODUCTION_CERTIFICATE_SOURCE_ACME
            : (certificate->imported
                   ? APPLIANCE_PRODUCTION_CERTIFICATE_SOURCE_OPERATOR
                   : APPLIANCE_PRODUCTION_CERTIFICATE_SOURCE_GENERATED));
    appliance_production_certificate_store_u32(
        header.certificate_length, certificate->certificate_length);
    appliance_production_certificate_store_u32(
        header.chain_length, certificate->chain_length);
    appliance_production_certificate_store_u32(
        header.key_length, certificate->key_length);
    memcpy(&record[0], &header, sizeof(header));
    memcpy(
        &record[sizeof(header)],
        certificate->certificate_der,
        certificate->certificate_length);
    if (certificate->chain_length != 0U) {
        memcpy(
            &record[sizeof(header) + certificate->certificate_length],
            certificate->chain_der,
            certificate->chain_length);
    }
    memcpy(
        &record[sizeof(header) + certificate->certificate_length +
                certificate->chain_length],
        certificate->key_der,
        certificate->key_length);
    if (appliance_state_block_device_write(
            &certificate->store, 0U, record, sizeof(record)) !=
            BLOCK_DEVICE_OK ||
        appliance_state_block_device_flush(&certificate->store) !=
            BLOCK_DEVICE_OK) {
        certificate->last_status =
            APPLIANCE_PRODUCTION_CERTIFICATE_STORE_WRITE_FAILED;
        appliance_production_certificate_zero(record, sizeof(record));
        return false;
    }
    ++certificate->store_write_count;
    certificate->persisted = true;
    return true;
}

/*
 * ITEM-503: write an operator certificate as record v2 and read it back.
 * ITEM-508 increment 23: the same writer carries an ACME-issued chain, and
 * the `acme` flag is the only difference between the two provenances.
 *
 * The record is the durable form of an import, and the active in-memory
 * certificate is deliberately left alone: the listener's mbedTLS
 * configuration is built once from the bytes it loaded at boot, so replacing
 * the served bytes under a live listener would make `/hfoc-ca.crt` disagree
 * with the handshake.  The record is loaded by the next `prepare()`, which is
 * exactly the honest "applies after reboot" the caller publishes.
 *
 * Nothing is written until every length fits the record; a read-back failure
 * reports STORE_READ_FAILED and leaves the new record in place only when the
 * bytes read back are the bytes written.
 */
static enum appliance_production_certificate_status
appliance_production_certificate_adopt_source(
    struct appliance_production_certificate *certificate,
    const uint8_t *certificate_der,
    uint32_t certificate_length,
    const uint8_t *chain_der,
    uint32_t chain_length,
    const uint8_t *key_der,
    uint32_t key_length,
    bool acme,
    uint8_t fingerprint_out[SHA256_DIGEST_SIZE])
{
    /*
     * The candidate and the read-back view are file-scope: one certificate
     * structure is over 6 KiB and two of them must not become kernel stack
     * frames.  The import path is single-threaded.
     */
    static struct appliance_production_certificate candidate;
    static struct appliance_production_certificate verify;
    uint8_t fingerprint[SHA256_DIGEST_SIZE];

    if (certificate == NULL || certificate_der == NULL ||
        key_der == NULL || fingerprint_out == NULL ||
        certificate_length == 0U ||
        certificate_length > APPLIANCE_PRODUCTION_CERTIFICATE_DER_MAX ||
        chain_length > APPLIANCE_PRODUCTION_CERTIFICATE_CHAIN_DER_MAX ||
        (chain_length != 0U && chain_der == NULL) ||
        key_length == 0U ||
        key_length > APPLIANCE_PRODUCTION_CERTIFICATE_KEY_DER_MAX) {
        return APPLIANCE_PRODUCTION_CERTIFICATE_RECORD_TOO_LARGE;
    }
    if (!certificate->store_bound) {
        certificate->last_status =
            APPLIANCE_PRODUCTION_CERTIFICATE_STORE_UNAVAILABLE;
        return certificate->last_status;
    }
    memset(&candidate, 0, sizeof(candidate));
    candidate.initialized = true;
    candidate.version = APPLIANCE_PRODUCTION_CERTIFICATE_VERSION;
    candidate.store = certificate->store;
    candidate.store_bound = true;
    memcpy(candidate.certificate_der, certificate_der, certificate_length);
    candidate.certificate_length = certificate_length;
    candidate.imported = !acme;
    candidate.acme_issued = acme;
    if (chain_length != 0U) {
        memcpy(candidate.chain_der, chain_der, chain_length);
        candidate.chain_length = chain_length;
    }
    memcpy(candidate.key_der, key_der, key_length);
    candidate.key_length = key_length;
    /*
     * The record is only written for bytes whose metadata parses: a record
     * that cannot be served must never be persisted over a servable one.
     */
    if (!appliance_production_certificate_metadata_from_der(&candidate)) {
        certificate->last_status =
            APPLIANCE_PRODUCTION_CERTIFICATE_RECORD_CORRUPT;
        appliance_production_certificate_zero(
            &candidate, sizeof(candidate));
        return certificate->last_status;
    }
    memcpy(fingerprint, candidate.fingerprint, sizeof(fingerprint));
    if (!appliance_production_certificate_write_record(&candidate)) {
        certificate->last_status = candidate.last_status;
        appliance_production_certificate_zero(
            &candidate, sizeof(candidate));
        return certificate->last_status;
    }
    /*
     * Read the record back through the loader path and require the served
     * bytes and their fingerprint to match the candidate exactly.  A mismatch
     * is a failed import, not a silent partial write.
     */
    {
        memset(&verify, 0, sizeof(verify));
        verify.initialized = true;
        verify.version = APPLIANCE_PRODUCTION_CERTIFICATE_VERSION;
        verify.store = certificate->store;
        verify.store_bound = true;
        if (!appliance_production_certificate_load_record(&verify) ||
            verify.certificate_length != certificate_length ||
            verify.chain_length != chain_length ||
            verify.key_length != key_length ||
            memcmp(verify.certificate_der, certificate_der,
                   certificate_length) != 0 ||
            (chain_length != 0U &&
             memcmp(verify.chain_der, chain_der, chain_length) != 0) ||
            memcmp(verify.key_der, key_der, key_length) != 0 ||
            memcmp(verify.fingerprint, fingerprint,
                   sizeof(fingerprint)) != 0) {
            certificate->last_status =
                APPLIANCE_PRODUCTION_CERTIFICATE_STORE_READ_FAILED;
            appliance_production_certificate_zero(&verify, sizeof(verify));
            appliance_production_certificate_zero(
                &candidate, sizeof(candidate));
            return certificate->last_status;
        }
        appliance_production_certificate_zero(&verify, sizeof(verify));
    }
    memcpy(fingerprint_out, fingerprint, sizeof(fingerprint));
    if (acme) {
        ++certificate->acme_count;
    } else {
        ++certificate->import_count;
    }
    certificate->last_status = APPLIANCE_PRODUCTION_CERTIFICATE_OK;
    appliance_production_certificate_zero(&candidate, sizeof(candidate));
    return APPLIANCE_PRODUCTION_CERTIFICATE_OK;
}

enum appliance_production_certificate_status
appliance_production_certificate_adopt(
    struct appliance_production_certificate *certificate,
    const uint8_t *certificate_der,
    uint32_t certificate_length,
    const uint8_t *chain_der,
    uint32_t chain_length,
    const uint8_t *key_der,
    uint32_t key_length,
    uint8_t fingerprint_out[SHA256_DIGEST_SIZE])
{
    return appliance_production_certificate_adopt_source(
        certificate, certificate_der, certificate_length, chain_der,
        chain_length, key_der, key_length, false, fingerprint_out);
}

enum appliance_production_certificate_status
appliance_production_certificate_adopt_acme(
    struct appliance_production_certificate *certificate,
    const uint8_t *certificate_der,
    uint32_t certificate_length,
    const uint8_t *chain_der,
    uint32_t chain_length,
    const uint8_t *key_der,
    uint32_t key_length,
    uint8_t fingerprint_out[SHA256_DIGEST_SIZE])
{
    return appliance_production_certificate_adopt_source(
        certificate, certificate_der, certificate_length, chain_der,
        chain_length, key_der, key_length, true, fingerprint_out);
}

/* ------------------------------------------------------------ generation */

/*
 * ITEM-503: the DER of the subjectAltName extension value for the appliance's
 * own name.  Modern clients ignore the common name entirely, so a certificate
 * without a SAN warns even when the operator has imported the certificate into
 * the trust store - the extension is what makes "trust our certificate" work.
 *
 * GeneralNames ::= SEQUENCE OF GeneralName; one dNSName ([2], IMPLICIT
 * IA5String) is written.  The name is one validated hostname of at most
 * APPLIANCE_PRODUCTION_CERTIFICATE_COMMON_NAME_MAX bytes, so both length
 * octets stay in the short form.
 */
static bool appliance_production_certificate_san_der(
    const struct appliance_production_certificate *certificate,
    unsigned char *der,
    size_t capacity,
    size_t *length)
{
    const size_t name_length = certificate->common_name_length;

    if (der == NULL || length == NULL || name_length == 0U ||
        name_length > 125U || capacity < name_length + 4U) {
        return false;
    }
    der[0] = 0x30U;                             /* SEQUENCE */
    der[1] = (unsigned char)(name_length + 2U);
    der[2] = 0x82U;                             /* [2] dNSName */
    der[3] = (unsigned char)name_length;
    memcpy(&der[4U], certificate->common_name, name_length);
    *length = name_length + 4U;
    return true;
}

static bool appliance_production_certificate_generate(
    struct appliance_production_certificate *certificate,
    uint32_t now_epoch_seconds)
{
    mbedtls_pk_context key;
    mbedtls_x509write_cert writer;
    uint8_t buffer[APPLIANCE_PRODUCTION_CERTIFICATE_DER_MAX];
    uint8_t serial[APPLIANCE_PRODUCTION_CERTIFICATE_SERIAL_BYTES];
    char name[APPLIANCE_PRODUCTION_CERTIFICATE_COMMON_NAME_MAX +
              sizeof(APPLIANCE_PRODUCTION_CERTIFICATE_SUBJECT_OID) +
              4U];
    size_t name_length;
    char not_before[APPLIANCE_PRODUCTION_CERTIFICATE_VALIDITY_LENGTH];
    char not_after[APPLIANCE_PRODUCTION_CERTIFICATE_VALIDITY_LENGTH];
    uint32_t lifetime_seconds =
        APPLIANCE_PRODUCTION_CERTIFICATE_LIFETIME_SECONDS;
    int result;

    if (!appliance_production_entropy_ready()) {
        certificate->last_status =
            APPLIANCE_PRODUCTION_CERTIFICATE_ENTROPY_UNAVAILABLE;
        return false;
    }
    if (certificate->common_name_length == 0U) {
        certificate->last_status =
            APPLIANCE_PRODUCTION_CERTIFICATE_HOSTNAME_UNAVAILABLE;
        return false;
    }
    {
        uint32_t name_length = certificate->common_name_length;
        uint32_t index;

        for (index = 0U; index < name_length; ++index) {
            char character = certificate->common_name[index];

            /*
             * The subject is assembled into a fixed buffer, so every byte
             * that could terminate or re-open an RDN is refused rather than
             * escaped after the fact.
             */
            if (character == '\0' || character == ',' ||
                character == '=' || character == '+' ||
                character == '<' || character == '>' ||
                character == '#' || character == ';' ||
                character == '"' || character == '\\' ||
                (uint8_t)character < 0x20U) {
                certificate->last_status =
                    APPLIANCE_PRODUCTION_CERTIFICATE_HOSTNAME_UNAVAILABLE;
                return false;
            }
        }
    }

    memset(buffer, 0, sizeof(buffer));
    memset(name, 0, sizeof(name));
    memset(serial, 0, sizeof(serial));
    memcpy(&name[0], "CN=", 3U);
    memcpy(
        &name[3],
        certificate->common_name,
        certificate->common_name_length);
    name[3U + certificate->common_name_length] = ',';
    memcpy(
        &name[4U + certificate->common_name_length],
        APPLIANCE_PRODUCTION_CERTIFICATE_SUBJECT_OID,
        sizeof(APPLIANCE_PRODUCTION_CERTIFICATE_SUBJECT_OID) - 1U);
    name_length = 4U + certificate->common_name_length +
                  (sizeof(APPLIANCE_PRODUCTION_CERTIFICATE_SUBJECT_OID) -
                   1U);
    name[name_length] = '\0';
    if (appliance_production_entropy_random(
            NULL, serial, sizeof(serial)) != 0) {
        certificate->last_status =
            APPLIANCE_PRODUCTION_CERTIFICATE_ENTROPY_UNAVAILABLE;
        return false;
    }
    /* A positive serial: a leading zero byte would be a negative INTEGER. */
    serial[0] = (uint8_t)((serial[0] & 0x7FU) | 0x01U);

    if (now_epoch_seconds < lifetime_seconds) {
        /*
         * No validated wall clock: publish a documented fixed window rather
         * than a window derived from an unvalidated time, and say so in the
         * state rendering.
         */
        memcpy(
            not_before,
            APPLIANCE_PRODUCTION_CERTIFICATE_FALLBACK_NOT_BEFORE,
            APPLIANCE_PRODUCTION_CERTIFICATE_VALIDITY_LENGTH);
        memcpy(
            not_after,
            APPLIANCE_PRODUCTION_CERTIFICATE_FALLBACK_NOT_AFTER,
            APPLIANCE_PRODUCTION_CERTIFICATE_VALIDITY_LENGTH);
        certificate->fallback_validity = true;
        certificate->not_before_epoch =
            APPLIANCE_PRODUCTION_CERTIFICATE_FALLBACK_NOT_BEFORE_EPOCH;
        certificate->not_after_epoch =
            APPLIANCE_PRODUCTION_CERTIFICATE_FALLBACK_NOT_AFTER_EPOCH;
    } else {
        uint32_t start = now_epoch_seconds - 86400U;

        appliance_production_certificate_format_time(start, not_before);
        appliance_production_certificate_format_time(
            start + lifetime_seconds, not_after);
        certificate->not_before_epoch = start;
        certificate->not_after_epoch = start + lifetime_seconds;
    }

    mbedtls_pk_init(&key);
    mbedtls_x509write_crt_init(&writer);
    result = mbedtls_pk_setup(
        &key, mbedtls_pk_info_from_type(MBEDTLS_PK_ECKEY));
    if (result == 0) {
        result = mbedtls_ecp_gen_key(
            MBEDTLS_ECP_DP_SECP256R1,
            mbedtls_pk_ec(key),
            appliance_production_entropy_random,
            NULL);
    }
    if (result != 0) {
        certificate->last_result = (int32_t)result;
        certificate->last_status =
            APPLIANCE_PRODUCTION_CERTIFICATE_KEY_GENERATION_FAILED;
        mbedtls_x509write_crt_free(&writer);
        mbedtls_pk_free(&key);
        appliance_production_certificate_zero(buffer, sizeof(buffer));
        return false;
    }

    mbedtls_x509write_crt_set_version(
        &writer, MBEDTLS_X509_CRT_VERSION_3);
    mbedtls_x509write_crt_set_md_alg(&writer, MBEDTLS_MD_SHA256);
    mbedtls_x509write_crt_set_subject_key(&writer, &key);
    mbedtls_x509write_crt_set_issuer_key(&writer, &key);
    result = mbedtls_x509write_crt_set_subject_name(&writer, name);
    if (result == 0) {
        result = mbedtls_x509write_crt_set_issuer_name(&writer, name);
    }
    if (result == 0) {
        result = mbedtls_x509write_crt_set_validity(
            &writer, not_before, not_after);
    }
    if (result == 0) {
        result = mbedtls_x509write_crt_set_basic_constraints(
            &writer, 0, -1);
    }
    if (result == 0) {
        unsigned char san[APPLIANCE_PRODUCTION_CERTIFICATE_COMMON_NAME_MAX +
                          4U];
        size_t san_length = 0U;

        if (!appliance_production_certificate_san_der(
                certificate, san, sizeof(san), &san_length)) {
            result = -1;
        } else {
            result = mbedtls_x509write_crt_set_extension(
                &writer, MBEDTLS_OID_SUBJECT_ALT_NAME,
                MBEDTLS_OID_SIZE(MBEDTLS_OID_SUBJECT_ALT_NAME), 0,
                san, san_length);
        }
    }
    if (result == 0) {
        result = mbedtls_x509write_crt_set_serial_raw(
            &writer, serial, sizeof(serial));
    }

    if (result == 0) {
        result = mbedtls_x509write_crt_der(
            &writer,
            buffer,
            sizeof(buffer),
            appliance_production_entropy_random,
            NULL);
    }
    if (result <= 0 ||
        (size_t)result >
            APPLIANCE_PRODUCTION_CERTIFICATE_DER_MAX) {
        certificate->last_result = (int32_t)result;
        certificate->last_status =
            APPLIANCE_PRODUCTION_CERTIFICATE_CERTIFICATE_WRITE_FAILED;
        mbedtls_x509write_crt_free(&writer);
        mbedtls_pk_free(&key);
        appliance_production_certificate_zero(buffer, sizeof(buffer));
        return false;
    }
    /*
     * mbedtls_x509write_crt_der writes the certificate at the end of the
     * caller buffer, so the DER starts at the returned offset from the end.
     */
    memcpy(
        certificate->certificate_der,
        &buffer[sizeof(buffer) - (size_t)result],
        (size_t)result);
    certificate->certificate_length = (uint32_t)result;
    mbedtls_x509write_crt_free(&writer);

    {
        uint8_t key_buffer[APPLIANCE_PRODUCTION_CERTIFICATE_KEY_DER_MAX];
        int key_length;

        memset(key_buffer, 0, sizeof(key_buffer));
        key_length = mbedtls_pk_write_key_der(
            &key, key_buffer, sizeof(key_buffer));
        if (key_length <= 0 ||
            (size_t)key_length >
                APPLIANCE_PRODUCTION_CERTIFICATE_KEY_DER_MAX) {
            certificate->last_result = (int32_t)key_length;
            certificate->last_status =
                APPLIANCE_PRODUCTION_CERTIFICATE_KEY_GENERATION_FAILED;
            mbedtls_pk_free(&key);
            appliance_production_certificate_zero(
                key_buffer, sizeof(key_buffer));
            appliance_production_certificate_zero(
                buffer, sizeof(buffer));
            return false;
        }
        memcpy(
            certificate->key_der,
            &key_buffer[sizeof(key_buffer) - (size_t)key_length],
            (size_t)key_length);
        certificate->key_length = (uint32_t)key_length;
        appliance_production_certificate_zero(
            key_buffer, sizeof(key_buffer));
    }
    mbedtls_pk_free(&key);
    appliance_production_certificate_zero(buffer, sizeof(buffer));
    appliance_production_certificate_zero(name, sizeof(name));
    appliance_production_certificate_zero(serial, sizeof(serial));
    /*
     * ITEM-404: publish the metadata of the DER that was just generated.  It
     * must be the geometry the writer encoded, so it is parsed back from the
     * DER rather than taken from the local window variables; the fingerprint
     * is SHA-256 over the served bytes.
     */
    if (!appliance_production_certificate_metadata_from_der(certificate)) {
        certificate->certificate_length = 0U;
        certificate->key_length = 0U;
        certificate->last_status =
            APPLIANCE_PRODUCTION_CERTIFICATE_CERTIFICATE_WRITE_FAILED;
        return false;
    }
    certificate->generated = true;
    ++certificate->generation_count;
    return true;
}

/* -------------------------------------------------------------- public API */

enum appliance_production_certificate_status
appliance_production_certificate_prepare(
    struct appliance_production_certificate *certificate,
    struct appliance_state_loader *loader,
    uint32_t now_epoch_seconds)
{
    if (certificate == NULL) {
        return APPLIANCE_PRODUCTION_CERTIFICATE_NULL;
    }
    if (!certificate->initialized) {
        memset(certificate, 0, sizeof(*certificate));
        certificate->version = APPLIANCE_PRODUCTION_CERTIFICATE_VERSION;
        certificate->initialized = true;
        certificate->state =
            APPLIANCE_PRODUCTION_CERTIFICATE_STATE_UNINITIALIZED;
        certificate->last_status = APPLIANCE_PRODUCTION_CERTIFICATE_OK;
        if (!appliance_production_certificate_identity(
                certificate, loader)) {
            certificate->state =
                APPLIANCE_PRODUCTION_CERTIFICATE_STATE_UNAVAILABLE;
            certificate->last_status =
                APPLIANCE_PRODUCTION_CERTIFICATE_HOSTNAME_UNAVAILABLE;
            return certificate->last_status;
        }
    }
    /*
     * Re-runnable by contract: ordinary boot calls this again on every event
     * loop turn until the appliance has a certificate, because the state
     * volume and the hardware entropy source both arrive asynchronously.  A
     * published certificate is never regenerated.
     */
    if (certificate->available) {
        return APPLIANCE_PRODUCTION_CERTIFICATE_OK;
    }
    if (!certificate->store_bound) {
        (void)appliance_production_certificate_bind_store(
            certificate, loader);
    }
    if (certificate->certificate_length == 0U) {
        (void)appliance_production_certificate_load_record(certificate);
    }
    if (certificate->certificate_length == 0U) {
        if (!appliance_production_certificate_generate(
                certificate, now_epoch_seconds)) {
            certificate->state =
                APPLIANCE_PRODUCTION_CERTIFICATE_STATE_UNAVAILABLE;
            return certificate->last_status;
        }
        certificate->last_result = 0;
        if (certificate->store_bound) {
            (void)appliance_production_certificate_write_record(
                certificate);
        }
    }

    certificate->available = true;
    if (certificate->persisted) {
        certificate->state =
            APPLIANCE_PRODUCTION_CERTIFICATE_STATE_READY;
        certificate->last_status =
            APPLIANCE_PRODUCTION_CERTIFICATE_OK;
    } else {
        certificate->state =
            APPLIANCE_PRODUCTION_CERTIFICATE_STATE_READY_EPHEMERAL;
        if (certificate->last_status ==
            APPLIANCE_PRODUCTION_CERTIFICATE_OK) {
            certificate->last_status =
                APPLIANCE_PRODUCTION_CERTIFICATE_STORE_UNAVAILABLE;
        }
    }
    return APPLIANCE_PRODUCTION_CERTIFICATE_OK;
}

bool appliance_production_certificate_available(
    const struct appliance_production_certificate *certificate)
{
    return certificate != NULL &&
           certificate->initialized &&
           certificate->available &&
           certificate->certificate_length != 0U &&
           certificate->key_length != 0U;
}

enum appliance_production_certificate_state
appliance_production_certificate_state(
    const struct appliance_production_certificate *certificate)
{
    return certificate != NULL
        ? certificate->state
        : APPLIANCE_PRODUCTION_CERTIFICATE_STATE_NONE;
}

const char *appliance_production_certificate_state_string(
    enum appliance_production_certificate_state state)
{
    switch (state) {
    case APPLIANCE_PRODUCTION_CERTIFICATE_STATE_UNINITIALIZED:
        return "UNINITIALIZED";
    case APPLIANCE_PRODUCTION_CERTIFICATE_STATE_READY:
        return "READY";
    case APPLIANCE_PRODUCTION_CERTIFICATE_STATE_READY_EPHEMERAL:
        return "READY_EPHEMERAL";
    case APPLIANCE_PRODUCTION_CERTIFICATE_STATE_UNAVAILABLE:
        return "UNAVAILABLE";
    case APPLIANCE_PRODUCTION_CERTIFICATE_STATE_FAILED:
        return "FAILED";
    case APPLIANCE_PRODUCTION_CERTIFICATE_STATE_NONE:
    default:
        return "NONE";
    }
}

const char *appliance_production_certificate_status_string(
    enum appliance_production_certificate_status status)
{
    switch (status) {
    case APPLIANCE_PRODUCTION_CERTIFICATE_OK:
        return "OK";
    case APPLIANCE_PRODUCTION_CERTIFICATE_NULL:
        return "NULL";
    case APPLIANCE_PRODUCTION_CERTIFICATE_BAD_STATE:
        return "BAD_STATE";
    case APPLIANCE_PRODUCTION_CERTIFICATE_STORE_UNAVAILABLE:
        return "STORE_UNAVAILABLE";
    case APPLIANCE_PRODUCTION_CERTIFICATE_STORE_SEALED:
        return "STORE_SEALED";
    case APPLIANCE_PRODUCTION_CERTIFICATE_STORE_READ_FAILED:
        return "STORE_READ_FAILED";
    case APPLIANCE_PRODUCTION_CERTIFICATE_STORE_WRITE_FAILED:
        return "STORE_WRITE_FAILED";
    case APPLIANCE_PRODUCTION_CERTIFICATE_RECORD_CORRUPT:
        return "RECORD_CORRUPT";
    case APPLIANCE_PRODUCTION_CERTIFICATE_ENTROPY_UNAVAILABLE:
        return "ENTROPY_UNAVAILABLE";
    case APPLIANCE_PRODUCTION_CERTIFICATE_KEY_GENERATION_FAILED:
        return "KEY_GENERATION_FAILED";
    case APPLIANCE_PRODUCTION_CERTIFICATE_CERTIFICATE_WRITE_FAILED:
        return "CERTIFICATE_WRITE_FAILED";
    case APPLIANCE_PRODUCTION_CERTIFICATE_HOSTNAME_UNAVAILABLE:
        return "HOSTNAME_UNAVAILABLE";
    case APPLIANCE_PRODUCTION_CERTIFICATE_RECORD_TOO_LARGE:
        return "RECORD_TOO_LARGE";
    default:
        return NULL;
    }
}

#else

typedef int appliance_production_certificate_translation_unit_guard;

#endif
