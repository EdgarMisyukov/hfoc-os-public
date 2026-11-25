#ifndef HFOC_APPLIANCE_SERVICE_FAILURE_HANDLER_H
#define HFOC_APPLIANCE_SERVICE_FAILURE_HANDLER_H

#include "appliance_service_manager.h"
#include "health_check.h"
#include "service_supervisor.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define APPLIANCE_SERVICE_FAILURE_HANDLER_VERSION 1U
#define APPLIANCE_SERVICE_FAILURE_HANDLER_OUTPUT_MAX 320U

enum appliance_service_failure_kind {
    APPLIANCE_SERVICE_FAILURE_KIND_NONE = 0,
    APPLIANCE_SERVICE_FAILURE_KIND_EXIT = 1,
    APPLIANCE_SERVICE_FAILURE_KIND_READINESS_TIMEOUT = 2,
    APPLIANCE_SERVICE_FAILURE_KIND_LIVENESS_FAILED = 3,
};

enum appliance_service_failure_action {
    APPLIANCE_SERVICE_FAILURE_ACTION_NONE = 0,
    APPLIANCE_SERVICE_FAILURE_ACTION_RESTART = 1,
    APPLIANCE_SERVICE_FAILURE_ACTION_DEFER = 2,
    APPLIANCE_SERVICE_FAILURE_ACTION_ISOLATE = 3,
};

enum appliance_service_failure_result {
    APPLIANCE_SERVICE_FAILURE_RESULT_NONE = 0,
    APPLIANCE_SERVICE_FAILURE_RESULT_RESTARTED = 1,
    APPLIANCE_SERVICE_FAILURE_RESULT_DEFERRED = 2,
    APPLIANCE_SERVICE_FAILURE_RESULT_EXHAUSTED = 3,
    APPLIANCE_SERVICE_FAILURE_RESULT_NOT_RESTARTED = 4,
};

enum appliance_service_failure_handler_status {
    APPLIANCE_SERVICE_FAILURE_HANDLER_OK = 0,
    APPLIANCE_SERVICE_FAILURE_HANDLER_NULL = 1,
    APPLIANCE_SERVICE_FAILURE_HANDLER_BAD_STATE = 2,
    APPLIANCE_SERVICE_FAILURE_HANDLER_NOT_CONFIGURED = 3,
    APPLIANCE_SERVICE_FAILURE_HANDLER_NO_FAILURE = 4,
    APPLIANCE_SERVICE_FAILURE_HANDLER_SNAPSHOT_FAILED = 5,
    APPLIANCE_SERVICE_FAILURE_HANDLER_RESTART_FAILED = 6,
    APPLIANCE_SERVICE_FAILURE_HANDLER_OVERFLOW = 7,
};

struct appliance_service_failure_event {
    bool published;
    uint32_t version;
    uint32_t service_id;
    enum appliance_service_failure_kind kind;
    enum appliance_service_failure_action action;
    enum appliance_service_failure_result result;
    uint32_t exit_status;
    uint32_t restart_count;
    uint64_t now_ticks;
};

struct appliance_service_failure_handler {
    bool initialized;
    uint32_t version;
    struct appliance_service_manager *manager;
    uint32_t observed_count;
    uint32_t recovered_count;
    uint32_t deferred_count;
    uint32_t exhausted_count;
    struct appliance_service_failure_event last_event;
};

struct appliance_service_failure_handler_snapshot {
    bool published;
    uint32_t version;
    uint32_t observed_count;
    uint32_t recovered_count;
    uint32_t deferred_count;
    uint32_t exhausted_count;
    struct appliance_service_failure_event last_event;
    enum appliance_service_manager_state manager_state;
    enum health_check_result_state manager_health;
};

enum appliance_service_failure_handler_status
appliance_service_failure_handler_init(
    struct appliance_service_failure_handler *handler,
    struct appliance_service_manager *manager);
enum appliance_service_failure_handler_status
appliance_service_failure_handler_poll(
    struct appliance_service_failure_handler *handler,
    uint64_t now_ticks,
    struct appliance_service_failure_event *event_out);
enum appliance_service_failure_handler_status
appliance_service_failure_handler_snapshot(
    const struct appliance_service_failure_handler *handler,
    struct appliance_service_failure_handler_snapshot *snapshot_out);
enum appliance_service_failure_handler_status
appliance_service_failure_handler_render(
    const struct appliance_service_failure_handler_snapshot *snapshot,
    char *output,
    size_t capacity,
    size_t *output_length);
const char *appliance_service_failure_handler_status_string(
    enum appliance_service_failure_handler_status status);
const char *appliance_service_failure_kind_string(
    enum appliance_service_failure_kind kind);
const char *appliance_service_failure_action_string(
    enum appliance_service_failure_action action);
const char *appliance_service_failure_result_string(
    enum appliance_service_failure_result result);
const char *appliance_service_failure_handler_state_string(
    enum appliance_service_manager_state state);

#endif
