#ifndef HFOC_APPLIANCE_PRODUCTION_NETWORK_H
#define HFOC_APPLIANCE_PRODUCTION_NETWORK_H

#include "hfoc_lwip_netif.h"
#include "hfoc_lwip_proto.h"
#include "network_backend.h"
#include "network_device.h"
#include "network_ipv4_profile.h"
#include "virtio_net_feature_profile.h"
#include "virtio_net_link_state.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define APPLIANCE_PRODUCTION_NETWORK_VERSION 1U
#define APPLIANCE_PRODUCTION_NETWORK_GENERATION 1U

enum appliance_production_network_readiness {
    APPLIANCE_PRODUCTION_NETWORK_UNAVAILABLE = 0,
    APPLIANCE_PRODUCTION_NETWORK_DEGRADED = 1,
    APPLIANCE_PRODUCTION_NETWORK_READY = 2,
};

enum appliance_production_network_status {
    APPLIANCE_PRODUCTION_NETWORK_OK = 0,
    APPLIANCE_PRODUCTION_NETWORK_NULL = 1,
    APPLIANCE_PRODUCTION_NETWORK_BAD_STATE = 2,
    APPLIANCE_PRODUCTION_NETWORK_NO_DEVICE = 3,
    APPLIANCE_PRODUCTION_NETWORK_INVALID_INPUT = 4,
    APPLIANCE_PRODUCTION_NETWORK_LINK_NOT_READY = 5,
    APPLIANCE_PRODUCTION_NETWORK_NETIF_FAILED = 6,
    APPLIANCE_PRODUCTION_NETWORK_PROTO_FAILED = 7,
    APPLIANCE_PRODUCTION_NETWORK_BIND_FAILED = 8,
};

enum appliance_production_network_input_result {
    APPLIANCE_PRODUCTION_NETWORK_INPUT_ACCEPTED = 0,
    APPLIANCE_PRODUCTION_NETWORK_INPUT_REJECTED = 1,
    APPLIANCE_PRODUCTION_NETWORK_INPUT_UNAVAILABLE = 2,
};

/*
 * Production transport seam. The ordinary-boot x86 backend submits Ethernet
 * frames to virtio-net; host proofs and later items may substitute their own
 * bounded sink without changing the lwIP-facing composition.
 */
struct appliance_production_network_output {
    bool (*transmit)(
        void *context,
        const uint8_t *frame,
        size_t length);
    void *context;
};

struct appliance_production_network_bind_inputs {
    const struct virtio_net_link_state *link_state;
    const struct virtio_net_feature_profile *profile;
    const struct network_ipv4_config *ipv4_config;
    const struct network_ipv4_port_policy *port_policy;
    struct appliance_production_network_output output;
};

/*
 * ITEM-389 backend-neutral bind inputs.
 *
 * A backend that publishes no virtio feature profile (the on-board Realtek
 * NIC) binds through its published network_backend_binding instead of the
 * virtio profile/link pair.  Every lwIP-facing step after the bind - the
 * netif, the Ethernet input path and the protocol composition - is the same
 * code, so the serving path, the DHCP/DNS wire owner and the panel see one
 * production network whichever backend is live.
 */
struct appliance_production_network_backend_inputs {
    const struct network_backend_binding *binding;
    const struct network_ipv4_config *ipv4_config;
    struct appliance_production_network_output output;
};

struct appliance_production_network {
    bool initialized;
    bool bound;
    bool proto_published;
    enum appliance_production_network_status status;
    enum appliance_production_network_readiness readiness;
    uint32_t generation;
    struct network_device_snapshot device_snapshot;
    struct network_device_selection device_selection;
    struct virtio_net_mac_identity mac_identity;
    struct network_ipv4_profile ipv4_profile;
    struct hfoc_lwip_netif netif;
    struct hfoc_lwip_proto proto;
    struct appliance_production_network_output output;
};

enum appliance_production_network_status appliance_production_network_init(
    struct appliance_production_network *network);
enum appliance_production_network_status appliance_production_network_bind(
    struct appliance_production_network *network,
    const struct appliance_production_network_bind_inputs *inputs);
enum appliance_production_network_status
appliance_production_network_bind_backend(
    struct appliance_production_network *network,
    const struct appliance_production_network_backend_inputs *inputs);
enum appliance_production_network_status appliance_production_network_stop(
    struct appliance_production_network *network);
enum appliance_production_network_input_result
appliance_production_network_input(
    struct appliance_production_network *network,
    const uint8_t *frame,
    size_t frame_length,
    uint32_t now_ms);
enum appliance_production_network_readiness
appliance_production_network_readiness(
    const struct appliance_production_network *network);
bool appliance_production_network_bound(
    const struct appliance_production_network *network);
/*
 * ITEM-451: the two entry flags the frame path sets and never published.
 *
 * `appliance_production_network_bound()` says the composition was staged and
 * the netif registered; a frame can still be refused afterwards by either of
 * these: the lwIP adapter is not published, or the netif is not live.  A record
 * that omits them cannot tell "no frame arrived" from "a frame arrived and the
 * path was not open", which is what the 2026-09-19 hardware run needed.
 */
bool appliance_production_network_proto_published(
    const struct appliance_production_network *network);
bool appliance_production_network_netif_live(
    const struct appliance_production_network *network);
bool appliance_production_network_ready(
    const struct appliance_production_network *network);
const struct network_device_snapshot *
appliance_production_network_device(
    const struct appliance_production_network *network);
const uint8_t *appliance_production_network_mac(
    const struct appliance_production_network *network);
const char *appliance_production_network_readiness_string(
    enum appliance_production_network_readiness readiness);
const char *appliance_production_network_status_string(
    enum appliance_production_network_status status);

#endif
