#include "backup_manifest.h"
#include "backup_stream.h"
#include "sha256.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifdef HFOC_BACKUP_MANIFEST_HOST_TEST

#define TEST_CAPACITY 4U

static int failures;
static unsigned int alloc_attempts;
static unsigned int free_attempts;
static const unsigned char manifest_magic[BACKUP_MANIFEST_MAGIC_SIZE] =
    BACKUP_MANIFEST_MAGIC_BYTES;

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

static bool bytes_equal(const unsigned char *left,
                        const unsigned char *right,
                        size_t length)
{
    return memcmp(left, right, length) == 0;
}

static void load_u32(const unsigned char *bytes, uint32_t *value_out)
{
    *value_out = (uint32_t)bytes[0] |
                 ((uint32_t)bytes[1] << 8U) |
                 ((uint32_t)bytes[2] << 16U) |
                 ((uint32_t)bytes[3] << 24U);
}

static void load_u64(const unsigned char *bytes, uint64_t *value_out)
{
    uint64_t value = 0U;
    size_t index;

    for (index = 0U; index < 8U; ++index) {
        value |= (uint64_t)bytes[index] << (8U * index);
    }
    *value_out = value;
}

static void init_empty_kv(struct config_kv *kv,
                          struct config_kv_entry *entries,
                          size_t capacity)
{
    memset(entries, 0, sizeof(entries[0]) * capacity);
    CHECK(config_kv_init(kv, entries, capacity) == CONFIG_KV_OK,
          "manifest fixture initializes kv");
}

static void populate_state(struct config_kv *kv,
                           struct config_kv_entry *entries,
                           bool reverse_slots)
{
    bool replaced = false;

    init_empty_kv(kv, entries, TEST_CAPACITY);
    if (!reverse_slots) {
        CHECK(config_kv_put(kv, CONFIG_KV_KIND_CONFIG,
                            "beta", 4U, "two", 3U, &replaced) ==
                  CONFIG_KV_OK &&
                  config_kv_put(kv, CONFIG_KV_KIND_AUDIT,
                                "beta", 4U, "audit-two", 9U, &replaced) ==
                      CONFIG_KV_OK &&
                  config_kv_put(kv, CONFIG_KV_KIND_CONFIG,
                                "alpha", 5U, "one", 3U, &replaced) ==
                      CONFIG_KV_OK &&
                  config_kv_put(kv, CONFIG_KV_KIND_AUDIT,
                                "alpha", 5U, "audit-one", 9U, &replaced) ==
                      CONFIG_KV_OK,
              "manifest forward checkpoint populates exact entries");
    } else {
        CHECK(config_kv_put(kv, CONFIG_KV_KIND_AUDIT,
                            "alpha", 5U, "audit-one", 9U, &replaced) ==
                  CONFIG_KV_OK &&
                  config_kv_put(kv, CONFIG_KV_KIND_CONFIG,
                                "alpha", 5U, "one", 3U, &replaced) ==
                      CONFIG_KV_OK &&
                  config_kv_put(kv, CONFIG_KV_KIND_AUDIT,
                                "beta", 4U, "audit-two", 9U, &replaced) ==
                      CONFIG_KV_OK &&
                  config_kv_put(kv, CONFIG_KV_KIND_CONFIG,
                                "beta", 4U, "two", 3U, &replaced) ==
                      CONFIG_KV_OK,
              "manifest reverse checkpoint populates exact entries");
    }
}

static struct state_schema_descriptor make_descriptor(
    uint8_t kind,
    uint8_t target_kind,
    const char *id,
    uint32_t version,
    uint32_t minimum_version,
    uint32_t maximum_version,
    uint16_t minimum_length,
    uint16_t maximum_length)
{
    struct state_schema_descriptor descriptor;
    size_t id_length = strlen(id);

    memset(&descriptor, 0, sizeof(descriptor));
    descriptor.kind = kind;
    descriptor.target_kind = target_kind;
    descriptor.id_length = (uint8_t)id_length;
    memcpy(descriptor.id, id, id_length);
    descriptor.version = version;
    descriptor.minimum_version = minimum_version;
    descriptor.maximum_version = maximum_version;
    descriptor.minimum_length = minimum_length;
    descriptor.maximum_length = maximum_length;
    return descriptor;
}

static void init_registry(struct state_schema_registry *registry)
{
    struct state_schema_descriptor descriptors[3];

    descriptors[0] = make_descriptor(
        STATE_SCHEMA_KIND_JOURNAL,
        STATE_SCHEMA_TARGET_JOURNAL,
        "config", 1U, 1U, 1U, 1U, 256U);
    descriptors[1] = make_descriptor(
        STATE_SCHEMA_KIND_KV,
        CONFIG_KV_KIND_CONFIG,
        "config", 1U, 1U, 1U, 1U, CONFIG_KV_VALUE_MAX);
    descriptors[2] = make_descriptor(
        STATE_SCHEMA_KIND_KV,
        CONFIG_KV_KIND_AUDIT,
        "audit", 1U, 1U, 1U, 1U, CONFIG_KV_VALUE_MAX);
    CHECK(state_schema_registry_init(registry, descriptors, 3U) ==
              STATE_SCHEMA_OK,
          "manifest fixture initializes schema registry");
}

static void init_generation(
    struct journal_compaction_generation *generation,
    bool reverse_slots,
    uint64_t source_sequence,
    uint64_t generation_id)
{
    struct config_kv kv;
    struct config_kv_entry generation_entries[TEST_CAPACITY];
    struct journal_compaction_candidate candidate;
    struct journal_compaction_checkpoint checkpoints[1];
    struct journal_compaction_commit commit;

    memset(&commit, 0, sizeof(commit));
    populate_state(&kv, generation_entries, reverse_slots);
    checkpoints[0].version = JOURNAL_COMPACTION_CHECKPOINT_VERSION;
    checkpoints[0].sequence = source_sequence;
    checkpoints[0].kv = &kv;
    CHECK(journal_compaction_select(&candidate, checkpoints, 1U) ==
              JOURNAL_COMPACTION_OK,
          "manifest fixture selects candidate");
    candidate.source_sequence = source_sequence;
    CHECK(journal_compaction_commit_init(&commit, &candidate) ==
              JOURNAL_COMPACTION_OK,
          "manifest fixture publishes initial generation");
    *generation = commit.slots[0];
    generation->generation_id = generation_id;
    generation->active = true;
}

static void make_source(struct backup_manifest_source *source,
                        struct config_kv *kv,
                        struct config_kv_entry *entries,
                        struct state_schema_registry *registry,
                        struct journal_compaction_generation *generation,
                        bool reverse_slots,
                        uint64_t sequence)
{
    populate_state(kv, entries, reverse_slots);
    init_registry(registry);
    init_generation(generation, reverse_slots, sequence, sequence + 1U);
    memset(source, 0, sizeof(*source));
    source->checkpoint = kv;
    source->registry = registry;
    source->generation = generation;
}

static bool manifest_matches_fields(
    const unsigned char *bytes,
    const struct backup_manifest_source *source,
    size_t length)
{
    uint32_t config_count = 0U;
    uint32_t audit_count = 0U;
    uint64_t generation_id = 0U;
    uint64_t source_sequence = 0U;
    size_t config_seen = 0U;
    size_t audit_seen = 0U;
    size_t index;

    if (length != BACKUP_MANIFEST_SIZE ||
        memcmp(bytes, manifest_magic,
               BACKUP_MANIFEST_MAGIC_SIZE) != 0 ||
        bytes[8U] != BACKUP_MANIFEST_FORMAT_VERSION) {
        return false;
    }
    for (index = 9U; index < 16U; ++index) {
        if (bytes[index] != 0U) {
            return false;
        }
    }
    load_u32(bytes + 16U, &config_count);
    load_u32(bytes + 20U, &audit_count);
    load_u64(bytes + 120U, &generation_id);
    load_u64(bytes + 128U, &source_sequence);
    if (config_count != 2U || audit_count != 2U ||
        generation_id != source->generation->generation_id ||
        source_sequence != source->generation->source_sequence) {
        return false;
    }
    for (index = 0U; index < source->checkpoint->capacity; ++index) {
        const struct config_kv_entry *entry =
            &source->checkpoint->entries[index];

        if (entry->occupied && entry->kind == CONFIG_KV_KIND_CONFIG) {
            ++config_seen;
        } else if (entry->occupied) {
            ++audit_seen;
        }
    }
    return config_seen == config_count && audit_seen == audit_count;
}

static void frame_digest_for_kind(
    const unsigned char *stream,
    size_t length,
    uint8_t kind,
    unsigned char *digest_out)
{
    struct sha256 context;
    unsigned char digest[SHA256_DIGEST_SIZE];
    size_t offset = BACKUP_STREAM_HEADER_SIZE;
    size_t entry_count;
    size_t index;
    uint32_t config_count = 0U;
    uint32_t audit_count = 0U;

    load_u32(stream + 16U, &config_count);
    load_u32(stream + 20U, &audit_count);
    entry_count = (size_t)config_count + audit_count;
    CHECK(sha256_init(&context) == SHA256_OK,
          "manifest frame digest context initializes");
    for (index = 0U; index < entry_count; ++index) {
        uint8_t frame_kind = stream[offset];
        uint8_t key_length = stream[offset + 1U];
        uint16_t value_length;
        size_t frame_size;

        value_length = (uint16_t)stream[offset + 2U] |
                       ((uint16_t)stream[offset + 3U] << 8U);
        frame_size = BACKUP_STREAM_FRAME_HEADER_SIZE +
                     key_length + value_length;
        if (frame_kind == kind) {
            CHECK(sha256_update(&context, stream + offset, frame_size) ==
                      SHA256_OK,
                  "manifest frame digest updates with exact frame");
        }
        offset += frame_size;
    }
    CHECK(offset == length - BACKUP_STREAM_DIGEST_SIZE,
          "manifest frame digest consumes exact stream prefix");
    CHECK(sha256_final(&context, digest) == SHA256_OK,
          "manifest frame digest finalizes");
    memcpy(digest_out, digest, SHA256_DIGEST_SIZE);
}

static void check_export_failure(
    const struct backup_manifest_source *source,
    size_t capacity,
    enum backup_manifest_status expected,
    const char *message)
{
    unsigned char buffer[BACKUP_MANIFEST_SIZE + 1U];
    unsigned char before[sizeof(buffer)];
    size_t length = 0x5A5A5A5AU;

    memset(buffer, 0xA5, sizeof(buffer));
    memcpy(before, buffer, sizeof(buffer));
    CHECK(backup_manifest_export(source, buffer, capacity, &length) ==
              expected,
          message);
    CHECK(memcmp(buffer, before, sizeof(buffer)) == 0,
          "failed manifest export leaves buffer unchanged");
    CHECK(length == 0x5A5A5A5AU,
          "failed manifest export leaves length unchanged");
}

static void test_validation_and_status_strings(void)
{
    static const enum backup_manifest_status statuses[] = {
        BACKUP_MANIFEST_OK,
        BACKUP_MANIFEST_NULL,
        BACKUP_MANIFEST_BAD_STATE,
        BACKUP_MANIFEST_TOO_SMALL,
    };
    struct backup_manifest_source source;
    struct config_kv bad_kv;
    struct config_kv_entry entries[TEST_CAPACITY];
    struct config_kv kv;
    struct state_schema_registry registry;
    struct journal_compaction_generation generation;
    unsigned char buffer[BACKUP_MANIFEST_SIZE];
    unsigned char before[sizeof(buffer)];
    size_t length = 0x5A5A5A5AU;
    size_t index;

    make_source(&source, &kv, entries, &registry, &generation, false, 7U);
    memset(buffer, 0xA5, sizeof(buffer));
    memcpy(before, buffer, sizeof(buffer));
    CHECK(backup_manifest_export(NULL, buffer, sizeof(buffer),
                                 &length) == BACKUP_MANIFEST_NULL,
          "manifest export rejects null source");
    CHECK(backup_manifest_export(&source, NULL, sizeof(buffer),
                                 &length) == BACKUP_MANIFEST_NULL,
          "manifest export rejects null buffer");
    CHECK(backup_manifest_export(&source, buffer, sizeof(buffer),
                                 NULL) == BACKUP_MANIFEST_NULL,
          "manifest export rejects null length output");
    CHECK(memcmp(buffer, before, sizeof(buffer)) == 0 &&
              length == 0x5A5A5A5AU,
          "null manifest failures leave outputs unchanged");

    memset(&bad_kv, 0, sizeof(bad_kv));
    source.checkpoint = &bad_kv;
    check_export_failure(&source, sizeof(buffer),
                         BACKUP_MANIFEST_BAD_STATE,
                         "uninitialized checkpoint is bad state");
    make_source(&source, &kv, entries, &registry, &generation, false, 7U);
    source.registry = NULL;
    check_export_failure(&source, sizeof(buffer),
                         BACKUP_MANIFEST_BAD_STATE,
                         "null registry is bad state");
    make_source(&source, &kv, entries, &registry, &generation, false, 7U);
    source.generation = NULL;
    check_export_failure(&source, sizeof(buffer),
                         BACKUP_MANIFEST_BAD_STATE,
                         "null generation is bad state");

    make_source(&source, &kv, entries, &registry, &generation, false, 7U);
    check_export_failure(&source, BACKUP_MANIFEST_SIZE - 1U,
                         BACKUP_MANIFEST_TOO_SMALL,
                         "short manifest buffer returns too-small");

    for (index = 0U;
         index < sizeof(statuses) / sizeof(statuses[0]);
         ++index) {
        CHECK(backup_manifest_status_string(statuses[index]) != NULL &&
                  strcmp(backup_manifest_status_string(statuses[index]),
                         statuses[index] == BACKUP_MANIFEST_OK ?
                             "BACKUP_MANIFEST_OK" :
                         statuses[index] == BACKUP_MANIFEST_NULL ?
                             "BACKUP_MANIFEST_NULL" :
                         statuses[index] == BACKUP_MANIFEST_BAD_STATE ?
                             "BACKUP_MANIFEST_BAD_STATE" :
                             "BACKUP_MANIFEST_TOO_SMALL") == 0,
              "manifest status string is exact");
    }
    CHECK(strcmp(backup_manifest_status_string(
                     (enum backup_manifest_status)99U),
                 "BACKUP_MANIFEST_UNKNOWN") == 0,
          "manifest undefined status string is exact");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "manifest validation performs no allocation");
}

static void test_empty_and_populated_inventory(void)
{
    struct backup_manifest_source source;
    struct config_kv_entry entries[TEST_CAPACITY];
    struct config_kv kv;
    struct state_schema_registry registry;
    struct journal_compaction_generation generation;
    unsigned char buffer[BACKUP_MANIFEST_SIZE];
    size_t length = 0U;
    unsigned char config_digest[SHA256_DIGEST_SIZE];
    unsigned char audit_digest[SHA256_DIGEST_SIZE];
    unsigned char empty_digest[SHA256_DIGEST_SIZE];
    unsigned char stream[1024U];
    size_t stream_length = 0U;
    uint32_t config_count = 0U;
    uint32_t audit_count = 0U;
    uint64_t generation_id = 0U;
    uint64_t source_sequence = 0U;
    size_t index;

    CHECK(sha256_compute("", 0U, empty_digest) == SHA256_OK,
          "manifest empty digest fixture computes");
    init_empty_kv(&kv, entries, TEST_CAPACITY);
    init_registry(&registry);
    init_generation(&generation, false, 1U, 1U);
    memset(&source, 0, sizeof(source));
    source.checkpoint = &kv;
    source.registry = &registry;
    source.generation = &generation;

    memset(buffer, 0xCD, sizeof(buffer));
    length = 0U;
    CHECK(backup_manifest_export(&source, buffer, sizeof(buffer),
                                 &length) == BACKUP_MANIFEST_OK &&
              length == BACKUP_MANIFEST_SIZE,
          "empty manifest exports fixed size");
    load_u32(buffer + 16U, &config_count);
    load_u32(buffer + 20U, &audit_count);
    load_u64(buffer + 120U, &generation_id);
    load_u64(buffer + 128U, &source_sequence);
    CHECK(config_count == 0U && audit_count == 0U &&
              generation_id == 1U && source_sequence == 1U,
          "empty manifest publishes exact counts and generation");
    CHECK(bytes_equal(buffer + 24U, empty_digest,
                      BACKUP_MANIFEST_DIGEST_SIZE) &&
              bytes_equal(buffer + 56U, empty_digest,
                          BACKUP_MANIFEST_DIGEST_SIZE),
          "empty checkpoint hashes are SHA-256 of empty inputs");
    CHECK(!bytes_equal(buffer + 88U, empty_digest,
                       BACKUP_MANIFEST_DIGEST_SIZE),
          "schema hash covers non-empty schema registry");

    make_source(&source, &kv, entries, &registry, &generation, false, 7U);
    memset(buffer, 0xCD, sizeof(buffer));
    length = 0U;
    CHECK(backup_manifest_export(&source, buffer, sizeof(buffer),
                                 &length) == BACKUP_MANIFEST_OK &&
              manifest_matches_fields(buffer, &source, length),
          "populated manifest publishes exact metadata fields");
    CHECK(backup_stream_export(&kv, stream, sizeof(stream),
                               &stream_length) == BACKUP_STREAM_OK,
          "manifest digest fixture exports canonical stream");
    frame_digest_for_kind(stream, stream_length, CONFIG_KV_KIND_CONFIG,
                          config_digest);
    frame_digest_for_kind(stream, stream_length, CONFIG_KV_KIND_AUDIT,
                          audit_digest);
    CHECK(bytes_equal(buffer + 24U, config_digest,
                      BACKUP_MANIFEST_DIGEST_SIZE) &&
              bytes_equal(buffer + 56U, audit_digest,
                          BACKUP_MANIFEST_DIGEST_SIZE),
          "manifest config/audit digests match canonical backup frames");
    for (index = 136U; index < BACKUP_MANIFEST_SIZE; ++index) {
        CHECK(buffer[index] == 0U,
              "manifest reserved tail is zero");
    }
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "manifest export performs no allocation");
}

static void test_repeated_export_is_byte_identical(void)
{
    struct backup_manifest_source first_source;
    struct backup_manifest_source second_source;
    struct config_kv_entry first_entries[TEST_CAPACITY];
    struct config_kv_entry second_entries[TEST_CAPACITY];
    struct config_kv first_kv;
    struct config_kv second_kv;
    struct state_schema_registry first_registry;
    struct state_schema_registry second_registry;
    struct journal_compaction_generation first_generation;
    struct journal_compaction_generation second_generation;
    unsigned char first[BACKUP_MANIFEST_SIZE];
    unsigned char second[BACKUP_MANIFEST_SIZE];
    size_t first_length = 0U;
    size_t second_length = 0U;

    make_source(&first_source, &first_kv, first_entries,
                &first_registry, &first_generation, false, 7U);
    make_source(&second_source, &second_kv, second_entries,
                &second_registry, &second_generation, true, 7U);
    CHECK(backup_manifest_export(&first_source, first, sizeof(first),
                                 &first_length) == BACKUP_MANIFEST_OK &&
              backup_manifest_export(&second_source, second, sizeof(second),
                                     &second_length) == BACKUP_MANIFEST_OK,
          "forward and reverse equal states export successfully");
    CHECK(first_length == second_length &&
              bytes_equal(first, second, BACKUP_MANIFEST_SIZE),
          "equal logical state exports byte-identical inventory");

    populate_state(&first_kv, first_entries, false);
    {
        bool replaced = false;

        CHECK(config_kv_put(&first_kv, CONFIG_KV_KIND_CONFIG,
                            "beta", 4U, "three", 5U, &replaced) ==
                  CONFIG_KV_OK,
              "manifest checkpoint-change fixture replaces value");
    }
    CHECK(backup_manifest_export(&first_source, first, sizeof(first),
                                 &first_length) == BACKUP_MANIFEST_OK &&
              !bytes_equal(first, second, BACKUP_MANIFEST_SIZE),
          "checkpoint value change changes inventory bytes");

    make_source(&first_source, &first_kv, first_entries,
                &first_registry, &first_generation, false, 8U);
    CHECK(backup_manifest_export(&first_source, first, sizeof(first),
                                 &first_length) == BACKUP_MANIFEST_OK &&
              !bytes_equal(first, second, BACKUP_MANIFEST_SIZE),
          "generation identity change changes inventory bytes");

    make_source(&first_source, &first_kv, first_entries,
                &first_registry, &first_generation, false, 7U);
    first_registry.descriptors[0].maximum_length = 512U;
    CHECK(backup_manifest_export(&first_source, first, sizeof(first),
                                 &first_length) == BACKUP_MANIFEST_OK &&
              !bytes_equal(first, second, BACKUP_MANIFEST_SIZE),
          "schema version change changes inventory bytes");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "byte-identical manifest proof performs no allocation");
}

int main(void)
{
    test_validation_and_status_strings();
    test_empty_and_populated_inventory();
    test_repeated_export_is_byte_identical();

    if (failures != 0) {
        fprintf(stderr, "Backup manifest host tests: FAIL (%d)\n", failures);
        return 1;
    }
    printf("Backup manifest host tests: ALL OK\n");
    return 0;
}

#endif
