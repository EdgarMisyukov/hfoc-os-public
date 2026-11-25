#ifndef HFOC_SERVICE_SUPERVISOR_H
#define HFOC_SERVICE_SUPERVISOR_H

#include "process.h"
#include "service_restart_policy.h"
#include "service.h"

#include <stdbool.h>
#include <stdint.h>

#define SERVICE_SUPERVISOR_CAPACITY 8U
#define SERVICE_SUPERVISOR_ENTRY_MAX SERVICE_GRAPH_SNAPSHOT_MAX
#define SERVICE_SUPERVISOR_RESTART_INTERVAL_DEFAULT_TICKS 1U
#define SERVICE_LIVENESS_TIMEOUT_MIN_TICKS 1U
#define SERVICE_LIVENESS_TIMEOUT_MAX_TICKS 0xFFFFFFFFU
#define SERVICE_READINESS_TIMEOUT_MIN_TICKS 1U
#define SERVICE_READINESS_TIMEOUT_MAX_TICKS 0xFFFFFFFFU
#define SERVICE_SUPERVISOR_SNAPSHOT_VERSION 1U
#define SERVICE_SUPERVISOR_SNAPSHOT_MAX SERVICE_SUPERVISOR_CAPACITY

enum service_health_state {
    SERVICE_HEALTH_PENDING = 0,
    SERVICE_HEALTH_READY = 1,
    SERVICE_HEALTH_LIVENESS_FAILED = 2,
    SERVICE_HEALTH_READINESS_TIMEOUT = 3,
};

enum service_supervisor_state {
    SERVICE_SUPERVISOR_STATE_STOPPED = 0,
    SERVICE_SUPERVISOR_STATE_RUNNING = 1,
    SERVICE_SUPERVISOR_STATE_EXITED = 2,
    SERVICE_SUPERVISOR_STATE_EXHAUSTED = 3,
};

enum service_supervisor_status {
    SERVICE_SUPERVISOR_OK = 0,
    SERVICE_SUPERVISOR_NULL = 1,
    SERVICE_SUPERVISOR_BAD_STATE = 2,
    SERVICE_SUPERVISOR_FULL = 3,
    SERVICE_SUPERVISOR_NOT_FOUND = 4,
    SERVICE_SUPERVISOR_DISABLED = 5,
    SERVICE_SUPERVISOR_ALREADY_STARTED = 6,
    SERVICE_SUPERVISOR_NOT_EXITED = 7,
    SERVICE_SUPERVISOR_EXHAUSTED = 8,
    SERVICE_SUPERVISOR_PROCESS_FAILED = 9,
    SERVICE_SUPERVISOR_NOT_BSP = 10,
    SERVICE_SUPERVISOR_INVALID_OWNED_STATE = 11,
    SERVICE_SUPERVISOR_NOT_READY = 12,
    SERVICE_SUPERVISOR_ALREADY_READY = 13,
    SERVICE_SUPERVISOR_LIVENESS_FAILED = 14,
    SERVICE_SUPERVISOR_BAD_LIVENESS = 15,
    SERVICE_SUPERVISOR_DEPENDENCY_BLOCKED = 16,
    SERVICE_SUPERVISOR_RESTART_INTERVAL = 17,
    SERVICE_SUPERVISOR_RESTART_DISABLED = 18,
    SERVICE_SUPERVISOR_RESTART_TIME_OVERFLOW = 19,
    SERVICE_SUPERVISOR_READINESS_TIMEOUT = 20,
    SERVICE_SUPERVISOR_BAD_READINESS = 21,
    SERVICE_SUPERVISOR_READINESS_OVERFLOW = 22,
    SERVICE_SUPERVISOR_SNAPSHOT_INVALID = 23,
};

struct service_supervisor_service {
    struct service_descriptor descriptor;
    struct service_restart_policy restart_policy;
    enum service_supervisor_state state;
    enum service_health_state health_state;
    uint32_t restart_count;
    uint32_t exit_status;
    uint64_t ready_tick;
    uint64_t last_liveness_tick;
    uint64_t liveness_generation;
    uint64_t readiness_start_tick;
    uint64_t readiness_deadline_tick;
    uint64_t readiness_timeout_ticks;
    void (*entry)(void);
    struct kernel_process *process;
};

struct service_supervisor_snapshot_entry {
    struct service_descriptor descriptor;
    enum service_supervisor_state state;
    enum service_health_state health_state;
    uint32_t restart_count;
    uint32_t exit_status;
    uint64_t ready_tick;
    uint64_t last_liveness_tick;
    uint64_t liveness_generation;
    uint64_t readiness_start_tick;
    uint64_t readiness_deadline_tick;
    uint64_t readiness_timeout_ticks;
};

struct service_supervisor_snapshot {
    bool published;
    uint32_t version;
    uint64_t generation;
    uint32_t active_count;
    uint32_t entry_count;
    struct service_supervisor_snapshot_entry
        entries[SERVICE_SUPERVISOR_CAPACITY];
};

struct service_supervisor {
    bool initialized;
    uint32_t active_count;
    uint64_t hhdm_offset;
    uint64_t mutation_generation;
    struct process_control_block supervisor_pcb;
    struct service_supervisor_service services[SERVICE_SUPERVISOR_CAPACITY];
};

struct service_supervisor_entry {
    uint32_t service_id;
    void (*entry)(void);
};

enum service_supervisor_status service_supervisor_init(
    struct service_supervisor *supervisor,
    uint64_t hhdm_offset);
enum service_supervisor_status service_supervisor_start(
    struct service_supervisor *supervisor,
    const struct service_descriptor *descriptor,
    void (*entry)(void));
enum service_supervisor_status service_supervisor_start_ordered(
    struct service_supervisor *supervisor,
    const struct service_graph_snapshot *snapshot,
    const struct service_supervisor_entry *entries,
    uint32_t entry_count);
enum service_supervisor_status service_supervisor_stop_ordered(
    struct service_supervisor *supervisor,
    const struct service_graph_snapshot *snapshot);
enum service_supervisor_status service_supervisor_configure_restart_policy(
    struct service_supervisor *supervisor,
    uint32_t service_id,
    uint64_t restart_interval_ticks,
    bool disabled_after_exhaustion);
enum service_supervisor_status service_supervisor_reap_contained(
    struct service_supervisor *supervisor,
    uint32_t service_id,
    uint64_t now_ticks,
    const struct service_graph_snapshot *snapshot,
    uint32_t *exit_status_out,
    bool *restarted);
enum service_supervisor_status service_supervisor_restart_contained(
    struct service_supervisor *supervisor,
    uint32_t service_id,
    uint64_t now_ticks,
    const struct service_graph_snapshot *snapshot,
    bool *restarted);
enum service_supervisor_status service_supervisor_exit(
    struct service_supervisor *supervisor,
    uint32_t service_id,
    uint32_t exit_status);
enum service_supervisor_status service_supervisor_reap(
    struct service_supervisor *supervisor,
    uint32_t service_id,
    uint32_t *exit_status_out);
enum service_supervisor_status service_supervisor_reap_at(
    struct service_supervisor *supervisor,
    uint32_t service_id,
    uint64_t now_ticks,
    uint32_t *exit_status_out);
uint32_t service_supervisor_restart_count(
    const struct service_supervisor *supervisor,
    uint32_t service_id);
enum service_supervisor_state service_supervisor_state(
    const struct service_supervisor *supervisor,
    uint32_t service_id);
enum service_supervisor_status service_supervisor_report_ready(
    struct service_supervisor *supervisor,
    uint32_t service_id,
    uint64_t now_ticks,
    bool *published);
enum service_supervisor_status service_supervisor_report_liveness(
    struct service_supervisor *supervisor,
    uint32_t service_id,
    uint64_t now_ticks);
enum service_supervisor_status service_supervisor_check_liveness(
    struct service_supervisor *supervisor,
    uint32_t service_id,
    uint64_t now_ticks,
    uint64_t liveness_timeout_ticks,
    bool *failed);
enum service_supervisor_status service_supervisor_set_readiness_deadline(
    struct service_supervisor *supervisor,
    uint32_t service_id,
    uint64_t start_tick,
    uint64_t readiness_timeout_ticks);
enum service_supervisor_status service_supervisor_check_readiness(
    struct service_supervisor *supervisor,
    uint32_t service_id,
    uint64_t now_ticks,
    bool *timed_out);
enum service_health_state service_supervisor_health_state(
    const struct service_supervisor *supervisor,
    uint32_t service_id);
uint64_t service_supervisor_liveness_generation(
    const struct service_supervisor *supervisor,
    uint32_t service_id);
uint64_t service_supervisor_readiness_deadline_tick(
    const struct service_supervisor *supervisor,
    uint32_t service_id);
enum service_supervisor_status service_supervisor_snapshot(
    const struct service_supervisor *supervisor,
    struct service_supervisor_snapshot *snapshot);
bool service_supervisor_snapshot_published(
    const struct service_supervisor_snapshot *snapshot);
uint32_t service_supervisor_snapshot_count(
    const struct service_supervisor_snapshot *snapshot);
const struct service_supervisor_snapshot_entry *
service_supervisor_snapshot_entry(
    const struct service_supervisor_snapshot *snapshot,
    uint32_t position);
const char *service_supervisor_status_string(
    enum service_supervisor_status status);
const char *service_supervisor_state_string(
    enum service_supervisor_state state);
const char *service_supervisor_health_state_string(
    enum service_health_state state);

#ifdef HFOC_SERVICE_SUPERVISOR_QEMU_TEST
void service_supervisor_qemu_test_init(uint64_t hhdm_offset);
void service_supervisor_qemu_poll(void);
#endif

#ifdef HFOC_SERVICE_SUPERVISOR_HOST_TEST
void service_supervisor_host_test_reset(void);
uint32_t service_supervisor_host_test_stop_count(void);
const uint32_t *service_supervisor_host_test_stop_ids(void);
void service_supervisor_host_test_set_snapshot_mutate(bool enabled);
#endif

#endif
