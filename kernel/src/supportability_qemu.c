#ifdef HFOC_SUPPORTABILITY_QEMU_TEST

#include "appliance_status.h"
#include "audit.h"
#include "config_kv.h"
#include "diagnostic_privacy.h"
#include "health_check.h"
#include "metrics.h"
#include "serial.h"
#include "support_bundle_exporter.h"
#include "support_bundle_manifest.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define SUPPORTABILITY_CHECK_COUNT 5U
#define SUPPORTABILITY_CHECK_STORAGE 1U
#define SUPPORTABILITY_CHECK_SERVICE 2U
#define SUPPORTABILITY_CHECK_NETWORK 3U
#define SUPPORTABILITY_CHECK_AUDIT 4U
#define SUPPORTABILITY_CHECK_UPDATE 5U

#define SUPPORTABILITY_CONFIG_CAPACITY 8U
#define SUPPORTABILITY_AUDIT_COUNT 2U
#define SUPPORTABILITY_STREAM_CAPACITY 4096U
#define SUPPORTABILITY_UNIT_CAPACITY 1024U

#define SUPPORTABILITY_SECRET_KEY "secret"
#define SUPPORTABILITY_SECRET_KEY_LEN 6U
#define SUPPORTABILITY_SECRET_VALUE "hunter2"
#define SUPPORTABILITY_SECRET_VALUE_LEN 7U
#define SUPPORTABILITY_SAFE_KEY "host"
#define SUPPORTABILITY_SAFE_KEY_LEN 4U
#define SUPPORTABILITY_SAFE_VALUE "local"
#define SUPPORTABILITY_SAFE_VALUE_LEN 5U
#define SUPPORTABILITY_SAFE_KEY2 "schema"
#define SUPPORTABILITY_SAFE_KEY2_LEN 6U
#define SUPPORTABILITY_SAFE_VALUE2 "v1"
#define SUPPORTABILITY_SAFE_VALUE2_LEN 2U

#define SUPPORTABILITY_METRICS_PAYLOAD_OFFSET \
    (SUPPORT_BUNDLE_EXPORTER_HEADER_SIZE + \
     SUPPORT_BUNDLE_MANIFEST_SIZE)
#define SUPPORTABILITY_METRICS_SERVICE_READY_OFFSET 149U
#define SUPPORTABILITY_METRICS_KNOWN_GOOD_OFFSET 205U
#define SUPPORTABILITY_METRICS_PENDING_OFFSET 209U
#define SUPPORTABILITY_METRICS_ACCEPTANCES_OFFSET 221U

static struct metrics_store supportability_metrics_store;
static struct metrics_snapshot supportability_metrics;
static struct health_check_store supportability_health_store;
static struct health_check_snapshot supportability_health;
static struct appliance_status_snapshot supportability_status;
static struct appliance_status_snapshot supportability_status_source;
static struct appliance_status_snapshot supportability_recovered;
static struct appliance_status_snapshot supportability_recovered_source;
static struct audit_record supportability_audit[
    SUPPORTABILITY_AUDIT_COUNT];
static struct config_kv_entry supportability_config_entries[
    SUPPORTABILITY_CONFIG_CAPACITY];
static struct config_kv supportability_config;
static struct support_bundle_manifest_source supportability_source;
static unsigned char supportability_stream[
    SUPPORTABILITY_STREAM_CAPACITY];

static void supportability_qemu_fail(const char *message)
    __attribute__((noreturn));

static void supportability_qemu_fail(const char *message)
{
    {
        const uint64_t serial_flags = serial_sequence_begin();
        serial_write("[SUPPORTABILITY] failed: ");
        serial_write(message);
        serial_write("\n");
        serial_sequence_end(serial_flags);
    }
    for (;;) {
        __asm__ volatile ("hlt" : : : "memory");
    }
}

static void supportability_qemu_require(bool condition,
                                        const char *message)
{
    if (!condition) {
        supportability_qemu_fail(message);
    }
}

static void supportability_qemu_zero(void *destination, size_t length)
{
    unsigned char *const bytes = destination;
    size_t index;

    for (index = 0U; index < length; ++index) {
        bytes[index] = 0U;
    }
}

static void supportability_qemu_copy(void *destination,
                                     const void *source,
                                     size_t length)
{
    unsigned char *const out = destination;
    const unsigned char *const in = source;
    size_t index;

    for (index = 0U; index < length; ++index) {
        out[index] = in[index];
    }
}

static bool supportability_qemu_all_zero(const unsigned char *bytes,
                                         size_t length)
{
    size_t index;

    for (index = 0U; index < length; ++index) {
        if (bytes[index] != 0U) {
            return false;
        }
    }
    return true;
}

static bool supportability_qemu_contains(const void *data,
                                         size_t data_length,
                                         const void *needle,
                                         size_t needle_length)
{
    const unsigned char *const bytes = data;
    const unsigned char *const pattern = needle;
    size_t base;
    size_t index;

    if (needle_length == 0U || data_length < needle_length) {
        return false;
    }
    for (base = 0U;
         base + needle_length <= data_length;
         ++base) {
        for (index = 0U; index < needle_length; ++index) {
            if (bytes[base + index] != pattern[index]) {
                break;
            }
        }
        if (index == needle_length) {
            return true;
        }
    }
    return false;
}

static bool supportability_qemu_contains_string(const char *text,
                                                const char *needle)
{
    size_t text_length = 0U;
    size_t needle_length = 0U;

    while (text[text_length] != '\0') {
        ++text_length;
    }
    while (needle[needle_length] != '\0') {
        ++needle_length;
    }
    return supportability_qemu_contains(
        text, text_length, needle, needle_length);
}

static uint32_t supportability_qemu_load_le32(
    const unsigned char *bytes)
{
    return (uint32_t)bytes[0] |
           ((uint32_t)bytes[1] << 8U) |
           ((uint32_t)bytes[2] << 16U) |
           ((uint32_t)bytes[3] << 24U);
}

static uint64_t supportability_qemu_load_le64(
    const unsigned char *bytes)
{
    uint64_t value = 0U;
    size_t index;

    for (index = 0U; index < 8U; ++index) {
        value |= (uint64_t)bytes[index] << (8U * index);
    }
    return value;
}

static void supportability_qemu_publish_health(void)
{
    struct health_check_descriptor descriptors[
        SUPPORTABILITY_CHECK_COUNT];
    size_t index;

    for (index = 0U; index < SUPPORTABILITY_CHECK_COUNT; ++index) {
        supportability_qemu_zero(
            &descriptors[index], sizeof(descriptors[index]));
        descriptors[index].version = HEALTH_CHECK_VERSION;
        descriptors[index].check_id = (uint32_t)(index + 1U);
        descriptors[index].cadence_ms = 5000U;
    }
    descriptors[0].severity = HEALTH_CHECK_SEVERITY_CRITICAL;
    descriptors[1].severity = HEALTH_CHECK_SEVERITY_WARNING;
    descriptors[2].severity = HEALTH_CHECK_SEVERITY_INFO;
    descriptors[3].severity = HEALTH_CHECK_SEVERITY_CRITICAL;
    descriptors[4].severity = HEALTH_CHECK_SEVERITY_WARNING;

    supportability_qemu_require(
        health_check_init(&supportability_health_store) ==
            HEALTH_CHECK_OK,
        "health store init");
    supportability_qemu_require(
        health_check_publish(
            &supportability_health_store, descriptors,
            SUPPORTABILITY_CHECK_COUNT) == HEALTH_CHECK_OK,
        "health descriptor publication");
    supportability_qemu_require(
        health_check_result(
            &supportability_health_store,
            SUPPORTABILITY_CHECK_STORAGE,
            HEALTH_CHECK_RESULT_FAIL) == HEALTH_CHECK_OK &&
        health_check_result(
            &supportability_health_store,
            SUPPORTABILITY_CHECK_NETWORK,
            HEALTH_CHECK_RESULT_WARN) == HEALTH_CHECK_OK &&
        health_check_result(
            &supportability_health_store,
            SUPPORTABILITY_CHECK_AUDIT,
            HEALTH_CHECK_RESULT_FAIL) == HEALTH_CHECK_OK,
        "degraded health results");
    supportability_qemu_require(
        health_check_snapshot(
            &supportability_health_store,
            &supportability_health) == HEALTH_CHECK_OK &&
        supportability_health.descriptor_count ==
            SUPPORTABILITY_CHECK_COUNT &&
        supportability_health.entries[0].result ==
            HEALTH_CHECK_RESULT_FAIL &&
        supportability_health.entries[2].result ==
            HEALTH_CHECK_RESULT_WARN &&
        supportability_health.entries[3].result ==
            HEALTH_CHECK_RESULT_FAIL,
        "degraded health snapshot");
}

static void supportability_qemu_publish_metrics(void)
{
    struct metrics_health_values health;
    struct metrics_state_values state;
    struct metrics_service_values service;
    struct metrics_update_values update;

    supportability_qemu_zero(&health, sizeof(health));
    health.version = METRICS_VERSION;
    health.check_count = SUPPORTABILITY_CHECK_COUNT;
    health.pass_count = 2U;
    health.warn_count = 1U;
    health.fail_count = 2U;
    health.ring_published = true;
    health.ring_count = 16U;
    health.ring_available = 48U;
    health.ring_utilization_percent = 25U;

    supportability_qemu_zero(&state, sizeof(state));
    state.version = METRICS_VERSION;
    state.generation = 1U;
    state.journal_generation = 4U;
    state.checkpoint_sequence = 9U;
    state.schema_version = 1U;
    state.active_generation = 1U;
    state.backup_exports = 1U;

    supportability_qemu_zero(&service, sizeof(service));
    service.version = METRICS_VERSION;
    service.generation = 1U;
    service.service_count = 2U;
    service.ready_count = 1U;
    service.pending_count = 1U;
    service.restarts = 1U;

    supportability_qemu_zero(&update, sizeof(update));
    update.version = METRICS_VERSION;
    update.generation = 1U;
    update.inventory_generation = 3U;
    update.slot_count = 2U;
    update.known_good_slot = 1U;
    update.pending_slot = 2U;
    update.rollback_slot = 1U;
    update.candidate_pending = true;
    update.acceptances = 1U;
    update.rejections = 1U;

    supportability_qemu_require(
        metrics_init(&supportability_metrics_store) == METRICS_OK &&
        metrics_begin_update(&supportability_metrics_store) ==
            METRICS_OK &&
        metrics_set_health(
            &supportability_metrics_store, &health) == METRICS_OK &&
        metrics_set_state(
            &supportability_metrics_store, &state) == METRICS_OK &&
        metrics_set_service(
            &supportability_metrics_store, &service) == METRICS_OK &&
        metrics_set_update(
            &supportability_metrics_store, &update) == METRICS_OK &&
        metrics_commit(&supportability_metrics_store) == METRICS_OK &&
        metrics_snapshot(
            &supportability_metrics_store,
            &supportability_metrics) == METRICS_OK &&
        supportability_metrics.generation == 1U &&
        supportability_metrics.paths.service.ready_count == 1U &&
        supportability_metrics.paths.update.known_good_slot == 1U,
        "coherent metrics publication");
}

static void supportability_qemu_publish_status(void)
{
    supportability_qemu_zero(
        &supportability_status_source,
        sizeof(supportability_status_source));
    supportability_status_source.version = APPLIANCE_STATUS_VERSION;
    supportability_status_source.state.version =
        APPLIANCE_STATUS_VERSION;
    supportability_status_source.state.generation = 1U;
    supportability_status_source.state.uptime_ticks = 42U;
    supportability_status_source.state.config_revision = 2U;
    supportability_status_source.state.audit_sequence = 100U;
    supportability_status_source.storage.version =
        APPLIANCE_STATUS_VERSION;
    supportability_status_source.storage.journal_generation = 4U;
    supportability_status_source.storage.backup_generation = 1U;
    supportability_status_source.storage.schema_version = 1U;
    supportability_status_source.storage.state =
        APPLIANCE_STORAGE_OK;
    supportability_status_source.health.version =
        APPLIANCE_STATUS_VERSION;
    supportability_status_source.health.state =
        APPLIANCE_HEALTH_DEGRADED;
    supportability_status_source.health.ready_count = 2U;
    supportability_status_source.health.failed_count = 2U;
    supportability_status_source.service_count = 2U;
    supportability_status_source.services[0].id = 1U;
    supportability_status_source.services[0].state =
        APPLIANCE_SERVICE_READY;
    supportability_status_source.services[0].health =
        APPLIANCE_SERVICE_HEALTH_READY;
    supportability_status_source.services[1].id = 2U;
    supportability_status_source.services[1].state =
        APPLIANCE_SERVICE_RUNNING;
    supportability_status_source.services[1].health =
        APPLIANCE_SERVICE_HEALTH_PENDING;
    supportability_status_source.services[1].restart_count = 1U;

    supportability_qemu_require(
        appliance_status_snapshot_init(
            &supportability_status,
            &supportability_status_source) == APPLIANCE_STATUS_OK &&
        supportability_status.published &&
        supportability_status.health.state ==
            APPLIANCE_HEALTH_DEGRADED,
        "degraded appliance status publication");
}

static void supportability_qemu_build_config(void)
{
    bool replaced;

    supportability_qemu_require(
        config_kv_init(
            &supportability_config,
            supportability_config_entries,
            SUPPORTABILITY_CONFIG_CAPACITY) == CONFIG_KV_OK &&
        config_kv_put(
            &supportability_config, CONFIG_KV_KIND_CONFIG,
            SUPPORTABILITY_SAFE_KEY,
            SUPPORTABILITY_SAFE_KEY_LEN,
            SUPPORTABILITY_SAFE_VALUE,
            SUPPORTABILITY_SAFE_VALUE_LEN,
            &replaced) == CONFIG_KV_OK &&
        config_kv_put(
            &supportability_config, CONFIG_KV_KIND_CONFIG,
            SUPPORTABILITY_SAFE_KEY2,
            SUPPORTABILITY_SAFE_KEY2_LEN,
            SUPPORTABILITY_SAFE_VALUE2,
            SUPPORTABILITY_SAFE_VALUE2_LEN,
            &replaced) == CONFIG_KV_OK &&
        config_kv_put(
            &supportability_config, CONFIG_KV_KIND_CONFIG,
            SUPPORTABILITY_SECRET_KEY,
            SUPPORTABILITY_SECRET_KEY_LEN,
            SUPPORTABILITY_SECRET_VALUE,
            SUPPORTABILITY_SECRET_VALUE_LEN,
            &replaced) == CONFIG_KV_OK,
        "support bundle config fixture");
}

static void supportability_qemu_build_audit(void)
{
    supportability_qemu_require(
        audit_record_init(
            &supportability_audit[0], AUDIT_RECORD_PROCESS,
            1U, 0U, 1U, 0U, 2U, 7U) == AUDIT_OK &&
        audit_record_init_management(
            &supportability_audit[1], 1U, 0U, 3U, 4U, 1U, 9U) ==
            AUDIT_OK,
        "support bundle audit fixture");
    supportability_audit[0].sequence = 100U;
    supportability_audit[1].sequence = 101U;
}

static void supportability_qemu_export_bundle(
    size_t *stream_length_out)
{
    unsigned char manifest[SUPPORT_BUNDLE_MANIFEST_SIZE];
    unsigned char unit[SUPPORTABILITY_UNIT_CAPACITY];
    struct support_bundle_exporter exporter;
    size_t manifest_length = 0U;
    size_t stream_length = 0U;

    supportability_qemu_zero(
        &supportability_source,
        sizeof(supportability_source));
    supportability_source.version = SUPPORT_BUNDLE_MANIFEST_VERSION;
    supportability_source.metrics = &supportability_metrics;
    supportability_source.status = &supportability_status;
    supportability_source.health = &supportability_health;
    supportability_source.audit_records = supportability_audit;
    supportability_source.audit_count = SUPPORTABILITY_AUDIT_COUNT;
    supportability_source.config = &supportability_config;

    supportability_qemu_require(
        support_bundle_manifest_export(
            &supportability_source, manifest, sizeof(manifest),
            &manifest_length) == SUPPORT_BUNDLE_MANIFEST_OK &&
        manifest_length == SUPPORT_BUNDLE_MANIFEST_SIZE,
        "support bundle manifest export");
    supportability_qemu_require(
        support_bundle_exporter_init(
            &exporter, &supportability_source) ==
            SUPPORT_BUNDLE_EXPORTER_OK,
        "support bundle exporter init");

    for (;;) {
        enum support_bundle_exporter_status status;
        size_t unit_length = 0U;

        status = support_bundle_exporter_next(
            &exporter, unit, sizeof(unit), &unit_length);
        if (status == SUPPORT_BUNDLE_EXPORTER_DONE) {
            break;
        }
        supportability_qemu_require(
            status == SUPPORT_BUNDLE_EXPORTER_OK,
            support_bundle_exporter_status_string(status));
        supportability_qemu_require(
            stream_length + unit_length <=
                SUPPORTABILITY_STREAM_CAPACITY,
            "support bundle stream capacity");
        supportability_qemu_copy(
            supportability_stream + stream_length,
            unit, unit_length);
        stream_length += unit_length;
    }
    supportability_qemu_require(
        stream_length == (size_t)exporter.total_bytes &&
        stream_length >=
            SUPPORT_BUNDLE_EXPORTER_HEADER_SIZE +
            SUPPORT_BUNDLE_MANIFEST_SIZE +
            SUPPORT_BUNDLE_EXPORTER_DIGEST_SIZE,
        "support bundle stream length");
    supportability_qemu_require(
        support_bundle_exporter_close(&exporter) ==
            SUPPORT_BUNDLE_EXPORTER_OK,
        "support bundle exporter close");
    *stream_length_out = stream_length;
}

static void supportability_qemu_verify_evidence(
    size_t stream_length)
{
    const unsigned char *metrics_payload =
        supportability_stream + SUPPORTABILITY_METRICS_PAYLOAD_OFFSET;

    supportability_qemu_require(
        stream_length >= SUPPORTABILITY_METRICS_PAYLOAD_OFFSET +
            245U,
        "support bundle metrics payload present");
    supportability_qemu_require(
        supportability_qemu_load_le32(
            metrics_payload +
            SUPPORTABILITY_METRICS_SERVICE_READY_OFFSET) == 1U &&
        supportability_qemu_load_le32(
            metrics_payload +
            SUPPORTABILITY_METRICS_KNOWN_GOOD_OFFSET) == 1U &&
        supportability_qemu_load_le32(
            metrics_payload +
            SUPPORTABILITY_METRICS_PENDING_OFFSET) == 2U &&
        supportability_qemu_load_le64(
            metrics_payload +
            SUPPORTABILITY_METRICS_ACCEPTANCES_OFFSET) == 1U,
        "exported update/service evidence");
}

static void supportability_qemu_recover_health(void)
{
    char output[APPLIANCE_STATUS_OUTPUT_MAX + 1U];
    size_t output_length = 0U;
    size_t index;

    for (index = 0U;
         index < supportability_health.descriptor_count;
         ++index) {
        supportability_qemu_require(
            health_check_result(
                &supportability_health_store,
                supportability_health.entries[index]
                    .descriptor.check_id,
                HEALTH_CHECK_RESULT_PASS) == HEALTH_CHECK_OK,
            "healthy health result");
    }
    supportability_qemu_require(
        health_check_snapshot(
            &supportability_health_store,
            &supportability_health) == HEALTH_CHECK_OK,
        "healthy health snapshot");
    for (index = 0U;
         index < supportability_health.descriptor_count;
         ++index) {
        supportability_qemu_require(
            supportability_health.entries[index].result ==
                HEALTH_CHECK_RESULT_PASS,
            "healthy health results");
    }

    supportability_qemu_zero(
        &supportability_recovered_source,
        sizeof(supportability_recovered_source));
    supportability_recovered_source.version =
        APPLIANCE_STATUS_VERSION;
    supportability_recovered_source.state.version =
        APPLIANCE_STATUS_VERSION;
    supportability_recovered_source.state.generation = 2U;
    supportability_recovered_source.state.uptime_ticks = 84U;
    supportability_recovered_source.state.config_revision = 3U;
    supportability_recovered_source.state.audit_sequence = 101U;
    supportability_recovered_source.storage.version =
        APPLIANCE_STATUS_VERSION;
    supportability_recovered_source.storage.journal_generation = 4U;
    supportability_recovered_source.storage.backup_generation = 1U;
    supportability_recovered_source.storage.schema_version = 1U;
    supportability_recovered_source.storage.state =
        APPLIANCE_STORAGE_OK;
    supportability_recovered_source.health.version =
        APPLIANCE_STATUS_VERSION;
    supportability_recovered_source.health.state =
        APPLIANCE_HEALTH_OK;
    supportability_recovered_source.health.ready_count = 5U;
    supportability_recovered_source.service_count = 2U;
    supportability_recovered_source.services[0].id = 1U;
    supportability_recovered_source.services[0].state =
        APPLIANCE_SERVICE_READY;
    supportability_recovered_source.services[0].health =
        APPLIANCE_SERVICE_HEALTH_READY;
    supportability_recovered_source.services[1].id = 2U;
    supportability_recovered_source.services[1].state =
        APPLIANCE_SERVICE_READY;
    supportability_recovered_source.services[1].health =
        APPLIANCE_SERVICE_HEALTH_READY;

    supportability_qemu_require(
        appliance_status_snapshot_init(
            &supportability_recovered,
            &supportability_recovered_source) ==
            APPLIANCE_STATUS_OK &&
        supportability_recovered.health.state ==
            APPLIANCE_HEALTH_OK,
        "healthy appliance status publication");
    supportability_qemu_zero(output, sizeof(output));
    supportability_qemu_require(
        appliance_status_render(
            &supportability_recovered,
            output, sizeof(output), &output_length) ==
            APPLIANCE_STATUS_OK &&
        output_length > 0U &&
        supportability_qemu_contains_string(
            output, "health st=0 r=5 p=0 f=0") &&
        supportability_qemu_contains_string(
            output, "services=2"),
        "healthy bounded status exposure");
}

void supportability_qemu_test_init(void)
{
    char status_output[APPLIANCE_STATUS_OUTPUT_MAX + 1U];
    size_t status_output_length = 0U;
    unsigned char redacted[32];
    size_t redacted_length = 0U;
    enum diagnostic_privacy_category category =
        DIAGNOSTIC_PRIVACY_CATEGORY_NONE;
    size_t stream_length = 0U;

    supportability_qemu_publish_health();
    supportability_qemu_publish_metrics();
    supportability_qemu_publish_status();
    supportability_qemu_build_config();
    supportability_qemu_build_audit();

    serial_write("[SUPPORTABILITY] proof init\n");
    serial_write(
        "[SUPPORTABILITY] degraded health exposed\n");

    supportability_qemu_zero(
        status_output, sizeof(status_output));
    supportability_qemu_require(
        appliance_status_render(
            &supportability_status,
            status_output, sizeof(status_output),
            &status_output_length) == APPLIANCE_STATUS_OK &&
        status_output_length > 0U &&
        supportability_qemu_contains_string(
            status_output, "health st=1 r=2 p=0 f=2") &&
        supportability_qemu_contains_string(
            status_output, "services=2") &&
        supportability_qemu_contains_string(
            status_output, "1:2/1/0") &&
        supportability_qemu_contains_string(
            status_output, "2:1/0/1"),
        "degraded bounded status exposure");
    serial_write(
        "[SUPPORTABILITY] status exposure bounded\n");

    supportability_qemu_export_bundle(&stream_length);
    serial_write(
        "[SUPPORTABILITY] support bundle exported\n");

    supportability_qemu_require(
        diagnostic_privacy_classify_key(
            SUPPORTABILITY_SECRET_KEY,
            SUPPORTABILITY_SECRET_KEY_LEN,
            &category) == DIAGNOSTIC_PRIVACY_OK &&
        category == DIAGNOSTIC_PRIVACY_CATEGORY_CREDENTIAL,
        "secret config key classification");
    supportability_qemu_require(
        diagnostic_privacy_redact(
            DIAGNOSTIC_PRIVACY_CATEGORY_CREDENTIAL,
            SUPPORTABILITY_SECRET_VALUE,
            SUPPORTABILITY_SECRET_VALUE_LEN,
            redacted, sizeof(redacted),
            &redacted_length) == DIAGNOSTIC_PRIVACY_OK &&
        redacted_length == SUPPORTABILITY_SECRET_VALUE_LEN &&
        supportability_qemu_all_zero(
            redacted, SUPPORTABILITY_SECRET_VALUE_LEN),
        "credential redaction");
    supportability_qemu_require(
        !supportability_qemu_contains(
            supportability_stream, stream_length,
            SUPPORTABILITY_SECRET_VALUE,
            SUPPORTABILITY_SECRET_VALUE_LEN) &&
        !supportability_qemu_contains(
            supportability_stream, stream_length,
            SUPPORTABILITY_SECRET_KEY,
            SUPPORTABILITY_SECRET_KEY_LEN),
        "support bundle secret exclusion");
    serial_write(
        "[SUPPORTABILITY] redaction enforced\n");

    supportability_qemu_verify_evidence(stream_length);
    serial_write(
        "[SUPPORTABILITY] update/service evidence preserved\n");

    supportability_qemu_recover_health();
    serial_write(
        "[SUPPORTABILITY] normal recovery healthy\n");

    serial_write("[SUPPORTABILITY] clean reboot\n");
    serial_write("[SUPPORTABILITY] proof OK\n");
}

#else
typedef int supportability_qemu_translation_unit_guard;
#endif
