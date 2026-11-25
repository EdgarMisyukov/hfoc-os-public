#ifndef HFOC_APPLIANCE_HTTPS_SERVICE_H
#define HFOC_APPLIANCE_HTTPS_SERVICE_H

#include "appliance_service_manager.h"
#include "https_service.h"
#include "https_service_lifecycle.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define APPLIANCE_HTTPS_SERVICE_VERSION 1U
#define APPLIANCE_HTTPS_SERVICE_OUTPUT_MAX 320U

enum appliance_https_service_state {
    APPLIANCE_HTTPS_SERVICE_STATE_STOPPED = 0,
    APPLIANCE_HTTPS_SERVICE_STATE_STARTING = 1,
    APPLIANCE_HTTPS_SERVICE_STATE_RUNNING = 2,
    APPLIANCE_HTTPS_SERVICE_STATE_STOPPING = 3,
    APPLIANCE_HTTPS_SERVICE_STATE_FAILED = 4,
};

enum appliance_https_service_failure {
    APPLIANCE_HTTPS_SERVICE_FAILURE_NONE = 0,
    APPLIANCE_HTTPS_SERVICE_FAILURE_START = 1,
    APPLIANCE_HTTPS_SERVICE_FAILURE_READY = 2,
    APPLIANCE_HTTPS_SERVICE_FAILURE_STOP = 3,
    APPLIANCE_HTTPS_SERVICE_FAILURE_RESTART = 4,
    APPLIANCE_HTTPS_SERVICE_FAILURE_SNAPSHOT = 5,
};

enum appliance_https_service_status {
    APPLIANCE_HTTPS_SERVICE_OK = 0,
    APPLIANCE_HTTPS_SERVICE_NULL = 1,
    APPLIANCE_HTTPS_SERVICE_BAD_STATE = 2,
    APPLIANCE_HTTPS_SERVICE_ALREADY_PUBLISHED = 3,
    APPLIANCE_HTTPS_SERVICE_UNPUBLISHED = 4,
    APPLIANCE_HTTPS_SERVICE_INVALID_MANAGER = 5,
    APPLIANCE_HTTPS_SERVICE_INVALID_DESCRIPTOR = 6,
    APPLIANCE_HTTPS_SERVICE_INVALID_CONFIG = 7,
    APPLIANCE_HTTPS_SERVICE_INVALID_READINESS = 8,
    APPLIANCE_HTTPS_SERVICE_DEPENDENCY_UNAVAILABLE = 9,
    APPLIANCE_HTTPS_SERVICE_START_FAILED = 10,
    APPLIANCE_HTTPS_SERVICE_READY_FAILED = 11,
    APPLIANCE_HTTPS_SERVICE_STOP_PENDING = 12,
    APPLIANCE_HTTPS_SERVICE_STOP_FAILED = 13,
    APPLIANCE_HTTPS_SERVICE_RESTART_FAILED = 14,
    APPLIANCE_HTTPS_SERVICE_SNAPSHOT_FAILED = 15,
    APPLIANCE_HTTPS_SERVICE_OVERFLOW = 16,
};

struct appliance_https_service {
    bool initialized;
    uint32_t version;
    enum appliance_https_service_state state;
    enum appliance_https_service_failure failure;
    struct appliance_service_manager *manager;
    struct https_service_descriptor descriptor;
    struct https_service_lifecycle lifecycle;
    uint32_t start_count;
    uint32_t stop_count;
    uint32_t restart_count;
};

struct appliance_https_service_snapshot {
    bool published;
    uint32_t version;
    enum appliance_https_service_state state;
    enum appliance_https_service_failure failure;
    enum appliance_service_manager_state manager_state;
    uint64_t manager_generation;
    enum health_check_result_state manager_health;
    enum service_health_state service_health;
    enum https_service_lifecycle_phase lifecycle_phase;
    uint32_t lifecycle_generation;
    uint32_t active_connection_count;
    uint32_t start_count;
    uint32_t stop_count;
    uint32_t restart_count;
    uint32_t lifecycle_stopped_count;
    uint32_t drained_count;
    uint32_t aborted_count;
    uint32_t zeroized_count;
    uint32_t http_released_count;
    uint32_t packet_released_count;
};

enum appliance_https_service_status appliance_https_service_init(
    struct appliance_https_service *service,
    struct appliance_service_manager *manager,
    const struct https_service_descriptor *descriptor,
    const struct https_service_lifecycle_config *lifecycle_config,
    const struct https_service_lifecycle_ops *lifecycle_ops);
enum appliance_https_service_status appliance_https_service_start(
    struct appliance_https_service *service,
    const struct https_service_readiness *readiness);
enum appliance_https_service_status appliance_https_service_ready(
    struct appliance_https_service *service,
    const struct https_service_readiness *readiness);
enum appliance_https_service_status appliance_https_service_stop_admission(
    struct appliance_https_service *service,
    uint32_t now_ms);
enum appliance_https_service_status appliance_https_service_stop_step(
    struct appliance_https_service *service,
    uint32_t now_ms);
enum appliance_https_service_status appliance_https_service_restart(
    struct appliance_https_service *service,
    const struct https_service_readiness *readiness,
    uint32_t now_ms,
    uint64_t now_ticks,
    uint32_t *exit_status_out,
    bool *restarted_out);
enum appliance_https_service_status appliance_https_service_snapshot(
    const struct appliance_https_service *service,
    struct appliance_https_service_snapshot *snapshot_out);
enum appliance_https_service_status appliance_https_service_render(
    const struct appliance_https_service_snapshot *snapshot,
    char *output,
    size_t capacity,
    size_t *output_length);
bool appliance_https_service_published(
    const struct appliance_https_service *service);
enum appliance_https_service_state appliance_https_service_state(
    const struct appliance_https_service *service);
enum appliance_https_service_failure appliance_https_service_failure(
    const struct appliance_https_service *service);
const char *appliance_https_service_state_string(
    enum appliance_https_service_state state);
const char *appliance_https_service_failure_string(
    enum appliance_https_service_failure failure);
const char *appliance_https_service_status_string(
    enum appliance_https_service_status status);

#endif
