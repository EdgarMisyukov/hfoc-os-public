#include "appliance_production_network.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#if defined(HFOC_PRODUCTION_LWIP) || \
    defined(HFOC_PRODUCTION_NETWORK_HOST_TEST)
#include "lwip/err.h"
#include "lwip/pbuf.h"
#endif

static bool appliance_production_network_generation_valid(
    uint32_t generation)
{
    return generation >= HFOC_LWIP_NETIF_GENERATION_MIN &&
           generation <= HFOC_LWIP_NETIF_GENERATION_MAX;
}

static void appliance_production_network_reset(
    struct appliance_production_network *network)
{
    memset(&network->device_snapshot, 0,
           sizeof(network->device_snapshot));
    memset(&network->device_selection, 0,
           sizeof(network->device_selection));
    memset(&network->mac_identity, 0, sizeof(network->mac_identity));
    memset(&network->ipv4_profile, 0, sizeof(network->ipv4_profile));
    memset(&network->netif, 0, sizeof(network->netif));
    memset(&network->proto, 0, sizeof(network->proto));
    memset(&network->output, 0, sizeof(network->output));
    network->bound = false;
    network->proto_published = false;
}

#if defined(HFOC_PRODUCTION_LWIP) || \
    defined(HFOC_PRODUCTION_NETWORK_HOST_TEST)
static err_t appliance_production_network_linkoutput(
    void *context,
    struct pbuf *frame)
{
    struct appliance_production_network *network =
        (struct appliance_production_network *)context;
    uint8_t buffer[HFOC_LWIP_PROTO_FRAME_MAX];
    u16_t copied;

    if (network == NULL || frame == NULL ||
        network->output.transmit == NULL) {
        return ERR_VAL;
    }
    if (frame->tot_len == 0U ||
        frame->tot_len > HFOC_LWIP_PROTO_FRAME_MAX) {
        return ERR_VAL;
    }
    copied = pbuf_copy_partial(frame, buffer, sizeof(buffer), 0);
    if (copied == 0U || copied != frame->tot_len) {
        return ERR_VAL;
    }
    if (!network->output.transmit(
            network->output.context, buffer, (size_t)copied)) {
        return ERR_IF;
    }
    return ERR_OK;
}
#endif

enum appliance_production_network_status appliance_production_network_init(
    struct appliance_production_network *network)
{
    if (network == NULL) {
        return APPLIANCE_PRODUCTION_NETWORK_NULL;
    }
    memset(network, 0, sizeof(*network));
    network->initialized = true;
    network->status = APPLIANCE_PRODUCTION_NETWORK_NO_DEVICE;
    network->readiness = APPLIANCE_PRODUCTION_NETWORK_UNAVAILABLE;
    return APPLIANCE_PRODUCTION_NETWORK_OK;
}

enum appliance_production_network_status appliance_production_network_bind(
    struct appliance_production_network *network,
    const struct appliance_production_network_bind_inputs *inputs)
{
    struct network_ipv4_port_policy default_policy;
    const struct virtio_net_link_state *link_state;
    const struct network_device_snapshot *snapshot;
    enum network_device_status device_status;
    enum virtio_net_mac_identity_status mac_status;
    enum network_ipv4_profile_status profile_status;
    enum hfoc_lwip_netif_status netif_status;

    if (network == NULL || inputs == NULL) {
        return APPLIANCE_PRODUCTION_NETWORK_NULL;
    }
    if (!network->initialized) {
        return APPLIANCE_PRODUCTION_NETWORK_BAD_STATE;
    }
    if (network->bound) {
        return APPLIANCE_PRODUCTION_NETWORK_BAD_STATE;
    }
    if (inputs->link_state == NULL ||
        !virtio_net_link_state_published(inputs->link_state) ||
        inputs->profile == NULL ||
        !virtio_net_feature_profile_published(inputs->profile) ||
        inputs->output.transmit == NULL) {
        network->status = APPLIANCE_PRODUCTION_NETWORK_INVALID_INPUT;
        network->readiness = APPLIANCE_PRODUCTION_NETWORK_UNAVAILABLE;
        return network->status;
    }
    if (!appliance_production_network_generation_valid(
            APPLIANCE_PRODUCTION_NETWORK_GENERATION)) {
        return APPLIANCE_PRODUCTION_NETWORK_BAD_STATE;
    }

    appliance_production_network_reset(network);
    network->generation = APPLIANCE_PRODUCTION_NETWORK_GENERATION;
    network->output = inputs->output;
    link_state = inputs->link_state;
    snapshot = &link_state->device_snapshot;
    network->device_snapshot = *snapshot;

    device_status = network_device_snapshot_validate(snapshot);
    if (device_status != NETWORK_DEVICE_OK) {
        network->status = APPLIANCE_PRODUCTION_NETWORK_INVALID_INPUT;
        network->readiness = APPLIANCE_PRODUCTION_NETWORK_UNAVAILABLE;
        return network->status;
    }
    device_status = network_device_selection_init(
        &network->device_selection, snapshot, network->generation);
    if (device_status != NETWORK_DEVICE_OK) {
        network->status = APPLIANCE_PRODUCTION_NETWORK_INVALID_INPUT;
        network->readiness = APPLIANCE_PRODUCTION_NETWORK_UNAVAILABLE;
        return network->status;
    }
    mac_status = virtio_net_mac_identity_bind(
        &network->mac_identity, &network->device_selection,
        inputs->profile, network->generation);
    if (mac_status != VIRTIO_NET_MAC_IDENTITY_OK) {
        network->status = APPLIANCE_PRODUCTION_NETWORK_INVALID_INPUT;
        network->readiness = APPLIANCE_PRODUCTION_NETWORK_UNAVAILABLE;
        return network->status;
    }

    if (inputs->ipv4_config == NULL) {
        /*
         * ITEM-416: a caller with no configuration gets **no address**, not the
         * fixture's.
         *
         * This path used to fill `10.0.2.15/24`, gateway `10.0.2.2`, DNS
         * `10.0.2.3` - the address of QEMU's user-mode network - and the
         * hardware path passes no configuration (`appliance_production_network_
         * x86.c` binds with `ipv4_config = NULL`), so every machine booted on
         * the bench adopted a test address: the acceptance host reported
         * `10.0.2.15` while its own DHCP server was handing out `192.0.2.0/24`
         * leases, the listener published itself on an address nothing routes to,
         * and the panel was unreachable although the network worked.  An
         * unaddressed profile is the honest statement: the interface is up, the
         * machine holds no address, and the addressing mode it was configured
         * for fills one in when its server answers - the
         * `[PRODUCTION-NET-MODE] mode=DHCP` record says which mode that is.
         */
        memset(&default_policy, 0, sizeof(default_policy));
        default_policy.version = NETWORK_IPV4_PORT_POLICY_VERSION;
        default_policy.port = NETWORK_IPV4_PROFILE_QEMU_PORT;
        profile_status = network_ipv4_profile_init_unaddressed(
            &network->ipv4_profile, &network->mac_identity,
            &default_policy, network->generation);
    } else {
        if (inputs->port_policy == NULL) {
            network->status =
                APPLIANCE_PRODUCTION_NETWORK_INVALID_INPUT;
            network->readiness =
                APPLIANCE_PRODUCTION_NETWORK_UNAVAILABLE;
            return network->status;
        }
        profile_status = network_ipv4_profile_init(
            &network->ipv4_profile, inputs->ipv4_config,
            &network->mac_identity, inputs->port_policy,
            network->generation);
    }
    if (profile_status != NETWORK_IPV4_PROFILE_OK) {
        network->status = APPLIANCE_PRODUCTION_NETWORK_INVALID_INPUT;
        network->readiness = APPLIANCE_PRODUCTION_NETWORK_UNAVAILABLE;
        return network->status;
    }

    netif_status = hfoc_lwip_netif_init(&network->netif,
                                        network->generation);
    if (netif_status != HFOC_LWIP_NETIF_OK) {
        network->status = APPLIANCE_PRODUCTION_NETWORK_NETIF_FAILED;
        network->readiness = APPLIANCE_PRODUCTION_NETWORK_UNAVAILABLE;
        return network->status;
    }
    netif_status = hfoc_lwip_netif_stage(
        &network->netif, &network->ipv4_profile, link_state,
        network->generation);
    if (netif_status != HFOC_LWIP_NETIF_OK) {
        network->status = APPLIANCE_PRODUCTION_NETWORK_NETIF_FAILED;
        network->readiness = APPLIANCE_PRODUCTION_NETWORK_UNAVAILABLE;
        return network->status;
    }

    network->bound = true;
    if (!virtio_net_link_state_path_enabled(link_state)) {
        network->status =
            APPLIANCE_PRODUCTION_NETWORK_LINK_NOT_READY;
        network->readiness = APPLIANCE_PRODUCTION_NETWORK_DEGRADED;
        return network->status;
    }

    netif_status = hfoc_lwip_netif_activate(
        &network->netif, &network->ipv4_profile, link_state,
        network->generation);
    if (netif_status != HFOC_LWIP_NETIF_OK) {
        network->status = APPLIANCE_PRODUCTION_NETWORK_NETIF_FAILED;
        network->readiness = APPLIANCE_PRODUCTION_NETWORK_DEGRADED;
        return network->status;
    }

#if defined(HFOC_PRODUCTION_LWIP) || \
    defined(HFOC_PRODUCTION_NETWORK_HOST_TEST)
    {
        struct hfoc_lwip_proto_output output;

        memset(&output, 0, sizeof(output));
        output.transmit = appliance_production_network_linkoutput;
        output.context = network;
        if (hfoc_lwip_proto_init(
                &network->proto, &network->netif, &output,
                network->generation) != HFOC_LWIP_PROTO_OK) {
            network->status = APPLIANCE_PRODUCTION_NETWORK_PROTO_FAILED;
            network->readiness = APPLIANCE_PRODUCTION_NETWORK_DEGRADED;
            return network->status;
        }
        network->proto_published = true;
    }
#endif

    network->status = APPLIANCE_PRODUCTION_NETWORK_OK;
    network->readiness = APPLIANCE_PRODUCTION_NETWORK_READY;
    return network->status;
}

/*
 * ITEM-389 backend-neutral bind.
 *
 * The steps that make the lwIP path live are shared with the virtio bind:
 * validate the device snapshot, publish the device selection, stage and
 * activate the netif against the backend's published link, then publish the
 * protocol composition.  The only difference is the MAC/link source: this
 * path takes them from the backend binding, which has already proved that the
 * MAC it reads from the device's own registers agrees with the published
 * snapshot and that the device's link signal agrees with the link record.
 */
enum appliance_production_network_status
appliance_production_network_bind_backend(
    struct appliance_production_network *network,
    const struct appliance_production_network_backend_inputs *inputs)
{
    const struct network_ipv4_config *config;
    const struct network_backend_binding *binding;
    const struct network_device_snapshot *snapshot;
    enum network_device_status device_status;
    enum hfoc_lwip_netif_status netif_status;

    if (network == NULL || inputs == NULL) {
        return APPLIANCE_PRODUCTION_NETWORK_NULL;
    }
    if (!network->initialized) {
        return APPLIANCE_PRODUCTION_NETWORK_BAD_STATE;
    }
    if (network->bound) {
        return APPLIANCE_PRODUCTION_NETWORK_BAD_STATE;
    }
    if (inputs->binding == NULL ||
        !network_backend_published(inputs->binding) ||
        inputs->output.transmit == NULL) {
        network->status = APPLIANCE_PRODUCTION_NETWORK_INVALID_INPUT;
        network->readiness = APPLIANCE_PRODUCTION_NETWORK_UNAVAILABLE;
        return network->status;
    }
    if (!appliance_production_network_generation_valid(
            APPLIANCE_PRODUCTION_NETWORK_GENERATION)) {
        return APPLIANCE_PRODUCTION_NETWORK_BAD_STATE;
    }

    appliance_production_network_reset(network);
    network->generation = APPLIANCE_PRODUCTION_NETWORK_GENERATION;
    network->output = inputs->output;
    binding = inputs->binding;
    snapshot = &binding->device_snapshot;
    network->device_snapshot = *snapshot;

    device_status = network_device_snapshot_validate(snapshot);
    if (device_status != NETWORK_DEVICE_OK) {
        network->status = APPLIANCE_PRODUCTION_NETWORK_INVALID_INPUT;
        network->readiness = APPLIANCE_PRODUCTION_NETWORK_UNAVAILABLE;
        return network->status;
    }
    device_status = network_device_selection_init(
        &network->device_selection, snapshot, network->generation);
    if (device_status != NETWORK_DEVICE_OK) {
        network->status = APPLIANCE_PRODUCTION_NETWORK_INVALID_INPUT;
        network->readiness = APPLIANCE_PRODUCTION_NETWORK_UNAVAILABLE;
        return network->status;
    }

    if (inputs->ipv4_config == NULL) {
        /*
         * ITEM-416: the backend bind takes the same decision as the ordinary
         * one - a caller with no configuration gets no address, never the
         * fixture's (`10.0.2.15/24`), which is what every bench machine used to
         * adopt on the hardware path.
         */
        config = NULL;
    } else {
        config = inputs->ipv4_config;
    }

    netif_status = hfoc_lwip_netif_init(&network->netif,
                                        network->generation);
    if (netif_status != HFOC_LWIP_NETIF_OK) {
        network->status = APPLIANCE_PRODUCTION_NETWORK_NETIF_FAILED;
        network->readiness = APPLIANCE_PRODUCTION_NETWORK_UNAVAILABLE;
        return network->status;
    }
    netif_status = hfoc_lwip_netif_stage_backend(
        &network->netif, config, binding, network->generation);
    if (netif_status != HFOC_LWIP_NETIF_OK) {
        network->status = APPLIANCE_PRODUCTION_NETWORK_NETIF_FAILED;
        network->readiness = APPLIANCE_PRODUCTION_NETWORK_UNAVAILABLE;
        return network->status;
    }

    network->bound = true;
    if (!network_backend_path_enabled(binding)) {
        network->status =
            APPLIANCE_PRODUCTION_NETWORK_LINK_NOT_READY;
        network->readiness = APPLIANCE_PRODUCTION_NETWORK_DEGRADED;
        return network->status;
    }

    netif_status = hfoc_lwip_netif_activate_backend(
        &network->netif, config, binding, network->generation);
    if (netif_status != HFOC_LWIP_NETIF_OK) {
        network->status = APPLIANCE_PRODUCTION_NETWORK_NETIF_FAILED;
        network->readiness = APPLIANCE_PRODUCTION_NETWORK_DEGRADED;
        return network->status;
    }

#if defined(HFOC_PRODUCTION_LWIP) || \
    defined(HFOC_PRODUCTION_NETWORK_HOST_TEST)
    {
        struct hfoc_lwip_proto_output output;

        memset(&output, 0, sizeof(output));
        output.transmit = appliance_production_network_linkoutput;
        output.context = network;
        if (hfoc_lwip_proto_init(
                &network->proto, &network->netif, &output,
                network->generation) != HFOC_LWIP_PROTO_OK) {
            network->status = APPLIANCE_PRODUCTION_NETWORK_PROTO_FAILED;
            network->readiness = APPLIANCE_PRODUCTION_NETWORK_DEGRADED;
            return network->status;
        }
        network->proto_published = true;
    }
#endif

    network->status = APPLIANCE_PRODUCTION_NETWORK_OK;
    network->readiness = APPLIANCE_PRODUCTION_NETWORK_READY;
    return network->status;
}

enum appliance_production_network_status appliance_production_network_stop(
    struct appliance_production_network *network)
{
    if (network == NULL) {
        return APPLIANCE_PRODUCTION_NETWORK_NULL;
    }
    if (!network->initialized) {
        return APPLIANCE_PRODUCTION_NETWORK_BAD_STATE;
    }
#if defined(HFOC_PRODUCTION_LWIP) || \
    defined(HFOC_PRODUCTION_NETWORK_HOST_TEST)
    if (network->proto_published) {
        (void)hfoc_lwip_proto_shutdown(&network->proto);
    }
#endif
    if (network->bound && network->netif.published) {
        (void)hfoc_lwip_netif_remove(&network->netif);
    }
    appliance_production_network_reset(network);
    network->status = APPLIANCE_PRODUCTION_NETWORK_NO_DEVICE;
    network->readiness = APPLIANCE_PRODUCTION_NETWORK_UNAVAILABLE;
    return APPLIANCE_PRODUCTION_NETWORK_OK;
}

enum appliance_production_network_input_result
appliance_production_network_input(
    struct appliance_production_network *network,
    const uint8_t *frame,
    size_t frame_length,
    uint32_t now_ms)
{
#if defined(HFOC_PRODUCTION_LWIP) || \
    defined(HFOC_PRODUCTION_NETWORK_HOST_TEST)
    enum hfoc_lwip_proto_result result;
#endif

    if (network == NULL || frame == NULL || frame_length == 0U) {
        return APPLIANCE_PRODUCTION_NETWORK_INPUT_UNAVAILABLE;
    }
    if (!network->bound || !network->proto_published ||
        !network->netif.live) {
        return APPLIANCE_PRODUCTION_NETWORK_INPUT_UNAVAILABLE;
    }
#if defined(HFOC_PRODUCTION_LWIP) || \
    defined(HFOC_PRODUCTION_NETWORK_HOST_TEST)
    result = HFOC_LWIP_PROTO_REJECTED;
    if (hfoc_lwip_proto_input(
            &network->proto, frame, frame_length, now_ms,
            &result) != HFOC_LWIP_PROTO_OK) {
        return APPLIANCE_PRODUCTION_NETWORK_INPUT_UNAVAILABLE;
    }
    return result == HFOC_LWIP_PROTO_ACCEPTED
        ? APPLIANCE_PRODUCTION_NETWORK_INPUT_ACCEPTED
        : APPLIANCE_PRODUCTION_NETWORK_INPUT_REJECTED;
#else
    (void)now_ms;
    return APPLIANCE_PRODUCTION_NETWORK_INPUT_UNAVAILABLE;
#endif
}

enum appliance_production_network_readiness
appliance_production_network_readiness(
    const struct appliance_production_network *network)
{
    return network != NULL
        ? network->readiness
        : APPLIANCE_PRODUCTION_NETWORK_UNAVAILABLE;
}

bool appliance_production_network_bound(
    const struct appliance_production_network *network)
{
    return network != NULL && network->initialized && network->bound;
}

bool appliance_production_network_proto_published(
    const struct appliance_production_network *network)
{
    return network != NULL && network->initialized &&
           network->proto_published;
}

bool appliance_production_network_netif_live(
    const struct appliance_production_network *network)
{
    return network != NULL && network->initialized &&
           network->netif.live;
}

bool appliance_production_network_ready(
    const struct appliance_production_network *network)
{
    return appliance_production_network_readiness(network) ==
           APPLIANCE_PRODUCTION_NETWORK_READY;
}

const struct network_device_snapshot *
appliance_production_network_device(
    const struct appliance_production_network *network)
{
    if (network == NULL || !network->bound) {
        return NULL;
    }
    return &network->device_snapshot;
}

const uint8_t *appliance_production_network_mac(
    const struct appliance_production_network *network)
{
    if (network == NULL || !network->bound) {
        return NULL;
    }
    return network->device_snapshot.mac_address;
}

const char *appliance_production_network_readiness_string(
    enum appliance_production_network_readiness readiness)
{
    switch (readiness) {
    case APPLIANCE_PRODUCTION_NETWORK_UNAVAILABLE:
        return "UNAVAILABLE";
    case APPLIANCE_PRODUCTION_NETWORK_DEGRADED:
        return "DEGRADED";
    case APPLIANCE_PRODUCTION_NETWORK_READY:
        return "READY";
    default:
        return NULL;
    }
}

const char *appliance_production_network_status_string(
    enum appliance_production_network_status status)
{
    switch (status) {
    case APPLIANCE_PRODUCTION_NETWORK_OK:
        return "OK";
    case APPLIANCE_PRODUCTION_NETWORK_NULL:
        return "NULL";
    case APPLIANCE_PRODUCTION_NETWORK_BAD_STATE:
        return "BAD_STATE";
    case APPLIANCE_PRODUCTION_NETWORK_NO_DEVICE:
        return "NO_DEVICE";
    case APPLIANCE_PRODUCTION_NETWORK_INVALID_INPUT:
        return "INVALID_INPUT";
    case APPLIANCE_PRODUCTION_NETWORK_LINK_NOT_READY:
        return "LINK_NOT_READY";
    case APPLIANCE_PRODUCTION_NETWORK_NETIF_FAILED:
        return "NETIF_FAILED";
    case APPLIANCE_PRODUCTION_NETWORK_PROTO_FAILED:
        return "PROTO_FAILED";
    default:
        return NULL;
    }
}
