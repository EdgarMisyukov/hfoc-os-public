#include "appliance_network_lifecycle.h"

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

static void appliance_network_lifecycle_zero_bytes(
    void *destination,
    size_t length)
{
    unsigned char *bytes = destination;
    size_t index;

    for (index = 0U; index < length; ++index) {
        bytes[index] = 0U;
    }
}

static void appliance_network_lifecycle_copy_bytes(
    void *destination,
    const void *source,
    size_t length)
{
    unsigned char *output = destination;
    const unsigned char *input = source;
    size_t index;

    for (index = 0U; index < length; ++index) {
        output[index] = input[index];
    }
}

static size_t appliance_network_lifecycle_string_length(
    const char *text)
{
    size_t length = 0U;

    while (text[length] != '\0') {
        ++length;
    }
    return length;
}

static bool appliance_network_lifecycle_published(
    const struct appliance_network_lifecycle *lifecycle)
{
    return lifecycle != NULL &&
           lifecycle->initialized &&
           lifecycle->version == APPLIANCE_NETWORK_LIFECYCLE_VERSION;
}

static bool appliance_network_lifecycle_reachability_valid(
    enum appliance_network_reachability reachability)
{
    return reachability == APPLIANCE_NETWORK_REACHABILITY_UNKNOWN ||
           reachability == APPLIANCE_NETWORK_REACHABILITY_REACHABLE ||
           reachability == APPLIANCE_NETWORK_REACHABILITY_UNREACHABLE;
}

static bool appliance_network_lifecycle_address_nonzero(
    const struct network_ipv4_address *address)
{
    size_t index;

    for (index = 0U;
         index < NETWORK_IPV4_ADDRESS_OCTETS;
         ++index) {
        if (address->octets[index] != 0U) {
            return true;
        }
    }
    return false;
}

static const char *appliance_network_lifecycle_link_string(
    enum network_link_state state)
{
    switch (state) {
    case NETWORK_LINK_DOWN:
        return "DOWN";
    case NETWORK_LINK_INITIALIZING:
        return "INITIALIZING";
    case NETWORK_LINK_UP:
        return "UP";
    case NETWORK_LINK_FAILED:
        return "FAILED";
    default:
        return NULL;
    }
}

static const char *appliance_network_lifecycle_health_string(
    const struct appliance_network_lifecycle *lifecycle)
{
    switch (lifecycle->diagnostics.result) {
    case HEALTH_CHECK_RESULT_UNKNOWN:
        return "UNKNOWN";
    case HEALTH_CHECK_RESULT_PASS:
        return "PASS";
    case HEALTH_CHECK_RESULT_WARN:
        return "WARN";
    case HEALTH_CHECK_RESULT_FAIL:
        return "FAIL";
    default:
        return "UNKNOWN";
    }
}

static enum appliance_network_lifecycle_status
appliance_network_lifecycle_map_dhcp_status(
    enum appliance_dhcp_client_status status)
{
    switch (status) {
    case APPLIANCE_DHCP_CLIENT_OK:
        return APPLIANCE_NETWORK_LIFECYCLE_OK;
    case APPLIANCE_DHCP_CLIENT_NULL:
        return APPLIANCE_NETWORK_LIFECYCLE_NULL;
    case APPLIANCE_DHCP_CLIENT_BAD_STATE:
    case APPLIANCE_DHCP_CLIENT_STOPPED:
        return APPLIANCE_NETWORK_LIFECYCLE_BAD_STATE;
    case APPLIANCE_DHCP_CLIENT_BAD_VERSION:
        return APPLIANCE_NETWORK_LIFECYCLE_BAD_VERSION;
    case APPLIANCE_DHCP_CLIENT_BAD_CONFIG:
    case APPLIANCE_DHCP_CLIENT_BAD_LEASE:
        return APPLIANCE_NETWORK_LIFECYCLE_BAD_DHCP;
    case APPLIANCE_DHCP_CLIENT_EXHAUSTED:
        return APPLIANCE_NETWORK_LIFECYCLE_EXHAUSTED;
    case APPLIANCE_DHCP_CLIENT_BUFFER_TOO_SMALL:
        return APPLIANCE_NETWORK_LIFECYCLE_BUFFER_TOO_SMALL;
    default:
        return APPLIANCE_NETWORK_LIFECYCLE_BAD_DHCP;
    }
}

static enum appliance_network_lifecycle_status
appliance_network_lifecycle_map_dns_status(
    enum appliance_dns_resolver_status status)
{
    switch (status) {
    case APPLIANCE_DNS_RESOLVER_OK:
        return APPLIANCE_NETWORK_LIFECYCLE_OK;
    case APPLIANCE_DNS_RESOLVER_NULL:
        return APPLIANCE_NETWORK_LIFECYCLE_NULL;
    case APPLIANCE_DNS_RESOLVER_BAD_STATE:
    case APPLIANCE_DNS_RESOLVER_STOPPED:
        return APPLIANCE_NETWORK_LIFECYCLE_BAD_STATE;
    case APPLIANCE_DNS_RESOLVER_BAD_VERSION:
        return APPLIANCE_NETWORK_LIFECYCLE_BAD_VERSION;
    case APPLIANCE_DNS_RESOLVER_BAD_CONFIG:
    case APPLIANCE_DNS_RESOLVER_BAD_HOSTNAME:
    case APPLIANCE_DNS_RESOLVER_BAD_ANSWER:
        return APPLIANCE_NETWORK_LIFECYCLE_BAD_DNS;
    case APPLIANCE_DNS_RESOLVER_EXHAUSTED:
        return APPLIANCE_NETWORK_LIFECYCLE_EXHAUSTED;
    case APPLIANCE_DNS_RESOLVER_BUFFER_TOO_SMALL:
        return APPLIANCE_NETWORK_LIFECYCLE_BUFFER_TOO_SMALL;
    default:
        return APPLIANCE_NETWORK_LIFECYCLE_BAD_DNS;
    }
}

static void appliance_network_lifecycle_clear_active(
    struct appliance_network_lifecycle *lifecycle)
{
    if (lifecycle->has_active &&
        lifecycle->transport_installed &&
        lifecycle->transport.deactivate_ipv4 != NULL &&
        !lifecycle->transport.deactivate_ipv4(
            lifecycle->transport.context)) {
        lifecycle->last_status =
            APPLIANCE_NETWORK_LIFECYCLE_TRANSPORT_FAILED;
    }
    appliance_network_lifecycle_zero_bytes(
        &lifecycle->active, sizeof(lifecycle->active));
    lifecycle->has_active = false;
    if (lifecycle->dns_initialized) {
        (void)appliance_dns_resolver_reset(&lifecycle->dns);
    }
    lifecycle->dns_initialized = false;
}

static void appliance_network_lifecycle_publish_diagnostics(
    struct appliance_network_lifecycle *lifecycle)
{
    struct appliance_network_diagnostics diagnostics;
    enum appliance_network_diagnostics_status status;

    if (!lifecycle->link_published) {
        appliance_network_lifecycle_zero_bytes(
            &lifecycle->diagnostics,
            sizeof(lifecycle->diagnostics));
        return;
    }
    appliance_network_lifecycle_zero_bytes(
        &diagnostics, sizeof(diagnostics));
    status = appliance_network_diagnostics_publish(
        &diagnostics,
        &lifecycle->desired,
        &lifecycle->link,
        lifecycle->has_active ? &lifecycle->active : NULL,
        lifecycle->gateway_reachability,
        lifecycle->dns_reachability);
    if (status == APPLIANCE_NETWORK_DIAGNOSTICS_OK) {
        appliance_network_lifecycle_copy_bytes(
            &lifecycle->diagnostics,
            &diagnostics,
            sizeof(diagnostics));
    }
}

static bool appliance_network_lifecycle_init_dns(
    struct appliance_network_lifecycle *lifecycle)
{
    if (!lifecycle->has_active ||
        !appliance_network_lifecycle_address_nonzero(
            &lifecycle->active.dns)) {
        lifecycle->dns_initialized = false;
        return true;
    }
    appliance_network_lifecycle_zero_bytes(
        &lifecycle->dns, sizeof(lifecycle->dns));
    if (appliance_dns_resolver_init(
            &lifecycle->dns,
            &lifecycle->active,
            lifecycle->desired.interface,
            lifecycle->desired.interface_length) !=
        APPLIANCE_DNS_RESOLVER_OK) {
        return false;
    }
    lifecycle->dns_initialized = true;
    return true;
}

static enum appliance_network_lifecycle_status
appliance_network_lifecycle_activate(
    struct appliance_network_lifecycle *lifecycle,
    const struct network_ipv4_config *config)
{
    if (!lifecycle->transport_installed ||
        lifecycle->transport.activate_ipv4 == NULL) {
        lifecycle->last_status =
            APPLIANCE_NETWORK_LIFECYCLE_TRANSPORT_UNAVAILABLE;
        return lifecycle->last_status;
    }
    if (!lifecycle->transport.activate_ipv4(
            lifecycle->transport.context, config)) {
        lifecycle->last_status =
            APPLIANCE_NETWORK_LIFECYCLE_TRANSPORT_FAILED;
        return lifecycle->last_status;
    }
    appliance_network_lifecycle_copy_bytes(
        &lifecycle->active, config, sizeof(lifecycle->active));
    lifecycle->has_active = true;
    lifecycle->state = APPLIANCE_NETWORK_LIFECYCLE_STATE_ACTIVE;
    /*
     * ITEM-408: a completed (re-)acquisition is the healthy state again, so
     * the reason field stops naming a failure that is over.
     */
    lifecycle->reason = APPLIANCE_NETWORK_LIFECYCLE_REASON_NONE;
    lifecycle->hold_count = 0U;
    if (!appliance_network_lifecycle_init_dns(lifecycle)) {
        lifecycle->last_status = APPLIANCE_NETWORK_LIFECYCLE_BAD_DNS;
        return lifecycle->last_status;
    }
    appliance_network_lifecycle_publish_diagnostics(lifecycle);
    lifecycle->last_status = APPLIANCE_NETWORK_LIFECYCLE_OK;
    return APPLIANCE_NETWORK_LIFECYCLE_OK;
}

static bool appliance_network_lifecycle_link_up(
    const struct appliance_network_lifecycle *lifecycle)
{
    return lifecycle->link_published &&
           lifecycle->link.state == NETWORK_LINK_UP;
}

/*
 * ITEM-408: the only path that gives up an applied address.
 *
 * It is reached when the network itself says the address is gone - an
 * explicit DHCP refusal, or a link sample that stayed down - or when there is
 * no live address to lose (a first acquisition that never completed).  Every
 * other failure of the addressing or probing path is a transient that
 * appliance_network_lifecycle_hold() survives, because the address the
 * appliance is already serving is not what failed.
 */
static void appliance_network_lifecycle_fail(
    struct appliance_network_lifecycle *lifecycle,
    enum appliance_network_lifecycle_status status,
    enum appliance_network_lifecycle_reason reason)
{
    appliance_network_lifecycle_clear_active(lifecycle);
    if (lifecycle->dhcp_initialized) {
        (void)appliance_dhcp_client_reset(&lifecycle->dhcp);
    }
    lifecycle->dhcp_initialized = false;
    lifecycle->state = APPLIANCE_NETWORK_LIFECYCLE_STATE_FAILED;
    lifecycle->last_status = status;
    lifecycle->reason = reason;
    lifecycle->hold_count = 0U;
    appliance_network_lifecycle_publish_diagnostics(lifecycle);
}

static bool appliance_network_lifecycle_reacquire(
    struct appliance_network_lifecycle *lifecycle)
{
    enum appliance_dhcp_client_status dhcp_status;

    appliance_network_lifecycle_zero_bytes(
        &lifecycle->dhcp, sizeof(lifecycle->dhcp));
    lifecycle->dhcp_initialized = false;
    dhcp_status = appliance_dhcp_client_init(
        &lifecycle->dhcp, &lifecycle->desired);
    if (dhcp_status != APPLIANCE_DHCP_CLIENT_OK) {
        return false;
    }
    if (appliance_dhcp_client_start(&lifecycle->dhcp) !=
        APPLIANCE_DHCP_CLIENT_OK) {
        appliance_network_lifecycle_zero_bytes(
            &lifecycle->dhcp, sizeof(lifecycle->dhcp));
        return false;
    }
    lifecycle->dhcp_initialized = true;
    /*
     * The held address keeps its place in the transport: the fresh cycle
     * starts while the netif still carries it, so the panel keeps answering
     * during the whole re-acquisition.
     */
    lifecycle->state = APPLIANCE_NETWORK_LIFECYCLE_STATE_ADDRESSING;
    if (!lifecycle->transport_installed ||
        lifecycle->transport.dhcp_discover == NULL) {
        return false;
    }
    /*
     * A discover that could not be transmitted leaves the cycle parked; the
     * wire's paced kick retries it while the held address keeps serving.
     */
    return lifecycle->transport.dhcp_discover(lifecycle->transport.context);
}

/*
 * ITEM-408: a live address survives a transient failure.
 *
 * A missed renewal, a DHCP server that stopped answering, a transmit that
 * failed or an unanswered DNS probe are facts about the network path, not
 * about the address the appliance is already using and serving.  Nothing is
 * deactivated, no lease is dropped, the failed step is retried, and the path
 * that failed plus the number of consecutive failures survived are published.
 *
 * `reacquire` re-enters ADDRESSING - while the held address stays applied -
 * and starts a fresh DHCP cycle, because a renewal the server never answered
 * leaves the client with nothing to renew.  The next acknowledgement
 * re-applies the lease: the same address unless the server moved us.
 */
static enum appliance_network_lifecycle_status
appliance_network_lifecycle_hold(
    struct appliance_network_lifecycle *lifecycle,
    enum appliance_network_lifecycle_status status,
    enum appliance_network_lifecycle_reason reason,
    bool reacquire)
{
    ++lifecycle->hold_count;
    lifecycle->reason = reason;
    lifecycle->last_status = status;
    lifecycle->state = APPLIANCE_NETWORK_LIFECYCLE_STATE_ACTIVE;
    if (reacquire &&
        lifecycle->desired.mode == APPLIANCE_NETWORK_CONFIG_MODE_DHCP) {
        (void)appliance_network_lifecycle_reacquire(lifecycle);
    }
    appliance_network_lifecycle_publish_diagnostics(lifecycle);
    return status;
}

/*
 * The one place that decides between giving an address up and holding it.
 */
static enum appliance_network_lifecycle_status
appliance_network_lifecycle_failure(
    struct appliance_network_lifecycle *lifecycle,
    enum appliance_network_lifecycle_status status,
    enum appliance_network_lifecycle_reason reason,
    bool reacquire)
{
    if (lifecycle->has_active &&
        appliance_network_lifecycle_link_up(lifecycle)) {
        return appliance_network_lifecycle_hold(
            lifecycle, status, reason, reacquire);
    }
    appliance_network_lifecycle_fail(lifecycle, status, reason);
    return status;
}

/*
 * ITEM-428: a frame the transmit path refused is not the end of the
 * addressing cycle.
 *
 * The 2026-09-16 hardware run's own log ended on a record that said
 * `state=ADDRESSING lease=NONE reason=NONE attempt=11/40`: the machine was
 * still asking, and nothing said that its frames had stopped reaching the
 * wire.  A refused transmit is a fact about the path, not about the cycle: the
 * reason and the failure count are published, the cycle keeps its bounded
 * attempt budget, and the appliance keeps asking - so a transmit path that
 * comes back (the driver's own bounded ring restart, for one) resumes the
 * acquisition instead of finding it already failed.
 */
enum appliance_network_lifecycle_status
appliance_network_lifecycle_dhcp_transport_refused(
    struct appliance_network_lifecycle *lifecycle)
{
    if (lifecycle == NULL) {
        return APPLIANCE_NETWORK_LIFECYCLE_NULL;
    }
    if (!appliance_network_lifecycle_published(lifecycle) ||
        !lifecycle->dhcp_initialized) {
        return APPLIANCE_NETWORK_LIFECYCLE_BAD_STATE;
    }
    lifecycle->reason = APPLIANCE_NETWORK_LIFECYCLE_REASON_DHCP_TRANSPORT;
    lifecycle->last_status =
        APPLIANCE_NETWORK_LIFECYCLE_TRANSPORT_UNAVAILABLE;
    if (lifecycle->has_active) {
        ++lifecycle->hold_count;
    }
    appliance_network_lifecycle_publish_diagnostics(lifecycle);
    return APPLIANCE_NETWORK_LIFECYCLE_TRANSPORT_UNAVAILABLE;
}

/*
 * ITEM-408: the DNS probe observes reachability; it never owns the address.
 * A probe that timed out, was refused or could not be transmitted is
 * published as an unreachable DNS path - the diagnostics record and the
 * reason field say so - and the live address, the netif and the panel keep
 * running.
 */
static enum appliance_network_lifecycle_status
appliance_network_lifecycle_dns_failure(
    struct appliance_network_lifecycle *lifecycle,
    enum appliance_network_lifecycle_status status,
    enum appliance_network_lifecycle_reason reason)
{
    lifecycle->dns_reachability = APPLIANCE_NETWORK_REACHABILITY_UNREACHABLE;
    lifecycle->reason = reason;
    lifecycle->last_status = status;
    if (lifecycle->has_active) {
        ++lifecycle->hold_count;
    }
    appliance_network_lifecycle_publish_diagnostics(lifecycle);
    return status;
}

static bool appliance_network_lifecycle_dhcp_action(
    struct appliance_network_lifecycle *lifecycle)
{
    switch (appliance_dhcp_client_state(&lifecycle->dhcp)) {
    case APPLIANCE_DHCP_CLIENT_STATE_DISCOVERING:
        return lifecycle->transport.dhcp_discover(
            lifecycle->transport.context);
    case APPLIANCE_DHCP_CLIENT_STATE_REQUESTING:
        return lifecycle->transport.dhcp_request(
            lifecycle->transport.context);
    case APPLIANCE_DHCP_CLIENT_STATE_RENEWING:
        return lifecycle->transport.dhcp_renew(
            lifecycle->transport.context);
    case APPLIANCE_DHCP_CLIENT_STATE_REBINDING:
        return lifecycle->transport.dhcp_rebind(
            lifecycle->transport.context);
    default:
        return true;
    }
}

enum appliance_network_lifecycle_status
appliance_network_lifecycle_init(
    struct appliance_network_lifecycle *lifecycle,
    const struct appliance_network_config *desired,
    const struct network_device_snapshot *device,
    const struct appliance_network_lifecycle_transport *transport)
{
    if (lifecycle == NULL || desired == NULL || device == NULL) {
        return APPLIANCE_NETWORK_LIFECYCLE_NULL;
    }
    if (lifecycle->initialized) {
        return APPLIANCE_NETWORK_LIFECYCLE_BAD_STATE;
    }
    if (appliance_network_config_validate(desired) !=
            APPLIANCE_NETWORK_CONFIG_OK ||
        (desired->mode != APPLIANCE_NETWORK_CONFIG_MODE_STATIC &&
         desired->mode != APPLIANCE_NETWORK_CONFIG_MODE_DHCP)) {
        return APPLIANCE_NETWORK_LIFECYCLE_BAD_CONFIG;
    }
    if (network_device_snapshot_validate(device) !=
        NETWORK_DEVICE_OK) {
        return APPLIANCE_NETWORK_LIFECYCLE_BAD_DEVICE;
    }
    if (transport != NULL) {
        if (transport->version !=
            APPLIANCE_NETWORK_LIFECYCLE_VERSION) {
            return APPLIANCE_NETWORK_LIFECYCLE_BAD_VERSION;
        }
        if (transport->activate_ipv4 == NULL ||
            transport->deactivate_ipv4 == NULL ||
            transport->dhcp_discover == NULL ||
            transport->dhcp_request == NULL ||
            transport->dhcp_renew == NULL ||
            transport->dhcp_rebind == NULL ||
            transport->dns_query == NULL) {
            return APPLIANCE_NETWORK_LIFECYCLE_BAD_VERSION;
        }
    }

    appliance_network_lifecycle_zero_bytes(
        lifecycle, sizeof(*lifecycle));
    appliance_network_lifecycle_copy_bytes(
        &lifecycle->desired, desired,
        sizeof(lifecycle->desired));
    appliance_network_lifecycle_copy_bytes(
        &lifecycle->device, device,
        sizeof(lifecycle->device));
    if (transport != NULL) {
        appliance_network_lifecycle_copy_bytes(
            &lifecycle->transport, transport,
            sizeof(lifecycle->transport));
        lifecycle->transport_installed = true;
    }
    lifecycle->version = APPLIANCE_NETWORK_LIFECYCLE_VERSION;
    lifecycle->initialized = true;
    lifecycle->state = APPLIANCE_NETWORK_LIFECYCLE_STATE_PREPARED;
    lifecycle->gateway_reachability =
        APPLIANCE_NETWORK_REACHABILITY_UNKNOWN;
    lifecycle->dns_reachability =
        APPLIANCE_NETWORK_REACHABILITY_UNKNOWN;
    lifecycle->last_status = APPLIANCE_NETWORK_LIFECYCLE_OK;
    return APPLIANCE_NETWORK_LIFECYCLE_OK;
}

enum appliance_network_lifecycle_status
appliance_network_lifecycle_start(
    struct appliance_network_lifecycle *lifecycle)
{
    if (lifecycle == NULL) {
        return APPLIANCE_NETWORK_LIFECYCLE_NULL;
    }
    if (!appliance_network_lifecycle_published(lifecycle)) {
        return APPLIANCE_NETWORK_LIFECYCLE_BAD_STATE;
    }
    if (lifecycle->state !=
        APPLIANCE_NETWORK_LIFECYCLE_STATE_PREPARED) {
        return APPLIANCE_NETWORK_LIFECYCLE_BAD_STATE;
    }
    lifecycle->started = true;
    lifecycle->state = APPLIANCE_NETWORK_LIFECYCLE_STATE_STARTED;
    lifecycle->last_status = APPLIANCE_NETWORK_LIFECYCLE_OK;
    return APPLIANCE_NETWORK_LIFECYCLE_OK;
}

enum appliance_network_lifecycle_status
appliance_network_lifecycle_link(
    struct appliance_network_lifecycle *lifecycle,
    const struct network_link_state_record *link)
{
    enum appliance_network_lifecycle_status status;
    enum appliance_dhcp_client_status dhcp_status;
    bool had_active;

    if (lifecycle == NULL || link == NULL) {
        return APPLIANCE_NETWORK_LIFECYCLE_NULL;
    }
    if (!appliance_network_lifecycle_published(lifecycle) ||
        !lifecycle->started) {
        return APPLIANCE_NETWORK_LIFECYCLE_BAD_STATE;
    }
    if (network_link_state_validate(link) != NETWORK_LINK_OK ||
        link->device_id != lifecycle->device.device_id) {
        return APPLIANCE_NETWORK_LIFECYCLE_BAD_LINK;
    }
    if (lifecycle->link_published &&
        link->generation < lifecycle->link.generation) {
        lifecycle->last_status = APPLIANCE_NETWORK_LIFECYCLE_BAD_LINK;
        return APPLIANCE_NETWORK_LIFECYCLE_BAD_LINK;
    }
    if (lifecycle->state == APPLIANCE_NETWORK_LIFECYCLE_STATE_FAILED &&
        link->state != NETWORK_LINK_DOWN) {
        return APPLIANCE_NETWORK_LIFECYCLE_BAD_STATE;
    }
    if (lifecycle->state == APPLIANCE_NETWORK_LIFECYCLE_STATE_ACTIVE &&
        link->state == NETWORK_LINK_UP &&
        lifecycle->link_published &&
        lifecycle->link.generation == link->generation) {
        return APPLIANCE_NETWORK_LIFECYCLE_OK;
    }

    had_active = lifecycle->has_active;
    appliance_network_lifecycle_clear_active(lifecycle);
    if (lifecycle->dhcp_initialized) {
        (void)appliance_dhcp_client_reset(&lifecycle->dhcp);
    }
    lifecycle->dhcp_initialized = false;
    lifecycle->dns_initialized = false;
    appliance_network_lifecycle_copy_bytes(
        &lifecycle->link, link, sizeof(lifecycle->link));
    lifecycle->link_published = true;

    if (link->state == NETWORK_LINK_DOWN ||
        link->state == NETWORK_LINK_INITIALIZING) {
        /*
         * ITEM-408: a link record that took an applied address away says
         * which sample it was, so the record names the loss instead of
         * leaving an operator to guess.  A record that arrives before any
         * address was applied names nothing, because nothing was left.
         */
        if (had_active) {
            lifecycle->reason =
                link->state == NETWORK_LINK_DOWN
                    ? APPLIANCE_NETWORK_LIFECYCLE_REASON_LINK_DOWN_SAMPLE
                    : APPLIANCE_NETWORK_LIFECYCLE_REASON_LINK_INITIALIZING_SAMPLE;
            lifecycle->hold_count = 0U;
        }
        lifecycle->state =
            APPLIANCE_NETWORK_LIFECYCLE_STATE_WAITING_FOR_LINK;
        lifecycle->last_status = APPLIANCE_NETWORK_LIFECYCLE_OK;
        appliance_network_lifecycle_publish_diagnostics(lifecycle);
        return APPLIANCE_NETWORK_LIFECYCLE_OK;
    }
    if (link->state == NETWORK_LINK_FAILED) {
        if (had_active) {
            lifecycle->reason =
                APPLIANCE_NETWORK_LIFECYCLE_REASON_LINK_FAILED_SAMPLE;
            lifecycle->hold_count = 0U;
        }
        lifecycle->state = APPLIANCE_NETWORK_LIFECYCLE_STATE_FAILED;
        lifecycle->last_status = APPLIANCE_NETWORK_LIFECYCLE_OK;
        appliance_network_lifecycle_publish_diagnostics(lifecycle);
        return APPLIANCE_NETWORK_LIFECYCLE_OK;
    }
    if (link->state != NETWORK_LINK_UP) {
        return APPLIANCE_NETWORK_LIFECYCLE_BAD_LINK;
    }
    if (lifecycle->state !=
            APPLIANCE_NETWORK_LIFECYCLE_STATE_STARTED &&
        lifecycle->state !=
            APPLIANCE_NETWORK_LIFECYCLE_STATE_WAITING_FOR_LINK) {
        return APPLIANCE_NETWORK_LIFECYCLE_BAD_STATE;
    }

    lifecycle->state = APPLIANCE_NETWORK_LIFECYCLE_STATE_ADDRESSING;
    if (lifecycle->desired.mode ==
        APPLIANCE_NETWORK_CONFIG_MODE_STATIC) {
        if (!lifecycle->transport_installed ||
            lifecycle->transport.activate_ipv4 == NULL) {
            status = appliance_network_lifecycle_failure(
                lifecycle,
                APPLIANCE_NETWORK_LIFECYCLE_TRANSPORT_UNAVAILABLE,
                APPLIANCE_NETWORK_LIFECYCLE_REASON_TRANSPORT_UNAVAILABLE,
                false);
            return status;
        }
        status = appliance_network_lifecycle_activate(
            lifecycle, &lifecycle->desired.static_config);
        if (status != APPLIANCE_NETWORK_LIFECYCLE_OK) {
            (void)appliance_network_lifecycle_failure(
                lifecycle, status,
                APPLIANCE_NETWORK_LIFECYCLE_REASON_ADDRESS_ACTIVATION,
                false);
        }
        return status;
    }

    appliance_network_lifecycle_zero_bytes(
        &lifecycle->dhcp, sizeof(lifecycle->dhcp));
    dhcp_status = appliance_dhcp_client_init(
        &lifecycle->dhcp, &lifecycle->desired);
    if (dhcp_status != APPLIANCE_DHCP_CLIENT_OK) {
        status = appliance_network_lifecycle_map_dhcp_status(
            dhcp_status);
        return appliance_network_lifecycle_failure(
            lifecycle, status,
            APPLIANCE_NETWORK_LIFECYCLE_REASON_DHCP_TRANSPORT, true);
    }
    lifecycle->dhcp_initialized = true;
    dhcp_status = appliance_dhcp_client_start(&lifecycle->dhcp);
    if (dhcp_status != APPLIANCE_DHCP_CLIENT_OK) {
        status = appliance_network_lifecycle_map_dhcp_status(
            dhcp_status);
        return appliance_network_lifecycle_failure(
            lifecycle, status,
            APPLIANCE_NETWORK_LIFECYCLE_REASON_DHCP_TRANSPORT, true);
    }
    if (!lifecycle->transport_installed ||
        lifecycle->transport.dhcp_discover == NULL) {
        return appliance_network_lifecycle_failure(
            lifecycle,
            APPLIANCE_NETWORK_LIFECYCLE_TRANSPORT_UNAVAILABLE,
            APPLIANCE_NETWORK_LIFECYCLE_REASON_TRANSPORT_UNAVAILABLE,
            false);
    }
    if (!lifecycle->transport.dhcp_discover(
            lifecycle->transport.context)) {
        return appliance_network_lifecycle_failure(
            lifecycle,
            APPLIANCE_NETWORK_LIFECYCLE_TRANSPORT_FAILED,
            APPLIANCE_NETWORK_LIFECYCLE_REASON_DHCP_TRANSPORT, true);
    }
    appliance_network_lifecycle_publish_diagnostics(lifecycle);
    lifecycle->last_status = APPLIANCE_NETWORK_LIFECYCLE_OK;
    return APPLIANCE_NETWORK_LIFECYCLE_OK;
}

enum appliance_network_lifecycle_status
appliance_network_lifecycle_dhcp_offer(
    struct appliance_network_lifecycle *lifecycle,
    const struct network_ipv4_config *candidate)
{
    enum appliance_dhcp_client_status dhcp_status;

    if (lifecycle == NULL || candidate == NULL) {
        return APPLIANCE_NETWORK_LIFECYCLE_NULL;
    }
    if (!appliance_network_lifecycle_published(lifecycle) ||
        !lifecycle->dhcp_initialized ||
        lifecycle->desired.mode !=
            APPLIANCE_NETWORK_CONFIG_MODE_DHCP ||
        lifecycle->state !=
            APPLIANCE_NETWORK_LIFECYCLE_STATE_ADDRESSING) {
        return APPLIANCE_NETWORK_LIFECYCLE_BAD_STATE;
    }
    dhcp_status = appliance_dhcp_client_offer(
        &lifecycle->dhcp, candidate);
    if (dhcp_status != APPLIANCE_DHCP_CLIENT_OK) {
        enum appliance_network_lifecycle_status status =
            appliance_network_lifecycle_map_dhcp_status(dhcp_status);
        lifecycle->last_status = status;
        return status;
    }
    if (!lifecycle->transport_installed ||
        lifecycle->transport.dhcp_request == NULL ||
        !lifecycle->transport.dhcp_request(
            lifecycle->transport.context)) {
        return appliance_network_lifecycle_failure(
            lifecycle,
            APPLIANCE_NETWORK_LIFECYCLE_TRANSPORT_UNAVAILABLE,
            APPLIANCE_NETWORK_LIFECYCLE_REASON_DHCP_TRANSPORT, true);
    }
    lifecycle->last_status = APPLIANCE_NETWORK_LIFECYCLE_OK;
    return APPLIANCE_NETWORK_LIFECYCLE_OK;
}

enum appliance_network_lifecycle_status
appliance_network_lifecycle_dhcp_ack(
    struct appliance_network_lifecycle *lifecycle,
    const struct network_ipv4_config *lease)
{
    enum appliance_dhcp_client_status dhcp_status;

    if (lifecycle == NULL || lease == NULL) {
        return APPLIANCE_NETWORK_LIFECYCLE_NULL;
    }
    if (!appliance_network_lifecycle_published(lifecycle) ||
        !lifecycle->dhcp_initialized ||
        lifecycle->desired.mode !=
            APPLIANCE_NETWORK_CONFIG_MODE_DHCP ||
        (lifecycle->state !=
             APPLIANCE_NETWORK_LIFECYCLE_STATE_ADDRESSING &&
         !(lifecycle->state ==
               APPLIANCE_NETWORK_LIFECYCLE_STATE_ACTIVE &&
           (appliance_dhcp_client_state(&lifecycle->dhcp) ==
                APPLIANCE_DHCP_CLIENT_STATE_RENEWING ||
            appliance_dhcp_client_state(&lifecycle->dhcp) ==
                APPLIANCE_DHCP_CLIENT_STATE_REBINDING)))) {
        return APPLIANCE_NETWORK_LIFECYCLE_BAD_STATE;
    }
    dhcp_status = appliance_dhcp_client_ack(
        &lifecycle->dhcp, lease);
    if (dhcp_status != APPLIANCE_DHCP_CLIENT_OK) {
        enum appliance_network_lifecycle_status status =
            appliance_network_lifecycle_map_dhcp_status(dhcp_status);
        if (status == APPLIANCE_NETWORK_LIFECYCLE_EXHAUSTED) {
            return appliance_network_lifecycle_failure(
                lifecycle, status,
                APPLIANCE_NETWORK_LIFECYCLE_REASON_DHCP_TRANSPORT, true);
        }
        lifecycle->last_status = status;
        return lifecycle->last_status;
    }
    {
        enum appliance_network_lifecycle_status status =
            appliance_network_lifecycle_activate(lifecycle, lease);

        if (status != APPLIANCE_NETWORK_LIFECYCLE_OK) {
            (void)appliance_network_lifecycle_failure(
                lifecycle, status,
                APPLIANCE_NETWORK_LIFECYCLE_REASON_ADDRESS_ACTIVATION, true);
        }
        return status;
    }
}

enum appliance_network_lifecycle_status
appliance_network_lifecycle_dhcp_nak(
    struct appliance_network_lifecycle *lifecycle)
{
    enum appliance_dhcp_client_status dhcp_status;
    enum appliance_network_lifecycle_status status;

    if (lifecycle == NULL) {
        return APPLIANCE_NETWORK_LIFECYCLE_NULL;
    }
    if (!appliance_network_lifecycle_published(lifecycle) ||
        !lifecycle->dhcp_initialized ||
        lifecycle->desired.mode !=
            APPLIANCE_NETWORK_CONFIG_MODE_DHCP ||
        (lifecycle->state !=
             APPLIANCE_NETWORK_LIFECYCLE_STATE_ADDRESSING &&
         !(lifecycle->state ==
               APPLIANCE_NETWORK_LIFECYCLE_STATE_ACTIVE &&
           (appliance_dhcp_client_state(&lifecycle->dhcp) ==
                APPLIANCE_DHCP_CLIENT_STATE_RENEWING ||
            appliance_dhcp_client_state(&lifecycle->dhcp) ==
                APPLIANCE_DHCP_CLIENT_STATE_REBINDING)))) {
        return APPLIANCE_NETWORK_LIFECYCLE_BAD_STATE;
    }
    dhcp_status = appliance_dhcp_client_nak(&lifecycle->dhcp);
    if (dhcp_status == APPLIANCE_DHCP_CLIENT_EXHAUSTED) {
        /*
         * ITEM-408: an explicit DHCPNAK is the network saying this address is
         * not ours.  It is not a transient: it is the one refusal that is
         * allowed to take the address away, and it is named as such.
         */
        appliance_network_lifecycle_fail(
            lifecycle, APPLIANCE_NETWORK_LIFECYCLE_EXHAUSTED,
            APPLIANCE_NETWORK_LIFECYCLE_REASON_DHCP_REFUSED);
        return lifecycle->last_status;
    }
    if (dhcp_status != APPLIANCE_DHCP_CLIENT_OK) {
        status = appliance_network_lifecycle_map_dhcp_status(
            dhcp_status);
        lifecycle->last_status = status;
        return status;
    }
    if (appliance_dhcp_client_state(&lifecycle->dhcp) ==
        APPLIANCE_DHCP_CLIENT_STATE_IDLE) {
        if (lifecycle->has_active) {
            appliance_network_lifecycle_clear_active(lifecycle);
            /*
             * ITEM-408: the refusal is the reason this address was given up,
             * whether or not the restart that follows it succeeds.
             */
            lifecycle->reason =
                APPLIANCE_NETWORK_LIFECYCLE_REASON_DHCP_REFUSED;
            lifecycle->hold_count = 0U;
        }
        lifecycle->state =
            APPLIANCE_NETWORK_LIFECYCLE_STATE_ADDRESSING;
        dhcp_status = appliance_dhcp_client_start(&lifecycle->dhcp);
        if (dhcp_status != APPLIANCE_DHCP_CLIENT_OK) {
            status = appliance_network_lifecycle_map_dhcp_status(
                dhcp_status);
            return appliance_network_lifecycle_failure(
                lifecycle, status,
                APPLIANCE_NETWORK_LIFECYCLE_REASON_DHCP_REFUSED, false);
        }
        if (!lifecycle->transport.dhcp_discover(
                lifecycle->transport.context)) {
            return appliance_network_lifecycle_failure(
                lifecycle,
                APPLIANCE_NETWORK_LIFECYCLE_TRANSPORT_FAILED,
                APPLIANCE_NETWORK_LIFECYCLE_REASON_DHCP_TRANSPORT, false);
        }
    }
    appliance_network_lifecycle_publish_diagnostics(lifecycle);
    lifecycle->last_status = APPLIANCE_NETWORK_LIFECYCLE_OK;
    return APPLIANCE_NETWORK_LIFECYCLE_OK;
}

enum appliance_network_lifecycle_status
appliance_network_lifecycle_dhcp_timeout(
    struct appliance_network_lifecycle *lifecycle,
    enum appliance_network_lifecycle_dhcp_timeout timeout)
{
    enum appliance_dhcp_client_status dhcp_status;
    enum appliance_network_lifecycle_status status;

    if (lifecycle == NULL) {
        return APPLIANCE_NETWORK_LIFECYCLE_NULL;
    }
    if (!appliance_network_lifecycle_published(lifecycle) ||
        !lifecycle->dhcp_initialized ||
        lifecycle->desired.mode !=
            APPLIANCE_NETWORK_CONFIG_MODE_DHCP ||
        (lifecycle->state !=
             APPLIANCE_NETWORK_LIFECYCLE_STATE_ADDRESSING &&
         (lifecycle->state !=
              APPLIANCE_NETWORK_LIFECYCLE_STATE_ACTIVE ||
          (timeout !=
               APPLIANCE_NETWORK_LIFECYCLE_DHCP_TIMEOUT_RENEW &&
           timeout !=
               APPLIANCE_NETWORK_LIFECYCLE_DHCP_TIMEOUT_REBIND)))) {
        return APPLIANCE_NETWORK_LIFECYCLE_BAD_STATE;
    }
    switch (timeout) {
    case APPLIANCE_NETWORK_LIFECYCLE_DHCP_TIMEOUT_DISCOVER:
        dhcp_status =
            appliance_dhcp_client_discover_timeout(&lifecycle->dhcp);
        break;
    case APPLIANCE_NETWORK_LIFECYCLE_DHCP_TIMEOUT_REQUEST:
        dhcp_status =
            appliance_dhcp_client_request_timeout(&lifecycle->dhcp);
        break;
    case APPLIANCE_NETWORK_LIFECYCLE_DHCP_TIMEOUT_RENEW:
        dhcp_status =
            appliance_dhcp_client_renew_timeout(&lifecycle->dhcp);
        break;
    case APPLIANCE_NETWORK_LIFECYCLE_DHCP_TIMEOUT_REBIND:
        dhcp_status =
            appliance_dhcp_client_rebind_timeout(&lifecycle->dhcp);
        break;
    default:
        return APPLIANCE_NETWORK_LIFECYCLE_BAD_DHCP;
    }
    if (dhcp_status == APPLIANCE_DHCP_CLIENT_EXHAUSTED) {
        /*
         * ITEM-408: the bounded attempt budget of the addressing or refresh
         * cycle ran out.  That is a transient for an appliance that is
         * already serving an address on a link the device reports as up: the
         * address stays, the reason names the timeout, and the cycle is
         * restarted so the next server that answers puts us back in ACTIVE.
         *
         * ITEM-428: when the last failure was the transmit path itself, the
         * verdict says so.  "The budget ran out" and "the budget ran out
         * because not one frame reached the wire" are different findings for
         * the operator, and the record must not make them the same word.
         */
        const bool transport_refused =
            lifecycle->reason ==
            APPLIANCE_NETWORK_LIFECYCLE_REASON_DHCP_TRANSPORT;

        switch (timeout) {
        case APPLIANCE_NETWORK_LIFECYCLE_DHCP_TIMEOUT_DISCOVER:
            return appliance_network_lifecycle_failure(
                lifecycle, APPLIANCE_NETWORK_LIFECYCLE_EXHAUSTED,
                transport_refused
                    ? APPLIANCE_NETWORK_LIFECYCLE_REASON_DHCP_TRANSPORT
                    : APPLIANCE_NETWORK_LIFECYCLE_REASON_DISCOVER_TIMEOUT,
                true);
        case APPLIANCE_NETWORK_LIFECYCLE_DHCP_TIMEOUT_REQUEST:
            return appliance_network_lifecycle_failure(
                lifecycle, APPLIANCE_NETWORK_LIFECYCLE_EXHAUSTED,
                transport_refused
                    ? APPLIANCE_NETWORK_LIFECYCLE_REASON_DHCP_TRANSPORT
                    : APPLIANCE_NETWORK_LIFECYCLE_REASON_REQUEST_TIMEOUT,
                true);
        case APPLIANCE_NETWORK_LIFECYCLE_DHCP_TIMEOUT_RENEW:
            return appliance_network_lifecycle_failure(
                lifecycle, APPLIANCE_NETWORK_LIFECYCLE_EXHAUSTED,
                APPLIANCE_NETWORK_LIFECYCLE_REASON_RENEW_TIMEOUT, true);
        case APPLIANCE_NETWORK_LIFECYCLE_DHCP_TIMEOUT_REBIND:
            return appliance_network_lifecycle_failure(
                lifecycle, APPLIANCE_NETWORK_LIFECYCLE_EXHAUSTED,
                APPLIANCE_NETWORK_LIFECYCLE_REASON_REBIND_TIMEOUT, true);
        default:
            return APPLIANCE_NETWORK_LIFECYCLE_BAD_DHCP;
        }
    }
    if (dhcp_status != APPLIANCE_DHCP_CLIENT_OK) {
        status = appliance_network_lifecycle_map_dhcp_status(
            dhcp_status);
        lifecycle->last_status = status;
        return status;
    }
    if (!lifecycle->transport_installed ||
        !appliance_network_lifecycle_dhcp_action(lifecycle)) {
        /*
         * ITEM-428: a refused transmit is published as the reason the cycle
         * is not moving and the cycle is kept, not ended.  While the machine
         * holds no address there is nothing to hold and nothing to give up:
         * the bounded attempt budget is what ends the acquisition, and the
         * next attempt is a second away - which is exactly the time a driver
         * needs to recover a stalled transmit ring.  An appliance that does
         * hold an address keeps the ITEM-408 behaviour unchanged: the address
         * stays, the reason is published, and the failed step is retried.
         */
        if (!lifecycle->has_active) {
            return appliance_network_lifecycle_dhcp_transport_refused(
                lifecycle);
        }
        return appliance_network_lifecycle_failure(
            lifecycle,
            APPLIANCE_NETWORK_LIFECYCLE_TRANSPORT_UNAVAILABLE,
            APPLIANCE_NETWORK_LIFECYCLE_REASON_DHCP_TRANSPORT, true);
    }
    /*
     * ITEM-428: the frame reached the wire, so a transmit-path verdict is
     * over; the reason stops naming it, exactly as a DNS answer retires the
     * probe's own verdict.  A refusal that is followed by successful sends
     * must not colour the verdict of a later timeout.
     */
    if (lifecycle->reason ==
        APPLIANCE_NETWORK_LIFECYCLE_REASON_DHCP_TRANSPORT) {
        lifecycle->reason = APPLIANCE_NETWORK_LIFECYCLE_REASON_NONE;
    }
    lifecycle->last_status = APPLIANCE_NETWORK_LIFECYCLE_OK;
    return APPLIANCE_NETWORK_LIFECYCLE_OK;
}

enum appliance_network_lifecycle_status
appliance_network_lifecycle_dhcp_renew(
    struct appliance_network_lifecycle *lifecycle)
{
    enum appliance_dhcp_client_status dhcp_status;
    enum appliance_network_lifecycle_status status;

    if (lifecycle == NULL) {
        return APPLIANCE_NETWORK_LIFECYCLE_NULL;
    }
    if (!appliance_network_lifecycle_published(lifecycle) ||
        !lifecycle->dhcp_initialized ||
        lifecycle->state != APPLIANCE_NETWORK_LIFECYCLE_STATE_ACTIVE ||
        !lifecycle->has_active) {
        return APPLIANCE_NETWORK_LIFECYCLE_BAD_STATE;
    }
    dhcp_status = appliance_dhcp_client_renew(&lifecycle->dhcp);
    if (dhcp_status != APPLIANCE_DHCP_CLIENT_OK) {
        status = appliance_network_lifecycle_map_dhcp_status(
            dhcp_status);
        lifecycle->last_status = status;
        return status;
    }
    if (!lifecycle->transport_installed ||
        lifecycle->transport.dhcp_renew == NULL ||
        !lifecycle->transport.dhcp_renew(
            lifecycle->transport.context)) {
        /*
         * ITEM-408: a renewal that could not even be transmitted is the same
         * transient as one that was never answered - the held address keeps
         * serving and the wire retries the renewal.
         */
        return appliance_network_lifecycle_failure(
            lifecycle,
            APPLIANCE_NETWORK_LIFECYCLE_TRANSPORT_UNAVAILABLE,
            APPLIANCE_NETWORK_LIFECYCLE_REASON_DHCP_TRANSPORT, false);
    }
    lifecycle->last_status = APPLIANCE_NETWORK_LIFECYCLE_OK;
    return APPLIANCE_NETWORK_LIFECYCLE_OK;
}

/*
 * The DNS probe is bound to a live address, not to the lifecycle's state
 * word: during a hold the lifecycle is ADDRESSING again while the held
 * address - and with it the resolver's reason to exist - is still applied.
 */
static bool appliance_network_lifecycle_dns_usable(
    const struct appliance_network_lifecycle *lifecycle)
{
    return lifecycle->dns_initialized && lifecycle->has_active;
}

enum appliance_network_lifecycle_status
appliance_network_lifecycle_dns_query(
    struct appliance_network_lifecycle *lifecycle,
    const char *hostname,
    size_t hostname_length)
{
    enum appliance_dns_resolver_status dns_status;
    enum appliance_network_lifecycle_status status;

    if (lifecycle == NULL || hostname == NULL) {
        return APPLIANCE_NETWORK_LIFECYCLE_NULL;
    }
    if (!appliance_network_lifecycle_published(lifecycle) ||
        !appliance_network_lifecycle_dns_usable(lifecycle)) {
        return APPLIANCE_NETWORK_LIFECYCLE_BAD_STATE;
    }
    if (!lifecycle->transport_installed ||
        lifecycle->transport.dns_query == NULL) {
        return APPLIANCE_NETWORK_LIFECYCLE_TRANSPORT_UNAVAILABLE;
    }
    dns_status = appliance_dns_resolver_query(
        &lifecycle->dns, hostname, hostname_length);
    if (dns_status != APPLIANCE_DNS_RESOLVER_OK) {
        status = appliance_network_lifecycle_map_dns_status(
            dns_status);
        lifecycle->last_status = status;
        return status;
    }
    if (!lifecycle->transport.dns_query(
            lifecycle->transport.context,
            hostname,
            hostname_length)) {
        return appliance_network_lifecycle_dns_failure(
            lifecycle,
            APPLIANCE_NETWORK_LIFECYCLE_TRANSPORT_FAILED,
            APPLIANCE_NETWORK_LIFECYCLE_REASON_DNS_TRANSPORT);
    }
    lifecycle->last_status = APPLIANCE_NETWORK_LIFECYCLE_OK;
    return APPLIANCE_NETWORK_LIFECYCLE_OK;
}

enum appliance_network_lifecycle_status
appliance_network_lifecycle_dns_response(
    struct appliance_network_lifecycle *lifecycle,
    const struct network_ipv4_address *answer)
{
    enum appliance_dns_resolver_status dns_status;
    enum appliance_network_lifecycle_status status;

    if (lifecycle == NULL || answer == NULL) {
        return APPLIANCE_NETWORK_LIFECYCLE_NULL;
    }
    if (!appliance_network_lifecycle_published(lifecycle) ||
        !appliance_network_lifecycle_dns_usable(lifecycle)) {
        return APPLIANCE_NETWORK_LIFECYCLE_BAD_STATE;
    }
    dns_status = appliance_dns_resolver_response(
        &lifecycle->dns, answer);
    if (dns_status != APPLIANCE_DNS_RESOLVER_OK) {
        status = appliance_network_lifecycle_map_dns_status(
            dns_status);
        lifecycle->last_status = status;
        return status;
    }
    lifecycle->dns_reachability = APPLIANCE_NETWORK_REACHABILITY_REACHABLE;
    /*
     * A probe that answered retires the probe's own verdict; it never erases
     * the reason of an addressing failure the appliance is still holding.
     */
    if (lifecycle->reason ==
            APPLIANCE_NETWORK_LIFECYCLE_REASON_DNS_PROBE_TIMEOUT ||
        lifecycle->reason ==
            APPLIANCE_NETWORK_LIFECYCLE_REASON_DNS_PROBE_REFUSED ||
        lifecycle->reason ==
            APPLIANCE_NETWORK_LIFECYCLE_REASON_DNS_TRANSPORT) {
        lifecycle->reason = APPLIANCE_NETWORK_LIFECYCLE_REASON_NONE;
    }
    lifecycle->last_status = APPLIANCE_NETWORK_LIFECYCLE_OK;
    return APPLIANCE_NETWORK_LIFECYCLE_OK;
}

enum appliance_network_lifecycle_status
appliance_network_lifecycle_dns_timeout(
    struct appliance_network_lifecycle *lifecycle)
{
    enum appliance_dns_resolver_status dns_status;
    enum appliance_network_lifecycle_status status;

    if (lifecycle == NULL) {
        return APPLIANCE_NETWORK_LIFECYCLE_NULL;
    }
    if (!appliance_network_lifecycle_published(lifecycle) ||
        !appliance_network_lifecycle_dns_usable(lifecycle)) {
        return APPLIANCE_NETWORK_LIFECYCLE_BAD_STATE;
    }
    dns_status = appliance_dns_resolver_timeout(&lifecycle->dns);
    if (dns_status == APPLIANCE_DNS_RESOLVER_EXHAUSTED) {
        return appliance_network_lifecycle_dns_failure(
            lifecycle, APPLIANCE_NETWORK_LIFECYCLE_EXHAUSTED,
            APPLIANCE_NETWORK_LIFECYCLE_REASON_DNS_PROBE_TIMEOUT);
    }
    if (dns_status != APPLIANCE_DNS_RESOLVER_OK) {
        status = appliance_network_lifecycle_map_dns_status(
            dns_status);
        lifecycle->last_status = status;
        return status;
    }
    lifecycle->last_status = APPLIANCE_NETWORK_LIFECYCLE_OK;
    return APPLIANCE_NETWORK_LIFECYCLE_OK;
}

enum appliance_network_lifecycle_status
appliance_network_lifecycle_dns_server_failure(
    struct appliance_network_lifecycle *lifecycle)
{
    enum appliance_dns_resolver_status dns_status;
    enum appliance_network_lifecycle_status status;

    if (lifecycle == NULL) {
        return APPLIANCE_NETWORK_LIFECYCLE_NULL;
    }
    if (!appliance_network_lifecycle_published(lifecycle) ||
        !appliance_network_lifecycle_dns_usable(lifecycle)) {
        return APPLIANCE_NETWORK_LIFECYCLE_BAD_STATE;
    }
    dns_status = appliance_dns_resolver_server_failure(
        &lifecycle->dns);
    if (dns_status == APPLIANCE_DNS_RESOLVER_EXHAUSTED) {
        return appliance_network_lifecycle_dns_failure(
            lifecycle, APPLIANCE_NETWORK_LIFECYCLE_EXHAUSTED,
            APPLIANCE_NETWORK_LIFECYCLE_REASON_DNS_PROBE_REFUSED);
    }
    if (dns_status != APPLIANCE_DNS_RESOLVER_OK) {
        status = appliance_network_lifecycle_map_dns_status(
            dns_status);
        lifecycle->last_status = status;
        return status;
    }
    lifecycle->last_status = APPLIANCE_NETWORK_LIFECYCLE_OK;
    return APPLIANCE_NETWORK_LIFECYCLE_OK;
}

enum appliance_network_lifecycle_status
appliance_network_lifecycle_dns_probe_retry(
    struct appliance_network_lifecycle *lifecycle)
{
    if (lifecycle == NULL) {
        return APPLIANCE_NETWORK_LIFECYCLE_NULL;
    }
    if (!appliance_network_lifecycle_published(lifecycle) ||
        !appliance_network_lifecycle_dns_usable(lifecycle)) {
        return APPLIANCE_NETWORK_LIFECYCLE_BAD_STATE;
    }
    /*
     * Only a probe the resolver already gave up on is restarted: a query in
     * flight keeps its own bounded deadline, and the live address is never
     * part of this transition.
     */
    if (appliance_dns_resolver_state(&lifecycle->dns) ==
        APPLIANCE_DNS_RESOLVER_STATE_QUERYING) {
        return APPLIANCE_NETWORK_LIFECYCLE_BAD_STATE;
    }
    if (appliance_dns_resolver_clear(&lifecycle->dns) !=
        APPLIANCE_DNS_RESOLVER_OK) {
        return APPLIANCE_NETWORK_LIFECYCLE_BAD_STATE;
    }
    lifecycle->last_status = APPLIANCE_NETWORK_LIFECYCLE_OK;
    return APPLIANCE_NETWORK_LIFECYCLE_OK;
}

enum appliance_network_lifecycle_status
appliance_network_lifecycle_reachability(
    struct appliance_network_lifecycle *lifecycle,
    enum appliance_network_reachability gateway,
    enum appliance_network_reachability dns)
{
    if (lifecycle == NULL) {
        return APPLIANCE_NETWORK_LIFECYCLE_NULL;
    }
    if (!appliance_network_lifecycle_published(lifecycle) ||
        !lifecycle->link_published) {
        return APPLIANCE_NETWORK_LIFECYCLE_BAD_STATE;
    }
    if (!appliance_network_lifecycle_reachability_valid(gateway) ||
        !appliance_network_lifecycle_reachability_valid(dns)) {
        return APPLIANCE_NETWORK_LIFECYCLE_BAD_REACHABILITY;
    }
    lifecycle->gateway_reachability = gateway;
    lifecycle->dns_reachability = dns;
    appliance_network_lifecycle_publish_diagnostics(lifecycle);
    lifecycle->last_status = APPLIANCE_NETWORK_LIFECYCLE_OK;
    return APPLIANCE_NETWORK_LIFECYCLE_OK;
}

enum appliance_network_lifecycle_status
appliance_network_lifecycle_stop(
    struct appliance_network_lifecycle *lifecycle)
{
    if (lifecycle == NULL) {
        return APPLIANCE_NETWORK_LIFECYCLE_NULL;
    }
    if (!appliance_network_lifecycle_published(lifecycle) ||
        lifecycle->state == APPLIANCE_NETWORK_LIFECYCLE_STATE_STOPPED) {
        return APPLIANCE_NETWORK_LIFECYCLE_BAD_STATE;
    }
    appliance_network_lifecycle_clear_active(lifecycle);
    if (lifecycle->dhcp_initialized) {
        (void)appliance_dhcp_client_reset(&lifecycle->dhcp);
    }
    lifecycle->dhcp_initialized = false;
    lifecycle->started = false;
    lifecycle->state = APPLIANCE_NETWORK_LIFECYCLE_STATE_STOPPED;
    lifecycle->reason = APPLIANCE_NETWORK_LIFECYCLE_REASON_STOPPED;
    lifecycle->hold_count = 0U;
    lifecycle->last_status = APPLIANCE_NETWORK_LIFECYCLE_OK;
    return APPLIANCE_NETWORK_LIFECYCLE_OK;
}

enum appliance_network_lifecycle_status
appliance_network_lifecycle_reset(
    struct appliance_network_lifecycle *lifecycle)
{
    if (lifecycle == NULL) {
        return APPLIANCE_NETWORK_LIFECYCLE_NULL;
    }
    appliance_network_lifecycle_clear_active(lifecycle);
    if (lifecycle->dhcp_initialized) {
        (void)appliance_dhcp_client_reset(&lifecycle->dhcp);
    }
    lifecycle->dhcp_initialized = false;
    lifecycle->started = false;
    lifecycle->initialized = false;
    lifecycle->state =
        APPLIANCE_NETWORK_LIFECYCLE_STATE_UNINITIALIZED;
    lifecycle->reason = APPLIANCE_NETWORK_LIFECYCLE_REASON_NONE;
    lifecycle->hold_count = 0U;
    lifecycle->last_status = APPLIANCE_NETWORK_LIFECYCLE_OK;
    return APPLIANCE_NETWORK_LIFECYCLE_OK;
}

static bool appliance_network_lifecycle_append_bytes(
    char *output,
    size_t capacity,
    size_t *position,
    const void *source,
    size_t length)
{
    const unsigned char *bytes = source;
    size_t index;

    if (*position > capacity ||
        length > capacity - *position) {
        return false;
    }
    for (index = 0U; index < length; ++index) {
        output[*position + index] = (char)bytes[index];
    }
    *position += length;
    return true;
}

static bool appliance_network_lifecycle_append_string(
    char *output,
    size_t capacity,
    size_t *position,
    const char *text)
{
    return appliance_network_lifecycle_append_bytes(
        output, capacity, position, text,
        appliance_network_lifecycle_string_length(text));
}

static bool appliance_network_lifecycle_append_u8(
    char *output,
    size_t capacity,
    size_t *position,
    uint8_t value)
{
    char digits[3U];
    size_t count = 0U;
    uint16_t remaining = value;

    do {
        digits[count++] = (char)('0' + (remaining % 10U));
        remaining /= 10U;
    } while (remaining != 0U);
    while (count > 0U) {
        if (!appliance_network_lifecycle_append_bytes(
                output, capacity, position,
                digits + count - 1U, 1U)) {
            return false;
        }
        --count;
    }
    return true;
}

static bool appliance_network_lifecycle_append_u32(
    char *output,
    size_t capacity,
    size_t *position,
    uint32_t value)
{
    char digits[10U];
    size_t count = 0U;
    uint32_t remaining = value;

    do {
        digits[count++] = (char)('0' + (remaining % 10U));
        remaining /= 10U;
    } while (remaining != 0U);
    while (count > 0U) {
        if (!appliance_network_lifecycle_append_bytes(
                output, capacity, position,
                digits + count - 1U, 1U)) {
            return false;
        }
        --count;
    }
    return true;
}

static bool appliance_network_lifecycle_append_address(
    char *output,
    size_t capacity,
    size_t *position,
    const struct network_ipv4_address *address)
{
    size_t index;

    for (index = 0U;
         index < NETWORK_IPV4_ADDRESS_OCTETS;
         ++index) {
        if (!appliance_network_lifecycle_append_u8(
                output, capacity, position,
                address->octets[index]) ||
            (index + 1U < NETWORK_IPV4_ADDRESS_OCTETS &&
             !appliance_network_lifecycle_append_string(
                 output, capacity, position, "."))) {
            return false;
        }
    }
    return true;
}

enum appliance_network_lifecycle_status
appliance_network_lifecycle_render(
    const struct appliance_network_lifecycle *lifecycle,
    char *output,
    size_t capacity,
    size_t *output_length)
{
    char rendered[APPLIANCE_NETWORK_LIFECYCLE_OUTPUT_MAX + 1U];
    const char *state_string;
    const char *mode_string;
    const char *link_string;
    const char *dhcp_string = "OFF";
    const char *dns_string = "OFF";
    size_t position = 0U;
    size_t index;

    if (lifecycle == NULL || output == NULL ||
        output_length == NULL) {
        return APPLIANCE_NETWORK_LIFECYCLE_NULL;
    }
    if (!appliance_network_lifecycle_published(lifecycle)) {
        return APPLIANCE_NETWORK_LIFECYCLE_BAD_STATE;
    }
    state_string = appliance_network_lifecycle_state_string(
        lifecycle->state);
    mode_string = appliance_network_config_mode_string(
        lifecycle->desired.mode);
    link_string = lifecycle->link_published ?
        appliance_network_lifecycle_link_string(
            lifecycle->link.state) : "NONE";
    if (state_string == NULL || mode_string == NULL ||
        link_string == NULL) {
        return APPLIANCE_NETWORK_LIFECYCLE_BAD_STATE;
    }
    if (lifecycle->dhcp_initialized) {
        dhcp_string = appliance_dhcp_client_state_string(
            appliance_dhcp_client_state(&lifecycle->dhcp));
        if (dhcp_string == NULL) {
            return APPLIANCE_NETWORK_LIFECYCLE_BAD_STATE;
        }
    }
    if (lifecycle->dns_initialized) {
        dns_string = appliance_dns_resolver_state_string(
            appliance_dns_resolver_state(&lifecycle->dns));
        if (dns_string == NULL) {
            return APPLIANCE_NETWORK_LIFECYCLE_BAD_STATE;
        }
    }

    if (!appliance_network_lifecycle_append_string(
            rendered, sizeof(rendered), &position,
            "network_lifecycle v=1 state=") ||
        !appliance_network_lifecycle_append_string(
            rendered, sizeof(rendered), &position, state_string) ||
        !appliance_network_lifecycle_append_string(
            rendered, sizeof(rendered), &position,
            " interface=") ||
        !appliance_network_lifecycle_append_bytes(
            rendered, sizeof(rendered), &position,
            lifecycle->desired.interface,
            lifecycle->desired.interface_length) ||
        !appliance_network_lifecycle_append_string(
            rendered, sizeof(rendered), &position, " mode=") ||
        !appliance_network_lifecycle_append_string(
            rendered, sizeof(rendered), &position, mode_string) ||
        !appliance_network_lifecycle_append_string(
            rendered, sizeof(rendered), &position, " device=") ||
        !appliance_network_lifecycle_append_u32(
            rendered, sizeof(rendered), &position,
            lifecycle->device.device_id) ||
        !appliance_network_lifecycle_append_string(
            rendered, sizeof(rendered), &position, " link=") ||
        !appliance_network_lifecycle_append_string(
            rendered, sizeof(rendered), &position, link_string) ||
        !appliance_network_lifecycle_append_string(
            rendered, sizeof(rendered), &position,
            lifecycle->has_active ?
                " active=yes address=" : " active=no address=0.0.0.0") ||
        (lifecycle->has_active &&
         !appliance_network_lifecycle_append_address(
             rendered, sizeof(rendered), &position,
             &lifecycle->active.address)) ||
        !appliance_network_lifecycle_append_string(
            rendered, sizeof(rendered), &position, " prefix=") ||
        !appliance_network_lifecycle_append_u8(
            rendered, sizeof(rendered), &position,
            lifecycle->has_active ?
                lifecycle->active.prefix_length : 0U) ||
        !appliance_network_lifecycle_append_string(
            rendered, sizeof(rendered), &position, " dhcp=") ||
        !appliance_network_lifecycle_append_string(
            rendered, sizeof(rendered), &position, dhcp_string) ||
        !appliance_network_lifecycle_append_string(
            rendered, sizeof(rendered), &position, " dns=") ||
        !appliance_network_lifecycle_append_string(
            rendered, sizeof(rendered), &position, dns_string) ||
        !appliance_network_lifecycle_append_string(
            rendered, sizeof(rendered), &position, " health=") ||
        !appliance_network_lifecycle_append_string(
            rendered, sizeof(rendered), &position,
            appliance_network_lifecycle_health_string(lifecycle)) ||
        /*
         * ITEM-408: the bounded reason and the number of failures the live
         * address survived belong to the record, so one line says why the
         * lifecycle is where it is.
         */
        !appliance_network_lifecycle_append_string(
            rendered, sizeof(rendered), &position, " reason=") ||
        !appliance_network_lifecycle_append_string(
            rendered, sizeof(rendered), &position,
            appliance_network_lifecycle_reason_string(
                lifecycle->reason)) ||
        !appliance_network_lifecycle_append_string(
            rendered, sizeof(rendered), &position, " hold=") ||
        !appliance_network_lifecycle_append_u32(
            rendered, sizeof(rendered), &position,
            lifecycle->hold_count) ||
        !appliance_network_lifecycle_append_string(
            rendered, sizeof(rendered), &position, "\n")) {
        return APPLIANCE_NETWORK_LIFECYCLE_BUFFER_TOO_SMALL;
    }
    if (capacity <= position) {
        return APPLIANCE_NETWORK_LIFECYCLE_BUFFER_TOO_SMALL;
    }
    for (index = 0U; index < position; ++index) {
        output[index] = rendered[index];
    }
    output[position] = '\0';
    *output_length = position;
    return APPLIANCE_NETWORK_LIFECYCLE_OK;
}

enum appliance_network_lifecycle_state
appliance_network_lifecycle_state(
    const struct appliance_network_lifecycle *lifecycle)
{
    if (lifecycle == NULL) {
        return APPLIANCE_NETWORK_LIFECYCLE_STATE_NONE;
    }
    return lifecycle->state;
}

bool appliance_network_lifecycle_active(
    const struct appliance_network_lifecycle *lifecycle)
{
    return lifecycle != NULL &&
           lifecycle->initialized &&
           lifecycle->has_active;
}

const struct network_ipv4_config *
appliance_network_lifecycle_active_config(
    const struct appliance_network_lifecycle *lifecycle)
{
    if (lifecycle == NULL || !lifecycle->initialized ||
        !lifecycle->has_active) {
        return NULL;
    }
    return &lifecycle->active;
}

const struct appliance_dhcp_client *
appliance_network_lifecycle_dhcp(
    const struct appliance_network_lifecycle *lifecycle)
{
    if (lifecycle == NULL || !lifecycle->initialized ||
        !lifecycle->dhcp_initialized) {
        return NULL;
    }
    return &lifecycle->dhcp;
}

const struct appliance_dns_resolver *
appliance_network_lifecycle_dns(
    const struct appliance_network_lifecycle *lifecycle)
{
    if (lifecycle == NULL || !lifecycle->initialized ||
        !lifecycle->dns_initialized) {
        return NULL;
    }
    return &lifecycle->dns;
}

enum appliance_network_lifecycle_reason
appliance_network_lifecycle_reason(
    const struct appliance_network_lifecycle *lifecycle)
{
    if (lifecycle == NULL) {
        return APPLIANCE_NETWORK_LIFECYCLE_REASON_NONE;
    }
    return lifecycle->reason;
}

uint32_t appliance_network_lifecycle_hold_count(
    const struct appliance_network_lifecycle *lifecycle)
{
    return lifecycle != NULL ? lifecycle->hold_count : 0U;
}

const char *appliance_network_lifecycle_reason_string(
    enum appliance_network_lifecycle_reason reason)
{
    switch (reason) {
    case APPLIANCE_NETWORK_LIFECYCLE_REASON_NONE:
        return "NONE";
    case APPLIANCE_NETWORK_LIFECYCLE_REASON_LINK_DOWN_SAMPLE:
        return "LINK_DOWN_SAMPLE";
    case APPLIANCE_NETWORK_LIFECYCLE_REASON_LINK_INITIALIZING_SAMPLE:
        return "LINK_INITIALIZING_SAMPLE";
    case APPLIANCE_NETWORK_LIFECYCLE_REASON_LINK_FAILED_SAMPLE:
        return "LINK_FAILED_SAMPLE";
    case APPLIANCE_NETWORK_LIFECYCLE_REASON_DISCOVER_TIMEOUT:
        return "DISCOVER_TIMEOUT";
    case APPLIANCE_NETWORK_LIFECYCLE_REASON_REQUEST_TIMEOUT:
        return "REQUEST_TIMEOUT";
    case APPLIANCE_NETWORK_LIFECYCLE_REASON_RENEW_TIMEOUT:
        return "RENEW_TIMEOUT";
    case APPLIANCE_NETWORK_LIFECYCLE_REASON_REBIND_TIMEOUT:
        return "REBIND_TIMEOUT";
    case APPLIANCE_NETWORK_LIFECYCLE_REASON_DHCP_REFUSED:
        return "DHCP_REFUSED";
    case APPLIANCE_NETWORK_LIFECYCLE_REASON_DHCP_TRANSPORT:
        return "DHCP_TRANSPORT";
    case APPLIANCE_NETWORK_LIFECYCLE_REASON_ADDRESS_ACTIVATION:
        return "ADDRESS_ACTIVATION";
    case APPLIANCE_NETWORK_LIFECYCLE_REASON_DNS_PROBE_TIMEOUT:
        return "DNS_PROBE_TIMEOUT";
    case APPLIANCE_NETWORK_LIFECYCLE_REASON_DNS_PROBE_REFUSED:
        return "DNS_PROBE_REFUSED";
    case APPLIANCE_NETWORK_LIFECYCLE_REASON_DNS_TRANSPORT:
        return "DNS_TRANSPORT";
    case APPLIANCE_NETWORK_LIFECYCLE_REASON_TRANSPORT_UNAVAILABLE:
        return "TRANSPORT_UNAVAILABLE";
    case APPLIANCE_NETWORK_LIFECYCLE_REASON_STOPPED:
        return "STOPPED";
    default:
        return NULL;
    }
}

const char *appliance_network_lifecycle_state_string(
    enum appliance_network_lifecycle_state state)
{
    switch (state) {
    case APPLIANCE_NETWORK_LIFECYCLE_STATE_NONE:
        return "NONE";
    case APPLIANCE_NETWORK_LIFECYCLE_STATE_UNINITIALIZED:
        return "UNINITIALIZED";
    case APPLIANCE_NETWORK_LIFECYCLE_STATE_PREPARED:
        return "PREPARED";
    case APPLIANCE_NETWORK_LIFECYCLE_STATE_STARTED:
        return "STARTED";
    case APPLIANCE_NETWORK_LIFECYCLE_STATE_WAITING_FOR_LINK:
        return "WAITING_FOR_LINK";
    case APPLIANCE_NETWORK_LIFECYCLE_STATE_ADDRESSING:
        return "ADDRESSING";
    case APPLIANCE_NETWORK_LIFECYCLE_STATE_ACTIVE:
        return "ACTIVE";
    case APPLIANCE_NETWORK_LIFECYCLE_STATE_STOPPED:
        return "STOPPED";
    case APPLIANCE_NETWORK_LIFECYCLE_STATE_FAILED:
        return "FAILED";
    default:
        return NULL;
    }
}

const char *appliance_network_lifecycle_status_string(
    enum appliance_network_lifecycle_status status)
{
    switch (status) {
    case APPLIANCE_NETWORK_LIFECYCLE_OK:
        return "APPLIANCE_NETWORK_LIFECYCLE_OK";
    case APPLIANCE_NETWORK_LIFECYCLE_NULL:
        return "APPLIANCE_NETWORK_LIFECYCLE_NULL";
    case APPLIANCE_NETWORK_LIFECYCLE_BAD_STATE:
        return "APPLIANCE_NETWORK_LIFECYCLE_BAD_STATE";
    case APPLIANCE_NETWORK_LIFECYCLE_BAD_VERSION:
        return "APPLIANCE_NETWORK_LIFECYCLE_BAD_VERSION";
    case APPLIANCE_NETWORK_LIFECYCLE_BAD_CONFIG:
        return "APPLIANCE_NETWORK_LIFECYCLE_BAD_CONFIG";
    case APPLIANCE_NETWORK_LIFECYCLE_BAD_DEVICE:
        return "APPLIANCE_NETWORK_LIFECYCLE_BAD_DEVICE";
    case APPLIANCE_NETWORK_LIFECYCLE_BAD_LINK:
        return "APPLIANCE_NETWORK_LIFECYCLE_BAD_LINK";
    case APPLIANCE_NETWORK_LIFECYCLE_BAD_REACHABILITY:
        return "APPLIANCE_NETWORK_LIFECYCLE_BAD_REACHABILITY";
    case APPLIANCE_NETWORK_LIFECYCLE_BAD_DHCP:
        return "APPLIANCE_NETWORK_LIFECYCLE_BAD_DHCP";
    case APPLIANCE_NETWORK_LIFECYCLE_BAD_DNS:
        return "APPLIANCE_NETWORK_LIFECYCLE_BAD_DNS";
    case APPLIANCE_NETWORK_LIFECYCLE_TRANSPORT_UNAVAILABLE:
        return "APPLIANCE_NETWORK_LIFECYCLE_TRANSPORT_UNAVAILABLE";
    case APPLIANCE_NETWORK_LIFECYCLE_TRANSPORT_FAILED:
        return "APPLIANCE_NETWORK_LIFECYCLE_TRANSPORT_FAILED";
    case APPLIANCE_NETWORK_LIFECYCLE_EXHAUSTED:
        return "APPLIANCE_NETWORK_LIFECYCLE_EXHAUSTED";
    case APPLIANCE_NETWORK_LIFECYCLE_BUFFER_TOO_SMALL:
        return "APPLIANCE_NETWORK_LIFECYCLE_BUFFER_TOO_SMALL";
    default:
        return NULL;
    }
}
