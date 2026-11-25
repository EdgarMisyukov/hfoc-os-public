#include "virtio_net_feature_profile.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifdef HFOC_VIRTIO_NET_FEATURE_PROFILE_HOST_TEST

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

static struct virtio_net_feature_bitset make_bitset(
    uint32_t low,
    uint32_t high)
{
    struct virtio_net_feature_bitset bitset;

    bitset.low = low;
    bitset.high = high;
    return bitset;
}

static struct virtio_net_feature_bitset make_modern_offered(void)
{
    return make_bitset(
        VIRTIO_NET_REQUIRED_FEATURE_LO_MASK,
        VIRTIO_NET_VERSION_1_HIGH_BIT);
}

static struct virtio_net_feature_bitset make_legacy_offered(void)
{
    return make_bitset(VIRTIO_NET_REQUIRED_FEATURE_LO_MASK, 0U);
}

static struct virtio_net_configuration make_configuration(void)
{
    struct virtio_net_configuration configuration;

    memset(&configuration, 0, sizeof(configuration));
    configuration.version = VIRTIO_NET_CONFIGURATION_VERSION;
    configuration.mac_address[0] = 0x52U;
    configuration.mac_address[1] = 0x54U;
    configuration.mac_address[2] = 0x00U;
    configuration.mac_address[3] = 0x12U;
    configuration.mac_address[4] = 0x34U;
    configuration.mac_address[5] = 0x56U;
    configuration.status = VIRTIO_NET_STATUS_LINK_UP;
    return configuration;
}

static struct virtio_net_topology make_topology(void)
{
    struct virtio_net_topology topology;

    memset(&topology, 0, sizeof(topology));
    topology.max_virtqueue_pairs = 1U;
    topology.receive_queue_count = 1U;
    topology.transmit_queue_count = 1U;
    topology.control_queue_present = false;
    return topology;
}

static bool status_string_matches(
    enum virtio_net_feature_profile_status status,
    const char *string)
{
    switch (status) {
    case VIRTIO_NET_FEATURE_PROFILE_OK:
        return strcmp(string, "VIRTIO_NET_FEATURE_PROFILE_OK") == 0;
    case VIRTIO_NET_FEATURE_PROFILE_NULL:
        return strcmp(string, "VIRTIO_NET_FEATURE_PROFILE_NULL") == 0;
    case VIRTIO_NET_FEATURE_PROFILE_INVALID_VERSION:
        return strcmp(
            string,
            "VIRTIO_NET_FEATURE_PROFILE_INVALID_VERSION") == 0;
    case VIRTIO_NET_FEATURE_PROFILE_INVALID_GENERATION:
        return strcmp(
            string,
            "VIRTIO_NET_FEATURE_PROFILE_INVALID_GENERATION") == 0;
    case VIRTIO_NET_FEATURE_PROFILE_ALREADY_PUBLISHED:
        return strcmp(
            string,
            "VIRTIO_NET_FEATURE_PROFILE_ALREADY_PUBLISHED") == 0;
    case VIRTIO_NET_FEATURE_PROFILE_UNPUBLISHED:
        return strcmp(
            string,
            "VIRTIO_NET_FEATURE_PROFILE_UNPUBLISHED") == 0;
    case VIRTIO_NET_FEATURE_PROFILE_INVALID_IDENTITY:
        return strcmp(
            string,
            "VIRTIO_NET_FEATURE_PROFILE_INVALID_IDENTITY") == 0;
    case VIRTIO_NET_FEATURE_PROFILE_NOT_NET_DEVICE:
        return strcmp(
            string,
            "VIRTIO_NET_FEATURE_PROFILE_NOT_NET_DEVICE") == 0;
    case VIRTIO_NET_FEATURE_PROFILE_UNKNOWN_FEATURE:
        return strcmp(
            string,
            "VIRTIO_NET_FEATURE_PROFILE_UNKNOWN_FEATURE") == 0;
    case VIRTIO_NET_FEATURE_PROFILE_MISSING_REQUIRED_FEATURE:
        return strcmp(
            string,
            "VIRTIO_NET_FEATURE_PROFILE_MISSING_REQUIRED_FEATURE") == 0;
    case VIRTIO_NET_FEATURE_PROFILE_UNSUPPORTED_FEATURE:
        return strcmp(
            string,
            "VIRTIO_NET_FEATURE_PROFILE_UNSUPPORTED_FEATURE") == 0;
    case VIRTIO_NET_FEATURE_PROFILE_INVALID_CONFIGURATION:
        return strcmp(
            string,
            "VIRTIO_NET_FEATURE_PROFILE_INVALID_CONFIGURATION") == 0;
    case VIRTIO_NET_FEATURE_PROFILE_INVALID_MAC_ADDRESS:
        return strcmp(
            string,
            "VIRTIO_NET_FEATURE_PROFILE_INVALID_MAC_ADDRESS") == 0;
    case VIRTIO_NET_FEATURE_PROFILE_INVALID_STATUS:
        return strcmp(
            string,
            "VIRTIO_NET_FEATURE_PROFILE_INVALID_STATUS") == 0;
    case VIRTIO_NET_FEATURE_PROFILE_INVALID_TOPOLOGY:
        return strcmp(
            string,
            "VIRTIO_NET_FEATURE_PROFILE_INVALID_TOPOLOGY") == 0;
    case VIRTIO_NET_FEATURE_PROFILE_MULTIQUEUE:
        return strcmp(
            string,
            "VIRTIO_NET_FEATURE_PROFILE_MULTIQUEUE") == 0;
    case VIRTIO_NET_FEATURE_PROFILE_CONTROL_QUEUE:
        return strcmp(
            string,
            "VIRTIO_NET_FEATURE_PROFILE_CONTROL_QUEUE") == 0;
    case VIRTIO_NET_FEATURE_PROFILE_CHANGED:
        return strcmp(
            string,
            "VIRTIO_NET_FEATURE_PROFILE_CHANGED") == 0;
    default:
        return false;
    }
}

static void test_status_strings(void)
{
    static const enum virtio_net_feature_profile_status statuses[] = {
        VIRTIO_NET_FEATURE_PROFILE_OK,
        VIRTIO_NET_FEATURE_PROFILE_NULL,
        VIRTIO_NET_FEATURE_PROFILE_INVALID_VERSION,
        VIRTIO_NET_FEATURE_PROFILE_INVALID_GENERATION,
        VIRTIO_NET_FEATURE_PROFILE_ALREADY_PUBLISHED,
        VIRTIO_NET_FEATURE_PROFILE_UNPUBLISHED,
        VIRTIO_NET_FEATURE_PROFILE_INVALID_IDENTITY,
        VIRTIO_NET_FEATURE_PROFILE_NOT_NET_DEVICE,
        VIRTIO_NET_FEATURE_PROFILE_UNKNOWN_FEATURE,
        VIRTIO_NET_FEATURE_PROFILE_MISSING_REQUIRED_FEATURE,
        VIRTIO_NET_FEATURE_PROFILE_UNSUPPORTED_FEATURE,
        VIRTIO_NET_FEATURE_PROFILE_INVALID_CONFIGURATION,
        VIRTIO_NET_FEATURE_PROFILE_INVALID_MAC_ADDRESS,
        VIRTIO_NET_FEATURE_PROFILE_INVALID_STATUS,
        VIRTIO_NET_FEATURE_PROFILE_INVALID_TOPOLOGY,
        VIRTIO_NET_FEATURE_PROFILE_MULTIQUEUE,
        VIRTIO_NET_FEATURE_PROFILE_CONTROL_QUEUE,
        VIRTIO_NET_FEATURE_PROFILE_CHANGED,
    };
    size_t index;

    for (index = 0U;
         index < sizeof(statuses) / sizeof(statuses[0]);
         ++index) {
        enum virtio_net_feature_profile_status status = statuses[index];
        const char *string = virtio_net_feature_profile_status_string(status);

        CHECK(string != NULL && status_string_matches(status, string),
              "virtio net feature profile status string is exact");
    }
    CHECK(virtio_net_feature_profile_status_string(
              (enum virtio_net_feature_profile_status)18U) == NULL,
          "virtio net feature profile undefined status is rejected");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "virtio net feature profile status handling performs no allocation");
}

static void test_null_and_identity(void)
{
    struct virtio_net_feature_profile profile;
    struct virtio_pci_identity identity;
    struct virtio_net_feature_bitset offered;
    struct virtio_net_configuration configuration;
    struct virtio_net_topology topology;

    identity = make_identity(VIRTIO_PCI_DEVICE_ID_NET_MODERN, 1U);
    offered = make_modern_offered();
    configuration = make_configuration();
    topology = make_topology();
    memset(&profile, 0, sizeof(profile));

    CHECK(virtio_net_feature_profile_activate(
              NULL, &identity, &offered, &configuration, &topology, 1U) ==
              VIRTIO_NET_FEATURE_PROFILE_NULL,
          "feature profile activate rejects null profile");
    CHECK(virtio_net_feature_profile_activate(
              &profile, NULL, &offered, &configuration, &topology, 1U) ==
              VIRTIO_NET_FEATURE_PROFILE_NULL,
          "feature profile activate rejects null identity");
    CHECK(virtio_net_feature_profile_activate(
              &profile, &identity, NULL, &configuration, &topology, 1U) ==
              VIRTIO_NET_FEATURE_PROFILE_NULL,
          "feature profile activate rejects null offered bitset");
    CHECK(virtio_net_feature_profile_activate(
              &profile, &identity, &offered, NULL, &topology, 1U) ==
              VIRTIO_NET_FEATURE_PROFILE_NULL,
          "feature profile activate rejects null configuration");
    CHECK(virtio_net_feature_profile_activate(
              &profile, &identity, &offered, &configuration, NULL, 1U) ==
              VIRTIO_NET_FEATURE_PROFILE_NULL,
          "feature profile activate rejects null topology");

    identity.version = VIRTIO_PCI_IDENTITY_VERSION + 1U;
    CHECK(virtio_net_feature_profile_activate(
              &profile, &identity, &offered, &configuration, &topology, 1U) ==
              VIRTIO_NET_FEATURE_PROFILE_INVALID_IDENTITY &&
              !profile.published,
          "feature profile rejects invalid identity without mutation");
    identity = make_identity(VIRTIO_PCI_DEVICE_ID_RNG_MODERN, 1U);
    CHECK(virtio_net_feature_profile_activate(
              &profile, &identity, &offered, &configuration, &topology, 1U) ==
              VIRTIO_NET_FEATURE_PROFILE_NOT_NET_DEVICE &&
              !profile.published,
          "feature profile rejects non-net device without mutation");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "virtio net feature profile null handling performs no allocation");
}

static void test_modern_feature_policy(void)
{
    struct virtio_net_feature_profile profile;
    struct virtio_pci_identity identity;
    struct virtio_net_feature_bitset offered;
    struct virtio_net_configuration configuration;
    struct virtio_net_topology topology;
    struct virtio_net_feature_profile original;

    identity = make_identity(VIRTIO_PCI_DEVICE_ID_NET_MODERN, 1U);
    configuration = make_configuration();
    topology = make_topology();

    memset(&profile, 0, sizeof(profile));
    offered = make_modern_offered();
    offered.low &= ~(UINT32_C(1) << VIRTIO_NET_F_MAC);
    CHECK(virtio_net_feature_profile_activate(
              &profile, &identity, &offered, &configuration, &topology, 1U) ==
              VIRTIO_NET_FEATURE_PROFILE_MISSING_REQUIRED_FEATURE &&
              !profile.published,
          "modern profile rejects missing MAC feature");

    memset(&profile, 0, sizeof(profile));
    offered = make_modern_offered();
    offered.low &= ~(UINT32_C(1) << VIRTIO_NET_F_STATUS);
    CHECK(virtio_net_feature_profile_activate(
              &profile, &identity, &offered, &configuration, &topology, 1U) ==
              VIRTIO_NET_FEATURE_PROFILE_MISSING_REQUIRED_FEATURE &&
              !profile.published,
          "modern profile rejects missing status feature");

    memset(&profile, 0, sizeof(profile));
    offered = make_modern_offered();
    offered.high &= ~VIRTIO_NET_VERSION_1_HIGH_BIT;
    CHECK(virtio_net_feature_profile_activate(
              &profile, &identity, &offered, &configuration, &topology, 1U) ==
              VIRTIO_NET_FEATURE_PROFILE_MISSING_REQUIRED_FEATURE &&
              !profile.published,
          "modern profile rejects missing version-1 feature");

    memset(&profile, 0, sizeof(profile));
    offered = make_modern_offered();
    offered.low |= UINT32_C(1) << 4U;
    CHECK(virtio_net_feature_profile_activate(
              &profile, &identity, &offered, &configuration, &topology, 1U) ==
              VIRTIO_NET_FEATURE_PROFILE_UNKNOWN_FEATURE &&
              !profile.published,
          "modern profile rejects unknown feature bit");

    memset(&profile, 0, sizeof(profile));
    offered = make_modern_offered();
    offered.low |= UINT32_C(1) << VIRTIO_NET_F_CSUM;
    CHECK(virtio_net_feature_profile_activate(
              &profile, &identity, &offered, &configuration, &topology, 1U) ==
              VIRTIO_NET_FEATURE_PROFILE_UNSUPPORTED_FEATURE &&
              !profile.published,
          "modern profile rejects checksum offload");

    memset(&profile, 0, sizeof(profile));
    offered = make_modern_offered();
    offered.low |= UINT32_C(1) << VIRTIO_NET_F_GUEST_TSO4;
    CHECK(virtio_net_feature_profile_activate(
              &profile, &identity, &offered, &configuration, &topology, 1U) ==
              VIRTIO_NET_FEATURE_PROFILE_UNSUPPORTED_FEATURE,
          "modern profile rejects guest TSO4 segmentation");

    memset(&profile, 0, sizeof(profile));
    offered = make_modern_offered();
    offered.low |= UINT32_C(1) << VIRTIO_NET_F_MRG_RXBUF;
    CHECK(virtio_net_feature_profile_activate(
              &profile, &identity, &offered, &configuration, &topology, 1U) ==
              VIRTIO_NET_FEATURE_PROFILE_UNSUPPORTED_FEATURE,
          "modern profile rejects mergeable receive buffers");

    memset(&profile, 0, sizeof(profile));
    offered = make_modern_offered();
    offered.low |= UINT32_C(1) << VIRTIO_NET_F_CTRL_VQ;
    CHECK(virtio_net_feature_profile_activate(
              &profile, &identity, &offered, &configuration, &topology, 1U) ==
              VIRTIO_NET_FEATURE_PROFILE_UNSUPPORTED_FEATURE,
          "modern profile rejects control queue feature");

    memset(&profile, 0, sizeof(profile));
    offered = make_modern_offered();
    offered.low |= UINT32_C(1) << VIRTIO_NET_F_MQ;
    CHECK(virtio_net_feature_profile_activate(
              &profile, &identity, &offered, &configuration, &topology, 1U) ==
              VIRTIO_NET_FEATURE_PROFILE_UNSUPPORTED_FEATURE,
          "modern profile rejects multiqueue feature");

    memset(&profile, 0, sizeof(profile));
    offered = make_modern_offered();
    offered.high |= UINT32_C(1) << (VIRTIO_NET_F_SPEED_DUPLEX - 32U);
    CHECK(virtio_net_feature_profile_activate(
              &profile, &identity, &offered, &configuration, &topology, 1U) ==
              VIRTIO_NET_FEATURE_PROFILE_UNSUPPORTED_FEATURE,
          "modern profile rejects speed/duplex feature");

    memset(&profile, 0, sizeof(profile));
    offered = make_modern_offered();
    CHECK(virtio_net_feature_profile_activate(
              &profile, &identity, &offered, &configuration, &topology, 7U) ==
              VIRTIO_NET_FEATURE_PROFILE_OK &&
              virtio_net_feature_profile_published(&profile) &&
              profile.version == VIRTIO_NET_FEATURE_PROFILE_VERSION &&
              profile.generation == 7U &&
              profile.header_mode == VIRTIO_NET_HEADER_MODE_V1 &&
              profile.header_size == VIRTIO_NET_HEADER_SIZE_V1 &&
              profile.header_flags_mask == VIRTIO_NET_HEADER_FLAGS_MASK &&
              profile.header_gso_mask == VIRTIO_NET_HEADER_GSO_MASK &&
              profile.accepted.low == VIRTIO_NET_REQUIRED_FEATURE_LO_MASK &&
              profile.accepted.high == VIRTIO_NET_VERSION_1_HIGH_BIT &&
              profile.max_virtqueue_pairs == 1U &&
              profile.receive_queue_count == 1U &&
              profile.transmit_queue_count == 1U &&
              !profile.control_queue_present,
          "valid modern profile publishes exact immutable policy");
    original = profile;
    CHECK(virtio_net_feature_profile_activate(
              &profile, &identity, &offered, &configuration, &topology, 8U) ==
              VIRTIO_NET_FEATURE_PROFILE_ALREADY_PUBLISHED &&
              memcmp(&profile, &original, sizeof(profile)) == 0,
          "duplicate modern profile activation is rejected without mutation");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "virtio net feature profile modern policy performs no allocation");
}

static void test_legacy_feature_policy(void)
{
    struct virtio_net_feature_profile profile;
    struct virtio_pci_identity identity;
    struct virtio_net_feature_bitset offered;
    struct virtio_net_configuration configuration;
    struct virtio_net_topology topology;

    identity = make_identity(VIRTIO_PCI_DEVICE_ID_NET_TRANSITIONAL, 0U);
    configuration = make_configuration();
    topology = make_topology();
    memset(&profile, 0, sizeof(profile));
    offered = make_legacy_offered();

    CHECK(virtio_net_feature_profile_activate(
              &profile, &identity, &offered, &configuration, &topology, 1U) ==
              VIRTIO_NET_FEATURE_PROFILE_OK &&
              profile.header_mode == VIRTIO_NET_HEADER_MODE_LEGACY &&
              profile.header_size == VIRTIO_NET_HEADER_SIZE_LEGACY &&
              profile.accepted.low == VIRTIO_NET_REQUIRED_FEATURE_LO_MASK &&
              profile.accepted.high == 0U,
          "valid transitional profile publishes legacy header mode");

    memset(&profile, 0, sizeof(profile));
    offered = make_legacy_offered();
    offered.high |= VIRTIO_NET_VERSION_1_HIGH_BIT;
    CHECK(virtio_net_feature_profile_activate(
              &profile, &identity, &offered, &configuration, &topology, 1U) ==
              VIRTIO_NET_FEATURE_PROFILE_UNSUPPORTED_FEATURE &&
              !profile.published,
          "transitional profile rejects modern version-1 offer");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "virtio net feature profile legacy policy performs no allocation");
}

static void test_configuration_policy(void)
{
    struct virtio_net_feature_profile profile;
    struct virtio_pci_identity identity;
    struct virtio_net_feature_bitset offered;
    struct virtio_net_configuration configuration;
    struct virtio_net_topology topology;

    identity = make_identity(VIRTIO_PCI_DEVICE_ID_NET_MODERN, 1U);
    offered = make_modern_offered();
    topology = make_topology();

    memset(&profile, 0, sizeof(profile));
    configuration = make_configuration();
    configuration.version = VIRTIO_NET_CONFIGURATION_VERSION + 1U;
    CHECK(virtio_net_feature_profile_activate(
              &profile, &identity, &offered, &configuration, &topology, 1U) ==
              VIRTIO_NET_FEATURE_PROFILE_INVALID_CONFIGURATION &&
              !profile.published,
          "feature profile rejects unknown configuration version");

    memset(&profile, 0, sizeof(profile));
    configuration = make_configuration();
    memset(configuration.mac_address, 0, VIRTIO_NET_MAC_LENGTH);
    CHECK(virtio_net_feature_profile_activate(
              &profile, &identity, &offered, &configuration, &topology, 1U) ==
              VIRTIO_NET_FEATURE_PROFILE_INVALID_MAC_ADDRESS &&
              !profile.published,
          "feature profile rejects zero MAC address");

    memset(&profile, 0, sizeof(profile));
    configuration = make_configuration();
    configuration.status = 0U;
    CHECK(virtio_net_feature_profile_activate(
              &profile, &identity, &offered, &configuration, &topology, 1U) ==
              VIRTIO_NET_FEATURE_PROFILE_INVALID_STATUS &&
              !profile.published,
          "feature profile rejects zero status");

    memset(&profile, 0, sizeof(profile));
    configuration = make_configuration();
    configuration.status = VIRTIO_NET_STATUS_ANNOUNCE;
    CHECK(virtio_net_feature_profile_activate(
              &profile, &identity, &offered, &configuration, &topology, 1U) ==
              VIRTIO_NET_FEATURE_PROFILE_INVALID_STATUS,
          "feature profile rejects announce-only status");

    memset(&profile, 0, sizeof(profile));
    configuration = make_configuration();
    configuration.status = 4U;
    CHECK(virtio_net_feature_profile_activate(
              &profile, &identity, &offered, &configuration, &topology, 1U) ==
              VIRTIO_NET_FEATURE_PROFILE_INVALID_STATUS,
          "feature profile rejects unknown status bits");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "virtio net feature profile configuration performs no allocation");
}

static void test_topology_policy(void)
{
    struct virtio_net_feature_profile profile;
    struct virtio_pci_identity identity;
    struct virtio_net_feature_bitset offered;
    struct virtio_net_configuration configuration;
    struct virtio_net_topology topology;

    identity = make_identity(VIRTIO_PCI_DEVICE_ID_NET_MODERN, 1U);
    offered = make_modern_offered();
    configuration = make_configuration();

    memset(&profile, 0, sizeof(profile));
    topology = make_topology();
    topology.max_virtqueue_pairs = 0U;
    CHECK(virtio_net_feature_profile_activate(
              &profile, &identity, &offered, &configuration, &topology, 1U) ==
              VIRTIO_NET_FEATURE_PROFILE_INVALID_TOPOLOGY,
          "feature profile rejects zero virtqueue pairs");

    memset(&profile, 0, sizeof(profile));
    topology = make_topology();
    topology.max_virtqueue_pairs = 2U;
    CHECK(virtio_net_feature_profile_activate(
              &profile, &identity, &offered, &configuration, &topology, 1U) ==
              VIRTIO_NET_FEATURE_PROFILE_MULTIQUEUE,
          "feature profile rejects multiqueue topology");

    memset(&profile, 0, sizeof(profile));
    topology = make_topology();
    topology.receive_queue_count = 2U;
    CHECK(virtio_net_feature_profile_activate(
              &profile, &identity, &offered, &configuration, &topology, 1U) ==
              VIRTIO_NET_FEATURE_PROFILE_MULTIQUEUE,
          "feature profile rejects multiple receive queues");

    memset(&profile, 0, sizeof(profile));
    topology = make_topology();
    topology.transmit_queue_count = 0U;
    CHECK(virtio_net_feature_profile_activate(
              &profile, &identity, &offered, &configuration, &topology, 1U) ==
              VIRTIO_NET_FEATURE_PROFILE_MULTIQUEUE,
          "feature profile rejects zero transmit queues");

    memset(&profile, 0, sizeof(profile));
    topology = make_topology();
    topology.control_queue_present = true;
    CHECK(virtio_net_feature_profile_activate(
              &profile, &identity, &offered, &configuration, &topology, 1U) ==
              VIRTIO_NET_FEATURE_PROFILE_CONTROL_QUEUE,
          "feature profile rejects control queue topology");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "virtio net feature profile topology performs no allocation");
}

static void test_generation_and_observed_state(void)
{
    struct virtio_net_feature_profile profile;
    struct virtio_net_feature_profile original;
    struct virtio_pci_identity identity;
    struct virtio_net_feature_bitset offered;
    struct virtio_net_configuration configuration;
    struct virtio_net_topology topology;
    struct virtio_net_feature_bitset observed_offered;
    struct virtio_net_configuration observed_configuration;
    struct virtio_net_topology observed_topology;

    identity = make_identity(VIRTIO_PCI_DEVICE_ID_NET_MODERN, 1U);
    offered = make_modern_offered();
    configuration = make_configuration();
    topology = make_topology();
    memset(&profile, 0, sizeof(profile));

    CHECK(virtio_net_feature_profile_activate(
              &profile, &identity, &offered, &configuration, &topology, 0U) ==
              VIRTIO_NET_FEATURE_PROFILE_INVALID_GENERATION &&
              !profile.published,
          "feature profile rejects zero generation");
    CHECK(virtio_net_feature_profile_activate(
              &profile, &identity, &offered, &configuration, &topology,
              VIRTIO_NET_PROFILE_GENERATION_MAX + 1U) ==
              VIRTIO_NET_FEATURE_PROFILE_INVALID_GENERATION &&
              !profile.published,
          "feature profile rejects over-limit generation");

    memset(&profile, 0, sizeof(profile));
    CHECK(virtio_net_feature_profile_activate(
              &profile, &identity, &offered, &configuration, &topology, 5U) ==
              VIRTIO_NET_FEATURE_PROFILE_OK,
          "observed-state fixture profile activates");
    original = profile;

    CHECK(virtio_net_feature_profile_validate_state(
              NULL, &identity, &offered, &configuration, &topology, 5U) ==
              VIRTIO_NET_FEATURE_PROFILE_NULL,
          "observed-state validation rejects null profile");
    CHECK(virtio_net_feature_profile_validate_state(
              &profile, NULL, &offered, &configuration, &topology, 5U) ==
              VIRTIO_NET_FEATURE_PROFILE_NULL,
          "observed-state validation rejects null identity");
    CHECK(virtio_net_feature_profile_validate_state(
              &profile, &identity, NULL, &configuration, &topology, 5U) ==
              VIRTIO_NET_FEATURE_PROFILE_NULL,
          "observed-state validation rejects null offered bitset");
    CHECK(virtio_net_feature_profile_validate_state(
              &profile, &identity, &offered, NULL, &topology, 5U) ==
              VIRTIO_NET_FEATURE_PROFILE_NULL,
          "observed-state validation rejects null configuration");
    CHECK(virtio_net_feature_profile_validate_state(
              &profile, &identity, &offered, &configuration, NULL, 5U) ==
              VIRTIO_NET_FEATURE_PROFILE_NULL,
          "observed-state validation rejects null topology");

    CHECK(virtio_net_feature_profile_validate_state(
              &profile, &identity, &offered, &configuration, &topology, 5U) ==
              VIRTIO_NET_FEATURE_PROFILE_OK,
          "unchanged observed state validates");

    CHECK(virtio_net_feature_profile_validate_state(
              &profile, &identity, &offered, &configuration, &topology, 6U) ==
              VIRTIO_NET_FEATURE_PROFILE_CHANGED,
          "changed observed generation fails closed");

    observed_offered = offered;
    observed_offered.low |= UINT32_C(1) << VIRTIO_NET_F_CTRL_VQ;
    CHECK(virtio_net_feature_profile_validate_state(
              &profile, &identity, &observed_offered, &configuration,
              &topology, 5U) == VIRTIO_NET_FEATURE_PROFILE_CHANGED,
          "changed observed feature fails closed");

    observed_configuration = configuration;
    observed_configuration.status = VIRTIO_NET_STATUS_ANNOUNCE;
    CHECK(virtio_net_feature_profile_validate_state(
              &profile, &identity, &offered, &observed_configuration,
              &topology, 5U) == VIRTIO_NET_FEATURE_PROFILE_CHANGED,
          "changed observed status fails closed");

    observed_topology = topology;
    observed_topology.control_queue_present = true;
    CHECK(virtio_net_feature_profile_validate_state(
              &profile, &identity, &offered, &configuration,
              &observed_topology, 5U) == VIRTIO_NET_FEATURE_PROFILE_CHANGED,
          "changed observed topology fails closed");
    CHECK(memcmp(&profile, &original, sizeof(profile)) == 0,
          "observed-state validation does not mutate profile");

    memset(&profile, 0, sizeof(profile));
    CHECK(virtio_net_feature_profile_validate_state(
              &profile, &identity, &offered, &configuration, &topology, 5U) ==
              VIRTIO_NET_FEATURE_PROFILE_UNPUBLISHED,
          "observed-state validation rejects unpublished profile");
    CHECK(!virtio_net_feature_profile_published(&profile) &&
              !virtio_net_feature_profile_published(NULL),
          "published accessor rejects null and unpublished profiles");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "virtio net feature profile observed state performs no allocation");
}

int main(void)
{
    test_status_strings();
    test_null_and_identity();
    test_modern_feature_policy();
    test_legacy_feature_policy();
    test_configuration_policy();
    test_topology_policy();
    test_generation_and_observed_state();

    if (failures != 0) {
        fprintf(stderr, "Virtio net feature profile host tests: FAIL\n");
        return 1;
    }
    printf("Virtio net feature profile host tests: ALL OK\n");
    return 0;
}

#endif
