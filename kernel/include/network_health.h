#ifndef HFOC_NETWORK_HEALTH_H
#define HFOC_NETWORK_HEALTH_H

#include "health_check.h"
#include "link_state.h"
#include "network_admission.h"
#include "network_config_transaction.h"
#include "network_device.h"

#include <stdbool.h>
#include <stdint.h>

#define NETWORK_HEALTH_VERSION 1U

enum network_health_status {
    NETWORK_HEALTH_OK = 0,
    NETWORK_HEALTH_NULL = 1,
    NETWORK_HEALTH_BAD_VERSION = 2,
    NETWORK_HEALTH_BAD_STATE = 3,
};

enum network_health_fault {
    NETWORK_HEALTH_FAULT_NONE = 0,
    NETWORK_HEALTH_FAULT_LINK = 1,
    NETWORK_HEALTH_FAULT_CONFIG = 2,
    NETWORK_HEALTH_FAULT_ADMISSION = 3,
};

struct network_health_input {
    uint32_t version;
    const struct network_device_selection *device;
    const struct network_link_selection *link;
    const struct network_config_transaction *config;
    const struct network_admission_selection *admission;
};

struct network_health_verdict {
    uint32_t version;
    bool healthy;
    enum health_check_result_state result;
    enum health_check_severity severity;
    enum network_health_fault fault;
    uint32_t device_id;
    enum network_link_state link_state;
    uint32_t link_generation;
    bool has_active_config;
    bool has_pending_config;
    bool admission_published;
    enum network_admission_mode admission_mode;
    bool local_control_available;
};

enum network_health_status network_health_evaluate(
    const struct network_health_input *input,
    struct network_health_verdict *verdict_out);
const char *network_health_status_string(
    enum network_health_status status);
const char *network_health_fault_string(
    enum network_health_fault fault);

#endif
