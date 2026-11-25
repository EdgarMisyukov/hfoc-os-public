#ifndef HFOC_IPV4_CONFIG_H
#define HFOC_IPV4_CONFIG_H

#include <stddef.h>
#include <stdint.h>

#define NETWORK_IPV4_CONFIG_VERSION 1U
#define NETWORK_IPV4_INPUT_MAX 4096U
#define NETWORK_IPV4_LINE_MAX 64U
#define NETWORK_IPV4_KEY_MAX 32U
#define NETWORK_IPV4_VALUE_MAX 32U
#define NETWORK_IPV4_PREFIX_MIN 0U
#define NETWORK_IPV4_PREFIX_MAX 32U
#define NETWORK_IPV4_ADDRESS_OCTETS 4U

enum network_ipv4_status {
    NETWORK_IPV4_OK = 0,
    NETWORK_IPV4_NULL = 1,
    NETWORK_IPV4_TOO_LONG = 2,
    NETWORK_IPV4_MISSING_FIELD = 3,
    NETWORK_IPV4_DUPLICATE_FIELD = 4,
    NETWORK_IPV4_UNKNOWN_FIELD = 5,
    NETWORK_IPV4_MALFORMED_FIELD = 6,
    NETWORK_IPV4_BAD_VERSION = 7,
    NETWORK_IPV4_INVALID_ADDRESS = 8,
    NETWORK_IPV4_INVALID_PREFIX = 9,
    NETWORK_IPV4_INVALID_ROUTE = 10,
    NETWORK_IPV4_INVALID_DNS = 11,
};

struct network_ipv4_address {
    uint8_t octets[NETWORK_IPV4_ADDRESS_OCTETS];
};

struct network_ipv4_config {
    uint32_t schema_version;
    struct network_ipv4_address address;
    uint8_t prefix_length;
    struct network_ipv4_address gateway;
    struct network_ipv4_address dns;
};

/*
 * ITEM-654: one literal `a.b.c.d` address, through the same parser the IPv4
 * configuration owner already uses.  A second text parser of this grammar is
 * not allowed anywhere in the tree; callers that need a single address (the
 * explicit SNTP source) reach this one.
 */
enum network_ipv4_status network_ipv4_address_parse(
    const char *value,
    size_t value_length,
    struct network_ipv4_address *address_out);

enum network_ipv4_status network_ipv4_config_parse(
    const char *input,
    size_t input_length,
    struct network_ipv4_config *config_out);
/*
 * ITEM-447: the `a.b.c.d/prefix` form the operator and the install medium
 * declare, turned into the configuration the one parser above accepts.  The
 * builder splits the one `/`, composes the canonical five lines and runs the
 * same parse and validate; it is the only place that knows the slash form, so
 * the medium declaration and the menu cannot drift apart.
 */
enum network_ipv4_status network_ipv4_config_from_parts(
    const char *address_with_prefix,
    size_t address_length,
    const char *gateway,
    size_t gateway_length,
    const char *dns,
    size_t dns_length,
    struct network_ipv4_config *config_out);
enum network_ipv4_status network_ipv4_config_validate(
    const struct network_ipv4_config *config);
const char *network_ipv4_status_string(
    enum network_ipv4_status status);

#endif
