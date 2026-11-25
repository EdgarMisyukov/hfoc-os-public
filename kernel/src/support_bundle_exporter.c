#include "support_bundle_exporter.h"

#include "config_kv.h"
#include "config_schema.h"
#include "diagnostic_privacy.h"
#include "sha256.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define SUPPORT_BUNDLE_EXPORTER_PHASE_HEADER 0U
#define SUPPORT_BUNDLE_EXPORTER_PHASE_MANIFEST 1U
#define SUPPORT_BUNDLE_EXPORTER_PHASE_ENTRY_BASE 2U
#define SUPPORT_BUNDLE_EXPORTER_PHASE_DIGEST \
    (SUPPORT_BUNDLE_EXPORTER_PHASE_ENTRY_BASE + \
     SUPPORT_BUNDLE_MANIFEST_ENTRY_MAX)
#define SUPPORT_BUNDLE_EXPORTER_PHASE_DONE \
    (SUPPORT_BUNDLE_EXPORTER_PHASE_DIGEST + 1U)

#define SUPPORT_BUNDLE_EXPORTER_METRICS_LENGTH 245U
#define SUPPORT_BUNDLE_EXPORTER_HEALTH_LENGTH 336U
#define SUPPORT_BUNDLE_EXPORTER_AUDIT_RECORD_LENGTH 52U
#define SUPPORT_BUNDLE_EXPORTER_VERSIONS_LENGTH \
    (6U * 4U + SUPPORT_BUNDLE_MANIFEST_SECRET_POLICY_ID_LENGTH)

static const unsigned char support_bundle_exporter_magic[
    SUPPORT_BUNDLE_EXPORTER_MAGIC_SIZE] = {
    'H', 'F', 'O', 'C', 'S', 'B', 'E', 'X'
};

static void support_bundle_exporter_zero_bytes(
    unsigned char *bytes,
    size_t length)
{
    size_t index;

    for (index = 0U; index < length; ++index) {
        bytes[index] = 0U;
    }
}

static void support_bundle_exporter_copy_bytes(
    unsigned char *destination,
    const unsigned char *source,
    size_t length)
{
    size_t index;

    for (index = 0U; index < length; ++index) {
        destination[index] = source[index];
    }
}

static void support_bundle_exporter_store_le16(
    unsigned char *bytes,
    uint16_t value)
{
    bytes[0] = (unsigned char)(value);
    bytes[1] = (unsigned char)(value >> 8U);
}

static void support_bundle_exporter_store_le32(
    unsigned char *bytes,
    uint32_t value)
{
    bytes[0] = (unsigned char)(value);
    bytes[1] = (unsigned char)(value >> 8U);
    bytes[2] = (unsigned char)(value >> 16U);
    bytes[3] = (unsigned char)(value >> 24U);
}

static void support_bundle_exporter_store_le64(
    unsigned char *bytes,
    uint64_t value)
{
    size_t index;

    for (index = 0U; index < 8U; ++index) {
        bytes[index] = (unsigned char)(value >> (8U * index));
    }
}

static size_t support_bundle_exporter_status_length(
    const struct appliance_status_snapshot *snapshot)
{
    return 4U + 1U + 44U + 28U + 20U + 4U +
           (size_t)snapshot->service_count * 16U;
}

static bool support_bundle_exporter_entry_secret(
    const struct config_kv_entry *entry)
{
    enum diagnostic_privacy_category category;

    return diagnostic_privacy_classify_key(
               entry->key, entry->key_length, &category) ==
               DIAGNOSTIC_PRIVACY_OK &&
           category == DIAGNOSTIC_PRIVACY_CATEGORY_CREDENTIAL;
}

static size_t support_bundle_exporter_config_length(
    const struct config_kv *kv)
{
    size_t length = 0U;
    size_t index;

    for (index = 0U; index < kv->capacity; ++index) {
        const struct config_kv_entry *entry = &kv->entries[index];

        if (entry->occupied &&
            entry->kind == CONFIG_KV_KIND_CONFIG &&
            !support_bundle_exporter_entry_secret(entry)) {
            length += 12U + entry->key_length + entry->value_length;
        }
    }
    return length;
}

static size_t support_bundle_exporter_payload_length(
    const struct support_bundle_manifest_source *source,
    uint32_t entry)
{
    switch (entry) {
    case SUPPORT_BUNDLE_ENTRY_METRICS:
        return SUPPORT_BUNDLE_EXPORTER_METRICS_LENGTH;
    case SUPPORT_BUNDLE_ENTRY_APPLIANCE_STATUS:
        return support_bundle_exporter_status_length(source->status);
    case SUPPORT_BUNDLE_ENTRY_HEALTH:
        return SUPPORT_BUNDLE_EXPORTER_HEALTH_LENGTH;
    case SUPPORT_BUNDLE_ENTRY_AUDIT_SLICE:
        return (size_t)source->audit_count *
               SUPPORT_BUNDLE_EXPORTER_AUDIT_RECORD_LENGTH;
    case SUPPORT_BUNDLE_ENTRY_CONFIG_METADATA:
        return support_bundle_exporter_config_length(source->config);
    case SUPPORT_BUNDLE_ENTRY_VERSIONS:
        return SUPPORT_BUNDLE_EXPORTER_VERSIONS_LENGTH;
    default:
        return 0U;
    }
}

static void support_bundle_exporter_write_header(
    unsigned char *output,
    const struct support_bundle_exporter *exporter)
{
    support_bundle_exporter_zero_bytes(
        output, SUPPORT_BUNDLE_EXPORTER_HEADER_SIZE);
    support_bundle_exporter_copy_bytes(
        output, support_bundle_exporter_magic,
        SUPPORT_BUNDLE_EXPORTER_MAGIC_SIZE);
    output[SUPPORT_BUNDLE_EXPORTER_VERSION_OFFSET] =
        SUPPORT_BUNDLE_EXPORTER_VERSION;
    support_bundle_exporter_copy_bytes(
        output + SUPPORT_BUNDLE_EXPORTER_SCHEMA_ID_OFFSET,
        (const unsigned char *)SUPPORT_BUNDLE_EXPORTER_SCHEMA_ID,
        SUPPORT_BUNDLE_EXPORTER_SCHEMA_ID_LENGTH);
    support_bundle_exporter_store_le32(
        output + SUPPORT_BUNDLE_EXPORTER_HEADER_MANIFEST_SIZE_OFFSET,
        SUPPORT_BUNDLE_EXPORTER_MANIFEST_SIZE);
    support_bundle_exporter_store_le32(
        output + SUPPORT_BUNDLE_EXPORTER_HEADER_ENTRY_COUNT_OFFSET,
        SUPPORT_BUNDLE_MANIFEST_ENTRY_MAX);
    support_bundle_exporter_store_le32(
        output + SUPPORT_BUNDLE_EXPORTER_HEADER_EXPORTER_VERSION_OFFSET,
        SUPPORT_BUNDLE_EXPORTER_VERSION);
    support_bundle_exporter_store_le32(
        output + SUPPORT_BUNDLE_EXPORTER_HEADER_MANIFEST_VERSION_OFFSET,
        SUPPORT_BUNDLE_MANIFEST_VERSION);
    support_bundle_exporter_store_le64(
        output + SUPPORT_BUNDLE_EXPORTER_HEADER_PAYLOAD_BYTES_OFFSET,
        exporter->payload_bytes);
    support_bundle_exporter_store_le64(
        output + SUPPORT_BUNDLE_EXPORTER_HEADER_TOTAL_BYTES_OFFSET,
        exporter->total_bytes);
}

static void support_bundle_exporter_write_metrics(
    const struct metrics_snapshot *snapshot,
    unsigned char *output)
{
    const struct metrics_health_values *health = &snapshot->paths.health;
    const struct metrics_state_values *state = &snapshot->paths.state;
    const struct metrics_service_values *service =
        &snapshot->paths.service;
    const struct metrics_update_values *update =
        &snapshot->paths.update;
    size_t offset = 0U;

    support_bundle_exporter_store_le32(output + offset,
                                       snapshot->version);
    offset += 4U;
    support_bundle_exporter_store_le64(output + offset,
                                       snapshot->generation);
    offset += 8U;
    support_bundle_exporter_store_le32(output + offset,
                                       health->version);
    offset += 4U;
    support_bundle_exporter_store_le32(output + offset,
                                       health->check_count);
    offset += 4U;
    support_bundle_exporter_store_le64(output + offset,
                                       health->pass_count);
    offset += 8U;
    support_bundle_exporter_store_le64(output + offset,
                                       health->warn_count);
    offset += 8U;
    support_bundle_exporter_store_le64(output + offset,
                                       health->fail_count);
    offset += 8U;
    support_bundle_exporter_store_le64(output + offset,
                                       health->unknown_count);
    offset += 8U;
    output[offset] = health->ring_published ? 1U : 0U;
    offset += 1U;
    support_bundle_exporter_store_le32(output + offset,
                                       health->ring_count);
    offset += 4U;
    support_bundle_exporter_store_le32(output + offset,
                                       health->ring_available);
    offset += 4U;
    support_bundle_exporter_store_le32(
        output + offset, health->ring_utilization_percent);
    offset += 4U;

    support_bundle_exporter_store_le32(output + offset, state->version);
    offset += 4U;
    support_bundle_exporter_store_le64(output + offset,
                                       state->generation);
    offset += 8U;
    support_bundle_exporter_store_le64(
        output + offset, state->journal_generation);
    offset += 8U;
    support_bundle_exporter_store_le64(
        output + offset, state->checkpoint_sequence);
    offset += 8U;
    support_bundle_exporter_store_le32(output + offset,
                                       state->schema_version);
    offset += 4U;
    support_bundle_exporter_store_le32(output + offset,
                                       state->active_generation);
    offset += 4U;
    support_bundle_exporter_store_le64(output + offset,
                                       state->migrations);
    offset += 8U;
    support_bundle_exporter_store_le64(output + offset,
                                       state->compactions);
    offset += 8U;
    support_bundle_exporter_store_le64(output + offset,
                                       state->restores);
    offset += 8U;
    support_bundle_exporter_store_le64(output + offset,
                                       state->backup_exports);
    offset += 8U;

    support_bundle_exporter_store_le32(output + offset,
                                       service->version);
    offset += 4U;
    support_bundle_exporter_store_le64(output + offset,
                                       service->generation);
    offset += 8U;
    support_bundle_exporter_store_le32(output + offset,
                                       service->service_count);
    offset += 4U;
    support_bundle_exporter_store_le32(output + offset,
                                       service->ready_count);
    offset += 4U;
    support_bundle_exporter_store_le32(output + offset,
                                       service->pending_count);
    offset += 4U;
    support_bundle_exporter_store_le32(output + offset,
                                       service->failed_count);
    offset += 4U;
    support_bundle_exporter_store_le32(output + offset,
                                       service->exhausted_count);
    offset += 4U;
    support_bundle_exporter_store_le32(
        output + offset, service->dependency_blocked_count);
    offset += 4U;
    support_bundle_exporter_store_le64(output + offset,
                                       service->restarts);
    offset += 8U;
    support_bundle_exporter_store_le64(output + offset,
                                       service->exits);
    offset += 8U;

    support_bundle_exporter_store_le32(output + offset,
                                       update->version);
    offset += 4U;
    support_bundle_exporter_store_le64(output + offset,
                                       update->generation);
    offset += 8U;
    support_bundle_exporter_store_le32(
        output + offset, update->inventory_generation);
    offset += 4U;
    support_bundle_exporter_store_le32(output + offset,
                                       update->slot_count);
    offset += 4U;
    support_bundle_exporter_store_le32(output + offset,
                                       update->known_good_slot);
    offset += 4U;
    support_bundle_exporter_store_le32(output + offset,
                                       update->pending_slot);
    offset += 4U;
    support_bundle_exporter_store_le32(output + offset,
                                       update->rollback_slot);
    offset += 4U;
    output[offset] = update->candidate_pending ? 1U : 0U;
    offset += 1U;
    output[offset] = update->candidate_active ? 1U : 0U;
    offset += 1U;
    output[offset] = update->accepted ? 1U : 0U;
    offset += 1U;
    output[offset] = update->rollback_published ? 1U : 0U;
    offset += 1U;
    support_bundle_exporter_store_le64(output + offset,
                                       update->acceptances);
    offset += 8U;
    support_bundle_exporter_store_le64(output + offset,
                                       update->rollbacks);
    offset += 8U;
    support_bundle_exporter_store_le64(output + offset,
                                       update->rejections);
}

static void support_bundle_exporter_write_status(
    const struct appliance_status_snapshot *snapshot,
    unsigned char *output)
{
    const struct appliance_status_state_snapshot *state =
        &snapshot->state;
    const struct appliance_status_storage_snapshot *storage =
        &snapshot->storage;
    const struct appliance_status_health_snapshot *health =
        &snapshot->health;
    size_t offset = 0U;
    uint32_t index;

    support_bundle_exporter_store_le32(output + offset,
                                       snapshot->version);
    offset += 4U;
    output[offset] = snapshot->published ? 1U : 0U;
    offset += 1U;
    support_bundle_exporter_store_le32(output + offset,
                                       state->version);
    offset += 4U;
    support_bundle_exporter_store_le64(output + offset,
                                       state->generation);
    offset += 8U;
    support_bundle_exporter_store_le64(output + offset,
                                       state->uptime_ticks);
    offset += 8U;
    support_bundle_exporter_store_le32(output + offset,
                                       state->recovery_mode);
    offset += 4U;
    support_bundle_exporter_store_le64(output + offset,
                                       state->config_revision);
    offset += 8U;
    support_bundle_exporter_store_le64(output + offset,
                                       state->audit_sequence);
    offset += 8U;
    support_bundle_exporter_store_le32(output + offset,
                                       state->active_process_count);
    offset += 4U;

    support_bundle_exporter_store_le32(output + offset,
                                       storage->version);
    offset += 4U;
    support_bundle_exporter_store_le64(
        output + offset, storage->journal_generation);
    offset += 8U;
    support_bundle_exporter_store_le64(
        output + offset, storage->backup_generation);
    offset += 8U;
    support_bundle_exporter_store_le32(output + offset,
                                       storage->schema_version);
    offset += 4U;
    support_bundle_exporter_store_le32(output + offset,
                                       (uint32_t)storage->state);
    offset += 4U;

    support_bundle_exporter_store_le32(output + offset,
                                       health->version);
    offset += 4U;
    support_bundle_exporter_store_le32(output + offset,
                                       (uint32_t)health->state);
    offset += 4U;
    support_bundle_exporter_store_le32(output + offset,
                                       health->ready_count);
    offset += 4U;
    support_bundle_exporter_store_le32(output + offset,
                                       health->pending_count);
    offset += 4U;
    support_bundle_exporter_store_le32(output + offset,
                                       health->failed_count);
    offset += 4U;

    support_bundle_exporter_store_le32(output + offset,
                                       snapshot->service_count);
    offset += 4U;
    for (index = 0U; index < snapshot->service_count; ++index) {
        const struct appliance_status_service_entry *entry =
            &snapshot->services[index];

        support_bundle_exporter_store_le32(output + offset, entry->id);
        offset += 4U;
        support_bundle_exporter_store_le32(
            output + offset, (uint32_t)entry->state);
        offset += 4U;
        support_bundle_exporter_store_le32(
            output + offset, (uint32_t)entry->health);
        offset += 4U;
        support_bundle_exporter_store_le32(output + offset,
                                           entry->restart_count);
        offset += 4U;
    }
}

static void support_bundle_exporter_write_health(
    const struct health_check_snapshot *snapshot,
    unsigned char *output)
{
    size_t offset = 0U;
    size_t index;

    support_bundle_exporter_store_le32(output + offset,
                                       snapshot->version);
    offset += 4U;
    support_bundle_exporter_store_le32(output + offset,
                                       snapshot->generation);
    offset += 4U;
    support_bundle_exporter_store_le64(
        output + offset, (uint64_t)snapshot->descriptor_count);
    offset += 8U;
    for (index = 0U; index < HEALTH_CHECK_MAX_DESCRIPTORS; ++index) {
        const struct health_check_entry *entry = &snapshot->entries[index];

        support_bundle_exporter_store_le32(
            output + offset, entry->descriptor.version);
        offset += 4U;
        support_bundle_exporter_store_le32(
            output + offset, entry->descriptor.check_id);
        offset += 4U;
        support_bundle_exporter_store_le32(
            output + offset,
            (uint32_t)entry->descriptor.severity);
        offset += 4U;
        support_bundle_exporter_store_le32(
            output + offset, entry->descriptor.cadence_ms);
        offset += 4U;
        support_bundle_exporter_store_le32(
            output + offset, (uint32_t)entry->result);
        offset += 4U;
    }
}

static void support_bundle_exporter_write_audit(
    const struct audit_record *records,
    uint32_t count,
    unsigned char *output)
{
    size_t offset = 0U;
    uint32_t index;

    for (index = 0U; index < count; ++index) {
        const struct audit_record *record = &records[index];

        support_bundle_exporter_store_le64(output + offset,
                                           record->sequence);
        offset += 8U;
        support_bundle_exporter_store_le32(output + offset,
                                           record->type);
        offset += 4U;
        support_bundle_exporter_store_le32(output + offset,
                                           record->operation);
        offset += 4U;
        support_bundle_exporter_store_le32(output + offset,
                                           record->result);
        offset += 4U;
        support_bundle_exporter_store_le32(output + offset,
                                           record->pid);
        offset += 4U;
        support_bundle_exporter_store_le32(output + offset,
                                           record->domain);
        offset += 4U;
        support_bundle_exporter_store_le32(output + offset,
                                           record->generation);
        offset += 4U;
        support_bundle_exporter_store_le32(output + offset,
                                           record->session_id);
        offset += 4U;
        support_bundle_exporter_store_le32(output + offset,
                                           record->principal_id);
        offset += 4U;
        support_bundle_exporter_store_le32(output + offset,
                                           record->command_id);
        offset += 4U;
        support_bundle_exporter_store_le64(output + offset,
                                           record->detail);
        offset += 8U;
    }
}

static bool support_bundle_exporter_config_entry_less(
    const struct config_kv_entry *left,
    const struct config_kv_entry *right)
{
    size_t index;

    if (left->kind != right->kind) {
        return left->kind < right->kind;
    }
    if (left->key_length != right->key_length) {
        return left->key_length < right->key_length;
    }
    for (index = 0U; index < left->key_length; ++index) {
        if (left->key[index] != right->key[index]) {
            return left->key[index] < right->key[index];
        }
    }
    return left->revision < right->revision;
}

static void support_bundle_exporter_config_sort(
    const struct config_kv_entry **refs,
    size_t count)
{
    size_t index;

    for (index = 1U; index < count; ++index) {
        const struct config_kv_entry *entry = refs[index];
        size_t position = index;

        while (position > 0U &&
               support_bundle_exporter_config_entry_less(
                   entry, refs[position - 1U])) {
            refs[position] = refs[position - 1U];
            --position;
        }
        refs[position] = entry;
    }
}

static void support_bundle_exporter_write_config(
    const struct config_kv *kv,
    unsigned char *output)
{
    const struct config_kv_entry *refs[CONFIG_KV_CAPACITY_MAX];
    size_t count = 0U;
    size_t offset = 0U;
    size_t index;

    for (index = 0U; index < kv->capacity; ++index) {
        const struct config_kv_entry *entry = &kv->entries[index];

        if (entry->occupied &&
            entry->kind == CONFIG_KV_KIND_CONFIG &&
            !support_bundle_exporter_entry_secret(entry)) {
            refs[count] = entry;
            ++count;
        }
    }
    support_bundle_exporter_config_sort(refs, count);

    for (index = 0U; index < count; ++index) {
        const struct config_kv_entry *entry = refs[index];

        output[offset] = entry->kind;
        offset += 1U;
        output[offset] = entry->key_length;
        offset += 1U;
        support_bundle_exporter_store_le16(
            output + offset, (uint16_t)entry->value_length);
        offset += 2U;
        support_bundle_exporter_store_le64(
            output + offset, entry->revision);
        offset += 8U;
        support_bundle_exporter_copy_bytes(
            output + offset, entry->key, entry->key_length);
        offset += entry->key_length;
        support_bundle_exporter_copy_bytes(
            output + offset, entry->value, entry->value_length);
        offset += entry->value_length;
    }
}

static void support_bundle_exporter_write_versions(unsigned char *output)
{
    size_t offset = 0U;

    support_bundle_exporter_store_le32(
        output + offset, SUPPORT_BUNDLE_MANIFEST_VERSION);
    offset += 4U;
    support_bundle_exporter_store_le32(output + offset, METRICS_VERSION);
    offset += 4U;
    support_bundle_exporter_store_le32(
        output + offset, APPLIANCE_STATUS_VERSION);
    offset += 4U;
    support_bundle_exporter_store_le32(output + offset,
                                       HEALTH_CHECK_VERSION);
    offset += 4U;
    support_bundle_exporter_store_le32(output + offset,
                                       CONFIG_SCHEMA_VERSION);
    offset += 4U;
    support_bundle_exporter_store_le32(
        output + offset, KERNEL_EVENT_RING_CAPACITY);
    offset += 4U;
    support_bundle_exporter_copy_bytes(
        output + offset,
        (const unsigned char *)
            SUPPORT_BUNDLE_MANIFEST_SECRET_POLICY_ID,
        SUPPORT_BUNDLE_MANIFEST_SECRET_POLICY_ID_LENGTH);
}

static void support_bundle_exporter_write_payload(
    const struct support_bundle_manifest_source *source,
    uint32_t entry,
    unsigned char *output)
{
    switch (entry) {
    case SUPPORT_BUNDLE_ENTRY_METRICS:
        support_bundle_exporter_write_metrics(source->metrics, output);
        break;
    case SUPPORT_BUNDLE_ENTRY_APPLIANCE_STATUS:
        support_bundle_exporter_write_status(source->status, output);
        break;
    case SUPPORT_BUNDLE_ENTRY_HEALTH:
        support_bundle_exporter_write_health(source->health, output);
        break;
    case SUPPORT_BUNDLE_ENTRY_AUDIT_SLICE:
        support_bundle_exporter_write_audit(
            source->audit_records, source->audit_count, output);
        break;
    case SUPPORT_BUNDLE_ENTRY_CONFIG_METADATA:
        support_bundle_exporter_write_config(source->config, output);
        break;
    case SUPPORT_BUNDLE_ENTRY_VERSIONS:
        support_bundle_exporter_write_versions(output);
        break;
    default:
        break;
    }
}

enum support_bundle_exporter_status support_bundle_exporter_init(
    struct support_bundle_exporter *exporter,
    const struct support_bundle_manifest_source *source)
{
    unsigned char manifest[SUPPORT_BUNDLE_MANIFEST_SIZE];
    size_t manifest_length = 0U;
    uint64_t payload_bytes = 0U;
    uint32_t entry;

    if (exporter == NULL || source == NULL) {
        return SUPPORT_BUNDLE_EXPORTER_NULL;
    }
    if (support_bundle_manifest_export(
            source, manifest, sizeof(manifest),
            &manifest_length) != SUPPORT_BUNDLE_MANIFEST_OK ||
        manifest_length != SUPPORT_BUNDLE_MANIFEST_SIZE) {
        return SUPPORT_BUNDLE_EXPORTER_BAD_MANIFEST;
    }

    memset(exporter, 0, sizeof(*exporter));
    for (entry = SUPPORT_BUNDLE_ENTRY_METRICS;
         entry <= SUPPORT_BUNDLE_ENTRY_VERSIONS;
         ++entry) {
        payload_bytes += support_bundle_exporter_payload_length(
            source, entry);
    }

    exporter->source = source;
    support_bundle_exporter_copy_bytes(
        exporter->manifest, manifest, sizeof(manifest));
    exporter->phase = SUPPORT_BUNDLE_EXPORTER_PHASE_HEADER;
    exporter->payload_bytes = payload_bytes;
    exporter->total_bytes =
        SUPPORT_BUNDLE_EXPORTER_HEADER_SIZE +
        SUPPORT_BUNDLE_EXPORTER_MANIFEST_SIZE +
        payload_bytes +
        SUPPORT_BUNDLE_EXPORTER_DIGEST_SIZE;
    if (sha256_init(&exporter->digest) != SHA256_OK) {
        memset(exporter, 0, sizeof(*exporter));
        return SUPPORT_BUNDLE_EXPORTER_BAD_STATE;
    }
    exporter->active = true;
    return SUPPORT_BUNDLE_EXPORTER_OK;
}

enum support_bundle_exporter_status support_bundle_exporter_next(
    struct support_bundle_exporter *exporter,
    void *buffer,
    size_t capacity,
    size_t *length_out)
{
    unsigned char *output = buffer;
    unsigned char digest[SHA256_DIGEST_SIZE];
    struct sha256 final_context;
    size_t unit_length = 0U;

    if (exporter == NULL || buffer == NULL || length_out == NULL) {
        return SUPPORT_BUNDLE_EXPORTER_NULL;
    }
    if (exporter->closed) {
        return exporter->done ? SUPPORT_BUNDLE_EXPORTER_DONE :
                               SUPPORT_BUNDLE_EXPORTER_BAD_STATE;
    }
    if (!exporter->active ||
        exporter->source == NULL ||
        exporter->phase > SUPPORT_BUNDLE_EXPORTER_PHASE_DONE) {
        return SUPPORT_BUNDLE_EXPORTER_BAD_STATE;
    }
    if (exporter->done) {
        return SUPPORT_BUNDLE_EXPORTER_DONE;
    }

    if (exporter->phase == SUPPORT_BUNDLE_EXPORTER_PHASE_HEADER) {
        unit_length = SUPPORT_BUNDLE_EXPORTER_HEADER_SIZE;
        if (capacity < unit_length) {
            return SUPPORT_BUNDLE_EXPORTER_TOO_SMALL;
        }
        support_bundle_exporter_write_header(output, exporter);
        if (sha256_update(&exporter->digest, output,
                          unit_length) != SHA256_OK) {
            return SUPPORT_BUNDLE_EXPORTER_BAD_STATE;
        }
        exporter->phase = SUPPORT_BUNDLE_EXPORTER_PHASE_MANIFEST;
    } else if (exporter->phase == SUPPORT_BUNDLE_EXPORTER_PHASE_MANIFEST) {
        unit_length = SUPPORT_BUNDLE_EXPORTER_MANIFEST_SIZE;
        if (capacity < unit_length) {
            return SUPPORT_BUNDLE_EXPORTER_TOO_SMALL;
        }
        support_bundle_exporter_copy_bytes(
            output, exporter->manifest, unit_length);
        if (sha256_update(&exporter->digest, output,
                          unit_length) != SHA256_OK) {
            return SUPPORT_BUNDLE_EXPORTER_BAD_STATE;
        }
        exporter->phase =
            SUPPORT_BUNDLE_EXPORTER_PHASE_ENTRY_BASE;
    } else if (exporter->phase < SUPPORT_BUNDLE_EXPORTER_PHASE_DIGEST) {
        const uint32_t entry =
            exporter->phase -
            SUPPORT_BUNDLE_EXPORTER_PHASE_ENTRY_BASE + 1U;

        unit_length = support_bundle_exporter_payload_length(
            exporter->source, entry);
        if (capacity < unit_length) {
            return SUPPORT_BUNDLE_EXPORTER_TOO_SMALL;
        }
        support_bundle_exporter_write_payload(
            exporter->source, entry, output);
        if (sha256_update(&exporter->digest, output,
                          unit_length) != SHA256_OK) {
            return SUPPORT_BUNDLE_EXPORTER_BAD_STATE;
        }
        ++exporter->phase;
    } else if (exporter->phase == SUPPORT_BUNDLE_EXPORTER_PHASE_DIGEST) {
        unit_length = SUPPORT_BUNDLE_EXPORTER_DIGEST_SIZE;
        if (capacity < unit_length) {
            return SUPPORT_BUNDLE_EXPORTER_TOO_SMALL;
        }
        final_context = exporter->digest;
        if (sha256_final(&final_context, digest) != SHA256_OK) {
            return SUPPORT_BUNDLE_EXPORTER_BAD_STATE;
        }
        support_bundle_exporter_copy_bytes(
            output, digest, sizeof(digest));
        exporter->phase = SUPPORT_BUNDLE_EXPORTER_PHASE_DONE;
        exporter->done = true;
    } else {
        return SUPPORT_BUNDLE_EXPORTER_BAD_STATE;
    }

    *length_out = unit_length;
    return SUPPORT_BUNDLE_EXPORTER_OK;
}

enum support_bundle_exporter_status support_bundle_exporter_close(
    struct support_bundle_exporter *exporter)
{
    if (exporter == NULL) {
        return SUPPORT_BUNDLE_EXPORTER_NULL;
    }
    exporter->source = NULL;
    exporter->phase = 0U;
    exporter->payload_bytes = 0U;
    exporter->total_bytes = 0U;
    exporter->active = false;
    exporter->closed = true;
    return SUPPORT_BUNDLE_EXPORTER_OK;
}

const char *support_bundle_exporter_status_string(
    enum support_bundle_exporter_status status)
{
    switch (status) {
    case SUPPORT_BUNDLE_EXPORTER_OK:
        return "SUPPORT_BUNDLE_EXPORTER_OK";
    case SUPPORT_BUNDLE_EXPORTER_NULL:
        return "SUPPORT_BUNDLE_EXPORTER_NULL";
    case SUPPORT_BUNDLE_EXPORTER_BAD_STATE:
        return "SUPPORT_BUNDLE_EXPORTER_BAD_STATE";
    case SUPPORT_BUNDLE_EXPORTER_BAD_MANIFEST:
        return "SUPPORT_BUNDLE_EXPORTER_BAD_MANIFEST";
    case SUPPORT_BUNDLE_EXPORTER_TOO_SMALL:
        return "SUPPORT_BUNDLE_EXPORTER_TOO_SMALL";
    case SUPPORT_BUNDLE_EXPORTER_DONE:
        return "SUPPORT_BUNDLE_EXPORTER_DONE";
    default:
        return "SUPPORT_BUNDLE_EXPORTER_UNKNOWN";
    }
}
