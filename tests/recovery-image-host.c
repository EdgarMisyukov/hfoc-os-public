#include "boot_policy.h"
#include "image_integrity.h"
#include "recovery_image.h"
#include "system_image.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifdef HFOC_RECOVERY_IMAGE_HOST_TEST

#define KERNEL_DATA "kernel-image"
#define INITRAMFS_DATA "initramfs-image"
#define TAMPERED_DATA "tampered-image"

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

static struct system_image_manifest make_manifest(uint32_t image_id) {
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

static void make_payloads(struct image_integrity_payload payloads[2]) {
    static const char kernel[] = KERNEL_DATA;
    static const char initramfs[] = INITRAMFS_DATA;

    payloads[0].name = "kernel";
    payloads[0].data = kernel;
    payloads[0].data_length = sizeof(kernel) - 1U;
    payloads[1].name = "initramfs";
    payloads[1].data = initramfs;
    payloads[1].data_length = sizeof(initramfs) - 1U;
}

static struct recovery_image_candidate make_candidate(void)
{
    struct recovery_image_candidate candidate;
    static struct image_integrity_payload payloads[2];

    make_payloads(payloads);
    candidate.payloads = payloads;
    candidate.payload_count = 2U;
    candidate.manifest = NULL;
    return candidate;
}

static struct boot_policy_record make_policy_record(void) {
    struct boot_policy_record record;

    record.image_ids[BOOT_POLICY_MODE_NORMAL] = 7U;
    record.image_ids[BOOT_POLICY_MODE_RECOVERY] = 9U;
    record.image_ids[BOOT_POLICY_MODE_LAST_KNOWN_GOOD] = 9U;
    return record;
}

static bool policy_publish(struct boot_policy *policy) {
    struct boot_policy_record record = make_policy_record();

    return boot_policy_init(policy, &record) == BOOT_POLICY_OK;
}

static void poison_selection(struct recovery_image_selection *selection) {
    memset(selection, 0xA5, sizeof(*selection));
    selection->published = false;
}

static bool selection_unchanged(
    const struct recovery_image_selection *selection)
{
    const unsigned char *bytes = (const unsigned char *)selection;
    const size_t data_size =
        offsetof(struct recovery_image_selection, published);
    size_t index;

    for (index = 0U; index < data_size; ++index) {
        if (bytes[index] != 0xA5U) {
            return false;
        }
    }
    return !selection->published;
}

static void test_validation_and_status_strings(void) {
    struct recovery_image_selection selection;
    struct boot_policy policy;
    struct recovery_image_candidate normal;
    struct recovery_image_candidate recovery;
    struct image_integrity_payload payloads[2];
    struct system_image_manifest normal_manifest;
    struct system_image_manifest recovery_manifest;
    enum recovery_image_status status;

    memset(&policy, 0, sizeof(policy));
    normal = make_candidate();
    normal.manifest = NULL;
    recovery = make_candidate();
    recovery.manifest = NULL;
    poison_selection(&selection);

    CHECK(recovery_image_select(NULL, &policy, &normal, &recovery) ==
              RECOVERY_IMAGE_NULL,
          "recovery select rejects null selection");
    CHECK(recovery_image_select(&selection, NULL, &normal, &recovery) ==
              RECOVERY_IMAGE_NULL,
          "recovery select rejects null policy");
    CHECK(recovery_image_select(&selection, &policy, NULL, &recovery) ==
              RECOVERY_IMAGE_NULL,
          "recovery select rejects null normal candidate");
    CHECK(recovery_image_select(&selection, &policy, &normal, NULL) ==
              RECOVERY_IMAGE_NULL,
          "recovery select rejects null recovery candidate");
    CHECK(recovery_image_is_published(NULL) == false,
          "recovery published query rejects null selection");
    CHECK(recovery_image_manifest(NULL) == NULL,
          "recovery manifest query rejects null selection");

    for (status = RECOVERY_IMAGE_OK;
         status <= RECOVERY_IMAGE_NO_IMAGE; ++status) {
        const char *name = recovery_image_status_string(status);

        CHECK(name != NULL &&
                  (status == RECOVERY_IMAGE_OK ?
                       strcmp(name, "RECOVERY_IMAGE_OK") == 0 :
                       status == RECOVERY_IMAGE_NULL ?
                           strcmp(name, "RECOVERY_IMAGE_NULL") == 0 :
                           status == RECOVERY_IMAGE_BAD_STATE ?
                               strcmp(name, "RECOVERY_IMAGE_BAD_STATE") == 0 :
                               status == RECOVERY_IMAGE_POLICY_UNAVAILABLE ?
                                   strcmp(name,
                                          "RECOVERY_IMAGE_POLICY_UNAVAILABLE") == 0 :
                                   strcmp(name, "RECOVERY_IMAGE_NO_IMAGE") == 0),
              "recovery status string is exact for every defined status");
    }
    CHECK(recovery_image_status_string(
              (enum recovery_image_status)99U) != NULL,
          "recovery status string handles undefined status");

    normal_manifest = make_manifest(7U);
    normal.manifest = &normal_manifest;
    recovery_manifest = make_manifest(9U);
    recovery.manifest = &recovery_manifest;
    normal.payloads = NULL;
    normal.payload_count = 2U;
    make_payloads(payloads);
    recovery.payloads = payloads;
    recovery.payload_count = 2U;
    poison_selection(&selection);
    CHECK(recovery_image_select(&selection, &policy, &normal, &recovery) ==
              RECOVERY_IMAGE_NULL &&
              selection_unchanged(&selection),
          "normal candidate null payload array is rejected");

    normal.manifest = NULL;
    normal.payloads = payloads;
    normal.payload_count = 2U;
    recovery.manifest = &recovery_manifest;
    poison_selection(&selection);
    CHECK(recovery_image_select(&selection, &policy, &normal, &recovery) ==
              RECOVERY_IMAGE_NULL &&
              selection_unchanged(&selection),
          "normal candidate null manifest is rejected");

    normal.manifest = &normal_manifest;
    poison_selection(&selection);
    CHECK(recovery_image_select(&selection, &policy, &normal, &recovery) ==
              RECOVERY_IMAGE_POLICY_UNAVAILABLE &&
              selection_unchanged(&selection),
          "unpublished policy with valid candidates is unavailable");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "recovery validation performs no allocation");
}

static void test_policy_failure_and_bad_state(void) {
    struct recovery_image_selection selection;
    struct boot_policy policy;
    struct recovery_image_candidate normal;
    struct recovery_image_candidate recovery;
    struct image_integrity_payload payloads[2];
    struct system_image_manifest normal_manifest;
    struct system_image_manifest recovery_manifest;

    make_payloads(payloads);
    normal_manifest = make_manifest(7U);
    normal.manifest = &normal_manifest;
    normal.payloads = payloads;
    normal.payload_count = 2U;
    recovery_manifest = make_manifest(9U);
    recovery.manifest = &recovery_manifest;
    recovery.payloads = payloads;
    recovery.payload_count = 2U;

    memset(&policy, 0, sizeof(policy));
    poison_selection(&selection);
    CHECK(recovery_image_select(&selection, &policy, &normal, &recovery) ==
              RECOVERY_IMAGE_POLICY_UNAVAILABLE &&
              selection_unchanged(&selection),
          "unpublished policy leaves selection unchanged");

    memset(&policy, 0, sizeof(policy));
    policy.published = true;
    poison_selection(&selection);
    CHECK(recovery_image_select(&selection, &policy, &normal, &recovery) ==
              RECOVERY_IMAGE_POLICY_UNAVAILABLE &&
              selection_unchanged(&selection),
          "invalid published policy leaves selection unchanged");

    memset(&policy, 0, sizeof(policy));
    CHECK(policy_publish(&policy), "policy failure fixture publishes policy");
    poison_selection(&selection);
    selection.published = true;
    CHECK(recovery_image_select(&selection, &policy, &normal, &recovery) ==
              RECOVERY_IMAGE_BAD_STATE &&
              selection.published,
          "published selection rejects second selection");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "policy failure handling performs no allocation");
}

static void test_normal_selection(void) {
    struct recovery_image_selection selection;
    struct boot_policy policy;
    struct recovery_image_candidate normal;
    struct recovery_image_candidate recovery;
    struct image_integrity_payload payloads[2];
    struct system_image_manifest normal_manifest;
    struct system_image_manifest recovery_manifest;

    make_payloads(payloads);
    normal_manifest = make_manifest(7U);
    normal.manifest = &normal_manifest;
    normal.payloads = payloads;
    normal.payload_count = 2U;
    recovery_manifest = make_manifest(9U);
    recovery.manifest = &recovery_manifest;
    recovery.payloads = payloads;
    recovery.payload_count = 2U;

    memset(&policy, 0, sizeof(policy));
    CHECK(policy_publish(&policy), "normal fixture publishes policy");
    poison_selection(&selection);
    CHECK(recovery_image_select(&selection, &policy, &normal, &recovery) ==
              RECOVERY_IMAGE_OK &&
              recovery_image_is_published(&selection) &&
              selection.selected_mode == RECOVERY_IMAGE_SELECTED_NORMAL &&
              selection.selected_image_id == 7U &&
              recovery_image_manifest(&selection) != NULL &&
              recovery_image_manifest(&selection)->image_id == 7U,
          "verified normal candidate selects exact normal image");
    CHECK(recovery_image_manifest(NULL) == NULL,
          "published manifest query returns non-null only while published");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "normal selection performs no allocation");
}

static void test_normal_policy_mismatch_falls_back(void) {
    struct recovery_image_selection selection;
    struct boot_policy policy;
    struct recovery_image_candidate normal;
    struct recovery_image_candidate recovery;
    struct image_integrity_payload payloads[2];
    struct system_image_manifest normal_manifest;
    struct system_image_manifest recovery_manifest;

    make_payloads(payloads);
    normal_manifest = make_manifest(8U);
    normal.manifest = &normal_manifest;
    normal.payloads = payloads;
    normal.payload_count = 2U;
    recovery_manifest = make_manifest(9U);
    recovery.manifest = &recovery_manifest;
    recovery.payloads = payloads;
    recovery.payload_count = 2U;

    memset(&policy, 0, sizeof(policy));
    CHECK(policy_publish(&policy), "policy mismatch fixture publishes policy");
    poison_selection(&selection);
    CHECK(recovery_image_select(&selection, &policy, &normal, &recovery) ==
              RECOVERY_IMAGE_OK &&
              recovery_image_is_published(&selection) &&
              selection.selected_mode == RECOVERY_IMAGE_SELECTED_RECOVERY &&
              selection.selected_image_id == 9U &&
              recovery_image_manifest(&selection) != NULL &&
              recovery_image_manifest(&selection)->image_id == 9U,
          "normal policy mismatch selects exact recovery image");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "policy mismatch fallback performs no allocation");
}

static void test_normal_manifest_failure_falls_back(void) {
    struct recovery_image_selection selection;
    struct boot_policy policy;
    struct recovery_image_candidate normal;
    struct recovery_image_candidate recovery;
    struct image_integrity_payload payloads[2];
    struct system_image_manifest normal_manifest;
    struct system_image_manifest recovery_manifest;

    make_payloads(payloads);
    normal_manifest = make_manifest(7U);
    normal_manifest.manifest_version = 2U;
    normal.manifest = &normal_manifest;
    normal.payloads = payloads;
    normal.payload_count = 2U;
    recovery_manifest = make_manifest(9U);
    recovery.manifest = &recovery_manifest;
    recovery.payloads = payloads;
    recovery.payload_count = 2U;

    memset(&policy, 0, sizeof(policy));
    CHECK(policy_publish(&policy), "manifest fixture publishes policy");
    poison_selection(&selection);
    CHECK(recovery_image_select(&selection, &policy, &normal, &recovery) ==
              RECOVERY_IMAGE_OK &&
              selection.selected_mode == RECOVERY_IMAGE_SELECTED_RECOVERY &&
              selection.selected_image_id == 9U,
          "malformed normal manifest selects exact recovery image");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "manifest fallback performs no allocation");
}

static void test_normal_hash_mismatch_falls_back(void) {
    struct recovery_image_selection selection;
    struct boot_policy policy;
    struct recovery_image_candidate normal;
    struct recovery_image_candidate recovery;
    struct image_integrity_payload normal_payloads[2];
    struct image_integrity_payload recovery_payloads[2];
    struct system_image_manifest normal_manifest;
    struct system_image_manifest recovery_manifest;
    const char tampered[] = TAMPERED_DATA;

    make_payloads(normal_payloads);
    normal_payloads[1].data = tampered;
    normal_payloads[1].data_length = sizeof(tampered) - 1U;
    normal_manifest = make_manifest(7U);
    normal.manifest = &normal_manifest;
    normal.payloads = normal_payloads;
    normal.payload_count = 2U;
    make_payloads(recovery_payloads);
    recovery_manifest = make_manifest(9U);
    recovery.manifest = &recovery_manifest;
    recovery.payloads = recovery_payloads;
    recovery.payload_count = 2U;

    memset(&policy, 0, sizeof(policy));
    CHECK(policy_publish(&policy), "hash fixture publishes policy");
    poison_selection(&selection);
    CHECK(recovery_image_select(&selection, &policy, &normal, &recovery) ==
              RECOVERY_IMAGE_OK &&
              selection.selected_mode == RECOVERY_IMAGE_SELECTED_RECOVERY &&
              selection.selected_image_id == 9U,
          "normal hash mismatch selects exact recovery image");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "hash mismatch fallback performs no allocation");
}

static void test_recovery_failure_never_publishes(void) {
    struct recovery_image_selection selection;
    struct boot_policy policy;
    struct recovery_image_candidate normal;
    struct recovery_image_candidate recovery;
    struct image_integrity_payload normal_payloads[2];
    struct image_integrity_payload recovery_payloads[2];
    struct system_image_manifest normal_manifest;
    struct system_image_manifest recovery_manifest;
    const char tampered[] = TAMPERED_DATA;

    make_payloads(normal_payloads);
    normal_manifest = make_manifest(7U);
    normal_manifest.manifest_version = 2U;
    normal.manifest = &normal_manifest;
    normal.payloads = normal_payloads;
    normal.payload_count = 2U;
    make_payloads(recovery_payloads);
    recovery_manifest = make_manifest(10U);
    recovery.manifest = &recovery_manifest;
    recovery.payloads = recovery_payloads;
    recovery.payload_count = 2U;

    memset(&policy, 0, sizeof(policy));
    CHECK(policy_publish(&policy), "recovery policy fixture publishes policy");
    poison_selection(&selection);
    CHECK(recovery_image_select(&selection, &policy, &normal, &recovery) ==
              RECOVERY_IMAGE_NO_IMAGE &&
              selection_unchanged(&selection),
          "recovery policy mismatch never publishes an image");

    make_payloads(normal_payloads);
    normal_manifest = make_manifest(7U);
    normal_manifest.manifest_version = 2U;
    normal.manifest = &normal_manifest;
    normal.payloads = normal_payloads;
    normal.payload_count = 2U;
    make_payloads(recovery_payloads);
    recovery_manifest = make_manifest(9U);
    recovery_manifest.manifest_version = 2U;
    recovery.manifest = &recovery_manifest;
    recovery.payloads = recovery_payloads;
    recovery.payload_count = 2U;
    poison_selection(&selection);
    CHECK(recovery_image_select(&selection, &policy, &normal, &recovery) ==
              RECOVERY_IMAGE_NO_IMAGE &&
              selection_unchanged(&selection),
          "malformed recovery manifest never publishes an image");

    make_payloads(normal_payloads);
    normal_manifest = make_manifest(7U);
    normal_manifest.manifest_version = 2U;
    normal.manifest = &normal_manifest;
    normal.payloads = normal_payloads;
    normal.payload_count = 2U;
    make_payloads(recovery_payloads);
    recovery_manifest = make_manifest(9U);
    recovery_payloads[1].data = tampered;
    recovery_payloads[1].data_length = sizeof(tampered) - 1U;
    recovery.manifest = &recovery_manifest;
    recovery.payloads = recovery_payloads;
    recovery.payload_count = 2U;
    poison_selection(&selection);
    CHECK(recovery_image_select(&selection, &policy, &normal, &recovery) ==
              RECOVERY_IMAGE_NO_IMAGE &&
              selection_unchanged(&selection),
          "recovery hash mismatch never publishes an image");

    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "recovery failure handling performs no allocation");
}

int main(void) {
    test_validation_and_status_strings();
    test_policy_failure_and_bad_state();
    test_normal_selection();
    test_normal_policy_mismatch_falls_back();
    test_normal_manifest_failure_falls_back();
    test_normal_hash_mismatch_falls_back();
    test_recovery_failure_never_publishes();

    if (failures != 0) {
        fprintf(stderr, "Recovery image host tests: %d FAILURES\n", failures);
        return 1;
    }
    puts("Recovery image host tests: ALL OK");
    return 0;
}

#endif
