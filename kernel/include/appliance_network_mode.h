#ifndef HFOC_APPLIANCE_NETWORK_MODE_H
#define HFOC_APPLIANCE_NETWORK_MODE_H

#include "appliance_network_config.h"

#include <stdbool.h>
#include <stdint.h>

/*
 * ITEM-374 network mode decision.
 *
 * The production wire owner asks this module what to do with the desired
 * network configuration before it touches lwIP: run DHCP, apply the static
 * configuration, stay offline without an address, or report the interface as
 * unavailable.  The decision is pure and host-testable, and it publishes the
 * honest degraded verdict for the offline and absent-device cases.
 */

#define APPLIANCE_NETWORK_MODE_VERSION 1U

enum appliance_network_mode_action {
    APPLIANCE_NETWORK_MODE_ACTION_NONE = 0,
    APPLIANCE_NETWORK_MODE_ACTION_DHCP = 1,
    APPLIANCE_NETWORK_MODE_ACTION_STATIC = 2,
    APPLIANCE_NETWORK_MODE_ACTION_OFFLINE = 3,
    APPLIANCE_NETWORK_MODE_ACTION_UNAVAILABLE = 4,
};

enum appliance_network_mode_status {
    APPLIANCE_NETWORK_MODE_OK = 0,
    APPLIANCE_NETWORK_MODE_NULL = 1,
    APPLIANCE_NETWORK_MODE_BAD_VERSION = 2,
    APPLIANCE_NETWORK_MODE_BAD_CONFIG = 3,
};

struct appliance_network_mode_decision {
    uint32_t version;
    enum appliance_network_mode_action action;
    enum appliance_network_config_mode mode;
    bool addressing;
    bool degraded;
};

enum appliance_network_mode_status appliance_network_mode_decide(
    const struct appliance_network_config *desired,
    bool device_present,
    struct appliance_network_mode_decision *decision);
const char *appliance_network_mode_action_string(
    enum appliance_network_mode_action action);
const char *appliance_network_mode_status_string(
    enum appliance_network_mode_status status);

#endif
