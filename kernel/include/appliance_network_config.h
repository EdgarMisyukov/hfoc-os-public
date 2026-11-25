#ifndef HFOC_APPLIANCE_NETWORK_CONFIG_H
#define HFOC_APPLIANCE_NETWORK_CONFIG_H

#include "ipv4_config.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define APPLIANCE_NETWORK_CONFIG_VERSION 1U
#define APPLIANCE_NETWORK_CONFIG_INTERFACE_MAX 15U
#define APPLIANCE_NETWORK_CONFIG_DEFAULT_INTERFACE "eth0"
#define APPLIANCE_NETWORK_CONFIG_OUTPUT_MAX 192U

enum appliance_network_config_mode {
    APPLIANCE_NETWORK_CONFIG_MODE_NONE = 0,
    APPLIANCE_NETWORK_CONFIG_MODE_STATIC = 1,
    APPLIANCE_NETWORK_CONFIG_MODE_DHCP = 2,
    /* ITEM-374: explicitly offline; no address and no DHCP/DNS attempt. */
    APPLIANCE_NETWORK_CONFIG_MODE_OFFLINE = 3,
};

enum appliance_network_config_status {
    APPLIANCE_NETWORK_CONFIG_OK = 0,
    APPLIANCE_NETWORK_CONFIG_NULL = 1,
    APPLIANCE_NETWORK_CONFIG_BAD_STATE = 2,
    APPLIANCE_NETWORK_CONFIG_BAD_VERSION = 3,
    APPLIANCE_NETWORK_CONFIG_INVALID_INTERFACE = 4,
    APPLIANCE_NETWORK_CONFIG_INVALID_MODE = 5,
    APPLIANCE_NETWORK_CONFIG_INVALID_STATIC_CONFIG = 6,
    APPLIANCE_NETWORK_CONFIG_BUFFER_TOO_SMALL = 7,
};

struct appliance_network_config {
    uint32_t version;
    char interface[APPLIANCE_NETWORK_CONFIG_INTERFACE_MAX + 1U];
    uint8_t interface_length;
    enum appliance_network_config_mode mode;
    bool static_present;
    struct network_ipv4_config static_config;
    bool initialized;
};

enum appliance_network_config_status
appliance_network_config_publish_default(
    struct appliance_network_config *config);
enum appliance_network_config_status
appliance_network_config_publish_dhcp(
    struct appliance_network_config *config,
    const char *interface,
    size_t interface_length);
enum appliance_network_config_status
appliance_network_config_publish_offline(
    struct appliance_network_config *config,
    const char *interface,
    size_t interface_length);
enum appliance_network_config_status
appliance_network_config_publish_static(
    struct appliance_network_config *config,
    const char *interface,
    size_t interface_length,
    const struct network_ipv4_config *static_config);
enum appliance_network_config_status
appliance_network_config_validate(
    const struct appliance_network_config *config);
enum appliance_network_config_status
appliance_network_config_render(
    const struct appliance_network_config *config,
    char *output,
    size_t capacity,
    size_t *output_length);
enum appliance_network_config_status
appliance_network_config_interface(
    const struct appliance_network_config *config,
    char *interface,
    size_t capacity,
    size_t *interface_length);
enum appliance_network_config_mode
appliance_network_config_mode(
    const struct appliance_network_config *config);
const struct network_ipv4_config *
appliance_network_config_static_config(
    const struct appliance_network_config *config);
const char *appliance_network_config_mode_string(
    enum appliance_network_config_mode mode);
const char *appliance_network_config_status_string(
    enum appliance_network_config_status status);

#endif
