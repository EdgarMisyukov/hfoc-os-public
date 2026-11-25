#include "boot_evidence.h"

#include "image_policy.h"
#include "key_use.h"
#include "sha256.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifdef HFOC_BOOT_EVIDENCE_HOST_TEST

#define TEST_KV_CAPACITY 4U

#define KERNEL_DATA "kernel-image"
#define INITRAMFS_DATA "initramfs-image"

#define KERNEL_HASH \
    "7d85a326c4eaf5c09d11bed101a08ea50915897a31b627285b5b3446bd37262f"
#define INITRAMFS_HASH \
    "c94f8be14b260af440b8e93fc1b208e55f8eb554439408552ea5aa078968158b"

static int failures;
static unsigned int alloc_attempts;
static unsigned int free_attempts;

#define CHECK(cond, msg) do { \
    if (!(cond)) { \
        fprintf(stderr, "FAIL: %s\n", msg); \
        ++failures; \
    } else { \
        printf("PASS: %s\n", msg); \
    } \
} while (0)

void *kmalloc(size_t size)
{
    (void)size;
    ++alloc_attempts;
    return NULL;
}

bool kfree(void *ptr)
{
    (void)ptr;
    ++free_attempts;
    return false;
}

static struct trust_anchor make_anchor(uint32_t id)
{
    struct trust_anchor anchor;

    memset(&anchor, 0, sizeof(anchor));
    anchor.id = id;
    anchor.version = TRUST_ANCHOR_DESCRIPTOR_VERSION;
    anchor.algorithm_id = TRUST_ANCHOR_ALGORITHM_ED25519;
    anchor.state = TRUST_ANCHOR_STATE_ACTIVE;
    anchor.accepted_purposes = KEY_USE_ACCEPTED_PURPOSES_ALL;
    return anchor;
}

static struct verification_policy_snapshot make_policy_snapshot(
    uint32_t generation)
{
    struct trust_anchor anchors[2];
    struct verification_policy_snapshot snapshot;

    anchors[0] = make_anchor(1U);
    anchors[1] = make_anchor(2U);
    memset(&snapshot, 0, sizeof(snapshot));
    snapshot.version = VERIFICATION_POLICY_VERSION;
    snapshot.generation = generation;
    snapshot.anchor_count = 2U;
    memcpy(snapshot.anchors, anchors, sizeof(anchors));
    return snapshot;
}

static bool init_policy(struct verification_policy *policy,
                        uint32_t generation)
{
    struct verification_policy_snapshot snapshot =
        make_policy_snapshot(generation);

    memset(policy, 0, sizeof(*policy));
    return verification_policy_init(policy, &snapshot) ==
           VERIFICATION_POLICY_OK;
}

static struct system_image_manifest make_manifest(uint32_t image_id)
{
    struct system_image_manifest manifest;

    memset(&manifest, 0, sizeof(manifest));
    manifest.manifest_version = SYSTEM_IMAGE_MANIFEST_VERSION;
    manifest.image_id = image_id;
    manifest.min_kernel_abi = 1U;
    manifest.max_kernel_abi = 3U;
    strcpy(manifest.arch, SYSTEM_IMAGE_ARCH_VALUE);
    manifest.component_count = 2U;
    strcpy(manifest.components[0].name, "kernel");
    strcpy(manifest.components[0].version, "0.2.6");
    strcpy(manifest.components[0].hash, KERNEL_HASH);
    strcpy(manifest.components[1].name, "initramfs");
    strcpy(manifest.components[1].version, "1.0");
    strcpy(manifest.components[1].hash, INITRAMFS_HASH);
    return manifest;
}

static void make_payloads(struct image_integrity_payload payloads[2])
{
    static const char kernel[] = KERNEL_DATA;
    static const char initramfs[] = INITRAMFS_DATA;

    payloads[0].name = "kernel";
    payloads[0].data = kernel;
    payloads[0].data_length = sizeof(kernel) - 1U;
    payloads[1].name = "initramfs";
    payloads[1].data = initramfs;
    payloads[1].data_length = sizeof(initramfs) - 1U;
}

static bool make_gate(struct image_integrity_gate *gate,
                      uint32_t image_id)
{
    struct image_integrity_payload payloads[2];
    struct system_image_manifest manifest = make_manifest(image_id);

    make_payloads(payloads);
    memset(gate, 0, sizeof(*gate));
    return image_integrity_verify(gate, &manifest, payloads, 2U) ==
           IMAGE_INTEGRITY_OK;
}

static void make_measurement(uint8_t measurement[SHA256_DIGEST_SIZE])
{
    size_t index;

    for (index = 0U; index < SHA256_DIGEST_SIZE; ++index) {
        measurement[index] = (uint8_t)(0xA0U + index);
    }
}

static void poison_evidence(struct boot_evidence *evidence)
{
    memset(evidence, 0xA5, sizeof(*evidence));
    evidence->published = false;
}

static bool evidence_unchanged(
    const struct boot_evidence *evidence,
    const struct boot_evidence *original)
{
    return memcmp(evidence, original, sizeof(*evidence)) == 0;
}

static void init_audit_kv(struct config_kv *kv,
                          struct config_kv_entry *entries,
                          size_t capacity)
{
    memset(entries, 0, capacity * sizeof(entries[0]));
    CHECK(config_kv_init(kv, entries, capacity) == CONFIG_KV_OK,
          "audit fixture initializes bounded checkpoint");
}

static struct config_kv_entry *find_audit_entry(
    struct config_kv *kv,
    const char *key,
    size_t key_length)
{
    size_t index;

    for (index = 0U; index < kv->capacity; ++index) {
        struct config_kv_entry *entry = &kv->entries[index];

        if (entry->occupied &&
            entry->kind == CONFIG_KV_KIND_AUDIT &&
            entry->key_length == key_length &&
            memcmp(entry->key, key, key_length) == 0) {
            return entry;
        }
    }
    return NULL;
}

static void test_status_strings(void)
{
    static const struct {
        enum boot_evidence_status status;
        const char *string;
    } statuses[] = {
        {BOOT_EVIDENCE_OK, "BOOT_EVIDENCE_OK"},
        {BOOT_EVIDENCE_NULL, "BOOT_EVIDENCE_NULL"},
        {BOOT_EVIDENCE_BAD_STATE, "BOOT_EVIDENCE_BAD_STATE"},
        {BOOT_EVIDENCE_NO_POLICY, "BOOT_EVIDENCE_NO_POLICY"},
        {BOOT_EVIDENCE_NO_IMAGE, "BOOT_EVIDENCE_NO_IMAGE"},
        {BOOT_EVIDENCE_POLICY_MISMATCH,
         "BOOT_EVIDENCE_POLICY_MISMATCH"},
        {BOOT_EVIDENCE_INVALID_RECORD,
         "BOOT_EVIDENCE_INVALID_RECORD"},
        {BOOT_EVIDENCE_DIGEST_MISMATCH,
         "BOOT_EVIDENCE_DIGEST_MISMATCH"},
        {BOOT_EVIDENCE_STORAGE_FAILED,
         "BOOT_EVIDENCE_STORAGE_FAILED"},
        {BOOT_EVIDENCE_NOT_FOUND, "BOOT_EVIDENCE_NOT_FOUND"},
    };
    size_t index;

    for (index = 0U;
         index < sizeof(statuses) / sizeof(statuses[0]);
         ++index) {
        const char *string = boot_evidence_status_string(
            statuses[index].status);

        CHECK(string != NULL &&
                  strcmp(string, statuses[index].string) == 0,
              "boot evidence status string is exact");
    }
    CHECK(boot_evidence_status_string(
              (enum boot_evidence_status)10U) == NULL,
          "boot evidence undefined status is rejected");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "boot evidence status handling performs no allocation");
}

static void test_null_and_unpublished(void)
{
    struct verification_policy policy;
    struct image_policy_binding binding;
    struct config_kv_entry entries[TEST_KV_CAPACITY];
    struct config_kv kv;
    struct config_kv bad_kv;
    struct boot_evidence evidence;
    struct boot_evidence original;
    uint8_t measurement[SHA256_DIGEST_SIZE];
    struct image_integrity_gate gate;

    memset(&policy, 0, sizeof(policy));
    memset(&binding, 0, sizeof(binding));
    make_measurement(measurement);
    init_audit_kv(&kv, entries, TEST_KV_CAPACITY);
    memset(&bad_kv, 0, sizeof(bad_kv));
    poison_evidence(&evidence);
    memcpy(&original, &evidence, sizeof(evidence));
    make_gate(&gate, 7U);

    CHECK(boot_evidence_init(NULL, &policy, &binding, measurement) ==
              BOOT_EVIDENCE_NULL,
          "boot evidence init rejects null evidence");
    CHECK(boot_evidence_init(&evidence, NULL, &binding, measurement) ==
              BOOT_EVIDENCE_NULL,
          "boot evidence init rejects null policy");
    CHECK(boot_evidence_init(&evidence, &policy, NULL, measurement) ==
              BOOT_EVIDENCE_NULL,
          "boot evidence init rejects null binding");
    CHECK(boot_evidence_init(&evidence, &policy, &binding, NULL) ==
              BOOT_EVIDENCE_NULL,
          "boot evidence init rejects null measurement");
    CHECK(boot_evidence_save(NULL, &kv) == BOOT_EVIDENCE_NULL,
          "boot evidence save rejects null evidence");
    CHECK(boot_evidence_save(&evidence, NULL) == BOOT_EVIDENCE_NULL,
          "boot evidence save rejects null audit state");
    CHECK(boot_evidence_load(NULL, &kv) == BOOT_EVIDENCE_NULL,
          "boot evidence load rejects null evidence");
    CHECK(boot_evidence_load(&evidence, NULL) == BOOT_EVIDENCE_NULL,
          "boot evidence load rejects null audit state");
    CHECK(boot_evidence_check_policy(NULL, &policy) ==
              BOOT_EVIDENCE_NULL,
          "boot evidence policy check rejects null evidence");
    CHECK(boot_evidence_check_policy(&evidence, NULL) ==
              BOOT_EVIDENCE_NULL,
          "boot evidence policy check rejects null policy");
    CHECK(!boot_evidence_is_published(NULL),
          "boot evidence published query rejects null evidence");

    poison_evidence(&evidence);
    memcpy(&original, &evidence, sizeof(evidence));
    CHECK(boot_evidence_init(&evidence, &policy, &binding, measurement) ==
              BOOT_EVIDENCE_NO_POLICY &&
              evidence_unchanged(&evidence, &original),
          "boot evidence init with unpublished policy fails closed");

    CHECK(init_policy(&policy, 1U),
          "no-image fixture initializes active policy");
    CHECK(image_policy_bind(&binding, &policy, &gate) ==
              IMAGE_POLICY_OK,
          "no-image fixture publishes image policy binding");
    poison_evidence(&evidence);
    memcpy(&original, &evidence, sizeof(evidence));
    CHECK(boot_evidence_init(&evidence, &policy, &binding, measurement) ==
              BOOT_EVIDENCE_OK &&
              evidence.version == BOOT_EVIDENCE_VERSION &&
              evidence.policy_generation == 1U &&
              evidence.image_id == 7U &&
              evidence.digest_match &&
              evidence.published &&
              memcmp(evidence.measurement, measurement,
                     sizeof(measurement)) == 0,
          "boot evidence init publishes exact active generation and image");

    CHECK(boot_evidence_save(&evidence, &bad_kv) ==
              BOOT_EVIDENCE_STORAGE_FAILED,
          "boot evidence save rejects uninitialized audit state");
    poison_evidence(&evidence);
    CHECK(boot_evidence_save(&evidence, &kv) == BOOT_EVIDENCE_BAD_STATE,
          "boot evidence save rejects unpublished evidence");
    CHECK(boot_evidence_check_policy(&evidence, &policy) ==
              BOOT_EVIDENCE_BAD_STATE,
          "boot evidence policy check rejects unpublished evidence");

    poison_evidence(&evidence);
    memcpy(&original, &evidence, sizeof(evidence));
    CHECK(boot_evidence_load(&evidence, &kv) ==
              BOOT_EVIDENCE_NOT_FOUND &&
              evidence_unchanged(&evidence, &original),
          "boot evidence load reports missing audit evidence");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "boot evidence null handling performs no allocation");
}

static void test_reboot_continuity_and_policy_advance(void)
{
    struct verification_policy policy;
    struct image_integrity_gate gate;
    struct image_policy_binding binding;
    struct config_kv_entry entries[TEST_KV_CAPACITY];
    struct config_kv kv;
    struct boot_evidence evidence;
    struct boot_evidence loaded;
    uint8_t measurement[SHA256_DIGEST_SIZE];
    unsigned char record[BOOT_EVIDENCE_RECORD_SIZE];
    size_t record_length = 0U;
    uint64_t revision = 0U;
    struct verification_policy_snapshot next;

    make_measurement(measurement);
    init_audit_kv(&kv, entries, TEST_KV_CAPACITY);
    CHECK(init_policy(&policy, 1U),
          "round-trip fixture initializes active policy");
    CHECK(make_gate(&gate, 7U),
          "round-trip fixture publishes verified image gate");
    memset(&binding, 0, sizeof(binding));
    CHECK(image_policy_bind(&binding, &policy, &gate) ==
              IMAGE_POLICY_OK,
          "round-trip fixture binds image to active generation");
    CHECK(boot_evidence_init(&evidence, &policy, &binding, measurement) ==
              BOOT_EVIDENCE_OK,
          "round-trip fixture initializes evidence");

    CHECK(boot_evidence_save(&evidence, &kv) == BOOT_EVIDENCE_OK,
          "boot evidence save publishes audit checkpoint record");
    CHECK(config_kv_get(&kv, CONFIG_KV_KIND_AUDIT,
                        BOOT_EVIDENCE_KEY, BOOT_EVIDENCE_KEY_LEN,
                        record, sizeof(record), &record_length,
                        &revision) == CONFIG_KV_OK &&
              record_length == BOOT_EVIDENCE_RECORD_SIZE &&
              revision == 1U &&
              record[0] == 'H' && record[1] == 'F' &&
              record[2] == 'O' && record[3] == 'C' &&
              record[4] == 'E' && record[5] == 'V' &&
              record[6] == '0' && record[7] == '1' &&
              record[8] == 0U && record[9] == 0U &&
              record[10] == 0U && record[11] == BOOT_EVIDENCE_VERSION &&
              record[12] == 0U && record[13] == 0U &&
              record[14] == 0U && record[15] == 1U,
          "boot evidence save writes exact 96-byte framed record");

    poison_evidence(&loaded);
    CHECK(boot_evidence_load(&loaded, &kv) == BOOT_EVIDENCE_OK &&
              boot_evidence_is_published(&loaded) &&
              loaded.digest_match &&
              loaded.version == BOOT_EVIDENCE_VERSION &&
              loaded.policy_generation == 1U &&
              loaded.image_id == 7U &&
              memcmp(loaded.measurement, measurement,
                     sizeof(measurement)) == 0 &&
              memcmp(loaded.digest, evidence.digest,
                     sizeof(evidence.digest)) == 0,
          "boot evidence load preserves evidence after reboot");
    CHECK(boot_evidence_check_policy(&loaded, &policy) ==
              BOOT_EVIDENCE_OK,
          "boot evidence policy check matches original generation");

    next = make_policy_snapshot(2U);
    CHECK(verification_policy_stage(&policy, &next) ==
              VERIFICATION_POLICY_OK &&
              verification_policy_commit(&policy) ==
                  VERIFICATION_POLICY_OK,
          "round-trip fixture advances active policy generation");
    CHECK(boot_evidence_check_policy(&loaded, &policy) ==
              BOOT_EVIDENCE_POLICY_MISMATCH,
          "boot evidence policy check rejects superseded generation");
    poison_evidence(&loaded);
    CHECK(boot_evidence_load(&loaded, &kv) == BOOT_EVIDENCE_OK &&
              loaded.policy_generation == 1U,
          "boot evidence load remains policy-independent after reboot");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "boot evidence round-trip performs no allocation");
}

static void test_digest_mismatch_evidence(void)
{
    struct verification_policy policy;
    struct image_integrity_gate gate;
    struct image_policy_binding binding;
    struct config_kv_entry entries[TEST_KV_CAPACITY];
    struct config_kv kv;
    struct boot_evidence evidence;
    struct boot_evidence loaded;
    struct config_kv_entry *entry;
    uint8_t measurement[SHA256_DIGEST_SIZE];
    uint8_t tampered[SHA256_DIGEST_SIZE];

    make_measurement(measurement);
    init_audit_kv(&kv, entries, TEST_KV_CAPACITY);
    CHECK(init_policy(&policy, 1U),
          "digest fixture initializes active policy");
    CHECK(make_gate(&gate, 7U),
          "digest fixture publishes verified image gate");
    memset(&binding, 0, sizeof(binding));
    CHECK(image_policy_bind(&binding, &policy, &gate) ==
              IMAGE_POLICY_OK &&
              boot_evidence_init(&evidence, &policy, &binding,
                                 measurement) == BOOT_EVIDENCE_OK &&
              boot_evidence_save(&evidence, &kv) == BOOT_EVIDENCE_OK,
          "digest fixture saves valid evidence");

    entry = find_audit_entry(&kv, BOOT_EVIDENCE_KEY,
                             BOOT_EVIDENCE_KEY_LEN);
    CHECK(entry != NULL,
          "digest fixture finds persisted evidence entry");
    memcpy(tampered, measurement, sizeof(tampered));
    tampered[0] ^= 0x01U;
    if (entry != NULL) {
        entry->value[24U] ^= 0x01U;
    }

    poison_evidence(&loaded);
    CHECK(boot_evidence_load(&loaded, &kv) ==
              BOOT_EVIDENCE_DIGEST_MISMATCH &&
              boot_evidence_is_published(&loaded) &&
              !loaded.digest_match &&
              loaded.version == BOOT_EVIDENCE_VERSION &&
              loaded.policy_generation == 1U &&
              loaded.image_id == 7U &&
              memcmp(loaded.measurement, tampered,
                     sizeof(tampered)) == 0 &&
              memcmp(loaded.digest, evidence.digest,
                     sizeof(evidence.digest)) == 0,
          "boot evidence load retains explicit digest mismatch evidence");
    CHECK(boot_evidence_save(&loaded, &kv) == BOOT_EVIDENCE_BAD_STATE,
          "boot evidence cannot save a digest-mismatch record");
    CHECK(boot_evidence_check_policy(&loaded, &policy) ==
              BOOT_EVIDENCE_DIGEST_MISMATCH,
          "boot evidence policy check rejects digest mismatch");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "boot evidence digest mismatch performs no allocation");
}

static void test_invalid_records(void)
{
    struct verification_policy policy;
    struct image_integrity_gate gate;
    struct image_policy_binding binding;
    struct config_kv_entry entries[TEST_KV_CAPACITY];
    struct config_kv kv;
    struct boot_evidence evidence;
    struct boot_evidence loaded;
    struct boot_evidence original;
    struct config_kv_entry *entry;
    uint8_t measurement[SHA256_DIGEST_SIZE];
    unsigned char short_value[] = "short";
    unsigned char long_value[BOOT_EVIDENCE_RECORD_SIZE + 1U];
    bool replaced = false;

    make_measurement(measurement);
    init_audit_kv(&kv, entries, TEST_KV_CAPACITY);
    CHECK(init_policy(&policy, 1U),
          "invalid-record fixture initializes active policy");
    CHECK(make_gate(&gate, 7U),
          "invalid-record fixture publishes verified image gate");
    memset(&binding, 0, sizeof(binding));
    CHECK(image_policy_bind(&binding, &policy, &gate) ==
              IMAGE_POLICY_OK &&
              boot_evidence_init(&evidence, &policy, &binding,
                                 measurement) == BOOT_EVIDENCE_OK &&
              boot_evidence_save(&evidence, &kv) == BOOT_EVIDENCE_OK,
          "invalid-record fixture saves valid evidence");

    entry = find_audit_entry(&kv, BOOT_EVIDENCE_KEY,
                             BOOT_EVIDENCE_KEY_LEN);
    CHECK(entry != NULL,
          "invalid-record fixture finds persisted evidence entry");
    if (entry != NULL) {
        entry->value[6U] = '9';
    }
    poison_evidence(&loaded);
    memcpy(&original, &loaded, sizeof(loaded));
    CHECK(boot_evidence_load(&loaded, &kv) ==
              BOOT_EVIDENCE_INVALID_RECORD &&
              evidence_unchanged(&loaded, &original),
          "boot evidence rejects bad magic without overwriting evidence");

    init_audit_kv(&kv, entries, TEST_KV_CAPACITY);
    CHECK(config_kv_put(&kv, CONFIG_KV_KIND_AUDIT,
                        BOOT_EVIDENCE_KEY, BOOT_EVIDENCE_KEY_LEN,
                        short_value, sizeof(short_value) - 1U,
                        &replaced) == CONFIG_KV_OK,
          "invalid-record fixture stores short evidence value");
    poison_evidence(&loaded);
    memcpy(&original, &loaded, sizeof(loaded));
    CHECK(boot_evidence_load(&loaded, &kv) ==
              BOOT_EVIDENCE_INVALID_RECORD &&
              evidence_unchanged(&loaded, &original),
          "boot evidence rejects wrong-length record");

    memset(long_value, 0x5A, sizeof(long_value));
    CHECK(config_kv_put(&kv, CONFIG_KV_KIND_AUDIT,
                        BOOT_EVIDENCE_KEY, BOOT_EVIDENCE_KEY_LEN,
                        long_value, sizeof(long_value),
                        &replaced) == CONFIG_KV_OK,
          "invalid-record fixture stores long evidence value");
    poison_evidence(&loaded);
    memcpy(&original, &loaded, sizeof(loaded));
    CHECK(boot_evidence_load(&loaded, &kv) ==
              BOOT_EVIDENCE_INVALID_RECORD &&
              evidence_unchanged(&loaded, &original),
          "boot evidence rejects over-long record");

    CHECK(boot_evidence_save(&evidence, &kv) == BOOT_EVIDENCE_OK,
          "invalid-record fixture restores valid record");
    entry = find_audit_entry(&kv, BOOT_EVIDENCE_KEY,
                             BOOT_EVIDENCE_KEY_LEN);
    if (entry != NULL) {
        entry->value[11U] = 2U;
    }
    poison_evidence(&loaded);
    memcpy(&original, &loaded, sizeof(loaded));
    CHECK(boot_evidence_load(&loaded, &kv) ==
              BOOT_EVIDENCE_INVALID_RECORD &&
              evidence_unchanged(&loaded, &original),
          "boot evidence rejects unsupported version");

    CHECK(boot_evidence_save(&evidence, &kv) == BOOT_EVIDENCE_OK,
          "invalid-record fixture restores valid version");
    entry = find_audit_entry(&kv, BOOT_EVIDENCE_KEY,
                             BOOT_EVIDENCE_KEY_LEN);
    if (entry != NULL) {
        entry->value[20U] = 1U;
    }
    poison_evidence(&loaded);
    memcpy(&original, &loaded, sizeof(loaded));
    CHECK(boot_evidence_load(&loaded, &kv) ==
              BOOT_EVIDENCE_INVALID_RECORD &&
              evidence_unchanged(&loaded, &original),
          "boot evidence rejects nonzero reserved byte");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "boot evidence invalid-record handling performs no allocation");
}

static void test_storage_failures(void)
{
    struct verification_policy policy;
    struct image_integrity_gate gate;
    struct image_policy_binding binding;
    struct config_kv_entry full_entries[1U];
    struct config_kv full_kv;
    struct boot_evidence evidence;
    uint8_t measurement[SHA256_DIGEST_SIZE];
    unsigned char filler = 0x42U;
    bool replaced = false;
    size_t count = 0U;

    make_measurement(measurement);
    CHECK(init_policy(&policy, 1U),
          "storage fixture initializes active policy");
    CHECK(make_gate(&gate, 7U),
          "storage fixture publishes verified image gate");
    memset(&binding, 0, sizeof(binding));
    CHECK(image_policy_bind(&binding, &policy, &gate) ==
              IMAGE_POLICY_OK &&
              boot_evidence_init(&evidence, &policy, &binding,
                                 measurement) == BOOT_EVIDENCE_OK,
          "storage fixture initializes evidence");
    init_audit_kv(&full_kv, full_entries, 1U);
    CHECK(config_kv_put(&full_kv, CONFIG_KV_KIND_AUDIT,
                        "other", 5U, &filler, 1U, &replaced) ==
              CONFIG_KV_OK,
          "storage fixture fills audit checkpoint");
    CHECK(boot_evidence_save(&evidence, &full_kv) ==
              BOOT_EVIDENCE_STORAGE_FAILED &&
              config_kv_count(&full_kv, &count) == CONFIG_KV_OK &&
              count == 1U,
          "full audit checkpoint rejects save without partial publication");
    poison_evidence(&evidence);
    CHECK(boot_evidence_load(&evidence, &full_kv) ==
              BOOT_EVIDENCE_NOT_FOUND,
          "full audit checkpoint reports missing boot evidence");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "boot evidence storage failures perform no allocation");
}

int main(void)
{
    test_status_strings();
    test_null_and_unpublished();
    test_reboot_continuity_and_policy_advance();
    test_digest_mismatch_evidence();
    test_invalid_records();
    test_storage_failures();

    if (failures != 0) {
        fprintf(stderr, "boot evidence host tests: %d failures\n",
                failures);
        return 1;
    }
    printf("boot evidence host tests: OK\n");
    return 0;
}

#endif
