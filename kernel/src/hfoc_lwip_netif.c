#include "hfoc_lwip_netif.h"

#include "ipv4_config.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

static bool hfoc_lwip_netif_generation_valid(uint32_t generation)
{
    return generation >= HFOC_LWIP_NETIF_GENERATION_MIN &&
           generation <= HFOC_LWIP_NETIF_GENERATION_MAX;
}

static bool hfoc_lwip_netif_published_state(
    const struct hfoc_lwip_netif *netif)
{
    return netif != NULL &&
           netif->published &&
           netif->version == HFOC_LWIP_NETIF_VERSION &&
           hfoc_lwip_netif_generation_valid(netif->generation);
}

static ip4_addr_t hfoc_lwip_netif_address_to_ip4(
    const struct network_ipv4_address *address)
{
    ip4_addr_t ip4;

    ip4.addr = ((uint32_t)address->octets[0] << 24U) |
               ((uint32_t)address->octets[1] << 16U) |
               ((uint32_t)address->octets[2] << 8U) |
               (uint32_t)address->octets[3];
    ip4.addr = PP_HTONL(ip4.addr);
    return ip4;
}

static ip4_addr_t hfoc_lwip_netif_prefix_netmask(uint8_t prefix_length)
{
    ip4_addr_t mask;

    mask.addr = prefix_length == 0U
        ? 0U
        : PP_HTONL(~0U << (32U - (uint32_t)prefix_length));
    return mask;
}

static enum hfoc_lwip_netif_status hfoc_lwip_netif_validate_common(
    const struct hfoc_lwip_netif *netif,
    uint32_t observed_generation)
{
    if (netif == NULL) {
        return HFOC_LWIP_NETIF_NULL;
    }
    if (!netif->published) {
        return HFOC_LWIP_NETIF_UNPUBLISHED;
    }
    if (netif->version != HFOC_LWIP_NETIF_VERSION) {
        return HFOC_LWIP_NETIF_INVALID_VERSION;
    }
    if (!hfoc_lwip_netif_generation_valid(observed_generation) ||
        observed_generation != netif->generation) {
        return HFOC_LWIP_NETIF_INVALID_GENERATION;
    }
    if (netif->live) {
        return HFOC_LWIP_NETIF_ALREADY_LIVE;
    }
    return HFOC_LWIP_NETIF_OK;
}

static enum hfoc_lwip_netif_status
hfoc_lwip_netif_validate_inputs(
    const struct hfoc_lwip_netif *netif,
    const struct network_ipv4_profile *profile,
    const struct virtio_net_link_state *link_state,
    uint32_t observed_generation)
{
    const struct network_ipv4_config *config;
    const struct virtio_net_mac_identity *identity;
    const struct network_device_selection *device_selection;
    const struct network_link_selection *link_selection;
    enum hfoc_lwip_netif_status status;

    if (profile == NULL || link_state == NULL) {
        return HFOC_LWIP_NETIF_NULL;
    }
    status = hfoc_lwip_netif_validate_common(netif, observed_generation);
    if (status != HFOC_LWIP_NETIF_OK) {
        return status;
    }
    if (!network_ipv4_profile_published(profile)) {
        return HFOC_LWIP_NETIF_PROFILE_UNPUBLISHED;
    }

    config = network_ipv4_profile_active_config(profile);
    if (config != NULL &&
        network_ipv4_config_validate(config) != NETWORK_IPV4_OK) {
        return HFOC_LWIP_NETIF_INVALID_CONFIG;
    }

    identity = network_ipv4_profile_identity(profile);
    if (identity == NULL) {
        return HFOC_LWIP_NETIF_INVALID_PROFILE;
    }
    device_selection = virtio_net_mac_identity_device_selection(identity);
    if (device_selection == NULL ||
        !device_selection->published) {
        return HFOC_LWIP_NETIF_INVALID_PROFILE;
    }
    if (!virtio_net_link_state_published(link_state)) {
        return HFOC_LWIP_NETIF_LINK_UNPUBLISHED;
    }
    link_selection = virtio_net_link_state_selection(link_state);
    if (link_selection == NULL || !link_selection->published) {
        return HFOC_LWIP_NETIF_LINK_UNPUBLISHED;
    }
    if (device_selection->device.device_id !=
        link_selection->link.device_id) {
        return HFOC_LWIP_NETIF_DEVICE_MISMATCH;
    }
    return HFOC_LWIP_NETIF_OK;
}

/*
 * ITEM-389: the backend-neutral counterpart of the input validation above.
 * The published network_backend_binding carries the same device selection and
 * link selection, so every netif rule after this point is shared by both
 * backends.
 */
static enum hfoc_lwip_netif_status
hfoc_lwip_netif_validate_backend_inputs(
    const struct hfoc_lwip_netif *netif,
    const struct network_ipv4_config *config,
    const struct network_backend_binding *binding,
    uint32_t observed_generation)
{
    const struct network_device_selection *device_selection;
    const struct network_link_selection *link_selection;
    enum hfoc_lwip_netif_status status;

    if (binding == NULL) {
        return HFOC_LWIP_NETIF_NULL;
    }
    status = hfoc_lwip_netif_validate_common(netif, observed_generation);
    if (status != HFOC_LWIP_NETIF_OK) {
        return status;
    }
    /*
     * ITEM-416: a NULL configuration is the unaddressed profile - an interface
     * that comes up with no address, which is the state a DHCP acquisition
     * starts from and the state the hardware path binds in.
     */
    if (config != NULL &&
        network_ipv4_config_validate(config) != NETWORK_IPV4_OK) {
        return HFOC_LWIP_NETIF_INVALID_CONFIG;
    }
    device_selection = network_backend_device_selection(binding);
    if (device_selection == NULL || !device_selection->published) {
        return HFOC_LWIP_NETIF_INVALID_PROFILE;
    }
    link_selection = network_backend_link_selection(binding);
    if (link_selection == NULL || !link_selection->published) {
        return HFOC_LWIP_NETIF_LINK_UNPUBLISHED;
    }
    if (device_selection->device.device_id !=
        link_selection->link.device_id) {
        return HFOC_LWIP_NETIF_DEVICE_MISMATCH;
    }
    return HFOC_LWIP_NETIF_OK;
}

static enum hfoc_lwip_netif_status hfoc_lwip_netif_stage_common(
    struct hfoc_lwip_netif *netif,
    const struct network_ipv4_config *config,
    const struct network_device_snapshot *device_snapshot,
    const struct network_link_selection *link_selection)
{
    /*
     * ITEM-416: a NULL configuration is the unaddressed profile - the interface
     * comes up with no address, which is the state a DHCP acquisition starts
     * from.  It used to be impossible to express, and the product borrowed the
     * QEMU fixture's address to fill the gap.
     */
    if (config != NULL) {
        netif->config = *config;
    } else {
        memset(&netif->config, 0, sizeof(netif->config));
    }
    netif->device_snapshot = *device_snapshot;
    netif->link = *link_selection;
    netif->staged = true;
    return HFOC_LWIP_NETIF_OK;
}

static enum hfoc_lwip_netif_status hfoc_lwip_netif_activate_common(
    struct hfoc_lwip_netif *netif,
    const struct network_ipv4_config *config,
    const struct network_device_snapshot *device_snapshot,
    bool path_enabled)
{
    struct netif draft;

    if (!netif->staged) {
        return HFOC_LWIP_NETIF_NOT_STAGED;
    }
    if (!path_enabled) {
        return HFOC_LWIP_NETIF_LINK_NOT_READY;
    }
    if ((config != NULL &&
         memcmp(config, &netif->config, sizeof(*config)) != 0) ||
        memcmp(device_snapshot,
               &netif->device_snapshot,
               sizeof(netif->device_snapshot)) != 0) {
        return HFOC_LWIP_NETIF_STAGE_MISMATCH;
    }

    memset(&draft, 0, sizeof(draft));
    draft.mtu = HFOC_LWIP_NETIF_MTU;
    draft.flags = HFOC_LWIP_NETIF_FLAGS;
    draft.hwaddr_len = NETWORK_DEVICE_MAC_LENGTH;
    memcpy(draft.hwaddr, netif->device_snapshot.mac_address,
           NETWORK_DEVICE_MAC_LENGTH);
    draft.name[0] = HFOC_LWIP_NETIF_NAME0;
    draft.name[1] = HFOC_LWIP_NETIF_NAME1;
    draft.num = HFOC_LWIP_NETIF_NUM;
    /*
     * ITEM-416: an unaddressed activation leaves the address fields zero: the
     * interface is up and reachable at layer two, and the acquisition cycle
     * fills the address in through `hfoc_lwip_netif_set_config` when the server
     * answers.
     */
    if (config != NULL) {
        draft.ip_addr = hfoc_lwip_netif_address_to_ip4(&config->address);
        draft.netmask =
            hfoc_lwip_netif_prefix_netmask(config->prefix_length);
        draft.gw = hfoc_lwip_netif_address_to_ip4(&config->gateway);
    }

    netif->netif = draft;
    netif->staged = false;
    netif->live = true;
    return HFOC_LWIP_NETIF_OK;
}

enum hfoc_lwip_netif_status hfoc_lwip_netif_init(
    struct hfoc_lwip_netif *netif,
    uint32_t generation)
{
    if (netif == NULL) {
        return HFOC_LWIP_NETIF_NULL;
    }
    if (netif->published) {
        return HFOC_LWIP_NETIF_ALREADY_PUBLISHED;
    }
    if (netif->version != 0U) {
        if (netif->version == HFOC_LWIP_NETIF_VERSION) {
            return HFOC_LWIP_NETIF_ALREADY_PUBLISHED;
        }
        return HFOC_LWIP_NETIF_INVALID_VERSION;
    }
    if (!hfoc_lwip_netif_generation_valid(generation)) {
        return HFOC_LWIP_NETIF_INVALID_GENERATION;
    }

    memset(netif, 0, sizeof(*netif));
    netif->published = true;
    netif->version = HFOC_LWIP_NETIF_VERSION;
    netif->generation = generation;
    return HFOC_LWIP_NETIF_OK;
}

enum hfoc_lwip_netif_status hfoc_lwip_netif_stage(
    struct hfoc_lwip_netif *netif,
    const struct network_ipv4_profile *profile,
    const struct virtio_net_link_state *link_state,
    uint32_t observed_generation)
{
    enum hfoc_lwip_netif_status status;
    const struct network_ipv4_config *config;
    const struct virtio_net_mac_identity *identity;
    const struct network_device_selection *device_selection;
    const struct network_link_selection *link_selection;

    status = hfoc_lwip_netif_validate_inputs(
        netif, profile, link_state, observed_generation);
    if (status != HFOC_LWIP_NETIF_OK) {
        return status;
    }

    config = network_ipv4_profile_active_config(profile);
    identity = network_ipv4_profile_identity(profile);
    device_selection = virtio_net_mac_identity_device_selection(identity);
    link_selection = virtio_net_link_state_selection(link_state);

    return hfoc_lwip_netif_stage_common(
        netif, config, &device_selection->device, link_selection);
}

enum hfoc_lwip_netif_status hfoc_lwip_netif_activate(
    struct hfoc_lwip_netif *netif,
    const struct network_ipv4_profile *profile,
    const struct virtio_net_link_state *link_state,
    uint32_t observed_generation)
{
    enum hfoc_lwip_netif_status status;
    const struct network_ipv4_config *config;
    const struct virtio_net_mac_identity *identity;
    const struct network_device_selection *device_selection;

    status = hfoc_lwip_netif_validate_inputs(
        netif, profile, link_state, observed_generation);
    if (status != HFOC_LWIP_NETIF_OK) {
        return status;
    }

    config = network_ipv4_profile_active_config(profile);
    identity = network_ipv4_profile_identity(profile);
    device_selection = virtio_net_mac_identity_device_selection(identity);

    return hfoc_lwip_netif_activate_common(
        netif, config, &device_selection->device,
        virtio_net_link_state_path_enabled(link_state));
}

enum hfoc_lwip_netif_status hfoc_lwip_netif_stage_backend(
    struct hfoc_lwip_netif *netif,
    const struct network_ipv4_config *config,
    const struct network_backend_binding *binding,
    uint32_t observed_generation)
{
    enum hfoc_lwip_netif_status status;
    const struct network_device_selection *device_selection;
    const struct network_link_selection *link_selection;

    status = hfoc_lwip_netif_validate_backend_inputs(
        netif, config, binding, observed_generation);
    if (status != HFOC_LWIP_NETIF_OK) {
        return status;
    }

    device_selection = network_backend_device_selection(binding);
    link_selection = network_backend_link_selection(binding);

    return hfoc_lwip_netif_stage_common(
        netif, config, &device_selection->device, link_selection);
}

enum hfoc_lwip_netif_status hfoc_lwip_netif_activate_backend(
    struct hfoc_lwip_netif *netif,
    const struct network_ipv4_config *config,
    const struct network_backend_binding *binding,
    uint32_t observed_generation)
{
    enum hfoc_lwip_netif_status status;
    const struct network_device_selection *device_selection;

    status = hfoc_lwip_netif_validate_backend_inputs(
        netif, config, binding, observed_generation);
    if (status != HFOC_LWIP_NETIF_OK) {
        return status;
    }

    device_selection = network_backend_device_selection(binding);

    return hfoc_lwip_netif_activate_common(
        netif, config, &device_selection->device,
        network_backend_path_enabled(binding));
}

enum hfoc_lwip_netif_status hfoc_lwip_netif_set_config(
    struct hfoc_lwip_netif *netif,
    const struct network_ipv4_config *config)
{
    if (netif == NULL || config == NULL) {
        return HFOC_LWIP_NETIF_NULL;
    }
    if (!netif->published) {
        return HFOC_LWIP_NETIF_UNPUBLISHED;
    }
    if (netif->version != HFOC_LWIP_NETIF_VERSION) {
        return HFOC_LWIP_NETIF_INVALID_VERSION;
    }
    if (!netif->live) {
        return HFOC_LWIP_NETIF_NOT_LIVE;
    }
    if (network_ipv4_config_validate(config) != NETWORK_IPV4_OK) {
        return HFOC_LWIP_NETIF_INVALID_CONFIG;
    }

    netif->config = *config;
    netif->netif.ip_addr = hfoc_lwip_netif_address_to_ip4(&config->address);
    netif->netif.netmask =
        hfoc_lwip_netif_prefix_netmask(config->prefix_length);
    netif->netif.gw = hfoc_lwip_netif_address_to_ip4(&config->gateway);
    return HFOC_LWIP_NETIF_OK;
}

/*
 * ITEM-416: releasing the address leaves the interface up and **unaddressed**.
 *
 * `set_config` refuses a configuration without an address, and rightly so: it
 * applies an address somebody gave the machine.  Releasing one is the opposite
 * operation - the state a DHCP client starts from and the state a lost lease
 * returns to - and it has no configuration to apply.  The interface stays live
 * so the acquisition cycle can keep using it, and the stored configuration is
 * cleared so nothing downstream reads an address this machine no longer holds.
 */
enum hfoc_lwip_netif_status hfoc_lwip_netif_clear_address(
    struct hfoc_lwip_netif *netif)
{
    if (netif == NULL) {
        return HFOC_LWIP_NETIF_NULL;
    }
    if (!netif->published) {
        return HFOC_LWIP_NETIF_UNPUBLISHED;
    }
    if (netif->version != HFOC_LWIP_NETIF_VERSION) {
        return HFOC_LWIP_NETIF_INVALID_VERSION;
    }
    if (!netif->live) {
        return HFOC_LWIP_NETIF_NOT_LIVE;
    }
    memset(&netif->config, 0, sizeof(netif->config));
    memset(&netif->netif.ip_addr, 0, sizeof(netif->netif.ip_addr));
    memset(&netif->netif.netmask, 0, sizeof(netif->netif.netmask));
    memset(&netif->netif.gw, 0, sizeof(netif->netif.gw));
    return HFOC_LWIP_NETIF_OK;
}

enum hfoc_lwip_netif_status hfoc_lwip_netif_remove(
    struct hfoc_lwip_netif *netif)
{
    if (netif == NULL) {
        return HFOC_LWIP_NETIF_NULL;
    }
    if (!netif->published) {
        return HFOC_LWIP_NETIF_UNPUBLISHED;
    }
    if (netif->version != HFOC_LWIP_NETIF_VERSION) {
        return HFOC_LWIP_NETIF_INVALID_VERSION;
    }

    memset(&netif->netif, 0, sizeof(netif->netif));
    memset(&netif->config, 0, sizeof(netif->config));
    memset(&netif->device_snapshot, 0,
           sizeof(netif->device_snapshot));
    memset(&netif->link, 0, sizeof(netif->link));
    netif->live = false;
    netif->staged = false;
    return HFOC_LWIP_NETIF_OK;
}

enum hfoc_lwip_netif_status hfoc_lwip_netif_shutdown(
    struct hfoc_lwip_netif *netif)
{
    if (netif == NULL) {
        return HFOC_LWIP_NETIF_NULL;
    }
    if (!netif->published) {
        return HFOC_LWIP_NETIF_UNPUBLISHED;
    }
    if (netif->version != HFOC_LWIP_NETIF_VERSION) {
        return HFOC_LWIP_NETIF_INVALID_VERSION;
    }
    if (netif->live) {
        return HFOC_LWIP_NETIF_LIVE;
    }

    memset(netif, 0, sizeof(*netif));
    return HFOC_LWIP_NETIF_OK;
}

bool hfoc_lwip_netif_published(
    const struct hfoc_lwip_netif *netif)
{
    return hfoc_lwip_netif_published_state(netif);
}

bool hfoc_lwip_netif_addressed(
    const struct hfoc_lwip_netif *netif)
{
    size_t index;

    if (!hfoc_lwip_netif_live(netif)) {
        return false;
    }
    for (index = 0U; index < NETWORK_IPV4_ADDRESS_OCTETS; ++index) {
        if (netif->config.address.octets[index] != 0U) {
            return true;
        }
    }
    return false;
}

bool hfoc_lwip_netif_live(
    const struct hfoc_lwip_netif *netif)
{
    return hfoc_lwip_netif_published_state(netif) &&
           netif->live &&
           netif->netif.mtu == HFOC_LWIP_NETIF_MTU &&
           netif->netif.flags == HFOC_LWIP_NETIF_FLAGS &&
           netif->netif.hwaddr_len == NETWORK_DEVICE_MAC_LENGTH &&
           netif->netif.name[0] == HFOC_LWIP_NETIF_NAME0 &&
           netif->netif.name[1] == HFOC_LWIP_NETIF_NAME1 &&
           netif->netif.num == HFOC_LWIP_NETIF_NUM;
}

bool hfoc_lwip_netif_staged(
    const struct hfoc_lwip_netif *netif)
{
    return hfoc_lwip_netif_published_state(netif) &&
           netif->staged &&
           !netif->live;
}

uint32_t hfoc_lwip_netif_generation(
    const struct hfoc_lwip_netif *netif)
{
    return hfoc_lwip_netif_published_state(netif)
        ? netif->generation
        : 0U;
}

const struct network_ipv4_config *hfoc_lwip_netif_config(
    const struct hfoc_lwip_netif *netif)
{
    if (!hfoc_lwip_netif_published_state(netif) ||
        (!netif->staged && !netif->live)) {
        return NULL;
    }
    return &netif->config;
}

const struct netif *hfoc_lwip_netif_lwip_netif(
    const struct hfoc_lwip_netif *netif)
{
    if (!hfoc_lwip_netif_live(netif)) {
        return NULL;
    }
    return &netif->netif;
}

const char *hfoc_lwip_netif_status_string(
    enum hfoc_lwip_netif_status status)
{
    switch (status) {
    case HFOC_LWIP_NETIF_OK:
        return "HFOC_LWIP_NETIF_OK";
    case HFOC_LWIP_NETIF_NULL:
        return "HFOC_LWIP_NETIF_NULL";
    case HFOC_LWIP_NETIF_BAD_STATE:
        return "HFOC_LWIP_NETIF_BAD_STATE";
    case HFOC_LWIP_NETIF_INVALID_VERSION:
        return "HFOC_LWIP_NETIF_INVALID_VERSION";
    case HFOC_LWIP_NETIF_ALREADY_PUBLISHED:
        return "HFOC_LWIP_NETIF_ALREADY_PUBLISHED";
    case HFOC_LWIP_NETIF_UNPUBLISHED:
        return "HFOC_LWIP_NETIF_UNPUBLISHED";
    case HFOC_LWIP_NETIF_INVALID_GENERATION:
        return "HFOC_LWIP_NETIF_INVALID_GENERATION";
    case HFOC_LWIP_NETIF_PROFILE_UNPUBLISHED:
        return "HFOC_LWIP_NETIF_PROFILE_UNPUBLISHED";
    case HFOC_LWIP_NETIF_INVALID_PROFILE:
        return "HFOC_LWIP_NETIF_INVALID_PROFILE";
    case HFOC_LWIP_NETIF_INVALID_CONFIG:
        return "HFOC_LWIP_NETIF_INVALID_CONFIG";
    case HFOC_LWIP_NETIF_LINK_UNPUBLISHED:
        return "HFOC_LWIP_NETIF_LINK_UNPUBLISHED";
    case HFOC_LWIP_NETIF_DEVICE_MISMATCH:
        return "HFOC_LWIP_NETIF_DEVICE_MISMATCH";
    case HFOC_LWIP_NETIF_NOT_STAGED:
        return "HFOC_LWIP_NETIF_NOT_STAGED";
    case HFOC_LWIP_NETIF_LINK_NOT_READY:
        return "HFOC_LWIP_NETIF_LINK_NOT_READY";
    case HFOC_LWIP_NETIF_STAGE_MISMATCH:
        return "HFOC_LWIP_NETIF_STAGE_MISMATCH";
    case HFOC_LWIP_NETIF_ALREADY_LIVE:
        return "HFOC_LWIP_NETIF_ALREADY_LIVE";
    case HFOC_LWIP_NETIF_NOT_LIVE:
        return "HFOC_LWIP_NETIF_NOT_LIVE";
    case HFOC_LWIP_NETIF_LIVE:
        return "HFOC_LWIP_NETIF_LIVE";
    default:
        return NULL;
    }
}
