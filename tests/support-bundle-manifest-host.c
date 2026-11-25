#include "support_bundle_manifest.h"

#include "config_kv.h"
#include "config_schema.h"
#include "sha256.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifdef HFOC_SUPPORT_BUNDLE_MANIFEST_HOST_TEST

#define TEST_CONFIG_CAPACITY 6U
#define TEST_AUDIT_COUNT 2U
#define TEST_METRICS_CANONICAL_LENGTH 245U
#define TEST_HEALTH_CANONICAL_LENGTH 336U
#define TEST_AUDIT_RECORD_CANONICAL_LENGTH 52U
#define TEST_VERSIONS_CANONICAL_LENGTH \
    (6U * 4U + SUPPORT_BUNDLE_MANIFEST_SECRET_POLICY_ID_LENGTH)
#define TEST_HEADER_SIZE 128U
#define TEST_ENTRY_RECORD_SIZE 64U

static int failures;
static unsigned int alloc_attempts;
static unsigned int free_attempts;

static const unsigned char manifest_magic[
    SUPPORT_BUNDLE_MANIFEST_MAGIC_SIZE] = {
    'H', 'F', 'O', 'C', 'S', 'B', 'M', 'B'
};

static const char *secret_keys[SUPPORT_BUNDLE_MANIFEST_SECRET_KEY_COUNT] = {
    "credential",
    "secret",
    "password",
    "token",
    "private_key",
    "capability",
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

static void store_le16(unsigned char *bytes, uint16_t value)
{
    bytes[0] = (unsigned char)(value);
    bytes[1] = (unsigned char)(value >> 8U);
}

static void store_le32(unsigned char *bytes, uint32_t value)
{
    bytes[0] = (unsigned char)(value);
    bytes[1] = (unsigned char)(value >> 8U);
    bytes[2] = (unsigned char)(value >> 16U);
    bytes[3] = (unsigned char)(value >> 24U);
}

static void store_le64(unsigned char *bytes, uint64_t value)
{
    size_t index;

    for (index = 0U; index < 8U; ++index) {
        bytes[index] = (unsigned char)(value >> (8U * index));
    }
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

static void hash_u32(struct sha256 *context, uint32_t value)
{
    unsigned char bytes[4];

    store_le32(bytes, value);
    (void)sha256_update(context, bytes, sizeof(bytes));
}

static void hash_u64(struct sha256 *context, uint64_t value)
{
    unsigned char bytes[8];

    store_le64(bytes, value);
    (void)sha256_update(context, bytes, sizeof(bytes));
}

static void hash_byte(struct sha256 *context, uint8_t value)
{
    (void)sha256_update(context, &value, 1U);
}

static void hash_bytes(struct sha256 *context,
                       const void *data,
                       size_t length)
{
    (void)sha256_update(context, data, length);
}

static void hash_metrics(const struct metrics_snapshot *snapshot,
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
    hash_u32(&context, snapshot->version);
    hash_u64(&context, snapshot->generation);
    hash_u32(&context, health->version);
    hash_u32(&context, health->check_count);
    hash_u64(&context, health->pass_count);
    hash_u64(&context, health->warn_count);
    hash_u64(&context, health->fail_count);
    hash_u64(&context, health->unknown_count);
    hash_byte(&context, health->ring_published ? 1U : 0U);
    hash_u32(&context, health->ring_count);
    hash_u32(&context, health->ring_available);
    hash_u32(&context, health->ring_utilization_percent);
    hash_u32(&context, state->version);
    hash_u64(&context, state->generation);
    hash_u64(&context, state->journal_generation);
    hash_u64(&context, state->checkpoint_sequence);
    hash_u32(&context, state->schema_version);
    hash_u32(&context, state->active_generation);
    hash_u64(&context, state->migrations);
    hash_u64(&context, state->compactions);
    hash_u64(&context, state->restores);
    hash_u64(&context, state->backup_exports);
    hash_u32(&context, service->version);
    hash_u64(&context, service->generation);
    hash_u32(&context, service->service_count);
    hash_u32(&context, service->ready_count);
    hash_u32(&context, service->pending_count);
    hash_u32(&context, service->failed_count);
    hash_u32(&context, service->exhausted_count);
    hash_u32(&context, service->dependency_blocked_count);
    hash_u64(&context, service->restarts);
    hash_u64(&context, service->exits);
    hash_u32(&context, update->version);
    hash_u64(&context, update->generation);
    hash_u32(&context, update->inventory_generation);
    hash_u32(&context, update->slot_count);
    hash_u32(&context, update->known_good_slot);
    hash_u32(&context, update->pending_slot);
    hash_u32(&context, update->rollback_slot);
    hash_byte(&context, update->candidate_pending ? 1U : 0U);
    hash_byte(&context, update->candidate_active ? 1U : 0U);
    hash_byte(&context, update->accepted ? 1U : 0U);
    hash_byte(&context, update->rollback_published ? 1U : 0U);
    hash_u64(&context, update->acceptances);
    hash_u64(&context, update->rollbacks);
    hash_u64(&context, update->rejections);
    (void)sha256_final(&context, result);
    memcpy(digest, result, SHA256_DIGEST_SIZE);
}

static size_t status_canonical_length(
    const struct appliance_status_snapshot *snapshot)
{
    return 4U + 1U + 44U + 28U + 20U + 4U +
           (size_t)snapshot->service_count * 16U;
}

static void hash_status(const struct appliance_status_snapshot *snapshot,
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
    hash_u32(&context, snapshot->version);
    hash_byte(&context, snapshot->published ? 1U : 0U);
    hash_u32(&context, state->version);
    hash_u64(&context, state->generation);
    hash_u64(&context, state->uptime_ticks);
    hash_u32(&context, state->recovery_mode);
    hash_u64(&context, state->config_revision);
    hash_u64(&context, state->audit_sequence);
    hash_u32(&context, state->active_process_count);
    hash_u32(&context, storage->version);
    hash_u64(&context, storage->journal_generation);
    hash_u64(&context, storage->backup_generation);
    hash_u32(&context, storage->schema_version);
    hash_u32(&context, (uint32_t)storage->state);
    hash_u32(&context, health->version);
    hash_u32(&context, (uint32_t)health->state);
    hash_u32(&context, health->ready_count);
    hash_u32(&context, health->pending_count);
    hash_u32(&context, health->failed_count);
    hash_u32(&context, snapshot->service_count);
    for (index = 0U; index < snapshot->service_count; ++index) {
        const struct appliance_status_service_entry *entry =
            &snapshot->services[index];

        hash_u32(&context, entry->id);
        hash_u32(&context, (uint32_t)entry->state);
        hash_u32(&context, (uint32_t)entry->health);
        hash_u32(&context, entry->restart_count);
    }
    (void)sha256_final(&context, result);
    memcpy(digest, result, SHA256_DIGEST_SIZE);
}

static void hash_health(const struct health_check_snapshot *snapshot,
                        unsigned char digest[SHA256_DIGEST_SIZE])
{
    struct sha256 context;
    unsigned char result[SHA256_DIGEST_SIZE];
    size_t index;

    (void)sha256_init(&context);
    hash_u32(&context, snapshot->version);
    hash_u32(&context, snapshot->generation);
    hash_u64(&context, (uint64_t)snapshot->descriptor_count);
    for (index = 0U; index < HEALTH_CHECK_MAX_DESCRIPTORS; ++index) {
        const struct health_check_entry *entry = &snapshot->entries[index];

        hash_u32(&context, entry->descriptor.version);
        hash_u32(&context, entry->descriptor.check_id);
        hash_u32(&context, (uint32_t)entry->descriptor.severity);
        hash_u32(&context, entry->descriptor.cadence_ms);
        hash_u32(&context, (uint32_t)entry->result);
    }
    (void)sha256_final(&context, result);
    memcpy(digest, result, SHA256_DIGEST_SIZE);
}

static void hash_audit(const struct audit_record *records,
                       uint32_t count,
                       unsigned char digest[SHA256_DIGEST_SIZE])
{
    struct sha256 context;
    unsigned char result[SHA256_DIGEST_SIZE];
    uint32_t index;

    (void)sha256_init(&context);
    for (index = 0U; index < count; ++index) {
        const struct audit_record *record = &records[index];

        hash_u64(&context, record->sequence);
        hash_u32(&context, record->type);
        hash_u32(&context, record->operation);
        hash_u32(&context, record->result);
        hash_u32(&context, record->pid);
        hash_u32(&context, record->domain);
        hash_u32(&context, record->generation);
        hash_u32(&context, record->session_id);
        hash_u32(&context, record->principal_id);
        hash_u32(&context, record->command_id);
        hash_u64(&context, record->detail);
    }
    (void)sha256_final(&context, result);
    memcpy(digest, result, SHA256_DIGEST_SIZE);
}

static bool entry_is_secret(const struct config_kv_entry *entry)
{
    size_t key_index;

    for (key_index = 0U;
         key_index < SUPPORT_BUNDLE_MANIFEST_SECRET_KEY_COUNT;
         ++key_index) {
        const size_t secret_length = strlen(secret_keys[key_index]);

        if (entry->key_length == secret_length &&
            memcmp(entry->key, secret_keys[key_index],
                   secret_length) == 0) {
            return true;
        }
    }
    return false;
}

static bool config_entry_less(const struct config_kv_entry *left,
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

static void sort_config_refs(const struct config_kv_entry **refs,
                             size_t count)
{
    size_t index;

    for (index = 1U; index < count; ++index) {
        const struct config_kv_entry *entry = refs[index];
        size_t position = index;

        while (position > 0U &&
               config_entry_less(entry, refs[position - 1U])) {
            refs[position] = refs[position - 1U];
            --position;
        }
        refs[position] = entry;
    }
}

static void hash_config(const struct config_kv *kv,
                        unsigned char digest[SHA256_DIGEST_SIZE],
                        uint32_t *included_out,
                        uint32_t *excluded_out,
                        size_t *length_out)
{
    const struct config_kv_entry *refs[CONFIG_KV_CAPACITY_MAX];
    unsigned char frame_header[12];
    unsigned char result[SHA256_DIGEST_SIZE];
    struct sha256 context;
    uint32_t included = 0U;
    uint32_t excluded = 0U;
    size_t length = 0U;
    size_t count = 0U;
    size_t index;

    for (index = 0U; index < kv->capacity; ++index) {
        const struct config_kv_entry *entry = &kv->entries[index];

        if (entry->occupied && entry->kind == CONFIG_KV_KIND_CONFIG) {
            if (entry_is_secret(entry)) {
                ++excluded;
            } else {
                refs[count] = entry;
                ++count;
            }
        }
    }
    sort_config_refs(refs, count);

    (void)sha256_init(&context);
    for (index = 0U; index < count; ++index) {
        const struct config_kv_entry *entry = refs[index];

        memset(frame_header, 0, sizeof(frame_header));
        frame_header[0] = entry->kind;
        frame_header[1] = entry->key_length;
        store_le16(frame_header + 2U, (uint16_t)entry->value_length);
        store_le64(frame_header + 4U, entry->revision);
        (void)sha256_update(&context, frame_header,
                            sizeof(frame_header));
        (void)sha256_update(&context, entry->key, entry->key_length);
        (void)sha256_update(&context, entry->value,
                            entry->value_length);
        length += 12U + entry->key_length + entry->value_length;
        ++included;
    }
    (void)sha256_final(&context, result);

    memcpy(digest, result, SHA256_DIGEST_SIZE);
    if (included_out != NULL) {
        *included_out = included;
    }
    if (excluded_out != NULL) {
        *excluded_out = excluded;
    }
    if (length_out != NULL) {
        *length_out = length;
    }
}

static void hash_versions(unsigned char digest[SHA256_DIGEST_SIZE])
{
    struct sha256 context;
    unsigned char result[SHA256_DIGEST_SIZE];

    (void)sha256_init(&context);
    hash_u32(&context, SUPPORT_BUNDLE_MANIFEST_VERSION);
    hash_u32(&context, METRICS_VERSION);
    hash_u32(&context, APPLIANCE_STATUS_VERSION);
    hash_u32(&context, HEALTH_CHECK_VERSION);
    hash_u32(&context, CONFIG_SCHEMA_VERSION);
    hash_u32(&context, KERNEL_EVENT_RING_CAPACITY);
    hash_bytes(&context, SUPPORT_BUNDLE_MANIFEST_SECRET_POLICY_ID,
               SUPPORT_BUNDLE_MANIFEST_SECRET_POLICY_ID_LENGTH);
    (void)sha256_final(&context, result);
    memcpy(digest, result, SHA256_DIGEST_SIZE);
}

static size_t config_metadata_length(const struct config_kv *kv);

static size_t entry_offset(uint32_t entry_index)
{
    return TEST_HEADER_SIZE + (size_t)entry_index *
           TEST_ENTRY_RECORD_SIZE;
}

static bool expected_digest_for_type(
    uint32_t type,
    const struct support_bundle_manifest_source *source,
    const struct config_kv *config,
    unsigned char digest[SHA256_DIGEST_SIZE],
    uint32_t *length_out)
{
    switch (type) {
    case SUPPORT_BUNDLE_ENTRY_METRICS:
        hash_metrics(source->metrics, digest);
        *length_out = TEST_METRICS_CANONICAL_LENGTH;
        return true;
    case SUPPORT_BUNDLE_ENTRY_APPLIANCE_STATUS:
        hash_status(source->status, digest);
        *length_out = (uint32_t)status_canonical_length(source->status);
        return true;
    case SUPPORT_BUNDLE_ENTRY_HEALTH:
        hash_health(source->health, digest);
        *length_out = TEST_HEALTH_CANONICAL_LENGTH;
        return true;
    case SUPPORT_BUNDLE_ENTRY_AUDIT_SLICE:
        hash_audit(source->audit_records, source->audit_count, digest);
        *length_out = source->audit_count *
                      TEST_AUDIT_RECORD_CANONICAL_LENGTH;
        return true;
    case SUPPORT_BUNDLE_ENTRY_CONFIG_METADATA:
        hash_config(config, digest, NULL, NULL, NULL);
        *length_out = (uint32_t)config_metadata_length(config);
        return true;
    case SUPPORT_BUNDLE_ENTRY_VERSIONS:
        hash_versions(digest);
        *length_out = TEST_VERSIONS_CANONICAL_LENGTH;
        return true;
    default:
        return false;
    }
}

static size_t config_metadata_length(const struct config_kv *kv)
{
    unsigned char digest[SHA256_DIGEST_SIZE];
    size_t length = 0U;

    hash_config(kv, digest, NULL, NULL, &length);
    return length;
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
          "manifest fixture initializes config checkpoint");
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
              "manifest fixture populates forward config checkpoint");
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
              "manifest fixture populates reverse config checkpoint");
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

static void poison_output(unsigned char *buffer,
                          size_t length,
                          size_t *length_out)
{
    size_t index;

    for (index = 0U; index < length; ++index) {
        buffer[index] = 0xA5U;
    }
    *length_out = 0xDEADBEEFU;
}

static bool output_unchanged(const unsigned char *buffer,
                             size_t length,
                             size_t length_out)
{
    size_t index;

    if (length_out != 0xDEADBEEFU) {
        return false;
    }
    for (index = 0U; index < length; ++index) {
        if (buffer[index] != 0xA5U) {
            return false;
        }
    }
    return true;
}

static bool manifest_matches_fixture(
    const unsigned char *bytes,
    size_t length,
    const struct support_bundle_manifest_source *source,
    const struct config_kv *config)
{
    unsigned char config_digest[SHA256_DIGEST_SIZE];
    uint64_t expected_audit_first = 0U;
    uint64_t expected_audit_last = 0U;
    uint32_t expected_included = 0U;
    uint32_t expected_excluded = 0U;
    size_t expected_config_length = 0U;
    uint32_t entry_index;

    if (length != SUPPORT_BUNDLE_MANIFEST_SIZE ||
        memcmp(bytes, manifest_magic,
               SUPPORT_BUNDLE_MANIFEST_MAGIC_SIZE) != 0 ||
        bytes[8U] != SUPPORT_BUNDLE_MANIFEST_VERSION) {
        return false;
    }
    if (memcmp(bytes + 16U, SUPPORT_BUNDLE_MANIFEST_SCHEMA_ID,
               SUPPORT_BUNDLE_MANIFEST_SCHEMA_ID_LENGTH) != 0 ||
        load_u32(bytes + 48U) != SUPPORT_BUNDLE_MANIFEST_VERSION ||
        load_u32(bytes + 52U) != METRICS_VERSION ||
        load_u32(bytes + 56U) != APPLIANCE_STATUS_VERSION ||
        load_u32(bytes + 60U) != HEALTH_CHECK_VERSION ||
        load_u32(bytes + 64U) != CONFIG_SCHEMA_VERSION ||
        load_u32(bytes + 68U) != KERNEL_EVENT_RING_CAPACITY ||
        load_u32(bytes + 72U) != SUPPORT_BUNDLE_MANIFEST_ENTRY_MAX ||
        load_u32(bytes + 76U) != source->audit_count) {
        return false;
    }
    if (source->audit_count > 0U) {
        expected_audit_first = source->audit_records[0].sequence;
        expected_audit_last =
            source->audit_records[source->audit_count - 1U].sequence;
    }
    hash_config(config, config_digest, &expected_included,
                &expected_excluded, &expected_config_length);
    if (load_u32(bytes + 80U) != expected_included ||
        load_u32(bytes + 84U) != expected_excluded ||
        load_u64(bytes + 88U) != expected_audit_first ||
        load_u64(bytes + 96U) != expected_audit_last ||
        load_u64(bytes + 104U) != expected_config_length ||
        memcmp(bytes + 112U,
               SUPPORT_BUNDLE_MANIFEST_SECRET_POLICY_ID,
               SUPPORT_BUNDLE_MANIFEST_SECRET_POLICY_ID_LENGTH) != 0) {
        return false;
    }

    for (entry_index = 0U;
         entry_index < SUPPORT_BUNDLE_MANIFEST_ENTRY_MAX;
         ++entry_index) {
        const uint32_t expected_type = entry_index + 1U;
        const size_t offset = entry_offset(entry_index);
        unsigned char expected_digest[SHA256_DIGEST_SIZE];
        uint32_t expected_length = 0U;

        if (load_u32(bytes + offset) != expected_type ||
            !expected_digest_for_type(expected_type, source, config,
                                      expected_digest, &expected_length) ||
            load_u32(bytes + offset + 4U) != expected_length ||
            memcmp(bytes + offset + 8U, expected_digest,
                   SHA256_DIGEST_SIZE) != 0) {
            return false;
        }
    }
    return true;
}

static void test_status_and_schema_strings(void)
{
    CHECK(support_bundle_manifest_status_string(
              SUPPORT_BUNDLE_MANIFEST_OK) != NULL &&
              support_bundle_manifest_status_string(
                  SUPPORT_BUNDLE_MANIFEST_BAD_CONFIG) != NULL,
          "manifest status strings are present");
    CHECK(support_bundle_manifest_status_string(
              (enum support_bundle_manifest_status)99) == NULL,
          "undefined manifest status returns null");
    CHECK(support_bundle_manifest_entry_type_string(
              SUPPORT_BUNDLE_ENTRY_METRICS) != NULL &&
              support_bundle_manifest_entry_type_string(
                  SUPPORT_BUNDLE_ENTRY_VERSIONS) != NULL,
          "manifest entry type strings are present");
    CHECK(support_bundle_manifest_entry_type_string(
              (enum support_bundle_entry_type)99) == NULL,
          "undefined manifest entry type returns null");
    CHECK(strcmp(support_bundle_manifest_schema_id(),
                 SUPPORT_BUNDLE_MANIFEST_SCHEMA_ID) == 0,
          "manifest schema ID is exact");
}

static void test_null_and_fail_closed(void)
{
    unsigned char buffer[SUPPORT_BUNDLE_MANIFEST_SIZE];
    size_t length;

    poison_output(buffer, sizeof(buffer), &length);
    CHECK(support_bundle_manifest_export(NULL, buffer,
                                         sizeof(buffer), &length) ==
              SUPPORT_BUNDLE_MANIFEST_NULL &&
              output_unchanged(buffer, sizeof(buffer), length),
          "null source fails without output mutation");

    poison_output(buffer, sizeof(buffer), &length);
    CHECK(support_bundle_manifest_export(&test_source, NULL,
                                         sizeof(buffer), &length) ==
              SUPPORT_BUNDLE_MANIFEST_NULL &&
              output_unchanged(buffer, sizeof(buffer), length),
          "null buffer fails without output mutation");

    poison_output(buffer, sizeof(buffer), &length);
    CHECK(support_bundle_manifest_export(&test_source, buffer,
                                         sizeof(buffer), NULL) ==
              SUPPORT_BUNDLE_MANIFEST_NULL &&
              output_unchanged(buffer, sizeof(buffer), length),
          "null length output fails without output mutation");

    poison_output(buffer, sizeof(buffer), &length);
    CHECK(support_bundle_manifest_export(&test_source, buffer,
                                         sizeof(buffer) - 1U,
                                         &length) ==
              SUPPORT_BUNDLE_MANIFEST_TOO_SMALL &&
              output_unchanged(buffer, sizeof(buffer), length),
          "undersized buffer fails without output mutation");
}

static void test_invalid_sources(void)
{
    struct support_bundle_manifest_source invalid;
    struct metrics_snapshot bad_metrics;
    struct appliance_status_snapshot bad_status;
    struct health_check_snapshot bad_health;
    struct audit_record bad_audit[TEST_AUDIT_COUNT];
    unsigned char buffer[SUPPORT_BUNDLE_MANIFEST_SIZE];
    size_t length;

    invalid = test_source;
    invalid.version = 0U;
    poison_output(buffer, sizeof(buffer), &length);
    CHECK(support_bundle_manifest_export(&invalid, buffer,
                                         sizeof(buffer), &length) ==
              SUPPORT_BUNDLE_MANIFEST_BAD_VERSION &&
              output_unchanged(buffer, sizeof(buffer), length),
          "unsupported source version fails closed");

    bad_metrics = test_metrics;
    bad_metrics.paths.health.check_count = 99U;
    invalid = test_source;
    invalid.metrics = &bad_metrics;
    poison_output(buffer, sizeof(buffer), &length);
    CHECK(support_bundle_manifest_export(&invalid, buffer,
                                         sizeof(buffer), &length) ==
              SUPPORT_BUNDLE_MANIFEST_BAD_SNAPSHOT &&
              output_unchanged(buffer, sizeof(buffer), length),
          "invalid metrics snapshot fails closed");

    bad_status = test_status;
    bad_status.service_count = 99U;
    invalid = test_source;
    invalid.status = &bad_status;
    poison_output(buffer, sizeof(buffer), &length);
    CHECK(support_bundle_manifest_export(&invalid, buffer,
                                         sizeof(buffer), &length) ==
              SUPPORT_BUNDLE_MANIFEST_BAD_SNAPSHOT &&
              output_unchanged(buffer, sizeof(buffer), length),
          "invalid status snapshot fails closed");

    bad_health = test_health;
    bad_health.descriptor_count = 99U;
    invalid = test_source;
    invalid.health = &bad_health;
    poison_output(buffer, sizeof(buffer), &length);
    CHECK(support_bundle_manifest_export(&invalid, buffer,
                                         sizeof(buffer), &length) ==
              SUPPORT_BUNDLE_MANIFEST_BAD_SNAPSHOT &&
              output_unchanged(buffer, sizeof(buffer), length),
          "invalid health snapshot fails closed");

    memcpy(bad_audit, test_audit, sizeof(bad_audit));
    bad_audit[1].sequence = 10U;
    invalid = test_source;
    invalid.audit_records = bad_audit;
    poison_output(buffer, sizeof(buffer), &length);
    CHECK(support_bundle_manifest_export(&invalid, buffer,
                                         sizeof(buffer), &length) ==
              SUPPORT_BUNDLE_MANIFEST_BAD_AUDIT &&
              output_unchanged(buffer, sizeof(buffer), length),
          "non-increasing audit slice fails closed");

    invalid = test_source;
    invalid.audit_count = KERNEL_EVENT_RING_CAPACITY + 1U;
    poison_output(buffer, sizeof(buffer), &length);
    CHECK(support_bundle_manifest_export(&invalid, buffer,
                                         sizeof(buffer), &length) ==
              SUPPORT_BUNDLE_MANIFEST_BAD_AUDIT &&
              output_unchanged(buffer, sizeof(buffer), length),
          "oversized audit slice fails closed");

    test_config.entries[0].key_length = 0U;
    invalid = test_source;
    poison_output(buffer, sizeof(buffer), &length);
    CHECK(support_bundle_manifest_export(&invalid, buffer,
                                         sizeof(buffer), &length) ==
              SUPPORT_BUNDLE_MANIFEST_BAD_CONFIG &&
              output_unchanged(buffer, sizeof(buffer), length),
          "malformed config metadata fails closed");
    test_config.entries[0].key_length = 5U;
}

static void test_valid_export_and_determinism(void)
{
    unsigned char first[SUPPORT_BUNDLE_MANIFEST_SIZE];
    unsigned char second[SUPPORT_BUNDLE_MANIFEST_SIZE];
    struct config_kv reverse_config;
    struct config_kv_entry reverse_entries[TEST_CONFIG_CAPACITY];
    struct support_bundle_manifest_source reverse_source;
    size_t length;
    uint32_t index;

    length = 0U;
    CHECK(support_bundle_manifest_export(&test_source, first,
                                         sizeof(first), &length) ==
              SUPPORT_BUNDLE_MANIFEST_OK,
          "valid source exports support bundle manifest");
    CHECK(length == SUPPORT_BUNDLE_MANIFEST_SIZE &&
              manifest_matches_fixture(first, length, &test_source,
                                       &test_config),
          "exported manifest matches exact fixed fixture");

    length = 0U;
    CHECK(support_bundle_manifest_export(&test_source, second,
                                         sizeof(second), &length) ==
              SUPPORT_BUNDLE_MANIFEST_OK &&
              memcmp(first, second, sizeof(first)) == 0,
          "equal source exports byte-identical manifest");

    populate_config(&reverse_config, reverse_entries, true);
    reverse_source = test_source;
    reverse_source.config = &reverse_config;
    length = 0U;
    CHECK(support_bundle_manifest_export(&reverse_source, second,
                                         sizeof(second), &length) ==
              SUPPORT_BUNDLE_MANIFEST_OK &&
              memcmp(first, second, sizeof(first)) == 0,
          "reversed config insertion exports byte-identical manifest");

    for (index = 0U; index < SUPPORT_BUNDLE_MANIFEST_ENTRY_MAX;
         ++index) {
        const size_t offset = entry_offset(index);

        CHECK(load_u32(first + offset) == index + 1U,
              "manifest entries are ordered by fixed type");
    }
}

static void test_digest_change_and_secret_exclusion(void)
{
    unsigned char baseline[SUPPORT_BUNDLE_MANIFEST_SIZE];
    unsigned char changed[SUPPORT_BUNDLE_MANIFEST_SIZE];
    struct metrics_snapshot changed_metrics;
    struct config_kv_entry secret_entries[TEST_CONFIG_CAPACITY];
    struct config_kv secret_config;
    struct support_bundle_manifest_source source;
    unsigned char config_digest[SHA256_DIGEST_SIZE];
    uint32_t included;
    uint32_t excluded;
    size_t metadata_length;
    size_t length;

    length = 0U;
    CHECK(support_bundle_manifest_export(&test_source, baseline,
                                         sizeof(baseline), &length) ==
              SUPPORT_BUNDLE_MANIFEST_OK,
          "baseline manifest exports");

    changed_metrics = test_metrics;
    changed_metrics.generation = 99U;
    source = test_source;
    source.metrics = &changed_metrics;
    length = 0U;
    CHECK(support_bundle_manifest_export(&source, changed,
                                         sizeof(changed), &length) ==
              SUPPORT_BUNDLE_MANIFEST_OK &&
              memcmp(baseline, changed, sizeof(baseline)) != 0,
          "metrics change changes manifest bytes");

    populate_config(&secret_config, secret_entries, false);
    hash_config(&secret_config, config_digest, &included, &excluded,
                &metadata_length);
    CHECK(included == 2U && excluded == 2U,
          "secret-bearing config fields are excluded and counted");
    CHECK(metadata_length == 39U,
          "config metadata length excludes secret-bearing values");

    secret_config.entries[2].value[0] = (unsigned char)'x';
    {
        unsigned char changed_digest[SHA256_DIGEST_SIZE];
        uint32_t changed_included = 0U;
        uint32_t changed_excluded = 0U;
        size_t changed_length = 0U;

        hash_config(&secret_config, changed_digest, &changed_included,
                    &changed_excluded, &changed_length);
        CHECK(changed_included == 2U && changed_excluded == 2U &&
                  memcmp(config_digest, changed_digest,
                         SHA256_DIGEST_SIZE) == 0,
              "changing excluded secret value cannot change config digest");
    }

    CHECK(support_bundle_manifest_entry_type_string(
              SUPPORT_BUNDLE_ENTRY_CONFIG_METADATA) != NULL &&
              support_bundle_manifest_status_string(
                  SUPPORT_BUNDLE_MANIFEST_OK) != NULL,
          "manifest strings remain usable after fixture mutation");
}

static void test_no_allocation(void)
{
    unsigned char buffer[SUPPORT_BUNDLE_MANIFEST_SIZE];
    size_t length = 0U;

    alloc_attempts = 0U;
    free_attempts = 0U;
    CHECK(support_bundle_manifest_export(&test_source, buffer,
                                         sizeof(buffer), &length) ==
              SUPPORT_BUNDLE_MANIFEST_OK,
          "manifest fixture re-exports after mutation");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "manifest export performs no allocation or free");
}

int main(void)
{
    init_metrics(&test_metrics);
    init_status(&test_status);
    init_health(&test_health);
    init_audit(test_audit);
    populate_config(&test_config, test_entries, false);
    init_source(&test_source);

    test_status_and_schema_strings();
    test_null_and_fail_closed();
    test_invalid_sources();
    test_valid_export_and_determinism();
    test_digest_change_and_secret_exclusion();
    test_no_allocation();

    if (failures == 0) {
        printf("Support bundle manifest host tests: ALL OK\n");
        return 0;
    }
    fprintf(stderr, "Support bundle manifest host tests: %d FAILURES\n",
            failures);
    return 1;
}

#endif
