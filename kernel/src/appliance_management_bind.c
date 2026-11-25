#include "appliance_management_bind.h"

#include <stdbool.h>
#include <stddef.h>
#include <string.h>

static bool management_bind_interface_char_allowed(unsigned char value)
{
    return (value >= (unsigned char)'a' &&
            value <= (unsigned char)'z') ||
           (value >= (unsigned char)'0' &&
            value <= (unsigned char)'9') ||
           value == (unsigned char)'-' ||
           value == (unsigned char)'_';
}

static bool management_bind_interface_alnum(unsigned char value)
{
    return (value >= (unsigned char)'a' &&
            value <= (unsigned char)'z') ||
           (value >= (unsigned char)'0' &&
            value <= (unsigned char)'9');
}

static bool management_bind_interface_valid(
    const char *interface,
    size_t interface_length)
{
    size_t index;

    if (interface == NULL ||
        interface_length == 0U ||
        interface_length > APPLIANCE_NETWORK_CONFIG_INTERFACE_MAX ||
        !management_bind_interface_alnum(
            (unsigned char)interface[0]) ||
        !management_bind_interface_alnum(
            (unsigned char)interface[interface_length - 1U])) {
        return false;
    }
    for (index = 0U; index < interface_length; ++index) {
        if (!management_bind_interface_char_allowed(
                (unsigned char)interface[index])) {
            return false;
        }
    }
    return true;
}

static enum appliance_management_bind_status management_bind_publish_scope(
    struct appliance_management_bind *bind,
    enum appliance_management_bind_scope scope)
{
    if (bind == NULL) {
        return APPLIANCE_MANAGEMENT_BIND_NULL;
    }
    memset(bind, 0, sizeof(*bind));
    bind->version = APPLIANCE_MANAGEMENT_BIND_VERSION;
    bind->published = true;
    bind->scope = scope;
    return APPLIANCE_MANAGEMENT_BIND_OK;
}

enum appliance_management_bind_status
appliance_management_bind_publish_default(
    struct appliance_management_bind *bind)
{
    return appliance_management_bind_publish_local_only(bind);
}

enum appliance_management_bind_status
appliance_management_bind_publish_local_only(
    struct appliance_management_bind *bind)
{
    return management_bind_publish_scope(
        bind, APPLIANCE_MANAGEMENT_BIND_SCOPE_LOCAL_ONLY);
}

enum appliance_management_bind_status
appliance_management_bind_publish_management_interface(
    struct appliance_management_bind *bind,
    const char *interface,
    size_t interface_length,
    const uint8_t (*peers)[APPLIANCE_MANAGEMENT_BIND_ADDRESS_LENGTH],
    uint32_t peer_count)
{
    enum appliance_management_bind_status status;
    uint32_t index;

    if (bind == NULL) {
        return APPLIANCE_MANAGEMENT_BIND_NULL;
    }
    if (!management_bind_interface_valid(interface, interface_length)) {
        return APPLIANCE_MANAGEMENT_BIND_BAD_INTERFACE;
    }
    if (peer_count > APPLIANCE_MANAGEMENT_BIND_PEER_MAX ||
        (peer_count != 0U && peers == NULL)) {
        return APPLIANCE_MANAGEMENT_BIND_BAD_PEER;
    }
    status = management_bind_publish_scope(
        bind, APPLIANCE_MANAGEMENT_BIND_SCOPE_MANAGEMENT_INTERFACE);
    if (status != APPLIANCE_MANAGEMENT_BIND_OK) {
        return status;
    }
    memcpy(bind->management_interface, interface, interface_length);
    bind->management_interface[interface_length] = '\0';
    bind->management_interface_length = (uint8_t)interface_length;
    bind->management_interface_present = true;
    bind->peer_count = peer_count;
    for (index = 0U; index < peer_count; ++index) {
        memcpy(bind->peers[index], peers[index],
               APPLIANCE_MANAGEMENT_BIND_ADDRESS_LENGTH);
    }
    return APPLIANCE_MANAGEMENT_BIND_OK;
}

enum appliance_management_bind_status
appliance_management_bind_publish_outer_interface(
    struct appliance_management_bind *bind)
{
    return management_bind_publish_scope(
        bind, APPLIANCE_MANAGEMENT_BIND_SCOPE_OUTER_INTERFACE);
}

static bool management_bind_peer_allowed(
    const struct appliance_management_bind *bind,
    const uint8_t address[APPLIANCE_MANAGEMENT_BIND_ADDRESS_LENGTH])
{
    uint32_t index;

    if (bind->peer_count == 0U) {
        return true;
    }
    for (index = 0U; index < bind->peer_count; ++index) {
        if (memcmp(bind->peers[index], address,
                   APPLIANCE_MANAGEMENT_BIND_ADDRESS_LENGTH) == 0) {
            return true;
        }
    }
    return false;
}

enum appliance_management_bind_status appliance_management_bind_admit(
    const struct appliance_management_bind *bind,
    bool ingress_is_management_interface,
    const uint8_t ingress_address[
        APPLIANCE_MANAGEMENT_BIND_ADDRESS_LENGTH])
{
    if (bind == NULL || ingress_address == NULL) {
        return APPLIANCE_MANAGEMENT_BIND_NULL;
    }
    if (!bind->published ||
        bind->scope == APPLIANCE_MANAGEMENT_BIND_SCOPE_NONE) {
        return APPLIANCE_MANAGEMENT_BIND_DENIED_UNCONFIGURED;
    }
    switch (bind->scope) {
    case APPLIANCE_MANAGEMENT_BIND_SCOPE_LOCAL_ONLY:
        return APPLIANCE_MANAGEMENT_BIND_DENIED_OUTER;
    case APPLIANCE_MANAGEMENT_BIND_SCOPE_MANAGEMENT_INTERFACE:
        if (!ingress_is_management_interface) {
            return APPLIANCE_MANAGEMENT_BIND_DENIED_OUTER;
        }
        return management_bind_peer_allowed(bind, ingress_address)
                   ? APPLIANCE_MANAGEMENT_BIND_ALLOWED
                   : APPLIANCE_MANAGEMENT_BIND_DENIED_PEER;
    case APPLIANCE_MANAGEMENT_BIND_SCOPE_OUTER_INTERFACE:
        return APPLIANCE_MANAGEMENT_BIND_ALLOWED;
    default:
        return APPLIANCE_MANAGEMENT_BIND_DENIED_UNCONFIGURED;
    }
}

bool appliance_management_bind_is_published(
    const struct appliance_management_bind *bind)
{
    return bind != NULL && bind->published &&
           bind->version == APPLIANCE_MANAGEMENT_BIND_VERSION;
}

enum appliance_management_bind_scope appliance_management_bind_scope_of(
    const struct appliance_management_bind *bind)
{
    if (bind == NULL || !bind->published) {
        return APPLIANCE_MANAGEMENT_BIND_SCOPE_NONE;
    }
    return bind->scope;
}

const char *appliance_management_bind_scope_string(
    enum appliance_management_bind_scope scope)
{
    switch (scope) {
    case APPLIANCE_MANAGEMENT_BIND_SCOPE_NONE:
        return "NONE";
    case APPLIANCE_MANAGEMENT_BIND_SCOPE_LOCAL_ONLY:
        return "LOCAL_ONLY";
    case APPLIANCE_MANAGEMENT_BIND_SCOPE_MANAGEMENT_INTERFACE:
        return "MANAGEMENT_INTERFACE";
    case APPLIANCE_MANAGEMENT_BIND_SCOPE_OUTER_INTERFACE:
        return "OUTER_INTERFACE";
    default:
        return NULL;
    }
}

const char *appliance_management_bind_status_string(
    enum appliance_management_bind_status status)
{
    switch (status) {
    case APPLIANCE_MANAGEMENT_BIND_OK:
        return "OK";
    case APPLIANCE_MANAGEMENT_BIND_NULL:
        return "NULL";
    case APPLIANCE_MANAGEMENT_BIND_BAD_STATE:
        return "BAD_STATE";
    case APPLIANCE_MANAGEMENT_BIND_BAD_VERSION:
        return "BAD_VERSION";
    case APPLIANCE_MANAGEMENT_BIND_BAD_SCOPE:
        return "BAD_SCOPE";
    case APPLIANCE_MANAGEMENT_BIND_BAD_INTERFACE:
        return "BAD_INTERFACE";
    case APPLIANCE_MANAGEMENT_BIND_BAD_PEER:
        return "BAD_PEER";
    case APPLIANCE_MANAGEMENT_BIND_ALLOWED:
        return "ALLOWED";
    case APPLIANCE_MANAGEMENT_BIND_DENIED_OUTER:
        return "DENIED_OUTER";
    case APPLIANCE_MANAGEMENT_BIND_DENIED_PEER:
        return "DENIED_PEER";
    case APPLIANCE_MANAGEMENT_BIND_DENIED_UNCONFIGURED:
        return "DENIED_UNCONFIGURED";
    default:
        return NULL;
    }
}
