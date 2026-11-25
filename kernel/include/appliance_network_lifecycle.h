#ifndef HFOC_APPLIANCE_NETWORK_LIFECYCLE_H
#define HFOC_APPLIANCE_NETWORK_LIFECYCLE_H

#include "appliance_dhcp_client.h"
#include "appliance_dns_resolver.h"
#include "appliance_network_config.h"
#include "appliance_network_diagnostics.h"
#include "ipv4_config.h"
#include "link_state.h"
#include "network_device.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define APPLIANCE_NETWORK_LIFECYCLE_VERSION 1U
#define APPLIANCE_NETWORK_LIFECYCLE_OUTPUT_MAX 640U

enum appliance_network_lifecycle_state {
    APPLIANCE_NETWORK_LIFECYCLE_STATE_NONE = 0,
    APPLIANCE_NETWORK_LIFECYCLE_STATE_UNINITIALIZED = 1,
    APPLIANCE_NETWORK_LIFECYCLE_STATE_PREPARED = 2,
    APPLIANCE_NETWORK_LIFECYCLE_STATE_STARTED = 3,
    APPLIANCE_NETWORK_LIFECYCLE_STATE_WAITING_FOR_LINK = 4,
    APPLIANCE_NETWORK_LIFECYCLE_STATE_ADDRESSING = 5,
    APPLIANCE_NETWORK_LIFECYCLE_STATE_ACTIVE = 6,
    APPLIANCE_NETWORK_LIFECYCLE_STATE_STOPPED = 7,
    APPLIANCE_NETWORK_LIFECYCLE_STATE_FAILED = 8,
};

enum appliance_network_lifecycle_status {
    APPLIANCE_NETWORK_LIFECYCLE_OK = 0,
    APPLIANCE_NETWORK_LIFECYCLE_NULL = 1,
    APPLIANCE_NETWORK_LIFECYCLE_BAD_STATE = 2,
    APPLIANCE_NETWORK_LIFECYCLE_BAD_VERSION = 3,
    APPLIANCE_NETWORK_LIFECYCLE_BAD_CONFIG = 4,
    APPLIANCE_NETWORK_LIFECYCLE_BAD_DEVICE = 5,
    APPLIANCE_NETWORK_LIFECYCLE_BAD_LINK = 6,
    APPLIANCE_NETWORK_LIFECYCLE_BAD_REACHABILITY = 7,
    APPLIANCE_NETWORK_LIFECYCLE_BAD_DHCP = 8,
    APPLIANCE_NETWORK_LIFECYCLE_BAD_DNS = 9,
    APPLIANCE_NETWORK_LIFECYCLE_TRANSPORT_UNAVAILABLE = 10,
    APPLIANCE_NETWORK_LIFECYCLE_TRANSPORT_FAILED = 11,
    APPLIANCE_NETWORK_LIFECYCLE_EXHAUSTED = 12,
    APPLIANCE_NETWORK_LIFECYCLE_BUFFER_TOO_SMALL = 13,
};

enum appliance_network_lifecycle_dhcp_timeout {
    APPLIANCE_NETWORK_LIFECYCLE_DHCP_TIMEOUT_NONE = 0,
    APPLIANCE_NETWORK_LIFECYCLE_DHCP_TIMEOUT_DISCOVER = 1,
    APPLIANCE_NETWORK_LIFECYCLE_DHCP_TIMEOUT_REQUEST = 2,
    APPLIANCE_NETWORK_LIFECYCLE_DHCP_TIMEOUT_RENEW = 3,
    APPLIANCE_NETWORK_LIFECYCLE_DHCP_TIMEOUT_REBIND = 4,
};

/*
 * ITEM-408: why the lifecycle last left ACTIVE, or last held a live address
 * through a failure it did not accept.  The vocabulary is bounded and one
 * field of the [PRODUCTION-DHCP] record carries it, so a boot without a
 * serial capture still names the path, the timeout and the link sample that
 * ended - or failed to end - the address the appliance was serving.
 *
 * A reason is either honest evidence of a state the appliance was actually
 * told (an explicit DHCP refusal, a link sample the device itself reported)
 * or the bounded failure a retry survived; it is never invented.
 */
enum appliance_network_lifecycle_reason {
    APPLIANCE_NETWORK_LIFECYCLE_REASON_NONE = 0,
    APPLIANCE_NETWORK_LIFECYCLE_REASON_LINK_DOWN_SAMPLE = 1,
    APPLIANCE_NETWORK_LIFECYCLE_REASON_LINK_INITIALIZING_SAMPLE = 2,
    APPLIANCE_NETWORK_LIFECYCLE_REASON_LINK_FAILED_SAMPLE = 3,
    APPLIANCE_NETWORK_LIFECYCLE_REASON_DISCOVER_TIMEOUT = 4,
    APPLIANCE_NETWORK_LIFECYCLE_REASON_REQUEST_TIMEOUT = 5,
    APPLIANCE_NETWORK_LIFECYCLE_REASON_RENEW_TIMEOUT = 6,
    APPLIANCE_NETWORK_LIFECYCLE_REASON_REBIND_TIMEOUT = 7,
    APPLIANCE_NETWORK_LIFECYCLE_REASON_DHCP_REFUSED = 8,
    APPLIANCE_NETWORK_LIFECYCLE_REASON_DHCP_TRANSPORT = 9,
    APPLIANCE_NETWORK_LIFECYCLE_REASON_ADDRESS_ACTIVATION = 10,
    APPLIANCE_NETWORK_LIFECYCLE_REASON_DNS_PROBE_TIMEOUT = 11,
    APPLIANCE_NETWORK_LIFECYCLE_REASON_DNS_PROBE_REFUSED = 12,
    APPLIANCE_NETWORK_LIFECYCLE_REASON_DNS_TRANSPORT = 13,
    APPLIANCE_NETWORK_LIFECYCLE_REASON_TRANSPORT_UNAVAILABLE = 14,
    APPLIANCE_NETWORK_LIFECYCLE_REASON_STOPPED = 15,
};

struct appliance_network_lifecycle_transport {
    uint32_t version;
    void *context;
    bool (*activate_ipv4)(
        void *context,
        const struct network_ipv4_config *active);
    bool (*deactivate_ipv4)(void *context);
    bool (*dhcp_discover)(void *context);
    bool (*dhcp_request)(void *context);
    bool (*dhcp_renew)(void *context);
    bool (*dhcp_rebind)(void *context);
    bool (*dns_query)(
        void *context,
        const char *hostname,
        size_t hostname_length);
};

struct appliance_network_lifecycle {
    uint32_t version;
    bool initialized;
    bool started;
    bool transport_installed;
    enum appliance_network_lifecycle_state state;
    enum appliance_network_lifecycle_status last_status;
    /*
     * ITEM-408: the published reason for the last leave-ACTIVE decision and
     * the number of consecutive failures the live address survived.  Both are
     * bounded, both are re-published when they change, and both are zero or
     * NONE on a healthy appliance.
     */
    enum appliance_network_lifecycle_reason reason;
    uint32_t hold_count;
    struct appliance_network_config desired;
    struct network_device_snapshot device;
    struct network_link_state_record link;
    bool link_published;
    bool has_active;
    struct network_ipv4_config active;
    bool dhcp_initialized;
    struct appliance_dhcp_client dhcp;
    bool dns_initialized;
    struct appliance_dns_resolver dns;
    struct appliance_network_lifecycle_transport transport;
    enum appliance_network_reachability gateway_reachability;
    enum appliance_network_reachability dns_reachability;
    struct appliance_network_diagnostics diagnostics;
};

enum appliance_network_lifecycle_status
appliance_network_lifecycle_init(
    struct appliance_network_lifecycle *lifecycle,
    const struct appliance_network_config *desired,
    const struct network_device_snapshot *device,
    const struct appliance_network_lifecycle_transport *transport);
enum appliance_network_lifecycle_status
appliance_network_lifecycle_start(
    struct appliance_network_lifecycle *lifecycle);
enum appliance_network_lifecycle_status
appliance_network_lifecycle_link(
    struct appliance_network_lifecycle *lifecycle,
    const struct network_link_state_record *link);
enum appliance_network_lifecycle_status
appliance_network_lifecycle_dhcp_offer(
    struct appliance_network_lifecycle *lifecycle,
    const struct network_ipv4_config *candidate);
enum appliance_network_lifecycle_status
appliance_network_lifecycle_dhcp_ack(
    struct appliance_network_lifecycle *lifecycle,
    const struct network_ipv4_config *lease);
enum appliance_network_lifecycle_status
appliance_network_lifecycle_dhcp_nak(
    struct appliance_network_lifecycle *lifecycle);
enum appliance_network_lifecycle_status
appliance_network_lifecycle_dhcp_timeout(
    struct appliance_network_lifecycle *lifecycle,
    enum appliance_network_lifecycle_dhcp_timeout timeout);
/*
 * ITEM-428: a DHCP frame the transmit path refused.
 *
 * It is published as the reason the addressing or refresh cycle is not moving
 * - `reason=DHCP_TRANSPORT` on the `[PRODUCTION-DHCP]` record - and it never
 * ends the cycle: an appliance that holds an address keeps it, and an
 * appliance that is still acquiring keeps its bounded attempt budget, so the
 * transmit path recovering resumes the cycle instead of finding it failed.
 */
enum appliance_network_lifecycle_status
appliance_network_lifecycle_dhcp_transport_refused(
    struct appliance_network_lifecycle *lifecycle);
enum appliance_network_lifecycle_status
appliance_network_lifecycle_dhcp_renew(
    struct appliance_network_lifecycle *lifecycle);
enum appliance_network_lifecycle_status
appliance_network_lifecycle_dns_query(
    struct appliance_network_lifecycle *lifecycle,
    const char *hostname,
    size_t hostname_length);
enum appliance_network_lifecycle_status
appliance_network_lifecycle_dns_response(
    struct appliance_network_lifecycle *lifecycle,
    const struct network_ipv4_address *answer);
enum appliance_network_lifecycle_status
appliance_network_lifecycle_dns_timeout(
    struct appliance_network_lifecycle *lifecycle);
enum appliance_network_lifecycle_status
appliance_network_lifecycle_dns_server_failure(
    struct appliance_network_lifecycle *lifecycle);
/*
 * ITEM-408: return a DNS probe the resolver gave up on to READY so the probe
 * can be retried without touching the live address or the lease that owns it.
 * It is the only resolver transition the wire needs after exhaustion, and it
 * refuses to run on a lifecycle that is not serving an address.
 */
enum appliance_network_lifecycle_status
appliance_network_lifecycle_dns_probe_retry(
    struct appliance_network_lifecycle *lifecycle);
enum appliance_network_lifecycle_status
appliance_network_lifecycle_reachability(
    struct appliance_network_lifecycle *lifecycle,
    enum appliance_network_reachability gateway,
    enum appliance_network_reachability dns);
enum appliance_network_lifecycle_status
appliance_network_lifecycle_stop(
    struct appliance_network_lifecycle *lifecycle);
enum appliance_network_lifecycle_status
appliance_network_lifecycle_reset(
    struct appliance_network_lifecycle *lifecycle);
enum appliance_network_lifecycle_status
appliance_network_lifecycle_render(
    const struct appliance_network_lifecycle *lifecycle,
    char *output,
    size_t capacity,
    size_t *output_length);
enum appliance_network_lifecycle_state
appliance_network_lifecycle_state(
    const struct appliance_network_lifecycle *lifecycle);
bool appliance_network_lifecycle_active(
    const struct appliance_network_lifecycle *lifecycle);
const struct network_ipv4_config *
appliance_network_lifecycle_active_config(
    const struct appliance_network_lifecycle *lifecycle);
const struct appliance_dhcp_client *
appliance_network_lifecycle_dhcp(
    const struct appliance_network_lifecycle *lifecycle);
const struct appliance_dns_resolver *
appliance_network_lifecycle_dns(
    const struct appliance_network_lifecycle *lifecycle);
enum appliance_network_lifecycle_reason
appliance_network_lifecycle_reason(
    const struct appliance_network_lifecycle *lifecycle);
uint32_t appliance_network_lifecycle_hold_count(
    const struct appliance_network_lifecycle *lifecycle);
const char *appliance_network_lifecycle_reason_string(
    enum appliance_network_lifecycle_reason reason);
const char *appliance_network_lifecycle_state_string(
    enum appliance_network_lifecycle_state state);
const char *appliance_network_lifecycle_status_string(
    enum appliance_network_lifecycle_status status);

#endif
