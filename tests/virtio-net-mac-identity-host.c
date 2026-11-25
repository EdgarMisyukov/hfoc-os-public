#include "virtio_net_mac_identity.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifdef HFOC_VIRTIO_NET_MAC_IDENTITY_HOST_TEST

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

static void copy_mac(uint8_t destination[6], const uint8_t source[6])
{
    memcpy(destination, source, 6U);
}

static struct network_device_snapshot make_snapshot(
    const uint8_t mac_address[6])
{
    struct network_device_snapshot snapshot;

    memset(&snapshot, 0, sizeof(snapshot));
    snapshot.version = NETWORK_DEVICE_SNAPSHOT_VERSION;
    snapshot.device_id = NETWORK_DEVICE_ID_MIN;
    snapshot.vendor_id = VIRTIO_PCI_VENDOR_ID;
    snapshot.product_id = VIRTIO_PCI_DEVICE_ID_NET_MODERN;
    snapshot.bus_address = 0x0010U;
    snapshot.capability_mask =
        NETWORK_DEVICE_CAPABILITY_RX |
        NETWORK_DEVICE_CAPABILITY_TX |
        NETWORK_DEVICE_CAPABILITY_LINK_STATE;
    copy_mac(snapshot.mac_address, mac_address);
    return snapshot;
}

static struct network_device_selection make_device_selection(
    const uint8_t mac_address[6])
{
    struct network_device_selection selection;
    struct network_device_snapshot snapshot =
        make_snapshot(mac_address);

    memset(&selection, 0, sizeof(selection));
    CHECK(network_device_selection_init(
              &selection, &snapshot, 1U) == NETWORK_DEVICE_OK,
          "MAC identity fixture publishes device selection");
    return selection;
}

static struct virtio_pci_identity make_identity(void)
{
    struct virtio_pci_identity identity;

    memset(&identity, 0, sizeof(identity));
    identity.version = VIRTIO_PCI_IDENTITY_VERSION;
    identity.vendor_id = VIRTIO_PCI_VENDOR_ID;
    identity.device_id = VIRTIO_PCI_DEVICE_ID_NET_MODERN;
    identity.bdf.bus = 0U;
    identity.bdf.device = 0U;
    identity.bdf.function = 0U;
    identity.modern = 1U;
    return identity;
}

static struct virtio_net_feature_bitset make_offered(void)
{
    struct virtio_net_feature_bitset offered;

    offered.low = VIRTIO_NET_REQUIRED_FEATURE_LO_MASK;
    offered.high = VIRTIO_NET_VERSION_1_HIGH_BIT;
    return offered;
}

static struct virtio_net_configuration make_configuration(
    const uint8_t mac_address[6])
{
    struct virtio_net_configuration configuration;

    memset(&configuration, 0, sizeof(configuration));
    configuration.version = VIRTIO_NET_CONFIGURATION_VERSION;
    copy_mac(configuration.mac_address, mac_address);
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

static struct virtio_net_feature_profile make_profile(
    const uint8_t mac_address[6])
{
    struct virtio_net_feature_profile profile;
    struct virtio_pci_identity identity = make_identity();
    struct virtio_net_feature_bitset offered = make_offered();
    struct virtio_net_configuration configuration =
        make_configuration(mac_address);
    struct virtio_net_topology topology = make_topology();

    memset(&profile, 0, sizeof(profile));
    CHECK(virtio_net_feature_profile_activate(
              &profile, &identity, &offered, &configuration,
              &topology, 2U) == VIRTIO_NET_FEATURE_PROFILE_OK,
          "MAC identity fixture publishes feature profile");
    return profile;
}

static void poison_binding(struct virtio_net_mac_identity *binding)
{
    memset(binding, 0xA5, sizeof(*binding));
    binding->published = false;
    binding->version = 0U;
}

static bool binding_unchanged(
    const struct virtio_net_mac_identity *binding,
    const struct virtio_net_mac_identity *original)
{
    return memcmp(binding, original, sizeof(*binding)) == 0;
}

static void test_status_strings(void)
{
    static const struct {
        enum virtio_net_mac_identity_status status;
        const char *string;
    } cases[] = {
        {VIRTIO_NET_MAC_IDENTITY_OK,
         "VIRTIO_NET_MAC_IDENTITY_OK"},
        {VIRTIO_NET_MAC_IDENTITY_NULL,
         "VIRTIO_NET_MAC_IDENTITY_NULL"},
        {VIRTIO_NET_MAC_IDENTITY_INVALID_VERSION,
         "VIRTIO_NET_MAC_IDENTITY_INVALID_VERSION"},
        {VIRTIO_NET_MAC_IDENTITY_INVALID_GENERATION,
         "VIRTIO_NET_MAC_IDENTITY_INVALID_GENERATION"},
        {VIRTIO_NET_MAC_IDENTITY_ALREADY_PUBLISHED,
         "VIRTIO_NET_MAC_IDENTITY_ALREADY_PUBLISHED"},
        {VIRTIO_NET_MAC_IDENTITY_UNPUBLISHED,
         "VIRTIO_NET_MAC_IDENTITY_UNPUBLISHED"},
        {VIRTIO_NET_MAC_IDENTITY_INVALID_DEVICE,
         "VIRTIO_NET_MAC_IDENTITY_INVALID_DEVICE"},
        {VIRTIO_NET_MAC_IDENTITY_DEVICE_UNPUBLISHED,
         "VIRTIO_NET_MAC_IDENTITY_DEVICE_UNPUBLISHED"},
        {VIRTIO_NET_MAC_IDENTITY_INVALID_PROFILE,
         "VIRTIO_NET_MAC_IDENTITY_INVALID_PROFILE"},
        {VIRTIO_NET_MAC_IDENTITY_PROFILE_UNPUBLISHED,
         "VIRTIO_NET_MAC_IDENTITY_PROFILE_UNPUBLISHED"},
        {VIRTIO_NET_MAC_IDENTITY_ZERO_MAC,
         "VIRTIO_NET_MAC_IDENTITY_ZERO_MAC"},
        {VIRTIO_NET_MAC_IDENTITY_MULTICAST_MAC,
         "VIRTIO_NET_MAC_IDENTITY_MULTICAST_MAC"},
        {VIRTIO_NET_MAC_IDENTITY_DEVICE_MISMATCH,
         "VIRTIO_NET_MAC_IDENTITY_DEVICE_MISMATCH"},
        {VIRTIO_NET_MAC_IDENTITY_CHANGED,
         "VIRTIO_NET_MAC_IDENTITY_CHANGED"},
    };
    size_t index;

    for (index = 0U;
         index < sizeof(cases) / sizeof(cases[0]);
         ++index) {
        const char *string = virtio_net_mac_identity_status_string(
            cases[index].status);

        CHECK(string != NULL &&
                  strcmp(string, cases[index].string) == 0,
              "virtio net MAC identity status string is exact");
    }
    CHECK(virtio_net_mac_identity_status_string(
              (enum virtio_net_mac_identity_status)14U) == NULL,
          "virtio net MAC identity undefined status is rejected");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "virtio net MAC identity status handling allocates nothing");
}

static void test_null_and_state_rejection(void)
{
    static const uint8_t mac[6] = {
        0x52U, 0x54U, 0x00U, 0x12U, 0x34U, 0x56U,
    };
    struct virtio_net_mac_identity binding;
    struct virtio_net_mac_identity original;
    struct network_device_selection device =
        make_device_selection(mac);
    struct virtio_net_feature_profile profile = make_profile(mac);
    struct network_device_selection unpublished_device = device;
    struct virtio_net_feature_profile unpublished_profile = profile;
    struct network_device_selection invalid_device = device;
    struct virtio_net_feature_profile invalid_profile = profile;

    poison_binding(&binding);
    memcpy(&original, &binding, sizeof(binding));
    unpublished_device.published = false;
    unpublished_profile.published = false;
    invalid_device.device.capability_mask = 0U;
    invalid_profile.version = VIRTIO_NET_FEATURE_PROFILE_VERSION + 1U;

    CHECK(virtio_net_mac_identity_bind(
              NULL, &device, &profile, 3U) ==
              VIRTIO_NET_MAC_IDENTITY_NULL,
          "MAC identity bind rejects null binding");
    CHECK(virtio_net_mac_identity_bind(
              &binding, NULL, &profile, 3U) ==
              VIRTIO_NET_MAC_IDENTITY_NULL &&
              binding_unchanged(&binding, &original),
          "MAC identity bind rejects null device selection");
    CHECK(virtio_net_mac_identity_bind(
              &binding, &device, NULL, 3U) ==
              VIRTIO_NET_MAC_IDENTITY_NULL &&
              binding_unchanged(&binding, &original),
          "MAC identity bind rejects null profile");
    CHECK(virtio_net_mac_identity_validate_state(
              NULL, &device, &profile, 3U) ==
              VIRTIO_NET_MAC_IDENTITY_NULL,
          "MAC identity state validation rejects null binding");
    CHECK(virtio_net_mac_identity_validate_state(
              &binding, NULL, &profile, 3U) ==
              VIRTIO_NET_MAC_IDENTITY_NULL,
          "MAC identity state validation rejects null device");
    CHECK(virtio_net_mac_identity_validate_state(
              &binding, &device, NULL, 3U) ==
              VIRTIO_NET_MAC_IDENTITY_NULL,
          "MAC identity state validation rejects null profile");
    CHECK(!virtio_net_mac_identity_published(NULL) &&
              virtio_net_mac_identity_device_selection(NULL) == NULL &&
              virtio_net_mac_identity_profile(NULL) == NULL,
          "MAC identity null queries are rejected");

    poison_binding(&binding);
    binding.version = 99U;
    memcpy(&original, &binding, sizeof(binding));
    CHECK(virtio_net_mac_identity_bind(
              &binding, &device, &profile, 3U) ==
              VIRTIO_NET_MAC_IDENTITY_INVALID_VERSION &&
              binding_unchanged(&binding, &original),
          "MAC identity bind rejects unknown binding version");

    poison_binding(&binding);
    memcpy(&original, &binding, sizeof(binding));
    CHECK(virtio_net_mac_identity_bind(
              &binding, &device, &profile, 0U) ==
              VIRTIO_NET_MAC_IDENTITY_INVALID_GENERATION &&
              binding_unchanged(&binding, &original),
          "MAC identity bind rejects zero generation");
    CHECK(virtio_net_mac_identity_bind(
              &binding, &device, &profile,
              VIRTIO_NET_MAC_IDENTITY_GENERATION_MAX + 1U) ==
              VIRTIO_NET_MAC_IDENTITY_INVALID_GENERATION &&
              binding_unchanged(&binding, &original),
          "MAC identity bind rejects over-limit generation");

    poison_binding(&binding);
    memcpy(&original, &binding, sizeof(binding));
    CHECK(virtio_net_mac_identity_bind(
              &binding, &unpublished_device, &profile, 3U) ==
              VIRTIO_NET_MAC_IDENTITY_DEVICE_UNPUBLISHED &&
              binding_unchanged(&binding, &original),
          "MAC identity bind rejects unpublished device");

    poison_binding(&binding);
    memcpy(&original, &binding, sizeof(binding));
    CHECK(virtio_net_mac_identity_bind(
              &binding, &invalid_device, &profile, 3U) ==
              VIRTIO_NET_MAC_IDENTITY_INVALID_DEVICE &&
              binding_unchanged(&binding, &original),
          "MAC identity bind rejects invalid published device");

    poison_binding(&binding);
    memcpy(&original, &binding, sizeof(binding));
    CHECK(virtio_net_mac_identity_bind(
              &binding, &device, &unpublished_profile, 3U) ==
              VIRTIO_NET_MAC_IDENTITY_PROFILE_UNPUBLISHED &&
              binding_unchanged(&binding, &original),
          "MAC identity bind rejects unpublished profile");

    poison_binding(&binding);
    memcpy(&original, &binding, sizeof(binding));
    CHECK(virtio_net_mac_identity_bind(
              &binding, &device, &invalid_profile, 3U) ==
              VIRTIO_NET_MAC_IDENTITY_INVALID_PROFILE &&
              binding_unchanged(&binding, &original),
          "MAC identity bind rejects invalid published profile");

    poison_binding(&binding);
    CHECK(virtio_net_mac_identity_validate_state(
              &binding, &device, &profile, 3U) ==
              VIRTIO_NET_MAC_IDENTITY_UNPUBLISHED,
          "MAC identity state validation rejects unpublished binding");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "MAC identity null and state rejection allocates nothing");
}

static void test_mac_rejections(void)
{
    static const uint8_t unicast[6] = {
        0x52U, 0x54U, 0x00U, 0x12U, 0x34U, 0x56U,
    };
    static const uint8_t multicast[6] = {
        0x01U, 0x00U, 0x5EU, 0x00U, 0x00U, 0x01U,
    };
    static const uint8_t broadcast[6] = {
        0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU,
    };
    struct virtio_net_mac_identity binding;
    struct virtio_net_mac_identity original;
    struct network_device_selection device;
    struct virtio_net_feature_profile profile;
    struct network_device_selection zero_device;
    struct virtio_net_feature_profile zero_profile;
    struct network_device_selection multicast_device;
    struct virtio_net_feature_profile multicast_profile;
    struct network_device_selection broadcast_device;

    device = make_device_selection(unicast);
    profile = make_profile(unicast);

    zero_device = make_device_selection(unicast);
    memset(zero_device.device.mac_address, 0,
           NETWORK_DEVICE_MAC_LENGTH);
    poison_binding(&binding);
    memcpy(&original, &binding, sizeof(binding));
    CHECK(virtio_net_mac_identity_bind(
              &binding, &zero_device, &profile, 3U) ==
              VIRTIO_NET_MAC_IDENTITY_ZERO_MAC &&
              binding_unchanged(&binding, &original),
          "MAC identity bind rejects zero device MAC");

    zero_profile = make_profile(unicast);
    memset(zero_profile.mac_address, 0, VIRTIO_NET_MAC_LENGTH);
    poison_binding(&binding);
    memcpy(&original, &binding, sizeof(binding));
    CHECK(virtio_net_mac_identity_bind(
              &binding, &device, &zero_profile, 3U) ==
              VIRTIO_NET_MAC_IDENTITY_ZERO_MAC &&
              binding_unchanged(&binding, &original),
          "MAC identity bind rejects zero profile MAC");

    multicast_device = make_device_selection(unicast);
    copy_mac(multicast_device.device.mac_address, multicast);
    poison_binding(&binding);
    memcpy(&original, &binding, sizeof(binding));
    CHECK(virtio_net_mac_identity_bind(
              &binding, &multicast_device, &profile, 3U) ==
              VIRTIO_NET_MAC_IDENTITY_MULTICAST_MAC &&
              binding_unchanged(&binding, &original),
          "MAC identity bind rejects multicast device MAC");

    multicast_profile = make_profile(multicast);
    poison_binding(&binding);
    memcpy(&original, &binding, sizeof(binding));
    CHECK(virtio_net_mac_identity_bind(
              &binding, &device, &multicast_profile, 3U) ==
              VIRTIO_NET_MAC_IDENTITY_MULTICAST_MAC &&
              binding_unchanged(&binding, &original),
          "MAC identity bind rejects multicast profile MAC");

    broadcast_device = make_device_selection(unicast);
    copy_mac(broadcast_device.device.mac_address, broadcast);
    poison_binding(&binding);
    memcpy(&original, &binding, sizeof(binding));
    CHECK(virtio_net_mac_identity_bind(
              &binding, &broadcast_device, &profile, 3U) ==
              VIRTIO_NET_MAC_IDENTITY_MULTICAST_MAC &&
              binding_unchanged(&binding, &original),
          "MAC identity bind rejects broadcast device MAC");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "MAC identity MAC rejection allocates nothing");
}

static void test_identity_mismatch(void)
{
    static const uint8_t mac[6] = {
        0x52U, 0x54U, 0x00U, 0x12U, 0x34U, 0x56U,
    };
    static const uint8_t other_mac[6] = {
        0x52U, 0x54U, 0x00U, 0x12U, 0x34U, 0x57U,
    };
    struct virtio_net_mac_identity binding;
    struct virtio_net_mac_identity original;
    struct network_device_selection device = make_device_selection(mac);
    struct virtio_net_feature_profile profile = make_profile(mac);
    struct network_device_selection vendor_mismatch = device;
    struct network_device_selection product_mismatch = device;
    struct virtio_net_feature_profile mac_mismatch = make_profile(other_mac);

    vendor_mismatch.device.vendor_id = 0x1234U;
    product_mismatch.device.product_id = 0x1000U;

    poison_binding(&binding);
    memcpy(&original, &binding, sizeof(binding));
    CHECK(virtio_net_mac_identity_bind(
              &binding, &vendor_mismatch, &profile, 3U) ==
              VIRTIO_NET_MAC_IDENTITY_DEVICE_MISMATCH &&
              binding_unchanged(&binding, &original),
          "MAC identity bind rejects vendor mismatch");

    poison_binding(&binding);
    memcpy(&original, &binding, sizeof(binding));
    CHECK(virtio_net_mac_identity_bind(
              &binding, &product_mismatch, &profile, 3U) ==
              VIRTIO_NET_MAC_IDENTITY_DEVICE_MISMATCH &&
              binding_unchanged(&binding, &original),
          "MAC identity bind rejects product mismatch");

    poison_binding(&binding);
    memcpy(&original, &binding, sizeof(binding));
    CHECK(virtio_net_mac_identity_bind(
              &binding, &device, &mac_mismatch, 3U) ==
              VIRTIO_NET_MAC_IDENTITY_DEVICE_MISMATCH &&
              binding_unchanged(&binding, &original),
          "MAC identity bind rejects MAC mismatch");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "MAC identity mismatch rejection allocates nothing");
}

static void test_bind_and_duplicate(void)
{
    static const uint8_t mac[6] = {
        0x52U, 0x54U, 0x00U, 0x12U, 0x34U, 0x56U,
    };
    static const uint8_t other_mac[6] = {
        0x52U, 0x54U, 0x00U, 0x12U, 0x34U, 0x57U,
    };
    struct virtio_net_mac_identity binding;
    struct virtio_net_mac_identity original;
    struct network_device_selection device =
        make_device_selection(mac);
    struct virtio_net_feature_profile profile = make_profile(mac);

    poison_binding(&binding);
    CHECK(virtio_net_mac_identity_bind(
              &binding, &device, &profile, 3U) ==
              VIRTIO_NET_MAC_IDENTITY_OK &&
              virtio_net_mac_identity_published(&binding) &&
              binding.version == VIRTIO_NET_MAC_IDENTITY_VERSION &&
              binding.generation == 3U &&
              memcmp(&binding.device, &device,
                     sizeof(binding.device)) == 0 &&
              memcmp(&binding.profile, &profile,
                     sizeof(binding.profile)) == 0,
          "MAC identity bind publishes exact source objects");
    CHECK(virtio_net_mac_identity_device_selection(&binding) ==
              &binding.device &&
              virtio_net_mac_identity_profile(&binding) ==
                  &binding.profile &&
              memcmp(virtio_net_mac_identity_device_selection(&binding),
                     &device, sizeof(device)) == 0 &&
              memcmp(virtio_net_mac_identity_profile(&binding),
                     &profile, sizeof(profile)) == 0,
          "MAC identity accessors return exact immutable copies");

    memcpy(&original, &binding, sizeof(binding));
    CHECK(virtio_net_mac_identity_bind(
              &binding, &device, &profile, 3U) ==
              VIRTIO_NET_MAC_IDENTITY_ALREADY_PUBLISHED &&
              binding_unchanged(&binding, &original),
          "MAC identity bind rejects duplicate publication");

    binding.published = false;
    binding.version = 99U;
    CHECK(!virtio_net_mac_identity_published(&binding) &&
              virtio_net_mac_identity_device_selection(&binding) == NULL &&
              virtio_net_mac_identity_profile(&binding) == NULL,
          "MAC identity queries reject corrupted binding");

    binding.published = true;
    binding.version = VIRTIO_NET_MAC_IDENTITY_VERSION;
    binding.generation = 3U;
    copy_mac(binding.profile.mac_address, other_mac);
    CHECK(!virtio_net_mac_identity_published(&binding) &&
              virtio_net_mac_identity_device_selection(&binding) == NULL &&
              virtio_net_mac_identity_profile(&binding) == NULL,
          "MAC identity queries reject internally mismatched binding");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "MAC identity bind and duplicate handling allocates nothing");
}

static void test_observed_state_validation(void)
{
    static const uint8_t mac[6] = {
        0x52U, 0x54U, 0x00U, 0x12U, 0x34U, 0x56U,
    };
    static const uint8_t other_mac[6] = {
        0x52U, 0x54U, 0x00U, 0x12U, 0x34U, 0x57U,
    };
    struct virtio_net_mac_identity binding;
    struct network_device_selection device =
        make_device_selection(mac);
    struct virtio_net_feature_profile profile = make_profile(mac);
    struct network_device_selection changed_device;
    struct virtio_net_feature_profile changed_profile;

    poison_binding(&binding);
    CHECK(virtio_net_mac_identity_bind(
              &binding, &device, &profile, 3U) ==
              VIRTIO_NET_MAC_IDENTITY_OK,
          "observed-state fixture binds valid identity");
    CHECK(virtio_net_mac_identity_validate_state(
              &binding, &device, &profile, 3U) ==
              VIRTIO_NET_MAC_IDENTITY_OK,
          "MAC identity state validation accepts unchanged state");
    CHECK(virtio_net_mac_identity_validate_state(
              &binding, &device, &profile, 0U) ==
              VIRTIO_NET_MAC_IDENTITY_INVALID_GENERATION,
          "MAC identity state validation rejects zero generation");
    CHECK(virtio_net_mac_identity_validate_state(
              &binding, &device, &profile,
              VIRTIO_NET_MAC_IDENTITY_GENERATION_MAX + 1U) ==
              VIRTIO_NET_MAC_IDENTITY_INVALID_GENERATION,
          "MAC identity state validation rejects over-limit generation");
    CHECK(virtio_net_mac_identity_validate_state(
              &binding, &device, &profile, 4U) ==
              VIRTIO_NET_MAC_IDENTITY_CHANGED,
          "MAC identity state validation rejects changed binding generation");

    changed_device = device;
    changed_device.generation = 2U;
    CHECK(virtio_net_mac_identity_validate_state(
              &binding, &changed_device, &profile, 3U) ==
              VIRTIO_NET_MAC_IDENTITY_CHANGED,
          "MAC identity state validation rejects changed device generation");
    changed_device = device;
    copy_mac(changed_device.device.mac_address, other_mac);
    CHECK(virtio_net_mac_identity_validate_state(
              &binding, &changed_device, &profile, 3U) ==
              VIRTIO_NET_MAC_IDENTITY_CHANGED,
          "MAC identity state validation rejects changed device MAC");
    changed_device = device;
    changed_device.device.capability_mask = 0U;
    CHECK(virtio_net_mac_identity_validate_state(
              &binding, &changed_device, &profile, 3U) ==
              VIRTIO_NET_MAC_IDENTITY_INVALID_DEVICE,
          "MAC identity state validation rejects invalid observed device");

    changed_profile = profile;
    changed_profile.generation = 3U;
    CHECK(virtio_net_mac_identity_validate_state(
              &binding, &device, &changed_profile, 3U) ==
              VIRTIO_NET_MAC_IDENTITY_CHANGED,
          "MAC identity state validation rejects changed profile generation");
    changed_profile = profile;
    copy_mac(changed_profile.mac_address, other_mac);
    CHECK(virtio_net_mac_identity_validate_state(
              &binding, &device, &changed_profile, 3U) ==
              VIRTIO_NET_MAC_IDENTITY_CHANGED,
          "MAC identity state validation rejects changed profile MAC");
    changed_profile = profile;
    changed_profile.version = VIRTIO_NET_FEATURE_PROFILE_VERSION + 1U;
    CHECK(virtio_net_mac_identity_validate_state(
              &binding, &device, &changed_profile, 3U) ==
              VIRTIO_NET_MAC_IDENTITY_INVALID_PROFILE,
          "MAC identity state validation rejects invalid observed profile");

    binding.version = VIRTIO_NET_MAC_IDENTITY_VERSION + 1U;
    CHECK(virtio_net_mac_identity_validate_state(
              &binding, &device, &profile, 3U) ==
              VIRTIO_NET_MAC_IDENTITY_INVALID_VERSION,
          "MAC identity state validation rejects changed binding version");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "MAC identity observed-state validation allocates nothing");
}

int main(void)
{
    test_status_strings();
    test_null_and_state_rejection();
    test_mac_rejections();
    test_identity_mismatch();
    test_bind_and_duplicate();
    test_observed_state_validation();

    if (failures != 0) {
        fprintf(stderr,
                "Virtio net MAC identity host tests: FAIL\n");
        return 1;
    }
    printf("Virtio net MAC identity host tests: ALL OK\n");
    return 0;
}

#endif
