#ifndef HFOC_APPLIANCE_CONFIG_INTEGRITY_H
#define HFOC_APPLIANCE_CONFIG_INTEGRITY_H

#include "appliance_persistent_storage_namespace.h"
#include "config_journal.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define APPLIANCE_CONFIG_INTEGRITY_VERSION 1U
#define APPLIANCE_CONFIG_INTEGRITY_RECORD_COUNT 11U
#define APPLIANCE_CONFIG_INTEGRITY_SERVICE_ID_MIN 1U
#define APPLIANCE_CONFIG_INTEGRITY_SERVICE_ID_MAX 255U
#define APPLIANCE_CONFIG_INTEGRITY_RECORD_MAX CONFIG_JOURNAL_DATA_MAX

enum appliance_config_integrity_record_kind {
    APPLIANCE_CONFIG_INTEGRITY_KIND_NONE = 0,
    APPLIANCE_CONFIG_INTEGRITY_KIND_SYSTEM_HOSTNAME = 1,
    APPLIANCE_CONFIG_INTEGRITY_KIND_NETWORK_INTERFACE = 2,
    APPLIANCE_CONFIG_INTEGRITY_KIND_NETWORK_MODE = 3,
    APPLIANCE_CONFIG_INTEGRITY_KIND_NETWORK_IPV4_ADDRESS = 4,
    APPLIANCE_CONFIG_INTEGRITY_KIND_NETWORK_IPV4_PREFIX = 5,
    APPLIANCE_CONFIG_INTEGRITY_KIND_NETWORK_IPV4_GATEWAY = 6,
    APPLIANCE_CONFIG_INTEGRITY_KIND_NETWORK_IPV4_DNS = 7,
    APPLIANCE_CONFIG_INTEGRITY_KIND_SERVICE_TABLE = 8,
    APPLIANCE_CONFIG_INTEGRITY_KIND_SERVICE_ENTRY = 9,
    /* ITEM-509 increment 2: the interface language of the system record. */
    APPLIANCE_CONFIG_INTEGRITY_KIND_SYSTEM_LANGUAGE = 10,
    /* ITEM-516: the management gate's after-boot policy of the same record. */
    APPLIANCE_CONFIG_INTEGRITY_KIND_SECURITY_PANEL_AFTER_BOOT = 11,
};

enum appliance_config_integrity_recovery {
    APPLIANCE_CONFIG_INTEGRITY_RECOVERY_NONE = 0,
    APPLIANCE_CONFIG_INTEGRITY_RECOVERY_COMMIT = 1,
    APPLIANCE_CONFIG_INTEGRITY_RECOVERY_KEEP_LAST_KNOWN_GOOD = 2,
    APPLIANCE_CONFIG_INTEGRITY_RECOVERY_DEFAULT_MISSING = 3,
    APPLIANCE_CONFIG_INTEGRITY_RECOVERY_DEFAULT_INVALID = 4,
    APPLIANCE_CONFIG_INTEGRITY_RECOVERY_EXPLICIT_ERROR = 5,
};

enum appliance_config_integrity_status {
    APPLIANCE_CONFIG_INTEGRITY_OK = 0,
    APPLIANCE_CONFIG_INTEGRITY_NULL = 1,
    APPLIANCE_CONFIG_INTEGRITY_OUT_OF_RANGE = 2,
    APPLIANCE_CONFIG_INTEGRITY_BAD_RECORD = 3,
    APPLIANCE_CONFIG_INTEGRITY_BAD_VERSION = 4,
    APPLIANCE_CONFIG_INTEGRITY_UNKNOWN_KEY = 5,
    APPLIANCE_CONFIG_INTEGRITY_BAD_ID = 6,
};

struct appliance_config_integrity_record {
    enum appliance_config_integrity_record_kind kind;
    enum appliance_persistent_storage_namespace_id namespace_id;
    const char *name;
    const char *prefix;
    uint32_t version;
    uint32_t minimum_version;
    uint32_t maximum_version;
    const char *purpose;
};

size_t appliance_config_integrity_count(void);
enum appliance_config_integrity_status
appliance_config_integrity_catalogue_at(
    size_t index,
    const struct appliance_config_integrity_record **record_out);
enum appliance_config_integrity_status
appliance_config_integrity_describe(
    enum appliance_config_integrity_record_kind kind,
    const struct appliance_config_integrity_record **record_out);
enum appliance_config_integrity_status
appliance_config_integrity_classify_record_key(
    const char *key,
    size_t key_length,
    enum appliance_config_integrity_record_kind *kind_out,
    enum appliance_persistent_storage_namespace_id *namespace_out,
    uint32_t *version_out);
enum appliance_config_integrity_recovery
appliance_config_integrity_recovery_plan(
    enum config_journal_status journal_status,
    bool record_valid,
    bool last_known_good_available);
const char *appliance_config_integrity_record_kind_string(
    enum appliance_config_integrity_record_kind kind);
const char *appliance_config_integrity_recovery_string(
    enum appliance_config_integrity_recovery recovery);
const char *appliance_config_integrity_status_string(
    enum appliance_config_integrity_status status);

#endif
