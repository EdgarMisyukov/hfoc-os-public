#include "image_policy.h"

#include "key_use.h"
#include "sha256.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifdef HFOC_IMAGE_POLICY_HOST_TEST

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

static void poison_binding(struct image_policy_binding *binding)
{
    memset(binding, 0xA5, sizeof(*binding));
    binding->published = false;
}

static bool binding_unchanged(
    const struct image_policy_binding *binding,
    const struct image_policy_binding *original)
{
    return memcmp(binding, original, sizeof(*binding)) == 0;
}

static void test_status_strings(void)
{
    static const struct {
        enum image_policy_status status;
        const char *string;
    } statuses[] = {
        {IMAGE_POLICY_OK, "IMAGE_POLICY_OK"},
        {IMAGE_POLICY_NULL, "IMAGE_POLICY_NULL"},
        {IMAGE_POLICY_BAD_STATE, "IMAGE_POLICY_BAD_STATE"},
        {IMAGE_POLICY_NO_POLICY, "IMAGE_POLICY_NO_POLICY"},
        {IMAGE_POLICY_NO_IMAGE, "IMAGE_POLICY_NO_IMAGE"},
        {IMAGE_POLICY_INVALID_IMAGE, "IMAGE_POLICY_INVALID_IMAGE"},
        {IMAGE_POLICY_NOT_PUBLISHED, "IMAGE_POLICY_NOT_PUBLISHED"},
        {IMAGE_POLICY_POLICY_MISMATCH,
         "IMAGE_POLICY_POLICY_MISMATCH"},
    };
    size_t index;

    for (index = 0U;
         index < sizeof(statuses) / sizeof(statuses[0]);
         ++index) {
        const char *string = image_policy_status_string(
            statuses[index].status);

        CHECK(string != NULL &&
                  strcmp(string, statuses[index].string) == 0,
              "image policy status string is exact");
    }
    CHECK(image_policy_status_string(
              (enum image_policy_status)8U) == NULL,
          "image policy undefined status is rejected");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "image policy status handling performs no allocation");
}

static void test_null_and_no_image(void)
{
    struct verification_policy policy;
    struct image_integrity_gate gate;
    struct image_integrity_gate unpublished_gate;
    struct image_integrity_gate invalid_gate;
    struct image_policy_binding binding;
    struct image_policy_binding original;
    struct system_image_manifest manifest_out;
    struct system_image_manifest manifest_before;

    memset(&policy, 0, sizeof(policy));
    poison_binding(&binding);
    memcpy(&original, &binding, sizeof(binding));
    make_gate(&gate, 7U);
    memset(&unpublished_gate, 0, sizeof(unpublished_gate));
    memset(&invalid_gate, 0, sizeof(invalid_gate));
    invalid_gate.published = true;
    memset(&manifest_out, 0x5A, sizeof(manifest_out));
    memcpy(&manifest_before, &manifest_out, sizeof(manifest_out));

    CHECK(image_policy_bind(NULL, &policy, &gate) ==
              IMAGE_POLICY_NULL,
          "image policy bind rejects null binding");
    CHECK(image_policy_bind(&binding, NULL, &gate) ==
              IMAGE_POLICY_NULL,
          "image policy bind rejects null policy");
    CHECK(image_policy_bind(&binding, &policy, NULL) ==
              IMAGE_POLICY_NULL,
          "image policy bind rejects null gate");
    CHECK(image_policy_activate(NULL, &policy, &manifest_out) ==
              IMAGE_POLICY_NULL,
          "image policy activate rejects null binding");
    CHECK(image_policy_activate(&binding, NULL, &manifest_out) ==
              IMAGE_POLICY_NULL,
          "image policy activate rejects null policy");
    CHECK(image_policy_activate(&binding, &policy, NULL) ==
              IMAGE_POLICY_NULL,
          "image policy activate rejects null manifest output");
    CHECK(image_policy_reset(NULL) == IMAGE_POLICY_NULL,
          "image policy reset rejects null binding");
    CHECK(!image_policy_is_published(NULL),
          "image policy published query rejects null binding");
    CHECK(image_policy_manifest(NULL) == NULL,
          "image policy manifest query rejects null binding");

    CHECK(image_policy_bind(&binding, &policy, &gate) ==
              IMAGE_POLICY_NO_POLICY &&
              binding_unchanged(&binding, &original),
          "image policy bind with unpublished policy fails closed");
    CHECK(init_policy(&policy, 1U),
          "no-image fixture initializes active policy");
    poison_binding(&binding);
    memcpy(&original, &binding, sizeof(binding));
    CHECK(image_policy_bind(&binding, &policy, NULL) ==
              IMAGE_POLICY_NULL,
          "image policy bind rejects null gate without writing");
    CHECK(image_policy_bind(&binding, &policy, &unpublished_gate) ==
              IMAGE_POLICY_NO_IMAGE &&
              binding_unchanged(&binding, &original),
          "image policy bind rejects unpublished gate without writing");
    invalid_gate.published = true;
    CHECK(image_policy_bind(&binding, &policy, &invalid_gate) ==
              IMAGE_POLICY_INVALID_IMAGE &&
              binding_unchanged(&binding, &original),
          "image policy bind rejects invalid published gate");
    CHECK(image_policy_activate(&binding, &policy, &manifest_out) ==
              IMAGE_POLICY_NOT_PUBLISHED &&
              memcmp(&manifest_out, &manifest_before,
                     sizeof(manifest_out)) == 0,
          "image policy activate rejects unpublished binding");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "image policy null and no-image handling performs no allocation");
}

static void test_bind_activate_reset(void)
{
    struct verification_policy policy;
    struct image_integrity_gate gate;
    struct image_policy_binding binding;
    struct image_policy_binding original;
    struct system_image_manifest manifest_out;

    CHECK(init_policy(&policy, 1U),
          "image policy fixture initializes active policy");
    CHECK(make_gate(&gate, 7U),
          "image policy fixture publishes verified image gate");
    poison_binding(&binding);
    memcpy(&original, &binding, sizeof(binding));

    CHECK(image_policy_bind(&binding, &policy, &gate) ==
              IMAGE_POLICY_OK &&
              binding.version == IMAGE_POLICY_VERSION &&
              binding.policy_generation == 1U &&
              image_policy_is_published(&binding) &&
              memcmp(&binding.manifest, &gate.manifest,
                     sizeof(binding.manifest)) == 0,
          "image policy bind publishes exact manifest and generation");
    CHECK(image_policy_manifest(&binding) != NULL &&
              memcmp(image_policy_manifest(&binding), &gate.manifest,
                     sizeof(gate.manifest)) == 0,
          "image policy manifest returns exact bound manifest");
    memcpy(&original, &binding, sizeof(binding));
    CHECK(image_policy_bind(&binding, &policy, &gate) ==
              IMAGE_POLICY_BAD_STATE &&
              binding_unchanged(&binding, &original),
          "image policy bind rejects already-published binding");

    CHECK(image_policy_activate(&binding, &policy, &manifest_out) ==
              IMAGE_POLICY_OK &&
              memcmp(&manifest_out, &gate.manifest,
                     sizeof(manifest_out)) == 0,
          "image policy activate publishes exact manifest under matching policy");
    CHECK(image_policy_reset(&binding) == IMAGE_POLICY_OK &&
              !image_policy_is_published(&binding) &&
              image_policy_manifest(&binding) == NULL,
          "image policy reset clears published binding");
    CHECK(image_policy_reset(&binding) == IMAGE_POLICY_BAD_STATE,
          "image policy reset rejects unpublished binding");
    CHECK(image_policy_bind(&binding, &policy, &gate) ==
              IMAGE_POLICY_OK,
          "image policy rebind succeeds after reset");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "image policy bind and activate perform no allocation");
}

static void test_inactive_generation_denial(void)
{
    struct verification_policy policy;
    struct verification_policy_snapshot next;
    struct image_integrity_gate gate;
    struct image_policy_binding binding;
    struct image_policy_binding stale;
    struct system_image_manifest manifest_out;
    struct system_image_manifest manifest_before;

    CHECK(init_policy(&policy, 1U),
          "mismatch fixture initializes active policy generation one");
    CHECK(make_gate(&gate, 7U),
          "mismatch fixture publishes verified image gate");
    poison_binding(&binding);
    CHECK(image_policy_bind(&binding, &policy, &gate) ==
              IMAGE_POLICY_OK,
          "mismatch fixture binds manifest under generation one");

    next = make_policy_snapshot(2U);
    CHECK(verification_policy_stage(&policy, &next) ==
              VERIFICATION_POLICY_OK,
          "mismatch fixture stages inactive generation two");
    memset(&manifest_out, 0x5A, sizeof(manifest_out));
    memcpy(&manifest_before, &manifest_out, sizeof(manifest_out));
    CHECK(image_policy_activate(&binding, &policy, &manifest_out) ==
              IMAGE_POLICY_OK &&
              memcmp(&manifest_out, &gate.manifest,
                     sizeof(manifest_out)) == 0,
          "pending inactive policy does not invalidate active binding");

    stale = binding;
    stale.policy_generation = 2U;
    memset(&manifest_out, 0x5A, sizeof(manifest_out));
    memcpy(&manifest_before, &manifest_out, sizeof(manifest_out));
    CHECK(image_policy_activate(&stale, &policy, &manifest_out) ==
              IMAGE_POLICY_POLICY_MISMATCH &&
              memcmp(&manifest_out, &manifest_before,
                     sizeof(manifest_out)) == 0,
          "binding for inactive policy generation cannot activate");

    CHECK(verification_policy_commit(&policy) ==
              VERIFICATION_POLICY_OK,
          "mismatch fixture commits generation two");
    memset(&manifest_out, 0x5A, sizeof(manifest_out));
    memcpy(&manifest_before, &manifest_out, sizeof(manifest_out));
    CHECK(image_policy_activate(&binding, &policy, &manifest_out) ==
              IMAGE_POLICY_POLICY_MISMATCH &&
              memcmp(&manifest_out, &manifest_before,
                     sizeof(manifest_out)) == 0,
          "old binding cannot activate after policy generation advances");

    CHECK(image_policy_reset(&binding) == IMAGE_POLICY_OK &&
              image_policy_bind(&binding, &policy, &gate) ==
                  IMAGE_POLICY_OK &&
              binding.policy_generation == 2U &&
              image_policy_activate(&binding, &policy, &manifest_out) ==
                  IMAGE_POLICY_OK &&
              memcmp(&manifest_out, &gate.manifest,
                     sizeof(manifest_out)) == 0,
          "fresh binding under new active policy generation activates");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "image policy mismatch handling performs no allocation");
}

int main(void)
{
    test_status_strings();
    test_null_and_no_image();
    test_bind_activate_reset();
    test_inactive_generation_denial();

    if (failures != 0) {
        fprintf(stderr, "image policy host tests: %d failures\n",
                failures);
        return 1;
    }
    printf("image policy host tests: OK\n");
    return 0;
}

#endif
