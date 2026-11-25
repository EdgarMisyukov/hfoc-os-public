#ifdef HFOC_APPLIANCE_CERTIFICATE_NAME_FIXTURE_QEMU_TEST

/*
 * ITEM-507 increment 6: the two-certificate selection fixture.
 *
 * ITEM-507's evidence needs two different SNI names served two different
 * certificates by a live listener.  The operator path that declares a name -
 * `POST /api/v1/system/certificate/name` with `action=import` - runs the
 * ITEM-503 import gate, and that gate refuses a pair until the appliance has
 * validated its wall clock; the QEMU fixture has no SNTP server it may reach
 * (the SNTP port is 123 and the test host runs unprivileged), so the import is
 * refused BAD_CLOCK there by design and a proof cannot build the table through
 * it.  This fixture therefore writes the two test-only pairs through the same
 * durable writer an operator commit uses - `appliance_certificate_name_store_save`,
 * which writes, flushes and reads the record back - before the listener reads
 * it.  Everything the proof then observes is the production path: the boot
 * loads the record from the encrypted state volume, parses each pair with the
 * TLS profile, registers the SNI callback and selects per handshake.
 *
 * First boot: two EC P-256 pairs are generated with the fixture-only names
 * `alpha.example` and `beta.example` as their subjectAltName, the record
 * declares `default=REFUSE`, and the fixture publishes each entry's
 * certificate SHA-256 prefix.  A later boot writes nothing: the record is
 * already committed, and the fixture publishes the fingerprints it read back,
 * so the second boot proves the record survived.
 *
 * The fixture also sets the account with a test-only credential, because the
 * listener is withheld until an account exists and every other proof reaches
 * that state through the local console conversation; the credential never
 * reaches the serial log, and the proof asserts that.
 */

#include "appliance_certificate_name_store.h"
#include "appliance_production_admin_enrollment.h"
#include "appliance_production_entropy.h"
#include "appliance_production_certificate.h"
#include "mbedtls/ecp.h"
#include "mbedtls/md.h"
#include "mbedtls/oid.h"
#include "mbedtls/pk.h"
#include "mbedtls/x509.h"
#include "mbedtls/x509_crt.h"
#include "serial.h"
#include "sha256.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define APPLIANCE_CERTIFICATE_NAME_FIXTURE_FIRST_NAME "alpha.example"
#define APPLIANCE_CERTIFICATE_NAME_FIXTURE_SECOND_NAME "beta.example"
/* Test-only credential: the fixture ISO is never a production artifact. */
#define APPLIANCE_CERTIFICATE_NAME_FIXTURE_LOGIN "fixture-admin"
#define APPLIANCE_CERTIFICATE_NAME_FIXTURE_SECRET "Hfoc-507-fixture-secret"
#define APPLIANCE_CERTIFICATE_NAME_FIXTURE_SUBJECT_OID "O=HFOC"
#define APPLIANCE_CERTIFICATE_NAME_FIXTURE_SUBJECT_MAX 128U

/*
 * The declaration is a format the next boot reads byte for byte, so the
 * fixture builds it in one static table exactly as the panel's commit does;
 * it is far too large for a kernel stack frame.
 */
static struct appliance_certificate_name_table
    certificate_name_fixture_table;
static uint8_t certificate_name_fixture_certificate[
    APPLIANCE_CERTIFICATE_NAME_DER_MAX];
static uint8_t certificate_name_fixture_key[
    APPLIANCE_CERTIFICATE_NAME_KEY_DER_MAX];
static char certificate_name_fixture_subject[
    APPLIANCE_CERTIFICATE_NAME_FIXTURE_SUBJECT_MAX];

static void certificate_name_fixture_write_hex(
    const uint8_t *bytes,
    size_t length)
{
    static const char digits[] = "0123456789abcdef";
    size_t index;
    char text[2U];

    for (index = 0U; index < length; ++index) {
        text[0] = digits[(bytes[index] >> 4) & 0x0FU];
        text[1] = digits[bytes[index] & 0x0FU];
        serial_write(text);
    }
}

/*
 * The DER of one dNSName (GeneralName [2], IMPLICIT IA5String) inside a
 * GeneralNames SEQUENCE, the same encoding the appliance's own certificate
 * uses.  Both length octets stay in the short form for the fixture names.
 */
static bool certificate_name_fixture_san_der(
    const char *name,
    size_t name_length,
    unsigned char *der,
    size_t capacity,
    size_t *length_out)
{
    if (name == NULL || der == NULL || length_out == NULL ||
        name_length == 0U || name_length > 125U ||
        capacity < name_length + 4U) {
        return false;
    }
    der[0] = 0x30U;
    der[1] = (unsigned char)(name_length + 2U);
    der[2] = 0x82U;
    der[3] = (unsigned char)name_length;
    memcpy(&der[4U], name, name_length);
    *length_out = name_length + 4U;
    return true;
}

/*
 * One EC P-256 self-signed pair whose subjectAltName is the fixture name.  The
 * validity window is the documented fallback window: this fixture runs without
 * a validated wall clock, exactly like the appliance's own first-boot
 * certificate, and the proof's client does not verify the peer.
 */
static bool certificate_name_fixture_generate(
    const char *name,
    size_t name_length,
    uint8_t *certificate_out,
    size_t *certificate_length_out,
    uint8_t *key_out,
    size_t *key_length_out)
{
    mbedtls_pk_context key;
    mbedtls_x509write_cert writer;
    uint8_t buffer[APPLIANCE_CERTIFICATE_NAME_DER_MAX];
    uint8_t serial[APPLIANCE_PRODUCTION_CERTIFICATE_SERIAL_BYTES];
    unsigned char san[APPLIANCE_CERTIFICATE_NAME_MAX + 4U];
    size_t san_length = 0U;
    size_t subject_length = 0U;
    int result;

    if (certificate_out == NULL || certificate_length_out == NULL ||
        key_out == NULL || key_length_out == NULL ||
        name == NULL || name_length == 0U ||
        !appliance_production_entropy_ready()) {
        return false;
    }
    memset(buffer, 0, sizeof(buffer));
    memset(serial, 0, sizeof(serial));
    memset(certificate_name_fixture_subject, 0,
           sizeof(certificate_name_fixture_subject));
    memcpy(&certificate_name_fixture_subject[0], "CN=", 3U);
    memcpy(&certificate_name_fixture_subject[3], name, name_length);
    certificate_name_fixture_subject[3U + name_length] = ',';
    memcpy(&certificate_name_fixture_subject[4U + name_length],
           APPLIANCE_CERTIFICATE_NAME_FIXTURE_SUBJECT_OID,
           sizeof(APPLIANCE_CERTIFICATE_NAME_FIXTURE_SUBJECT_OID) - 1U);
    subject_length = 4U + name_length +
        (sizeof(APPLIANCE_CERTIFICATE_NAME_FIXTURE_SUBJECT_OID) - 1U);
    certificate_name_fixture_subject[subject_length] = '\0';
    if (appliance_production_entropy_random(
            NULL, serial, sizeof(serial)) != 0) {
        return false;
    }
    serial[0] = (uint8_t)((serial[0] & 0x7FU) | 0x01U);
    if (!certificate_name_fixture_san_der(
            name, name_length, san, sizeof(san), &san_length)) {
        return false;
    }

    mbedtls_pk_init(&key);
    mbedtls_x509write_crt_init(&writer);
    result = mbedtls_pk_setup(
        &key, mbedtls_pk_info_from_type(MBEDTLS_PK_ECKEY));
    if (result == 0) {
        result = mbedtls_ecp_gen_key(
            MBEDTLS_ECP_DP_SECP256R1, mbedtls_pk_ec(key),
            appliance_production_entropy_random, NULL);
    }
    if (result == 0) {
        mbedtls_x509write_crt_set_version(
            &writer, MBEDTLS_X509_CRT_VERSION_3);
        mbedtls_x509write_crt_set_md_alg(&writer, MBEDTLS_MD_SHA256);
        mbedtls_x509write_crt_set_subject_key(&writer, &key);
        mbedtls_x509write_crt_set_issuer_key(&writer, &key);
        result = mbedtls_x509write_crt_set_subject_name(
            &writer, certificate_name_fixture_subject);
    }
    if (result == 0) {
        result = mbedtls_x509write_crt_set_issuer_name(
            &writer, certificate_name_fixture_subject);
    }
    if (result == 0) {
        result = mbedtls_x509write_crt_set_validity(
            &writer,
            APPLIANCE_PRODUCTION_CERTIFICATE_FALLBACK_NOT_BEFORE,
            APPLIANCE_PRODUCTION_CERTIFICATE_FALLBACK_NOT_AFTER);
    }
    if (result == 0) {
        result = mbedtls_x509write_crt_set_basic_constraints(
            &writer, 0, -1);
    }
    if (result == 0) {
        result = mbedtls_x509write_crt_set_extension(
            &writer, MBEDTLS_OID_SUBJECT_ALT_NAME,
            MBEDTLS_OID_SIZE(MBEDTLS_OID_SUBJECT_ALT_NAME), 0,
            san, san_length);
    }
    if (result == 0) {
        result = mbedtls_x509write_crt_set_serial_raw(
            &writer, serial, sizeof(serial));
    }
    if (result == 0) {
        result = mbedtls_x509write_crt_der(
            &writer, buffer, sizeof(buffer),
            appliance_production_entropy_random, NULL);
    }
    if (result <= 0 ||
        (size_t)result > APPLIANCE_CERTIFICATE_NAME_DER_MAX) {
        mbedtls_x509write_crt_free(&writer);
        mbedtls_pk_free(&key);
        memset(buffer, 0, sizeof(buffer));
        return false;
    }
    memcpy(certificate_out, &buffer[sizeof(buffer) - (size_t)result],
           (size_t)result);
    *certificate_length_out = (size_t)result;
    mbedtls_x509write_crt_free(&writer);

    {
        int key_length = mbedtls_pk_write_key_der(
            &key, key_out, APPLIANCE_CERTIFICATE_NAME_KEY_DER_MAX);

        if (key_length <= 0 ||
            (size_t)key_length > APPLIANCE_CERTIFICATE_NAME_KEY_DER_MAX) {
            mbedtls_pk_free(&key);
            memset(buffer, 0, sizeof(buffer));
            memset(key_out, 0, APPLIANCE_CERTIFICATE_NAME_KEY_DER_MAX);
            return false;
        }
        if ((size_t)key_length !=
            APPLIANCE_CERTIFICATE_NAME_KEY_DER_MAX) {
            memmove(
                key_out,
                &key_out[APPLIANCE_CERTIFICATE_NAME_KEY_DER_MAX -
                         (size_t)key_length],
                (size_t)key_length);
        }
        *key_length_out = (size_t)key_length;
    }
    mbedtls_pk_free(&key);
    memset(buffer, 0, sizeof(buffer));
    memset(serial, 0, sizeof(serial));
    memset(certificate_name_fixture_subject, 0,
           sizeof(certificate_name_fixture_subject));
    return true;
}

/*
 * Publish one bounded record per declared name: the name, its index, whether a
 * key is present and the SHA-256 prefix of the certificate DER.  The proof's
 * client compares the certificate it received with this prefix, so "which
 * certificate this name is served" is asserted on both sides.  No key byte and
 * no certificate body is ever rendered.
 */
static void certificate_name_fixture_publish(
    const struct appliance_certificate_name_table *table)
{
    uint32_t index;

    for (index = 0U; index < table->count; ++index) {
        const struct appliance_certificate_name *entry =
            &table->entries[index];
        uint8_t digest[SHA256_DIGEST_SIZE];
        uint64_t serial_flags;

        memset(digest, 0, sizeof(digest));
        if (sha256_compute(
                entry->certificate_der, entry->certificate_length,
                digest) != SHA256_OK) {
            serial_flags = serial_sequence_begin();
            serial_write("[CERTIFICATE-FIXTURE] failed: sha256\n");
            serial_sequence_end(serial_flags);
            continue;
        }
        serial_flags = serial_sequence_begin();
        serial_write("[CERTIFICATE-FIXTURE] name=");
        serial_write(entry->name);
        serial_write(" index=");
        serial_write_u64(index);
        serial_write(" certificate_sha256=");
        certificate_name_fixture_write_hex(digest, 16U);
        serial_write(" key=");
        serial_write(entry->key_length != 0U ? "present" : "absent");
        serial_write("\n");
        serial_sequence_end(serial_flags);
        memset(digest, 0, sizeof(digest));
    }
}

void appliance_certificate_name_fixture_seed(
    struct appliance_production_admin_enrollment *enrollment,
    struct appliance_state_loader *loader,
    uint64_t now_tick)
{
    struct appliance_certificate_name_store store;
    enum appliance_certificate_name_store_source source;
    bool seeded = false;
    bool persisted = false;

    if (enrollment != NULL &&
        !appliance_production_admin_enrollment_credential_set(enrollment)) {
        const enum appliance_production_admin_enrollment_status status =
            appliance_production_admin_enrollment_provision_credential(
                enrollment,
                APPLIANCE_CERTIFICATE_NAME_FIXTURE_LOGIN,
                sizeof(APPLIANCE_CERTIFICATE_NAME_FIXTURE_LOGIN) - 1U,
                APPLIANCE_CERTIFICATE_NAME_FIXTURE_SECRET,
                sizeof(APPLIANCE_CERTIFICATE_NAME_FIXTURE_SECRET) - 1U,
                now_tick);

        {
            const uint64_t serial_flags = serial_sequence_begin();

            serial_write("[CERTIFICATE-FIXTURE] account=");
            serial_write(
                status == APPLIANCE_PRODUCTION_ADMIN_ENROLLMENT_OK
                    ? "PROVISIONED"
                    : appliance_production_admin_enrollment_status_string(
                          status));
            serial_write("\n");
            serial_sequence_end(serial_flags);
        }
    }
    appliance_certificate_name_store_init(&store);
    if (loader == NULL) {
        const uint64_t serial_flags = serial_sequence_begin();

        serial_write(
            "[CERTIFICATE-FIXTURE] failed: no state loader\n");
        serial_sequence_end(serial_flags);
        return;
    }
    (void)appliance_certificate_name_store_load(
        &store, loader, &certificate_name_fixture_table);
    source = appliance_certificate_name_store_source(&store);
    if (source != APPLIANCE_CERTIFICATE_NAME_STORE_SOURCE_COMMITTED ||
        certificate_name_fixture_table.count != 2U) {
        size_t certificate_length = 0U;
        size_t key_length = 0U;

        appliance_certificate_name_table_init(
            &certificate_name_fixture_table);
        if (certificate_name_fixture_generate(
                APPLIANCE_CERTIFICATE_NAME_FIXTURE_FIRST_NAME,
                sizeof(APPLIANCE_CERTIFICATE_NAME_FIXTURE_FIRST_NAME) - 1U,
                certificate_name_fixture_certificate,
                &certificate_length,
                certificate_name_fixture_key, &key_length) &&
            appliance_certificate_name_table_add(
                &certificate_name_fixture_table,
                APPLIANCE_CERTIFICATE_NAME_FIXTURE_FIRST_NAME,
                sizeof(APPLIANCE_CERTIFICATE_NAME_FIXTURE_FIRST_NAME) - 1U,
                certificate_name_fixture_certificate,
                certificate_length,
                certificate_name_fixture_key, key_length) ==
                APPLIANCE_CERTIFICATE_NAME_OK) {
            certificate_length = 0U;
            key_length = 0U;
            if (certificate_name_fixture_generate(
                    APPLIANCE_CERTIFICATE_NAME_FIXTURE_SECOND_NAME,
                    sizeof(APPLIANCE_CERTIFICATE_NAME_FIXTURE_SECOND_NAME) -
                        1U,
                    certificate_name_fixture_certificate,
                    &certificate_length,
                    certificate_name_fixture_key, &key_length) &&
                appliance_certificate_name_table_add(
                    &certificate_name_fixture_table,
                    APPLIANCE_CERTIFICATE_NAME_FIXTURE_SECOND_NAME,
                    sizeof(APPLIANCE_CERTIFICATE_NAME_FIXTURE_SECOND_NAME) -
                        1U,
                    certificate_name_fixture_certificate,
                    certificate_length,
                    certificate_name_fixture_key, key_length) ==
                    APPLIANCE_CERTIFICATE_NAME_OK &&
                appliance_certificate_name_table_set_default(
                    &certificate_name_fixture_table,
                    APPLIANCE_CERTIFICATE_NAME_DEFAULT_REFUSE) ==
                    APPLIANCE_CERTIFICATE_NAME_OK) {
                seeded = true;
            }
        }
        memset(certificate_name_fixture_key, 0,
               sizeof(certificate_name_fixture_key));
        memset(certificate_name_fixture_certificate, 0,
               sizeof(certificate_name_fixture_certificate));
        if (seeded) {
            persisted =
                appliance_certificate_name_store_save(
                    &store, &certificate_name_fixture_table) ==
                APPLIANCE_CERTIFICATE_NAME_STORE_OK;
        }
        if (persisted) {
            /*
             * Read the committed record back through the loader path, so the
             * table the listener will use is the one the device holds and not
             * the candidate this fixture built.
             */
            appliance_certificate_name_store_init(&store);
            (void)appliance_certificate_name_store_load(
                &store, loader, &certificate_name_fixture_table);
            source = appliance_certificate_name_store_source(&store);
        }
    }
    {
        const uint64_t serial_flags = serial_sequence_begin();

        serial_write("[CERTIFICATE-FIXTURE] seeded=");
        serial_write(seeded ? "YES" : "NO");
        serial_write(" persisted=");
        serial_write(persisted ? "OK" : "NOT_WRITTEN");
        serial_write(" source=");
        serial_write(
            source == APPLIANCE_CERTIFICATE_NAME_STORE_SOURCE_COMMITTED
                ? "COMMITTED"
                : source ==
                          APPLIANCE_CERTIFICATE_NAME_STORE_SOURCE_DEFAULT_MISSING
                      ? "DEFAULT_MISSING"
                      : source ==
                                APPLIANCE_CERTIFICATE_NAME_STORE_SOURCE_DEFAULT_INVALID
                            ? "DEFAULT_INVALID"
                            : "DEFAULT_UNAVAILABLE");
        serial_write(" count=");
        serial_write_u64(certificate_name_fixture_table.count);
        serial_write("\n");
        serial_sequence_end(serial_flags);
    }
    certificate_name_fixture_publish(&certificate_name_fixture_table);
}

#else
/*
 * The production build compiles this file with the fixture define absent, so
 * the translation unit still has to declare something: an empty unit is a
 * pedantic warning, and the build-warning gate treats a warning as a failure.
 */
typedef int appliance_certificate_name_fixture_translation_unit_guard;
#endif /* HFOC_APPLIANCE_CERTIFICATE_NAME_FIXTURE_QEMU_TEST */
