#include "network_ipv4_profile.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifdef HFOC_NETWORK_IPV4_PROFILE_HOST_TEST

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
          "IPv4 profile fixture publishes device selection");
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
          "IPv4 profile fixture publishes feature profile");
    return profile;
}

static struct virtio_net_mac_identity make_mac_identity_with_mac(
    const uint8_t mac_address[6])
{
    struct virtio_net_mac_identity binding;
    struct network_device_selection device =
        make_device_selection(mac_address);
    struct virtio_net_feature_profile profile =
        make_profile(mac_address);

    memset(&binding, 0, sizeof(binding));
    CHECK(virtio_net_mac_identity_bind(
              &binding, &device, &profile, 3U) ==
              VIRTIO_NET_MAC_IDENTITY_OK,
          "IPv4 profile fixture publishes MAC identity");
    return binding;
}

static struct virtio_net_mac_identity make_mac_identity(void)
{
    static const uint8_t mac_address[6] = {
        0x52U, 0x54U, 0x00U, 0x12U, 0x34U, 0x56U,
    };

    return make_mac_identity_with_mac(mac_address);
}

static void poison_profile(struct network_ipv4_profile *profile)
{
    memset(profile, 0xA5, sizeof(*profile));
    profile->published = false;
    profile->version = 0U;
}

static bool profile_unchanged(
    const struct network_ipv4_profile *profile,
    const struct network_ipv4_profile *original)
{
    return memcmp(profile, original, sizeof(*profile)) == 0;
}

static bool parse_config(
    const char *input,
    struct network_ipv4_config *config)
{
    return network_ipv4_config_parse(
               input, strlen(input), config) == NETWORK_IPV4_OK;
}

static bool initialize_profile(
    struct network_ipv4_profile *profile)
{
    struct network_ipv4_config config;
    struct network_ipv4_port_policy policy;
    struct virtio_net_mac_identity identity =
        make_mac_identity();

    poison_profile(profile);
    if (network_ipv4_profile_qemu_fill(
            &config, &policy) != NETWORK_IPV4_PROFILE_OK) {
        return false;
    }
    return network_ipv4_profile_init(
               profile, &config, &identity, &policy, 7U) ==
           NETWORK_IPV4_PROFILE_OK;
}

static const char replacement_config[] =
    "schema_version=1\n"
    "ipv4_address=192.0.2.11\n"
    "ipv4_prefix=24\n"
    "ipv4_gateway=192.0.2.1\n"
    "ipv4_dns=192.0.2.53\n";

static const char malformed_config[] =
    "schema_version=1\n"
    "ipv4_address=192.0.2.11\n"
    "ipv4_prefix=24\n"
    "ipv4_gateway=192.0.2.1\n";

static void test_status_strings(void)
{
    static const struct {
        enum network_ipv4_profile_status status;
        const char *string;
    } cases[] = {
        {NETWORK_IPV4_PROFILE_OK, "NETWORK_IPV4_PROFILE_OK"},
        {NETWORK_IPV4_PROFILE_NULL, "NETWORK_IPV4_PROFILE_NULL"},
        {NETWORK_IPV4_PROFILE_BAD_STATE,
         "NETWORK_IPV4_PROFILE_BAD_STATE"},
        {NETWORK_IPV4_PROFILE_BAD_VERSION,
         "NETWORK_IPV4_PROFILE_BAD_VERSION"},
        {NETWORK_IPV4_PROFILE_INVALID_GENERATION,
         "NETWORK_IPV4_PROFILE_INVALID_GENERATION"},
        {NETWORK_IPV4_PROFILE_ALREADY_PUBLISHED,
         "NETWORK_IPV4_PROFILE_ALREADY_PUBLISHED"},
        {NETWORK_IPV4_PROFILE_UNPUBLISHED,
         "NETWORK_IPV4_PROFILE_UNPUBLISHED"},
        {NETWORK_IPV4_PROFILE_INVALID_CONFIG,
         "NETWORK_IPV4_PROFILE_INVALID_CONFIG"},
        {NETWORK_IPV4_PROFILE_INVALID_TRANSACTION,
         "NETWORK_IPV4_PROFILE_INVALID_TRANSACTION"},
        {NETWORK_IPV4_PROFILE_IDENTITY_UNPUBLISHED,
         "NETWORK_IPV4_PROFILE_IDENTITY_UNPUBLISHED"},
        {NETWORK_IPV4_PROFILE_INVALID_PORT,
         "NETWORK_IPV4_PROFILE_INVALID_PORT"},
        {NETWORK_IPV4_PROFILE_NO_PENDING,
         "NETWORK_IPV4_PROFILE_NO_PENDING"},
        {NETWORK_IPV4_PROFILE_PARSE_FAILED,
         "NETWORK_IPV4_PROFILE_PARSE_FAILED"},
        {NETWORK_IPV4_PROFILE_COMMIT_FAILED,
         "NETWORK_IPV4_PROFILE_COMMIT_FAILED"},
        {NETWORK_IPV4_PROFILE_CHANGED,
         "NETWORK_IPV4_PROFILE_CHANGED"},
    };
    size_t index;

    for (index = 0U;
         index < sizeof(cases) / sizeof(cases[0]);
         ++index) {
        const char *string = network_ipv4_profile_status_string(
            cases[index].status);

        CHECK(string != NULL &&
                  strcmp(string, cases[index].string) == 0,
              "IPv4 profile status string is exact");
    }
    CHECK(network_ipv4_profile_status_string(
              (enum network_ipv4_profile_status)15U) == NULL,
          "IPv4 profile undefined status is rejected");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "IPv4 profile status handling allocates nothing");
}

static void test_qemu_fill(void)
{
    struct network_ipv4_config config;
    struct network_ipv4_port_policy policy;
    struct network_ipv4_config original;

    memset(&config, 0xA5, sizeof(config));
    memcpy(&original, &config, sizeof(original));
    CHECK(network_ipv4_profile_qemu_fill(NULL, &policy) ==
              NETWORK_IPV4_PROFILE_NULL,
          "IPv4 profile QEMU fill rejects null config");
    CHECK(network_ipv4_profile_qemu_fill(&config, NULL) ==
              NETWORK_IPV4_PROFILE_NULL,
          "IPv4 profile QEMU fill rejects null port policy");
    CHECK(network_ipv4_profile_qemu_fill(
              &config, &policy) == NETWORK_IPV4_PROFILE_OK &&
              config.schema_version == NETWORK_IPV4_CONFIG_VERSION &&
              config.address.octets[0] ==
                  NETWORK_IPV4_PROFILE_QEMU_ADDRESS_0 &&
              config.address.octets[1] ==
                  NETWORK_IPV4_PROFILE_QEMU_ADDRESS_1 &&
              config.address.octets[2] ==
                  NETWORK_IPV4_PROFILE_QEMU_ADDRESS_2 &&
              config.address.octets[3] ==
                  NETWORK_IPV4_PROFILE_QEMU_ADDRESS_3 &&
              config.prefix_length ==
                  NETWORK_IPV4_PROFILE_QEMU_PREFIX &&
              config.gateway.octets[0] ==
                  NETWORK_IPV4_PROFILE_QEMU_GATEWAY_0 &&
              config.gateway.octets[1] ==
                  NETWORK_IPV4_PROFILE_QEMU_GATEWAY_1 &&
              config.gateway.octets[2] ==
                  NETWORK_IPV4_PROFILE_QEMU_GATEWAY_2 &&
              config.gateway.octets[3] ==
                  NETWORK_IPV4_PROFILE_QEMU_GATEWAY_3 &&
              config.dns.octets[0] ==
                  NETWORK_IPV4_PROFILE_QEMU_DNS_0 &&
              config.dns.octets[1] ==
                  NETWORK_IPV4_PROFILE_QEMU_DNS_1 &&
              config.dns.octets[2] ==
                  NETWORK_IPV4_PROFILE_QEMU_DNS_2 &&
              config.dns.octets[3] ==
                  NETWORK_IPV4_PROFILE_QEMU_DNS_3 &&
              policy.version == NETWORK_IPV4_PORT_POLICY_VERSION &&
              policy.port == NETWORK_IPV4_PROFILE_QEMU_PORT &&
              network_ipv4_config_validate(&config) ==
                  NETWORK_IPV4_OK &&
              network_ipv4_port_policy_validate(&policy) ==
                  NETWORK_IPV4_PROFILE_OK,
          "IPv4 profile QEMU fill publishes exact fixed profile");
    CHECK(network_ipv4_profile_qemu_fill(
              &config, &policy) == NETWORK_IPV4_PROFILE_OK &&
              memcmp(&config, &original, sizeof(original)) != 0,
          "IPv4 profile QEMU fill replaces poisoned config");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "IPv4 profile QEMU fill allocates nothing");
}

static void test_validation_and_null_arguments(void)
{
    struct network_ipv4_profile profile;
    struct network_ipv4_profile original;
    struct network_ipv4_config config;
    struct network_ipv4_port_policy policy;
    struct virtio_net_mac_identity identity =
        make_mac_identity();
    struct virtio_net_mac_identity unpublished_identity =
        identity;
    struct network_ipv4_port_policy invalid_policy;

    memset(&config, 0, sizeof(config));
    config.schema_version = NETWORK_IPV4_CONFIG_VERSION;
    memset(&policy, 0, sizeof(policy));
    policy.version = NETWORK_IPV4_PORT_POLICY_VERSION;
    policy.port = 443U;
    unpublished_identity.published = false;

    CHECK(network_ipv4_port_policy_validate(NULL) ==
              NETWORK_IPV4_PROFILE_NULL,
          "IPv4 port policy validation rejects null policy");
    invalid_policy = policy;
    invalid_policy.version = 0U;
    CHECK(network_ipv4_port_policy_validate(&invalid_policy) ==
              NETWORK_IPV4_PROFILE_BAD_VERSION,
          "IPv4 port policy validation rejects bad version");
    invalid_policy = policy;
    invalid_policy.port = 0U;
    CHECK(network_ipv4_port_policy_validate(&invalid_policy) ==
              NETWORK_IPV4_PROFILE_INVALID_PORT,
          "IPv4 port policy validation rejects zero port");

    poison_profile(&profile);
    CHECK(network_ipv4_profile_init(
              NULL, &config, &identity, &policy, 7U) ==
              NETWORK_IPV4_PROFILE_NULL,
          "IPv4 profile init rejects null profile");
    CHECK(network_ipv4_profile_init(
              &profile, NULL, &identity, &policy, 7U) ==
              NETWORK_IPV4_PROFILE_NULL,
          "IPv4 profile init rejects null config");
    CHECK(network_ipv4_profile_init(
              &profile, &config, NULL, &policy, 7U) ==
              NETWORK_IPV4_PROFILE_NULL,
          "IPv4 profile init rejects null identity");
    CHECK(network_ipv4_profile_init(
              &profile, &config, &identity, NULL, 7U) ==
              NETWORK_IPV4_PROFILE_NULL,
          "IPv4 profile init rejects null port policy");

    poison_profile(&profile);
    CHECK(network_ipv4_profile_init(
              &profile, &config, &identity, &policy, 7U) ==
              NETWORK_IPV4_PROFILE_INVALID_CONFIG &&
              !network_ipv4_profile_published(&profile),
          "IPv4 profile init rejects invalid config");

    CHECK(network_ipv4_profile_qemu_fill(
              &config, &policy) == NETWORK_IPV4_PROFILE_OK,
          "IPv4 profile negative-init fixture fills valid QEMU profile");
    poison_profile(&profile);
    CHECK(network_ipv4_profile_init(
              &profile, &config, &unpublished_identity,
              &policy, 7U) ==
              NETWORK_IPV4_PROFILE_IDENTITY_UNPUBLISHED &&
              !network_ipv4_profile_published(&profile),
          "IPv4 profile init rejects unpublished identity");

    poison_profile(&profile);
    CHECK(network_ipv4_profile_init(
              &profile, &config, &identity, &invalid_policy,
              7U) == NETWORK_IPV4_PROFILE_INVALID_PORT &&
              !network_ipv4_profile_published(&profile),
          "IPv4 profile init rejects invalid port policy");

    poison_profile(&profile);
    CHECK(network_ipv4_profile_init(
              &profile, &config, &identity, &policy, 0U) ==
              NETWORK_IPV4_PROFILE_INVALID_GENERATION &&
              !network_ipv4_profile_published(&profile),
          "IPv4 profile init rejects zero generation");
    CHECK(network_ipv4_profile_init(
              &profile, &config, &identity, &policy,
              NETWORK_IPV4_PROFILE_GENERATION_MAX + 1U) ==
              NETWORK_IPV4_PROFILE_INVALID_GENERATION &&
              !network_ipv4_profile_published(&profile),
          "IPv4 profile init rejects over-limit generation");

    CHECK(network_ipv4_profile_qemu_fill(
              &config, &policy) == NETWORK_IPV4_PROFILE_OK &&
              network_ipv4_profile_init(
                  &profile, &config, &identity, &policy, 7U) ==
                  NETWORK_IPV4_PROFILE_OK &&
              network_ipv4_profile_published(&profile),
          "IPv4 profile init publishes valid fixed profile");
    memcpy(&original, &profile, sizeof(profile));
    CHECK(network_ipv4_profile_init(
              &profile, &config, &identity, &policy, 7U) ==
              NETWORK_IPV4_PROFILE_ALREADY_PUBLISHED &&
              profile_unchanged(&profile, &original),
          "IPv4 profile init rejects duplicate publication");

    CHECK(network_ipv4_profile_stage(
              NULL, replacement_config,
              sizeof(replacement_config) - 1U,
              &identity, &policy) == NETWORK_IPV4_PROFILE_NULL,
          "IPv4 profile stage rejects null profile");
    CHECK(network_ipv4_profile_stage(
              &profile, NULL, 1U, &identity, &policy) ==
              NETWORK_IPV4_PROFILE_NULL,
          "IPv4 profile stage rejects null input");
    CHECK(network_ipv4_profile_stage(
              &profile, replacement_config,
              sizeof(replacement_config) - 1U,
              NULL, &policy) == NETWORK_IPV4_PROFILE_NULL,
          "IPv4 profile stage rejects null identity");
    CHECK(network_ipv4_profile_stage(
              &profile, replacement_config,
              sizeof(replacement_config) - 1U,
              &identity, NULL) == NETWORK_IPV4_PROFILE_NULL,
          "IPv4 profile stage rejects null port policy");
    CHECK(network_ipv4_profile_commit(NULL) ==
              NETWORK_IPV4_PROFILE_NULL,
          "IPv4 profile commit rejects null profile");
    CHECK(network_ipv4_profile_rollback(NULL) ==
              NETWORK_IPV4_PROFILE_NULL,
          "IPv4 profile rollback rejects null profile");

    poison_profile(&profile);
    CHECK(network_ipv4_profile_commit(&profile) ==
              NETWORK_IPV4_PROFILE_UNPUBLISHED,
          "IPv4 profile commit before init fails closed");
    CHECK(network_ipv4_profile_rollback(&profile) ==
              NETWORK_IPV4_PROFILE_UNPUBLISHED,
          "IPv4 profile rollback before init fails closed");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "IPv4 profile validation allocates nothing");
}

static void test_stage_preserves_active_state(void)
{
    struct network_ipv4_profile profile;
    struct network_ipv4_config active_before;
    struct network_ipv4_config replacement;
    struct network_ipv4_port_policy policy;
    struct network_ipv4_port_policy invalid_policy;
    struct virtio_net_mac_identity identity =
        make_mac_identity();
    struct virtio_net_mac_identity unpublished_identity =
        identity;
    struct virtio_net_mac_identity original_identity;
    uint16_t original_port;

    CHECK(initialize_profile(&profile),
          "stage fixture initializes valid profile");
    active_before = *network_ipv4_profile_active_config(&profile);
    original_identity = *network_ipv4_profile_identity(&profile);
    original_port = network_ipv4_profile_port_policy(&profile)->port;
    policy = *network_ipv4_profile_port_policy(&profile);
    unpublished_identity.published = false;
    invalid_policy = policy;
    invalid_policy.port = 0U;

    CHECK(network_ipv4_profile_stage(
              &profile, malformed_config,
              sizeof(malformed_config) - 1U,
              &identity, &policy) ==
              NETWORK_IPV4_PROFILE_PARSE_FAILED &&
              !network_ipv4_profile_has_pending(&profile) &&
              memcmp(network_ipv4_profile_active_config(&profile),
                     &active_before, sizeof(active_before)) == 0 &&
              memcmp(network_ipv4_profile_identity(&profile),
                     &original_identity,
                     sizeof(original_identity)) == 0 &&
              network_ipv4_profile_port_policy(&profile)->port ==
                  original_port,
          "malformed IPv4 profile stage preserves active state");

    CHECK(network_ipv4_profile_stage(
              &profile, replacement_config,
              sizeof(replacement_config) - 1U,
              &unpublished_identity, &policy) ==
              NETWORK_IPV4_PROFILE_IDENTITY_UNPUBLISHED &&
              !network_ipv4_profile_has_pending(&profile) &&
              memcmp(network_ipv4_profile_active_config(&profile),
                     &active_before, sizeof(active_before)) == 0,
          "partial profile with unpublished identity preserves active state");

    CHECK(network_ipv4_profile_stage(
              &profile, replacement_config,
              sizeof(replacement_config) - 1U,
              &identity, &invalid_policy) ==
              NETWORK_IPV4_PROFILE_INVALID_PORT &&
              !network_ipv4_profile_has_pending(&profile) &&
              memcmp(network_ipv4_profile_active_config(&profile),
                     &active_before, sizeof(active_before)) == 0,
          "partial profile with invalid port preserves active state");

    CHECK(parse_config(replacement_config, &replacement),
          "stage fixture parses replacement");
    CHECK(network_ipv4_profile_stage(
              &profile, replacement_config,
              sizeof(replacement_config) - 1U,
              &identity, &policy) == NETWORK_IPV4_PROFILE_OK &&
              network_ipv4_profile_has_pending(&profile) &&
              memcmp(network_ipv4_profile_pending_config(&profile),
                     &replacement, sizeof(replacement)) == 0 &&
              memcmp(network_ipv4_profile_pending_identity(&profile),
                     &identity, sizeof(identity)) == 0 &&
              network_ipv4_profile_pending_port_policy(&profile)->port ==
                  original_port &&
              memcmp(network_ipv4_profile_active_config(&profile),
                     &active_before, sizeof(active_before)) == 0,
          "valid IPv4 profile stage creates pending without active mutation");

    CHECK(network_ipv4_profile_stage(
              &profile, malformed_config,
              sizeof(malformed_config) - 1U,
              &identity, &policy) ==
              NETWORK_IPV4_PROFILE_PARSE_FAILED &&
              !network_ipv4_profile_has_pending(&profile) &&
              memcmp(network_ipv4_profile_active_config(&profile),
                     &active_before, sizeof(active_before)) == 0 &&
              memcmp(network_ipv4_profile_identity(&profile),
                     &original_identity,
                     sizeof(original_identity)) == 0,
          "later malformed stage clears stale pending and preserves active");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "IPv4 profile staging allocates nothing");
}

static void test_commit_and_rollback(void)
{
    struct network_ipv4_profile profile;
    struct network_ipv4_config active_before;
    struct network_ipv4_config replacement;
    struct network_ipv4_port_policy policy;
    struct virtio_net_mac_identity identity =
        make_mac_identity();
    struct virtio_net_mac_identity pending_identity;

    CHECK(initialize_profile(&profile),
          "commit fixture initializes valid profile");
    active_before = *network_ipv4_profile_active_config(&profile);
    policy = *network_ipv4_profile_port_policy(&profile);

    CHECK(network_ipv4_profile_commit(&profile) ==
              NETWORK_IPV4_PROFILE_NO_PENDING &&
              memcmp(network_ipv4_profile_active_config(&profile),
                     &active_before, sizeof(active_before)) == 0,
          "IPv4 profile commit without pending preserves active");

    CHECK(network_ipv4_profile_stage(
              &profile, replacement_config,
              sizeof(replacement_config) - 1U,
              &identity, &policy) == NETWORK_IPV4_PROFILE_OK &&
              network_ipv4_profile_rollback(&profile) ==
                  NETWORK_IPV4_PROFILE_OK &&
              !network_ipv4_profile_has_pending(&profile) &&
              memcmp(network_ipv4_profile_active_config(&profile),
                     &active_before, sizeof(active_before)) == 0,
          "IPv4 profile rollback preserves active state");

    CHECK(network_ipv4_profile_stage(
              &profile, replacement_config,
              sizeof(replacement_config) - 1U,
              &identity, &policy) == NETWORK_IPV4_PROFILE_OK,
          "invalid-commit fixture stages candidate");
    pending_identity = *network_ipv4_profile_pending_identity(&profile);
    pending_identity.published = false;
    profile.pending_identity = pending_identity;
    CHECK(network_ipv4_profile_commit(&profile) ==
              NETWORK_IPV4_PROFILE_IDENTITY_UNPUBLISHED &&
              !network_ipv4_profile_has_pending(&profile) &&
              memcmp(network_ipv4_profile_active_config(&profile),
                     &active_before, sizeof(active_before)) == 0 &&
              memcmp(network_ipv4_profile_identity(&profile),
                     &identity, sizeof(identity)) == 0,
          "invalid pending identity commit preserves active state");

    CHECK(network_ipv4_profile_stage(
              &profile, replacement_config,
              sizeof(replacement_config) - 1U,
              &identity, &policy) == NETWORK_IPV4_PROFILE_OK,
          "invalid-port fixture stages candidate");
    profile.pending_port_policy.port = 0U;
    CHECK(network_ipv4_profile_commit(&profile) ==
              NETWORK_IPV4_PROFILE_INVALID_PORT &&
              !network_ipv4_profile_has_pending(&profile) &&
              memcmp(network_ipv4_profile_active_config(&profile),
                     &active_before, sizeof(active_before)) == 0,
          "invalid pending port commit preserves active state");

    CHECK(network_ipv4_profile_stage(
              &profile, replacement_config,
              sizeof(replacement_config) - 1U,
              &identity, &policy) == NETWORK_IPV4_PROFILE_OK,
          "invalid-config fixture stages candidate");
    profile.config.pending.prefix_length = 33U;
    CHECK(network_ipv4_profile_commit(&profile) ==
              NETWORK_IPV4_PROFILE_COMMIT_FAILED &&
              !network_ipv4_profile_has_pending(&profile) &&
              memcmp(network_ipv4_profile_active_config(&profile),
                     &active_before, sizeof(active_before)) == 0,
          "invalid pending config commit preserves active state");

    CHECK(parse_config(replacement_config, &replacement),
          "successful-commit fixture parses replacement");
    CHECK(network_ipv4_profile_stage(
              &profile, replacement_config,
              sizeof(replacement_config) - 1U,
              &identity, &policy) == NETWORK_IPV4_PROFILE_OK &&
              network_ipv4_profile_commit(&profile) ==
                  NETWORK_IPV4_PROFILE_OK &&
              !network_ipv4_profile_has_pending(&profile) &&
              memcmp(network_ipv4_profile_active_config(&profile),
                     &replacement, sizeof(replacement)) == 0 &&
              memcmp(network_ipv4_profile_identity(&profile),
                     &identity, sizeof(identity)) == 0 &&
              network_ipv4_profile_port_policy(&profile)->port ==
                  policy.port,
          "successful IPv4 profile commit publishes exact pending state");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "IPv4 profile commit and rollback allocate nothing");
}

static void test_observed_state_validation(void)
{
    static const uint8_t other_mac[6] = {
        0x52U, 0x54U, 0x00U, 0x12U, 0x34U, 0x57U,
    };
    struct network_ipv4_profile profile;
    struct network_ipv4_config replacement;
    struct network_ipv4_port_policy policy;
    struct network_ipv4_port_policy changed_policy;
    struct virtio_net_mac_identity identity =
        make_mac_identity();
    struct virtio_net_mac_identity changed_identity =
        make_mac_identity_with_mac(other_mac);
    struct network_config_transaction changed_transaction;
    struct network_config_transaction invalid_transaction;

    CHECK(initialize_profile(&profile),
          "observed-state fixture initializes valid profile");
    policy = *network_ipv4_profile_port_policy(&profile);
    CHECK(network_ipv4_profile_validate_state(
              &profile, &profile.config, &identity,
              &policy, 7U) == NETWORK_IPV4_PROFILE_OK,
          "IPv4 profile state validation accepts unchanged state");
    CHECK(network_ipv4_profile_validate_state(
              NULL, &profile.config, &identity, &policy, 7U) ==
              NETWORK_IPV4_PROFILE_NULL,
          "IPv4 profile state validation rejects null profile");
    CHECK(network_ipv4_profile_validate_state(
              &profile, NULL, &identity, &policy, 7U) ==
              NETWORK_IPV4_PROFILE_NULL,
          "IPv4 profile state validation rejects null transaction");
    CHECK(network_ipv4_profile_validate_state(
              &profile, &profile.config, NULL, &policy, 7U) ==
              NETWORK_IPV4_PROFILE_NULL,
          "IPv4 profile state validation rejects null identity");
    CHECK(network_ipv4_profile_validate_state(
              &profile, &profile.config, &identity, NULL, 7U) ==
              NETWORK_IPV4_PROFILE_NULL,
          "IPv4 profile state validation rejects null port policy");

    memset(&invalid_transaction, 0, sizeof(invalid_transaction));
    CHECK(network_ipv4_profile_validate_state(
              &profile, &invalid_transaction, &identity,
              &policy, 7U) == NETWORK_IPV4_PROFILE_INVALID_TRANSACTION,
          "IPv4 profile state validation rejects invalid transaction");
    CHECK(network_ipv4_profile_validate_state(
              &profile, &profile.config, &identity, &policy, 0U) ==
              NETWORK_IPV4_PROFILE_INVALID_GENERATION,
          "IPv4 profile state validation rejects invalid generation");
    CHECK(network_ipv4_profile_validate_state(
              &profile, &profile.config, &identity, &policy, 8U) ==
              NETWORK_IPV4_PROFILE_CHANGED,
          "IPv4 profile state validation rejects changed generation");
    CHECK(network_ipv4_profile_validate_state(
              &profile, &profile.config, &changed_identity,
              &policy, 7U) == NETWORK_IPV4_PROFILE_CHANGED,
          "IPv4 profile state validation rejects changed identity");
    changed_policy = policy;
    changed_policy.port = 8443U;
    CHECK(network_ipv4_profile_validate_state(
              &profile, &profile.config, &identity,
              &changed_policy, 7U) == NETWORK_IPV4_PROFILE_CHANGED,
          "IPv4 profile state validation rejects changed port policy");
    CHECK(parse_config(replacement_config, &replacement),
          "IPv4 profile state fixture parses replacement");
    changed_transaction = profile.config;
    changed_transaction.active = replacement;
    CHECK(network_ipv4_profile_validate_state(
              &profile, &changed_transaction, &identity,
              &policy, 7U) == NETWORK_IPV4_PROFILE_CHANGED,
          "IPv4 profile state validation rejects changed active config");
    CHECK(network_ipv4_profile_validate_state(
              &profile, &profile.config, &identity,
              &policy, 7U) == NETWORK_IPV4_PROFILE_OK,
          "IPv4 profile state fixture leaves profile unchanged");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "IPv4 profile observed-state validation allocates nothing");
}

static void test_accessors(void)
{
    struct network_ipv4_profile profile;
    struct network_ipv4_port_policy policy;
    struct virtio_net_mac_identity identity =
        make_mac_identity();

    CHECK(!network_ipv4_profile_published(NULL) &&
              network_ipv4_profile_active_config(NULL) == NULL &&
              network_ipv4_profile_identity(NULL) == NULL &&
              network_ipv4_profile_port_policy(NULL) == NULL &&
              !network_ipv4_profile_has_pending(NULL) &&
              network_ipv4_profile_pending_config(NULL) == NULL &&
              network_ipv4_profile_pending_identity(NULL) == NULL &&
              network_ipv4_profile_pending_port_policy(NULL) == NULL,
          "IPv4 profile accessors reject null profile");
    poison_profile(&profile);
    CHECK(!network_ipv4_profile_published(&profile) &&
              network_ipv4_profile_active_config(&profile) == NULL &&
              network_ipv4_profile_identity(&profile) == NULL &&
              network_ipv4_profile_port_policy(&profile) == NULL,
          "IPv4 profile accessors reject unpublished profile");

    CHECK(initialize_profile(&profile),
          "accessor fixture initializes valid profile");
    policy = *network_ipv4_profile_port_policy(&profile);
    CHECK(network_ipv4_profile_published(&profile) &&
              network_ipv4_profile_active_config(&profile) ==
                  &profile.config.active &&
              network_ipv4_profile_identity(&profile) ==
                  &profile.identity &&
              network_ipv4_profile_port_policy(&profile) ==
                  &profile.port_policy &&
              !network_ipv4_profile_has_pending(&profile) &&
              network_ipv4_profile_pending_config(&profile) == NULL &&
              network_ipv4_profile_pending_identity(&profile) == NULL &&
              network_ipv4_profile_pending_port_policy(&profile) == NULL,
          "IPv4 profile accessors return exact active state");
    CHECK(network_ipv4_profile_stage(
              &profile, replacement_config,
              sizeof(replacement_config) - 1U,
              &identity, &policy) == NETWORK_IPV4_PROFILE_OK &&
              network_ipv4_profile_has_pending(&profile) &&
              network_ipv4_profile_pending_config(&profile) ==
                  &profile.config.pending &&
              network_ipv4_profile_pending_identity(&profile) ==
                  &profile.pending_identity &&
              network_ipv4_profile_pending_port_policy(&profile) ==
                  &profile.pending_port_policy,
          "IPv4 profile pending accessors return exact pending state");

    profile.published = false;
    CHECK(!network_ipv4_profile_published(&profile) &&
              network_ipv4_profile_active_config(&profile) == NULL &&
              network_ipv4_profile_identity(&profile) == NULL &&
              network_ipv4_profile_port_policy(&profile) == NULL,
          "IPv4 profile accessors reject corrupted profile");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "IPv4 profile accessors allocate nothing");
}

int main(void)
{
    test_status_strings();
    test_qemu_fill();
    test_validation_and_null_arguments();
    test_stage_preserves_active_state();
    test_commit_and_rollback();
    test_observed_state_validation();
    test_accessors();

    if (failures != 0) {
        fprintf(stderr,
                "Static QEMU IPv4 profile host tests: FAIL\n");
        return 1;
    }
    printf("Static QEMU IPv4 profile host tests: ALL OK\n");
    return 0;
}

#else
#error "This host test must be built with HFOC_NETWORK_IPV4_PROFILE_HOST_TEST"
#endif
