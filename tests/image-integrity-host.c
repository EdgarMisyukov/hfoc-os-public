#include "image_integrity.h"
#include "system_image.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifdef HFOC_IMAGE_INTEGRITY_HOST_TEST

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
static unsigned int mount_publications;
static unsigned int descriptor_publications;
static unsigned int process_publications;

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

static void poison_gate(struct image_integrity_gate *gate) {
    memset(gate, 0xA5, sizeof(*gate));
    gate->published = false;
}

static bool gate_unchanged(const struct image_integrity_gate *gate) {
    const unsigned char *bytes = (const unsigned char *)gate;
    const size_t data_size =
        offsetof(struct image_integrity_gate, published);
    size_t index;

    for (index = 0U; index < data_size; ++index) {
        if (bytes[index] != 0xA5U) {
            return false;
        }
    }
    return !gate->published;
}

static struct system_image_manifest make_manifest(void) {
    struct system_image_manifest manifest;

    memset(&manifest, 0, sizeof(manifest));
    manifest.manifest_version = SYSTEM_IMAGE_MANIFEST_VERSION;
    manifest.image_id = 7U;
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

static void publish_verified_gate(const struct image_integrity_gate *gate,
                                  uint32_t expected_image_id)
{
    if (image_integrity_is_published(gate) &&
        gate->manifest.image_id == expected_image_id) {
        ++mount_publications;
        ++descriptor_publications;
        ++process_publications;
    }
}

static void reset_publication_counters(void) {
    mount_publications = 0U;
    descriptor_publications = 0U;
    process_publications = 0U;
}

static void test_validation_and_status_strings(void) {
    struct image_integrity_gate gate;
    struct system_image_manifest manifest = make_manifest();
    struct image_integrity_payload payloads[2];
    enum image_integrity_status status;

    make_payloads(payloads);
    poison_gate(&gate);

    CHECK(image_integrity_verify(NULL, &manifest, payloads, 2U) ==
              IMAGE_INTEGRITY_NULL,
          "image integrity verify rejects null gate");
    CHECK(gate_unchanged(&gate),
          "null-gate rejection leaves caller gate unchanged");
    CHECK(image_integrity_verify(&gate, NULL, payloads, 2U) ==
              IMAGE_INTEGRITY_NULL,
          "image integrity verify rejects null manifest");
    CHECK(gate_unchanged(&gate),
          "null-manifest rejection leaves caller gate unchanged");
    CHECK(image_integrity_verify(&gate, &manifest, NULL, 2U) ==
              IMAGE_INTEGRITY_NULL,
          "image integrity verify rejects null payload array");
    CHECK(gate_unchanged(&gate),
          "null-payload rejection leaves caller gate unchanged");
    CHECK(image_integrity_verify(&gate, &manifest, payloads, 0U) ==
              IMAGE_INTEGRITY_MISSING_COMPONENT,
          "empty payload list is rejected as missing components");
    CHECK(gate_unchanged(&gate),
          "empty-payload rejection leaves caller gate unchanged");
    CHECK(image_integrity_verify(&gate, &manifest, payloads,
                                 IMAGE_INTEGRITY_PAYLOAD_MAX + 1U) ==
              IMAGE_INTEGRITY_TOO_MANY_PAYLOADS,
          "payload count above capacity is rejected");
    CHECK(gate_unchanged(&gate),
          "too-many-payload rejection leaves caller gate unchanged");
    CHECK(image_integrity_is_published(NULL) == false,
          "published query rejects null gate");

    for (status = IMAGE_INTEGRITY_OK;
         status <= IMAGE_INTEGRITY_BAD_PAYLOAD; ++status) {
        const char *name = image_integrity_status_string(status);

        CHECK(name != NULL &&
                  (status == IMAGE_INTEGRITY_OK ?
                       strcmp(name, "IMAGE_INTEGRITY_OK") == 0 :
                       status == IMAGE_INTEGRITY_NULL ?
                           strcmp(name, "IMAGE_INTEGRITY_NULL") == 0 :
                           status == IMAGE_INTEGRITY_BAD_STATE ?
                               strcmp(name, "IMAGE_INTEGRITY_BAD_STATE") == 0 :
                               status == IMAGE_INTEGRITY_TOO_MANY_PAYLOADS ?
                                   strcmp(name,
                                          "IMAGE_INTEGRITY_TOO_MANY_PAYLOADS") == 0 :
                                   status == IMAGE_INTEGRITY_BAD_MANIFEST ?
                                       strcmp(name,
                                              "IMAGE_INTEGRITY_BAD_MANIFEST") == 0 :
                                       status == IMAGE_INTEGRITY_DUPLICATE_PAYLOAD ?
                                           strcmp(name,
                                                  "IMAGE_INTEGRITY_DUPLICATE_PAYLOAD") == 0 :
                                           status == IMAGE_INTEGRITY_MISSING_COMPONENT ?
                                               strcmp(name,
                                                      "IMAGE_INTEGRITY_MISSING_COMPONENT") == 0 :
                                               status == IMAGE_INTEGRITY_UNKNOWN_COMPONENT ?
                                                   strcmp(name,
                                                          "IMAGE_INTEGRITY_UNKNOWN_COMPONENT") == 0 :
                                                   status == IMAGE_INTEGRITY_PAYLOAD_TOO_LONG ?
                                                       strcmp(name,
                                                              "IMAGE_INTEGRITY_PAYLOAD_TOO_LONG") == 0 :
                                                       status == IMAGE_INTEGRITY_HASH_MISMATCH ?
                                                           strcmp(name,
                                                                  "IMAGE_INTEGRITY_HASH_MISMATCH") == 0 :
                                                           strcmp(name,
                                                                  "IMAGE_INTEGRITY_BAD_PAYLOAD") == 0),
              "image integrity status string is exact for every defined status");
    }
    CHECK(image_integrity_status_string(
              (enum image_integrity_status)99U) != NULL,
          "image integrity status string handles undefined status");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "image integrity validation performs no allocation");
}

static void test_malformed_manifest_never_publishes(void) {
    struct image_integrity_gate gate;
    struct image_integrity_payload payloads[2];
    struct system_image_manifest manifest = make_manifest();
    size_t index;

    make_payloads(payloads);

    manifest.manifest_version = 2U;
    poison_gate(&gate);
    CHECK(image_integrity_verify(&gate, &manifest, payloads, 2U) ==
              IMAGE_INTEGRITY_BAD_MANIFEST &&
              gate_unchanged(&gate),
          "wrong manifest version is rejected without gate publication");

    manifest = make_manifest();
    manifest.image_id = 0U;
    poison_gate(&gate);
    CHECK(image_integrity_verify(&gate, &manifest, payloads, 2U) ==
              IMAGE_INTEGRITY_BAD_MANIFEST &&
              gate_unchanged(&gate),
          "zero image id is rejected without gate publication");

    manifest = make_manifest();
    strcpy(manifest.arch, "arm64");
    poison_gate(&gate);
    CHECK(image_integrity_verify(&gate, &manifest, payloads, 2U) ==
              IMAGE_INTEGRITY_BAD_MANIFEST &&
              gate_unchanged(&gate),
          "unsupported architecture is rejected without gate publication");

    manifest = make_manifest();
    manifest.min_kernel_abi = 4U;
    manifest.max_kernel_abi = 3U;
    poison_gate(&gate);
    CHECK(image_integrity_verify(&gate, &manifest, payloads, 2U) ==
              IMAGE_INTEGRITY_BAD_MANIFEST &&
              gate_unchanged(&gate),
          "incompatible kernel ABI is rejected without gate publication");

    manifest = make_manifest();
    manifest.component_count = 0U;
    poison_gate(&gate);
    CHECK(image_integrity_verify(&gate, &manifest, payloads, 2U) ==
              IMAGE_INTEGRITY_BAD_MANIFEST &&
              gate_unchanged(&gate),
          "empty manifest component list is rejected without gate publication");

    manifest = make_manifest();
    strcpy(manifest.components[1].name, "kernel");
    poison_gate(&gate);
    CHECK(image_integrity_verify(&gate, &manifest, payloads, 2U) ==
              IMAGE_INTEGRITY_BAD_MANIFEST &&
              gate_unchanged(&gate),
          "duplicate manifest component is rejected without gate publication");

    manifest = make_manifest();
    strcpy(manifest.components[0].hash, "short");
    poison_gate(&gate);
    CHECK(image_integrity_verify(&gate, &manifest, payloads, 2U) ==
              IMAGE_INTEGRITY_BAD_MANIFEST &&
              gate_unchanged(&gate),
          "short manifest hash is rejected without gate publication");

    for (index = 0U; index < 3U; ++index) {
        CHECK(mount_publications == 0U &&
                  descriptor_publications == 0U &&
                  process_publications == 0U,
              "malformed manifest publishes no mount, descriptor, or process");
    }
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "malformed-manifest handling performs no allocation");
}

static void test_payload_validation_never_publishes(void) {
    struct image_integrity_gate gate;
    struct image_integrity_payload payloads[2];
    struct system_image_manifest manifest = make_manifest();
    char long_name[IMAGE_INTEGRITY_NAME_MAX + 2U];
    const char kernel[] = KERNEL_DATA;
    const char initramfs[] = INITRAMFS_DATA;

    memset(long_name, 'x', sizeof(long_name));
    long_name[sizeof(long_name) - 1U] = '\0';

    make_payloads(payloads);
    payloads[1].name = "kernel";
    poison_gate(&gate);
    CHECK(image_integrity_verify(&gate, &manifest, payloads, 2U) ==
              IMAGE_INTEGRITY_DUPLICATE_PAYLOAD &&
              gate_unchanged(&gate),
          "duplicate payload name is rejected without gate publication");

    make_payloads(payloads);
    payloads[1].name = "unknown";
    poison_gate(&gate);
    CHECK(image_integrity_verify(&gate, &manifest, payloads, 2U) ==
              IMAGE_INTEGRITY_UNKNOWN_COMPONENT &&
              gate_unchanged(&gate),
          "unknown payload name is rejected without gate publication");

    payloads[0].name = "kernel";
    payloads[0].data = kernel;
    payloads[0].data_length = sizeof(kernel) - 1U;
    payloads[1].name = "initramfs";
    payloads[1].data = initramfs;
    payloads[1].data_length = sizeof(initramfs) - 1U;
    manifest.component_count = 1U;
    poison_gate(&gate);
    CHECK(image_integrity_verify(&gate, &manifest, payloads, 2U) ==
              IMAGE_INTEGRITY_UNKNOWN_COMPONENT &&
              gate_unchanged(&gate),
          "payload for absent manifest component is rejected");

    manifest = make_manifest();
    payloads[1].name = "initramfs";
    payloads[1].data = initramfs;
    payloads[1].data_length = sizeof(initramfs) - 1U;
    poison_gate(&gate);
    CHECK(image_integrity_verify(&gate, &manifest, payloads, 1U) ==
              IMAGE_INTEGRITY_MISSING_COMPONENT &&
              gate_unchanged(&gate),
          "missing manifest component is rejected without gate publication");

    make_payloads(payloads);
    payloads[0].name = long_name;
    poison_gate(&gate);
    CHECK(image_integrity_verify(&gate, &manifest, payloads, 2U) ==
              IMAGE_INTEGRITY_BAD_PAYLOAD &&
              gate_unchanged(&gate),
          "over-long payload name is rejected without gate publication");

    make_payloads(payloads);
    payloads[0].data = NULL;
    poison_gate(&gate);
    CHECK(image_integrity_verify(&gate, &manifest, payloads, 2U) ==
              IMAGE_INTEGRITY_BAD_PAYLOAD &&
              gate_unchanged(&gate),
          "null payload data is rejected without gate publication");

    make_payloads(payloads);
    payloads[1].data_length = IMAGE_INTEGRITY_DATA_MAX + 1U;
    poison_gate(&gate);
    CHECK(image_integrity_verify(&gate, &manifest, payloads, 2U) ==
              IMAGE_INTEGRITY_PAYLOAD_TOO_LONG &&
              gate_unchanged(&gate),
          "payload above data bound is rejected without gate publication");

    CHECK(mount_publications == 0U &&
              descriptor_publications == 0U &&
              process_publications == 0U,
          "invalid payload publishes no mount, descriptor, or process");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "invalid-payload handling performs no allocation");
}

static void test_hash_mismatch_and_publication(void) {
    struct image_integrity_gate gate;
    struct image_integrity_gate poison_copy;
    struct image_integrity_payload payloads[2];
    struct system_image_manifest manifest = make_manifest();
    const char tampered[] = TAMPERED_DATA;

    make_payloads(payloads);
    payloads[1].data = tampered;
    payloads[1].data_length = sizeof(tampered) - 1U;
    poison_gate(&gate);
    memcpy(&poison_copy, &gate, sizeof(gate));
    reset_publication_counters();

    CHECK(image_integrity_verify(&gate, &manifest, payloads, 2U) ==
              IMAGE_INTEGRITY_HASH_MISMATCH,
          "hash mismatch is rejected");
    CHECK(memcmp(&gate, &poison_copy, sizeof(gate)) == 0,
          "hash mismatch leaves gate byte-for-byte unchanged");
    publish_verified_gate(&gate, manifest.image_id);
    CHECK(mount_publications == 0U &&
              descriptor_publications == 0U &&
              process_publications == 0U,
          "hash mismatch publishes no mount, descriptor, or process");

    make_payloads(payloads);
    poison_gate(&gate);
    reset_publication_counters();
    CHECK(image_integrity_verify(&gate, &manifest, payloads, 2U) ==
              IMAGE_INTEGRITY_OK,
          "valid manifest and payloads publish a verified gate");
    CHECK(image_integrity_is_published(&gate) &&
              gate.manifest.manifest_version == manifest.manifest_version &&
              gate.manifest.image_id == manifest.image_id &&
              gate.manifest.component_count == manifest.component_count &&
              strcmp(gate.manifest.components[0].name, "kernel") == 0 &&
              strcmp(gate.manifest.components[1].name, "initramfs") == 0,
          "published gate contains the exact verified manifest");
    publish_verified_gate(&gate, manifest.image_id);
    CHECK(mount_publications == 1U &&
              descriptor_publications == 1U &&
              process_publications == 1U,
          "verified gate permits exact mount, descriptor, and process publication");

    poison_copy = gate;
    CHECK(image_integrity_verify(&gate, &manifest, payloads, 2U) ==
              IMAGE_INTEGRITY_BAD_STATE,
          "second verify on published gate is rejected");
    CHECK(memcmp(&gate, &poison_copy, sizeof(gate)) == 0,
          "published-gate rejection leaves verified state unchanged");
    CHECK(image_integrity_is_published(&gate),
          "published gate remains published after rejected second verify");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "hash and publication handling performs no allocation");
}

int main(void) {
    reset_publication_counters();
    test_validation_and_status_strings();
    test_malformed_manifest_never_publishes();
    test_payload_validation_never_publishes();
    test_hash_mismatch_and_publication();

    if (failures != 0) {
        fprintf(stderr, "Image integrity host tests: %d FAILURES\n", failures);
        return 1;
    }
    puts("Image integrity host tests: ALL OK");
    return 0;
}

#endif
