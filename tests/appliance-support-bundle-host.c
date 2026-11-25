/*
 * ITEM-378 focused proof: the signed diagnostic bundle.
 *
 * The fixture builds a real bundle through the ITEM-168 exporter, re-applies
 * the ITEM-169 diagnostic-privacy deny list, seals the canonical binding with
 * a real HMAC-SHA-256 through the caller-bound MAC boundary and proves that
 * every tamper path is refused.
 */
#include "appliance_support_bundle.h"

#include "appliance_state_crypto.h"
#include "config_kv.h"
#include "sha256.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifdef HFOC_APPLIANCE_SUPPORT_BUNDLE_HOST_TEST

#define TEST_CONFIG_CAPACITY 6U
#define TEST_AUDIT_COUNT 2U
#define TEST_STREAM_CAPACITY 16384U
#define TEST_MAX_UNIT_SIZE 9088U
#define TEST_RENDER_CAPACITY 512U

static int failures;
static unsigned int alloc_attempts;
static unsigned int free_attempts;

static struct metrics_snapshot test_metrics;
static struct appliance_status_snapshot test_status;
static struct health_check_snapshot test_health;
static struct audit_record test_audit[TEST_AUDIT_COUNT];
static struct config_kv test_config;
static struct config_kv_entry test_entries[TEST_CONFIG_CAPACITY];
static struct config_kv sparse_config;
static struct config_kv_entry sparse_entries[TEST_CONFIG_CAPACITY];
static struct support_bundle_manifest_source test_source;
static struct support_bundle_manifest_source sparse_source;
static unsigned char test_stream[TEST_STREAM_CAPACITY];
static size_t test_stream_length;

static const uint8_t test_seal_key[32] = {
    0x11U, 0x22U, 0x33U, 0x44U, 0x55U, 0x66U, 0x77U, 0x88U,
    0x91U, 0xA2U, 0xB3U, 0xC4U, 0xD5U, 0xE6U, 0xF7U, 0x08U,
    0x19U, 0x2AU, 0x3BU, 0x4CU, 0x5DU, 0x6EU, 0x7FU, 0x80U,
    0x8AU, 0x9BU, 0xACU, 0xBDU, 0xCEU, 0xDFU, 0xE0U, 0xF1U
};

#define CHECK(cond, msg) do { \
    if (!(cond)) { \
        fprintf(stderr, "FAIL: %s\n", msg); \
        ++failures; \
    } else { \
        printf("PASS: %s\n", msg); \
    } \
} while (0)

void *kmalloc(size_t size) {
    (void)size;
    ++alloc_attempts;
    return NULL;
}

bool kfree(void *ptr) {
    (void)ptr;
    ++free_attempts;
    return false;
}

/* The state-crypto Mbed TLS profile declares MBEDTLS_PLATFORM_ZEROIZE_ALT and
 * expects exactly one definition in the embedding image; the production kernel
 * takes the ITEM-372 one and this proof supplies its own. */
void mbedtls_platform_zeroize(void *buffer, size_t length)
{
    volatile unsigned char *bytes = (volatile unsigned char *)buffer;

    if (bytes == NULL) {
        return;
    }
    while (length > 0U) {
        *bytes = 0U;
        ++bytes;
        --length;
    }
}

static enum appliance_support_bundle_status test_mac(
    void *context,
    const uint8_t *key,
    size_t key_length,
    const uint8_t *data,
    size_t data_length,
    uint8_t mac[APPLIANCE_SUPPORT_BUNDLE_MAC_SIZE])
{
    (void)context;
    if (appliance_state_crypto_hmac_sha256(
            key, key_length, data, data_length, mac) !=
        APPLIANCE_STATE_CRYPTO_OK) {
        return APPLIANCE_SUPPORT_BUNDLE_MAC_UNAVAILABLE;
    }
    return APPLIANCE_SUPPORT_BUNDLE_OK;
}

static uint32_t load_u32(const unsigned char *bytes)
{
    return (uint32_t)bytes[0] |
           ((uint32_t)bytes[1] << 8U) |
           ((uint32_t)bytes[2] << 16U) |
           ((uint32_t)bytes[3] << 24U);
}

static void init_metrics(struct metrics_snapshot *snapshot)
{
    memset(snapshot, 0, sizeof(*snapshot));
    snapshot->version = METRICS_VERSION;
    snapshot->generation = 3U;
    snapshot->paths.health.version = METRICS_VERSION;
    snapshot->paths.health.check_count = 1U;
    snapshot->paths.health.pass_count = 1U;
    snapshot->paths.state.version = METRICS_VERSION;
    snapshot->paths.state.schema_version = 2U;
    snapshot->paths.state.active_generation = 1U;
    snapshot->paths.service.version = METRICS_VERSION;
    snapshot->paths.service.service_count = 1U;
    snapshot->paths.service.ready_count = 1U;
    snapshot->paths.update.version = METRICS_VERSION;
    snapshot->paths.update.slot_count = 1U;
    snapshot->paths.update.known_good_slot = 1U;
    snapshot->paths.update.candidate_active = true;
    snapshot->paths.update.accepted = true;
}

static void init_status(struct appliance_status_snapshot *snapshot)
{
    memset(snapshot, 0, sizeof(*snapshot));
    snapshot->version = APPLIANCE_STATUS_VERSION;
    snapshot->published = true;
    snapshot->state.version = APPLIANCE_STATUS_VERSION;
    snapshot->state.generation = 5U;
    snapshot->state.uptime_ticks = 123U;
    snapshot->state.config_revision = 7U;
    snapshot->state.audit_sequence = 99U;
    snapshot->state.active_process_count = 2U;
    snapshot->storage.version = APPLIANCE_STATUS_VERSION;
    snapshot->storage.journal_generation = 4U;
    snapshot->storage.backup_generation = 6U;
    snapshot->storage.schema_version = 1U;
    snapshot->storage.state = APPLIANCE_STORAGE_OK;
    snapshot->health.version = APPLIANCE_STATUS_VERSION;
    snapshot->health.state = APPLIANCE_HEALTH_OK;
    snapshot->health.ready_count = 1U;
    snapshot->health.pending_count = 1U;
    snapshot->service_count = 1U;
    snapshot->services[0].id = 1U;
    snapshot->services[0].state = APPLIANCE_SERVICE_READY;
    snapshot->services[0].health = APPLIANCE_SERVICE_HEALTH_READY;
}

static void init_health(struct health_check_snapshot *snapshot)
{
    memset(snapshot, 0, sizeof(*snapshot));
    snapshot->version = HEALTH_CHECK_VERSION;
    snapshot->generation = 2U;
    snapshot->descriptor_count = 1U;
    snapshot->entries[0].descriptor.version = HEALTH_CHECK_VERSION;
    snapshot->entries[0].descriptor.check_id = 1U;
    snapshot->entries[0].descriptor.severity =
        HEALTH_CHECK_SEVERITY_INFO;
    snapshot->entries[0].descriptor.cadence_ms = 5000U;
    snapshot->entries[0].result = HEALTH_CHECK_RESULT_PASS;
}

static void init_audit(struct audit_record *records)
{
    memset(records, 0, sizeof(records[0]) * TEST_AUDIT_COUNT);
    records[0].sequence = 10U;
    records[0].type = AUDIT_RECORD_PROCESS;
    records[0].operation = 1U;
    records[0].pid = 1U;
    records[0].generation = 2U;
    records[0].detail = 5U;
    records[1].sequence = 11U;
    records[1].type = AUDIT_RECORD_SYSCALL;
    records[1].operation = 2U;
    records[1].result = 1U;
    records[1].pid = 2U;
    records[1].domain = 1U;
    records[1].generation = 3U;
    records[1].detail = 9U;
}

static void populate_config(struct config_kv *kv,
                            struct config_kv_entry *entries)
{
    bool replaced;

    memset(entries, 0, sizeof(entries[0]) * TEST_CONFIG_CAPACITY);
    CHECK(config_kv_init(kv, entries, TEST_CONFIG_CAPACITY) ==
              CONFIG_KV_OK,
          "support bundle fixture initializes config checkpoint");
    CHECK(config_kv_put(kv, CONFIG_KV_KIND_CONFIG,
                        "alpha", 5U, "one", 3U, &replaced) ==
              CONFIG_KV_OK &&
          config_kv_put(kv, CONFIG_KV_KIND_CONFIG,
                        "beta", 4U, "two", 3U, &replaced) ==
              CONFIG_KV_OK &&
          config_kv_put(kv, CONFIG_KV_KIND_CONFIG,
                        "secret", 6U, "s3cr3t", 6U, &replaced) ==
              CONFIG_KV_OK &&
          config_kv_put(kv, CONFIG_KV_KIND_CONFIG,
                        "password", 8U, "hunter2", 7U, &replaced) ==
              CONFIG_KV_OK &&
          config_kv_put(kv, CONFIG_KV_KIND_AUDIT,
                        "alpha", 5U, "audit-one", 9U, &replaced) ==
              CONFIG_KV_OK,
          "support bundle fixture populates config checkpoint");
}

static void init_sources(void)
{
    init_metrics(&test_metrics);
    init_status(&test_status);
    init_health(&test_health);
    init_audit(test_audit);
    populate_config(&test_config, test_entries);

    memset(&test_source, 0, sizeof(test_source));
    test_source.version = SUPPORT_BUNDLE_MANIFEST_VERSION;
    test_source.metrics = &test_metrics;
    test_source.status = &test_status;
    test_source.health = &test_health;
    test_source.audit_records = test_audit;
    test_source.audit_count = TEST_AUDIT_COUNT;
    test_source.config = &test_config;

    memset(&sparse_source, 0, sizeof(sparse_source));
    sparse_source.version = SUPPORT_BUNDLE_MANIFEST_VERSION;
    sparse_source.metrics = &test_metrics;
    sparse_source.status = &test_status;
    sparse_source.health = &test_health;
    sparse_source.audit_records = test_audit;
    sparse_source.audit_count = TEST_AUDIT_COUNT;
    sparse_source.config = &sparse_config;
}

static void drain_exporter(unsigned char *stream,
                           size_t stream_capacity,
                           size_t *stream_length_out)
{
    struct support_bundle_exporter exporter;
    unsigned char unit[TEST_MAX_UNIT_SIZE];
    size_t offset = 0U;

    CHECK(support_bundle_exporter_init(&exporter, &test_source) ==
              SUPPORT_BUNDLE_EXPORTER_OK,
          "support bundle exporter admits the fixture source");
    for (;;) {
        enum support_bundle_exporter_status status;
        size_t unit_length = 0U;

        status = support_bundle_exporter_next(
            &exporter, unit, sizeof(unit), &unit_length);
        if (status == SUPPORT_BUNDLE_EXPORTER_DONE) {
            break;
        }
        CHECK(status == SUPPORT_BUNDLE_EXPORTER_OK,
              "support bundle exporter emits an atomic unit");
        CHECK(offset + unit_length <= stream_capacity,
              "support bundle stream stays within fixture capacity");
        memcpy(stream + offset, unit, unit_length);
        offset += unit_length;
    }
    CHECK(support_bundle_exporter_close(&exporter) ==
              SUPPORT_BUNDLE_EXPORTER_OK,
          "support bundle exporter closes cleanly");
    *stream_length_out = offset;
}

static void build_fixture_stream(void)
{
    drain_exporter(test_stream, sizeof(test_stream),
                   &test_stream_length);
    CHECK(test_stream_length >=
              SUPPORT_BUNDLE_EXPORTER_HEADER_SIZE +
                  SUPPORT_BUNDLE_MANIFEST_SIZE +
                  SUPPORT_BUNDLE_EXPORTER_DIGEST_SIZE,
          "support bundle stream carries header, manifest and digest");
}

static void test_redaction_report(void)
{
    struct appliance_support_bundle_redaction report;

    memset(&report, 0, sizeof(report));
    CHECK(appliance_support_bundle_redaction_eval(
              &test_source, &report) == APPLIANCE_SUPPORT_BUNDLE_OK,
          "redaction evaluation accepts the fixture source");
    CHECK(report.version == APPLIANCE_SUPPORT_BUNDLE_VERSION,
          "redaction report carries the module version");
    CHECK(report.declared_keys ==
              DIAGNOSTIC_PRIVACY_SECRET_KEY_COUNT,
          "redaction report declares the ITEM-169 deny-list size");
    CHECK(strcmp(report.policy_id,
                 SUPPORT_BUNDLE_MANIFEST_SECRET_POLICY_ID) == 0,
          "redaction report names the declared policy");
    CHECK(report.policy_applied,
          "redaction report records that the policy was applied");
    CHECK(report.config_entries == 4U && report.included == 2U &&
              report.withheld == 2U && report.leaked == 0U,
          "redaction report counts included, withheld and leaked "
          "entries exactly");

    memset(&report, 0, sizeof(report));
    CHECK(appliance_support_bundle_redaction_eval(
              NULL, &report) == APPLIANCE_SUPPORT_BUNDLE_NULL,
          "redaction evaluation refuses a null source");
    CHECK(appliance_support_bundle_redaction_eval(
              &test_source, NULL) == APPLIANCE_SUPPORT_BUNDLE_NULL,
          "redaction evaluation refuses a null report");
}

static void test_binding(void)
{
    struct appliance_support_bundle_redaction report;
    struct appliance_support_bundle_binding binding;
    unsigned char manifest_digest[SHA256_DIGEST_SIZE];
    unsigned char terminal_digest[SHA256_DIGEST_SIZE];
    unsigned char encoding[APPLIANCE_SUPPORT_BUNDLE_ENCODING_SIZE];
    unsigned char encoding_again[APPLIANCE_SUPPORT_BUNDLE_ENCODING_SIZE];
    unsigned char digest[SHA256_DIGEST_SIZE];
    size_t encoding_length = 0U;
    size_t again_length = 0U;

    (void)appliance_support_bundle_redaction_eval(&test_source, &report);
    memset(&binding, 0, sizeof(binding));
    CHECK(appliance_support_bundle_bind(
              test_stream, test_stream_length, &report,
              &binding) == APPLIANCE_SUPPORT_BUNDLE_OK,
          "binding accepts the exported fixture bundle");
    CHECK(binding.version == APPLIANCE_SUPPORT_BUNDLE_VERSION,
          "binding carries the module version");
    CHECK(binding.exporter_version == SUPPORT_BUNDLE_EXPORTER_VERSION &&
              binding.manifest_version ==
                  SUPPORT_BUNDLE_MANIFEST_VERSION,
          "binding takes the exporter and manifest versions from the "
          "bytes");
    CHECK(binding.entry_count == SUPPORT_BUNDLE_MANIFEST_ENTRY_MAX,
          "binding takes the entry count from the bytes");
    CHECK(binding.bundle_bytes == (uint64_t)test_stream_length,
          "binding commits to the exact exported length");
    CHECK(binding.payload_bytes ==
              load_u32(test_stream +
                       SUPPORT_BUNDLE_EXPORTER_HEADER_PAYLOAD_BYTES_OFFSET),
          "binding takes the payload length from the header");

    (void)sha256_compute(
        test_stream + SUPPORT_BUNDLE_EXPORTER_MANIFEST_OFFSET,
        SUPPORT_BUNDLE_MANIFEST_SIZE, manifest_digest);
    (void)sha256_compute(
        test_stream,
        test_stream_length - SUPPORT_BUNDLE_EXPORTER_DIGEST_SIZE,
        terminal_digest);
    CHECK(memcmp(binding.manifest_digest, manifest_digest,
                 SHA256_DIGEST_SIZE) == 0,
          "binding re-derives the embedded manifest digest");
    CHECK(memcmp(binding.bundle_digest, terminal_digest,
                 SHA256_DIGEST_SIZE) == 0,
          "binding re-derives the exporter terminal digest");
    CHECK(memcmp(test_stream + test_stream_length -
                     SUPPORT_BUNDLE_EXPORTER_DIGEST_SIZE,
                 terminal_digest, SHA256_DIGEST_SIZE) == 0,
          "binding proves the terminal digest covers the whole stream");

    CHECK(appliance_support_bundle_encode(
              &binding, encoding, sizeof(encoding),
              &encoding_length) == APPLIANCE_SUPPORT_BUNDLE_OK,
          "binding encodes into the declared fixed width");
    CHECK(encoding_length == APPLIANCE_SUPPORT_BUNDLE_ENCODING_SIZE,
          "binding encoding has the declared size");
    CHECK(appliance_support_bundle_encode(
              &binding, encoding_again, sizeof(encoding_again),
              &again_length) == APPLIANCE_SUPPORT_BUNDLE_OK &&
              again_length == encoding_length &&
              memcmp(encoding, encoding_again, encoding_length) == 0,
          "binding encoding is deterministic");
    CHECK(appliance_support_bundle_digest(&binding, digest) ==
              APPLIANCE_SUPPORT_BUNDLE_OK,
          "binding digest is computed");
    {
        unsigned char expected[SHA256_DIGEST_SIZE];

        (void)sha256_compute(encoding, encoding_length, expected);
        CHECK(memcmp(digest, expected, SHA256_DIGEST_SIZE) == 0,
              "binding digest is SHA-256 over the canonical encoding");
    }

    {
        unsigned char short_buffer[
            APPLIANCE_SUPPORT_BUNDLE_ENCODING_SIZE - 1U];

        CHECK(appliance_support_bundle_encode(
                  &binding, short_buffer, sizeof(short_buffer),
                  &again_length) ==
                  APPLIANCE_SUPPORT_BUNDLE_BUFFER_TOO_SMALL,
              "binding encoding refuses a short buffer");
    }

    /*
     * A report whose counts disagree with the manifest's own claim cannot
     * describe this bundle.
     */
    {
        struct support_bundle_manifest_source empty_source;
        struct appliance_support_bundle_redaction empty_report;
        bool replaced;

        memset(&empty_source, 0, sizeof(empty_source));
        memset(sparse_entries, 0, sizeof(sparse_entries));
        CHECK(config_kv_init(&sparse_config, sparse_entries,
                             TEST_CONFIG_CAPACITY) == CONFIG_KV_OK &&
                  config_kv_put(&sparse_config, CONFIG_KV_KIND_CONFIG,
                                "alpha", 5U, "one", 3U,
                                &replaced) == CONFIG_KV_OK,
              "sparse fixture config is available");
        empty_source = test_source;
        empty_source.config = &sparse_config;
        memset(&empty_report, 0, sizeof(empty_report));
        (void)appliance_support_bundle_redaction_eval(
            &empty_source, &empty_report);
        CHECK(empty_report.included == 1U &&
                  empty_report.withheld == 0U,
              "sparse report disagrees with the sealed bundle claim");
        CHECK(appliance_support_bundle_bind(
                  test_stream, test_stream_length, &empty_report,
                  &binding) ==
                  APPLIANCE_SUPPORT_BUNDLE_REDACTION_FAILED,
              "binding refuses a report that contradicts the manifest");
    }

    /* The manifest claim itself is not trusted: a leak refuses the bind. */
    {
        struct appliance_support_bundle_redaction leaked =
            report;

        (void)appliance_support_bundle_redaction_eval(&test_source,
                                                      &report);
        leaked = report;
        leaked.leaked = 1U;
        CHECK(appliance_support_bundle_bind(
                  test_stream, test_stream_length, &leaked,
                  &binding) ==
                  APPLIANCE_SUPPORT_BUNDLE_REDACTION_FAILED,
              "binding refuses a report with a leaked secret entry");
        leaked = report;
        leaked.policy_applied = false;
        CHECK(appliance_support_bundle_bind(
                  test_stream, test_stream_length, &leaked,
                  &binding) ==
                  APPLIANCE_SUPPORT_BUNDLE_REDACTION_FAILED,
              "binding refuses a report without an applied policy");
    }

    CHECK(appliance_support_bundle_bind(
              test_stream + 1U, test_stream_length - 1U, &report,
              &binding) == APPLIANCE_SUPPORT_BUNDLE_BAD_BINDING,
          "binding refuses bytes without the exporter magic");
    CHECK(appliance_support_bundle_bind(
              test_stream, SUPPORT_BUNDLE_EXPORTER_HEADER_SIZE,
              &report, &binding) == APPLIANCE_SUPPORT_BUNDLE_BAD_BINDING,
          "binding refuses a truncated bundle");
    {
        unsigned char tampered[TEST_STREAM_CAPACITY];

        memcpy(tampered, test_stream, test_stream_length);
        tampered[SUPPORT_BUNDLE_EXPORTER_MANIFEST_OFFSET + 100U] ^= 0x5AU;
        CHECK(appliance_support_bundle_bind(
                  tampered, test_stream_length, &report,
                  &binding) == APPLIANCE_SUPPORT_BUNDLE_BAD_BINDING,
              "binding detects a rewritten manifest byte");
        memcpy(tampered, test_stream, test_stream_length);
        tampered[test_stream_length - 1U] ^= 0x01U;
        CHECK(appliance_support_bundle_bind(
                  tampered, test_stream_length, &report,
                  &binding) == APPLIANCE_SUPPORT_BUNDLE_BAD_BINDING,
              "binding detects a rewritten terminal digest");
    }
}

static void test_seal_and_verify(void)
{
    struct appliance_support_bundle bundle;
    struct appliance_support_bundle_redaction report;

    (void)appliance_support_bundle_redaction_eval(&test_source, &report);

    memset(&bundle, 0, sizeof(bundle));
    CHECK(appliance_support_bundle_seal(
              &bundle, test_stream, test_stream_length, &report,
              test_mac, NULL, NULL, 0U) ==
              APPLIANCE_SUPPORT_BUNDLE_NO_SEAL_KEY,
          "seal without a key refuses to fabricate a signature");
    CHECK(appliance_support_bundle_state(&bundle) ==
              APPLIANCE_SUPPORT_BUNDLE_STATE_UNSIGNED,
          "seal without a key publishes UNSIGNED");
    CHECK(bundle.scheme == APPLIANCE_SUPPORT_BUNDLE_SCHEME_NONE,
          "an unsigned bundle declares no scheme");
    CHECK(!appliance_support_bundle_is_sealed(&bundle),
          "an unsigned bundle is not sealed");
    CHECK(appliance_support_bundle_withheld(&bundle) == 2U,
          "an unsigned bundle still carries the redaction report");
    {
        unsigned char zero_mac[APPLIANCE_SUPPORT_BUNDLE_MAC_SIZE];
        size_t index;
        bool all_zero = true;

        memset(zero_mac, 0, sizeof(zero_mac));
        for (index = 0U; index < sizeof(zero_mac); ++index) {
            if (bundle.mac[index] != 0U) {
                all_zero = false;
            }
        }
        CHECK(all_zero, "an unsigned bundle publishes no MAC bytes");
    }
    CHECK(appliance_support_bundle_seal(
              &bundle, test_stream, test_stream_length, &report,
              NULL, NULL, test_seal_key, sizeof(test_seal_key)) ==
              APPLIANCE_SUPPORT_BUNDLE_MAC_UNAVAILABLE,
          "seal without a MAC provider reports MAC_UNAVAILABLE");
    CHECK(appliance_support_bundle_state(&bundle) ==
              APPLIANCE_SUPPORT_BUNDLE_STATE_UNAVAILABLE,
          "seal without a MAC provider publishes UNAVAILABLE");

    memset(&bundle, 0, sizeof(bundle));
    CHECK(appliance_support_bundle_seal(
              &bundle, test_stream, test_stream_length, &report,
              test_mac, NULL, test_seal_key,
              sizeof(test_seal_key)) == APPLIANCE_SUPPORT_BUNDLE_OK,
          "seal with a bound key succeeds");
    CHECK(appliance_support_bundle_state(&bundle) ==
              APPLIANCE_SUPPORT_BUNDLE_STATE_SEALED,
          "seal with a bound key publishes SEALED");
    CHECK(bundle.scheme ==
              APPLIANCE_SUPPORT_BUNDLE_SCHEME_HMAC_SHA256,
          "seal declares the HMAC-SHA-256 scheme");
    CHECK(appliance_support_bundle_is_sealed(&bundle),
          "a sealed bundle reports sealed");
    CHECK(bundle.sealed_count == 1U && bundle.unsigned_count == 0U &&
              bundle.refused_count == 0U,
          "seal counts the sealed outcome once");

    CHECK(appliance_support_bundle_verify(
              &bundle, test_stream, test_stream_length, test_mac,
              NULL, test_seal_key, sizeof(test_seal_key)) ==
              APPLIANCE_SUPPORT_BUNDLE_OK,
          "verify accepts the sealed bundle with the same key");
    CHECK(bundle.verified,
          "verify records the verified outcome");

    CHECK(appliance_support_bundle_verify(
              &bundle, test_stream, test_stream_length, test_mac,
              NULL, test_seal_key, sizeof(test_seal_key) - 1U) ==
              APPLIANCE_SUPPORT_BUNDLE_MISMATCH,
          "verify refuses a truncated key");
    CHECK(!bundle.verified,
          "a refused verification clears the verified flag");
    bundle.mac[0] = (uint8_t)(bundle.mac[0] ^ 0x01U);
    CHECK(appliance_support_bundle_verify(
              &bundle, test_stream, test_stream_length, test_mac,
              NULL, test_seal_key, sizeof(test_seal_key)) ==
              APPLIANCE_SUPPORT_BUNDLE_MISMATCH,
          "verify refuses a rewritten MAC");
    bundle.mac[0] = (uint8_t)(bundle.mac[0] ^ 0x01U);
    CHECK(appliance_support_bundle_verify(
              &bundle, test_stream, test_stream_length, test_mac,
              NULL, test_seal_key, sizeof(test_seal_key)) ==
              APPLIANCE_SUPPORT_BUNDLE_OK &&
              bundle.verified,
          "verify accepts the restored MAC");

    {
        uint8_t wrong_key[32];
        size_t index;

        for (index = 0U; index < sizeof(wrong_key); ++index) {
            wrong_key[index] = (uint8_t)(test_seal_key[index] ^ 0xFFU);
        }
        CHECK(appliance_support_bundle_verify(
                  &bundle, test_stream, test_stream_length, test_mac,
                  NULL, wrong_key, sizeof(wrong_key)) ==
                  APPLIANCE_SUPPORT_BUNDLE_MISMATCH,
              "verify refuses a foreign key");
    }

    {
        unsigned char tampered[TEST_STREAM_CAPACITY];

        memcpy(tampered, test_stream, test_stream_length);
        tampered[SUPPORT_BUNDLE_EXPORTER_HEADER_SIZE + 700U] ^= 0x80U;
        CHECK(appliance_support_bundle_verify(
                  &bundle, tampered, test_stream_length, test_mac,
                  NULL, test_seal_key, sizeof(test_seal_key)) ==
                  APPLIANCE_SUPPORT_BUNDLE_BAD_BINDING,
              "verify detects a rewritten payload byte");
    }

    /* A tampered redaction report must break the MAC. */
    {
        struct appliance_support_bundle copy = bundle;

        copy.binding.redaction.withheld = 0U;
        CHECK(appliance_support_bundle_verify(
                  &copy, test_stream, test_stream_length, test_mac,
                  NULL, test_seal_key, sizeof(test_seal_key)) ==
                  APPLIANCE_SUPPORT_BUNDLE_REDACTION_FAILED,
              "verify refuses a rewritten redaction report");
    }

    /* Verification needs the MAC to exist at all. */
    {
        struct appliance_support_bundle unsigned_bundle;

        memset(&unsigned_bundle, 0, sizeof(unsigned_bundle));
        (void)appliance_support_bundle_seal(
            &unsigned_bundle, test_stream, test_stream_length, &report,
            test_mac, NULL, NULL, 0U);
        CHECK(appliance_support_bundle_verify(
                  &unsigned_bundle, test_stream, test_stream_length,
                  test_mac, NULL, test_seal_key,
                  sizeof(test_seal_key)) ==
                  APPLIANCE_SUPPORT_BUNDLE_UNSIGNED,
              "verify reports UNSIGNED for an unsigned bundle");
    }

    /* A redaction failure never produces a MAC or a digest. */
    {
        struct appliance_support_bundle refused;
        struct appliance_support_bundle_redaction leaked;

        (void)appliance_support_bundle_redaction_eval(&test_source,
                                                      &report);
        leaked = report;
        leaked.leaked = 3U;
        memset(&refused, 0, sizeof(refused));
        CHECK(appliance_support_bundle_seal(
                  &refused, test_stream, test_stream_length, &leaked,
                  test_mac, NULL, test_seal_key,
                  sizeof(test_seal_key)) ==
                  APPLIANCE_SUPPORT_BUNDLE_REDACTION_FAILED,
              "seal refuses a bundle with leaked secret entries");
        CHECK(appliance_support_bundle_state(&refused) ==
                  APPLIANCE_SUPPORT_BUNDLE_STATE_REDACTION_FAILED,
              "a redaction failure publishes REDACTION_FAILED");
        CHECK(!appliance_support_bundle_is_sealed(&refused),
              "a redaction failure produces no seal");
        CHECK(refused.refused_count == 1U,
              "seal counts the refused outcome once");
    }
}

static void test_render(void)
{
    struct appliance_support_bundle bundle;
    struct appliance_support_bundle_redaction report;
    char output[TEST_RENDER_CAPACITY];
    size_t output_length = 0U;

    (void)appliance_support_bundle_redaction_eval(&test_source, &report);
    memset(&bundle, 0, sizeof(bundle));
    (void)appliance_support_bundle_seal(
        &bundle, test_stream, test_stream_length, &report, test_mac,
        NULL, test_seal_key, sizeof(test_seal_key));
    CHECK(appliance_support_bundle_verify(
              &bundle, test_stream, test_stream_length, test_mac,
              NULL, test_seal_key, sizeof(test_seal_key)) ==
              APPLIANCE_SUPPORT_BUNDLE_OK,
          "render fixture verifies its own seal");

    memset(output, 0, sizeof(output));
    CHECK(appliance_support_bundle_render(
              &bundle, output, sizeof(output), &output_length) ==
              APPLIANCE_SUPPORT_BUNDLE_OK,
          "render accepts a sealed bundle");
    CHECK(strstr(output,
                 "support_bundle v=1 state=SEALED scheme=HMAC_SHA256") !=
              NULL,
          "render names the sealed state and scheme");
    CHECK(strstr(output, "sections=6") != NULL,
          "render names the exported section count");
    CHECK(strstr(output, "withheld=2") != NULL,
          "render publishes the withheld entry count");
    CHECK(strstr(output, "redaction=OK") != NULL,
          "render publishes the redaction verdict");
    CHECK(strstr(output, "verified=YES") != NULL,
          "render publishes the verified outcome");
    CHECK(output_length == strlen(output),
          "render reports the exact length");

    memset(&bundle, 0, sizeof(bundle));
    (void)appliance_support_bundle_seal(
        &bundle, test_stream, test_stream_length, &report, test_mac,
        NULL, NULL, 0U);
    CHECK(appliance_support_bundle_render(
              &bundle, output, sizeof(output), &output_length) ==
              APPLIANCE_SUPPORT_BUNDLE_OK &&
              strstr(output, "state=UNSIGNED") != NULL &&
              strstr(output, "scheme=NONE") != NULL &&
              strstr(output, "verified=NO") != NULL,
          "render publishes an unsigned bundle honestly");
    CHECK(appliance_support_bundle_render(
              &bundle, output, 8U, &output_length) ==
              APPLIANCE_SUPPORT_BUNDLE_BUFFER_TOO_SMALL,
          "render refuses a short buffer");
    CHECK(appliance_support_bundle_render(
              NULL, output, sizeof(output), &output_length) ==
              APPLIANCE_SUPPORT_BUNDLE_NULL,
          "render refuses a null bundle");
}

static void test_metadata(void)
{
    CHECK(strcmp(appliance_support_bundle_schema_id(),
                 APPLIANCE_SUPPORT_BUNDLE_SCHEMA_ID) == 0,
          "schema id is the declared canonical binding id");
    CHECK(strcmp(appliance_support_bundle_state_string(
                     APPLIANCE_SUPPORT_BUNDLE_STATE_SEALED),
                 "SEALED") == 0 &&
              strcmp(appliance_support_bundle_state_string(
                         APPLIANCE_SUPPORT_BUNDLE_STATE_REDACTION_FAILED),
                     "REDACTION_FAILED") == 0,
          "state strings are the declared vocabulary");
    CHECK(strcmp(appliance_support_bundle_status_string(
                     APPLIANCE_SUPPORT_BUNDLE_NO_SEAL_KEY),
                 "NO_SEAL_KEY") == 0 &&
              strcmp(appliance_support_bundle_status_string(
                         APPLIANCE_SUPPORT_BUNDLE_MISMATCH),
                     "MISMATCH") == 0,
          "status strings are the declared vocabulary");
    CHECK(strcmp(appliance_support_bundle_scheme_string(
                     APPLIANCE_SUPPORT_BUNDLE_SCHEME_HMAC_SHA256),
                 "HMAC_SHA256") == 0 &&
              strcmp(appliance_support_bundle_scheme_string(
                         APPLIANCE_SUPPORT_BUNDLE_SCHEME_NONE),
                     "NONE") == 0,
          "scheme strings are the declared vocabulary");
    CHECK(appliance_support_bundle_state(NULL) ==
              APPLIANCE_SUPPORT_BUNDLE_STATE_NONE &&
              appliance_support_bundle_last_status(NULL) ==
                  APPLIANCE_SUPPORT_BUNDLE_NULL &&
              appliance_support_bundle_withheld(NULL) == 0U &&
              !appliance_support_bundle_is_sealed(NULL),
          "accessors fail closed on a null bundle");
}

int main(void)
{
    init_sources();
    build_fixture_stream();

    test_redaction_report();
    test_binding();
    test_seal_and_verify();
    test_render();
    test_metadata();

    CHECK(test_stream_length != 0U,
          "the fixture bundle is non-empty");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "the support bundle path performs no allocation");

    if (failures != 0) {
        fprintf(stderr,
                "Appliance support bundle host tests: %d FAILURES\n",
                failures);
        return 1;
    }
    printf("Appliance support bundle host tests: ALL OK\n");
    return 0;
}

#endif
