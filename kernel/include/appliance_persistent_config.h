#ifndef HFOC_APPLIANCE_PERSISTENT_CONFIG_H
#define HFOC_APPLIANCE_PERSISTENT_CONFIG_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "appliance_config_validation.h"
#include "block_device.h"
#include "config_journal.h"

#define APPLIANCE_PERSISTENT_CONFIG_FORMAT_VERSION \
    APPLIANCE_CONFIG_VALIDATION_FORMAT_VERSION
#define APPLIANCE_PERSISTENT_CONFIG_KEY \
    APPLIANCE_CONFIG_VALIDATION_KEY
#define APPLIANCE_PERSISTENT_CONFIG_DEFAULT_HOSTNAME \
    APPLIANCE_CONFIG_VALIDATION_DEFAULT_HOSTNAME
#define APPLIANCE_PERSISTENT_CONFIG_HOSTNAME_MAX \
    APPLIANCE_CONFIG_VALIDATION_HOSTNAME_MAX
#define APPLIANCE_PERSISTENT_CONFIG_DEFAULT_LANGUAGE \
    APPLIANCE_LANGUAGE_DEFAULT
#define APPLIANCE_PERSISTENT_CONFIG_LANGUAGE_MAX \
    APPLIANCE_CONFIG_VALIDATION_LANGUAGE_MAX
#define APPLIANCE_PERSISTENT_CONFIG_PANEL_AFTER_BOOT_MAX \
    APPLIANCE_CONFIG_VALIDATION_PANEL_AFTER_BOOT_MAX
#define APPLIANCE_PERSISTENT_CONFIG_PANEL_AFTER_BOOT_PREFIX \
    APPLIANCE_CONFIG_VALIDATION_PANEL_AFTER_BOOT_PREFIX
#define APPLIANCE_PERSISTENT_CONFIG_RECORD_MAX \
    APPLIANCE_CONFIG_VALIDATION_RECORD_MAX

enum appliance_persistent_config_status {
    APPLIANCE_PERSISTENT_CONFIG_OK = 0,
    APPLIANCE_PERSISTENT_CONFIG_NULL = 1,
    APPLIANCE_PERSISTENT_CONFIG_BAD_STATE = 2,
    APPLIANCE_PERSISTENT_CONFIG_BAD_DEVICE = 3,
    APPLIANCE_PERSISTENT_CONFIG_IO_ERROR = 4,
    APPLIANCE_PERSISTENT_CONFIG_CORRUPT = 5,
    APPLIANCE_PERSISTENT_CONFIG_FULL = 6,
    APPLIANCE_PERSISTENT_CONFIG_BAD_RECORD = 7,
    APPLIANCE_PERSISTENT_CONFIG_BAD_HOSTNAME = 8,
    APPLIANCE_PERSISTENT_CONFIG_BUFFER_TOO_SMALL = 9,
    APPLIANCE_PERSISTENT_CONFIG_BAD_LANGUAGE = 10,
    /* ITEM-516: the after-boot key carries a value outside `open|closed`. */
    APPLIANCE_PERSISTENT_CONFIG_BAD_PANEL_AFTER_BOOT = 11,
};

struct appliance_persistent_config {
    struct appliance_config_validation_state values;
    struct config_journal journal;
    bool initialized;
};

enum appliance_persistent_config_status
appliance_persistent_config_load(
    struct appliance_persistent_config *config,
    struct block_device *device);
enum appliance_persistent_config_status
appliance_persistent_config_set_hostname(
    struct appliance_persistent_config *config,
    const char *hostname,
    size_t hostname_length);
enum appliance_persistent_config_status
appliance_persistent_config_hostname(
    const struct appliance_persistent_config *config,
    char *hostname,
    size_t capacity,
    size_t *hostname_length);
/*
 * ITEM-509 increment 2: the durable interface language, the second key of the
 * same system record.  `set_language` writes the language line only when the
 * language is not the default `en`, so an appliance that never chose one
 * writes the record of the previous format.
 */
enum appliance_persistent_config_status
appliance_persistent_config_set_language(
    struct appliance_persistent_config *config,
    enum appliance_language language);
enum appliance_persistent_config_status
appliance_persistent_config_language(
    const struct appliance_persistent_config *config,
    enum appliance_language *language);
/*
 * ITEM-516: the durable management gate, the third key of the same system
 * record.  `set_panel_after_boot(config, true)` writes the
 * `v1:security.panel.after-boot=open` line; `set_panel_after_boot(config,
 * false)` writes the record without it, because `closed` is the default and is
 * never written - a machine that never enabled the policy keeps exactly the
 * bytes the previous format wrote.  The getter answers `false` (closed) for a
 * record that predates the key.
 */
enum appliance_persistent_config_status
appliance_persistent_config_set_panel_after_boot(
    struct appliance_persistent_config *config,
    bool open);
enum appliance_persistent_config_status
appliance_persistent_config_panel_after_boot(
    const struct appliance_persistent_config *config,
    bool *open_out);
const char *appliance_persistent_config_status_string(
    enum appliance_persistent_config_status status);
enum appliance_config_validation_source
appliance_persistent_config_source(
    const struct appliance_persistent_config *config);

#endif
