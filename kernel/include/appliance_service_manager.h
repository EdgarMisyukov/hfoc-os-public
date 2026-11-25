#ifndef HFOC_APPLIANCE_SERVICE_MANAGER_H
#define HFOC_APPLIANCE_SERVICE_MANAGER_H

#include "health_check.h"
#include "service.h"
#include "service_health.h"
#include "service_supervisor.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define APPLIANCE_SERVICE_MANAGER_VERSION 1U
#define APPLIANCE_SERVICE_MANAGER_CAPACITY SERVICE_SUPERVISOR_CAPACITY
#define APPLIANCE_SERVICE_MANAGER_OUTPUT_MAX 256U

enum appliance_service_manager_state {
    APPLIANCE_SERVICE_MANAGER_STATE_STOPPED = 0,
    APPLIANCE_SERVICE_MANAGER_STATE_RUNNING = 1,
    APPLIANCE_SERVICE_MANAGER_STATE_START_FAILED = 2,
    APPLIANCE_SERVICE_MANAGER_STATE_STOP_FAILED = 3,
};

enum appliance_service_manager_startup_policy {
    APPLIANCE_SERVICE_MANAGER_STARTUP_ROLLBACK = 0,
    APPLIANCE_SERVICE_MANAGER_STARTUP_PRESERVE = 1,
};

enum appliance_service_manager_status {
    APPLIANCE_SERVICE_MANAGER_OK = 0,
    APPLIANCE_SERVICE_MANAGER_NULL = 1,
    APPLIANCE_SERVICE_MANAGER_BAD_STATE = 2,
    APPLIANCE_SERVICE_MANAGER_NOT_CONFIGURED = 3,
    APPLIANCE_SERVICE_MANAGER_INVALID_DESCRIPTOR_SET = 4,
    APPLIANCE_SERVICE_MANAGER_INVALID_ENTRIES = 5,
    APPLIANCE_SERVICE_MANAGER_START_FAILED = 6,
    APPLIANCE_SERVICE_MANAGER_STOP_FAILED = 7,
    APPLIANCE_SERVICE_MANAGER_RESTART_FAILED = 8,
    APPLIANCE_SERVICE_MANAGER_RESTART_DISABLED = 9,
    APPLIANCE_SERVICE_MANAGER_SNAPSHOT_FAILED = 10,
    APPLIANCE_SERVICE_MANAGER_HEALTH_FAILED = 11,
    APPLIANCE_SERVICE_MANAGER_OVERFLOW = 12,
};

struct appliance_service_manager_ops {
    enum service_supervisor_status (*start_ordered)(
        void *context,
        const struct service_graph_snapshot *snapshot,
        const struct service_supervisor_entry *entries,
        uint32_t entry_count);
    enum service_supervisor_status (*stop_ordered)(
        void *context,
        const struct service_graph_snapshot *snapshot);
    enum service_supervisor_status (*restart_contained)(
        void *context,
        uint32_t service_id,
        uint64_t now_ticks,
        const struct service_graph_snapshot *snapshot,
        uint32_t *exit_status_out,
        bool *restarted);
    enum service_supervisor_status (*snapshot)(
        void *context,
        struct service_supervisor_snapshot *snapshot);
    void *context;
};

struct appliance_service_manager {
    bool initialized;
    uint32_t version;
    enum appliance_service_manager_state state;
    enum appliance_service_manager_startup_policy startup_policy;
    uint64_t generation;
    struct service_graph_snapshot graph;
    struct service_supervisor_entry entries[
        APPLIANCE_SERVICE_MANAGER_CAPACITY];
    uint32_t entry_count;
    struct appliance_service_manager_ops ops;
};

struct appliance_service_manager_snapshot {
    uint32_t version;
    bool published;
    enum appliance_service_manager_state state;
    enum appliance_service_manager_startup_policy startup_policy;
    uint64_t generation;
    uint32_t configured_count;
    uint32_t active_count;
    uint32_t ready_count;
    uint32_t pending_count;
    uint32_t failed_count;
    uint32_t exhausted_count;
    enum health_check_result_state health_result;
    enum service_health_fault health_fault;
};

enum appliance_service_manager_status appliance_service_manager_init(
    struct appliance_service_manager *manager,
    const struct appliance_service_manager_ops *ops,
    enum appliance_service_manager_startup_policy startup_policy);
enum appliance_service_manager_status appliance_service_manager_configure(
    struct appliance_service_manager *manager,
    const struct service_descriptor *descriptors,
    uint32_t descriptor_count,
    const struct service_supervisor_entry *entries,
    uint32_t entry_count);
enum appliance_service_manager_status appliance_service_manager_start(
    struct appliance_service_manager *manager);
enum appliance_service_manager_status appliance_service_manager_stop(
    struct appliance_service_manager *manager);
enum appliance_service_manager_status appliance_service_manager_restart(
    struct appliance_service_manager *manager,
    uint32_t service_id,
    uint64_t now_ticks,
    uint32_t *exit_status_out,
    bool *restarted_out);
enum appliance_service_manager_status appliance_service_manager_snapshot(
    const struct appliance_service_manager *manager,
    struct appliance_service_manager_snapshot *snapshot_out);
enum appliance_service_manager_status appliance_service_manager_render(
    const struct appliance_service_manager_snapshot *snapshot,
    char *output,
    size_t capacity,
    size_t *output_length);
const char *appliance_service_manager_status_string(
    enum appliance_service_manager_status status);
const char *appliance_service_manager_state_string(
    enum appliance_service_manager_state state);
const char *appliance_service_manager_startup_policy_string(
    enum appliance_service_manager_startup_policy policy);

#endif
