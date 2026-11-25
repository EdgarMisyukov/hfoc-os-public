#include "hfoc_lwip_netif.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifdef HFOC_LWIP_NETIF_HOST_TEST

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

struct test_io_state {
    bool read_result;
    uint32_t status;
    unsigned int calls;
};

static struct test_io_state active_io;

static bool test_read_config_status(void *context, uint32_t *status)
{
    struct test_io_state *state = (struct test_io_state *)context;

    ++state->calls;
    if (!state->read_result) {
        return false;
    }
    *status = state->status;
    return true;
}

static void copy_mac(uint8_t destination[6], const uint8_t source[6])
{
    memcpy(destination, source, 6U);
}

static struct network_device_snapshot make_device_snapshot(
    uint32_t device_id,
    uint32_t capability_mask)
{
    struct network_device_snapshot snapshot;

    memset(&snapshot, 0, sizeof(snapshot));
    snapshot.version = NETWORK_DEVICE_SNAPSHOT_VERSION;
    snapshot.device_id = device_id;
    snapshot.vendor_id = VIRTIO_PCI_VENDOR_ID;
    snapshot.product_id = VIRTIO_PCI_DEVICE_ID_NET_MODERN;
    snapshot.bus_address = 0x0010U;
    snapshot.capability_mask = capability_mask;
    snapshot.mac_address[0] = 0x52U;
    snapshot.mac_address[1] = 0x54U;
    snapshot.mac_address[2] = 0x00U;
    snapshot.mac_address[3] = 0x12U;
    snapshot.mac_address[4] = 0x34U;
    snapshot.mac_address[5] = 0x56U;
    return snapshot;
}

static struct network_device_selection make_device_selection(
    const struct network_device_snapshot *snapshot)
{
    struct network_device_selection selection;

    memset(&selection, 0, sizeof(selection));
    CHECK(network_device_selection_init(
              &selection, snapshot, 1U) == NETWORK_DEVICE_OK,
          "netif fixture publishes device selection");
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

static struct virtio_net_feature_profile make_feature_profile(
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
          "netif fixture publishes feature profile");
    return profile;
}

static struct virtio_net_mac_identity make_mac_identity(
    const struct network_device_selection *device)
{
    struct virtio_net_mac_identity binding;
    struct virtio_net_feature_profile profile =
        make_feature_profile(device->device.mac_address);

    memset(&binding, 0, sizeof(binding));
    CHECK(virtio_net_mac_identity_bind(
              &binding, device, &profile, 3U) ==
              VIRTIO_NET_MAC_IDENTITY_OK,
          "netif fixture publishes MAC identity");
    return binding;
}

static struct network_ipv4_profile make_profile(
    const struct network_device_selection *device)
{
    struct network_ipv4_profile profile;
    struct network_ipv4_config config;
    struct network_ipv4_port_policy policy;
    struct virtio_net_mac_identity identity =
        make_mac_identity(device);

    CHECK(network_ipv4_profile_qemu_fill(
              &config, &policy) == NETWORK_IPV4_PROFILE_OK,
          "netif fixture fills QEMU IPv4 profile");
    memset(&profile, 0, sizeof(profile));
    CHECK(network_ipv4_profile_init(
              &profile, &config, &identity, &policy, 7U) ==
              NETWORK_IPV4_PROFILE_OK,
          "netif fixture publishes IPv4 profile");
    return profile;
}

static struct network_link_state_record make_link_record(
    uint32_t device_id,
    enum network_link_state state)
{
    struct network_link_state_record record;

    memset(&record, 0, sizeof(record));
    record.version = NETWORK_LINK_STATE_VERSION;
    record.device_id = device_id;
    record.state = state;
    record.generation = 5U;
    return record;
}

static struct virtio_net_link_readiness make_readiness(bool ready)
{
    struct virtio_net_link_readiness readiness;

    memset(&readiness, 0, sizeof(readiness));
    readiness.profile_published = ready;
    readiness.queue_composition_published = ready;
    readiness.interrupt_published = ready;
    return readiness;
}

static struct virtio_net_link_state make_link_state(
    const struct network_device_snapshot *device,
    enum network_link_state state)
{
    struct virtio_net_link_state binding;
    struct virtio_net_link_readiness readiness =
        make_readiness(true);
    struct virtio_net_link_io io_seam;
    struct network_link_state_record record =
        make_link_record(device->device_id, state);

    memset(&binding, 0, sizeof(binding));
    memset(&active_io, 0, sizeof(active_io));
    active_io.read_result = true;
    active_io.status =
        state == NETWORK_LINK_UP ? VIRTIO_NET_STATUS_LINK_UP : 0U;
    io_seam.read_config_status = test_read_config_status;
    io_seam.context = &active_io;
    CHECK(virtio_net_link_state_activate(
              &binding, device, &readiness, &io_seam, &record, 3U,
              7U) == VIRTIO_NET_LINK_OK,
          "netif fixture publishes link state");
    return binding;
}

static bool link_to_up(struct virtio_net_link_state *link)
{
    struct virtio_net_link_readiness readiness =
        make_readiness(true);
    struct network_link_state_record record =
        make_link_record(link->link.link.device_id, NETWORK_LINK_UP);

    active_io.read_result = true;
    active_io.status = VIRTIO_NET_STATUS_LINK_UP;
    return virtio_net_link_state_transition(
               link, &readiness, &record, 3U) ==
           VIRTIO_NET_LINK_OK;
}

static void poison_netif(struct hfoc_lwip_netif *netif)
{
    memset(netif, 0xA5, sizeof(*netif));
    netif->published = false;
    netif->version = 0U;
}

static bool netif_unchanged(
    const struct hfoc_lwip_netif *netif,
    const struct hfoc_lwip_netif *original)
{
    return memcmp(netif, original, sizeof(*netif)) == 0;
}

static bool netif_has_no_live_state(
    const struct hfoc_lwip_netif *netif)
{
    const struct netif *lwip = hfoc_lwip_netif_lwip_netif(netif);

    return !hfoc_lwip_netif_live(netif) &&
           lwip == NULL &&
           netif->netif.mtu == 0U &&
           netif->netif.flags == 0U &&
           netif->netif.hwaddr_len == 0U;
}

static void test_status_strings(void)
{
    static const struct {
        enum hfoc_lwip_netif_status status;
        const char *string;
    } cases[] = {
        {HFOC_LWIP_NETIF_OK, "HFOC_LWIP_NETIF_OK"},
        {HFOC_LWIP_NETIF_NULL, "HFOC_LWIP_NETIF_NULL"},
        {HFOC_LWIP_NETIF_BAD_STATE, "HFOC_LWIP_NETIF_BAD_STATE"},
        {HFOC_LWIP_NETIF_INVALID_VERSION,
         "HFOC_LWIP_NETIF_INVALID_VERSION"},
        {HFOC_LWIP_NETIF_ALREADY_PUBLISHED,
         "HFOC_LWIP_NETIF_ALREADY_PUBLISHED"},
        {HFOC_LWIP_NETIF_UNPUBLISHED,
         "HFOC_LWIP_NETIF_UNPUBLISHED"},
        {HFOC_LWIP_NETIF_INVALID_GENERATION,
         "HFOC_LWIP_NETIF_INVALID_GENERATION"},
        {HFOC_LWIP_NETIF_PROFILE_UNPUBLISHED,
         "HFOC_LWIP_NETIF_PROFILE_UNPUBLISHED"},
        {HFOC_LWIP_NETIF_INVALID_PROFILE,
         "HFOC_LWIP_NETIF_INVALID_PROFILE"},
        {HFOC_LWIP_NETIF_INVALID_CONFIG,
         "HFOC_LWIP_NETIF_INVALID_CONFIG"},
        {HFOC_LWIP_NETIF_LINK_UNPUBLISHED,
         "HFOC_LWIP_NETIF_LINK_UNPUBLISHED"},
        {HFOC_LWIP_NETIF_DEVICE_MISMATCH,
         "HFOC_LWIP_NETIF_DEVICE_MISMATCH"},
        {HFOC_LWIP_NETIF_NOT_STAGED,
         "HFOC_LWIP_NETIF_NOT_STAGED"},
        {HFOC_LWIP_NETIF_LINK_NOT_READY,
         "HFOC_LWIP_NETIF_LINK_NOT_READY"},
        {HFOC_LWIP_NETIF_STAGE_MISMATCH,
         "HFOC_LWIP_NETIF_STAGE_MISMATCH"},
        {HFOC_LWIP_NETIF_ALREADY_LIVE,
         "HFOC_LWIP_NETIF_ALREADY_LIVE"},
        {HFOC_LWIP_NETIF_NOT_LIVE,
         "HFOC_LWIP_NETIF_NOT_LIVE"},
        {HFOC_LWIP_NETIF_LIVE, "HFOC_LWIP_NETIF_LIVE"},
    };
    size_t index;

    for (index = 0U;
         index < sizeof(cases) / sizeof(cases[0]);
         ++index) {
        const char *string = hfoc_lwip_netif_status_string(
            cases[index].status);

        CHECK(string != NULL &&
                  strcmp(string, cases[index].string) == 0,
              "netif status string is exact");
    }
    CHECK(hfoc_lwip_netif_status_string(
              (enum hfoc_lwip_netif_status)18U) == NULL,
          "netif undefined status is rejected");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "netif status handling allocates nothing");
}

static void test_init_validation(void)
{
    struct hfoc_lwip_netif netif;
    struct hfoc_lwip_netif original;

    CHECK(hfoc_lwip_netif_init(NULL, 1U) == HFOC_LWIP_NETIF_NULL,
          "netif init rejects null object");

    poison_netif(&netif);
    memcpy(&original, &netif, sizeof(netif));
    CHECK(hfoc_lwip_netif_init(
              &netif, 0U) == HFOC_LWIP_NETIF_INVALID_GENERATION &&
              netif_unchanged(&netif, &original),
          "netif init rejects zero generation");
    CHECK(hfoc_lwip_netif_init(
              &netif,
              HFOC_LWIP_NETIF_GENERATION_MAX + 1U) ==
              HFOC_LWIP_NETIF_INVALID_GENERATION &&
              netif_unchanged(&netif, &original),
          "netif init rejects overflow generation");

    poison_netif(&netif);
    CHECK(hfoc_lwip_netif_init(
              &netif, 1U) == HFOC_LWIP_NETIF_OK &&
              hfoc_lwip_netif_published(&netif) &&
              !hfoc_lwip_netif_live(&netif) &&
              !hfoc_lwip_netif_staged(&netif) &&
              hfoc_lwip_netif_generation(&netif) == 1U &&
              hfoc_lwip_netif_config(&netif) == NULL &&
              hfoc_lwip_netif_lwip_netif(&netif) == NULL,
          "netif init publishes exact empty lifecycle");
    memcpy(&original, &netif, sizeof(netif));
    CHECK(hfoc_lwip_netif_init(
              &netif, 1U) == HFOC_LWIP_NETIF_ALREADY_PUBLISHED &&
              netif_unchanged(&netif, &original),
          "netif init rejects duplicate publication");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "netif init allocates nothing");
}

static void test_stage_validation(void)
{
    struct network_device_snapshot device =
        make_device_snapshot(NETWORK_DEVICE_ID_MIN,
                             VIRTIO_NET_LINK_CAPABILITY_MASK);
    struct network_device_selection selection =
        make_device_selection(&device);
    struct network_ipv4_profile profile = make_profile(&selection);
    struct network_ipv4_profile unpublished_profile = profile;
    struct virtio_net_link_state link =
        make_link_state(&device, NETWORK_LINK_DOWN);
    struct virtio_net_link_state unpublished_link = link;
    struct hfoc_lwip_netif netif;
    struct hfoc_lwip_netif original;

    poison_netif(&netif);
    CHECK(hfoc_lwip_netif_init(
              &netif, 1U) == HFOC_LWIP_NETIF_OK,
          "netif stage fixture initializes lifecycle");
    memcpy(&original, &netif, sizeof(netif));

    CHECK(hfoc_lwip_netif_stage(
              NULL, &profile, &link, 1U) == HFOC_LWIP_NETIF_NULL,
          "netif stage rejects null lifecycle");
    CHECK(hfoc_lwip_netif_stage(
              &netif, NULL, &link, 1U) == HFOC_LWIP_NETIF_NULL &&
              netif_unchanged(&netif, &original),
          "netif stage rejects null profile");
    CHECK(hfoc_lwip_netif_stage(
              &netif, &profile, NULL, 1U) == HFOC_LWIP_NETIF_NULL &&
              netif_unchanged(&netif, &original),
          "netif stage rejects null link state");
    CHECK(hfoc_lwip_netif_stage(
              &netif, &profile, &link, 2U) ==
              HFOC_LWIP_NETIF_INVALID_GENERATION &&
              netif_unchanged(&netif, &original),
          "netif stage rejects observed generation mismatch");

    unpublished_profile.published = false;
    CHECK(hfoc_lwip_netif_stage(
              &netif, &unpublished_profile, &link, 1U) ==
              HFOC_LWIP_NETIF_PROFILE_UNPUBLISHED &&
              netif_unchanged(&netif, &original),
          "netif stage rejects unpublished profile");

    unpublished_link.published = false;
    CHECK(hfoc_lwip_netif_stage(
              &netif, &profile, &unpublished_link, 1U) ==
              HFOC_LWIP_NETIF_LINK_UNPUBLISHED &&
              netif_unchanged(&netif, &original),
          "netif stage rejects unpublished link state");

    {
        struct network_device_snapshot other_device =
            make_device_snapshot(2U, VIRTIO_NET_LINK_CAPABILITY_MASK);
        struct virtio_net_link_state other_link =
            make_link_state(&other_device, NETWORK_LINK_DOWN);

        CHECK(hfoc_lwip_netif_stage(
                  &netif, &profile, &other_link, 1U) ==
                  HFOC_LWIP_NETIF_DEVICE_MISMATCH &&
                  netif_unchanged(&netif, &original),
              "netif stage rejects link/device mismatch");
    }

    CHECK(hfoc_lwip_netif_stage(
              &netif, &profile, &link, 1U) == HFOC_LWIP_NETIF_OK &&
              hfoc_lwip_netif_staged(&netif) &&
              !hfoc_lwip_netif_live(&netif) &&
              hfoc_lwip_netif_config(&netif) != NULL &&
              memcmp(hfoc_lwip_netif_config(&netif),
                     &profile.config.active,
                     sizeof(profile.config.active)) == 0 &&
              netif_has_no_live_state(&netif),
          "netif stage succeeds before link readiness");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "netif stage allocates nothing");
}

static void test_activate_gating(void)
{
    struct network_device_snapshot device =
        make_device_snapshot(NETWORK_DEVICE_ID_MIN,
                             VIRTIO_NET_LINK_CAPABILITY_MASK);
    struct network_device_selection selection =
        make_device_selection(&device);
    struct network_ipv4_profile profile = make_profile(&selection);
    struct virtio_net_link_state link =
        make_link_state(&device, NETWORK_LINK_DOWN);
    struct hfoc_lwip_netif netif;
    const struct netif *published;
    ip4_addr_t expected_ip;
    ip4_addr_t expected_mask;
    ip4_addr_t expected_gateway;

    poison_netif(&netif);
    CHECK(hfoc_lwip_netif_init(
              &netif, 1U) == HFOC_LWIP_NETIF_OK,
          "netif activate fixture initializes lifecycle");
    CHECK(hfoc_lwip_netif_activate(
              &netif, &profile, &link, 1U) ==
              HFOC_LWIP_NETIF_NOT_STAGED &&
              !hfoc_lwip_netif_live(&netif) &&
              netif_has_no_live_state(&netif),
          "netif activate without stage is rejected");

    CHECK(hfoc_lwip_netif_stage(
              &netif, &profile, &link, 1U) == HFOC_LWIP_NETIF_OK,
          "netif activate fixture stages down link");
    CHECK(hfoc_lwip_netif_activate(
              &netif, &profile, &link, 1U) ==
              HFOC_LWIP_NETIF_LINK_NOT_READY &&
              !hfoc_lwip_netif_live(&netif) &&
              hfoc_lwip_netif_staged(&netif) &&
              netif_has_no_live_state(&netif),
          "netif activate before link readiness is rejected");

    CHECK(link_to_up(&link),
          "netif fixture transitions link to ready");
    CHECK(hfoc_lwip_netif_activate(
              &netif, &profile, &link, 1U) == HFOC_LWIP_NETIF_OK &&
              hfoc_lwip_netif_live(&netif) &&
              !hfoc_lwip_netif_staged(&netif) &&
              hfoc_lwip_netif_generation(&netif) == 1U,
          "netif activate after link readiness publishes live netif");

    published = hfoc_lwip_netif_lwip_netif(&netif);
    IP4_ADDR(&expected_ip, 10U, 0U, 2U, 15U);
    IP4_ADDR(&expected_mask, 255U, 255U, 255U, 0U);
    IP4_ADDR(&expected_gateway, 10U, 0U, 2U, 2U);
    CHECK(published != NULL &&
              published->mtu == HFOC_LWIP_NETIF_MTU &&
              published->flags == HFOC_LWIP_NETIF_FLAGS &&
              published->hwaddr_len == NETWORK_DEVICE_MAC_LENGTH &&
              memcmp(published->hwaddr, device.mac_address,
                     NETWORK_DEVICE_MAC_LENGTH) == 0 &&
              published->name[0] == HFOC_LWIP_NETIF_NAME0 &&
              published->name[1] == HFOC_LWIP_NETIF_NAME1 &&
              published->num == HFOC_LWIP_NETIF_NUM &&
              published->ip_addr.addr == expected_ip.addr &&
              published->netmask.addr == expected_mask.addr &&
              published->gw.addr == expected_gateway.addr &&
              published->input == NULL &&
              published->output == NULL &&
              published->linkoutput == NULL,
          "netif activate publishes exact lwIP netif fields");

    CHECK(hfoc_lwip_netif_activate(
              &netif, &profile, &link, 1U) ==
              HFOC_LWIP_NETIF_ALREADY_LIVE &&
              hfoc_lwip_netif_live(&netif),
          "netif activate rejects duplicate live publication");
    /* ITEM-416: the positive half of the same predicate. */
    CHECK(hfoc_lwip_netif_addressed(&netif),
          "a live interface with an address is addressed");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "netif activate allocates nothing");
}

static void test_set_config(void)
{
    struct network_device_snapshot device =
        make_device_snapshot(NETWORK_DEVICE_ID_MIN,
                             VIRTIO_NET_LINK_CAPABILITY_MASK);
    struct network_device_selection selection =
        make_device_selection(&device);
    struct network_ipv4_profile profile = make_profile(&selection);
    struct virtio_net_link_state link =
        make_link_state(&device, NETWORK_LINK_DOWN);
    struct hfoc_lwip_netif netif;
    struct network_ipv4_config lease;
    struct network_ipv4_config invalid;
    const struct netif *published;
    const struct network_ipv4_config *current;
    ip4_addr_t expected_ip;
    ip4_addr_t expected_mask;
    ip4_addr_t expected_gateway;

    poison_netif(&netif);
    memset(&lease, 0, sizeof(lease));
    lease.schema_version = NETWORK_IPV4_CONFIG_VERSION;
    lease.address.octets[0] = 192U;
    lease.address.octets[1] = 168U;
    lease.address.octets[2] = 1U;
    lease.address.octets[3] = 50U;
    lease.prefix_length = 24U;
    lease.gateway.octets[0] = 192U;
    lease.gateway.octets[1] = 168U;
    lease.gateway.octets[2] = 1U;
    lease.gateway.octets[3] = 1U;
    lease.dns.octets[0] = 192U;
    lease.dns.octets[1] = 168U;
    lease.dns.octets[2] = 1U;
    lease.dns.octets[3] = 1U;
    invalid = lease;
    invalid.schema_version = 0U;

    CHECK(hfoc_lwip_netif_set_config(NULL, &lease) ==
              HFOC_LWIP_NETIF_NULL,
          "netif set config rejects a null netif");
    CHECK(hfoc_lwip_netif_init(
              &netif, 1U) == HFOC_LWIP_NETIF_OK,
          "netif set config fixture initializes lifecycle");
    CHECK(hfoc_lwip_netif_set_config(&netif, &lease) ==
              HFOC_LWIP_NETIF_NOT_LIVE,
          "netif set config rejects a non-live netif");
    CHECK(hfoc_lwip_netif_stage(
              &netif, &profile, &link, 1U) == HFOC_LWIP_NETIF_OK &&
              link_to_up(&link) &&
              hfoc_lwip_netif_activate(
                  &netif, &profile, &link, 1U) == HFOC_LWIP_NETIF_OK,
          "netif set config fixture publishes a live netif");
    CHECK(hfoc_lwip_netif_set_config(&netif, &invalid) ==
              HFOC_LWIP_NETIF_INVALID_CONFIG,
          "netif set config rejects an invalid configuration");
    CHECK(hfoc_lwip_netif_set_config(&netif, &lease) ==
              HFOC_LWIP_NETIF_OK,
          "netif set config accepts a validated lease");
    current = hfoc_lwip_netif_config(&netif);
    published = hfoc_lwip_netif_lwip_netif(&netif);
    IP4_ADDR(&expected_ip, 192U, 168U, 1U, 50U);
    IP4_ADDR(&expected_mask, 255U, 255U, 255U, 0U);
    IP4_ADDR(&expected_gateway, 192U, 168U, 1U, 1U);
    CHECK(current != NULL &&
              memcmp(current, &lease, sizeof(lease)) == 0,
          "netif set config republishes the stored configuration");
    CHECK(published != NULL &&
              published->ip_addr.addr == expected_ip.addr &&
              published->netmask.addr == expected_mask.addr &&
              published->gw.addr == expected_gateway.addr,
          "netif set config updates the live lwIP address fields");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "netif set config allocates nothing");
}

/*
 * ITEM-416: releasing an address leaves the interface up and unaddressed.
 *
 * The product used to "release" a lease by installing the QEMU fixture address
 * (`10.0.2.15/24`) - the acceptance host published its listener on it while its
 * own network was handing out `192.0.2.0/24` leases - and a configuration
 * without an address is refused by `set_config`, so there was no operation that
 * meant "no address".  This is that operation: the interface stays live, the
 * stored configuration is cleared, the lwIP address fields go to zero, and
 * nothing is allocated or freed while it happens.
 */
static void test_clear_address(void)
{
    struct network_device_snapshot device =
        make_device_snapshot(NETWORK_DEVICE_ID_MIN,
                             VIRTIO_NET_LINK_CAPABILITY_MASK);
    struct network_device_selection selection =
        make_device_selection(&device);
    struct network_ipv4_profile profile = make_profile(&selection);
    struct virtio_net_link_state link =
        make_link_state(&device, NETWORK_LINK_DOWN);
    struct hfoc_lwip_netif netif;
    struct network_ipv4_config lease;
    struct network_ipv4_config cleared;
    const struct netif *published;
    const struct network_ipv4_config *current;

    poison_netif(&netif);
    memset(&lease, 0, sizeof(lease));
    lease.schema_version = NETWORK_IPV4_CONFIG_VERSION;
    lease.address.octets[0] = 192U;
    lease.address.octets[1] = 168U;
    lease.address.octets[2] = 1U;
    lease.address.octets[3] = 50U;
    lease.prefix_length = 24U;
    lease.gateway.octets[0] = 192U;
    lease.gateway.octets[1] = 168U;
    lease.gateway.octets[2] = 1U;
    lease.gateway.octets[3] = 1U;
    lease.dns = lease.gateway;
    memset(&cleared, 0, sizeof(cleared));

    CHECK(hfoc_lwip_netif_clear_address(NULL) ==
              HFOC_LWIP_NETIF_NULL,
          "clearing an address needs a netif");
    CHECK(hfoc_lwip_netif_init(&netif, 1U) == HFOC_LWIP_NETIF_OK &&
              hfoc_lwip_netif_clear_address(&netif) ==
                  HFOC_LWIP_NETIF_NOT_LIVE,
          "a netif that is not live cannot release an address");
    CHECK(hfoc_lwip_netif_stage(&netif, &profile, &link, 1U) ==
              HFOC_LWIP_NETIF_OK &&
              link_to_up(&link) &&
              hfoc_lwip_netif_activate(
                  &netif, &profile, &link, 1U) == HFOC_LWIP_NETIF_OK &&
              hfoc_lwip_netif_set_config(&netif, &lease) ==
                  HFOC_LWIP_NETIF_OK,
          "the clear-address fixture holds a lease");
    CHECK(hfoc_lwip_netif_clear_address(&netif) ==
              HFOC_LWIP_NETIF_OK,
          "a live netif releases its address");
    current = hfoc_lwip_netif_config(&netif);
    published = hfoc_lwip_netif_lwip_netif(&netif);
    CHECK(current != NULL &&
              memcmp(current, &cleared, sizeof(cleared)) == 0,
          "the released configuration is the zero configuration");
    CHECK(published != NULL && published->ip_addr.addr == 0U &&
              published->netmask.addr == 0U &&
              published->gw.addr == 0U,
          "the lwIP address fields are cleared with it");
    /*
     * ITEM-416: live and unaddressed is its own state, and the caller that
     * needs an address to bind a listener has to be able to see it.
     */
    CHECK(!hfoc_lwip_netif_addressed(&netif),
          "a released address leaves the interface live but unaddressed");
    CHECK(!hfoc_lwip_netif_addressed(NULL),
          "an absent interface is not addressed");
    CHECK(hfoc_lwip_netif_live(&netif),
          "and the interface stays up: acquisition continues on it");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "clearing an address allocates nothing");
}

static void test_stage_mismatch_and_stop(void)
{
    struct network_device_snapshot device =
        make_device_snapshot(NETWORK_DEVICE_ID_MIN,
                             VIRTIO_NET_LINK_CAPABILITY_MASK);
    struct network_device_selection selection =
        make_device_selection(&device);
    struct network_ipv4_profile profile = make_profile(&selection);
    struct virtio_net_link_state link =
        make_link_state(&device, NETWORK_LINK_DOWN);
    struct hfoc_lwip_netif netif;

    poison_netif(&netif);
    CHECK(hfoc_lwip_netif_init(
              &netif, 1U) == HFOC_LWIP_NETIF_OK,
          "netif mismatch fixture initializes lifecycle");
    CHECK(hfoc_lwip_netif_stage(
              &netif, &profile, &link, 1U) == HFOC_LWIP_NETIF_OK,
          "netif mismatch fixture stages link");
    CHECK(link_to_up(&link),
          "netif fixture transitions mismatch link to ready");

    profile.config.active.address.octets[0] = 192U;
    profile.config.active.address.octets[1] = 0U;
    profile.config.active.address.octets[2] = 2U;
    profile.config.active.address.octets[3] = 11U;
    profile.config.active.gateway.octets[0] = 192U;
    profile.config.active.gateway.octets[1] = 0U;
    profile.config.active.gateway.octets[2] = 2U;
    profile.config.active.gateway.octets[3] = 1U;
    profile.config.active.dns.octets[0] = 192U;
    profile.config.active.dns.octets[1] = 0U;
    profile.config.active.dns.octets[2] = 2U;
    profile.config.active.dns.octets[3] = 53U;

    CHECK(hfoc_lwip_netif_activate(
              &netif, &profile, &link, 1U) ==
              HFOC_LWIP_NETIF_STAGE_MISMATCH &&
              !hfoc_lwip_netif_live(&netif) &&
              netif_has_no_live_state(&netif),
          "netif activate rejects staged profile mismatch");

    CHECK(hfoc_lwip_netif_remove(
              &netif) == HFOC_LWIP_NETIF_OK &&
              !hfoc_lwip_netif_live(&netif) &&
              !hfoc_lwip_netif_staged(&netif) &&
              netif_has_no_live_state(&netif),
          "netif remove atomically clears staged state");
    CHECK(hfoc_lwip_netif_remove(
              &netif) == HFOC_LWIP_NETIF_OK,
          "netif remove is idempotent");
    CHECK(hfoc_lwip_netif_shutdown(
              &netif) == HFOC_LWIP_NETIF_OK &&
              !hfoc_lwip_netif_published(&netif),
          "netif shutdown succeeds after remove");
    CHECK(hfoc_lwip_netif_shutdown(
              &netif) == HFOC_LWIP_NETIF_UNPUBLISHED,
          "netif shutdown rejects unpublished object");

    poison_netif(&netif);
    CHECK(hfoc_lwip_netif_init(
              &netif, 1U) == HFOC_LWIP_NETIF_OK,
          "netif stop fixture initializes lifecycle");
    CHECK(hfoc_lwip_netif_stage(
              &netif, &profile, &link, 1U) == HFOC_LWIP_NETIF_OK,
          "netif stop fixture stages link");
    CHECK(hfoc_lwip_netif_activate(
              &netif, &profile, &link, 1U) ==
              HFOC_LWIP_NETIF_OK &&
              hfoc_lwip_netif_live(&netif),
          "netif fixture reactivates changed profile");
    CHECK(hfoc_lwip_netif_shutdown(
              &netif) == HFOC_LWIP_NETIF_LIVE &&
              hfoc_lwip_netif_live(&netif),
          "netif shutdown rejects live object");
    CHECK(hfoc_lwip_netif_remove(
              &netif) == HFOC_LWIP_NETIF_OK &&
              hfoc_lwip_netif_shutdown(&netif) ==
                  HFOC_LWIP_NETIF_OK,
          "netif remove then shutdown succeeds");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "netif stop paths allocate nothing");
}

int main(void)
{
    test_status_strings();
    test_init_validation();
    test_stage_validation();
    test_activate_gating();
    test_set_config();
    test_clear_address();
    test_stage_mismatch_and_stop();

    if (failures != 0) {
        fprintf(stderr, "lwIP netif lifecycle host tests: %d FAIL\n",
                failures);
        return 1;
    }
    printf("lwIP netif lifecycle host tests: ALL OK\n");
    return 0;
}

#else
int main(void)
{
    return 0;
}
#endif
