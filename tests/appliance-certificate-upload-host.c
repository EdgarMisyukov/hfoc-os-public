#include "appliance_certificate_upload.h"

#include "appliance_acme_account.h"
#include "appliance_certificate_import.h"
#include "appliance_production_acme_service.h"
#include "appliance_production_certificate.h"
#include "appliance_state_layout.h"
#include "pem_decode.h"
#include "hfoc_mbedtls_alloc.h"
#include "block_device.h"

#include "appliance-certificate-fixtures.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifdef HFOC_APPLIANCE_CERTIFICATE_UPLOAD_HOST_TEST

/*
 * ITEM-503: the BYO certificate import proof.
 *
 * The first half drives the form path: `application/x-www-form-urlencoded`
 * bodies are decoded into PEM, the PEM is decoded into DER, the pair and the
 * optional chain are validated and a candidate is held; every refusal is
 * named and leaves nothing staged.
 *
 * The second half drives the durable path: a certificate owner bound to a
 * fixture state volume generates its own record, an operator candidate is
 * committed over it, and a fresh owner loads the record back and must see the
 * operator's leaf, chain and key with the operator provenance.  That is the
 * "applies on the next boot" claim the serving path publishes, proven at the
 * record level.
 *
 * ITEM-508 increment 23 adds the third provenance: a chain obtained through
 * ACME and verified against the named anchor is written by `adopt_acme` and a
 * fresh owner must load it back as ACME-issued - never as an operator upload
 * and never as the generated identity - while an unknown provenance on disk
 * is refused as corrupt instead of being relabelled.
 *
 * ITEM-508 increment 24 binds the ACME account key from that persisted
 * identity: the same key bytes must name the same public thumbprint after the
 * record is reloaded (the reboot), a fresh volume must name a different
 * account, and an unavailable identity, an out-of-bound key and a non-EC DER
 * are refusals that leave the account record unbound.
 */

#define HOSTNAME "hfoc-appliance"
#define HOSTNAME_LENGTH 14U

#define FIXTURE_BLOCKS 8192U
#define FIXTURE_BYTES (FIXTURE_BLOCKS * APPLIANCE_STATE_BLOCK_SIZE)

static int failures;

#define CHECK(condition, message) do { \
    if (!(condition)) { \
        fprintf(stderr, "FAIL: %s\n", message); \
        ++failures; \
    } else { \
        printf("PASS: %s\n", message); \
    } \
} while (0)

/* ------------------------------------------------------- platform stubs */

bool appliance_production_entropy_ready(void)
{
    return true;
}

int appliance_production_entropy_random(
    void *context,
    unsigned char *output,
    size_t length)
{
    size_t index;

    (void)context;
    for (index = 0U; index < length; ++index) {
        output[index] = (unsigned char)(0x5AU ^ (unsigned char)index);
    }
    return 0;
}

enum appliance_persistent_config_status
appliance_persistent_config_hostname(
    const struct appliance_persistent_config *config,
    char *hostname,
    size_t capacity,
    size_t *hostname_length)
{
    (void)config;
    if (hostname == NULL || hostname_length == NULL ||
        capacity <= HOSTNAME_LENGTH) {
        return APPLIANCE_PERSISTENT_CONFIG_NULL;
    }
    memcpy(hostname, HOSTNAME, HOSTNAME_LENGTH + 1U);
    *hostname_length = HOSTNAME_LENGTH;
    return APPLIANCE_PERSISTENT_CONFIG_OK;
}

/* ------------------------------------------------------- fixture volume */

static uint8_t fixture_volume[FIXTURE_BYTES];

static enum block_device_status fixture_read(
    struct block_device *device,
    uint64_t offset,
    void *buffer,
    size_t length)
{
    (void)device;
    if (buffer == NULL || offset + length > sizeof(fixture_volume)) {
        return BLOCK_DEVICE_OUT_OF_RANGE;
    }
    memcpy(buffer, fixture_volume + offset, length);
    return BLOCK_DEVICE_OK;
}

static enum block_device_status fixture_write(
    struct block_device *device,
    uint64_t offset,
    const void *buffer,
    size_t length)
{
    (void)device;
    if (buffer == NULL || offset + length > sizeof(fixture_volume)) {
        return BLOCK_DEVICE_OUT_OF_RANGE;
    }
    memcpy(fixture_volume + offset, buffer, length);
    return BLOCK_DEVICE_OK;
}

static enum block_device_status fixture_flush(struct block_device *device)
{
    (void)device;
    return BLOCK_DEVICE_OK;
}

static bool fixture_loader(
    struct appliance_state_loader *loader,
    struct block_device *device)
{
    memset(loader, 0, sizeof(*loader));
    memset(device, 0, sizeof(*device));
    device->block_size = APPLIANCE_STATE_BLOCK_SIZE;
    device->block_count = FIXTURE_BLOCKS;
    device->read_fn = fixture_read;
    device->write_fn = fixture_write;
    device->flush_fn = fixture_flush;
    loader->partition.device = *device;
    if (appliance_state_layout_default(&loader->layout) !=
        APPLIANCE_STATE_LAYOUT_OK) {
        return false;
    }
    loader->ready = true;
    return true;
}

/* ------------------------------------------------------- PEM and form */

static size_t host_pem_encode(
    const unsigned char *der,
    size_t der_length,
    const char *label,
    char *output,
    size_t capacity)
{
    static const char alphabet[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t written = 0U;
    size_t index = 0U;
    size_t line = 0U;

    written += (size_t)snprintf(
        output + written, capacity - written,
        "-----BEGIN %s-----\n", label);
    for (index = 0U; index < der_length; index += 3U) {
        const size_t remaining = der_length - index;
        const uint32_t triple =
            ((uint32_t)der[index] << 16U) |
            (remaining > 1U ? ((uint32_t)der[index + 1U] << 8U) : 0U) |
            (remaining > 2U ? (uint32_t)der[index + 2U] : 0U);
        char block[4U];
        size_t part;

        block[0] = alphabet[(triple >> 18U) & 0x3FU];
        block[1] = alphabet[(triple >> 12U) & 0x3FU];
        block[2] = remaining > 1U ? alphabet[(triple >> 6U) & 0x3FU] : '=';
        block[3] = remaining > 2U ? alphabet[triple & 0x3FU] : '=';
        for (part = 0U; part < 4U; ++part) {
            output[written++] = block[part];
            if (++line == 64U) {
                output[written++] = '\n';
                line = 0U;
            }
        }
    }
    if (line != 0U) {
        output[written++] = '\n';
    }
    written += (size_t)snprintf(
        output + written, capacity - written,
        "-----END %s-----\n", label);
    output[written] = '\0';
    return written;
}

static bool host_form_append_raw(
    char *body,
    size_t capacity,
    size_t *length,
    const char *text)
{
    const size_t text_length = strlen(text);

    if (*length + text_length >= capacity) {
        return false;
    }
    memcpy(body + *length, text, text_length);
    *length += text_length;
    body[*length] = '\0';
    return true;
}

static bool host_form_append(
    char *body,
    size_t capacity,
    size_t *length,
    const char *text,
    size_t text_length)
{
    size_t index;

    for (index = 0U; index < text_length; ++index) {
        const unsigned char byte = (unsigned char)text[index];

        if ((byte >= (unsigned char)'A' && byte <= (unsigned char)'Z') ||
            (byte >= (unsigned char)'a' && byte <= (unsigned char)'z') ||
            (byte >= (unsigned char)'0' && byte <= (unsigned char)'9') ||
            byte == (unsigned char)'-' || byte == (unsigned char)'.' ||
            byte == (unsigned char)'_' || byte == (unsigned char)'~') {
            if (*length + 1U >= capacity) {
                return false;
            }
            body[(*length)++] = (char)byte;
        } else {
            static const char hex[] = "0123456789ABCDEF";

            if (*length + 3U >= capacity) {
                return false;
            }
            body[(*length)++] = '%';
            body[(*length)++] = hex[(byte >> 4U) & 0x0FU];
            body[(*length)++] = hex[byte & 0x0FU];
        }
    }
    body[*length] = '\0';
    return true;
}

static size_t host_form(
    char *body,
    size_t capacity,
    const unsigned char *certificate,
    size_t certificate_length,
    const unsigned char *chain,
    size_t chain_length,
    const unsigned char *key,
    size_t key_length)
{
    static char pem[4096];
    size_t length = 0U;
    size_t pem_length;

    length = 0U;
    pem_length = host_pem_encode(
        certificate, certificate_length, "CERTIFICATE", pem, sizeof(pem));
    if (!host_form_append_raw(body, capacity, &length, "certificate=") ||
        !host_form_append(body, capacity, &length, pem, pem_length)) {
        return 0U;
    }
    if (chain != NULL && chain_length != 0U) {
        pem_length = host_pem_encode(
            chain, chain_length, "CERTIFICATE", pem, sizeof(pem));
        if (!host_form_append_raw(body, capacity, &length, "&chain=") ||
            !host_form_append(body, capacity, &length, pem, pem_length)) {
            return 0U;
        }
    }
    pem_length = host_pem_encode(
        key, key_length, "PRIVATE KEY", pem, sizeof(pem));
    if (!host_form_append_raw(body, capacity, &length, "&key=") ||
        !host_form_append(body, capacity, &length, pem, pem_length)) {
        return 0U;
    }
    return length;
}

/* ------------------------------------------------------------ the tests */

static void test_form_and_stage(void)
{
    struct appliance_production_certificate certificate;
    struct appliance_certificate_upload upload;
    static char body[APPLIANCE_CERTIFICATE_UPLOAD_BODY_MAX + 1U];
    static char oversized[APPLIANCE_CERTIFICATE_UPLOAD_BODY_MAX + 2U];
    size_t body_length;

    memset(&certificate, 0, sizeof(certificate));
    certificate.initialized = true;
    certificate.version = APPLIANCE_PRODUCTION_CERTIFICATE_VERSION;
    memcpy(certificate.common_name, HOSTNAME, HOSTNAME_LENGTH + 1U);
    certificate.common_name_length = HOSTNAME_LENGTH;

    CHECK(appliance_certificate_upload_init(
              &upload, &certificate, fixture_rng, NULL) ==
              APPLIANCE_CERTIFICATE_UPLOAD_OK,
          "upload composition binds the certificate and the random source");
    CHECK(appliance_certificate_upload_set_clock(&upload, FIXTURE_NOW) ==
              APPLIANCE_CERTIFICATE_UPLOAD_OK,
          "upload composition takes the validated clock");

    /* The single certificate and key pair. */
    body_length = host_form(
        body, sizeof(body), fixture_certificate,
        sizeof(fixture_certificate), NULL, 0U, fixture_key,
        sizeof(fixture_key));
    CHECK(body_length != 0U,
          "the certificate form is built from the fixture");
    CHECK(appliance_certificate_upload_stage(
              &upload,
              (const uint8_t *)APPLIANCE_CERTIFICATE_UPLOAD_CONTENT_TYPE,
              APPLIANCE_CERTIFICATE_UPLOAD_CONTENT_TYPE_LENGTH,
              (const uint8_t *)body,
              (uint32_t)body_length) == APPLIANCE_CERTIFICATE_UPLOAD_OK,
          "a valid certificate and key stage a candidate");
    CHECK(appliance_certificate_upload_staged(&upload) == true,
          "the staged candidate is published");
    CHECK(appliance_certificate_upload_chain_count(&upload) == 0U,
          "a candidate without a chain reports no chain");
    CHECK(appliance_certificate_upload_name(&upload) != NULL &&
              strcmp(appliance_certificate_upload_name(&upload),
                     HOSTNAME) == 0,
          "the matched subjectAltName is the appliance name");

    /*
     * ITEM-515: a browser submits every control the page draws.  The Security
     * page draws `certificate`, `chain` and `key`, so the body a browser sends
     * carries an empty `chain=` when the operator pastes no chain.  The stage
     * must accept the drawn body, not only the hand-written one that leaves
     * the field out.
     */
    {
        char *key_at = strstr(body, "&key=");

        CHECK(key_at != NULL, "the drawn Security body carries all fields");
        if (key_at != NULL) {
            const size_t prefix = (size_t)(key_at - body);
            const size_t tail = body_length - prefix;

            CHECK(body_length + 7U < sizeof(body),
                  "the drawn Security body fits its buffer");
            memmove(key_at + 7, key_at, tail + 1U);
            memcpy(key_at, "&chain=", 7U);
            body_length += 7U;
        }
        CHECK(strstr(body, "&chain=&key=") != NULL &&
                  appliance_certificate_upload_stage(
                      &upload,
                      (const uint8_t *)
                          APPLIANCE_CERTIFICATE_UPLOAD_CONTENT_TYPE,
                      APPLIANCE_CERTIFICATE_UPLOAD_CONTENT_TYPE_LENGTH,
                      (const uint8_t *)body,
                      (uint32_t)body_length) ==
                      APPLIANCE_CERTIFICATE_UPLOAD_OK &&
                  appliance_certificate_upload_chain_count(&upload) == 0U,
              "the drawn Security body with an empty chain is accepted");
    }

    /* The chained pair stages with its chain. */
    body_length = host_form(
        body, sizeof(body), fixture_chained_certificate,
        sizeof(fixture_chained_certificate), fixture_chain_ca,
        sizeof(fixture_chain_ca), fixture_chained_key,
        sizeof(fixture_chained_key));
    CHECK(body_length != 0U &&
              appliance_certificate_upload_stage(
                  &upload,
                  (const uint8_t *)APPLIANCE_CERTIFICATE_UPLOAD_CONTENT_TYPE,
                  APPLIANCE_CERTIFICATE_UPLOAD_CONTENT_TYPE_LENGTH,
                  (const uint8_t *)body,
                  (uint32_t)body_length) == APPLIANCE_CERTIFICATE_UPLOAD_OK,
          "a chained certificate stages with its chain");
    CHECK(appliance_certificate_upload_chain_count(&upload) == 1U,
          "the staged chain counts one certificate");

    /* A key that does not match the certificate is refused by name. */
    body_length = host_form(
        body, sizeof(body), fixture_chained_certificate,
        sizeof(fixture_chained_certificate), fixture_chain_ca,
        sizeof(fixture_chain_ca), fixture_key, sizeof(fixture_key));
    CHECK(appliance_certificate_upload_stage(
              &upload,
              (const uint8_t *)APPLIANCE_CERTIFICATE_UPLOAD_CONTENT_TYPE,
              APPLIANCE_CERTIFICATE_UPLOAD_CONTENT_TYPE_LENGTH,
              (const uint8_t *)body,
              (uint32_t)body_length) ==
              APPLIANCE_CERTIFICATE_UPLOAD_BAD_PAIR,
          "a mismatched key is refused by name");
    CHECK(appliance_certificate_upload_staged(&upload) == false,
          "a refused candidate is not staged");

    /* A zero clock is refused: "not validated" is not "valid". */
    CHECK(appliance_certificate_upload_set_clock(&upload, 0U) ==
              APPLIANCE_CERTIFICATE_UPLOAD_OK,
          "upload composition accepts a zero clock reading");
    body_length = host_form(
        body, sizeof(body), fixture_certificate,
        sizeof(fixture_certificate), NULL, 0U, fixture_key,
        sizeof(fixture_key));
    CHECK(appliance_certificate_upload_stage(
              &upload,
              (const uint8_t *)APPLIANCE_CERTIFICATE_UPLOAD_CONTENT_TYPE,
              APPLIANCE_CERTIFICATE_UPLOAD_CONTENT_TYPE_LENGTH,
              (const uint8_t *)body,
              (uint32_t)body_length) ==
              APPLIANCE_CERTIFICATE_UPLOAD_BAD_CLOCK,
          "an unvalidated clock refuses the import");
    CHECK(appliance_certificate_upload_set_clock(&upload, FIXTURE_NOW) ==
              APPLIANCE_CERTIFICATE_UPLOAD_OK,
          "the validated clock is restored");

    /* Missing field, wrong content type, malformed encoding, garbage. */
    CHECK(appliance_certificate_upload_stage(
              &upload,
              (const uint8_t *)APPLIANCE_CERTIFICATE_UPLOAD_CONTENT_TYPE,
              APPLIANCE_CERTIFICATE_UPLOAD_CONTENT_TYPE_LENGTH,
              (const uint8_t *)"certificate=x", 13U) ==
              APPLIANCE_CERTIFICATE_UPLOAD_MISSING_FIELD,
          "a body without a key is refused as missing a field");
    CHECK(appliance_certificate_upload_stage(
              &upload, (const uint8_t *)"text/plain", 10U,
              (const uint8_t *)body, (uint32_t)body_length) ==
              APPLIANCE_CERTIFICATE_UPLOAD_BAD_CONTENT_TYPE,
          "a body that is not a form is refused");
    CHECK(appliance_certificate_upload_stage(
              &upload,
              (const uint8_t *)APPLIANCE_CERTIFICATE_UPLOAD_CONTENT_TYPE,
              APPLIANCE_CERTIFICATE_UPLOAD_CONTENT_TYPE_LENGTH,
              (const uint8_t *)"certificate=%GG&key=x", 20U) ==
              APPLIANCE_CERTIFICATE_UPLOAD_MALFORMED,
          "a malformed percent escape is refused");
    CHECK(appliance_certificate_upload_stage(
              &upload,
              (const uint8_t *)APPLIANCE_CERTIFICATE_UPLOAD_CONTENT_TYPE,
              APPLIANCE_CERTIFICATE_UPLOAD_CONTENT_TYPE_LENGTH,
              (const uint8_t *)"certificate=not+a+pem&key=also+not", 34U) ==
              APPLIANCE_CERTIFICATE_UPLOAD_BAD_CERTIFICATE,
          "a body without PEM armour is refused as a bad certificate");

    memset(oversized, 'a', sizeof(oversized) - 1U);
    oversized[sizeof(oversized) - 1U] = '\0';
    CHECK(appliance_certificate_upload_stage(
              &upload,
              (const uint8_t *)APPLIANCE_CERTIFICATE_UPLOAD_CONTENT_TYPE,
              APPLIANCE_CERTIFICATE_UPLOAD_CONTENT_TYPE_LENGTH,
              (const uint8_t *)oversized,
              APPLIANCE_CERTIFICATE_UPLOAD_BODY_MAX + 1U) ==
              APPLIANCE_CERTIFICATE_UPLOAD_TOO_LARGE,
          "a body over the framing bound is refused, not truncated");

    CHECK(appliance_certificate_upload_rollback(&upload) ==
              APPLIANCE_CERTIFICATE_UPLOAD_OK,
          "rollback drops the candidate");
    CHECK(appliance_certificate_upload_commit(&upload) ==
              APPLIANCE_CERTIFICATE_UPLOAD_NO_CANDIDATE,
          "commit without a candidate writes nothing");
}

static void test_commit_and_record(void)
{
    static struct appliance_production_certificate owner;
    static struct appliance_production_certificate reloaded;
    struct appliance_certificate_upload upload;
    struct appliance_state_loader loader;
    struct block_device device;
    static char body[APPLIANCE_CERTIFICATE_UPLOAD_BODY_MAX + 1U];
    size_t body_length;

    memset(fixture_volume, 0, sizeof(fixture_volume));
    CHECK(fixture_loader(&loader, &device) == true,
          "the fixture state layout is valid");

    memset(&owner, 0, sizeof(owner));
    CHECK(appliance_production_certificate_prepare(
              &owner, &loader, FIXTURE_NOW) ==
              APPLIANCE_PRODUCTION_CERTIFICATE_OK &&
              appliance_production_certificate_available(&owner),
          "the certificate owner generates and persists its own record");
    CHECK(owner.imported == false,
          "the generated record is not an operator import");

    CHECK(appliance_certificate_upload_init(
              &upload, &owner, fixture_rng, NULL) ==
              APPLIANCE_CERTIFICATE_UPLOAD_OK,
          "the upload composition binds the persisted owner");
    (void)appliance_certificate_upload_set_clock(&upload, FIXTURE_NOW);
    body_length = host_form(
        body, sizeof(body), fixture_chained_certificate,
        sizeof(fixture_chained_certificate), fixture_chain_ca,
        sizeof(fixture_chain_ca), fixture_chained_key,
        sizeof(fixture_chained_key));
    CHECK(body_length != 0U &&
              appliance_certificate_upload_stage(
                  &upload,
                  (const uint8_t *)APPLIANCE_CERTIFICATE_UPLOAD_CONTENT_TYPE,
                  APPLIANCE_CERTIFICATE_UPLOAD_CONTENT_TYPE_LENGTH,
                  (const uint8_t *)body,
                  (uint32_t)body_length) == APPLIANCE_CERTIFICATE_UPLOAD_OK,
          "the operator candidate stages against the persisted owner");
    CHECK(appliance_certificate_upload_commit(&upload) ==
              APPLIANCE_CERTIFICATE_UPLOAD_OK,
          "the confirmed candidate writes record v2");
    CHECK(owner.import_count == 1U,
          "the owner counts the operator import");

    /*
     * The next boot loads the record.  The active in-memory owner was not
     * replaced (the listener keeps serving what it loaded), so a fresh owner
     * over the same volume is exactly the reboot.
     */
    memset(&reloaded, 0, sizeof(reloaded));
    CHECK(appliance_production_certificate_prepare(
              &reloaded, &loader, FIXTURE_NOW) ==
              APPLIANCE_PRODUCTION_CERTIFICATE_OK &&
              appliance_production_certificate_available(&reloaded),
          "the next boot loads the operator record");
    CHECK(reloaded.imported == true,
          "the loaded record keeps the operator provenance");
    CHECK(reloaded.certificate_length ==
              sizeof(fixture_chained_certificate) &&
              memcmp(reloaded.certificate_der, fixture_chained_certificate,
                     sizeof(fixture_chained_certificate)) == 0,
          "the loaded record serves the operator certificate byte for byte");
    CHECK(reloaded.key_length == sizeof(fixture_chained_key) &&
              memcmp(reloaded.key_der, fixture_chained_key,
                     sizeof(fixture_chained_key)) == 0,
          "the loaded record serves the operator key byte for byte");
    CHECK(reloaded.chain_length == sizeof(fixture_chain_ca) &&
              memcmp(reloaded.chain_der, fixture_chain_ca,
                     sizeof(fixture_chain_ca)) == 0,
          "the loaded record carries the operator chain byte for byte");
    CHECK(reloaded.issuer_length != 0U,
          "the loaded record publishes the issuer its DER names");

    /*
     * A second staged candidate that is rolled back, and a refused one, both
     * leave the stored operator record untouched.
     */
    body_length = host_form(
        body, sizeof(body), fixture_certificate,
        sizeof(fixture_certificate), NULL, 0U, fixture_key,
        sizeof(fixture_key));
    CHECK(appliance_certificate_upload_stage(
              &upload,
              (const uint8_t *)APPLIANCE_CERTIFICATE_UPLOAD_CONTENT_TYPE,
              APPLIANCE_CERTIFICATE_UPLOAD_CONTENT_TYPE_LENGTH,
              (const uint8_t *)body,
              (uint32_t)body_length) == APPLIANCE_CERTIFICATE_UPLOAD_OK,
          "a second candidate stages");
    CHECK(appliance_certificate_upload_rollback(&upload) ==
              APPLIANCE_CERTIFICATE_UPLOAD_OK,
          "the second candidate is cancelled");
    memset(&reloaded, 0, sizeof(reloaded));
    CHECK(appliance_production_certificate_prepare(
              &reloaded, &loader, FIXTURE_NOW) ==
              APPLIANCE_PRODUCTION_CERTIFICATE_OK &&
              reloaded.imported == true &&
              reloaded.chain_length == sizeof(fixture_chain_ca),
          "a cancelled candidate never replaces the stored certificate");
}

/*
 * ITEM-508 increment 23: a chain obtained through ACME and verified against
 * the named anchor is persisted with its own provenance.  The record path,
 * the bounds and the read-back are the operator path; what this proof adds is
 * that the provenance survives a reboot as ACME, that it is never confused
 * with an operator upload or with the generated identity, and that an unknown
 * provenance is a corrupt record rather than a silent "generated".
 */
static void test_acme_issued_record(void)
{
    static struct appliance_production_certificate owner;
    static struct appliance_production_certificate reloaded;
    struct appliance_state_loader loader;
    struct block_device device;
    uint8_t fingerprint[SHA256_DIGEST_SIZE];
    uint8_t record[APPLIANCE_PRODUCTION_CERTIFICATE_RECORD_BYTES];
    uint32_t source;

    memset(fixture_volume, 0, sizeof(fixture_volume));
    CHECK(fixture_loader(&loader, &device) == true,
          "the fixture state layout is valid for the ACME record");

    memset(&owner, 0, sizeof(owner));
    CHECK(appliance_production_certificate_prepare(
              &owner, &loader, FIXTURE_NOW) ==
              APPLIANCE_PRODUCTION_CERTIFICATE_OK &&
              appliance_production_certificate_available(&owner),
          "the owner generates and persists its own record first");
    CHECK(owner.acme_issued == false && owner.imported == false,
          "the generated record carries no issued provenance");

    CHECK(appliance_production_certificate_adopt_acme(
              &owner, fixture_chained_certificate,
              sizeof(fixture_chained_certificate), fixture_chain_ca,
              sizeof(fixture_chain_ca), fixture_chained_key,
              sizeof(fixture_chained_key), fingerprint) ==
              APPLIANCE_PRODUCTION_CERTIFICATE_OK,
          "the verified ACME chain is written as record v2");
    CHECK(owner.acme_count == 1U && owner.import_count == 0U,
          "the owner counts the ACME record, not an operator import");

    memset(&reloaded, 0, sizeof(reloaded));
    CHECK(appliance_production_certificate_prepare(
              &reloaded, &loader, FIXTURE_NOW) ==
              APPLIANCE_PRODUCTION_CERTIFICATE_OK &&
              appliance_production_certificate_available(&reloaded),
          "the next boot loads the ACME record");
    CHECK(reloaded.acme_issued == true && reloaded.imported == false,
          "the loaded record keeps the ACME provenance");
    CHECK(reloaded.certificate_length ==
              sizeof(fixture_chained_certificate) &&
              memcmp(reloaded.certificate_der, fixture_chained_certificate,
                     sizeof(fixture_chained_certificate)) == 0,
          "the loaded ACME record serves the issued certificate byte for byte");
    CHECK(reloaded.chain_length == sizeof(fixture_chain_ca) &&
              memcmp(reloaded.chain_der, fixture_chain_ca,
                     sizeof(fixture_chain_ca)) == 0,
          "the loaded ACME record carries its issuing chain byte for byte");
    CHECK(reloaded.key_length == sizeof(fixture_chained_key) &&
              memcmp(reloaded.key_der, fixture_chained_key,
                     sizeof(fixture_chained_key)) == 0,
          "the loaded ACME record carries the key the CSR named");
    CHECK(reloaded.issuer_length != 0U,
          "the loaded ACME record publishes the issuer its DER names");

    /*
     * An unknown provenance is corrupt, not generated: the record must not be
     * served as the appliance's self-signed identity.
     */
    memset(record, 0, sizeof(record));
    CHECK(appliance_state_block_device_read(
              &owner.store, 0U, record, sizeof(record)) ==
              BLOCK_DEVICE_OK,
          "the written record reads back for the provenance proof");
    source = ((uint32_t)record[8] << 24U) | ((uint32_t)record[9] << 16U) |
             ((uint32_t)record[10] << 8U) | (uint32_t)record[11];
    CHECK(source == APPLIANCE_PRODUCTION_CERTIFICATE_SOURCE_ACME,
          "the record names the ACME provenance on disk");
    record[8] = 0U;
    record[9] = 0U;
    record[10] = 0U;
    record[11] = 3U;
    CHECK(appliance_state_block_device_write(
              &owner.store, 0U, record, sizeof(record)) ==
              BLOCK_DEVICE_OK &&
              appliance_state_block_device_flush(&owner.store) ==
              BLOCK_DEVICE_OK,
          "a foreign provenance is written over the record");
    memset(&reloaded, 0, sizeof(reloaded));
    CHECK(appliance_production_certificate_prepare(
              &reloaded, &loader, FIXTURE_NOW) ==
              APPLIANCE_PRODUCTION_CERTIFICATE_OK &&
              appliance_production_certificate_available(&reloaded),
          "a boot over a foreign provenance still serves something");
    CHECK(reloaded.acme_issued == false && reloaded.imported == false &&
              reloaded.certificate_length !=
                  sizeof(fixture_chained_certificate),
          "a foreign provenance is refused and regenerated, never relabelled");
}

/*
 * ITEM-508 increment 24: the ACME account key is the persisted identity key.
 * The proof binds the account key from the generated identity, requires the
 * public thumbprint to be identical after the record is reloaded by a fresh
 * owner (that is the reboot), requires a different volume to name a different
 * account, and refuses an unavailable identity, an out-of-bound key and a DER
 * the JWS module will not accept - with the record returning to "unbound"
 * after every refusal.
 */
static void test_account_key_from_identity(void)
{
    static struct appliance_production_certificate owner;
    static struct appliance_production_certificate reloaded;
    static struct appliance_production_certificate identity;
    struct appliance_state_loader loader;
    struct block_device device;
    struct appliance_acme_key first;
    struct appliance_acme_key second;
    struct appliance_acme_key refused;
    char owner_thumbprint[APPLIANCE_ACME_THUMBPRINT_MAX + 1U];
    char reloaded_thumbprint[APPLIANCE_ACME_THUMBPRINT_MAX + 1U];
    char other_thumbprint[APPLIANCE_ACME_THUMBPRINT_MAX + 1U];
    const char *record;
    size_t length;

    memset(fixture_volume, 0, sizeof(fixture_volume));
    CHECK(fixture_loader(&loader, &device) == true,
          "the fixture state layout is valid for the account key");

    memset(&owner, 0, sizeof(owner));
    CHECK(appliance_production_certificate_prepare(
              &owner, &loader, FIXTURE_NOW) ==
              APPLIANCE_PRODUCTION_CERTIFICATE_OK &&
              appliance_production_certificate_available(&owner),
          "the persisted identity is prepared for the account key");
    CHECK(appliance_acme_account_bind(&owner, &first) ==
              APPLIANCE_ACME_ACCOUNT_OK &&
              appliance_acme_key_thumbprint(&first) != NULL,
          "the account key binds from the persisted identity");
    length = strlen(appliance_acme_key_thumbprint(&first));
    CHECK(length == APPLIANCE_ACME_THUMBPRINT_MAX,
          "the account thumbprint has the RFC 7638 length");
    memcpy(owner_thumbprint, appliance_acme_key_thumbprint(&first),
           length + 1U);
    record = appliance_acme_account_record();
    CHECK(strncmp(record, "thumbprint=", 11U) == 0 &&
              strcmp(&record[11], owner_thumbprint) == 0,
          "the bound record names the public thumbprint and nothing else");

    /*
     * The next boot loads the record; the key bytes are the same, so the
     * account the CA knows must be the same.
     */
    memset(&reloaded, 0, sizeof(reloaded));
    CHECK(appliance_production_certificate_prepare(
              &reloaded, &loader, FIXTURE_NOW) ==
              APPLIANCE_PRODUCTION_CERTIFICATE_OK &&
              appliance_production_certificate_available(&reloaded),
          "the next boot loads the persisted identity");
    CHECK(appliance_acme_account_bind(&reloaded, &second) ==
              APPLIANCE_ACME_ACCOUNT_OK,
          "the account key binds again from the loaded identity");
    length = strlen(appliance_acme_key_thumbprint(&second));
    memcpy(reloaded_thumbprint, appliance_acme_key_thumbprint(&second),
           length + 1U);
    CHECK(strcmp(reloaded_thumbprint, owner_thumbprint) == 0,
          "the account thumbprint survives the reboot byte for byte");

    /*
     * A different key is a different account: the binding tracks the key
     * bytes, it does not fabricate one stable identity.  (A fresh volume is
     * not a different key here: the fixture generator is deterministic, so
     * this proof uses the second fixture key instead.)
     */
    memset(&identity, 0, sizeof(identity));
    identity.initialized = true;
    identity.available = true;
    identity.key_length = sizeof(fixture_chained_key);
    memcpy(identity.key_der, fixture_chained_key,
           sizeof(fixture_chained_key));
    CHECK(appliance_acme_account_bind(&identity, &second) ==
              APPLIANCE_ACME_ACCOUNT_OK,
          "a second EC P-256 key binds its own account");
    length = strlen(appliance_acme_key_thumbprint(&second));
    memcpy(other_thumbprint, appliance_acme_key_thumbprint(&second),
           length + 1U);
    CHECK(strcmp(other_thumbprint, owner_thumbprint) != 0,
          "a different key names a different account, never the old one");

    /* Refusals leave the record unbound and write nothing. */
    CHECK(appliance_acme_account_bind(NULL, &refused) ==
              APPLIANCE_ACME_ACCOUNT_NULL &&
              appliance_acme_account_bind(&owner, NULL) ==
                  APPLIANCE_ACME_ACCOUNT_NULL,
          "a null identity or key is refused");
    memset(&identity, 0, sizeof(identity));
    identity.initialized = true;
    CHECK(appliance_acme_account_bind(&identity, &refused) ==
              APPLIANCE_ACME_ACCOUNT_IDENTITY_UNAVAILABLE,
          "an unavailable identity is refused by name");
    identity.available = true;
    identity.key_length = APPLIANCE_PRODUCTION_CERTIFICATE_KEY_DER_MAX + 1U;
    CHECK(appliance_acme_account_bind(&identity, &refused) ==
              APPLIANCE_ACME_ACCOUNT_KEY_UNUSABLE,
          "a key outside the record bound is refused before parsing");
    identity.key_length = 32U;
    memset(identity.key_der, 0x5AU, sizeof(identity.key_der));
    CHECK(appliance_acme_account_bind(&identity, &refused) ==
              APPLIANCE_ACME_ACCOUNT_KEY_REFUSED,
          "a DER that is not an EC P-256 key is refused");
    CHECK(strcmp(appliance_acme_account_record(), "unbound") == 0,
          "a refused bind publishes no account record");
}

/*
 * ITEM-508 increment 26: the ACME service ordinary boot owns.  The proof
 * drives the service over the same fixture volume: an unprepared identity
 * leaves it waiting and a later poll binds, the bound thumbprint survives the
 * reload of the record, a refused key is named and never half-binds, and the
 * renewal policy the service owns moves from not-needed to due while an
 * unvalidated clock refuses the decision.
 */
static void test_acme_service(void)
{
    static struct appliance_production_certificate owner;
    static struct appliance_production_certificate reloaded;
    static struct appliance_production_certificate identity;
    static struct appliance_production_acme_service service;
    static struct appliance_production_acme_service second;
    static struct appliance_production_acme_service uninitialized;
    struct appliance_state_loader loader;
    struct block_device device;
    const char *record;
    const char *thumbprint;
    char owner_thumbprint[APPLIANCE_ACME_THUMBPRINT_MAX + 1U];
    char reloaded_thumbprint[APPLIANCE_ACME_THUMBPRINT_MAX + 1U];

    memset(fixture_volume, 0, sizeof(fixture_volume));
    CHECK(fixture_loader(&loader, &device) == true,
          "the fixture state layout is valid for the ACME service");

    /* An unprepared identity waits; the next poll retries. */
    memset(&identity, 0, sizeof(identity));
    identity.initialized = true;
    memset(&service, 0, sizeof(service));
    CHECK(appliance_production_acme_service_init(&service, &identity) ==
              APPLIANCE_PRODUCTION_ACME_SERVICE_OK &&
              appliance_production_acme_service_state(&service) ==
                  APPLIANCE_PRODUCTION_ACME_SERVICE_STATE_AWAITING_IDENTITY &&
              !appliance_production_acme_service_account_bound(&service),
          "an unprepared identity leaves the ACME service waiting");
    CHECK(strcmp(appliance_production_acme_service_record(&service),
                 "[ACME-SERVICE] v=1 state=AWAITING_IDENTITY "
                 "reason=identity_unavailable") == 0,
          "the waiting service publishes the named reason");
    CHECK(appliance_production_acme_service_poll(
              &service, &identity, FIXTURE_NOW) ==
              APPLIANCE_PRODUCTION_ACME_SERVICE_OK &&
              appliance_production_acme_service_state(&service) ==
                  APPLIANCE_PRODUCTION_ACME_SERVICE_STATE_AWAITING_IDENTITY,
          "the poll retries and still waits for the identity");

    /* A prepared identity binds; the record names the public thumbprint. */
    memset(&owner, 0, sizeof(owner));
    CHECK(appliance_production_certificate_prepare(
              &owner, &loader, FIXTURE_NOW) ==
              APPLIANCE_PRODUCTION_CERTIFICATE_OK,
          "the identity is prepared for the ACME service");
    CHECK(appliance_production_acme_service_poll(
              &service, &owner, FIXTURE_NOW) ==
              APPLIANCE_PRODUCTION_ACME_SERVICE_OK &&
              appliance_production_acme_service_state(&service) ==
                  APPLIANCE_PRODUCTION_ACME_SERVICE_STATE_BOUND &&
              appliance_production_acme_service_account_bound(&service),
          "the service binds the account once the identity exists");
    record = appliance_production_acme_service_record(&service);
    thumbprint = strstr(record, "thumbprint=");
    CHECK(strncmp(record, "[ACME-SERVICE] v=1 state=BOUND account=bound ", 45U)
              == 0,
          "the bound service publishes the bound state");
    CHECK(thumbprint != NULL &&
              thumbprint[11U + APPLIANCE_ACME_THUMBPRINT_MAX] == ' ',
          "the bound service record names a bounded thumbprint");
    memcpy(owner_thumbprint, thumbprint + 11U,
           APPLIANCE_ACME_THUMBPRINT_MAX);
    owner_thumbprint[APPLIANCE_ACME_THUMBPRINT_MAX] = '\0';
    CHECK(strstr(record, "renewal=no-certificate") != NULL,
          "a boot with no verified expiry decides no-certificate");

    /* The next boot loads the same key, so the same account must come back. */
    memset(&reloaded, 0, sizeof(reloaded));
    CHECK(appliance_production_certificate_prepare(
              &reloaded, &loader, FIXTURE_NOW) ==
              APPLIANCE_PRODUCTION_CERTIFICATE_OK,
          "the next boot loads the persisted identity");
    memset(&second, 0, sizeof(second));
    CHECK(appliance_production_acme_service_init(&second, &reloaded) ==
              APPLIANCE_PRODUCTION_ACME_SERVICE_OK &&
              appliance_production_acme_service_account_bound(&second),
          "the service binds the loaded identity");
    record = appliance_production_acme_service_record(&second);
    thumbprint = strstr(record, "thumbprint=");
    memcpy(reloaded_thumbprint, thumbprint + 11U,
           APPLIANCE_ACME_THUMBPRINT_MAX);
    reloaded_thumbprint[APPLIANCE_ACME_THUMBPRINT_MAX] = '\0';
    CHECK(strcmp(reloaded_thumbprint, owner_thumbprint) == 0,
          "the account thumbprint survives the reboot byte for byte");

    /* The policy the service owns: fresh, due, and an unvalidated clock. */
    CHECK(appliance_production_acme_service_certificate_obtained(
              &service, FIXTURE_NOW,
              FIXTURE_NOW + UINT32_C(90) * UINT32_C(86400)) ==
              APPLIANCE_ACME_RENEWAL_OK,
          "the verified expiry enters the service's renewal policy");
    CHECK(appliance_production_acme_service_renewal_decision(&service) ==
              APPLIANCE_ACME_RENEWAL_DECISION_NOT_NEEDED &&
              strstr(appliance_production_acme_service_record(&service),
                     "renewal=not-needed") != NULL,
          "a fresh certificate is not renewed");
    CHECK(appliance_production_acme_service_certificate_obtained(
              &service, FIXTURE_NOW,
              FIXTURE_NOW + UINT32_C(10) * UINT32_C(86400)) ==
              APPLIANCE_ACME_RENEWAL_OK &&
              appliance_production_acme_service_renewal_decision(&service) ==
                  APPLIANCE_ACME_RENEWAL_DECISION_DUE,
          "a certificate inside the thirty-day window is due");
    CHECK(appliance_production_acme_service_poll(&service, &owner, 0U) ==
              APPLIANCE_PRODUCTION_ACME_SERVICE_OK &&
              appliance_production_acme_service_renewal_decision(&service) ==
                  APPLIANCE_ACME_RENEWAL_DECISION_BAD_CLOCK,
          "an unvalidated clock refuses the renewal decision");

    /* A key the JWS module rejects is a named refusal, never half a bind. */
    memset(&identity, 0, sizeof(identity));
    identity.initialized = true;
    identity.available = true;
    identity.key_length = 32U;
    memset(identity.key_der, 0x5AU, sizeof(identity.key_der));
    memset(&second, 0, sizeof(second));
    CHECK(appliance_production_acme_service_init(&second, &identity) ==
              APPLIANCE_PRODUCTION_ACME_SERVICE_OK &&
              appliance_production_acme_service_state(&second) ==
                  APPLIANCE_PRODUCTION_ACME_SERVICE_STATE_REFUSED &&
              appliance_production_acme_service_account_status(&second) ==
                  APPLIANCE_ACME_ACCOUNT_KEY_REFUSED &&
              !appliance_production_acme_service_account_bound(&second),
          "a key the JWS module rejects is a named refusal");
    CHECK(strcmp(appliance_production_acme_service_record(&second),
                 "[ACME-SERVICE] v=1 state=REFUSED reason=KEY_REFUSED") == 0,
          "the refused service publishes the account refusal");

    /* Misuse is named: null, double init and poll before init. */
    CHECK(appliance_production_acme_service_init(NULL, &owner) ==
              APPLIANCE_PRODUCTION_ACME_SERVICE_NULL &&
              appliance_production_acme_service_poll(
                  NULL, &owner, FIXTURE_NOW) ==
                  APPLIANCE_PRODUCTION_ACME_SERVICE_NULL,
          "a null service is refused");
    CHECK(appliance_production_acme_service_init(&second, &owner) ==
              APPLIANCE_PRODUCTION_ACME_SERVICE_ALREADY_INITIALIZED,
          "initializing twice is refused");
    memset(&uninitialized, 0, sizeof(uninitialized));
    CHECK(appliance_production_acme_service_poll(
              &uninitialized, &owner, FIXTURE_NOW) ==
              APPLIANCE_PRODUCTION_ACME_SERVICE_BAD_STATE &&
              strcmp(appliance_production_acme_service_record(
                         &uninitialized), "uninitialized") == 0,
          "an uninitialized service refuses to poll and publishes nothing");
}

int main(void)
{
    if (!hfoc_mbedtls_alloc_init()) {
        fprintf(stderr, "the mbedTLS allocator did not initialise\n");
        return 1;
    }
    test_form_and_stage();
    test_commit_and_record();
    test_acme_issued_record();
    test_account_key_from_identity();
    test_acme_service();

    if (failures != 0) {
        fprintf(stderr,
                "Appliance certificate upload host tests: %d FAILED\n",
                failures);
        return 1;
    }
    printf("Appliance certificate upload host tests: ALL OK\n");
    return 0;
}

#endif
