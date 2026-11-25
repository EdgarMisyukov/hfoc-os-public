#ifndef HFOC_APPLIANCE_DHCP_CLIENT_H
#define HFOC_APPLIANCE_DHCP_CLIENT_H

#include "appliance_network_config.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define APPLIANCE_DHCP_CLIENT_VERSION 1U
/*
 * ITEM-416: how long the client keeps asking before it calls the acquisition
 * failed.  Three attempts at the one-second DISCOVER deadline - the value this
 * had - is two seconds of patience: the acceptance host booted behind a switch
 * port that was still forwarding nothing, the appliance reached `state=FAILED
 * reason=DISCOVER_TIMEOUT` before the network answered, and the address it was
 * left holding was the fixture's.  Forty attempts is a bounded forty seconds of
 * asking, which covers a port that has to leave spanning-tree blocking and a
 * DHCP server that is simply slow, and the record names the attempt so an
 * operator reads a client that is still trying rather than one that gave up.
 */
#define APPLIANCE_DHCP_CLIENT_MAX_ATTEMPTS 40U
#define APPLIANCE_DHCP_CLIENT_OUTPUT_MAX 256U

enum appliance_dhcp_client_state {
    APPLIANCE_DHCP_CLIENT_STATE_NONE = 0,
    APPLIANCE_DHCP_CLIENT_STATE_IDLE = 1,
    APPLIANCE_DHCP_CLIENT_STATE_DISCOVERING = 2,
    APPLIANCE_DHCP_CLIENT_STATE_REQUESTING = 3,
    APPLIANCE_DHCP_CLIENT_STATE_BOUND = 4,
    APPLIANCE_DHCP_CLIENT_STATE_RENEWING = 5,
    APPLIANCE_DHCP_CLIENT_STATE_REBINDING = 6,
    APPLIANCE_DHCP_CLIENT_STATE_FAILED = 7,
};

enum appliance_dhcp_client_status {
    APPLIANCE_DHCP_CLIENT_OK = 0,
    APPLIANCE_DHCP_CLIENT_NULL = 1,
    APPLIANCE_DHCP_CLIENT_BAD_STATE = 2,
    APPLIANCE_DHCP_CLIENT_BAD_VERSION = 3,
    APPLIANCE_DHCP_CLIENT_BAD_CONFIG = 4,
    APPLIANCE_DHCP_CLIENT_BAD_LEASE = 5,
    APPLIANCE_DHCP_CLIENT_STOPPED = 6,
    APPLIANCE_DHCP_CLIENT_EXHAUSTED = 7,
    APPLIANCE_DHCP_CLIENT_BUFFER_TOO_SMALL = 8,
};

struct appliance_dhcp_client {
    uint32_t version;
    enum appliance_dhcp_client_state state;
    struct appliance_network_config desired;
    struct network_ipv4_config lease;
    uint32_t attempt_count;
    uint32_t max_attempts;
    bool lease_published;
    bool initialized;
};

enum appliance_dhcp_client_status
appliance_dhcp_client_init(
    struct appliance_dhcp_client *client,
    const struct appliance_network_config *desired);
enum appliance_dhcp_client_status
appliance_dhcp_client_start(
    struct appliance_dhcp_client *client);
enum appliance_dhcp_client_status
appliance_dhcp_client_offer(
    struct appliance_dhcp_client *client,
    const struct network_ipv4_config *candidate);
enum appliance_dhcp_client_status
appliance_dhcp_client_ack(
    struct appliance_dhcp_client *client,
    const struct network_ipv4_config *lease);
enum appliance_dhcp_client_status
appliance_dhcp_client_nak(
    struct appliance_dhcp_client *client);
enum appliance_dhcp_client_status
appliance_dhcp_client_discover_timeout(
    struct appliance_dhcp_client *client);
enum appliance_dhcp_client_status
appliance_dhcp_client_request_timeout(
    struct appliance_dhcp_client *client);
enum appliance_dhcp_client_status
appliance_dhcp_client_renew(
    struct appliance_dhcp_client *client);
enum appliance_dhcp_client_status
appliance_dhcp_client_renew_timeout(
    struct appliance_dhcp_client *client);
enum appliance_dhcp_client_status
appliance_dhcp_client_rebind_timeout(
    struct appliance_dhcp_client *client);
enum appliance_dhcp_client_status
appliance_dhcp_client_link_down(
    struct appliance_dhcp_client *client);
enum appliance_dhcp_client_status
appliance_dhcp_client_reset(
    struct appliance_dhcp_client *client);
enum appliance_dhcp_client_status
appliance_dhcp_client_render(
    const struct appliance_dhcp_client *client,
    char *output,
    size_t capacity,
    size_t *output_length);
enum appliance_dhcp_client_state
appliance_dhcp_client_state(
    const struct appliance_dhcp_client *client);
bool appliance_dhcp_client_lease_published(
    const struct appliance_dhcp_client *client);
const struct network_ipv4_config *
appliance_dhcp_client_lease(
    const struct appliance_dhcp_client *client);
/* ITEM-416: the attempt the client is on, and the budget it is bounded by. */
uint32_t appliance_dhcp_client_attempt_count(
    const struct appliance_dhcp_client *client);
uint32_t appliance_dhcp_client_attempt_budget(
    const struct appliance_dhcp_client *client);

const char *appliance_dhcp_client_state_string(
    enum appliance_dhcp_client_state state);
const char *appliance_dhcp_client_status_string(
    enum appliance_dhcp_client_status status);

#endif
