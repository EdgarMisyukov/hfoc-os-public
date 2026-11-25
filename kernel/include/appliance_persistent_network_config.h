#ifndef HFOC_APPLIANCE_PERSISTENT_NETWORK_CONFIG_H
#define HFOC_APPLIANCE_PERSISTENT_NETWORK_CONFIG_H

#include "appliance_network_config.h"
#include "block_device.h"
#include "config_journal.h"

#include <stdbool.h>
#include <stddef.h>

#define APPLIANCE_PERSISTENT_NETWORK_CONFIG_FORMAT_VERSION 1U
#define APPLIANCE_PERSISTENT_NETWORK_CONFIG_RECORD_MAX \
    CONFIG_JOURNAL_DATA_MAX

enum appliance_persistent_network_config_status {
    APPLIANCE_PERSISTENT_NETWORK_CONFIG_OK = 0,
    APPLIANCE_PERSISTENT_NETWORK_CONFIG_NULL = 1,
    APPLIANCE_PERSISTENT_NETWORK_CONFIG_BAD_STATE = 2,
    APPLIANCE_PERSISTENT_NETWORK_CONFIG_BAD_DEVICE = 3,
    APPLIANCE_PERSISTENT_NETWORK_CONFIG_IO_ERROR = 4,
    APPLIANCE_PERSISTENT_NETWORK_CONFIG_CORRUPT = 5,
    APPLIANCE_PERSISTENT_NETWORK_CONFIG_FULL = 6,
    APPLIANCE_PERSISTENT_NETWORK_CONFIG_BAD_RECORD = 7,
    APPLIANCE_PERSISTENT_NETWORK_CONFIG_BAD_CONFIG = 8,
    APPLIANCE_PERSISTENT_NETWORK_CONFIG_BUFFER_TOO_SMALL = 9,
};

enum appliance_persistent_network_config_source {
    APPLIANCE_PERSISTENT_NETWORK_CONFIG_SOURCE_NONE = 0,
    APPLIANCE_PERSISTENT_NETWORK_CONFIG_SOURCE_COMMITTED = 1,
    APPLIANCE_PERSISTENT_NETWORK_CONFIG_SOURCE_DEFAULT_MISSING = 2,
    APPLIANCE_PERSISTENT_NETWORK_CONFIG_SOURCE_DEFAULT_INVALID = 3,
};

struct appliance_persistent_network_config {
    struct appliance_network_config values;
    struct config_journal journal;
    enum appliance_persistent_network_config_source source;
    bool initialized;
};

enum appliance_persistent_network_config_status
appliance_persistent_network_config_load(
    struct appliance_persistent_network_config *config,
    struct block_device *device);
enum appliance_persistent_network_config_status
appliance_persistent_network_config_set_dhcp(
    struct appliance_persistent_network_config *config,
    const char *interface,
    size_t interface_length);
enum appliance_persistent_network_config_status
appliance_persistent_network_config_set_offline(
    struct appliance_persistent_network_config *config,
    const char *interface,
    size_t interface_length);
enum appliance_persistent_network_config_status
appliance_persistent_network_config_set_static(
    struct appliance_persistent_network_config *config,
    const char *interface,
    size_t interface_length,
    const struct network_ipv4_config *static_config);
enum appliance_persistent_network_config_status
appliance_persistent_network_config_render(
    const struct appliance_persistent_network_config *config,
    char *output,
    size_t capacity,
    size_t *output_length);
const struct appliance_network_config *
appliance_persistent_network_config_current(
    const struct appliance_persistent_network_config *config);
enum appliance_persistent_network_config_source
appliance_persistent_network_config_source(
    const struct appliance_persistent_network_config *config);
const char *appliance_persistent_network_config_status_string(
    enum appliance_persistent_network_config_status status);
const char *appliance_persistent_network_config_source_string(
    enum appliance_persistent_network_config_source source);

#endif
