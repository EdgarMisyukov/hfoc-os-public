#ifndef HFOC_SERVICE_RESTART_POLICY_H
#define HFOC_SERVICE_RESTART_POLICY_H

#include <stdbool.h>
#include <stdint.h>

#define SERVICE_RESTART_POLICY_VERSION 1U
#define SERVICE_RESTART_BUDGET_MAX 255U
#define SERVICE_RESTART_INTERVAL_MIN_TICKS 1U
#define SERVICE_RESTART_INTERVAL_MAX_TICKS UINT64_C(0xFFFFFFFFFFFFFFFE)

enum service_restart_policy_status {
    SERVICE_RESTART_POLICY_OK = 0,
    SERVICE_RESTART_POLICY_NULL = 1,
    SERVICE_RESTART_POLICY_BAD_STATE = 2,
    SERVICE_RESTART_POLICY_BAD_VERSION = 3,
    SERVICE_RESTART_POLICY_BAD_BUDGET = 4,
    SERVICE_RESTART_POLICY_BAD_INTERVAL = 5,
    SERVICE_RESTART_POLICY_INTERVAL = 6,
    SERVICE_RESTART_POLICY_EXHAUSTED = 7,
    SERVICE_RESTART_POLICY_DISABLED = 8,
    SERVICE_RESTART_POLICY_TIME_OVERFLOW = 9,
};

enum service_restart_policy_state {
    SERVICE_RESTART_POLICY_STATE_ACTIVE = 0,
    SERVICE_RESTART_POLICY_STATE_EXHAUSTED = 1,
    SERVICE_RESTART_POLICY_STATE_DISABLED = 2,
};

struct service_restart_policy {
    uint32_t version;
    uint32_t restart_budget;
    uint64_t restart_interval_ticks;
    bool disabled_after_exhaustion;
    bool published;
    enum service_restart_policy_state state;
    uint32_t restart_count;
    uint64_t next_allowed_tick;
};

enum service_restart_policy_status service_restart_policy_validate(
    const struct service_restart_policy *policy);
enum service_restart_policy_status service_restart_policy_init(
    struct service_restart_policy *policy,
    uint32_t restart_budget,
    uint64_t restart_interval_ticks,
    bool disabled_after_exhaustion);
enum service_restart_policy_status service_restart_policy_can_restart(
    const struct service_restart_policy *policy,
    uint64_t now_ticks,
    bool *allowed);
enum service_restart_policy_status service_restart_policy_record_restart(
    struct service_restart_policy *policy,
    uint64_t now_ticks);
enum service_restart_policy_status service_restart_policy_reset(
    struct service_restart_policy *policy);
uint32_t service_restart_policy_restart_count(
    const struct service_restart_policy *policy);
enum service_restart_policy_state service_restart_policy_state(
    const struct service_restart_policy *policy);
const char *service_restart_policy_status_string(
    enum service_restart_policy_status status);
const char *service_restart_policy_state_string(
    enum service_restart_policy_state state);

#endif
