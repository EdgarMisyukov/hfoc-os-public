#include "virtio_rng_feature_profile.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifdef HFOC_VIRTIO_RNG_FEATURE_PROFILE_HOST_TEST

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

static struct virtio_pci_identity make_identity(
    uint16_t device_id,
    uint8_t modern)
{
    struct virtio_pci_identity identity;

    memset(&identity, 0, sizeof(identity));
    identity.version = VIRTIO_PCI_IDENTITY_VERSION;
    identity.vendor_id = VIRTIO_PCI_VENDOR_ID;
    identity.device_id = device_id;
    identity.bdf.bus = 0U;
    identity.bdf.device = 0U;
    identity.bdf.function = 0U;
    identity.modern = modern;
    return identity;
}

static struct virtio_rng_feature_bitset make_bitset(
    uint32_t low,
    uint32_t high)
{
    struct virtio_rng_feature_bitset bitset;

    bitset.low = low;
    bitset.high = high;
    return bitset;
}

static struct virtio_rng_feature_bitset make_modern_offered(void)
{
    return make_bitset(0U, VIRTIO_RNG_VERSION_1_HIGH_BIT);
}

static struct virtio_rng_feature_bitset make_legacy_offered(void)
{
    return make_bitset(0U, 0U);
}

static struct virtio_rng_topology make_topology(void)
{
    struct virtio_rng_topology topology;

    memset(&topology, 0, sizeof(topology));
    topology.version = VIRTIO_RNG_TOPOLOGY_VERSION;
    topology.request_queue_count = VIRTIO_RNG_REQUEST_QUEUE_COUNT_MAX;
    topology.buffer_writer = VIRTIO_RNG_BUFFER_WRITER_DEVICE;
    return topology;
}

static bool bitset_equal(
    const struct virtio_rng_feature_bitset *left,
    const struct virtio_rng_feature_bitset *right)
{
    return left->low == right->low && left->high == right->high;
}

static bool status_string_matches(
    enum virtio_rng_feature_profile_status status,
    const char *string)
{
    switch (status) {
    case VIRTIO_RNG_FEATURE_PROFILE_OK:
        return strcmp(string, "VIRTIO_RNG_FEATURE_PROFILE_OK") == 0;
    case VIRTIO_RNG_FEATURE_PROFILE_NULL:
        return strcmp(string, "VIRTIO_RNG_FEATURE_PROFILE_NULL") == 0;
    case VIRTIO_RNG_FEATURE_PROFILE_INVALID_VERSION:
        return strcmp(
            string,
            "VIRTIO_RNG_FEATURE_PROFILE_INVALID_VERSION") == 0;
    case VIRTIO_RNG_FEATURE_PROFILE_INVALID_GENERATION:
        return strcmp(
            string,
            "VIRTIO_RNG_FEATURE_PROFILE_INVALID_GENERATION") == 0;
    case VIRTIO_RNG_FEATURE_PROFILE_ALREADY_PUBLISHED:
        return strcmp(
            string,
            "VIRTIO_RNG_FEATURE_PROFILE_ALREADY_PUBLISHED") == 0;
    case VIRTIO_RNG_FEATURE_PROFILE_UNPUBLISHED:
        return strcmp(
            string,
            "VIRTIO_RNG_FEATURE_PROFILE_UNPUBLISHED") == 0;
    case VIRTIO_RNG_FEATURE_PROFILE_INVALID_IDENTITY:
        return strcmp(
            string,
            "VIRTIO_RNG_FEATURE_PROFILE_INVALID_IDENTITY") == 0;
    case VIRTIO_RNG_FEATURE_PROFILE_NOT_RNG_DEVICE:
        return strcmp(
            string,
            "VIRTIO_RNG_FEATURE_PROFILE_NOT_RNG_DEVICE") == 0;
    case VIRTIO_RNG_FEATURE_PROFILE_UNKNOWN_FEATURE:
        return strcmp(
            string,
            "VIRTIO_RNG_FEATURE_PROFILE_UNKNOWN_FEATURE") == 0;
    case VIRTIO_RNG_FEATURE_PROFILE_MISSING_REQUIRED_FEATURE:
        return strcmp(
            string,
            "VIRTIO_RNG_FEATURE_PROFILE_MISSING_REQUIRED_FEATURE") == 0;
    case VIRTIO_RNG_FEATURE_PROFILE_UNSUPPORTED_FEATURE:
        return strcmp(
            string,
            "VIRTIO_RNG_FEATURE_PROFILE_UNSUPPORTED_FEATURE") == 0;
    case VIRTIO_RNG_FEATURE_PROFILE_INVALID_TOPOLOGY:
        return strcmp(
            string,
            "VIRTIO_RNG_FEATURE_PROFILE_INVALID_TOPOLOGY") == 0;
    case VIRTIO_RNG_FEATURE_PROFILE_UNSUPPORTED_QUEUE_COUNT:
        return strcmp(
            string,
            "VIRTIO_RNG_FEATURE_PROFILE_UNSUPPORTED_QUEUE_COUNT") == 0;
    case VIRTIO_RNG_FEATURE_PROFILE_UNSUPPORTED_BUFFER_WRITER:
        return strcmp(
            string,
            "VIRTIO_RNG_FEATURE_PROFILE_UNSUPPORTED_BUFFER_WRITER") == 0;
    case VIRTIO_RNG_FEATURE_PROFILE_CHANGED:
        return strcmp(
            string,
            "VIRTIO_RNG_FEATURE_PROFILE_CHANGED") == 0;
    default:
        return false;
    }
}

static void test_status_strings(void)
{
    static const enum virtio_rng_feature_profile_status statuses[] = {
        VIRTIO_RNG_FEATURE_PROFILE_OK,
        VIRTIO_RNG_FEATURE_PROFILE_NULL,
        VIRTIO_RNG_FEATURE_PROFILE_INVALID_VERSION,
        VIRTIO_RNG_FEATURE_PROFILE_INVALID_GENERATION,
        VIRTIO_RNG_FEATURE_PROFILE_ALREADY_PUBLISHED,
        VIRTIO_RNG_FEATURE_PROFILE_UNPUBLISHED,
        VIRTIO_RNG_FEATURE_PROFILE_INVALID_IDENTITY,
        VIRTIO_RNG_FEATURE_PROFILE_NOT_RNG_DEVICE,
        VIRTIO_RNG_FEATURE_PROFILE_UNKNOWN_FEATURE,
        VIRTIO_RNG_FEATURE_PROFILE_MISSING_REQUIRED_FEATURE,
        VIRTIO_RNG_FEATURE_PROFILE_UNSUPPORTED_FEATURE,
        VIRTIO_RNG_FEATURE_PROFILE_INVALID_TOPOLOGY,
        VIRTIO_RNG_FEATURE_PROFILE_UNSUPPORTED_QUEUE_COUNT,
        VIRTIO_RNG_FEATURE_PROFILE_UNSUPPORTED_BUFFER_WRITER,
        VIRTIO_RNG_FEATURE_PROFILE_CHANGED,
    };
    size_t index;

    for (index = 0U;
         index < sizeof(statuses) / sizeof(statuses[0]);
         ++index) {
        enum virtio_rng_feature_profile_status status = statuses[index];
        const char *string =
            virtio_rng_feature_profile_status_string(status);

        CHECK(string != NULL && status_string_matches(status, string),
              "virtio rng feature profile status string is exact");
    }
    CHECK(virtio_rng_feature_profile_status_string(
              (enum virtio_rng_feature_profile_status)15U) == NULL,
          "virtio rng feature profile undefined status is rejected");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "virtio rng feature profile status handling performs no allocation");
}

static void test_null_and_identity(void)
{
    struct virtio_rng_feature_profile profile;
    struct virtio_pci_identity identity;
    struct virtio_rng_feature_bitset offered;
    struct virtio_rng_topology topology;

    identity = make_identity(VIRTIO_PCI_DEVICE_ID_RNG_MODERN, 1U);
    offered = make_modern_offered();
    topology = make_topology();
    memset(&profile, 0, sizeof(profile));

    CHECK(virtio_rng_feature_profile_activate(
              NULL, &identity, &offered, &topology, 1U) ==
              VIRTIO_RNG_FEATURE_PROFILE_NULL,
          "rng feature profile activate rejects null profile");
    CHECK(virtio_rng_feature_profile_activate(
              &profile, NULL, &offered, &topology, 1U) ==
              VIRTIO_RNG_FEATURE_PROFILE_NULL,
          "rng feature profile activate rejects null identity");
    CHECK(virtio_rng_feature_profile_activate(
              &profile, &identity, NULL, &topology, 1U) ==
              VIRTIO_RNG_FEATURE_PROFILE_NULL,
          "rng feature profile activate rejects null offered bitset");
    CHECK(virtio_rng_feature_profile_activate(
              &profile, &identity, &offered, NULL, 1U) ==
              VIRTIO_RNG_FEATURE_PROFILE_NULL,
          "rng feature profile activate rejects null topology");

    identity.version = VIRTIO_PCI_IDENTITY_VERSION + 1U;
    CHECK(virtio_rng_feature_profile_activate(
              &profile, &identity, &offered, &topology, 1U) ==
              VIRTIO_RNG_FEATURE_PROFILE_INVALID_IDENTITY &&
              !profile.published,
          "rng feature profile rejects invalid identity without mutation");
    identity = make_identity(VIRTIO_PCI_DEVICE_ID_NET_MODERN, 1U);
    CHECK(virtio_rng_feature_profile_activate(
              &profile, &identity, &offered, &topology, 1U) ==
              VIRTIO_RNG_FEATURE_PROFILE_NOT_RNG_DEVICE &&
              !profile.published,
          "rng feature profile rejects non-RNG device without mutation");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "virtio rng feature profile null handling performs no allocation");
}

static void test_modern_feature_policy(void)
{
    struct virtio_rng_feature_profile profile;
    struct virtio_rng_feature_profile original;
    struct virtio_pci_identity identity;
    struct virtio_rng_feature_bitset offered;
    struct virtio_rng_feature_bitset expected_accepted;
    struct virtio_rng_topology topology;
    const uint32_t ring_indirect =
        UINT32_C(1) << VIRTIO_PCI_FEATURE_RING_INDIRECT_DESC;

    identity = make_identity(VIRTIO_PCI_DEVICE_ID_RNG_MODERN, 1U);
    topology = make_topology();

    memset(&profile, 0, sizeof(profile));
    offered = make_modern_offered();
    offered.high &= ~VIRTIO_RNG_VERSION_1_HIGH_BIT;
    CHECK(virtio_rng_feature_profile_activate(
              &profile, &identity, &offered, &topology, 1U) ==
              VIRTIO_RNG_FEATURE_PROFILE_MISSING_REQUIRED_FEATURE &&
              !profile.published,
          "modern rng profile rejects missing version-1 feature");

    memset(&profile, 0, sizeof(profile));
    offered = make_modern_offered();
    offered.low |= UINT32_C(1) << 4U;
    CHECK(virtio_rng_feature_profile_activate(
              &profile, &identity, &offered, &topology, 1U) ==
              VIRTIO_RNG_FEATURE_PROFILE_UNKNOWN_FEATURE &&
              !profile.published,
          "modern rng profile rejects unknown feature bit");

    memset(&profile, 0, sizeof(profile));
    offered = make_modern_offered();
    offered.high |= UINT32_C(1) << (50U - 32U);
    CHECK(virtio_rng_feature_profile_activate(
              &profile, &identity, &offered, &topology, 1U) ==
              VIRTIO_RNG_FEATURE_PROFILE_UNKNOWN_FEATURE &&
              !profile.published,
          "modern rng profile rejects unknown high feature bit");

    memset(&profile, 0, sizeof(profile));
    offered = make_modern_offered();
    offered.low |= ring_indirect;
    expected_accepted = make_bitset(
        0U, VIRTIO_RNG_VERSION_1_HIGH_BIT);
    CHECK(virtio_rng_feature_profile_activate(
              &profile, &identity, &offered, &topology, 7U) ==
              VIRTIO_RNG_FEATURE_PROFILE_OK &&
              virtio_rng_feature_profile_published(&profile) &&
              profile.version == VIRTIO_RNG_FEATURE_PROFILE_VERSION &&
              profile.generation == 7U &&
              bitset_equal(&profile.accepted, &expected_accepted) &&
              profile.request_queue_count == 1U &&
              profile.buffer_writer == VIRTIO_RNG_BUFFER_WRITER_DEVICE,
          "valid modern rng profile publishes exact immutable policy");
    original = profile;
    CHECK(virtio_rng_feature_profile_activate(
              &profile, &identity, &offered, &topology, 8U) ==
              VIRTIO_RNG_FEATURE_PROFILE_ALREADY_PUBLISHED &&
              memcmp(&profile, &original, sizeof(profile)) == 0,
          "duplicate modern rng profile activation is rejected without mutation");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "virtio rng feature profile modern policy performs no allocation");
}

static void test_legacy_feature_policy(void)
{
    struct virtio_rng_feature_profile profile;
    struct virtio_pci_identity identity;
    struct virtio_rng_feature_bitset offered;
    struct virtio_rng_topology topology;

    identity = make_identity(VIRTIO_PCI_DEVICE_ID_RNG_TRANSITIONAL, 0U);
    topology = make_topology();
    memset(&profile, 0, sizeof(profile));
    offered = make_legacy_offered();

    CHECK(virtio_rng_feature_profile_activate(
              &profile, &identity, &offered, &topology, 1U) ==
              VIRTIO_RNG_FEATURE_PROFILE_OK &&
              profile.accepted.low == 0U &&
              profile.accepted.high == 0U,
          "valid transitional rng profile publishes no required features");

    memset(&profile, 0, sizeof(profile));
    offered = make_legacy_offered();
    offered.high |= VIRTIO_RNG_VERSION_1_HIGH_BIT;
    CHECK(virtio_rng_feature_profile_activate(
              &profile, &identity, &offered, &topology, 1U) ==
              VIRTIO_RNG_FEATURE_PROFILE_UNSUPPORTED_FEATURE &&
              !profile.published,
          "transitional rng profile rejects modern version-1 offer");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "virtio rng feature profile legacy policy performs no allocation");
}

static void test_topology_policy(void)
{
    struct virtio_rng_feature_profile profile;
    struct virtio_pci_identity identity;
    struct virtio_rng_feature_bitset offered;
    struct virtio_rng_topology topology;

    identity = make_identity(VIRTIO_PCI_DEVICE_ID_RNG_MODERN, 1U);
    offered = make_modern_offered();

    memset(&profile, 0, sizeof(profile));
    topology = make_topology();
    topology.version = 0U;
    CHECK(virtio_rng_feature_profile_activate(
              &profile, &identity, &offered, &topology, 1U) ==
              VIRTIO_RNG_FEATURE_PROFILE_INVALID_TOPOLOGY,
          "rng feature profile rejects unknown topology version");

    memset(&profile, 0, sizeof(profile));
    topology = make_topology();
    topology.request_queue_count = 0U;
    CHECK(virtio_rng_feature_profile_activate(
              &profile, &identity, &offered, &topology, 1U) ==
              VIRTIO_RNG_FEATURE_PROFILE_UNSUPPORTED_QUEUE_COUNT,
          "rng feature profile rejects zero request queues");

    memset(&profile, 0, sizeof(profile));
    topology = make_topology();
    topology.request_queue_count = 2U;
    CHECK(virtio_rng_feature_profile_activate(
              &profile, &identity, &offered, &topology, 1U) ==
              VIRTIO_RNG_FEATURE_PROFILE_UNSUPPORTED_QUEUE_COUNT,
          "rng feature profile rejects multiple request queues");

    memset(&profile, 0, sizeof(profile));
    topology = make_topology();
    topology.buffer_writer = VIRTIO_RNG_BUFFER_WRITER_DRIVER;
    CHECK(virtio_rng_feature_profile_activate(
              &profile, &identity, &offered, &topology, 1U) ==
              VIRTIO_RNG_FEATURE_PROFILE_UNSUPPORTED_BUFFER_WRITER,
          "rng feature profile rejects driver buffer-write ownership");

    memset(&profile, 0, sizeof(profile));
    topology = make_topology();
    topology.buffer_writer = VIRTIO_RNG_BUFFER_WRITER_UNKNOWN;
    CHECK(virtio_rng_feature_profile_activate(
              &profile, &identity, &offered, &topology, 1U) ==
              VIRTIO_RNG_FEATURE_PROFILE_UNSUPPORTED_BUFFER_WRITER,
          "rng feature profile rejects unknown buffer-write ownership");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "virtio rng feature profile topology performs no allocation");
}

static void test_generation_and_observed_state(void)
{
    struct virtio_rng_feature_profile profile;
    struct virtio_rng_feature_profile original;
    struct virtio_pci_identity identity;
    struct virtio_rng_feature_bitset offered;
    struct virtio_rng_topology topology;
    struct virtio_pci_identity observed_identity;
    struct virtio_rng_feature_bitset observed_offered;
    struct virtio_rng_topology observed_topology;

    identity = make_identity(VIRTIO_PCI_DEVICE_ID_RNG_MODERN, 1U);
    offered = make_modern_offered();
    topology = make_topology();
    memset(&profile, 0, sizeof(profile));

    CHECK(virtio_rng_feature_profile_activate(
              &profile, &identity, &offered, &topology, 0U) ==
              VIRTIO_RNG_FEATURE_PROFILE_INVALID_GENERATION &&
              !profile.published,
          "rng feature profile rejects zero generation");
    CHECK(virtio_rng_feature_profile_activate(
              &profile, &identity, &offered, &topology,
              VIRTIO_RNG_PROFILE_GENERATION_MAX + 1U) ==
              VIRTIO_RNG_FEATURE_PROFILE_INVALID_GENERATION &&
              !profile.published,
          "rng feature profile rejects over-limit generation");

    memset(&profile, 0, sizeof(profile));
    CHECK(virtio_rng_feature_profile_activate(
              &profile, &identity, &offered, &topology, 5U) ==
              VIRTIO_RNG_FEATURE_PROFILE_OK,
          "observed-state fixture rng profile activates");
    original = profile;

    CHECK(virtio_rng_feature_profile_validate_state(
              NULL, &identity, &offered, &topology, 5U) ==
              VIRTIO_RNG_FEATURE_PROFILE_NULL,
          "rng observed-state validation rejects null profile");
    CHECK(virtio_rng_feature_profile_validate_state(
              &profile, NULL, &offered, &topology, 5U) ==
              VIRTIO_RNG_FEATURE_PROFILE_NULL,
          "rng observed-state validation rejects null identity");
    CHECK(virtio_rng_feature_profile_validate_state(
              &profile, &identity, NULL, &topology, 5U) ==
              VIRTIO_RNG_FEATURE_PROFILE_NULL,
          "rng observed-state validation rejects null offered bitset");
    CHECK(virtio_rng_feature_profile_validate_state(
              &profile, &identity, &offered, NULL, 5U) ==
              VIRTIO_RNG_FEATURE_PROFILE_NULL,
          "rng observed-state validation rejects null topology");

    CHECK(virtio_rng_feature_profile_validate_state(
              &profile, &identity, &offered, &topology, 5U) ==
              VIRTIO_RNG_FEATURE_PROFILE_OK,
          "unchanged rng observed state validates");

    CHECK(virtio_rng_feature_profile_validate_state(
              &profile, &identity, &offered, &topology, 6U) ==
              VIRTIO_RNG_FEATURE_PROFILE_CHANGED,
          "changed rng observed generation fails closed");

    observed_identity = identity;
    observed_identity.device_id = VIRTIO_PCI_DEVICE_ID_RNG_TRANSITIONAL;
    CHECK(virtio_rng_feature_profile_validate_state(
              &profile, &observed_identity, &offered, &topology, 5U) ==
              VIRTIO_RNG_FEATURE_PROFILE_CHANGED,
          "changed rng observed identity fails closed");

    observed_offered = offered;
    observed_offered.low |= UINT32_C(1) << 4U;
    CHECK(virtio_rng_feature_profile_validate_state(
              &profile, &identity, &observed_offered, &topology, 5U) ==
              VIRTIO_RNG_FEATURE_PROFILE_CHANGED,
          "changed rng observed feature fails closed");

    observed_topology = topology;
    observed_topology.request_queue_count = 2U;
    CHECK(virtio_rng_feature_profile_validate_state(
              &profile, &identity, &offered, &observed_topology, 5U) ==
              VIRTIO_RNG_FEATURE_PROFILE_CHANGED,
          "changed rng observed topology fails closed");
    CHECK(memcmp(&profile, &original, sizeof(profile)) == 0,
          "rng observed-state validation does not mutate profile");

    memset(&profile, 0, sizeof(profile));
    CHECK(virtio_rng_feature_profile_validate_state(
              &profile, &identity, &offered, &topology, 5U) ==
              VIRTIO_RNG_FEATURE_PROFILE_UNPUBLISHED,
          "rng observed-state validation rejects unpublished profile");
    CHECK(!virtio_rng_feature_profile_published(&profile) &&
              !virtio_rng_feature_profile_published(NULL),
          "rng published accessor rejects null and unpublished profiles");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "virtio rng feature profile observed state performs no allocation");
}

int main(void)
{
    test_status_strings();
    test_null_and_identity();
    test_modern_feature_policy();
    test_legacy_feature_policy();
    test_topology_policy();
    test_generation_and_observed_state();

    if (failures != 0) {
        fprintf(stderr, "Virtio rng feature profile host tests: FAIL\n");
        return 1;
    }
    printf("Virtio rng feature profile host tests: ALL OK\n");
    return 0;
}

#else
#error "This file must be compiled with HFOC_VIRTIO_RNG_FEATURE_PROFILE_HOST_TEST"
#endif
