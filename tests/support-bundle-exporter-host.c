#include "support_bundle_exporter.h"

#include "config_kv.h"
#include "sha256.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifdef HFOC_SUPPORT_BUNDLE_EXPORTER_HOST_TEST

#define TEST_CONFIG_CAPACITY 6U
#define TEST_AUDIT_COUNT 2U
#define TEST_STREAM_CAPACITY 16384U
#define TEST_MAX_UNIT_SIZE 9088U
#define TEST_MANIFEST_HEADER_SIZE 128U
#define TEST_MANIFEST_ENTRY_SIZE 64U
#define TEST_LENGTH_SENTINEL 0xDEADBEEFU

static int failures;
static unsigned int alloc_attempts;
static unsigned int free_attempts;

static const unsigned char exporter_magic[
    SUPPORT_BUNDLE_EXPORTER_MAGIC_SIZE] = {
    'H', 'F', 'O', 'C', 'S', 'B', 'E', 'X'
};

static struct metrics_snapshot test_metrics;
static struct appliance_status_snapshot test_status;
static struct health_check_snapshot test_health;
static struct audit_record test_audit[TEST_AUDIT_COUNT];
static struct config_kv test_config;
static struct config_kv_entry test_entries[TEST_CONFIG_CAPACITY];
static struct support_bundle_manifest_source test_source;

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

static uint32_t load_u32(const unsigned char *bytes)
{
    return (uint32_t)bytes[0] |
           ((uint32_t)bytes[1] << 8U) |
           ((uint32_t)bytes[2] << 16U) |
           ((uint32_t)bytes[3] << 24U);
}

static uint64_t load_u64(const unsigned char *bytes)
{
    uint64_t value = 0U;
    size_t index;

    for (index = 0U; index < 8U; ++index) {
        value |= (uint64_t)bytes[index] << (8U * index);
    }
    return value;
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
    snapshot->state.recovery_mode = 1U;
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
    records[0].domain = 0U;
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
                            struct config_kv_entry *entries,
                            bool reverse)
{
    bool replaced;

    memset(entries, 0, sizeof(entries[0]) * TEST_CONFIG_CAPACITY);
    CHECK(config_kv_init(kv, entries, TEST_CONFIG_CAPACITY) ==
              CONFIG_KV_OK,
          "exporter fixture initializes config checkpoint");
    if (!reverse) {
        CHECK(config_kv_put(kv, CONFIG_KV_KIND_CONFIG,
                            "alpha", 5U, "one", 3U, &replaced) ==
                  CONFIG_KV_OK &&
                  config_kv_put(kv, CONFIG_KV_KIND_CONFIG,
                                "beta", 4U, "two", 3U, &replaced) ==
                      CONFIG_KV_OK &&
                  config_kv_put(kv, CONFIG_KV_KIND_CONFIG,
                                "secret", 6U, "s3cr3t", 6U,
                                &replaced) == CONFIG_KV_OK &&
                  config_kv_put(kv, CONFIG_KV_KIND_CONFIG,
                                "password", 8U, "hunter2", 7U,
                                &replaced) == CONFIG_KV_OK &&
                  config_kv_put(kv, CONFIG_KV_KIND_AUDIT,
                                "alpha", 5U, "audit-one", 9U,
                                &replaced) == CONFIG_KV_OK,
              "exporter fixture populates forward config checkpoint");
    } else {
        CHECK(config_kv_put(kv, CONFIG_KV_KIND_AUDIT,
                            "alpha", 5U, "audit-one", 9U,
                            &replaced) == CONFIG_KV_OK &&
                  config_kv_put(kv, CONFIG_KV_KIND_CONFIG,
                                "password", 8U, "hunter2", 7U,
                                &replaced) == CONFIG_KV_OK &&
                  config_kv_put(kv, CONFIG_KV_KIND_CONFIG,
                                "secret", 6U, "s3cr3t", 6U,
                                &replaced) == CONFIG_KV_OK &&
                  config_kv_put(kv, CONFIG_KV_KIND_CONFIG,
                                "beta", 4U, "two", 3U, &replaced) ==
                      CONFIG_KV_OK &&
                  config_kv_put(kv, CONFIG_KV_KIND_CONFIG,
                                "alpha", 5U, "one", 3U, &replaced) ==
                      CONFIG_KV_OK,
              "exporter fixture populates reverse config checkpoint");
    }
}

static void init_source(struct support_bundle_manifest_source *source)
{
    memset(source, 0, sizeof(*source));
    source->version = SUPPORT_BUNDLE_MANIFEST_VERSION;
    source->metrics = &test_metrics;
    source->status = &test_status;
    source->health = &test_health;
    source->audit_records = test_audit;
    source->audit_count = TEST_AUDIT_COUNT;
    source->config = &test_config;
}

struct fixture_state {
    struct metrics_snapshot metrics;
    struct appliance_status_snapshot status;
    struct health_check_snapshot health;
    struct audit_record audit[TEST_AUDIT_COUNT];
    struct config_kv config;
    struct config_kv_entry entries[TEST_CONFIG_CAPACITY];
};

static void capture_state(struct fixture_state *state)
{
    memcpy(&state->metrics, &test_metrics, sizeof(state->metrics));
    memcpy(&state->status, &test_status, sizeof(state->status));
    memcpy(&state->health, &test_health, sizeof(state->health));
    memcpy(state->audit, test_audit, sizeof(state->audit));
    memcpy(&state->config, &test_config, sizeof(state->config));
    memcpy(state->entries, test_entries, sizeof(state->entries));
}

static bool state_unchanged(const struct fixture_state *state)
{
    if (memcmp(&state->metrics, &test_metrics,
               sizeof(state->metrics)) != 0 ||
        memcmp(&state->status, &test_status,
               sizeof(state->status)) != 0 ||
        memcmp(&state->health, &test_health,
               sizeof(state->health)) != 0 ||
        memcmp(state->audit, test_audit,
               sizeof(state->audit)) != 0 ||
        state->config.entries != test_config.entries ||
        memcmp(&state->config, &test_config,
               sizeof(state->config)) != 0 ||
        memcmp(state->entries, test_entries,
               sizeof(state->entries)) != 0) {
        return false;
    }
    return true;
}

static void drain_exporter(struct support_bundle_exporter *exporter,
                           unsigned char *stream,
                           size_t stream_capacity,
                           size_t *stream_length_out)
{
    unsigned char unit[TEST_MAX_UNIT_SIZE];
    size_t offset = 0U;

    for (;;) {
        enum support_bundle_exporter_status status;
        size_t unit_length = 0U;

        status = support_bundle_exporter_next(
            exporter, unit, sizeof(unit), &unit_length);
        if (status == SUPPORT_BUNDLE_EXPORTER_DONE) {
            break;
        }
        CHECK(status == SUPPORT_BUNDLE_EXPORTER_OK,
              "exporter next emits a complete atomic unit");
        CHECK(offset + unit_length <= stream_capacity,
              "exporter stream remains within fixture capacity");
        memcpy(stream + offset, unit, unit_length);
        offset += unit_length;
    }
    *stream_length_out = offset;
}

static uint32_t manifest_entry_length(const unsigned char *manifest,
                                      uint32_t index)
{
    return load_u32(manifest + TEST_MANIFEST_HEADER_SIZE +
                    (size_t)index * TEST_MANIFEST_ENTRY_SIZE + 4U);
}

static bool stream_matches_fixture(
    const unsigned char *stream,
    size_t length,
    const struct support_bundle_manifest_source *source)
{
    unsigned char expected_manifest[SUPPORT_BUNDLE_MANIFEST_SIZE];
    unsigned char payload_digest[SHA256_DIGEST_SIZE];
    unsigned char terminal_digest[SHA256_DIGEST_SIZE];
    size_t manifest_length = 0U;
    size_t payload_offset = SUPPORT_BUNDLE_EXPORTER_HEADER_SIZE +
                            SUPPORT_BUNDLE_MANIFEST_SIZE;
    uint64_t payload_bytes = 0U;
    uint32_t index;

    if (length < SUPPORT_BUNDLE_EXPORTER_HEADER_SIZE +
                     SUPPORT_BUNDLE_MANIFEST_SIZE +
                     SUPPORT_BUNDLE_EXPORTER_DIGEST_SIZE ||
        memcmp(stream, exporter_magic,
               SUPPORT_BUNDLE_EXPORTER_MAGIC_SIZE) != 0 ||
        stream[SUPPORT_BUNDLE_EXPORTER_MAGIC_SIZE] !=
            SUPPORT_BUNDLE_EXPORTER_VERSION ||
        memcmp(stream + 16U, SUPPORT_BUNDLE_EXPORTER_SCHEMA_ID,
               SUPPORT_BUNDLE_EXPORTER_SCHEMA_ID_LENGTH) != 0 ||
        load_u32(stream + 32U) !=
            SUPPORT_BUNDLE_EXPORTER_MANIFEST_SIZE ||
        load_u32(stream + 36U) !=
            SUPPORT_BUNDLE_MANIFEST_ENTRY_MAX ||
        load_u32(stream + 40U) !=
            SUPPORT_BUNDLE_EXPORTER_VERSION ||
        load_u32(stream + 44U) !=
            SUPPORT_BUNDLE_MANIFEST_VERSION) {
        return false;
    }

    if (support_bundle_manifest_export(
            source, expected_manifest, sizeof(expected_manifest),
            &manifest_length) != SUPPORT_BUNDLE_MANIFEST_OK ||
        manifest_length != SUPPORT_BUNDLE_MANIFEST_SIZE ||
        memcmp(stream + SUPPORT_BUNDLE_EXPORTER_HEADER_SIZE,
               expected_manifest, SUPPORT_BUNDLE_MANIFEST_SIZE) != 0) {
        return false;
    }

    for (index = 0U;
         index < SUPPORT_BUNDLE_MANIFEST_ENTRY_MAX;
         ++index) {
        const size_t entry_offset = TEST_MANIFEST_HEADER_SIZE +
                                    (size_t)index *
                                        TEST_MANIFEST_ENTRY_SIZE;
        const uint32_t expected_type = index + 1U;
        const uint32_t payload_length =
            load_u32(expected_manifest + entry_offset + 4U);

        if (load_u32(expected_manifest + entry_offset) !=
                expected_type ||
            payload_offset + payload_length > length -
                SUPPORT_BUNDLE_EXPORTER_DIGEST_SIZE) {
            return false;
        }
        if (sha256_compute(stream + payload_offset, payload_length,
                           payload_digest) != SHA256_OK ||
            memcmp(payload_digest,
                   expected_manifest + entry_offset + 8U,
                   SHA256_DIGEST_SIZE) != 0) {
            return false;
        }
        payload_bytes += payload_length;
        payload_offset += payload_length;
    }

    if (payload_offset != length - SUPPORT_BUNDLE_EXPORTER_DIGEST_SIZE ||
        load_u64(stream + 48U) != payload_bytes ||
        load_u64(stream + 56U) != (uint64_t)length ||
        sha256_compute(stream, payload_offset,
                       terminal_digest) != SHA256_OK ||
        memcmp(terminal_digest,
               stream + payload_offset,
               SUPPORT_BUNDLE_EXPORTER_DIGEST_SIZE) != 0) {
        return false;
    }
    return true;
}

static void test_validation_and_status_strings(void)
{
    static const enum support_bundle_exporter_status statuses[] = {
        SUPPORT_BUNDLE_EXPORTER_OK,
        SUPPORT_BUNDLE_EXPORTER_NULL,
        SUPPORT_BUNDLE_EXPORTER_BAD_STATE,
        SUPPORT_BUNDLE_EXPORTER_BAD_MANIFEST,
        SUPPORT_BUNDLE_EXPORTER_TOO_SMALL,
        SUPPORT_BUNDLE_EXPORTER_DONE,
    };
    struct support_bundle_exporter exporter;
    struct support_bundle_exporter before;
    struct support_bundle_manifest_source invalid;
    struct metrics_snapshot bad_metrics;
    struct appliance_status_snapshot bad_status;
    struct health_check_snapshot bad_health;
    struct audit_record bad_audit[TEST_AUDIT_COUNT];
    struct config_kv bad_kv;
    struct config_kv_entry bad_entries[TEST_CONFIG_CAPACITY];
    unsigned char chunk[SUPPORT_BUNDLE_EXPORTER_HEADER_SIZE];
    size_t length;
    size_t index;

    memset(&exporter, 0, sizeof(exporter));
    CHECK(support_bundle_exporter_init(NULL, &test_source) ==
              SUPPORT_BUNDLE_EXPORTER_NULL,
          "exporter init rejects null exporter");
    CHECK(support_bundle_exporter_init(&exporter, NULL) ==
              SUPPORT_BUNDLE_EXPORTER_NULL,
          "exporter init rejects null source");
    CHECK(support_bundle_exporter_init(&exporter, &test_source) ==
              SUPPORT_BUNDLE_EXPORTER_OK &&
              exporter.active &&
              !exporter.done &&
              !exporter.closed &&
              exporter.source == &test_source,
          "exporter init publishes an active source");
    CHECK(support_bundle_exporter_next(NULL, chunk, sizeof(chunk),
                                       &length) ==
              SUPPORT_BUNDLE_EXPORTER_NULL,
          "exporter next rejects null exporter");
    CHECK(support_bundle_exporter_next(&exporter, NULL, sizeof(chunk),
                                       &length) ==
              SUPPORT_BUNDLE_EXPORTER_NULL,
          "exporter next rejects null buffer");
    CHECK(support_bundle_exporter_next(&exporter, chunk, sizeof(chunk),
                                       NULL) ==
              SUPPORT_BUNDLE_EXPORTER_NULL,
          "exporter next rejects null length output");
    CHECK(support_bundle_exporter_close(NULL) ==
              SUPPORT_BUNDLE_EXPORTER_NULL,
          "exporter close rejects null exporter");
    CHECK(support_bundle_exporter_close(&exporter) ==
              SUPPORT_BUNDLE_EXPORTER_OK,
          "validation fixture closes exporter");

    invalid = test_source;
    invalid.version = 0U;
    memset(&before, 0, sizeof(before));
    CHECK(support_bundle_exporter_init(&before, &invalid) ==
              SUPPORT_BUNDLE_EXPORTER_BAD_MANIFEST &&
              memcmp(&before, &(struct support_bundle_exporter){0},
                     sizeof(before)) == 0,
          "unsupported source version fails without exporter mutation");

    bad_metrics = test_metrics;
    bad_metrics.paths.health.check_count = 99U;
    invalid = test_source;
    invalid.metrics = &bad_metrics;
    CHECK(support_bundle_exporter_init(&before, &invalid) ==
              SUPPORT_BUNDLE_EXPORTER_BAD_MANIFEST,
          "invalid metrics snapshot fails closed");

    bad_status = test_status;
    bad_status.service_count = 99U;
    invalid = test_source;
    invalid.status = &bad_status;
    CHECK(support_bundle_exporter_init(&before, &invalid) ==
              SUPPORT_BUNDLE_EXPORTER_BAD_MANIFEST,
          "invalid status snapshot fails closed");

    bad_health = test_health;
    bad_health.descriptor_count = 99U;
    invalid = test_source;
    invalid.health = &bad_health;
    CHECK(support_bundle_exporter_init(&before, &invalid) ==
              SUPPORT_BUNDLE_EXPORTER_BAD_MANIFEST,
          "invalid health snapshot fails closed");

    memcpy(bad_audit, test_audit, sizeof(bad_audit));
    bad_audit[1].sequence = 10U;
    invalid = test_source;
    invalid.audit_records = bad_audit;
    CHECK(support_bundle_exporter_init(&before, &invalid) ==
              SUPPORT_BUNDLE_EXPORTER_BAD_MANIFEST,
          "non-increasing audit slice fails closed");

    memcpy(bad_entries, test_entries, sizeof(bad_entries));
    bad_kv = test_config;
    bad_kv.entries = bad_entries;
    bad_entries[0].key_length = 0U;
    invalid = test_source;
    invalid.config = &bad_kv;
    CHECK(support_bundle_exporter_init(&before, &invalid) ==
              SUPPORT_BUNDLE_EXPORTER_BAD_MANIFEST,
          "malformed config checkpoint fails closed");

    for (index = 0U;
         index < sizeof(statuses) / sizeof(statuses[0]);
         ++index) {
        const enum support_bundle_exporter_status status = statuses[index];
        const char *expected =
            status == SUPPORT_BUNDLE_EXPORTER_OK ?
                "SUPPORT_BUNDLE_EXPORTER_OK" :
            status == SUPPORT_BUNDLE_EXPORTER_NULL ?
                "SUPPORT_BUNDLE_EXPORTER_NULL" :
            status == SUPPORT_BUNDLE_EXPORTER_BAD_STATE ?
                "SUPPORT_BUNDLE_EXPORTER_BAD_STATE" :
            status == SUPPORT_BUNDLE_EXPORTER_BAD_MANIFEST ?
                "SUPPORT_BUNDLE_EXPORTER_BAD_MANIFEST" :
            status == SUPPORT_BUNDLE_EXPORTER_TOO_SMALL ?
                "SUPPORT_BUNDLE_EXPORTER_TOO_SMALL" :
                "SUPPORT_BUNDLE_EXPORTER_DONE";

        CHECK(support_bundle_exporter_status_string(status) != NULL &&
                  strcmp(support_bundle_exporter_status_string(status),
                         expected) == 0,
              "exporter status string is exact");
    }
    CHECK(strcmp(support_bundle_exporter_status_string(
                     (enum support_bundle_exporter_status)99U),
                 "SUPPORT_BUNDLE_EXPORTER_UNKNOWN") == 0,
          "undefined exporter status string is exact");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "exporter validation performs no allocation");
}

static void check_empty_sequence(void)
{
    struct support_bundle_exporter exporter;
    struct support_bundle_manifest_source source;
    struct config_kv empty_config;
    struct config_kv_entry empty_entries[2U];
    unsigned char stream[TEST_STREAM_CAPACITY];
    size_t stream_length = 0U;

    memset(empty_entries, 0, sizeof(empty_entries));
    CHECK(config_kv_init(&empty_config, empty_entries,
                         sizeof(empty_entries) /
                             sizeof(empty_entries[0])) ==
              CONFIG_KV_OK,
          "empty exporter fixture initializes empty config");
    source = test_source;
    source.audit_records = NULL;
    source.audit_count = 0U;
    source.config = &empty_config;

    CHECK(support_bundle_exporter_init(&exporter, &source) ==
              SUPPORT_BUNDLE_EXPORTER_OK,
          "empty exporter init succeeds");
    drain_exporter(&exporter, stream, sizeof(stream), &stream_length);
    CHECK(stream_matches_fixture(stream, stream_length, &source),
          "empty exporter stream is valid and digest-verified");
    CHECK(support_bundle_exporter_close(&exporter) ==
              SUPPORT_BUNDLE_EXPORTER_OK,
          "empty exporter closes after exhaustion");
}

static void check_populated_sequence(
    const struct support_bundle_manifest_source *source,
    const char *message)
{
    struct support_bundle_exporter exporter;
    unsigned char stream[TEST_STREAM_CAPACITY];
    size_t stream_length = 0U;

    CHECK(support_bundle_exporter_init(&exporter, source) ==
              SUPPORT_BUNDLE_EXPORTER_OK,
          "populated exporter init succeeds");
    drain_exporter(&exporter, stream, sizeof(stream), &stream_length);
    CHECK(stream_matches_fixture(stream, stream_length, source),
          message);
}

static void test_byte_identical_sequences(void)
{
    struct support_bundle_exporter exporter;
    struct support_bundle_manifest_source reverse_source;
    struct config_kv reverse_config;
    struct config_kv_entry reverse_entries[TEST_CONFIG_CAPACITY];
    unsigned char forward[TEST_STREAM_CAPACITY];
    unsigned char reverse[TEST_STREAM_CAPACITY];
    size_t forward_length = 0U;
    size_t reverse_length = 0U;

    check_empty_sequence();
    check_populated_sequence(
        &test_source,
        "populated exporter stream is valid and digest-verified");

    populate_config(&reverse_config, reverse_entries, true);
    reverse_source = test_source;
    reverse_source.config = &reverse_config;
    check_populated_sequence(
        &reverse_source,
        "reverse-slot exporter stream is valid and digest-verified");

    CHECK(support_bundle_exporter_init(&exporter, &test_source) ==
              SUPPORT_BUNDLE_EXPORTER_OK,
          "byte-identical fixture initializes forward exporter");
    drain_exporter(&exporter, forward, sizeof(forward),
                   &forward_length);
    CHECK(support_bundle_exporter_init(&exporter, &reverse_source) ==
              SUPPORT_BUNDLE_EXPORTER_OK,
          "byte-identical fixture initializes reverse exporter");
    drain_exporter(&exporter, reverse, sizeof(reverse),
                   &reverse_length);
    CHECK(forward_length == reverse_length &&
              memcmp(forward, reverse, forward_length) == 0,
          "equal logical state exports byte-identical streams");
}

static void check_short_output_retry(void)
{
    struct support_bundle_exporter exporter;
    struct support_bundle_exporter before;
    unsigned char manifest[SUPPORT_BUNDLE_MANIFEST_SIZE];
    unsigned char chunk[TEST_MAX_UNIT_SIZE];
    unsigned char before_bytes[TEST_MAX_UNIT_SIZE];
    size_t chunk_length = 0U;
    size_t metrics_length;
    size_t status_length;
    size_t health_length;
    size_t audit_length;
    size_t config_length;

    CHECK(support_bundle_exporter_init(&exporter, &test_source) ==
              SUPPORT_BUNDLE_EXPORTER_OK,
          "short-output fixture initializes exporter");

    memcpy(&before, &exporter, sizeof(before));
    memset(chunk, 0xA5U, sizeof(chunk));
    memcpy(before_bytes, chunk, sizeof(chunk));
    chunk_length = TEST_LENGTH_SENTINEL;
    CHECK(support_bundle_exporter_next(
              &exporter, chunk,
              SUPPORT_BUNDLE_EXPORTER_HEADER_SIZE - 1U,
              &chunk_length) == SUPPORT_BUNDLE_EXPORTER_TOO_SMALL &&
              memcmp(&exporter, &before, sizeof(exporter)) == 0 &&
              memcmp(chunk, before_bytes, sizeof(chunk)) == 0 &&
              chunk_length == TEST_LENGTH_SENTINEL,
          "short header buffer returns too-small without mutation");

    CHECK(support_bundle_exporter_next(
              &exporter, chunk,
              SUPPORT_BUNDLE_EXPORTER_HEADER_SIZE,
              &chunk_length) == SUPPORT_BUNDLE_EXPORTER_OK &&
              chunk_length == SUPPORT_BUNDLE_EXPORTER_HEADER_SIZE,
          "header retry with enough capacity emits exact header");

    CHECK(support_bundle_exporter_next(
              &exporter, manifest, sizeof(manifest),
              &chunk_length) == SUPPORT_BUNDLE_EXPORTER_OK &&
              chunk_length == SUPPORT_BUNDLE_MANIFEST_SIZE,
          "exporter emits exact manifest after header");

    metrics_length = manifest_entry_length(manifest, 0U);
    status_length = manifest_entry_length(manifest, 1U);
    health_length = manifest_entry_length(manifest, 2U);
    audit_length = manifest_entry_length(manifest, 3U);
    config_length = manifest_entry_length(manifest, 4U);
    CHECK(metrics_length == 245U && status_length > 0U &&
              health_length == 336U && audit_length == 104U &&
              config_length > 0U && config_length <= sizeof(chunk),
          "fixture payload lengths are bounded");

    memcpy(&before, &exporter, sizeof(before));
    memset(chunk, 0x4AU, sizeof(chunk));
    memcpy(before_bytes, chunk, sizeof(chunk));
    chunk_length = TEST_LENGTH_SENTINEL;
    CHECK(support_bundle_exporter_next(
              &exporter, chunk, metrics_length - 1U,
              &chunk_length) == SUPPORT_BUNDLE_EXPORTER_TOO_SMALL &&
              memcmp(&exporter, &before, sizeof(exporter)) == 0 &&
              memcmp(chunk, before_bytes, sizeof(chunk)) == 0 &&
              chunk_length == TEST_LENGTH_SENTINEL,
          "short metrics payload returns too-small without mutation");

    CHECK(support_bundle_exporter_next(
              &exporter, chunk, metrics_length,
              &chunk_length) == SUPPORT_BUNDLE_EXPORTER_OK &&
              chunk_length == metrics_length,
          "metrics payload retry emits one complete unit");
    CHECK(support_bundle_exporter_next(
              &exporter, chunk, status_length,
              &chunk_length) == SUPPORT_BUNDLE_EXPORTER_OK &&
              chunk_length == status_length,
          "status payload emits one complete unit");
    CHECK(support_bundle_exporter_next(
              &exporter, chunk, health_length,
              &chunk_length) == SUPPORT_BUNDLE_EXPORTER_OK &&
              chunk_length == health_length,
          "health payload emits one complete unit");
    CHECK(support_bundle_exporter_next(
              &exporter, chunk, audit_length,
              &chunk_length) == SUPPORT_BUNDLE_EXPORTER_OK &&
              chunk_length == audit_length,
          "audit payload emits one complete unit");

    memcpy(&before, &exporter, sizeof(before));
    memset(chunk, 0x5AU, sizeof(chunk));
    memcpy(before_bytes, chunk, sizeof(chunk));
    chunk_length = TEST_LENGTH_SENTINEL;
    CHECK(support_bundle_exporter_next(
              &exporter, chunk, config_length - 1U,
              &chunk_length) == SUPPORT_BUNDLE_EXPORTER_TOO_SMALL &&
              memcmp(&exporter, &before, sizeof(exporter)) == 0 &&
              memcmp(chunk, before_bytes, sizeof(chunk)) == 0 &&
              chunk_length == TEST_LENGTH_SENTINEL,
          "short config payload returns too-small without mutation");

    CHECK(support_bundle_exporter_next(
              &exporter, chunk, config_length,
              &chunk_length) == SUPPORT_BUNDLE_EXPORTER_OK &&
              chunk_length == config_length,
          "config payload retry emits one complete unit");

    CHECK(support_bundle_exporter_next(
              &exporter, chunk, sizeof(chunk),
              &chunk_length) == SUPPORT_BUNDLE_EXPORTER_OK &&
              chunk_length > 0U,
          "exporter emits remaining versions unit");

    memcpy(&before, &exporter, sizeof(before));
    memset(chunk, 0x6AU, sizeof(chunk));
    memcpy(before_bytes, chunk, sizeof(chunk));
    chunk_length = TEST_LENGTH_SENTINEL;
    CHECK(support_bundle_exporter_next(
              &exporter, chunk,
              SUPPORT_BUNDLE_EXPORTER_DIGEST_SIZE - 1U,
              &chunk_length) == SUPPORT_BUNDLE_EXPORTER_TOO_SMALL &&
              memcmp(&exporter, &before, sizeof(exporter)) == 0 &&
              memcmp(chunk, before_bytes, sizeof(chunk)) == 0 &&
              chunk_length == TEST_LENGTH_SENTINEL,
          "short digest buffer returns too-small without mutation");

    CHECK(support_bundle_exporter_next(
              &exporter, chunk, sizeof(chunk),
              &chunk_length) == SUPPORT_BUNDLE_EXPORTER_OK &&
              chunk_length == SUPPORT_BUNDLE_EXPORTER_DIGEST_SIZE,
          "digest retry emits exact terminal digest");
    CHECK(support_bundle_exporter_next(
              &exporter, chunk, sizeof(chunk),
              &chunk_length) == SUPPORT_BUNDLE_EXPORTER_DONE,
          "exporter exhausts after terminal digest");
    CHECK(support_bundle_exporter_close(&exporter) ==
              SUPPORT_BUNDLE_EXPORTER_OK,
          "short-output fixture closes cleanly");
}

static void check_close_and_exhaustion(void)
{
    struct support_bundle_exporter exporter;
    struct fixture_state state;
    unsigned char chunk[TEST_MAX_UNIT_SIZE];
    unsigned char stream[TEST_STREAM_CAPACITY];
    size_t stream_length = 0U;
    size_t chunk_length = 0U;

    capture_state(&state);
    CHECK(support_bundle_exporter_init(&exporter, &test_source) ==
              SUPPORT_BUNDLE_EXPORTER_OK,
          "close fixture initializes exporter");
    CHECK(support_bundle_exporter_next(
              &exporter, chunk, sizeof(chunk),
              &chunk_length) == SUPPORT_BUNDLE_EXPORTER_OK,
          "close fixture emits header before close");
    CHECK(support_bundle_exporter_close(&exporter) ==
              SUPPORT_BUNDLE_EXPORTER_OK,
          "close before exhaustion succeeds");
    CHECK(!exporter.active && exporter.closed,
          "close marks exporter inactive and closed");
    CHECK(support_bundle_exporter_next(
              &exporter, chunk, sizeof(chunk),
              &chunk_length) == SUPPORT_BUNDLE_EXPORTER_BAD_STATE,
          "next after close before exhaustion is bad state");
    CHECK(support_bundle_exporter_close(&exporter) ==
              SUPPORT_BUNDLE_EXPORTER_OK,
          "close is idempotent");
    CHECK(state_unchanged(&state),
          "close leaves source state unchanged");

    capture_state(&state);
    CHECK(support_bundle_exporter_init(&exporter, &test_source) ==
              SUPPORT_BUNDLE_EXPORTER_OK,
          "exhaustion fixture initializes exporter");
    drain_exporter(&exporter, stream, sizeof(stream), &stream_length);
    CHECK(stream_length > 0U &&
              stream_matches_fixture(stream, stream_length,
                                     &test_source),
          "drained stream passes export format checks");
    CHECK(support_bundle_exporter_next(
              &exporter, chunk, sizeof(chunk),
              &chunk_length) == SUPPORT_BUNDLE_EXPORTER_DONE,
          "next after exhaustion returns done");
    CHECK(support_bundle_exporter_close(&exporter) ==
              SUPPORT_BUNDLE_EXPORTER_OK,
          "exhausted exporter closes cleanly");
    CHECK(state_unchanged(&state),
          "exhaustion leaves source state unchanged");
}

static void test_digest_change(void)
{
    struct support_bundle_exporter exporter;
    struct support_bundle_manifest_source source;
    struct metrics_snapshot changed_metrics;
    unsigned char baseline[TEST_STREAM_CAPACITY];
    unsigned char changed[TEST_STREAM_CAPACITY];
    size_t baseline_length = 0U;
    size_t changed_length = 0U;

    CHECK(support_bundle_exporter_init(&exporter, &test_source) ==
              SUPPORT_BUNDLE_EXPORTER_OK,
          "digest-change fixture exports baseline");
    drain_exporter(&exporter, baseline, sizeof(baseline),
                   &baseline_length);

    changed_metrics = test_metrics;
    changed_metrics.generation = 99U;
    source = test_source;
    source.metrics = &changed_metrics;
    CHECK(support_bundle_exporter_init(&exporter, &source) ==
              SUPPORT_BUNDLE_EXPORTER_OK,
          "digest-change fixture exports changed source");
    drain_exporter(&exporter, changed, sizeof(changed),
                   &changed_length);
    CHECK(baseline_length == changed_length &&
              memcmp(baseline, changed, baseline_length) != 0,
          "source change changes exported stream bytes");
}

static void test_no_allocation(void)
{
    struct support_bundle_exporter exporter;
    unsigned char stream[TEST_STREAM_CAPACITY];
    size_t stream_length = 0U;

    alloc_attempts = 0U;
    free_attempts = 0U;
    CHECK(support_bundle_exporter_init(&exporter, &test_source) ==
              SUPPORT_BUNDLE_EXPORTER_OK,
          "no-allocation fixture initializes exporter");
    drain_exporter(&exporter, stream, sizeof(stream), &stream_length);
    CHECK(support_bundle_exporter_close(&exporter) ==
              SUPPORT_BUNDLE_EXPORTER_OK,
          "no-allocation fixture closes exporter");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "exporter iteration performs no allocation");
}

int main(void)
{
    init_metrics(&test_metrics);
    init_status(&test_status);
    init_health(&test_health);
    init_audit(test_audit);
    populate_config(&test_config, test_entries, false);
    init_source(&test_source);

    test_validation_and_status_strings();
    test_byte_identical_sequences();
    check_short_output_retry();
    check_close_and_exhaustion();
    test_digest_change();
    test_no_allocation();

    if (failures == 0) {
        printf("Support bundle exporter host tests: ALL OK\n");
        return 0;
    }
    fprintf(stderr, "Support bundle exporter host tests: %d FAILURES\n",
            failures);
    return 1;
}

#endif
