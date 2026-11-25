#include "service_supervisor.h"

#include "cpu.h"
#include "process.h"
#include "service.h"
#include "service_restart_policy.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#ifdef HFOC_SERVICE_SUPERVISOR_HOST_TEST
static uint32_t service_supervisor_host_stop_count;
static uint32_t service_supervisor_host_stop_ids[
    SERVICE_SUPERVISOR_CAPACITY];
static bool service_supervisor_host_snapshot_mutate;

void service_supervisor_host_test_reset(void) {
    service_supervisor_host_stop_count = 0U;
    service_supervisor_host_snapshot_mutate = false;
    memset(service_supervisor_host_stop_ids, 0,
           sizeof(service_supervisor_host_stop_ids));
}

uint32_t service_supervisor_host_test_stop_count(void) {
    return service_supervisor_host_stop_count;
}

const uint32_t *service_supervisor_host_test_stop_ids(void) {
    return service_supervisor_host_stop_ids;
}

void service_supervisor_host_test_set_snapshot_mutate(bool enabled) {
    service_supervisor_host_snapshot_mutate = enabled;
}
#endif

static bool service_supervisor_is_bsp(void) {
    const struct cpu *const cpu = cpu_current();

    return cpu != NULL && cpu->index == 0U &&
           (cpu->state == CPU_STATE_BSP ||
            cpu->state == CPU_STATE_ONLINE);
}

static struct service_supervisor_service *service_supervisor_find(
    struct service_supervisor *supervisor,
    uint32_t service_id)
{
    uint32_t index;

    if (supervisor == NULL) {
        return NULL;
    }
    for (index = 0U; index < SERVICE_SUPERVISOR_CAPACITY; ++index) {
        struct service_supervisor_service *const service =
            &supervisor->services[index];

        if (service->state != SERVICE_SUPERVISOR_STATE_STOPPED &&
            service->descriptor.id == service_id) {
            return service;
        }
    }
    return NULL;
}

static const struct service_supervisor_service *service_supervisor_find_const(
    const struct service_supervisor *supervisor,
    uint32_t service_id)
{
    uint32_t index;

    if (supervisor == NULL) {
        return NULL;
    }
    for (index = 0U; index < SERVICE_SUPERVISOR_CAPACITY; ++index) {
        const struct service_supervisor_service *const service =
            &supervisor->services[index];

        if (service->state != SERVICE_SUPERVISOR_STATE_STOPPED &&
            service->descriptor.id == service_id) {
            return service;
        }
    }
    return NULL;
}

static bool service_supervisor_owned_state_valid(
    const struct service_supervisor_service *service)
{
    const struct kernel_process *process;

    if (service == NULL ||
        service->state != SERVICE_SUPERVISOR_STATE_RUNNING ||
        service->process == NULL ||
        service->entry == NULL) {
        return false;
    }
    process = service->process;
    return process->pcb.published &&
           process->lifecycle == PROCESS_LIFECYCLE_RUNNING &&
           process->address_space_owned &&
           process->stack_owned &&
           process->descriptor_table.published &&
           process->pcb.capability_table.published &&
           process_limit_cpu_ticks(process) ==
               service->descriptor.cpu_ticks_limit &&
           process_limit_user_memory_limit_pages(process) ==
               service->descriptor.user_memory_limit_pages &&
           service_descriptor_validate(&service->descriptor) ==
               SERVICE_DESCRIPTOR_OK;
}

static void service_supervisor_health_reset(
    struct service_supervisor_service *service)
{
    service->health_state = SERVICE_HEALTH_PENDING;
    service->ready_tick = 0U;
    service->last_liveness_tick = 0U;
    service->liveness_generation = 0U;
    service->readiness_start_tick = 0U;
    service->readiness_deadline_tick = 0U;
}

static void service_supervisor_touch(
    struct service_supervisor *supervisor)
{
    if (supervisor != NULL &&
        supervisor->mutation_generation < UINT64_MAX) {
        ++supervisor->mutation_generation;
    }
}

static void service_supervisor_readiness_timeout_clear(
    struct service_supervisor_service *service)
{
    service->readiness_start_tick = 0U;
    service->readiness_deadline_tick = 0U;
    service->readiness_timeout_ticks = 0U;
}

static bool service_supervisor_sync_exited(
    struct service_supervisor_service *service)
{
    if (service == NULL ||
        service->state != SERVICE_SUPERVISOR_STATE_RUNNING ||
        service->process == NULL ||
        service->process->lifecycle != PROCESS_LIFECYCLE_EXITED) {
        return false;
    }
    service->exit_status = service->process->exit_status;
    service->state = SERVICE_SUPERVISOR_STATE_EXITED;
    service_supervisor_health_reset(service);
    return true;
}

static enum service_supervisor_status service_supervisor_start_slot(
    struct service_supervisor *supervisor,
    struct service_supervisor_service *slot);

static enum service_supervisor_status service_supervisor_start_slot_at(
    struct service_supervisor *supervisor,
    struct service_supervisor_service *slot,
    uint64_t now_ticks)
{
    const uint64_t timeout_ticks = slot->readiness_timeout_ticks;
    enum service_supervisor_status start_status;

    start_status = service_supervisor_start_slot(supervisor, slot);
    if (start_status != SERVICE_SUPERVISOR_OK) {
        return start_status;
    }
    if (timeout_ticks == 0U) {
        return SERVICE_SUPERVISOR_OK;
    }
    if (now_ticks > UINT64_MAX - timeout_ticks) {
        (void)process_destroy_kernel(
            slot->process, supervisor->hhdm_offset);
        slot->process = NULL;
        slot->state = SERVICE_SUPERVISOR_STATE_STOPPED;
        return SERVICE_SUPERVISOR_READINESS_OVERFLOW;
    }
    slot->readiness_start_tick = now_ticks;
    slot->readiness_deadline_tick = now_ticks + timeout_ticks;
    slot->readiness_timeout_ticks = timeout_ticks;
    return SERVICE_SUPERVISOR_OK;
}

static const struct service_descriptor *service_supervisor_snapshot_descriptor(
    const struct service_graph_snapshot *snapshot,
    uint32_t service_id)
{
    uint32_t index;

    if (snapshot == NULL) {
        return NULL;
    }
    for (index = 0U;
         index < service_graph_snapshot_count(snapshot);
         ++index) {
        const struct service_descriptor *const descriptor =
            service_graph_snapshot_descriptor(snapshot, index);

        if (descriptor->id == service_id) {
            return descriptor;
        }
    }
    return NULL;
}

static bool service_supervisor_dependency_available(
    const struct service_supervisor *supervisor,
    const struct service_graph_snapshot *snapshot,
    const struct service_descriptor *descriptor)
{
    uint32_t dependency_position;

    if (descriptor->dependency_count == 0U) {
        return true;
    }
    if (snapshot == NULL ||
        !service_graph_snapshot_published(snapshot)) {
        return false;
    }
    for (dependency_position = 0U;
         dependency_position < descriptor->dependency_count;
         ++dependency_position) {
        const uint32_t dependency_id =
            descriptor->dependencies[dependency_position];
        const struct service_descriptor *const dependency =
            service_supervisor_snapshot_descriptor(
                snapshot, dependency_id);
        const struct service_supervisor_service *const service =
            service_supervisor_find_const(supervisor, dependency_id);

        if (dependency == NULL || !dependency->enabled ||
            service == NULL ||
            !service_supervisor_owned_state_valid(service) ||
            memcmp(&service->descriptor, dependency,
                   sizeof(*dependency)) != 0) {
            return false;
        }
    }
    return true;
}

static enum service_supervisor_status service_supervisor_map_policy_status(
    enum service_restart_policy_status status)
{
    switch (status) {
    case SERVICE_RESTART_POLICY_INTERVAL:
        return SERVICE_SUPERVISOR_RESTART_INTERVAL;
    case SERVICE_RESTART_POLICY_EXHAUSTED:
        return SERVICE_SUPERVISOR_EXHAUSTED;
    case SERVICE_RESTART_POLICY_DISABLED:
        return SERVICE_SUPERVISOR_RESTART_DISABLED;
    case SERVICE_RESTART_POLICY_TIME_OVERFLOW:
        return SERVICE_SUPERVISOR_RESTART_TIME_OVERFLOW;
    default:
        return SERVICE_SUPERVISOR_BAD_STATE;
    }
}

static enum service_supervisor_status service_supervisor_contained_restart(
    struct service_supervisor *supervisor,
    struct service_supervisor_service *service,
    uint64_t now_ticks,
    const struct service_graph_snapshot *snapshot,
    bool *restarted)
{
    enum service_restart_policy_status policy_status;
    enum service_supervisor_status start_status;

    *restarted = false;
    if (!service_supervisor_dependency_available(
            supervisor, snapshot, &service->descriptor)) {
        return SERVICE_SUPERVISOR_DEPENDENCY_BLOCKED;
    }
    policy_status = service_restart_policy_record_restart(
        &service->restart_policy, now_ticks);
    if (policy_status != SERVICE_RESTART_POLICY_OK) {
        return service_supervisor_map_policy_status(policy_status);
    }
    service->restart_count = service_restart_policy_restart_count(
        &service->restart_policy);
    start_status = service_supervisor_start_slot_at(
        supervisor, service, now_ticks);
    if (start_status != SERVICE_SUPERVISOR_OK) {
        service->state = SERVICE_SUPERVISOR_STATE_EXHAUSTED;
        service_supervisor_touch(supervisor);
        return start_status;
    }
    *restarted = true;
    return SERVICE_SUPERVISOR_OK;
}

static enum service_supervisor_status service_supervisor_start_slot(
    struct service_supervisor *supervisor,
    struct service_supervisor_service *slot)
{
    struct kernel_process *process = NULL;
    enum process_status limit_status;

    service_supervisor_health_reset(slot);
    if (process_create_kernel(&process, &supervisor->supervisor_pcb,
                              slot->entry, supervisor->hhdm_offset) !=
        PROCESS_OK) {
        return SERVICE_SUPERVISOR_PROCESS_FAILED;
    }
    limit_status = process_limit_set_cpu_ticks(
        process, slot->descriptor.cpu_ticks_limit);
    if (limit_status == PROCESS_OK) {
        limit_status = process_limit_set_user_memory_pages(
            process, slot->descriptor.user_memory_limit_pages);
    }
    if (limit_status != PROCESS_OK) {
        (void)process_destroy_kernel(process, supervisor->hhdm_offset);
        return SERVICE_SUPERVISOR_PROCESS_FAILED;
    }
    slot->process = process;
    slot->state = SERVICE_SUPERVISOR_STATE_RUNNING;
    slot->exit_status = 0U;
    service_supervisor_touch(supervisor);
    return SERVICE_SUPERVISOR_OK;
}

enum service_supervisor_status service_supervisor_init(
    struct service_supervisor *supervisor,
    uint64_t hhdm_offset)
{
    uint32_t pid = 0U;

    if (supervisor == NULL) {
        return SERVICE_SUPERVISOR_NULL;
    }
    if (supervisor->initialized) {
        return SERVICE_SUPERVISOR_BAD_STATE;
    }
    if (!service_supervisor_is_bsp()) {
        return SERVICE_SUPERVISOR_NOT_BSP;
    }

    memset(supervisor, 0, sizeof(*supervisor));
    supervisor->hhdm_offset = hhdm_offset;
    if (process_pcb_init(&supervisor->supervisor_pcb, NULL, &pid) !=
        PROCESS_OK) {
        return SERVICE_SUPERVISOR_BAD_STATE;
    }
    supervisor->initialized = true;
    service_supervisor_touch(supervisor);
    return SERVICE_SUPERVISOR_OK;
}

enum service_supervisor_status service_supervisor_start(
    struct service_supervisor *supervisor,
    const struct service_descriptor *descriptor,
    void (*entry)(void))
{
    struct service_supervisor_service *service;
    struct service_supervisor_service *free_slot = NULL;
    uint32_t index;
    enum service_supervisor_status start_status;

    if (supervisor == NULL || descriptor == NULL || entry == NULL) {
        return SERVICE_SUPERVISOR_NULL;
    }
    if (!supervisor->initialized) {
        return SERVICE_SUPERVISOR_BAD_STATE;
    }
    if (service_descriptor_validate(descriptor) != SERVICE_DESCRIPTOR_OK) {
        return SERVICE_SUPERVISOR_BAD_STATE;
    }
    if (!descriptor->enabled) {
        return SERVICE_SUPERVISOR_DISABLED;
    }

    service = service_supervisor_find(supervisor, descriptor->id);
    if (service != NULL) {
        return service->state == SERVICE_SUPERVISOR_STATE_EXHAUSTED ?
            SERVICE_SUPERVISOR_EXHAUSTED :
            SERVICE_SUPERVISOR_ALREADY_STARTED;
    }

    for (index = 0U; index < SERVICE_SUPERVISOR_CAPACITY; ++index) {
        struct service_supervisor_service *const candidate =
            &supervisor->services[index];

        if (candidate->state == SERVICE_SUPERVISOR_STATE_STOPPED &&
            candidate->process == NULL) {
            free_slot = candidate;
            break;
        }
    }
    if (free_slot == NULL) {
        return SERVICE_SUPERVISOR_FULL;
    }

    memset(free_slot, 0, sizeof(*free_slot));
    (void)service_descriptor_init(&free_slot->descriptor, descriptor);
    free_slot->entry = entry;
    if (service_restart_policy_init(
            &free_slot->restart_policy,
            descriptor->restart_limit,
            SERVICE_SUPERVISOR_RESTART_INTERVAL_DEFAULT_TICKS,
            false) != SERVICE_RESTART_POLICY_OK) {
        memset(free_slot, 0, sizeof(*free_slot));
        return SERVICE_SUPERVISOR_BAD_STATE;
    }
    start_status = service_supervisor_start_slot(supervisor, free_slot);
    if (start_status != SERVICE_SUPERVISOR_OK) {
        memset(free_slot, 0, sizeof(*free_slot));
        return start_status;
    }
    ++supervisor->active_count;
    return SERVICE_SUPERVISOR_OK;
}

static const struct service_supervisor_entry *service_supervisor_entry_find(
    const struct service_supervisor_entry *entries,
    uint32_t entry_count,
    uint32_t service_id)
{
    uint32_t index;

    for (index = 0U; index < entry_count; ++index) {
        if (entries[index].service_id == service_id) {
            return &entries[index];
        }
    }
    return NULL;
}

static bool service_supervisor_ordered_entries_valid(
    const struct service_graph_snapshot *snapshot,
    const struct service_supervisor_entry *entries,
    uint32_t entry_count)
{
    uint32_t enabled_count = 0U;
    uint32_t index;
    uint32_t previous;
    uint32_t descriptor_index;

    if (entry_count > SERVICE_SUPERVISOR_ENTRY_MAX) {
        return false;
    }
    if (entry_count > 0U && entries == NULL) {
        return false;
    }
    for (index = 0U; index < entry_count; ++index) {
        if (entries[index].service_id < SERVICE_ID_MIN ||
            entries[index].service_id > SERVICE_ID_MAX ||
            entries[index].entry == NULL) {
            return false;
        }
        for (previous = 0U; previous < index; ++previous) {
            if (entries[index].service_id == entries[previous].service_id) {
                return false;
            }
        }
    }

    for (descriptor_index = 0U;
         descriptor_index < service_graph_snapshot_count(snapshot);
         ++descriptor_index) {
        const struct service_descriptor *const descriptor =
            service_graph_snapshot_descriptor(snapshot, descriptor_index);

        if (descriptor->enabled) {
            ++enabled_count;
            if (service_supervisor_entry_find(
                    entries, entry_count, descriptor->id) == NULL) {
                return false;
            }
        }
    }
    if (enabled_count != entry_count) {
        return false;
    }
    for (index = 0U; index < entry_count; ++index) {
        bool found = false;

        for (descriptor_index = 0U;
             descriptor_index < service_graph_snapshot_count(snapshot);
             ++descriptor_index) {
            const struct service_descriptor *const descriptor =
                service_graph_snapshot_descriptor(snapshot, descriptor_index);

            if (descriptor->id == entries[index].service_id &&
                descriptor->enabled) {
                found = true;
                break;
            }
        }
        if (!found) {
            return false;
        }
    }
    return true;
}

enum service_supervisor_status service_supervisor_start_ordered(
    struct service_supervisor *supervisor,
    const struct service_graph_snapshot *snapshot,
    const struct service_supervisor_entry *entries,
    uint32_t entry_count)
{
    bool available[SERVICE_ID_MAX + 1U];
    uint32_t index;
    uint32_t descriptor_index;

    if (supervisor == NULL || snapshot == NULL) {
        return SERVICE_SUPERVISOR_NULL;
    }
    if (!supervisor->initialized) {
        return SERVICE_SUPERVISOR_BAD_STATE;
    }
    if (!service_graph_snapshot_published(snapshot)) {
        return SERVICE_SUPERVISOR_BAD_STATE;
    }
    if (entry_count > SERVICE_SUPERVISOR_ENTRY_MAX) {
        return SERVICE_SUPERVISOR_BAD_STATE;
    }
    if (entry_count > 0U && entries == NULL) {
        return SERVICE_SUPERVISOR_NULL;
    }
    for (index = 0U; index < entry_count; ++index) {
        if (entries[index].entry == NULL) {
            return SERVICE_SUPERVISOR_NULL;
        }
    }
    if (!service_supervisor_ordered_entries_valid(
            snapshot, entries, entry_count)) {
        return SERVICE_SUPERVISOR_BAD_STATE;
    }

    for (index = 0U; index <= SERVICE_ID_MAX; ++index) {
        available[index] = false;
    }
    for (descriptor_index = 0U;
         descriptor_index < service_graph_snapshot_count(snapshot);
         ++descriptor_index) {
        const struct service_descriptor *const descriptor =
            service_graph_snapshot_descriptor(snapshot, descriptor_index);
        struct service_supervisor_service *const service =
            service_supervisor_find(supervisor, descriptor->id);

        if (service == NULL) {
            continue;
        }
        if (service->state == SERVICE_SUPERVISOR_STATE_RUNNING) {
            if (memcmp(&service->descriptor, descriptor,
                       sizeof(*descriptor)) != 0) {
                return SERVICE_SUPERVISOR_BAD_STATE;
            }
            available[descriptor->id] = true;
        } else if (service->state == SERVICE_SUPERVISOR_STATE_EXHAUSTED) {
            return SERVICE_SUPERVISOR_EXHAUSTED;
        } else {
            return SERVICE_SUPERVISOR_BAD_STATE;
        }
    }

    for (index = 0U;
         index < service_graph_snapshot_count(snapshot);
         ++index) {
        const uint32_t service_id =
            service_graph_snapshot_start_id(snapshot, index);
        const struct service_descriptor *descriptor = NULL;
        const struct service_supervisor_entry *entry;
        uint32_t dependency_position;
        enum service_supervisor_status status;

        for (descriptor_index = 0U;
             descriptor_index < service_graph_snapshot_count(snapshot);
             ++descriptor_index) {
            const struct service_descriptor *const candidate =
                service_graph_snapshot_descriptor(snapshot, descriptor_index);

            if (candidate->id == service_id) {
                descriptor = candidate;
                break;
            }
        }
        if (descriptor == NULL || !descriptor->enabled) {
            continue;
        }
        for (dependency_position = 0U;
             dependency_position < descriptor->dependency_count;
             ++dependency_position) {
            if (!available[descriptor->dependencies[dependency_position]]) {
                return SERVICE_SUPERVISOR_DISABLED;
            }
        }
        if (available[service_id]) {
            continue;
        }

        entry = service_supervisor_entry_find(entries, entry_count, service_id);
        if (entry == NULL) {
            return SERVICE_SUPERVISOR_BAD_STATE;
        }
        status = service_supervisor_start(
            supervisor, descriptor, entry->entry);
        if (status != SERVICE_SUPERVISOR_OK &&
            status != SERVICE_SUPERVISOR_ALREADY_STARTED) {
            return status;
        }
        available[service_id] = true;
    }
    return SERVICE_SUPERVISOR_OK;
}

static enum service_supervisor_status service_supervisor_stop_slot(
    struct service_supervisor *supervisor,
    struct service_supervisor_service *service)
{
    enum service_supervisor_status exit_status;
    enum process_status reap_status;
    uint32_t shutdown_status = 0U;

    if (supervisor == NULL || service == NULL ||
        supervisor->active_count == 0U ||
        !service_supervisor_owned_state_valid(service)) {
        return SERVICE_SUPERVISOR_INVALID_OWNED_STATE;
    }
    exit_status = service_supervisor_exit(
        supervisor, service->descriptor.id, 0U);
    if (exit_status != SERVICE_SUPERVISOR_OK) {
        return exit_status;
    }
    reap_status = process_reap_kernel(
        service->process, &shutdown_status, supervisor->hhdm_offset);
    if (reap_status != PROCESS_OK) {
        return SERVICE_SUPERVISOR_PROCESS_FAILED;
    }
    service_supervisor_health_reset(service);
    service_supervisor_readiness_timeout_clear(service);
    service->process = NULL;
    service->exit_status = shutdown_status;
    service->state = SERVICE_SUPERVISOR_STATE_STOPPED;
    --supervisor->active_count;
    service_supervisor_touch(supervisor);
#ifdef HFOC_SERVICE_SUPERVISOR_HOST_TEST
    if (service_supervisor_host_stop_count <
        SERVICE_SUPERVISOR_CAPACITY) {
        service_supervisor_host_stop_ids[
            service_supervisor_host_stop_count] = service->descriptor.id;
        ++service_supervisor_host_stop_count;
    }
#endif
    return SERVICE_SUPERVISOR_OK;
}

enum service_supervisor_status service_supervisor_stop_ordered(
    struct service_supervisor *supervisor,
    const struct service_graph_snapshot *snapshot)
{
    uint32_t index;
    uint32_t descriptor_index;

    if (supervisor == NULL || snapshot == NULL) {
        return SERVICE_SUPERVISOR_NULL;
    }
    if (!supervisor->initialized) {
        return SERVICE_SUPERVISOR_BAD_STATE;
    }
    if (!service_graph_snapshot_published(snapshot)) {
        return SERVICE_SUPERVISOR_BAD_STATE;
    }

    for (index = service_graph_snapshot_count(snapshot);
         index > 0U;
         --index) {
        const uint32_t service_id =
            service_graph_snapshot_start_id(snapshot, index - 1U);
        const struct service_descriptor *descriptor = NULL;
        struct service_supervisor_service *service;
        enum service_supervisor_status status;

        for (descriptor_index = 0U;
             descriptor_index < service_graph_snapshot_count(snapshot);
             ++descriptor_index) {
            const struct service_descriptor *const candidate =
                service_graph_snapshot_descriptor(snapshot, descriptor_index);

            if (candidate->id == service_id) {
                descriptor = candidate;
                break;
            }
        }
        if (descriptor == NULL || !descriptor->enabled) {
            continue;
        }
        service = service_supervisor_find(supervisor, service_id);
        if (service == NULL ||
            service->state != SERVICE_SUPERVISOR_STATE_RUNNING) {
            continue;
        }
        if (memcmp(&service->descriptor, descriptor,
                   sizeof(*descriptor)) != 0) {
            return SERVICE_SUPERVISOR_BAD_STATE;
        }
        status = service_supervisor_stop_slot(supervisor, service);
        if (status != SERVICE_SUPERVISOR_OK) {
            return status;
        }
    }
    return SERVICE_SUPERVISOR_OK;
}

enum service_supervisor_status service_supervisor_configure_restart_policy(
    struct service_supervisor *supervisor,
    uint32_t service_id,
    uint64_t restart_interval_ticks,
    bool disabled_after_exhaustion)
{
    struct service_supervisor_service *service;
    enum service_restart_policy_status policy_status;

    if (supervisor == NULL) {
        return SERVICE_SUPERVISOR_NULL;
    }
    if (!supervisor->initialized) {
        return SERVICE_SUPERVISOR_BAD_STATE;
    }
    service = service_supervisor_find(supervisor, service_id);
    if (service == NULL) {
        return SERVICE_SUPERVISOR_NOT_FOUND;
    }
    if (service->state != SERVICE_SUPERVISOR_STATE_RUNNING &&
        service->state != SERVICE_SUPERVISOR_STATE_EXITED) {
        return SERVICE_SUPERVISOR_BAD_STATE;
    }
    policy_status = service_restart_policy_init(
        &service->restart_policy,
        service->descriptor.restart_limit,
        restart_interval_ticks,
        disabled_after_exhaustion);
    if (policy_status != SERVICE_RESTART_POLICY_OK) {
        return SERVICE_SUPERVISOR_BAD_STATE;
    }
    service->restart_count = 0U;
    service_supervisor_touch(supervisor);
    return SERVICE_SUPERVISOR_OK;
}

enum service_supervisor_status service_supervisor_reap_contained(
    struct service_supervisor *supervisor,
    uint32_t service_id,
    uint64_t now_ticks,
    const struct service_graph_snapshot *snapshot,
    uint32_t *exit_status_out,
    bool *restarted)
{
    struct service_supervisor_service *service;
    uint32_t exit_status = 0U;
    enum process_status reap_status;

    if (supervisor == NULL || exit_status_out == NULL ||
        restarted == NULL) {
        return SERVICE_SUPERVISOR_NULL;
    }
    *restarted = false;
    if (!supervisor->initialized) {
        return SERVICE_SUPERVISOR_BAD_STATE;
    }
    service = service_supervisor_find(supervisor, service_id);
    if (service == NULL) {
        return SERVICE_SUPERVISOR_NOT_FOUND;
    }
    if (service->state == SERVICE_SUPERVISOR_STATE_RUNNING) {
        (void)service_supervisor_sync_exited(service);
    }
    if (service->state != SERVICE_SUPERVISOR_STATE_EXITED ||
        service->process == NULL) {
        return SERVICE_SUPERVISOR_NOT_EXITED;
    }
    if (service->descriptor.dependency_count > 0U &&
        (snapshot == NULL ||
         !service_graph_snapshot_published(snapshot))) {
        return SERVICE_SUPERVISOR_BAD_STATE;
    }

    reap_status = process_reap_kernel(
        service->process, &exit_status, supervisor->hhdm_offset);
    if (reap_status != PROCESS_OK) {
        return SERVICE_SUPERVISOR_PROCESS_FAILED;
    }
    service_supervisor_health_reset(service);
    service->process = NULL;
    service->exit_status = exit_status;
    *exit_status_out = exit_status;
    service->state = SERVICE_SUPERVISOR_STATE_EXHAUSTED;
    service_supervisor_touch(supervisor);
    return service_supervisor_contained_restart(
        supervisor, service, now_ticks, snapshot, restarted);
}

enum service_supervisor_status service_supervisor_restart_contained(
    struct service_supervisor *supervisor,
    uint32_t service_id,
    uint64_t now_ticks,
    const struct service_graph_snapshot *snapshot,
    bool *restarted)
{
    struct service_supervisor_service *service;

    if (supervisor == NULL || restarted == NULL) {
        return SERVICE_SUPERVISOR_NULL;
    }
    *restarted = false;
    if (!supervisor->initialized) {
        return SERVICE_SUPERVISOR_BAD_STATE;
    }
    service = service_supervisor_find(supervisor, service_id);
    if (service == NULL) {
        return SERVICE_SUPERVISOR_NOT_FOUND;
    }
    if (service->state != SERVICE_SUPERVISOR_STATE_EXHAUSTED ||
        service->process != NULL) {
        return SERVICE_SUPERVISOR_BAD_STATE;
    }
    if (service->descriptor.dependency_count > 0U &&
        (snapshot == NULL ||
         !service_graph_snapshot_published(snapshot))) {
        return SERVICE_SUPERVISOR_BAD_STATE;
    }
    return service_supervisor_contained_restart(
        supervisor, service, now_ticks, snapshot, restarted);
}

enum service_supervisor_status service_supervisor_exit(
    struct service_supervisor *supervisor,
    uint32_t service_id,
    uint32_t exit_status)
{
    struct service_supervisor_service *service;

    if (supervisor == NULL) {
        return SERVICE_SUPERVISOR_NULL;
    }
    if (!supervisor->initialized) {
        return SERVICE_SUPERVISOR_BAD_STATE;
    }
    service = service_supervisor_find(supervisor, service_id);
    if (service == NULL) {
        return SERVICE_SUPERVISOR_NOT_FOUND;
    }
    if (service->state != SERVICE_SUPERVISOR_STATE_RUNNING ||
        service->process == NULL) {
        return SERVICE_SUPERVISOR_BAD_STATE;
    }
    if (process_exit_kernel(service->process, exit_status) != PROCESS_OK) {
        return SERVICE_SUPERVISOR_PROCESS_FAILED;
    }
    service->exit_status = exit_status;
    service->state = SERVICE_SUPERVISOR_STATE_EXITED;
    service_supervisor_health_reset(service);
    service_supervisor_touch(supervisor);
    return SERVICE_SUPERVISOR_OK;
}

enum service_supervisor_status service_supervisor_reap(
    struct service_supervisor *supervisor,
    uint32_t service_id,
    uint32_t *exit_status_out)
{
    struct service_supervisor_service *service;
    uint32_t exit_status = 0U;
    enum service_supervisor_status start_status;

    if (supervisor == NULL || exit_status_out == NULL) {
        return SERVICE_SUPERVISOR_NULL;
    }
    if (!supervisor->initialized) {
        return SERVICE_SUPERVISOR_BAD_STATE;
    }
    service = service_supervisor_find(supervisor, service_id);
    if (service == NULL) {
        return SERVICE_SUPERVISOR_NOT_FOUND;
    }
    if (service->state == SERVICE_SUPERVISOR_STATE_RUNNING) {
        (void)service_supervisor_sync_exited(service);
    }
    if (service->state != SERVICE_SUPERVISOR_STATE_EXITED ||
        service->process == NULL) {
        return SERVICE_SUPERVISOR_NOT_EXITED;
    }
    if (process_reap_kernel(service->process, &exit_status,
                            supervisor->hhdm_offset) != PROCESS_OK) {
        return SERVICE_SUPERVISOR_PROCESS_FAILED;
    }
    service_supervisor_health_reset(service);
    service->process = NULL;
    service->exit_status = exit_status;
    *exit_status_out = exit_status;

    if (service->descriptor.enabled &&
        service->restart_count < service->descriptor.restart_limit) {
        ++service->restart_count;
        service_supervisor_readiness_timeout_clear(service);
        start_status = service_supervisor_start_slot(supervisor, service);
        if (start_status != SERVICE_SUPERVISOR_OK) {
            service->state = SERVICE_SUPERVISOR_STATE_EXHAUSTED;
            service_supervisor_touch(supervisor);
            return start_status;
        }
        return SERVICE_SUPERVISOR_OK;
    }

    service_supervisor_readiness_timeout_clear(service);
    service->state = SERVICE_SUPERVISOR_STATE_EXHAUSTED;
    service_supervisor_touch(supervisor);
    return SERVICE_SUPERVISOR_OK;
}

enum service_supervisor_status service_supervisor_reap_at(
    struct service_supervisor *supervisor,
    uint32_t service_id,
    uint64_t now_ticks,
    uint32_t *exit_status_out)
{
    struct service_supervisor_service *service;
    uint32_t exit_status = 0U;
    enum service_supervisor_status start_status;

    if (supervisor == NULL || exit_status_out == NULL) {
        return SERVICE_SUPERVISOR_NULL;
    }
    if (!supervisor->initialized) {
        return SERVICE_SUPERVISOR_BAD_STATE;
    }
    service = service_supervisor_find(supervisor, service_id);
    if (service == NULL) {
        return SERVICE_SUPERVISOR_NOT_FOUND;
    }
    if (service->state == SERVICE_SUPERVISOR_STATE_RUNNING) {
        (void)service_supervisor_sync_exited(service);
    }
    if (service->state != SERVICE_SUPERVISOR_STATE_EXITED ||
        service->process == NULL) {
        return SERVICE_SUPERVISOR_NOT_EXITED;
    }
    if (process_reap_kernel(service->process, &exit_status,
                            supervisor->hhdm_offset) != PROCESS_OK) {
        return SERVICE_SUPERVISOR_PROCESS_FAILED;
    }
    service_supervisor_health_reset(service);
    service->process = NULL;
    service->exit_status = exit_status;
    *exit_status_out = exit_status;

    if (service->descriptor.enabled &&
        service->restart_count < service->descriptor.restart_limit) {
        ++service->restart_count;
        start_status = service_supervisor_start_slot_at(
            supervisor, service, now_ticks);
        if (start_status != SERVICE_SUPERVISOR_OK) {
            service_supervisor_readiness_timeout_clear(service);
            service->state = SERVICE_SUPERVISOR_STATE_EXHAUSTED;
            service_supervisor_touch(supervisor);
            return start_status;
        }
        return SERVICE_SUPERVISOR_OK;
    }

    service_supervisor_readiness_timeout_clear(service);
    service->state = SERVICE_SUPERVISOR_STATE_EXHAUSTED;
    service_supervisor_touch(supervisor);
    return SERVICE_SUPERVISOR_OK;
}

uint32_t service_supervisor_restart_count(
    const struct service_supervisor *supervisor,
    uint32_t service_id)
{
    const struct service_supervisor_service *const service =
        service_supervisor_find_const(supervisor, service_id);

    return service != NULL ? service->restart_count : 0U;
}

enum service_supervisor_state service_supervisor_state(
    const struct service_supervisor *supervisor,
    uint32_t service_id)
{
    const struct service_supervisor_service *const service =
        service_supervisor_find_const(supervisor, service_id);

    return service != NULL ? service->state :
                             SERVICE_SUPERVISOR_STATE_STOPPED;
}

enum service_supervisor_status service_supervisor_set_readiness_deadline(
    struct service_supervisor *supervisor,
    uint32_t service_id,
    uint64_t start_tick,
    uint64_t readiness_timeout_ticks)
{
    struct service_supervisor_service *service;

    if (supervisor == NULL) {
        return SERVICE_SUPERVISOR_NULL;
    }
    if (!supervisor->initialized) {
        return SERVICE_SUPERVISOR_BAD_STATE;
    }
    service = service_supervisor_find(supervisor, service_id);
    if (service == NULL) {
        return SERVICE_SUPERVISOR_NOT_FOUND;
    }
    if (!service_supervisor_owned_state_valid(service)) {
        return SERVICE_SUPERVISOR_INVALID_OWNED_STATE;
    }
    if (service->health_state != SERVICE_HEALTH_PENDING) {
        return SERVICE_SUPERVISOR_BAD_STATE;
    }
    if (readiness_timeout_ticks <
            SERVICE_READINESS_TIMEOUT_MIN_TICKS ||
        readiness_timeout_ticks >
            SERVICE_READINESS_TIMEOUT_MAX_TICKS) {
        return SERVICE_SUPERVISOR_BAD_READINESS;
    }
    if (service->readiness_timeout_ticks != 0U) {
        return SERVICE_SUPERVISOR_BAD_STATE;
    }
    if (start_tick > UINT64_MAX - readiness_timeout_ticks) {
        return SERVICE_SUPERVISOR_READINESS_OVERFLOW;
    }

    service->readiness_start_tick = start_tick;
    service->readiness_deadline_tick =
        start_tick + readiness_timeout_ticks;
    service->readiness_timeout_ticks = readiness_timeout_ticks;
    service_supervisor_touch(supervisor);
    return SERVICE_SUPERVISOR_OK;
}

enum service_supervisor_status service_supervisor_check_readiness(
    struct service_supervisor *supervisor,
    uint32_t service_id,
    uint64_t now_ticks,
    bool *timed_out)
{
    struct service_supervisor_service *service;

    if (supervisor == NULL || timed_out == NULL) {
        return SERVICE_SUPERVISOR_NULL;
    }
    *timed_out = false;
    if (!supervisor->initialized) {
        return SERVICE_SUPERVISOR_BAD_STATE;
    }
    service = service_supervisor_find(supervisor, service_id);
    if (service == NULL) {
        return SERVICE_SUPERVISOR_NOT_FOUND;
    }
    if (!service_supervisor_owned_state_valid(service)) {
        return SERVICE_SUPERVISOR_INVALID_OWNED_STATE;
    }
    if (service->health_state == SERVICE_HEALTH_READINESS_TIMEOUT) {
        *timed_out = true;
        return SERVICE_SUPERVISOR_READINESS_TIMEOUT;
    }
    if (service->health_state == SERVICE_HEALTH_READY ||
        service->health_state == SERVICE_HEALTH_LIVENESS_FAILED) {
        return SERVICE_SUPERVISOR_OK;
    }
    if (service->readiness_timeout_ticks == 0U) {
        return SERVICE_SUPERVISOR_OK;
    }
    if (now_ticks < service->readiness_start_tick) {
        return SERVICE_SUPERVISOR_BAD_READINESS;
    }
    if (now_ticks > service->readiness_deadline_tick) {
        service->health_state = SERVICE_HEALTH_READINESS_TIMEOUT;
        *timed_out = true;
        service_supervisor_touch(supervisor);
        return SERVICE_SUPERVISOR_READINESS_TIMEOUT;
    }
    return SERVICE_SUPERVISOR_OK;
}

enum service_supervisor_status service_supervisor_report_ready(
    struct service_supervisor *supervisor,
    uint32_t service_id,
    uint64_t now_ticks,
    bool *published)
{
    struct service_supervisor_service *service;

    if (supervisor == NULL || published == NULL) {
        return SERVICE_SUPERVISOR_NULL;
    }
    *published = false;
    if (!supervisor->initialized) {
        return SERVICE_SUPERVISOR_BAD_STATE;
    }
    service = service_supervisor_find(supervisor, service_id);
    if (service == NULL) {
        return SERVICE_SUPERVISOR_NOT_FOUND;
    }
    if (!service_supervisor_owned_state_valid(service)) {
        return SERVICE_SUPERVISOR_INVALID_OWNED_STATE;
    }
    if (service->health_state == SERVICE_HEALTH_READINESS_TIMEOUT) {
        return SERVICE_SUPERVISOR_READINESS_TIMEOUT;
    }
    if (service->health_state == SERVICE_HEALTH_READY) {
        return SERVICE_SUPERVISOR_ALREADY_READY;
    }
    if (service->health_state == SERVICE_HEALTH_LIVENESS_FAILED) {
        return SERVICE_SUPERVISOR_LIVENESS_FAILED;
    }
    if (service->readiness_timeout_ticks != 0U) {
        if (now_ticks < service->readiness_start_tick) {
            return SERVICE_SUPERVISOR_BAD_READINESS;
        }
        if (now_ticks > service->readiness_deadline_tick) {
            service->health_state = SERVICE_HEALTH_READINESS_TIMEOUT;
            service_supervisor_touch(supervisor);
            return SERVICE_SUPERVISOR_READINESS_TIMEOUT;
        }
    }

    service->health_state = SERVICE_HEALTH_READY;
    service->ready_tick = now_ticks;
    service->last_liveness_tick = now_ticks;
    service->liveness_generation = 1U;
    *published = true;
    service_supervisor_touch(supervisor);
    return SERVICE_SUPERVISOR_OK;
}

enum service_supervisor_status service_supervisor_report_liveness(
    struct service_supervisor *supervisor,
    uint32_t service_id,
    uint64_t now_ticks)
{
    struct service_supervisor_service *service;

    if (supervisor == NULL) {
        return SERVICE_SUPERVISOR_NULL;
    }
    if (!supervisor->initialized) {
        return SERVICE_SUPERVISOR_BAD_STATE;
    }
    service = service_supervisor_find(supervisor, service_id);
    if (service == NULL) {
        return SERVICE_SUPERVISOR_NOT_FOUND;
    }
    if (!service_supervisor_owned_state_valid(service)) {
        return SERVICE_SUPERVISOR_INVALID_OWNED_STATE;
    }
    if (service->health_state == SERVICE_HEALTH_READINESS_TIMEOUT) {
        return SERVICE_SUPERVISOR_READINESS_TIMEOUT;
    }
    if (service->health_state == SERVICE_HEALTH_PENDING) {
        return SERVICE_SUPERVISOR_NOT_READY;
    }
    if (service->health_state == SERVICE_HEALTH_LIVENESS_FAILED) {
        return SERVICE_SUPERVISOR_LIVENESS_FAILED;
    }
    if (now_ticks < service->last_liveness_tick ||
        service->liveness_generation == UINT64_MAX) {
        return SERVICE_SUPERVISOR_BAD_LIVENESS;
    }

    service->last_liveness_tick = now_ticks;
    ++service->liveness_generation;
    service_supervisor_touch(supervisor);
    return SERVICE_SUPERVISOR_OK;
}

enum service_supervisor_status service_supervisor_check_liveness(
    struct service_supervisor *supervisor,
    uint32_t service_id,
    uint64_t now_ticks,
    uint64_t liveness_timeout_ticks,
    bool *failed)
{
    struct service_supervisor_service *service;

    if (supervisor == NULL || failed == NULL) {
        return SERVICE_SUPERVISOR_NULL;
    }
    if (!supervisor->initialized) {
        return SERVICE_SUPERVISOR_BAD_STATE;
    }
    service = service_supervisor_find(supervisor, service_id);
    if (service == NULL) {
        return SERVICE_SUPERVISOR_NOT_FOUND;
    }
    if (liveness_timeout_ticks < SERVICE_LIVENESS_TIMEOUT_MIN_TICKS ||
        liveness_timeout_ticks > SERVICE_LIVENESS_TIMEOUT_MAX_TICKS) {
        return SERVICE_SUPERVISOR_BAD_LIVENESS;
    }
    if (!service_supervisor_owned_state_valid(service)) {
        return SERVICE_SUPERVISOR_INVALID_OWNED_STATE;
    }
    if (service->health_state == SERVICE_HEALTH_READINESS_TIMEOUT) {
        *failed = true;
        return SERVICE_SUPERVISOR_READINESS_TIMEOUT;
    }
    if (service->health_state == SERVICE_HEALTH_PENDING) {
        *failed = false;
        return SERVICE_SUPERVISOR_NOT_READY;
    }
    if (service->health_state == SERVICE_HEALTH_LIVENESS_FAILED) {
        *failed = true;
        return SERVICE_SUPERVISOR_LIVENESS_FAILED;
    }
    if (now_ticks < service->last_liveness_tick) {
        return SERVICE_SUPERVISOR_BAD_LIVENESS;
    }
    if (now_ticks - service->last_liveness_tick >
        liveness_timeout_ticks) {
        service->health_state = SERVICE_HEALTH_LIVENESS_FAILED;
        *failed = true;
        service_supervisor_touch(supervisor);
        return SERVICE_SUPERVISOR_LIVENESS_FAILED;
    }

    *failed = false;
    return SERVICE_SUPERVISOR_OK;
}

enum service_health_state service_supervisor_health_state(
    const struct service_supervisor *supervisor,
    uint32_t service_id)
{
    const struct service_supervisor_service *const service =
        service_supervisor_find_const(supervisor, service_id);

    return service != NULL ? service->health_state :
                             SERVICE_HEALTH_PENDING;
}

uint64_t service_supervisor_liveness_generation(
    const struct service_supervisor *supervisor,
    uint32_t service_id)
{
    const struct service_supervisor_service *const service =
        service_supervisor_find_const(supervisor, service_id);

    return service != NULL ? service->liveness_generation : 0U;
}

uint64_t service_supervisor_readiness_deadline_tick(
    const struct service_supervisor *supervisor,
    uint32_t service_id)
{
    const struct service_supervisor_service *const service =
        service_supervisor_find_const(supervisor, service_id);

    return service != NULL ? service->readiness_deadline_tick : 0U;
}

static void service_supervisor_snapshot_sort_entries(
    struct service_supervisor_snapshot_entry *entries,
    uint32_t entry_count)
{
    uint32_t index;

    for (index = 1U; index < entry_count; ++index) {
        struct service_supervisor_snapshot_entry entry = entries[index];
        uint32_t position = index;

        while (position > 0U &&
               entries[position - 1U].descriptor.id >
                   entry.descriptor.id) {
            entries[position] = entries[position - 1U];
            --position;
        }
        entries[position] = entry;
    }
}

enum service_supervisor_status service_supervisor_snapshot(
    const struct service_supervisor *supervisor,
    struct service_supervisor_snapshot *snapshot)
{
    struct service_supervisor_snapshot published;
    uint64_t generation;
    uint32_t entry_count = 0U;
    uint32_t index;

    if (supervisor == NULL || snapshot == NULL) {
        return SERVICE_SUPERVISOR_NULL;
    }
    if (!supervisor->initialized) {
        return SERVICE_SUPERVISOR_BAD_STATE;
    }

    memset(&published, 0, sizeof(published));
    generation = supervisor->mutation_generation;
    published.version = SERVICE_SUPERVISOR_SNAPSHOT_VERSION;
    published.generation = generation;
    published.active_count = supervisor->active_count;
    for (index = 0U; index < SERVICE_SUPERVISOR_CAPACITY; ++index) {
        const struct service_supervisor_service *const service =
            &supervisor->services[index];
        struct service_supervisor_snapshot_entry *const entry =
            &published.entries[entry_count];

        if (service->state == SERVICE_SUPERVISOR_STATE_STOPPED) {
            continue;
        }
        entry->descriptor = service->descriptor;
        entry->state = service->state;
        entry->health_state = service->health_state;
        entry->restart_count = service->restart_count;
        entry->exit_status = service->exit_status;
        entry->ready_tick = service->ready_tick;
        entry->last_liveness_tick = service->last_liveness_tick;
        entry->liveness_generation = service->liveness_generation;
        entry->readiness_start_tick = service->readiness_start_tick;
        entry->readiness_deadline_tick = service->readiness_deadline_tick;
        entry->readiness_timeout_ticks = service->readiness_timeout_ticks;
        ++entry_count;
    }
    published.entry_count = entry_count;
    service_supervisor_snapshot_sort_entries(
        published.entries, published.entry_count);

#ifdef HFOC_SERVICE_SUPERVISOR_HOST_TEST
    if (service_supervisor_host_snapshot_mutate) {
        struct service_supervisor *const mutable_supervisor =
            (struct service_supervisor *)supervisor;

        mutable_supervisor->services[0].state =
            SERVICE_SUPERVISOR_STATE_EXITED;
        service_supervisor_touch(mutable_supervisor);
    }
#endif

    if (supervisor->mutation_generation != generation) {
        return SERVICE_SUPERVISOR_SNAPSHOT_INVALID;
    }
    published.published = true;
    *snapshot = published;
    return SERVICE_SUPERVISOR_OK;
}

bool service_supervisor_snapshot_published(
    const struct service_supervisor_snapshot *snapshot)
{
    return snapshot != NULL && snapshot->published;
}

uint32_t service_supervisor_snapshot_count(
    const struct service_supervisor_snapshot *snapshot)
{
    if (snapshot == NULL || !snapshot->published) {
        return 0U;
    }
    return snapshot->entry_count;
}

const struct service_supervisor_snapshot_entry *
service_supervisor_snapshot_entry(
    const struct service_supervisor_snapshot *snapshot,
    uint32_t position)
{
    if (snapshot == NULL || !snapshot->published ||
        position >= snapshot->entry_count) {
        return NULL;
    }
    return &snapshot->entries[position];
}

const char *service_supervisor_status_string(
    enum service_supervisor_status status)
{
    switch (status) {
    case SERVICE_SUPERVISOR_OK:
        return "SERVICE_SUPERVISOR_OK";
    case SERVICE_SUPERVISOR_NULL:
        return "SERVICE_SUPERVISOR_NULL";
    case SERVICE_SUPERVISOR_BAD_STATE:
        return "SERVICE_SUPERVISOR_BAD_STATE";
    case SERVICE_SUPERVISOR_FULL:
        return "SERVICE_SUPERVISOR_FULL";
    case SERVICE_SUPERVISOR_NOT_FOUND:
        return "SERVICE_SUPERVISOR_NOT_FOUND";
    case SERVICE_SUPERVISOR_DISABLED:
        return "SERVICE_SUPERVISOR_DISABLED";
    case SERVICE_SUPERVISOR_ALREADY_STARTED:
        return "SERVICE_SUPERVISOR_ALREADY_STARTED";
    case SERVICE_SUPERVISOR_NOT_EXITED:
        return "SERVICE_SUPERVISOR_NOT_EXITED";
    case SERVICE_SUPERVISOR_EXHAUSTED:
        return "SERVICE_SUPERVISOR_EXHAUSTED";
    case SERVICE_SUPERVISOR_PROCESS_FAILED:
        return "SERVICE_SUPERVISOR_PROCESS_FAILED";
    case SERVICE_SUPERVISOR_NOT_BSP:
        return "SERVICE_SUPERVISOR_NOT_BSP";
    case SERVICE_SUPERVISOR_INVALID_OWNED_STATE:
        return "SERVICE_SUPERVISOR_INVALID_OWNED_STATE";
    case SERVICE_SUPERVISOR_NOT_READY:
        return "SERVICE_SUPERVISOR_NOT_READY";
    case SERVICE_SUPERVISOR_ALREADY_READY:
        return "SERVICE_SUPERVISOR_ALREADY_READY";
    case SERVICE_SUPERVISOR_LIVENESS_FAILED:
        return "SERVICE_SUPERVISOR_LIVENESS_FAILED";
    case SERVICE_SUPERVISOR_BAD_LIVENESS:
        return "SERVICE_SUPERVISOR_BAD_LIVENESS";
    case SERVICE_SUPERVISOR_DEPENDENCY_BLOCKED:
        return "SERVICE_SUPERVISOR_DEPENDENCY_BLOCKED";
    case SERVICE_SUPERVISOR_RESTART_INTERVAL:
        return "SERVICE_SUPERVISOR_RESTART_INTERVAL";
    case SERVICE_SUPERVISOR_RESTART_DISABLED:
        return "SERVICE_SUPERVISOR_RESTART_DISABLED";
    case SERVICE_SUPERVISOR_RESTART_TIME_OVERFLOW:
        return "SERVICE_SUPERVISOR_RESTART_TIME_OVERFLOW";
    case SERVICE_SUPERVISOR_READINESS_TIMEOUT:
        return "SERVICE_SUPERVISOR_READINESS_TIMEOUT";
    case SERVICE_SUPERVISOR_BAD_READINESS:
        return "SERVICE_SUPERVISOR_BAD_READINESS";
    case SERVICE_SUPERVISOR_READINESS_OVERFLOW:
        return "SERVICE_SUPERVISOR_READINESS_OVERFLOW";
    case SERVICE_SUPERVISOR_SNAPSHOT_INVALID:
        return "SERVICE_SUPERVISOR_SNAPSHOT_INVALID";
    default:
        return NULL;
    }
}

const char *service_supervisor_state_string(
    enum service_supervisor_state state)
{
    switch (state) {
    case SERVICE_SUPERVISOR_STATE_STOPPED:
        return "SERVICE_SUPERVISOR_STATE_STOPPED";
    case SERVICE_SUPERVISOR_STATE_RUNNING:
        return "SERVICE_SUPERVISOR_STATE_RUNNING";
    case SERVICE_SUPERVISOR_STATE_EXITED:
        return "SERVICE_SUPERVISOR_STATE_EXITED";
    case SERVICE_SUPERVISOR_STATE_EXHAUSTED:
        return "SERVICE_SUPERVISOR_STATE_EXHAUSTED";
    default:
        return NULL;
    }
}

const char *service_supervisor_health_state_string(
    enum service_health_state state)
{
    switch (state) {
    case SERVICE_HEALTH_PENDING:
        return "SERVICE_HEALTH_PENDING";
    case SERVICE_HEALTH_READY:
        return "SERVICE_HEALTH_READY";
    case SERVICE_HEALTH_LIVENESS_FAILED:
        return "SERVICE_HEALTH_LIVENESS_FAILED";
    case SERVICE_HEALTH_READINESS_TIMEOUT:
        return "SERVICE_HEALTH_READINESS_TIMEOUT";
    default:
        return NULL;
    }
}
