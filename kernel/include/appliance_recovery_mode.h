#ifndef HFOC_APPLIANCE_RECOVERY_MODE_H
#define HFOC_APPLIANCE_RECOVERY_MODE_H

#include "appliance_config_integrity.h"

#include <stdbool.h>
#include <stdint.h>

#define APPLIANCE_RECOVERY_MODE_VERSION 1U
#define APPLIANCE_RECOVERY_MODE_DOMAIN_NONE 0U
#define APPLIANCE_RECOVERY_MODE_DOMAIN_CONFIGURATION 1U
#define APPLIANCE_RECOVERY_MODE_DOMAIN_UPDATE 2U
#define APPLIANCE_RECOVERY_MODE_DOMAIN_RUNTIME 3U
#define APPLIANCE_RECOVERY_MODE_DOMAIN_MAX 3U
#define APPLIANCE_RECOVERY_MODE_ACTION_NONE 0U
#define APPLIANCE_RECOVERY_MODE_ACTION_KEEP_LAST_KNOWN_GOOD 1U
#define APPLIANCE_RECOVERY_MODE_ACTION_SAFE_DEFAULT 2U
#define APPLIANCE_RECOVERY_MODE_ACTION_AUTOMATIC_ROLLBACK 3U
#define APPLIANCE_RECOVERY_MODE_ACTION_SUPERVISED_RESTART 4U
#define APPLIANCE_RECOVERY_MODE_ACTION_READ_ONLY 5U
#define APPLIANCE_RECOVERY_MODE_ACTION_MAX 5U

enum appliance_recovery_mode_status {
    APPLIANCE_RECOVERY_MODE_OK = 0,
    APPLIANCE_RECOVERY_MODE_NULL = 1,
    APPLIANCE_RECOVERY_MODE_BAD_STATE = 2,
    APPLIANCE_RECOVERY_MODE_BAD_REQUEST = 3,
};

struct appliance_recovery_mode_evidence {
    uint32_t version;
    uint32_t domain;
    enum appliance_config_integrity_recovery config_recovery;
    bool update_verified;
    bool pending_update;
    bool first_boot_health_failed;
    bool restart_available;
    bool recovery_trust_active;
};

struct appliance_recovery_mode_decision {
    uint32_t version;
    uint32_t domain;
    uint32_t action;
    bool local_only;
    bool requires_reboot;
};

struct appliance_recovery_mode_gate {
    uint32_t version;
    bool evaluated;
    struct appliance_recovery_mode_decision decision;
};

enum appliance_recovery_mode_status appliance_recovery_mode_init(
    struct appliance_recovery_mode_gate *gate);
enum appliance_recovery_mode_status appliance_recovery_mode_evaluate(
    struct appliance_recovery_mode_gate *gate,
    const struct appliance_recovery_mode_evidence *evidence,
    struct appliance_recovery_mode_decision *decision_out);
bool appliance_recovery_mode_is_evaluated(
    const struct appliance_recovery_mode_gate *gate);
const char *appliance_recovery_mode_status_string(
    enum appliance_recovery_mode_status status);
const char *appliance_recovery_mode_domain_string(uint32_t domain);
const char *appliance_recovery_mode_action_string(uint32_t action);

#endif
