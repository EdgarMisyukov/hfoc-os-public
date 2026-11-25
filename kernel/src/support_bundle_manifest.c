#include "support_bundle_manifest.h"

#include "config_kv.h"
#include "config_schema.h"
#include "diagnostic_privacy.h"
#include "sha256.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define SUPPORT_BUNDLE_MANIFEST_SCHEMA_FIELD_SIZE 32U
#define SUPPORT_BUNDLE_MANIFEST_POLICY_FIELD_SIZE 16U
#define SUPPORT_BUNDLE_METRICS_CANONICAL_LENGTH 245U
#define SUPPORT_BUNDLE_HEALTH_CANONICAL_LENGTH 336U
#define SUPPORT_BUNDLE_AUDIT_RECORD_CANONICAL_LENGTH 52U
#define SUPPORT_BUNDLE_VERSIONS_CANONICAL_LENGTH \
    (6U * 4U + SUPPORT_BUNDLE_MANIFEST_SECRET_POLICY_ID_LENGTH)

static const unsigned char support_bundle_manifest_magic[
    SUPPORT_BUNDLE_MANIFEST_MAGIC_SIZE] = {
    'H', 'F', 'O', 'C', 'S', 'B', 'M', 'B'
};

static void support_bundle_zero_bytes(unsigned char *bytes, size_t length)
{
    size_t index;

    for (index = 0U; index < length; ++index) {
        bytes[index] = 0U;
    }
}

static void support_bundle_copy_bytes(unsigned char *destination,
                                      const unsigned char *source,
                                      size_t length)
{
    size_t index;

    for (index = 0U; index < length; ++index) {
        destination[index] = source[index];
    }
}

static void support_bundle_store_le16(unsigned char *bytes, uint16_t value)
{
    bytes[0] = (unsigned char)(value);
    bytes[1] = (unsigned char)(value >> 8U);
}

static void support_bundle_store_le32(unsigned char *bytes, uint32_t value)
{
    bytes[0] = (unsigned char)(value);
    bytes[1] = (unsigned char)(value >> 8U);
    bytes[2] = (unsigned char)(value >> 16U);
    bytes[3] = (unsigned char)(value >> 24U);
}

static void support_bundle_store_le64(unsigned char *bytes, uint64_t value)
{
    size_t index;

    for (index = 0U; index < 8U; ++index) {
        bytes[index] = (unsigned char)(value >> (8U * index));
    }
}

static void support_bundle_hash_u32(struct sha256 *context, uint32_t value)
{
    unsigned char bytes[4];

    support_bundle_store_le32(bytes, value);
    (void)sha256_update(context, bytes, sizeof(bytes));
}

static void support_bundle_hash_u64(struct sha256 *context, uint64_t value)
{
    unsigned char bytes[8];

    support_bundle_store_le64(bytes, value);
    (void)sha256_update(context, bytes, sizeof(bytes));
}

static void support_bundle_hash_byte(struct sha256 *context, uint8_t value)
{
    (void)sha256_update(context, &value, 1U);
}

static void support_bundle_hash_bytes(struct sha256 *context,
                                      const void *data,
                                      size_t length)
{
    (void)sha256_update(context, data, length);
}

static bool support_bundle_metrics_valid(
    const struct metrics_snapshot *snapshot)
{
    const struct metrics_health_values *health;
    const struct metrics_state_values *state;
    const struct metrics_service_values *service;
    const struct metrics_update_values *update;

    if (snapshot == NULL || snapshot->version != METRICS_VERSION) {
        return false;
    }
    health = &snapshot->paths.health;
    state = &snapshot->paths.state;
    service = &snapshot->paths.service;
    update = &snapshot->paths.update;
    if (health->version != METRICS_VERSION ||
        health->check_count > METRICS_HEALTH_CHECK_MAX) {
        return false;
    }
    if (health->ring_published) {
        if (health->ring_count > METRICS_EVENT_RING_CAPACITY ||
            health->ring_available > METRICS_EVENT_RING_CAPACITY ||
            health->ring_count + health->ring_available !=
                METRICS_EVENT_RING_CAPACITY ||
            health->ring_utilization_percent !=
                (health->ring_count * 100U) /
                    METRICS_EVENT_RING_CAPACITY) {
            return false;
        }
    } else if (health->ring_count != 0U ||
               health->ring_available != 0U ||
               health->ring_utilization_percent != 0U) {
        return false;
    }
    if (state->version != METRICS_VERSION ||
        state->schema_version < METRICS_STATE_SCHEMA_VERSION_MIN ||
        state->schema_version > METRICS_STATE_SCHEMA_VERSION_MAX) {
        return false;
    }
    if (service->version != METRICS_VERSION ||
        service->service_count > METRICS_SERVICE_CAPACITY ||
        service->ready_count + service->pending_count +
            service->failed_count != service->service_count ||
        service->exhausted_count > service->failed_count ||
        service->dependency_blocked_count > service->service_count) {
        return false;
    }
    if (update->version != METRICS_VERSION ||
        update->slot_count > METRICS_SLOT_CAPACITY ||
        update->known_good_slot > METRICS_SLOT_CAPACITY ||
        update->pending_slot > METRICS_SLOT_CAPACITY ||
        update->rollback_slot > METRICS_SLOT_CAPACITY) {
        return false;
    }
    return update->slot_count != 0U ||
           (update->known_good_slot == 0U &&
            update->pending_slot == 0U &&
            update->rollback_slot == 0U);
}

static bool support_bundle_status_service_state_valid(uint32_t state)
{
    return state == APPLIANCE_SERVICE_STOPPED ||
           state == APPLIANCE_SERVICE_RUNNING ||
           state == APPLIANCE_SERVICE_READY ||
           state == APPLIANCE_SERVICE_EXITED ||
           state == APPLIANCE_SERVICE_EXHAUSTED;
}

static bool support_bundle_status_service_health_valid(uint32_t health)
{
    return health == APPLIANCE_SERVICE_HEALTH_PENDING ||
           health == APPLIANCE_SERVICE_HEALTH_READY ||
           health == APPLIANCE_SERVICE_HEALTH_LIVENESS_FAILED ||
           health == APPLIANCE_SERVICE_HEALTH_READINESS_TIMEOUT;
}

static bool support_bundle_status_valid(
    const struct appliance_status_snapshot *snapshot)
{
    uint32_t index;

    if (snapshot == NULL || !snapshot->published ||
        snapshot->version != APPLIANCE_STATUS_VERSION ||
        snapshot->state.version != APPLIANCE_STATUS_VERSION ||
        snapshot->storage.version != APPLIANCE_STATUS_VERSION ||
        snapshot->health.version != APPLIANCE_STATUS_VERSION) {
        return false;
    }
    if (snapshot->state.recovery_mode >
            APPLIANCE_STATUS_RECOVERY_MODE_MAX ||
        snapshot->state.active_process_count >
            APPLIANCE_STATUS_COUNT_MAX) {
        return false;
    }
    if (snapshot->storage.state != APPLIANCE_STORAGE_OK &&
        snapshot->storage.state != APPLIANCE_STORAGE_DEGRADED &&
        snapshot->storage.state != APPLIANCE_STORAGE_FAILED) {
        return false;
    }
    if (snapshot->health.state != APPLIANCE_HEALTH_OK &&
        snapshot->health.state != APPLIANCE_HEALTH_DEGRADED &&
        snapshot->health.state != APPLIANCE_HEALTH_FAILED) {
        return false;
    }
    if (snapshot->health.ready_count >
            APPLIANCE_STATUS_COUNT_MAX ||
        snapshot->health.pending_count >
            APPLIANCE_STATUS_COUNT_MAX ||
        snapshot->health.failed_count >
            APPLIANCE_STATUS_COUNT_MAX ||
        snapshot->service_count >
            APPLIANCE_STATUS_SERVICE_MAX) {
        return false;
    }
    for (index = 0U; index < snapshot->service_count; ++index) {
        const struct appliance_status_service_entry *entry =
            &snapshot->services[index];

        if (entry->id == 0U || entry->id > 255U ||
            !support_bundle_status_service_state_valid(
                (uint32_t)entry->state) ||
            !support_bundle_status_service_health_valid(
                (uint32_t)entry->health) ||
            entry->restart_count > APPLIANCE_STATUS_COUNT_MAX) {
            return false;
        }
        if (index > 0U &&
            entry->id <= snapshot->services[index - 1U].id) {
            return false;
        }
    }
    return true;
}

static bool support_bundle_health_valid(
    const struct health_check_snapshot *snapshot)
{
    size_t index;
    size_t previous;

    if (snapshot == NULL || snapshot->version != HEALTH_CHECK_VERSION ||
        snapshot->descriptor_count == 0U ||
        snapshot->descriptor_count > HEALTH_CHECK_MAX_DESCRIPTORS) {
        return false;
    }
    for (index = 0U; index < snapshot->descriptor_count; ++index) {
        const struct health_check_entry *entry =
            &snapshot->entries[index];

        if (entry->descriptor.version != HEALTH_CHECK_VERSION ||
            entry->descriptor.check_id < HEALTH_CHECK_ID_MIN ||
            entry->descriptor.check_id > HEALTH_CHECK_ID_MAX ||
            entry->descriptor.severity < HEALTH_CHECK_SEVERITY_INFO ||
            entry->descriptor.severity > HEALTH_CHECK_SEVERITY_CRITICAL ||
            entry->descriptor.cadence_ms < HEALTH_CHECK_CADENCE_MIN_MS ||
            entry->descriptor.cadence_ms > HEALTH_CHECK_CADENCE_MAX_MS ||
            entry->result < HEALTH_CHECK_RESULT_UNKNOWN ||
            entry->result > HEALTH_CHECK_RESULT_FAIL) {
            return false;
        }
        for (previous = 0U; previous < index; ++previous) {
            if (snapshot->entries[previous].descriptor.check_id ==
                entry->descriptor.check_id) {
                return false;
            }
        }
    }
    return true;
}

static bool support_bundle_audit_record_valid(
    const struct audit_record *record)
{
    bool type_valid;

    if (record == NULL) {
        return false;
    }
    switch (record->type) {
    case AUDIT_RECORD_PROCESS:
    case AUDIT_RECORD_SYSCALL:
    case AUDIT_RECORD_CAPABILITY:
    case AUDIT_RECORD_FAULT:
    case AUDIT_RECORD_REBOOT_REASON:
    case AUDIT_RECORD_MANAGEMENT:
    case AUDIT_RECORD_NETWORK:
    case AUDIT_RECORD_TRUST:
        type_valid = true;
        break;
    default:
        type_valid = false;
        break;
    }
    if (!type_valid ||
        (record->operation & ~UINT32_C(0xFF)) != 0U ||
        (record->result & ~UINT32_C(0xFF)) != 0U ||
        (record->pid & ~UINT32_C(0xFF)) != 0U ||
        (record->domain & ~UINT32_C(0xFF)) != 0U ||
        (record->generation & ~UINT32_C(0xFFFFFF)) != 0U ||
        (record->session_id & ~UINT32_C(0xFF)) != 0U ||
        (record->principal_id & ~UINT32_C(0xFF)) != 0U ||
        (record->command_id & ~UINT32_C(0xFF)) != 0U) {
        return false;
    }
    if (record->type == AUDIT_RECORD_MANAGEMENT) {
        return record->pid == 0U && record->domain == 0U &&
               record->generation == 0U;
    }
    return record->session_id == 0U &&
           record->principal_id == 0U &&
           record->command_id == 0U;
}

static bool support_bundle_audit_valid(
    const struct audit_record *records,
    uint32_t count,
    uint64_t *first_sequence,
    uint64_t *last_sequence)
{
    uint32_t index;

    if (count > SUPPORT_BUNDLE_AUDIT_SLICE_MAX ||
        (count > 0U && records == NULL)) {
        return false;
    }
    if (first_sequence != NULL) {
        *first_sequence = 0U;
    }
    if (last_sequence != NULL) {
        *last_sequence = 0U;
    }
    for (index = 0U; index < count; ++index) {
        const struct audit_record *record = &records[index];

        if (!support_bundle_audit_record_valid(record)) {
            return false;
        }
        if (index > 0U &&
            record->sequence <= records[index - 1U].sequence) {
            return false;
        }
    }
    if (count > 0U) {
        if (first_sequence != NULL) {
            *first_sequence = records[0].sequence;
        }
        if (last_sequence != NULL) {
            *last_sequence = records[count - 1U].sequence;
        }
    }
    return true;
}

static bool support_bundle_config_entry_valid(
    const struct config_kv_entry *entry)
{
    return entry->occupied &&
           entry->kind < CONFIG_KV_KIND_COUNT &&
           entry->key_length > 0U &&
           entry->key_length <= CONFIG_KV_KEY_MAX &&
           entry->value_length <= CONFIG_KV_VALUE_MAX &&
           entry->revision != 0U;
}

static bool support_bundle_config_key_equal(
    const struct config_kv_entry *entry,
    uint8_t kind,
    const unsigned char *key,
    size_t key_length)
{
    return entry->occupied &&
           entry->kind == kind &&
           entry->key_length == key_length &&
           memcmp(entry->key, key, key_length) == 0;
}

static bool support_bundle_config_entry_secret(
    const struct config_kv_entry *entry)
{
    enum diagnostic_privacy_category category;

    return diagnostic_privacy_classify_key(
               entry->key, entry->key_length, &category) ==
               DIAGNOSTIC_PRIVACY_OK &&
           category == DIAGNOSTIC_PRIVACY_CATEGORY_CREDENTIAL;
}

static bool support_bundle_config_valid(
    const struct config_kv *kv,
    uint32_t *included_out,
    uint32_t *excluded_out,
    size_t *metadata_length_out)
{
    uint32_t included = 0U;
    uint32_t excluded = 0U;
    size_t occupied = 0U;
    size_t metadata_length = 0U;
    size_t index;
    size_t previous;

    if (kv == NULL || !kv->initialized || kv->entries == NULL ||
        kv->capacity == 0U ||
        kv->capacity > CONFIG_KV_CAPACITY_MAX ||
        kv->count > kv->capacity) {
        return false;
    }
    for (index = 0U; index < kv->capacity; ++index) {
        const struct config_kv_entry *entry = &kv->entries[index];

        if (!entry->occupied) {
            continue;
        }
        if (!support_bundle_config_entry_valid(entry)) {
            return false;
        }
        ++occupied;
        if (entry->kind != CONFIG_KV_KIND_CONFIG) {
            continue;
        }
        for (previous = 0U; previous < index; ++previous) {
            if (support_bundle_config_key_equal(
                    &kv->entries[previous], entry->kind,
                    entry->key,
                    entry->key_length)) {
                return false;
            }
        }
        if (support_bundle_config_entry_secret(entry)) {
            ++excluded;
        } else {
            metadata_length += 12U + entry->key_length +
                               entry->value_length;
            ++included;
        }
    }
    if (occupied != kv->count) {
        return false;
    }
    if (included_out != NULL) {
        *included_out = included;
    }
    if (excluded_out != NULL) {
        *excluded_out = excluded;
    }
    if (metadata_length_out != NULL) {
        *metadata_length_out = metadata_length;
    }
    return true;
}

static void support_bundle_metrics_digest(
    const struct metrics_snapshot *snapshot,
    unsigned char digest[SHA256_DIGEST_SIZE])
{
    const struct metrics_health_values *health = &snapshot->paths.health;
    const struct metrics_state_values *state = &snapshot->paths.state;
    const struct metrics_service_values *service =
        &snapshot->paths.service;
    const struct metrics_update_values *update =
        &snapshot->paths.update;
    struct sha256 context;
    unsigned char result[SHA256_DIGEST_SIZE];

    (void)sha256_init(&context);
    support_bundle_hash_u32(&context, snapshot->version);
    support_bundle_hash_u64(&context, snapshot->generation);
    support_bundle_hash_u32(&context, health->version);
    support_bundle_hash_u32(&context, health->check_count);
    support_bundle_hash_u64(&context, health->pass_count);
    support_bundle_hash_u64(&context, health->warn_count);
    support_bundle_hash_u64(&context, health->fail_count);
    support_bundle_hash_u64(&context, health->unknown_count);
    support_bundle_hash_byte(&context, health->ring_published ? 1U : 0U);
    support_bundle_hash_u32(&context, health->ring_count);
    support_bundle_hash_u32(&context, health->ring_available);
    support_bundle_hash_u32(&context, health->ring_utilization_percent);
    support_bundle_hash_u32(&context, state->version);
    support_bundle_hash_u64(&context, state->generation);
    support_bundle_hash_u64(&context, state->journal_generation);
    support_bundle_hash_u64(&context, state->checkpoint_sequence);
    support_bundle_hash_u32(&context, state->schema_version);
    support_bundle_hash_u32(&context, state->active_generation);
    support_bundle_hash_u64(&context, state->migrations);
    support_bundle_hash_u64(&context, state->compactions);
    support_bundle_hash_u64(&context, state->restores);
    support_bundle_hash_u64(&context, state->backup_exports);
    support_bundle_hash_u32(&context, service->version);
    support_bundle_hash_u64(&context, service->generation);
    support_bundle_hash_u32(&context, service->service_count);
    support_bundle_hash_u32(&context, service->ready_count);
    support_bundle_hash_u32(&context, service->pending_count);
    support_bundle_hash_u32(&context, service->failed_count);
    support_bundle_hash_u32(&context, service->exhausted_count);
    support_bundle_hash_u32(&context, service->dependency_blocked_count);
    support_bundle_hash_u64(&context, service->restarts);
    support_bundle_hash_u64(&context, service->exits);
    support_bundle_hash_u32(&context, update->version);
    support_bundle_hash_u64(&context, update->generation);
    support_bundle_hash_u32(&context, update->inventory_generation);
    support_bundle_hash_u32(&context, update->slot_count);
    support_bundle_hash_u32(&context, update->known_good_slot);
    support_bundle_hash_u32(&context, update->pending_slot);
    support_bundle_hash_u32(&context, update->rollback_slot);
    support_bundle_hash_byte(&context,
                             update->candidate_pending ? 1U : 0U);
    support_bundle_hash_byte(&context,
                             update->candidate_active ? 1U : 0U);
    support_bundle_hash_byte(&context, update->accepted ? 1U : 0U);
    support_bundle_hash_byte(&context,
                             update->rollback_published ? 1U : 0U);
    support_bundle_hash_u64(&context, update->acceptances);
    support_bundle_hash_u64(&context, update->rollbacks);
    support_bundle_hash_u64(&context, update->rejections);
    (void)sha256_final(&context, result);
    support_bundle_copy_bytes(digest, result, sizeof(result));
}

static size_t support_bundle_status_length(
    const struct appliance_status_snapshot *snapshot)
{
    return 4U + 1U + 44U + 28U + 20U + 4U +
           (size_t)snapshot->service_count * 16U;
}

static void support_bundle_status_digest(
    const struct appliance_status_snapshot *snapshot,
    unsigned char digest[SHA256_DIGEST_SIZE])
{
    const struct appliance_status_state_snapshot *state =
        &snapshot->state;
    const struct appliance_status_storage_snapshot *storage =
        &snapshot->storage;
    const struct appliance_status_health_snapshot *health =
        &snapshot->health;
    struct sha256 context;
    unsigned char result[SHA256_DIGEST_SIZE];
    uint32_t index;

    (void)sha256_init(&context);
    support_bundle_hash_u32(&context, snapshot->version);
    support_bundle_hash_byte(&context, snapshot->published ? 1U : 0U);
    support_bundle_hash_u32(&context, state->version);
    support_bundle_hash_u64(&context, state->generation);
    support_bundle_hash_u64(&context, state->uptime_ticks);
    support_bundle_hash_u32(&context, state->recovery_mode);
    support_bundle_hash_u64(&context, state->config_revision);
    support_bundle_hash_u64(&context, state->audit_sequence);
    support_bundle_hash_u32(&context, state->active_process_count);
    support_bundle_hash_u32(&context, storage->version);
    support_bundle_hash_u64(&context, storage->journal_generation);
    support_bundle_hash_u64(&context, storage->backup_generation);
    support_bundle_hash_u32(&context, storage->schema_version);
    support_bundle_hash_u32(&context, (uint32_t)storage->state);
    support_bundle_hash_u32(&context, health->version);
    support_bundle_hash_u32(&context, (uint32_t)health->state);
    support_bundle_hash_u32(&context, health->ready_count);
    support_bundle_hash_u32(&context, health->pending_count);
    support_bundle_hash_u32(&context, health->failed_count);
    support_bundle_hash_u32(&context, snapshot->service_count);
    for (index = 0U; index < snapshot->service_count; ++index) {
        const struct appliance_status_service_entry *entry =
            &snapshot->services[index];

        support_bundle_hash_u32(&context, entry->id);
        support_bundle_hash_u32(&context, (uint32_t)entry->state);
        support_bundle_hash_u32(&context, (uint32_t)entry->health);
        support_bundle_hash_u32(&context, entry->restart_count);
    }
    (void)sha256_final(&context, result);
    support_bundle_copy_bytes(digest, result, sizeof(result));
}

static void support_bundle_health_digest(
    const struct health_check_snapshot *snapshot,
    unsigned char digest[SHA256_DIGEST_SIZE])
{
    struct sha256 context;
    unsigned char result[SHA256_DIGEST_SIZE];
    size_t index;

    (void)sha256_init(&context);
    support_bundle_hash_u32(&context, snapshot->version);
    support_bundle_hash_u32(&context, snapshot->generation);
    support_bundle_hash_u64(&context,
                            (uint64_t)snapshot->descriptor_count);
    for (index = 0U; index < HEALTH_CHECK_MAX_DESCRIPTORS; ++index) {
        const struct health_check_entry *entry =
            &snapshot->entries[index];

        support_bundle_hash_u32(&context, entry->descriptor.version);
        support_bundle_hash_u32(&context,
                                entry->descriptor.check_id);
        support_bundle_hash_u32(
            &context, (uint32_t)entry->descriptor.severity);
        support_bundle_hash_u32(&context,
                                entry->descriptor.cadence_ms);
        support_bundle_hash_u32(&context, (uint32_t)entry->result);
    }
    (void)sha256_final(&context, result);
    support_bundle_copy_bytes(digest, result, sizeof(result));
}

static void support_bundle_audit_digest(
    const struct audit_record *records,
    uint32_t count,
    unsigned char digest[SHA256_DIGEST_SIZE])
{
    struct sha256 context;
    unsigned char result[SHA256_DIGEST_SIZE];
    uint32_t index;

    (void)sha256_init(&context);
    for (index = 0U; index < count; ++index) {
        const struct audit_record *record = &records[index];

        support_bundle_hash_u64(&context, record->sequence);
        support_bundle_hash_u32(&context, record->type);
        support_bundle_hash_u32(&context, record->operation);
        support_bundle_hash_u32(&context, record->result);
        support_bundle_hash_u32(&context, record->pid);
        support_bundle_hash_u32(&context, record->domain);
        support_bundle_hash_u32(&context, record->generation);
        support_bundle_hash_u32(&context, record->session_id);
        support_bundle_hash_u32(&context, record->principal_id);
        support_bundle_hash_u32(&context, record->command_id);
        support_bundle_hash_u64(&context, record->detail);
    }
    (void)sha256_final(&context, result);
    support_bundle_copy_bytes(digest, result, sizeof(result));
}

static bool support_bundle_config_entry_less(
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

static void support_bundle_config_sort(
    const struct config_kv_entry **refs,
    size_t count)
{
    size_t index;

    for (index = 1U; index < count; ++index) {
        const struct config_kv_entry *entry = refs[index];
        size_t position = index;

        while (position > 0U &&
               support_bundle_config_entry_less(
                   entry, refs[position - 1U])) {
            refs[position] = refs[position - 1U];
            --position;
        }
        refs[position] = entry;
    }
}

static void support_bundle_config_digest(
    const struct config_kv *kv,
    unsigned char digest[SHA256_DIGEST_SIZE])
{
    const struct config_kv_entry *refs[CONFIG_KV_CAPACITY_MAX];
    unsigned char frame_header[12];
    unsigned char result[SHA256_DIGEST_SIZE];
    struct sha256 context;
    size_t count = 0U;
    size_t index;

    for (index = 0U; index < kv->capacity; ++index) {
        const struct config_kv_entry *entry = &kv->entries[index];

        if (entry->occupied &&
            entry->kind == CONFIG_KV_KIND_CONFIG &&
            !support_bundle_config_entry_secret(entry)) {
            refs[count] = entry;
            ++count;
        }
    }
    support_bundle_config_sort(refs, count);

    (void)sha256_init(&context);
    for (index = 0U; index < count; ++index) {
        const struct config_kv_entry *entry = refs[index];

        support_bundle_zero_bytes(frame_header, sizeof(frame_header));
        frame_header[0] = entry->kind;
        frame_header[1] = entry->key_length;
        support_bundle_store_le16(frame_header + 2U,
                                  (uint16_t)entry->value_length);
        support_bundle_store_le64(frame_header + 4U, entry->revision);
        (void)sha256_update(&context, frame_header,
                            sizeof(frame_header));
        (void)sha256_update(&context, entry->key, entry->key_length);
        (void)sha256_update(&context, entry->value,
                            entry->value_length);
    }
    (void)sha256_final(&context, result);
    support_bundle_copy_bytes(digest, result, sizeof(result));
}

static void support_bundle_versions_digest(
    unsigned char digest[SHA256_DIGEST_SIZE])
{
    struct sha256 context;
    unsigned char result[SHA256_DIGEST_SIZE];

    (void)sha256_init(&context);
    support_bundle_hash_u32(&context, SUPPORT_BUNDLE_MANIFEST_VERSION);
    support_bundle_hash_u32(&context, METRICS_VERSION);
    support_bundle_hash_u32(&context, APPLIANCE_STATUS_VERSION);
    support_bundle_hash_u32(&context, HEALTH_CHECK_VERSION);
    support_bundle_hash_u32(&context, CONFIG_SCHEMA_VERSION);
    support_bundle_hash_u32(&context, KERNEL_EVENT_RING_CAPACITY);
    support_bundle_hash_bytes(
        &context, SUPPORT_BUNDLE_MANIFEST_SECRET_POLICY_ID,
        SUPPORT_BUNDLE_MANIFEST_SECRET_POLICY_ID_LENGTH);
    (void)sha256_final(&context, result);
    support_bundle_copy_bytes(digest, result, sizeof(result));
}

static void support_bundle_write_entry(
    unsigned char *buffer,
    uint32_t type,
    uint32_t length,
    const unsigned char digest[SHA256_DIGEST_SIZE])
{
    support_bundle_store_le32(buffer, type);
    support_bundle_store_le32(buffer + 4U, length);
    support_bundle_copy_bytes(buffer + 8U, digest,
                              SHA256_DIGEST_SIZE);
    support_bundle_zero_bytes(buffer + 40U, 24U);
}

enum support_bundle_manifest_status support_bundle_manifest_export(
    const struct support_bundle_manifest_source *source,
    void *buffer,
    size_t capacity,
    size_t *length_out)
{
    unsigned char *output = buffer;
    unsigned char metrics_digest[SHA256_DIGEST_SIZE];
    unsigned char status_digest[SHA256_DIGEST_SIZE];
    unsigned char health_digest[SHA256_DIGEST_SIZE];
    unsigned char audit_digest[SHA256_DIGEST_SIZE];
    unsigned char config_digest[SHA256_DIGEST_SIZE];
    unsigned char versions_digest[SHA256_DIGEST_SIZE];
    uint64_t audit_first = 0U;
    uint64_t audit_last = 0U;
    uint32_t config_included = 0U;
    uint32_t config_excluded = 0U;
    size_t config_length = 0U;
    size_t offset;

    if (source == NULL || buffer == NULL || length_out == NULL) {
        return SUPPORT_BUNDLE_MANIFEST_NULL;
    }
    if (source->version != SUPPORT_BUNDLE_MANIFEST_VERSION) {
        return SUPPORT_BUNDLE_MANIFEST_BAD_VERSION;
    }
    if (!support_bundle_metrics_valid(source->metrics) ||
        !support_bundle_status_valid(source->status) ||
        !support_bundle_health_valid(source->health)) {
        return SUPPORT_BUNDLE_MANIFEST_BAD_SNAPSHOT;
    }
    if (!support_bundle_audit_valid(source->audit_records,
                                    source->audit_count,
                                    &audit_first, &audit_last)) {
        return SUPPORT_BUNDLE_MANIFEST_BAD_AUDIT;
    }
    if (!support_bundle_config_valid(source->config,
                                     &config_included,
                                     &config_excluded,
                                     &config_length)) {
        return SUPPORT_BUNDLE_MANIFEST_BAD_CONFIG;
    }
    if (capacity < SUPPORT_BUNDLE_MANIFEST_SIZE) {
        return SUPPORT_BUNDLE_MANIFEST_TOO_SMALL;
    }

    support_bundle_metrics_digest(source->metrics, metrics_digest);
    support_bundle_status_digest(source->status, status_digest);
    support_bundle_health_digest(source->health, health_digest);
    support_bundle_audit_digest(source->audit_records,
                                source->audit_count, audit_digest);
    support_bundle_config_digest(source->config, config_digest);
    support_bundle_versions_digest(versions_digest);

    support_bundle_zero_bytes(output, SUPPORT_BUNDLE_MANIFEST_SIZE);
    support_bundle_copy_bytes(output, support_bundle_manifest_magic,
                              SUPPORT_BUNDLE_MANIFEST_MAGIC_SIZE);
    output[SUPPORT_BUNDLE_MANIFEST_VERSION_OFFSET] =
        SUPPORT_BUNDLE_MANIFEST_VERSION;
    support_bundle_copy_bytes(
        output + SUPPORT_BUNDLE_MANIFEST_SCHEMA_ID_OFFSET,
        (const unsigned char *)
            SUPPORT_BUNDLE_MANIFEST_SCHEMA_ID,
        SUPPORT_BUNDLE_MANIFEST_SCHEMA_ID_LENGTH);
    support_bundle_store_le32(output + 48U,
                              SUPPORT_BUNDLE_MANIFEST_VERSION);
    support_bundle_store_le32(output + 52U, METRICS_VERSION);
    support_bundle_store_le32(output + 56U, APPLIANCE_STATUS_VERSION);
    support_bundle_store_le32(output + 60U, HEALTH_CHECK_VERSION);
    support_bundle_store_le32(output + 64U, CONFIG_SCHEMA_VERSION);
    support_bundle_store_le32(output + 68U, KERNEL_EVENT_RING_CAPACITY);
    support_bundle_store_le32(output + 72U,
                              SUPPORT_BUNDLE_MANIFEST_ENTRY_MAX);
    support_bundle_store_le32(
        output + SUPPORT_BUNDLE_MANIFEST_AUDIT_COUNT_OFFSET,
        source->audit_count);
    support_bundle_store_le32(
        output + SUPPORT_BUNDLE_MANIFEST_CONFIG_INCLUDED_OFFSET,
        config_included);
    support_bundle_store_le32(
        output + SUPPORT_BUNDLE_MANIFEST_CONFIG_EXCLUDED_OFFSET,
        config_excluded);
    support_bundle_store_le64(output + 88U, audit_first);
    support_bundle_store_le64(output + 96U, audit_last);
    support_bundle_store_le64(output + 104U,
                              (uint64_t)config_length);
    support_bundle_copy_bytes(
        output + SUPPORT_BUNDLE_MANIFEST_POLICY_ID_OFFSET,
        (const unsigned char *)
            SUPPORT_BUNDLE_MANIFEST_SECRET_POLICY_ID,
        SUPPORT_BUNDLE_MANIFEST_SECRET_POLICY_ID_LENGTH);

    offset = SUPPORT_BUNDLE_MANIFEST_HEADER_SIZE;
    support_bundle_write_entry(
        output + offset, SUPPORT_BUNDLE_ENTRY_METRICS,
        SUPPORT_BUNDLE_METRICS_CANONICAL_LENGTH, metrics_digest);
    offset += SUPPORT_BUNDLE_MANIFEST_ENTRY_RECORD_SIZE;
    support_bundle_write_entry(
        output + offset, SUPPORT_BUNDLE_ENTRY_APPLIANCE_STATUS,
        (uint32_t)support_bundle_status_length(source->status),
        status_digest);
    offset += SUPPORT_BUNDLE_MANIFEST_ENTRY_RECORD_SIZE;
    support_bundle_write_entry(
        output + offset, SUPPORT_BUNDLE_ENTRY_HEALTH,
        SUPPORT_BUNDLE_HEALTH_CANONICAL_LENGTH, health_digest);
    offset += SUPPORT_BUNDLE_MANIFEST_ENTRY_RECORD_SIZE;
    support_bundle_write_entry(
        output + offset, SUPPORT_BUNDLE_ENTRY_AUDIT_SLICE,
        source->audit_count *
            SUPPORT_BUNDLE_AUDIT_RECORD_CANONICAL_LENGTH,
        audit_digest);
    offset += SUPPORT_BUNDLE_MANIFEST_ENTRY_RECORD_SIZE;
    support_bundle_write_entry(
        output + offset, SUPPORT_BUNDLE_ENTRY_CONFIG_METADATA,
        (uint32_t)config_length, config_digest);
    offset += SUPPORT_BUNDLE_MANIFEST_ENTRY_RECORD_SIZE;
    support_bundle_write_entry(
        output + offset, SUPPORT_BUNDLE_ENTRY_VERSIONS,
        SUPPORT_BUNDLE_VERSIONS_CANONICAL_LENGTH, versions_digest);

    *length_out = SUPPORT_BUNDLE_MANIFEST_SIZE;
    return SUPPORT_BUNDLE_MANIFEST_OK;
}

const char *support_bundle_manifest_status_string(
    enum support_bundle_manifest_status status)
{
    switch (status) {
    case SUPPORT_BUNDLE_MANIFEST_OK:
        return "SUPPORT_BUNDLE_MANIFEST_OK";
    case SUPPORT_BUNDLE_MANIFEST_NULL:
        return "SUPPORT_BUNDLE_MANIFEST_NULL";
    case SUPPORT_BUNDLE_MANIFEST_BAD_VERSION:
        return "SUPPORT_BUNDLE_MANIFEST_BAD_VERSION";
    case SUPPORT_BUNDLE_MANIFEST_BAD_STATE:
        return "SUPPORT_BUNDLE_MANIFEST_BAD_STATE";
    case SUPPORT_BUNDLE_MANIFEST_TOO_SMALL:
        return "SUPPORT_BUNDLE_MANIFEST_TOO_SMALL";
    case SUPPORT_BUNDLE_MANIFEST_BAD_SNAPSHOT:
        return "SUPPORT_BUNDLE_MANIFEST_BAD_SNAPSHOT";
    case SUPPORT_BUNDLE_MANIFEST_BAD_AUDIT:
        return "SUPPORT_BUNDLE_MANIFEST_BAD_AUDIT";
    case SUPPORT_BUNDLE_MANIFEST_BAD_CONFIG:
        return "SUPPORT_BUNDLE_MANIFEST_BAD_CONFIG";
    default:
        return NULL;
    }
}

const char *support_bundle_manifest_entry_type_string(
    enum support_bundle_entry_type type)
{
    switch (type) {
    case SUPPORT_BUNDLE_ENTRY_NONE:
        return "SUPPORT_BUNDLE_ENTRY_NONE";
    case SUPPORT_BUNDLE_ENTRY_METRICS:
        return "SUPPORT_BUNDLE_ENTRY_METRICS";
    case SUPPORT_BUNDLE_ENTRY_APPLIANCE_STATUS:
        return "SUPPORT_BUNDLE_ENTRY_APPLIANCE_STATUS";
    case SUPPORT_BUNDLE_ENTRY_HEALTH:
        return "SUPPORT_BUNDLE_ENTRY_HEALTH";
    case SUPPORT_BUNDLE_ENTRY_AUDIT_SLICE:
        return "SUPPORT_BUNDLE_ENTRY_AUDIT_SLICE";
    case SUPPORT_BUNDLE_ENTRY_CONFIG_METADATA:
        return "SUPPORT_BUNDLE_ENTRY_CONFIG_METADATA";
    case SUPPORT_BUNDLE_ENTRY_VERSIONS:
        return "SUPPORT_BUNDLE_ENTRY_VERSIONS";
    default:
        return NULL;
    }
}

const char *support_bundle_manifest_schema_id(void)
{
    return SUPPORT_BUNDLE_MANIFEST_SCHEMA_ID;
}
