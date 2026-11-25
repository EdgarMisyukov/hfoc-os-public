/*
 * ITEM-378 focused proof: the production support plane.
 *
 * The fixture exports a real bundle through the ITEM-168 exporter, checks the
 * composed bytes against an independent export, proves the seal and the
 * redaction attestation, walks the consent-based session, and proves that an
 * applied break-glass restores the LOCAL_ONLY management ingress policy and
 * revokes a live support session.
 */
#include "appliance_production_support.h"

#include "appliance_management_bind.h"
#include "appliance_state_crypto.h"
#include "config_kv.h"
#include "sha256.h"
#include "support_bundle_exporter.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifdef HFOC_APPLIANCE_PRODUCTION_SUPPORT_HOST_TEST

#define TEST_CONFIG_CAPACITY 6U
#define TEST_AUDIT_COUNT 2U
#define TEST_STREAM_CAPACITY APPLIANCE_PRODUCTION_SUPPORT_BUNDLE_MAX
#define TEST_MAX_UNIT_SIZE 9088U
#define TEST_RENDER_CAPACITY 1024U
#define TEST_TICK_BASE 20000U

static int failures;
static unsigned int alloc_attempts;
static unsigned int free_attempts;

static struct metrics_snapshot test_metrics;
static struct appliance_status_snapshot test_status;
static struct health_check_snapshot test_health;
static struct audit_record test_audit[TEST_AUDIT_COUNT];
static struct config_kv test_config;
static struct config_kv_entry test_entries[TEST_CONFIG_CAPACITY];
static struct support_bundle_manifest_source test_source;
static struct appliance_management_bind test_bind;
static struct appliance_production_support test_support;
static unsigned char reference_stream[TEST_STREAM_CAPACITY];
static size_t reference_length;

static const uint8_t test_seal_key[32] = {
    0x5AU, 0x6BU, 0x7CU, 0x8DU, 0x9EU, 0xAFU, 0xB0U, 0xC1U,
    0xD2U, 0xE3U, 0xF4U, 0x05U, 0x16U, 0x27U, 0x38U, 0x49U,
    0x5AU, 0x6BU, 0x7CU, 0x8DU, 0x9EU, 0xAFU, 0xB0U, 0xC1U,
    0xD2U, 0xE3U, 0xF4U, 0x05U, 0x16U, 0x27U, 0x38U, 0x49U
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
    snapshot->service_count = 2U;
    snapshot->services[0].id = 1U;
    snapshot->services[0].state = APPLIANCE_SERVICE_READY;
    snapshot->services[0].health = APPLIANCE_SERVICE_HEALTH_READY;
    snapshot->services[1].id = 2U;
    snapshot->services[1].state = APPLIANCE_SERVICE_RUNNING;
    snapshot->services[1].health = APPLIANCE_SERVICE_HEALTH_PENDING;
    snapshot->services[1].restart_count = 1U;
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

static void populate_config(void)
{
    bool replaced;

    memset(test_entries, 0, sizeof(test_entries));
    CHECK(config_kv_init(&test_config, test_entries,
                         TEST_CONFIG_CAPACITY) == CONFIG_KV_OK,
          "support plane fixture initializes config");
    CHECK(config_kv_put(&test_config, CONFIG_KV_KIND_CONFIG,
                        "alpha", 5U, "one", 3U, &replaced) ==
              CONFIG_KV_OK &&
          config_kv_put(&test_config, CONFIG_KV_KIND_CONFIG,
                        "beta", 4U, "two", 3U, &replaced) ==
              CONFIG_KV_OK &&
          config_kv_put(&test_config, CONFIG_KV_KIND_CONFIG,
                        "secret", 6U, "s3cr3t", 6U, &replaced) ==
              CONFIG_KV_OK &&
          config_kv_put(&test_config, CONFIG_KV_KIND_CONFIG,
                        "password", 8U, "hunter2", 7U, &replaced) ==
              CONFIG_KV_OK,
          "support plane fixture populates secrets and plain entries");
}

static bool appliance_production_support_bytes_match_reference(void);

static void init_sources(void)
{    init_metrics(&test_metrics);
    init_status(&test_status);
    init_health(&test_health);
    init_audit(test_audit);
    populate_config();

    memset(&test_source, 0, sizeof(test_source));
    test_source.version = SUPPORT_BUNDLE_MANIFEST_VERSION;
    test_source.metrics = &test_metrics;
    test_source.status = &test_status;
    test_source.health = &test_health;
    test_source.audit_records = test_audit;
    test_source.audit_count = TEST_AUDIT_COUNT;
    test_source.config = &test_config;
}

static void build_reference_stream(void)
{
    struct support_bundle_exporter exporter;
    unsigned char unit[TEST_MAX_UNIT_SIZE];
    size_t offset = 0U;

    CHECK(support_bundle_exporter_init(&exporter, &test_source) ==
              SUPPORT_BUNDLE_EXPORTER_OK,
          "reference exporter admits the fixture source");
    for (;;) {
        enum support_bundle_exporter_status step;
        size_t unit_length = 0U;

        step = support_bundle_exporter_next(
            &exporter, unit, sizeof(unit), &unit_length);
        if (step == SUPPORT_BUNDLE_EXPORTER_DONE) {
            break;
        }
        CHECK(step == SUPPORT_BUNDLE_EXPORTER_OK,
              "reference exporter emits an atomic unit");
        CHECK(offset + unit_length <= sizeof(reference_stream),
              "reference stream stays in bounds");
        memcpy(reference_stream + offset, unit, unit_length);
        offset += unit_length;
    }
    (void)support_bundle_exporter_close(&exporter);
    reference_length = offset;
}

static void test_publish_bundle_sealed(void)
{
    const struct appliance_support_bundle *bundle;
    char output[TEST_RENDER_CAPACITY];
    size_t output_length = 0U;

    CHECK(appliance_production_support_init(&test_support, &test_bind) ==
              APPLIANCE_PRODUCTION_SUPPORT_OK,
          "support plane initializes");
    CHECK(appliance_production_support_state(&test_support) ==
              APPLIANCE_PRODUCTION_SUPPORT_STATE_DEGRADED,
          "a plane without a bundle is degraded, not ready");
    CHECK(appliance_production_support_publish_bundle(
              &test_support, &test_source, test_mac, NULL,
              test_seal_key, sizeof(test_seal_key)) ==
              APPLIANCE_PRODUCTION_SUPPORT_OK,
          "publishing a bundle with a bound key succeeds");
    CHECK(appliance_production_support_state(&test_support) ==
              APPLIANCE_PRODUCTION_SUPPORT_STATE_READY,
          "a sealed bundle makes the plane ready");
    CHECK(appliance_production_support_bundle_published(&test_support),
          "the plane publishes the bundle");
    CHECK(appliance_production_support_bundle_bytes(&test_support) ==
              (uint64_t)reference_length &&
              appliance_production_support_bytes_match_reference(),
          "the composed bundle is byte-identical to the reference export");
    CHECK(appliance_production_support_sections(&test_support) ==
              SUPPORT_BUNDLE_MANIFEST_ENTRY_MAX,
          "the composed bundle carries every declared section");
    CHECK(appliance_production_support_withheld(&test_support) == 2U,
          "the composed bundle attests two withheld secret entries");

    bundle = appliance_production_support_bundle(&test_support);
    CHECK(bundle != NULL &&
              appliance_support_bundle_state(bundle) ==
                  APPLIANCE_SUPPORT_BUNDLE_STATE_SEALED &&
              appliance_support_bundle_is_sealed(bundle),
          "the composed bundle is sealed");
    CHECK(appliance_support_bundle_verify(
              (struct appliance_support_bundle *)bundle,
              test_support.bundle_buffer,
              (size_t)appliance_production_support_bundle_bytes(
                  &test_support),
              test_mac, NULL, test_seal_key, sizeof(test_seal_key)) ==
              APPLIANCE_SUPPORT_BUNDLE_OK,
          "the composed seal verifies with the same key");

    memset(output, 0, sizeof(output));
    CHECK(appliance_production_support_render(
              &test_support, TEST_TICK_BASE, output, sizeof(output),
              &output_length) == APPLIANCE_PRODUCTION_SUPPORT_OK,
          "render accepts a ready plane");
    CHECK(strstr(output, "support_plane v=1 state=READY") != NULL &&
              strstr(output, "bundle=SEALED") != NULL &&
              strstr(output, "sections=6") != NULL &&
              strstr(output, "withheld=2") != NULL &&
              strstr(output, "session=IDLE") != NULL &&
              strstr(output, "break_glass=STANDBY") != NULL &&
              strstr(output, "shell=FORBIDDEN") != NULL,
          "render publishes the sealed bundle, session and break-glass");
    CHECK(output_length == strlen(output),
          "render reports the exact length");
    CHECK(appliance_production_support_render(
              &test_support, TEST_TICK_BASE, output, 8U,
              &output_length) ==
              APPLIANCE_PRODUCTION_SUPPORT_BUFFER_TOO_SMALL,
          "render refuses a short buffer");
    CHECK(appliance_production_support_render(
              NULL, TEST_TICK_BASE, output, sizeof(output),
              &output_length) == APPLIANCE_PRODUCTION_SUPPORT_NULL,
          "render refuses a null plane");
}

static void test_publish_bundle_unsigned(void)
{
    CHECK(appliance_production_support_publish_bundle(
              &test_support, &test_source, test_mac, NULL, NULL, 0U) ==
              APPLIANCE_PRODUCTION_SUPPORT_DEGRADED,
          "publishing without a key reports DEGRADED");
    CHECK(appliance_production_support_state(&test_support) ==
              APPLIANCE_PRODUCTION_SUPPORT_STATE_DEGRADED,
          "an unsigned bundle keeps the plane degraded");
    CHECK(appliance_production_support_bundle_published(&test_support) &&
              appliance_support_bundle_state(
                  appliance_production_support_bundle(&test_support)) ==
                  APPLIANCE_SUPPORT_BUNDLE_STATE_UNSIGNED,
          "an unsigned bundle is published honestly as UNSIGNED");
    CHECK(appliance_production_support_withheld(&test_support) == 2U,
          "an unsigned bundle still attests its redaction");

    CHECK(appliance_production_support_publish_bundle(
              &test_support, &test_source, test_mac, NULL,
              test_seal_key, sizeof(test_seal_key)) ==
              APPLIANCE_PRODUCTION_SUPPORT_OK &&
              appliance_production_support_state(&test_support) ==
                  APPLIANCE_PRODUCTION_SUPPORT_STATE_READY,
          "republishing with a key restores the sealed ready state");
}

static void test_session_consent_flow(void)
{
    CHECK(appliance_production_support_session_request(
              &test_support,
              APPLIANCE_SUPPORT_SESSION_ORIGIN_NETWORK, 7U,
              APPLIANCE_SUPPORT_SESSION_SCOPES_DECLARED,
              TEST_TICK_BASE) == APPLIANCE_PRODUCTION_SUPPORT_OK,
          "a network support request is accepted as inert");
    CHECK(appliance_production_support_session_consent(
              &test_support,
              APPLIANCE_SUPPORT_SESSION_ORIGIN_NETWORK, 1U, 0U,
              TEST_TICK_BASE + 1U) ==
              APPLIANCE_PRODUCTION_SUPPORT_DENIED,
          "the plane refuses a network consent");
    CHECK(appliance_production_support_session_request(
              &test_support,
              APPLIANCE_SUPPORT_SESSION_ORIGIN_NETWORK, 7U,
              APPLIANCE_SUPPORT_SESSION_SCOPES_DECLARED,
              TEST_TICK_BASE + 2U) == APPLIANCE_PRODUCTION_SUPPORT_OK,
          "a new request after a denial is accepted");
    CHECK(appliance_production_support_session_consent(
              &test_support,
              APPLIANCE_SUPPORT_SESSION_ORIGIN_LOCAL_CONSOLE, 1U, 0U,
              TEST_TICK_BASE + 3U) == APPLIANCE_PRODUCTION_SUPPORT_OK,
          "a local consent activates the session");
    CHECK(appliance_support_session_active(
              appliance_production_support_session(&test_support),
              TEST_TICK_BASE + 4U),
          "the plane publishes an active session");
    CHECK(appliance_production_support_session_revoke(
              &test_support,
              APPLIANCE_SUPPORT_SESSION_ORIGIN_LOCAL_CONSOLE, 1U,
              TEST_TICK_BASE + 5U) == APPLIANCE_PRODUCTION_SUPPORT_OK,
          "the plane revokes the session");
    CHECK(!appliance_support_session_active(
              appliance_production_support_session(&test_support),
              TEST_TICK_BASE + 6U),
          "a revoked session is inactive");
}

static void test_break_glass_effects(void)
{
    uint64_t token = 0U;
    uint32_t actions = 0U;

    CHECK(appliance_management_bind_publish_outer_interface(&test_bind) ==
              APPLIANCE_MANAGEMENT_BIND_OK &&
              appliance_management_bind_scope_of(&test_bind) ==
                  APPLIANCE_MANAGEMENT_BIND_SCOPE_OUTER_INTERFACE,
          "the fixture opens the outer ingress explicitly");

    CHECK(appliance_production_support_session_request(
              &test_support,
              APPLIANCE_SUPPORT_SESSION_ORIGIN_NETWORK, 9U,
              APPLIANCE_SUPPORT_SESSION_SCOPE_EXPORT_BUNDLE,
              TEST_TICK_BASE) == APPLIANCE_PRODUCTION_SUPPORT_OK &&
              appliance_production_support_session_consent(
                  &test_support,
                  APPLIANCE_SUPPORT_SESSION_ORIGIN_LOCAL_CONSOLE, 1U,
                  0U, TEST_TICK_BASE + 1U) ==
                  APPLIANCE_PRODUCTION_SUPPORT_OK,
          "a remote support session is active before break-glass");

    CHECK(appliance_production_support_break_glass_arm(
              &test_support,
              APPLIANCE_SUPPORT_BREAK_GLASS_ORIGIN_NETWORK, 1U,
              APPLIANCE_SUPPORT_BREAK_GLASS_ACTIONS_DECLARED,
              TEST_TICK_BASE, &token) ==
              APPLIANCE_PRODUCTION_SUPPORT_DENIED,
          "the plane refuses a network break-glass arm");
    CHECK(token == 0U,
          "a refused arm produces no token");
    CHECK(appliance_production_support_break_glass_arm(
              &test_support,
              APPLIANCE_SUPPORT_BREAK_GLASS_ORIGIN_LOCAL_CONSOLE, 1U,
              APPLIANCE_SUPPORT_BREAK_GLASS_ACTIONS_DECLARED,
              TEST_TICK_BASE, &token) ==
              APPLIANCE_PRODUCTION_SUPPORT_OK,
          "a local break-glass arm succeeds");
    CHECK(token != 0U,
          "the local arm publishes a single-use token");
    CHECK(appliance_production_support_break_glass_apply(
              &test_support,
              APPLIANCE_SUPPORT_BREAK_GLASS_ORIGIN_NETWORK, token,
              TEST_TICK_BASE + 1U, &actions) ==
              APPLIANCE_PRODUCTION_SUPPORT_DENIED &&
              actions == 0U,
          "the plane refuses to apply break-glass from the network");
    CHECK(appliance_management_bind_scope_of(&test_bind) ==
              APPLIANCE_MANAGEMENT_BIND_SCOPE_OUTER_INTERFACE,
          "a refused apply changes no management policy");

    CHECK(appliance_production_support_break_glass_apply(
              &test_support,
              APPLIANCE_SUPPORT_BREAK_GLASS_ORIGIN_LOCAL_CONSOLE, token,
              TEST_TICK_BASE + 2U, &actions) ==
              APPLIANCE_PRODUCTION_SUPPORT_OK,
          "a local break-glass apply succeeds");
    CHECK(actions ==
              APPLIANCE_SUPPORT_BREAK_GLASS_ACTIONS_DECLARED,
          "the applied action mask is the declared set");
    CHECK(appliance_management_bind_scope_of(&test_bind) ==
              APPLIANCE_MANAGEMENT_BIND_SCOPE_LOCAL_ONLY,
          "break-glass restores the LOCAL_ONLY management ingress");
    CHECK(!appliance_support_session_active(
              appliance_production_support_session(&test_support),
              TEST_TICK_BASE + 3U),
          "break-glass revokes the live remote support session");
    CHECK(appliance_production_support_break_glass_effects(
              &test_support) == 1U,
          "the plane counts the applied effect once");
    CHECK(appliance_production_support_state(&test_support) ==
              APPLIANCE_PRODUCTION_SUPPORT_STATE_READY,
          "a complete break-glass effect keeps the plane ready");
}

static void test_partial_effect_degrades(void)
{
    static struct appliance_production_support unbound;
    uint64_t token = 0U;
    uint32_t actions = 0U;

    CHECK(appliance_production_support_init(&unbound, NULL) ==
              APPLIANCE_PRODUCTION_SUPPORT_OK,
          "a plane without a management bind owner initializes");
    (void)appliance_production_support_publish_bundle(
        &unbound, &test_source, test_mac, NULL, test_seal_key,
        sizeof(test_seal_key));
    CHECK(appliance_production_support_break_glass_arm(
              &unbound,
              APPLIANCE_SUPPORT_BREAK_GLASS_ORIGIN_LOCAL_CONSOLE, 1U,
              APPLIANCE_SUPPORT_BREAK_GLASS_ACTION_RESTORE_MANAGEMENT_LOCAL_ONLY,
              TEST_TICK_BASE, &token) ==
              APPLIANCE_PRODUCTION_SUPPORT_OK,
          "the unbound plane arms the restore action");
    CHECK(appliance_production_support_break_glass_apply(
              &unbound,
              APPLIANCE_SUPPORT_BREAK_GLASS_ORIGIN_LOCAL_CONSOLE, token,
              TEST_TICK_BASE + 1U, &actions) ==
              APPLIANCE_PRODUCTION_SUPPORT_DEGRADED,
          "an unperformable effect degrades instead of claiming success");
    CHECK(appliance_production_support_state(&unbound) ==
              APPLIANCE_PRODUCTION_SUPPORT_STATE_DEGRADED &&
              unbound.break_glass_partial_count == 1U,
          "the partial effect is counted and published as DEGRADED");
}

static void test_refresh_and_stop(void)
{
    char output[TEST_RENDER_CAPACITY];
    size_t output_length = 0U;

    (void)appliance_production_support_session_request(
        &test_support, APPLIANCE_SUPPORT_SESSION_ORIGIN_NETWORK, 11U,
        APPLIANCE_SUPPORT_SESSION_SCOPE_READ_DIAGNOSTICS,
        TEST_TICK_BASE);
    (void)appliance_production_support_session_consent(
        &test_support, APPLIANCE_SUPPORT_SESSION_ORIGIN_LOCAL_CONSOLE,
        1U, 60U, TEST_TICK_BASE + 1U);
    CHECK(appliance_production_support_refresh(
              &test_support, TEST_TICK_BASE + 62U) ==
              APPLIANCE_PRODUCTION_SUPPORT_OK,
          "refresh keeps a ready plane ready after the session expires");
    CHECK(appliance_support_session_state(
              appliance_production_support_session(&test_support)) ==
              APPLIANCE_SUPPORT_SESSION_STATE_EXPIRED,
          "refresh expires the bounded session");

    memset(output, 0, sizeof(output));
    (void)appliance_production_support_render(
        &test_support, TEST_TICK_BASE + 62U, output, sizeof(output),
        &output_length);
    CHECK(strstr(output, "session=EXPIRED") != NULL &&
              strstr(output, "session_active=NO") != NULL,
          "render publishes the expired session honestly");

    CHECK(appliance_production_support_stop(&test_support,
                                            TEST_TICK_BASE + 63U) ==
              APPLIANCE_PRODUCTION_SUPPORT_OK &&
              appliance_production_support_state(&test_support) ==
                  APPLIANCE_PRODUCTION_SUPPORT_STATE_STOPPED,
          "the plane stops cleanly");
    CHECK(appliance_support_session_state(
              appliance_production_support_session(&test_support)) ==
              APPLIANCE_SUPPORT_SESSION_STATE_STOPPED,
          "stopping the plane stops the session");
}

static void test_metadata_and_accessors(void)
{
    CHECK(strcmp(appliance_production_support_schema_id(),
                 APPLIANCE_PRODUCTION_SUPPORT_SCHEMA_ID) == 0,
          "the plane schema id is the declared id");
    CHECK(strcmp(appliance_production_support_state_string(
                     APPLIANCE_PRODUCTION_SUPPORT_STATE_READY),
                 "READY") == 0 &&
              strcmp(appliance_production_support_status_string(
                         APPLIANCE_PRODUCTION_SUPPORT_DENIED),
                     "DENIED") == 0,
          "plane state and status strings are declared");
    CHECK(appliance_production_support_state(NULL) ==
              APPLIANCE_PRODUCTION_SUPPORT_STATE_NONE &&
              appliance_production_support_last_status(NULL) ==
                  APPLIANCE_PRODUCTION_SUPPORT_NULL &&
              appliance_production_support_bundle(NULL) == NULL &&
              appliance_production_support_session(NULL) == NULL &&
              appliance_production_support_break_glass(NULL) == NULL &&
              !appliance_production_support_bundle_published(NULL) &&
              appliance_production_support_bundle_bytes(NULL) == 0U &&
              appliance_production_support_sections(NULL) == 0U &&
              appliance_production_support_withheld(NULL) == 0U &&
              appliance_production_support_break_glass_effects(NULL) == 0U,
          "accessors fail closed on a null plane");
    CHECK(appliance_production_support_init(NULL, NULL) ==
              APPLIANCE_PRODUCTION_SUPPORT_NULL &&
              appliance_production_support_publish_bundle(
                  NULL, &test_source, test_mac, NULL, test_seal_key,
                  sizeof(test_seal_key)) ==
                  APPLIANCE_PRODUCTION_SUPPORT_NULL,
          "operations refuse a null plane");
}

static bool appliance_production_support_bytes_match_reference(void)
{
    return test_support.bundle_buffer != NULL &&
           (size_t)appliance_production_support_bundle_bytes(
               &test_support) == reference_length &&
           memcmp(test_support.bundle_buffer, reference_stream,
                  reference_length) == 0;
}

int main(void)
{
    init_sources();
    build_reference_stream();
    CHECK(reference_length != 0U, "the reference bundle is non-empty");

    test_publish_bundle_sealed();
    test_publish_bundle_unsigned();
    test_session_consent_flow();
    test_break_glass_effects();
    test_partial_effect_degrades();
    test_refresh_and_stop();
    test_metadata_and_accessors();

    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "the production support path performs no allocation");

    if (failures != 0) {
        fprintf(stderr,
                "Appliance production support host tests: %d FAILURES\n",
                failures);
        return 1;
    }
    printf("Appliance production support host tests: ALL OK\n");
    return 0;
}

#endif
