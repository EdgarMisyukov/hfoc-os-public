#ifndef HFOC_APPLIANCE_MANAGEMENT_BIND_H
#define HFOC_APPLIANCE_MANAGEMENT_BIND_H

#include "appliance_network_config.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * ITEM-374 management ingress binding.
 *
 * The management plane must not be reachable from the outer interface by
 * default.  A published binding is fail-closed: LOCAL_ONLY allows no network
 * ingress, MANAGEMENT_INTERFACE allows only the declared management interface
 * (and, when declared, only the declared management peers), and
 * OUTER_INTERFACE is an explicit opt-in that a product decision has to make.
 * The default publication is LOCAL_ONLY.
 */

#define APPLIANCE_MANAGEMENT_BIND_VERSION 1U
#define APPLIANCE_MANAGEMENT_BIND_PEER_MAX 4U
#define APPLIANCE_MANAGEMENT_BIND_ADDRESS_LENGTH 4U

enum appliance_management_bind_scope {
    APPLIANCE_MANAGEMENT_BIND_SCOPE_NONE = 0,
    APPLIANCE_MANAGEMENT_BIND_SCOPE_LOCAL_ONLY = 1,
    APPLIANCE_MANAGEMENT_BIND_SCOPE_MANAGEMENT_INTERFACE = 2,
    APPLIANCE_MANAGEMENT_BIND_SCOPE_OUTER_INTERFACE = 3,
};

enum appliance_management_bind_status {
    APPLIANCE_MANAGEMENT_BIND_OK = 0,
    APPLIANCE_MANAGEMENT_BIND_NULL = 1,
    APPLIANCE_MANAGEMENT_BIND_BAD_STATE = 2,
    APPLIANCE_MANAGEMENT_BIND_BAD_VERSION = 3,
    APPLIANCE_MANAGEMENT_BIND_BAD_SCOPE = 4,
    APPLIANCE_MANAGEMENT_BIND_BAD_INTERFACE = 5,
    APPLIANCE_MANAGEMENT_BIND_BAD_PEER = 6,
    APPLIANCE_MANAGEMENT_BIND_ALLOWED = 7,
    APPLIANCE_MANAGEMENT_BIND_DENIED_OUTER = 8,
    APPLIANCE_MANAGEMENT_BIND_DENIED_PEER = 9,
    APPLIANCE_MANAGEMENT_BIND_DENIED_UNCONFIGURED = 10,
};

struct appliance_management_bind {
    uint32_t version;
    bool published;
    enum appliance_management_bind_scope scope;
    bool management_interface_present;
    char management_interface[
        APPLIANCE_NETWORK_CONFIG_INTERFACE_MAX + 1U];
    uint8_t management_interface_length;
    uint32_t peer_count;
    uint8_t peers[APPLIANCE_MANAGEMENT_BIND_PEER_MAX]
                 [APPLIANCE_MANAGEMENT_BIND_ADDRESS_LENGTH];
};

enum appliance_management_bind_status
appliance_management_bind_publish_default(
    struct appliance_management_bind *bind);
enum appliance_management_bind_status
appliance_management_bind_publish_local_only(
    struct appliance_management_bind *bind);
enum appliance_management_bind_status
appliance_management_bind_publish_management_interface(
    struct appliance_management_bind *bind,
    const char *interface,
    size_t interface_length,
    const uint8_t (*peers)[APPLIANCE_MANAGEMENT_BIND_ADDRESS_LENGTH],
    uint32_t peer_count);
enum appliance_management_bind_status
appliance_management_bind_publish_outer_interface(
    struct appliance_management_bind *bind);
enum appliance_management_bind_status appliance_management_bind_admit(
    const struct appliance_management_bind *bind,
    bool ingress_is_management_interface,
    const uint8_t ingress_address[
        APPLIANCE_MANAGEMENT_BIND_ADDRESS_LENGTH]);
bool appliance_management_bind_is_published(
    const struct appliance_management_bind *bind);
enum appliance_management_bind_scope appliance_management_bind_scope_of(
    const struct appliance_management_bind *bind);
const char *appliance_management_bind_scope_string(
    enum appliance_management_bind_scope scope);
const char *appliance_management_bind_status_string(
    enum appliance_management_bind_status status);

#endif
