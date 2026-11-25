#ifdef HFOC_PRODUCTION_NETWORK

#include "appliance_production_network_wire.h"

#include "appliance_network_mode.h"
#include "appliance_network_wire.h"
#include "appliance_production_network.h"
#include "appliance_production_network_boot.h"
#include "appliance_time_sync.h"
#include "appliance_time_wire.h"
#include "hfoc_lwip_netif.h"
#include "hfoc_lwip_proto.h"
#include "hfoc_lwip_time.h"
#include "link_state.h"
#include "network_device.h"
#include "network_ipv4_profile.h"

#include "lwip/etharp.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

static struct appliance_network_wire production_wire;
static bool production_wire_initialized;
static bool production_wire_dns_started;
static enum appliance_production_network_wire_status production_wire_status;
static enum appliance_network_mode_action production_wire_mode =
    APPLIANCE_NETWORK_MODE_ACTION_NONE;

/* ITEM-374 SNTP owner: one bounded client, one frame endpoint. */
#define PRODUCTION_TIME_SOURCE_PORT 50001U
static struct appliance_time_sync production_time_sync;
static bool production_time_sync_initialized;
static bool production_time_sync_started;
static struct appliance_time_wire_endpoint production_time_endpoint;
static bool production_time_endpoint_valid;
/* ITEM-654: the source this boot selected, and whether it is the gateway or
 * the operator's explicit IPv4. */
static struct appliance_production_network_time_config production_time_config;
static enum appliance_production_network_time_source production_time_source =
    APPLIANCE_PRODUCTION_NETWORK_TIME_SOURCE_NONE;
/* ITEM-654: the applied gateway is kept separately from the selected SNTP
 * source.  `production_time_endpoint.next_hop_address` carries the source
 * while a request is built, so comparing that field with the gateway would
 * make every activation look like an address change and restart an explicit
 * attempt on every DHCP renewal. */
static uint8_t production_time_gateway[APPLIANCE_TIME_WIRE_ADDRESS_LENGTH];
static uint8_t production_time_server[APPLIANCE_TIME_WIRE_ADDRESS_LENGTH];
static bool production_time_server_valid;

static struct appliance_production_network *production_network_mutable(void)
{
    return (struct appliance_production_network *)
        appliance_production_network_boot_network();
}

static bool production_wire_transmit(
    void *context,
    const uint8_t *frame,
    size_t length)
{
    const struct appliance_production_network *network =
        appliance_production_network_boot_network();

    (void)context;
    if (network == NULL || network->output.transmit == NULL) {
        return false;
    }
    return network->output.transmit(
        network->output.context, frame, length);
}

static bool production_wire_resolve(
    void *context,
    const struct network_ipv4_address *next_hop,
    uint8_t mac_out[NETWORK_DEVICE_MAC_LENGTH])
{
    const struct appliance_production_network *network =
        appliance_production_network_boot_network();
    ip4_addr_t address;
    struct eth_addr resolved;

    (void)context;
    if (network == NULL || next_hop == NULL) {
        return false;
    }
    address.addr = PP_HTONL(
        ((uint32_t)next_hop->octets[0] << 24U) |
        ((uint32_t)next_hop->octets[1] << 16U) |
        ((uint32_t)next_hop->octets[2] << 8U) |
        (uint32_t)next_hop->octets[3]);
    if (hfoc_lwip_proto_lookup(
            &network->proto, &address, &resolved) == HFOC_LWIP_PROTO_OK) {
        memcpy(mac_out, resolved.addr, NETWORK_DEVICE_MAC_LENGTH);
        return true;
    }
    (void)etharp_request((struct netif *)&network->proto.netif.netif,
                         &address);
    return false;
}

static bool production_time_send(uint32_t now_ms)
{
    struct network_ipv4_address next_hop;
    uint8_t frame[APPLIANCE_TIME_WIRE_FRAME_MAX];
    size_t frame_length = 0U;

    (void)now_ms;
    if (!production_time_endpoint_valid || !production_time_server_valid) {
        return false;
    }
    /* The activation hook owns the active local/gateway pair; the selected
     * SNTP source is restored here so a lease event cannot redirect it. */
    memcpy(production_time_endpoint.next_hop_address,
           production_time_server, APPLIANCE_TIME_WIRE_ADDRESS_LENGTH);
    memcpy(next_hop.octets, production_time_server,
           APPLIANCE_TIME_WIRE_ADDRESS_LENGTH);
    if (!production_wire_resolve(
            NULL, &next_hop, production_time_endpoint.next_hop_mac)) {
        return false;
    }
    if (appliance_time_wire_build(
            &production_time_endpoint, production_time_sync.request,
            APPLIANCE_TIME_SYNC_PACKET_LENGTH, frame, sizeof(frame),
            &frame_length) != APPLIANCE_TIME_WIRE_OK) {
        return false;
    }
    return production_wire_transmit(NULL, frame, frame_length);
}

static void production_time_begin(uint32_t now_ms)
{
    static const uint8_t zero_address[APPLIANCE_TIME_WIRE_ADDRESS_LENGTH] = {
        0U, 0U, 0U, 0U
    };
    struct appliance_production_network *network =
        production_network_mutable();
    const struct network_device_snapshot *device;
    struct appliance_time_sync_config config;

    if (production_time_sync_initialized || network == NULL) {
        return;
    }
    device = appliance_production_network_device(network);
    if (device == NULL ||
        memcmp(production_time_endpoint.local_address, zero_address,
               APPLIANCE_TIME_WIRE_ADDRESS_LENGTH) == 0) {
        return;
    }
    if (production_time_config.declared) {
        if (!production_time_config.valid) {
            /*
             * ITEM-654: the operator named a source the parser refused.  The
             * address stays visibly INVALID and the clock stays
             * UNSYNCHRONIZED; there is no silent fallback to the gateway.
             */
            production_time_source =
                APPLIANCE_PRODUCTION_NETWORK_TIME_SOURCE_INVALID;
            production_time_server_valid = false;
            return;
        }
        memcpy(production_time_server,
               production_time_config.server.octets,
               APPLIANCE_TIME_WIRE_ADDRESS_LENGTH);
        production_time_source =
            APPLIANCE_PRODUCTION_NETWORK_TIME_SOURCE_EXPLICIT;
    } else {
        /*
         * ITEM-654: without the directive the source stays the active
         * gateway, exactly as before.  A missing gateway leaves the clock
         * unstarted instead of inventing a destination.
         */
        if (memcmp(production_time_gateway, zero_address,
                   APPLIANCE_TIME_WIRE_ADDRESS_LENGTH) == 0) {
            return;
        }
        memcpy(production_time_server, production_time_gateway,
               APPLIANCE_TIME_WIRE_ADDRESS_LENGTH);
        production_time_source =
            APPLIANCE_PRODUCTION_NETWORK_TIME_SOURCE_GATEWAY;
    }
    production_time_server_valid = true;
    memcpy(production_time_endpoint.local_mac, device->mac_address,
           APPLIANCE_TIME_WIRE_MAC_LENGTH);
    production_time_endpoint.source_port = PRODUCTION_TIME_SOURCE_PORT;
    production_time_endpoint.destination_port = APPLIANCE_TIME_SYNC_PORT;
    production_time_endpoint_valid = true;

    memset(&config, 0, sizeof(config));
    config.version = APPLIANCE_TIME_SYNC_VERSION;
    memcpy(config.server, production_time_server,
           APPLIANCE_TIME_WIRE_ADDRESS_LENGTH);
    config.port = APPLIANCE_TIME_SYNC_PORT;
    config.max_attempts = APPLIANCE_TIME_SYNC_ATTEMPT_MAX;
    config.attempt_deadline_ms = APPLIANCE_TIME_SYNC_DEADLINE_MS;
    if (appliance_time_sync_init(&production_time_sync, &config) !=
        APPLIANCE_TIME_SYNC_OK) {
        return;
    }
    production_time_sync_initialized = true;
    if (appliance_time_sync_start(&production_time_sync, now_ms) !=
        APPLIANCE_TIME_SYNC_OK) {
        production_time_sync_initialized = false;
        return;
    }
    production_time_sync_started = true;
    (void)production_time_send(now_ms);
}

static bool production_wire_activate(
    void *context,
    const struct network_ipv4_config *config)
{
    struct appliance_production_network *network =
        production_network_mutable();
    bool address_changed = false;

    (void)context;
    if (network == NULL || config == NULL) {
        return false;
    }
    /*
     * ITEM-654: an address reset or a changed lease invalidates the endpoint
     * the client was started on.  An in-progress attempt is restarted against
     * the new local address; a validated clock keeps its anchor.
     */
    address_changed =
        memcmp(production_time_endpoint.local_address, config->address.octets,
               APPLIANCE_TIME_WIRE_ADDRESS_LENGTH) != 0 ||
        memcmp(production_time_gateway, config->gateway.octets,
               APPLIANCE_TIME_WIRE_ADDRESS_LENGTH) != 0;
    memcpy(production_time_endpoint.local_address,
           config->address.octets,
           APPLIANCE_TIME_WIRE_ADDRESS_LENGTH);
    memcpy(production_time_gateway, config->gateway.octets,
           APPLIANCE_TIME_WIRE_ADDRESS_LENGTH);
    /* Idle endpoint state follows the applied gateway; a request build
     * substitutes the selected SNTP source for its destination. */
    memcpy(production_time_endpoint.next_hop_address,
           config->gateway.octets,
           APPLIANCE_TIME_WIRE_ADDRESS_LENGTH);
    if (address_changed && production_time_sync_initialized &&
        !appliance_production_network_time_ready()) {
        production_time_sync_initialized = false;
        production_time_sync_started = false;
        production_time_endpoint_valid = false;
        production_time_source =
            APPLIANCE_PRODUCTION_NETWORK_TIME_SOURCE_NONE;
        production_time_server_valid = false;
    }
    if (hfoc_lwip_netif_set_config(&network->netif, config) !=
        HFOC_LWIP_NETIF_OK) {
        return false;
    }
    /*
     * The frozen hfoc_lwip_proto stores its own netif snapshot; keep the
     * ARP/ICMP copy aligned with the applied lease.
     */
    return hfoc_lwip_netif_set_config(&network->proto.netif, config) ==
           HFOC_LWIP_NETIF_OK;
}

/*
 * ITEM-416: releasing the address means the machine has **no** address.
 *
 * This hook used to install the QEMU fixture configuration (`10.0.2.15/24`,
 * gateway `10.0.2.2`, DNS `10.0.2.3`) as the state a released address leaves
 * behind, so the acceptance host - whose DHCP server had not answered yet, or
 * had not answered inside the old budget - ended up holding a test address: the
 * listener published itself on `10.0.2.15:443`, nothing on the operator's
 * network could route to it, and the panel was unreachable while ICMP to the
 * machine was the fixture address talking to itself on the same segment.
 *
 * A zero configuration is what the lifecycle means by "the address is gone",
 * and it is what lets the serving path report `AWAITING_NETWORK` honestly
 * instead of binding an address nobody gave this machine.
 */
static bool production_wire_deactivate(void *context)
{
    struct appliance_production_network *network =
        production_network_mutable();
    (void)context;
    if (network == NULL) {
        return false;
    }
    if (hfoc_lwip_netif_clear_address(&network->netif) !=
        HFOC_LWIP_NETIF_OK) {
        return false;
    }
    return hfoc_lwip_netif_clear_address(&network->proto.netif) ==
           HFOC_LWIP_NETIF_OK;
}

/*
 * ITEM-600: a link the appliance can use is the device link **and** a live
 * composition.
 *
 * The ordinary boot used to publish `NETWORK_LINK_UP` here unconditionally.
 * On a cold start the Realtek PHY is still negotiating when the interface is
 * first staged, so the saved STATIC address was offered to a netif that was
 * not live yet: `hfoc_lwip_netif_set_config` refused with NOT_LIVE, and the
 * lifecycle turned that boot ordering into `ADDRESS_ACTIVATION` and stayed
 * FAILED after the later bind made the netif live.  DHCP survived because it
 * has an acquisition cycle of its own; STATIC had nothing to retry.
 *
 * The boot record and the paced sampler therefore ask the same question: is
 * the device link up **and** the production composition ready to carry an
 * address?  Until it is, the lifecycle waits; the next sample after the bind
 * completes applies the saved configuration.
 */
static bool production_wire_link_usable(
    const struct appliance_production_network *network)
{
    return network != NULL &&
           appliance_production_network_ready(network) &&
           appliance_production_network_boot_link_up();
}

static bool production_wire_link_record(
    struct network_link_state_record *link)
{
    const struct appliance_production_network *network =
        appliance_production_network_boot_network();
    const struct network_device_snapshot *device =
        appliance_production_network_device(network);

    if (device == NULL || link == NULL) {
        return false;
    }
    memset(link, 0, sizeof(*link));
    link->version = NETWORK_LINK_STATE_VERSION;
    link->device_id = device->device_id;
    link->state = production_wire_link_usable(network)
        ? NETWORK_LINK_UP
        : NETWORK_LINK_DOWN;
    link->generation = APPLIANCE_PRODUCTION_NETWORK_GENERATION;
    return true;
}

enum appliance_production_network_wire_status
appliance_production_network_wire_boot_init(
    const struct appliance_network_config *desired,
    const struct appliance_production_network_time_config *time)
{
    const struct appliance_production_network *network =
        appliance_production_network_boot_network();
    struct appliance_network_wire_io io;
    struct network_link_state_record link;

    memset(&production_time_config, 0, sizeof(production_time_config));
    if (time != NULL) {
        production_time_config = *time;
    }
    production_time_source = APPLIANCE_PRODUCTION_NETWORK_TIME_SOURCE_NONE;
    memset(production_time_gateway, 0, sizeof(production_time_gateway));
    memset(production_time_server, 0, sizeof(production_time_server));
    production_time_server_valid = false;
    production_time_sync_initialized = false;
    production_time_sync_started = false;
    production_time_endpoint_valid = false;
    production_wire_initialized = false;
    production_wire_dns_started = false;
    production_wire_mode = APPLIANCE_NETWORK_MODE_ACTION_NONE;
    production_wire_status =
        APPLIANCE_PRODUCTION_NETWORK_WIRE_UNAVAILABLE;
    if (network == NULL || desired == NULL ||
        !appliance_production_network_boot_device_present() ||
        !appliance_production_network_bound(network) ||
        appliance_production_network_device(network) == NULL) {
        return production_wire_status;
    }
    {
        struct appliance_network_mode_decision decision;

        if (appliance_network_mode_decide(desired, true, &decision) !=
            APPLIANCE_NETWORK_MODE_OK) {
            return production_wire_status;
        }
        production_wire_mode = decision.action;
        if (decision.action == APPLIANCE_NETWORK_MODE_ACTION_OFFLINE) {
            /* No address, no DHCP and no DNS: the appliance boots offline and
             * publishes DEGRADED instead of a fabricated connection. */
            production_wire_status =
                APPLIANCE_PRODUCTION_NETWORK_WIRE_DEGRADED;
            return production_wire_status;
        }
    }

    memset(&io, 0, sizeof(io));
    io.transmit = production_wire_transmit;
    io.transmit_context = NULL;
    io.link.resolve = production_wire_resolve;
    io.link.context = NULL;
    io.activate.activate_ipv4 = production_wire_activate;
    io.activate.deactivate_ipv4 = production_wire_deactivate;
    io.activate.context = NULL;

    if (appliance_network_wire_init(
            &production_wire, desired,
            appliance_production_network_device(network), &io) !=
        APPLIANCE_NETWORK_WIRE_OK) {
        return production_wire_status;
    }
    production_wire_initialized = true;
    if (appliance_network_wire_start(&production_wire) !=
            APPLIANCE_NETWORK_WIRE_OK ||
        !production_wire_link_record(&link) ||
        appliance_network_wire_link_up(
            &production_wire, &link, hfoc_lwip_time_now()) !=
            APPLIANCE_NETWORK_WIRE_OK) {
        production_wire_status =
            APPLIANCE_PRODUCTION_NETWORK_WIRE_DEGRADED;
        return production_wire_status;
    }
    if (!appliance_production_network_ready(network)) {
        production_wire_status =
            APPLIANCE_PRODUCTION_NETWORK_WIRE_DEGRADED;
        return production_wire_status;
    }
    production_wire_status = APPLIANCE_PRODUCTION_NETWORK_WIRE_OK;
    return production_wire_status;
}

void appliance_production_network_wire_boot_poll(uint32_t now_ms)
{
    enum appliance_network_lifecycle_state wire_state;

    if (!production_wire_initialized) {
        return;
    }
    appliance_network_wire_tick(&production_wire, now_ms);
    /*
     * ITEM-408: the device's own link signal is fed to the lifecycle, paced
     * and debounced, so the one honest way a working address is given up - a
     * link that really stayed down - is actually observed on hardware
     * instead of only at the boot-time record.
     */
    (void)appliance_network_wire_link_sample(
        &production_wire,
        production_wire_link_usable(
            appliance_production_network_boot_network()),
        now_ms);
    wire_state = appliance_network_wire_state(&production_wire);
    /* ITEM-374: a runtime DHCP failure is published as DEGRADED instead of
     * keeping the status a boot-time constant. */
    if (wire_state == APPLIANCE_NETWORK_LIFECYCLE_STATE_FAILED ||
        wire_state == APPLIANCE_NETWORK_LIFECYCLE_STATE_STOPPED) {
        production_wire_status =
            APPLIANCE_PRODUCTION_NETWORK_WIRE_DEGRADED;
    } else if (wire_state == APPLIANCE_NETWORK_LIFECYCLE_STATE_ACTIVE) {
        production_wire_status = APPLIANCE_PRODUCTION_NETWORK_WIRE_OK;
        production_time_begin(now_ms);
    }
    if (!production_wire_dns_started &&
        wire_state == APPLIANCE_NETWORK_LIFECYCLE_STATE_ACTIVE) {
        production_wire_dns_started = true;
        (void)appliance_network_wire_resolve(
            &production_wire,
            APPLIANCE_NETWORK_WIRE_PROBE_HOSTNAME,
            APPLIANCE_NETWORK_WIRE_PROBE_HOSTNAME_LENGTH, now_ms);
    }
    if (production_time_sync_initialized && production_time_sync_started) {
        uint32_t deadline_ms = 0U;

        if (appliance_time_sync_deadline(
                &production_time_sync, &deadline_ms) &&
            (int32_t)(now_ms - deadline_ms) >= 0 &&
            appliance_time_sync_tick(&production_time_sync, now_ms) ==
                APPLIANCE_TIME_SYNC_OK) {
            (void)production_time_send(now_ms);
        }
    }
}

bool appliance_production_network_wire_boot_input(
    const uint8_t *frame,
    size_t frame_length,
    uint32_t now_ms)
{
    if (production_time_sync_initialized &&
        production_time_endpoint_valid && production_time_server_valid) {
        uint8_t payload[APPLIANCE_TIME_SYNC_PACKET_LENGTH];

        memcpy(production_time_endpoint.next_hop_address,
               production_time_server, APPLIANCE_TIME_WIRE_ADDRESS_LENGTH);
        size_t payload_length = 0U;

        if (appliance_time_wire_parse(
                &production_time_endpoint, frame, frame_length, payload,
                sizeof(payload), &payload_length) ==
            APPLIANCE_TIME_WIRE_OK) {
            (void)appliance_time_sync_input(
                &production_time_sync, payload, payload_length, now_ms);
            return true;
        }
    }
    if (!production_wire_initialized) {
        return false;
    }
    return appliance_network_wire_input(
               &production_wire, frame, frame_length, now_ms) ==
           APPLIANCE_NETWORK_WIRE_INPUT_CONSUMED;
}

const struct appliance_network_wire *
appliance_production_network_wire_boot_wire(void)
{
    return production_wire_initialized ? &production_wire : NULL;
}

enum appliance_production_network_wire_status
appliance_production_network_wire_boot_status(void)
{
    return production_wire_status;
}

enum appliance_network_mode_action
appliance_production_network_wire_boot_mode(void)
{
    return production_wire_mode;
}

bool appliance_production_network_time_ready(void)
{
    return production_time_sync_initialized &&
           appliance_time_sync_ready(&production_time_sync);
}

bool appliance_production_network_time_server(
    struct network_ipv4_address *server_out,
    enum appliance_production_network_time_source *source_out)
{
    if (source_out != NULL) {
        *source_out = production_time_source;
    }
    if (server_out != NULL) {
        memset(server_out, 0, sizeof(*server_out));
        if (production_time_server_valid) {
            memcpy(server_out->octets, production_time_server,
                   APPLIANCE_TIME_WIRE_ADDRESS_LENGTH);
        }
    }
    return production_time_server_valid;
}

enum appliance_time_sync_state appliance_production_network_time_state(void)
{
    if (production_time_source ==
        APPLIANCE_PRODUCTION_NETWORK_TIME_SOURCE_INVALID) {
        return APPLIANCE_TIME_SYNC_STATE_UNSYNCHRONIZED;
    }
    if (!production_time_sync_initialized) {
        return APPLIANCE_TIME_SYNC_STATE_NONE;
    }
    return appliance_time_sync_state(&production_time_sync);
}

enum appliance_time_sync_status appliance_production_network_time_status(void)
{
    if (production_time_source ==
        APPLIANCE_PRODUCTION_NETWORK_TIME_SOURCE_INVALID) {
        return APPLIANCE_TIME_SYNC_BAD_CONFIG;
    }
    if (!production_time_sync_initialized) {
        return APPLIANCE_TIME_SYNC_BAD_STATE;
    }
    return appliance_time_sync_last_status(&production_time_sync);
}

bool appliance_production_network_time_deadline(uint32_t *deadline_ms_out)
{
    if (!production_time_sync_initialized) {
        return false;
    }
    return appliance_time_sync_deadline(
        &production_time_sync, deadline_ms_out);
}

uint32_t appliance_production_network_time_epoch_seconds(uint32_t now_ms)
{
    if (!production_time_sync_initialized) {
        return 0U;
    }
    return appliance_time_sync_epoch_seconds(&production_time_sync, now_ms);
}

#else
typedef int appliance_production_network_wire_translation_unit_guard;
#endif
