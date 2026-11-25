#ifndef HFOC_APPLIANCE_PERSISTENT_STORAGE_NAMESPACE_H
#define HFOC_APPLIANCE_PERSISTENT_STORAGE_NAMESPACE_H

#include <stddef.h>
#include <stdint.h>

#define APPLIANCE_PERSISTENT_STORAGE_NAMESPACE_VERSION 1U
#define APPLIANCE_PERSISTENT_STORAGE_NAMESPACE_COUNT 6U
/*
 * ITEM-516 added the third prefix of the system-configuration namespace: the
 * management gate's after-boot key lives in the same record as the hostname,
 * the language and the network keys, so the inventory learns it.
 */
#define APPLIANCE_PERSISTENT_STORAGE_NAMESPACE_RECORD_PREFIX_MAX 3U

enum appliance_persistent_storage_namespace_id {
    APPLIANCE_PERSISTENT_STORAGE_NAMESPACE_ID_NONE = 0,
    APPLIANCE_PERSISTENT_STORAGE_NAMESPACE_ID_SYSTEM_CONFIG = 1,
    APPLIANCE_PERSISTENT_STORAGE_NAMESPACE_ID_SERVICE_DATA = 2,
    APPLIANCE_PERSISTENT_STORAGE_NAMESPACE_ID_CERTIFICATE = 3,
    APPLIANCE_PERSISTENT_STORAGE_NAMESPACE_ID_SECRET = 4,
    APPLIANCE_PERSISTENT_STORAGE_NAMESPACE_ID_DIAGNOSTIC = 5,
    APPLIANCE_PERSISTENT_STORAGE_NAMESPACE_ID_RECOVERY_METADATA = 6,
};

enum appliance_persistent_storage_namespace_sensitivity {
    APPLIANCE_PERSISTENT_STORAGE_NAMESPACE_SENSITIVITY_NONE = 0,
    APPLIANCE_PERSISTENT_STORAGE_NAMESPACE_SENSITIVITY_GENERAL = 1,
    APPLIANCE_PERSISTENT_STORAGE_NAMESPACE_SENSITIVITY_PRIVATE = 2,
    APPLIANCE_PERSISTENT_STORAGE_NAMESPACE_SENSITIVITY_SECRET = 3,
};

enum appliance_persistent_storage_namespace_status {
    APPLIANCE_PERSISTENT_STORAGE_NAMESPACE_OK = 0,
    APPLIANCE_PERSISTENT_STORAGE_NAMESPACE_NULL = 1,
    APPLIANCE_PERSISTENT_STORAGE_NAMESPACE_BAD_ID = 2,
    APPLIANCE_PERSISTENT_STORAGE_NAMESPACE_OUT_OF_RANGE = 3,
    APPLIANCE_PERSISTENT_STORAGE_NAMESPACE_BAD_KEY = 4,
    APPLIANCE_PERSISTENT_STORAGE_NAMESPACE_UNKNOWN_NAMESPACE = 5,
};

struct appliance_persistent_storage_namespace {
    enum appliance_persistent_storage_namespace_id id;
    const char *name;
    uint32_t record_prefix_count;
    const char *record_prefixes[
        APPLIANCE_PERSISTENT_STORAGE_NAMESPACE_RECORD_PREFIX_MAX];
    uint32_t record_prefix_lengths[
        APPLIANCE_PERSISTENT_STORAGE_NAMESPACE_RECORD_PREFIX_MAX];
    enum appliance_persistent_storage_namespace_sensitivity sensitivity;
    const char *purpose;
};

size_t appliance_persistent_storage_namespace_count(void);
enum appliance_persistent_storage_namespace_status
appliance_persistent_storage_namespace_catalogue_at(
    size_t index,
    const struct appliance_persistent_storage_namespace **namespace_out);
enum appliance_persistent_storage_namespace_status
appliance_persistent_storage_namespace_describe(
    enum appliance_persistent_storage_namespace_id id,
    const struct appliance_persistent_storage_namespace **namespace_out);
enum appliance_persistent_storage_namespace_status
appliance_persistent_storage_namespace_classify_record_key(
    const char *key,
    size_t key_length,
    enum appliance_persistent_storage_namespace_id *namespace_out);
const char *appliance_persistent_storage_namespace_id_string(
    enum appliance_persistent_storage_namespace_id id);
const char *appliance_persistent_storage_namespace_sensitivity_string(
    enum appliance_persistent_storage_namespace_sensitivity sensitivity);
const char *appliance_persistent_storage_namespace_status_string(
    enum appliance_persistent_storage_namespace_status status);

#endif
