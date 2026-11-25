#ifndef HFOC_APPLIANCE_PERSISTENT_SERVICE_CONFIG_H
#define HFOC_APPLIANCE_PERSISTENT_SERVICE_CONFIG_H

#include "appliance_service_manager.h"
#include "block_device.h"
#include "config_journal.h"
#include "service.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define APPLIANCE_PERSISTENT_SERVICE_CONFIG_VERSION 1U
#define APPLIANCE_PERSISTENT_SERVICE_CONFIG_CAPACITY \
    SERVICE_DEPENDENCY_SET_MAX
#define APPLIANCE_PERSISTENT_SERVICE_CONFIG_RECORD_MAX \
    CONFIG_JOURNAL_DATA_MAX

enum appliance_persistent_service_config_status {
    APPLIANCE_PERSISTENT_SERVICE_CONFIG_OK = 0,
    APPLIANCE_PERSISTENT_SERVICE_CONFIG_NULL = 1,
    APPLIANCE_PERSISTENT_SERVICE_CONFIG_BAD_STATE = 2,
    APPLIANCE_PERSISTENT_SERVICE_CONFIG_BAD_DEVICE = 3,
    APPLIANCE_PERSISTENT_SERVICE_CONFIG_IO_ERROR = 4,
    APPLIANCE_PERSISTENT_SERVICE_CONFIG_CORRUPT = 5,
    APPLIANCE_PERSISTENT_SERVICE_CONFIG_FULL = 6,
    APPLIANCE_PERSISTENT_SERVICE_CONFIG_BAD_RECORD = 7,
    APPLIANCE_PERSISTENT_SERVICE_CONFIG_BAD_CONFIG = 8,
    APPLIANCE_PERSISTENT_SERVICE_CONFIG_UNKNOWN_SERVICE = 9,
    APPLIANCE_PERSISTENT_SERVICE_CONFIG_ENTRY_UNAVAILABLE = 10,
    APPLIANCE_PERSISTENT_SERVICE_CONFIG_MANAGER_BAD_STATE = 11,
};

enum appliance_persistent_service_config_source {
    APPLIANCE_PERSISTENT_SERVICE_CONFIG_SOURCE_NONE = 0,
    APPLIANCE_PERSISTENT_SERVICE_CONFIG_SOURCE_COMMITTED = 1,
    APPLIANCE_PERSISTENT_SERVICE_CONFIG_SOURCE_DEFAULT_MISSING = 2,
    APPLIANCE_PERSISTENT_SERVICE_CONFIG_SOURCE_DEFAULT_INVALID = 3,
};

struct appliance_persistent_service_config_values {
    bool enabled;
    uint32_t restart_limit;
    uint32_t readiness_timeout_ms;
};

struct appliance_persistent_service_config {
    struct service_descriptor
        descriptors[APPLIANCE_PERSISTENT_SERVICE_CONFIG_CAPACITY];
    uint32_t descriptor_count;
    struct config_journal journal;
    enum appliance_persistent_service_config_source source;
    bool initialized;
};

enum appliance_persistent_service_config_status
appliance_persistent_service_config_load(
    struct appliance_persistent_service_config *config,
    struct block_device *device,
    const struct service_descriptor *baseline,
    uint32_t baseline_count);
enum appliance_persistent_service_config_status
appliance_persistent_service_config_set_config(
    struct appliance_persistent_service_config *config,
    uint32_t service_id,
    const struct appliance_persistent_service_config_values *values);
enum appliance_persistent_service_config_status
appliance_persistent_service_config_apply(
    const struct appliance_persistent_service_config *config,
    struct appliance_service_manager *manager,
    const struct service_supervisor_entry *entries,
    uint32_t entry_count);
uint32_t appliance_persistent_service_config_count(
    const struct appliance_persistent_service_config *config);
const struct service_descriptor *
appliance_persistent_service_config_descriptor(
    const struct appliance_persistent_service_config *config,
    uint32_t position);
bool appliance_persistent_service_config_enabled(
    const struct appliance_persistent_service_config *config,
    uint32_t service_id);
enum appliance_persistent_service_config_source
appliance_persistent_service_config_source(
    const struct appliance_persistent_service_config *config);
const char *appliance_persistent_service_config_status_string(
    enum appliance_persistent_service_config_status status);
const char *appliance_persistent_service_config_source_string(
    enum appliance_persistent_service_config_source source);

#endif
