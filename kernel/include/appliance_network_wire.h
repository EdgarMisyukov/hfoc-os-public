#ifndef HFOC_APPLIANCE_NETWORK_WIRE_H
#define HFOC_APPLIANCE_NETWORK_WIRE_H

#include "appliance_dns_resolver.h"
#include "appliance_network_config.h"
#include "appliance_network_lifecycle.h"
#include "ipv4_config.h"
#include "link_state.h"
#include "network_device.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define APPLIANCE_NETWORK_WIRE_VERSION 1U
#define APPLIANCE_NETWORK_WIRE_FRAME_MAX 1518U
#define APPLIANCE_NETWORK_WIRE_IPV4_HEADER_LENGTH 20U
#define APPLIANCE_NETWORK_WIRE_UDP_HEADER_LENGTH 8U
#define APPLIANCE_NETWORK_WIRE_ETHERNET_HEADER_LENGTH 14U
#define APPLIANCE_NETWORK_WIRE_DHCP_CLIENT_PORT 68U
#define APPLIANCE_NETWORK_WIRE_DHCP_SERVER_PORT 67U
#define APPLIANCE_NETWORK_WIRE_DNS_PORT 53U
#define APPLIANCE_NETWORK_WIRE_DNS_SOURCE_PORT 49152U
#define APPLIANCE_NETWORK_WIRE_DHCP_XID UINT32_C(0x48464F43)
#define APPLIANCE_NETWORK_WIRE_DHCP_PAYLOAD_MIN 300U
#define APPLIANCE_NETWORK_WIRE_DHCP_MAGIC UINT32_C(0x63825363)
#define APPLIANCE_NETWORK_WIRE_DHCP_TIMEOUT_MS 1000U
#define APPLIANCE_NETWORK_WIRE_DNS_TIMEOUT_MS 1000U
#define APPLIANCE_NETWORK_WIRE_PROBE_HOSTNAME "api.hfoc.test"
#define APPLIANCE_NETWORK_WIRE_PROBE_HOSTNAME_LENGTH 13U
#define APPLIANCE_NETWORK_WIRE_HOSTNAME_MAX \
    APPLIANCE_DNS_RESOLVER_HOSTNAME_MAX

/*
 * ITEM-408: the schedules that keep a live address live.
 *
 * The renewal is armed from the lease time the server itself returned, on the
 * monotonic millisecond clock the tick is driven with - never on the wall
 * clock, which an appliance without a validated time source does not have.
 * A link has to read down for three consecutive samples before the address is
 * given up, so one momentary sample cannot take a working lease away.
 */
#define APPLIANCE_NETWORK_WIRE_RENEW_FLOOR_MS 5000U
#define APPLIANCE_NETWORK_WIRE_RENEW_CEILING_MS 3600000U
#define APPLIANCE_NETWORK_WIRE_RENEW_RETRY_MS 5000U
#define APPLIANCE_NETWORK_WIRE_LINK_SAMPLE_MS 1000U
#define APPLIANCE_NETWORK_WIRE_LINK_DOWN_SAMPLES 3U
#define APPLIANCE_NETWORK_WIRE_DNS_PROBE_RETRY_MS 15000U

enum appliance_network_wire_status {
    APPLIANCE_NETWORK_WIRE_OK = 0,
    APPLIANCE_NETWORK_WIRE_NULL = 1,
    APPLIANCE_NETWORK_WIRE_BAD_STATE = 2,
    APPLIANCE_NETWORK_WIRE_BAD_VERSION = 3,
    APPLIANCE_NETWORK_WIRE_BAD_CONFIG = 4,
    APPLIANCE_NETWORK_WIRE_BAD_DEVICE = 5,
    APPLIANCE_NETWORK_WIRE_BAD_IO = 6,
    APPLIANCE_NETWORK_WIRE_BAD_LINK = 7,
    APPLIANCE_NETWORK_WIRE_BUFFER_TOO_SMALL = 8,
    APPLIANCE_NETWORK_WIRE_TRANSPORT_FAILED = 9,
};

enum appliance_network_wire_input_result {
    APPLIANCE_NETWORK_WIRE_INPUT_IGNORED = 0,
    APPLIANCE_NETWORK_WIRE_INPUT_CONSUMED = 1,
};

/*
 * Bounded link-layer resolution seam. The production owner resolves the next
 * hop through the live lwIP ARP cache and starts resolution when the entry is
 * missing; host proofs substitute a deterministic table.
 */
struct appliance_network_wire_link_io {
    bool (*resolve)(
        void *context,
        const struct network_ipv4_address *next_hop,
        uint8_t mac_out[NETWORK_DEVICE_MAC_LENGTH]);
    void *context;
};

/*
 * IPv4 activation seam. The production owner applies the acquired or static
 * address to the live netif; host proofs record the published configuration.
 */
struct appliance_network_wire_activate_io {
    bool (*activate_ipv4)(
        void *context,
        const struct network_ipv4_config *config);
    bool (*deactivate_ipv4)(void *context);
    void *context;
};

struct appliance_network_wire_io {
    bool (*transmit)(
        void *context,
        const uint8_t *frame,
        size_t length);
    void *transmit_context;
    struct appliance_network_wire_link_io link;
    struct appliance_network_wire_activate_io activate;
};

struct appliance_network_wire {
    uint32_t version;
    bool initialized;
    bool started;
    struct appliance_network_lifecycle lifecycle;
    struct appliance_network_wire_io io;
    uint8_t local_mac[NETWORK_DEVICE_MAC_LENGTH];
    uint32_t device_id;
    uint32_t now_ms;
    uint32_t dhcp_deadline_ms;
    bool dhcp_deadline_set;
    uint32_t dns_deadline_ms;
    bool dns_deadline_set;
    char dns_hostname[APPLIANCE_NETWORK_WIRE_HOSTNAME_MAX + 1U];
    size_t dns_hostname_length;
    uint16_t dns_transaction_id;
    uint32_t offered_address;
    bool offer_present;
    uint32_t offered_server;
    bool server_present;
    uint32_t transmitted_count;
    uint32_t received_count;
    uint32_t malformed_count;
    uint32_t retry_count;
    /*
     * ITEM-408: the live link sample the record is built from, the renewal
     * schedule the acknowledged lease time produced, and the retry schedule
     * of a DNS probe the resolver gave up on.  None of them owns the address:
     * they only decide when the appliance acts on the address it already has.
     */
    uint32_t link_generation;
    bool link_record_published;
    bool link_observed_up;
    bool link_sample_taken;
    uint32_t link_sample_ms;
    uint32_t link_down_samples;
    bool lease_time_published;
    uint32_t lease_seconds;
    bool renew_scheduled;
    uint32_t renew_at_ms;
    uint32_t dhcp_kick_ms;
    bool dns_probe_scheduled;
    uint32_t dns_probe_at_ms;
    uint32_t dns_probe_failures;
    /*
     * ITEM-428: the wire view of the DHCP exchange, for the boot record.
     *
     * The 2026-09-16 hardware run published `attempt=11/40` and nothing else,
     * which left "the machine never transmitted", "it transmitted and was
     * never answered" and "it was answered and ignored the answer" as one
     * fact.  These five counters separate them: what the transmit path
     * accepted (`discovers_sent`, `requests_sent`), what the server answered
     * (`offers_received`, `acks_received`), and how many DHCP frames the
     * transmit path refused (`send_refusals`) - a full transmit ring seen from
     * the wire.
     */
    uint32_t dhcp_discovers_sent;
    uint32_t dhcp_requests_sent;
    uint32_t dhcp_offers_received;
    uint32_t dhcp_acks_received;
    uint32_t dhcp_send_refusals;
};

enum appliance_network_wire_status appliance_network_wire_init(
    struct appliance_network_wire *wire,
    const struct appliance_network_config *desired,
    const struct network_device_snapshot *device,
    const struct appliance_network_wire_io *io);
enum appliance_network_wire_status appliance_network_wire_start(
    struct appliance_network_wire *wire);
enum appliance_network_wire_status appliance_network_wire_link_up(
    struct appliance_network_wire *wire,
    const struct network_link_state_record *link,
    uint32_t now_ms);
/*
 * ITEM-408: one live link sample from the backend that owns the device.  It
 * is paced, it debounces, and it only publishes a lifecycle link record when
 * the sample really changed what the appliance must do: a link that reads up
 * again restores the path, and a link that reads down for
 * APPLIANCE_NETWORK_WIRE_LINK_DOWN_SAMPLES consecutive samples is the one
 * honest way a held address is given up.
 */
enum appliance_network_wire_status appliance_network_wire_link_sample(
    struct appliance_network_wire *wire,
    bool link_up,
    uint32_t now_ms);
enum appliance_network_wire_status appliance_network_wire_resolve(
    struct appliance_network_wire *wire,
    const char *hostname,
    size_t hostname_length,
    uint32_t now_ms);
enum appliance_network_wire_input_result appliance_network_wire_input(
    struct appliance_network_wire *wire,
    const uint8_t *frame,
    size_t frame_length,
    uint32_t now_ms);
void appliance_network_wire_tick(
    struct appliance_network_wire *wire,
    uint32_t now_ms);
enum appliance_network_wire_status appliance_network_wire_stop(
    struct appliance_network_wire *wire);

bool appliance_network_wire_published(
    const struct appliance_network_wire *wire);
enum appliance_network_lifecycle_state appliance_network_wire_state(
    const struct appliance_network_wire *wire);
const struct network_ipv4_config *
appliance_network_wire_active_config(
    const struct appliance_network_wire *wire);
const struct appliance_dhcp_client *
appliance_network_wire_dhcp(
    const struct appliance_network_wire *wire);
const struct appliance_dns_resolver *
appliance_network_wire_dns(
    const struct appliance_network_wire *wire);
const struct network_ipv4_address *
appliance_network_wire_dns_answer(
    const struct appliance_network_wire *wire);
const struct appliance_network_lifecycle *
appliance_network_wire_lifecycle(
    const struct appliance_network_wire *wire);
/*
 * ITEM-416: the acquisition attempt the DHCP client is on, and the budget it is
 * bounded by, so the `[PRODUCTION-DHCP]` record can say "still asking, 7 of 40"
 * instead of leaving a machine that is retrying looking like one that stopped.
 */
bool appliance_network_wire_dhcp_attempts(
    const struct appliance_network_wire *wire,
    uint32_t *attempt,
    uint32_t *budget);

/*
 * ITEM-428: the DHCP wire view - what the transmit path accepted, what the
 * server answered, and what the transmit path refused.  False when the wire
 * owns no DHCP exchange at all, so zeroed counters are never read as "nothing
 * happened".
 */
struct appliance_network_wire_dhcp_counters {
    uint32_t discovers_sent;
    uint32_t requests_sent;
    uint32_t offers_received;
    uint32_t acks_received;
    uint32_t send_refusals;
};

bool appliance_network_wire_dhcp_counters(
    const struct appliance_network_wire *wire,
    struct appliance_network_wire_dhcp_counters *counters);

bool appliance_network_wire_lease_time(
    const struct appliance_network_wire *wire,
    uint32_t *lease_seconds_out);
const char *appliance_network_wire_status_string(
    enum appliance_network_wire_status status);
const char *appliance_network_wire_input_result_string(
    enum appliance_network_wire_input_result result);

#endif
