#ifndef HFOC_APPLIANCE_FACTORY_RESET_H
#define HFOC_APPLIANCE_FACTORY_RESET_H

#include "appliance_persistent_storage_namespace.h"

#include <stdbool.h>
#include <stdint.h>

#define APPLIANCE_FACTORY_RESET_VERSION 1U
#define APPLIANCE_FACTORY_RESET_NAMESPACE_COUNT \
    APPLIANCE_PERSISTENT_STORAGE_NAMESPACE_COUNT

enum appliance_factory_reset_status {
    APPLIANCE_FACTORY_RESET_OK = 0,
    APPLIANCE_FACTORY_RESET_NULL = 1,
    APPLIANCE_FACTORY_RESET_BAD_STATE = 2,
    APPLIANCE_FACTORY_RESET_BAD_REQUEST = 3,
    APPLIANCE_FACTORY_RESET_UNAUTHORIZED = 4,
    APPLIANCE_FACTORY_RESET_BOOT_FOUNDATION_UNAVAILABLE = 5,
    APPLIANCE_FACTORY_RESET_STORAGE_NOT_READY = 6,
    APPLIANCE_FACTORY_RESET_RECOVERY_TRUST_ACTIVE = 7,
};

enum appliance_factory_reset_action {
    APPLIANCE_FACTORY_RESET_ACTION_NONE = 0,
    APPLIANCE_FACTORY_RESET_ACTION_REWRITE_FACTORY_DEFAULT = 1,
    APPLIANCE_FACTORY_RESET_ACTION_CLEAR = 2,
    APPLIANCE_FACTORY_RESET_ACTION_PRESERVE_FOUNDATION = 3,
    APPLIANCE_FACTORY_RESET_ACTION_MAX = 3,
};

struct appliance_factory_reset_evidence {
    uint32_t version;
    bool authorization_verified;
    bool boot_foundation_verified;
    bool storage_loader_ready;
    bool recovery_trust_active;
};

struct appliance_factory_reset_namespace_plan {
    enum appliance_persistent_storage_namespace_id id;
    uint32_t action;
};

struct appliance_factory_reset_plan {
    uint32_t version;
    uint32_t namespace_count;
    bool local_only;
    bool requires_reboot;
    bool preserves_boot_foundation;
    bool preserves_recovery_foundation;
    struct appliance_factory_reset_namespace_plan namespaces[
        APPLIANCE_FACTORY_RESET_NAMESPACE_COUNT];
};

struct appliance_factory_reset_gate {
    uint32_t version;
    bool published;
    struct appliance_factory_reset_plan plan;
};

enum appliance_factory_reset_status appliance_factory_reset_init(
    struct appliance_factory_reset_gate *gate);
enum appliance_factory_reset_status appliance_factory_reset_evaluate(
    struct appliance_factory_reset_gate *gate,
    const struct appliance_factory_reset_evidence *evidence,
    struct appliance_factory_reset_plan *plan_out);
bool appliance_factory_reset_is_published(
    const struct appliance_factory_reset_gate *gate);
const char *appliance_factory_reset_status_string(
    enum appliance_factory_reset_status status);
const char *appliance_factory_reset_action_string(uint32_t action);

#endif
