#include "context.h"
#include "cpu.h"
#include "fault_injection.h"
#include "process.h"
#include "service.h"
#include "service_supervisor.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifdef HFOC_SERVICE_SUPERVISOR_HOST_TEST

#define TEST_HHDM        UINT64_C(0xFFFF800000000000)
#define TEST_STACK_PHYS  UINT64_C(0x00100000)
#define TEST_AS_PHYS     UINT64_C(0x00200000)
#define PROCESS_POOL_CAP 24U

static int failures;

#define CHECK(cond, msg) do { \
    if (!(cond)) { \
        fprintf(stderr, "FAIL: %s\n", msg); \
        ++failures; \
    } else { \
        printf("PASS: %s\n", msg); \
    } \
} while (0)

static void test_entry_a(void) {
}

static void test_entry_b(void) {
}

static struct kernel_process process_pool[PROCESS_POOL_CAP];
static bool process_pool_used[PROCESS_POOL_CAP];
static uint32_t heap_allocs;
static uint32_t heap_frees;
static bool heap_fail;

void *kmalloc(size_t size) {
    uint32_t index;

    if (heap_fail || size != sizeof(struct kernel_process)) {
        return NULL;
    }
    for (index = 0U; index < PROCESS_POOL_CAP; ++index) {
        if (!process_pool_used[index]) {
            process_pool_used[index] = true;
            memset(&process_pool[index], 0, sizeof(process_pool[index]));
            ++heap_allocs;
            return &process_pool[index];
        }
    }
    return NULL;
}

bool kfree(void *ptr) {
    uint32_t index;

    for (index = 0U; index < PROCESS_POOL_CAP; ++index) {
        if (&process_pool[index] == ptr) {
            process_pool_used[index] = false;
            ++heap_frees;
            return true;
        }
    }
    return false;
}

static uint64_t next_stack_phys;
static uint32_t stack_allocs;
static uint32_t stack_frees;
static bool stack_fail;

uint64_t pmm_alloc_contiguous_pages(uint32_t count) {
    uint64_t physical;

    if (stack_fail || count != PROCESS_KERNEL_STACK_PAGES) {
        return 0U;
    }
    physical = next_stack_phys;
    next_stack_phys += (uint64_t)count * 4096U;
    ++stack_allocs;
    return physical;
}

bool pmm_free_contiguous_pages(uint64_t physical_address, uint32_t count) {
    if (physical_address == 0U || count == 0U) {
        return false;
    }
    ++stack_frees;
    return true;
}

static uint32_t as_allocs;
static uint32_t as_frees;
static bool as_fail;

enum vmm_status vmm_create_address_space(struct address_space *out,
                                         uint64_t hhdm_offset)
{
    (void)hhdm_offset;
    if (out == NULL) {
        return VMM_BAD_OUTPUT_POINTER;
    }
    if (as_fail) {
        return VMM_PMM_ALLOC_FAILED;
    }
    out->pml4_phys = TEST_AS_PHYS + (uint64_t)as_allocs * 4096U;
    ++as_allocs;
    return VMM_OK;
}

enum vmm_status vmm_destroy_address_space(struct address_space *as,
                                          uint64_t hhdm_offset)
{
    (void)hhdm_offset;
    if (as == NULL || as->pml4_phys == 0U) {
        return VMM_BAD_ADDRESS_SPACE;
    }
    as->pml4_phys = 0U;
    ++as_frees;
    return VMM_OK;
}

enum context_status context_frame_init(struct context_frame *frame,
                                       uintptr_t stack_top,
                                       void (*entry)(void))
{
    if (frame == NULL || entry == NULL || stack_top < 16U) {
        return CONTEXT_NULL;
    }
    memset(frame, 0, sizeof(*frame));
    frame->rip = (uintptr_t)entry;
    frame->rsp = stack_top - 8U;
    return CONTEXT_OK;
}

static struct cpu test_cpu;
static struct cpu *test_current_cpu;

struct cpu *cpu_current(void) {
    return test_current_cpu;
}

static void set_bsp(void) {
    memset(&test_cpu, 0, sizeof(test_cpu));
    test_cpu.index = 0U;
    test_cpu.state = CPU_STATE_BSP;
    test_current_cpu = &test_cpu;
}

static void set_ap(void) {
    memset(&test_cpu, 0, sizeof(test_cpu));
    test_cpu.index = 1U;
    test_cpu.state = CPU_STATE_ONLINE;
    test_current_cpu = &test_cpu;
}

static struct service_descriptor make_descriptor(uint32_t id,
                                                 bool enabled,
                                                 uint32_t restart_limit,
                                                 const char *name)
{
    struct service_descriptor descriptor;

    memset(&descriptor, 0, sizeof(descriptor));
    descriptor.id = id;
    descriptor.version = SERVICE_DESCRIPTOR_VERSION;
    descriptor.enabled = enabled;
    descriptor.restart_limit = restart_limit;
    descriptor.readiness_timeout_ms = 1000U;
    if (name != NULL) {
        strncpy(descriptor.name, name, SERVICE_NAME_MAX);
    }
    return descriptor;
}

static struct service_descriptor make_descriptor_with_deps(
    uint32_t id,
    bool enabled,
    uint32_t restart_limit,
    const char *name,
    const uint8_t *dependencies,
    uint32_t dependency_count)
{
    struct service_descriptor descriptor =
        make_descriptor(id, enabled, restart_limit, name);

    descriptor.dependency_count = dependency_count;
    if (dependencies != NULL && dependency_count > 0U) {
        memcpy(descriptor.dependencies, dependencies, dependency_count);
    }
    return descriptor;
}

static struct service_descriptor make_descriptor_with_limits(
    uint32_t id,
    bool enabled,
    uint32_t restart_limit,
    const char *name,
    uint64_t cpu_ticks_limit,
    uint64_t user_memory_limit_pages)
{
    struct service_descriptor descriptor =
        make_descriptor(id, enabled, restart_limit, name);

    descriptor.cpu_ticks_limit = cpu_ticks_limit;
    descriptor.user_memory_limit_pages = user_memory_limit_pages;
    return descriptor;
}

static void reset_all(void) {
    process_host_test_reset();
    service_supervisor_host_test_reset();
    memset(process_pool, 0, sizeof(process_pool));
    memset(process_pool_used, 0, sizeof(process_pool_used));
    heap_allocs = 0U;
    heap_frees = 0U;
    heap_fail = false;
    next_stack_phys = TEST_STACK_PHYS;
    stack_allocs = 0U;
    stack_frees = 0U;
    stack_fail = false;
    as_allocs = 0U;
    as_frees = 0U;
    as_fail = false;
    set_bsp();
}

static void test_null_bad_state_and_not_bsp(void) {
    struct service_supervisor supervisor;
    struct service_descriptor descriptor;
    uint32_t status = 0U;

    reset_all();
    memset(&supervisor, 0, sizeof(supervisor));
    CHECK(service_supervisor_init(NULL, TEST_HHDM) ==
              SERVICE_SUPERVISOR_NULL,
          "supervisor init rejects null supervisor");
    CHECK(service_supervisor_init(&supervisor, TEST_HHDM) ==
              SERVICE_SUPERVISOR_OK &&
              supervisor.initialized &&
              supervisor.supervisor_pcb.published &&
              supervisor.supervisor_pcb.pid == 1U,
          "supervisor init publishes BSP root PCB");
    CHECK(service_supervisor_init(&supervisor, TEST_HHDM) ==
              SERVICE_SUPERVISOR_BAD_STATE,
          "supervisor double init returns BAD_STATE");

    descriptor = make_descriptor(1U, true, 0U, "service-a");
    CHECK(service_supervisor_start(NULL, &descriptor, test_entry_a) ==
              SERVICE_SUPERVISOR_NULL,
          "supervisor start rejects null supervisor");
    CHECK(service_supervisor_start(&supervisor, NULL, test_entry_a) ==
              SERVICE_SUPERVISOR_NULL,
          "supervisor start rejects null descriptor");
    CHECK(service_supervisor_start(&supervisor, &descriptor, NULL) ==
              SERVICE_SUPERVISOR_NULL,
          "supervisor start rejects null entry");
    CHECK(service_supervisor_exit(NULL, 1U, 0U) ==
              SERVICE_SUPERVISOR_NULL,
          "supervisor exit rejects null supervisor");
    CHECK(service_supervisor_reap(NULL, 1U, &status) ==
              SERVICE_SUPERVISOR_NULL,
          "supervisor reap rejects null supervisor");
    CHECK(service_supervisor_reap(&supervisor, 1U, NULL) ==
              SERVICE_SUPERVISOR_NULL,
          "supervisor reap rejects null status output");

    memset(&supervisor, 0, sizeof(supervisor));
    CHECK(service_supervisor_start(&supervisor, &descriptor, test_entry_a) ==
              SERVICE_SUPERVISOR_BAD_STATE,
          "supervisor start on uninitialized supervisor returns BAD_STATE");

    memset(&supervisor, 0, sizeof(supervisor));
    set_ap();
    CHECK(service_supervisor_init(&supervisor, TEST_HHDM) ==
              SERVICE_SUPERVISOR_NOT_BSP &&
              !supervisor.initialized,
          "supervisor init rejects non-BSP CPU without publication");
}

static void test_invalid_and_disabled_descriptor(void) {
    struct service_supervisor supervisor;
    struct service_descriptor descriptor;

    reset_all();
    memset(&supervisor, 0, sizeof(supervisor));
    CHECK(service_supervisor_init(&supervisor, TEST_HHDM) ==
              SERVICE_SUPERVISOR_OK,
          "invalid descriptor fixture initializes supervisor");

    descriptor = make_descriptor(0U, true, 0U, "bad-id");
    CHECK(service_supervisor_start(&supervisor, &descriptor,
                                   test_entry_a) ==
              SERVICE_SUPERVISOR_BAD_STATE,
          "invalid descriptor start returns BAD_STATE");

    descriptor = make_descriptor(1U, false, 0U, "disabled");
    CHECK(service_supervisor_start(&supervisor, &descriptor,
                                   test_entry_a) ==
              SERVICE_SUPERVISOR_DISABLED,
          "disabled descriptor start returns DISABLED");
    CHECK(supervisor.active_count == 0U &&
              heap_allocs == 0U && stack_allocs == 0U && as_allocs == 0U,
          "invalid and disabled starts publish no process resources");
}

static void test_start_ownership_and_duplicate(void) {
    struct service_supervisor supervisor;
    struct service_descriptor descriptor;

    reset_all();
    memset(&supervisor, 0, sizeof(supervisor));
    CHECK(service_supervisor_init(&supervisor, TEST_HHDM) ==
              SERVICE_SUPERVISOR_OK,
          "start ownership fixture initializes supervisor");

    descriptor = make_descriptor(7U, true, 3U, "service-a");
    CHECK(service_supervisor_start(&supervisor, &descriptor,
                                   test_entry_a) ==
              SERVICE_SUPERVISOR_OK,
          "valid descriptor starts a service");
    CHECK(supervisor.active_count == 1U &&
              supervisor.services[0].state ==
                  SERVICE_SUPERVISOR_STATE_RUNNING &&
              supervisor.services[0].descriptor.id == 7U &&
              supervisor.services[0].descriptor.restart_limit == 3U &&
              supervisor.services[0].entry == test_entry_a &&
              supervisor.services[0].restart_count == 0U &&
              supervisor.services[0].process != NULL,
          "start publishes exact descriptor and process ownership");
    CHECK(supervisor.services[0].process->pcb.parent ==
              &supervisor.supervisor_pcb &&
              supervisor.supervisor_pcb.child_count == 1U &&
              supervisor.services[0].process->pcb.pid == 2U,
          "started service process is a child of the supervisor PCB");
    CHECK(heap_allocs == 1U && stack_allocs == 1U && as_allocs == 1U &&
              heap_frees == 0U && stack_frees == 0U && as_frees == 0U,
          "started service owns one heap, stack, and address space");

    CHECK(service_supervisor_start(&supervisor, &descriptor,
                                   test_entry_a) ==
              SERVICE_SUPERVISOR_ALREADY_STARTED,
          "duplicate service ID start returns ALREADY_STARTED");
    CHECK(supervisor.active_count == 1U &&
              heap_allocs == 1U && stack_allocs == 1U && as_allocs == 1U,
          "duplicate start leaves supervisor ownership unchanged");
}

static void test_exit_and_exact_reap(void) {
    struct service_supervisor supervisor;
    struct service_descriptor descriptor_a;
    struct service_descriptor descriptor_b;
    uint32_t status = 0U;

    reset_all();
    memset(&supervisor, 0, sizeof(supervisor));
    CHECK(service_supervisor_init(&supervisor, TEST_HHDM) ==
              SERVICE_SUPERVISOR_OK,
          "exact reap fixture initializes supervisor");
    descriptor_a = make_descriptor(1U, true, 0U, "service-a");
    descriptor_b = make_descriptor(2U, true, 0U, "service-b");
    CHECK(service_supervisor_start(&supervisor, &descriptor_a,
                                   test_entry_a) ==
              SERVICE_SUPERVISOR_OK &&
              service_supervisor_start(&supervisor, &descriptor_b,
                                       test_entry_b) ==
                  SERVICE_SUPERVISOR_OK,
          "exact reap fixture starts two services");

    CHECK(service_supervisor_exit(&supervisor, 1U, 0U) ==
              SERVICE_SUPERVISOR_OK,
          "running service A exits");
    CHECK(supervisor.services[0].state ==
              SERVICE_SUPERVISOR_STATE_EXITED &&
              supervisor.services[0].process->lifecycle ==
                  PROCESS_LIFECYCLE_EXITED,
          "exit publishes exact service state");
    CHECK(service_supervisor_exit(&supervisor, 2U, 42U) ==
              SERVICE_SUPERVISOR_OK,
          "running service B exits with exact status");
    CHECK(heap_frees == 0U && stack_frees == 0U && as_frees == 0U,
          "exit keeps process resources until reap");

    CHECK(service_supervisor_reap(&supervisor, 1U, &status) ==
              SERVICE_SUPERVISOR_OK && status == 0U,
          "exact reap returns service A status");
    CHECK(supervisor.services[0].process == NULL &&
              supervisor.services[0].state ==
                  SERVICE_SUPERVISOR_STATE_EXHAUSTED &&
              supervisor.supervisor_pcb.child_count == 1U,
          "exact reap releases only service A");
    CHECK(heap_frees == 1U && stack_frees == 1U && as_frees == 1U,
          "exact reap frees only the first service resources");

    CHECK(service_supervisor_reap(&supervisor, 2U, &status) ==
              SERVICE_SUPERVISOR_OK && status == 42U,
          "exact reap returns service B status");
    CHECK(supervisor.services[1].process == NULL &&
              supervisor.supervisor_pcb.child_count == 0U &&
              heap_frees == 2U && stack_frees == 2U && as_frees == 2U,
          "exact reap releases both services exactly once");
}

static void test_restart_budget_no_leak(void) {
    struct service_supervisor supervisor;
    struct service_descriptor descriptor;
    uint32_t status = 0U;
    uint32_t expected_restarts;

    reset_all();
    memset(&supervisor, 0, sizeof(supervisor));
    CHECK(service_supervisor_init(&supervisor, TEST_HHDM) ==
              SERVICE_SUPERVISOR_OK,
          "restart budget fixture initializes supervisor");
    descriptor = make_descriptor(1U, true, 2U, "service-a");
    CHECK(service_supervisor_start(&supervisor, &descriptor,
                                   test_entry_a) ==
              SERVICE_SUPERVISOR_OK,
          "restart budget fixture starts service");

    for (expected_restarts = 1U; expected_restarts <= 2U;
         ++expected_restarts) {
        CHECK(service_supervisor_exit(&supervisor, 1U,
                                      expected_restarts + 10U) ==
                  SERVICE_SUPERVISOR_OK,
              "running restart service exits");
        CHECK(service_supervisor_reap(&supervisor, 1U, &status) ==
                  SERVICE_SUPERVISOR_OK &&
                  status == expected_restarts + 10U,
              "restart reap returns prior exit status");
        CHECK(supervisor.services[0].state ==
                  SERVICE_SUPERVISOR_STATE_RUNNING &&
                  supervisor.services[0].restart_count ==
                      expected_restarts &&
                  supervisor.services[0].process != NULL,
              "automatic restart publishes replacement process");
        CHECK(heap_allocs == expected_restarts + 1U &&
                  heap_frees == expected_restarts &&
                  stack_frees == expected_restarts &&
                  as_frees == expected_restarts,
              "automatic restart keeps exactly one live process");
    }

    CHECK(service_supervisor_exit(&supervisor, 1U, 99U) ==
              SERVICE_SUPERVISOR_OK,
          "final restart service exits");
    CHECK(service_supervisor_reap(&supervisor, 1U, &status) ==
              SERVICE_SUPERVISOR_OK && status == 99U,
          "final restart reap returns exact status");
    CHECK(supervisor.services[0].state ==
              SERVICE_SUPERVISOR_STATE_EXHAUSTED &&
              supervisor.services[0].restart_count == 2U &&
              supervisor.services[0].process == NULL &&
              supervisor.supervisor_pcb.child_count == 0U,
          "restart exhaustion publishes no process");
    CHECK(heap_allocs == 3U && heap_frees == 3U &&
              stack_allocs == 3U && stack_frees == 3U &&
              as_allocs == 3U && as_frees == 3U,
          "restart exhaustion leaves no leaked process resources");
}

static void test_restart_failure_no_leak(void) {
    struct service_supervisor supervisor;
    struct service_descriptor descriptor;
    uint32_t status = 0U;

    reset_all();
    memset(&supervisor, 0, sizeof(supervisor));
    CHECK(service_supervisor_init(&supervisor, TEST_HHDM) ==
              SERVICE_SUPERVISOR_OK,
          "restart failure fixture initializes supervisor");
    descriptor = make_descriptor(1U, true, 1U, "service-a");
    CHECK(service_supervisor_start(&supervisor, &descriptor,
                                   test_entry_a) ==
              SERVICE_SUPERVISOR_OK,
          "restart failure fixture starts service");
    CHECK(service_supervisor_exit(&supervisor, 1U, 5U) ==
              SERVICE_SUPERVISOR_OK,
          "restart failure fixture exits service");

    heap_fail = true;
    CHECK(service_supervisor_reap(&supervisor, 1U, &status) ==
              SERVICE_SUPERVISOR_PROCESS_FAILED && status == 5U,
          "failed automatic restart returns PROCESS_FAILED");
    CHECK(supervisor.services[0].state ==
              SERVICE_SUPERVISOR_STATE_EXHAUSTED &&
              supervisor.services[0].process == NULL &&
              supervisor.services[0].restart_count == 1U &&
              supervisor.supervisor_pcb.child_count == 0U,
          "failed automatic restart publishes exhausted no-process state");
    CHECK(heap_allocs == 1U && heap_frees == 1U &&
              stack_allocs == 1U && stack_frees == 1U &&
              as_allocs == 1U && as_frees == 1U,
          "failed automatic restart leaks no process resources");
}

static void test_capacity_full(void) {
    struct service_supervisor supervisor;
    struct service_descriptor descriptor;
    uint32_t id;

    reset_all();
    memset(&supervisor, 0, sizeof(supervisor));
    CHECK(service_supervisor_init(&supervisor, TEST_HHDM) ==
              SERVICE_SUPERVISOR_OK,
          "capacity fixture initializes supervisor");
    for (id = 1U; id <= SERVICE_SUPERVISOR_CAPACITY; ++id) {
        descriptor = make_descriptor(id, true, 0U, "service");
        CHECK(service_supervisor_start(&supervisor, &descriptor,
                                       test_entry_a) ==
                  SERVICE_SUPERVISOR_OK,
              "capacity fixture starts service within bound");
    }
    CHECK(supervisor.active_count == SERVICE_SUPERVISOR_CAPACITY,
          "capacity fixture publishes exact active count");

    descriptor = make_descriptor(255U, true, 0U, "full");
    CHECK(service_supervisor_start(&supervisor, &descriptor,
                                   test_entry_a) ==
              SERVICE_SUPERVISOR_FULL,
          "over-capacity start returns FULL");
    CHECK(supervisor.active_count == SERVICE_SUPERVISOR_CAPACITY &&
              heap_allocs == SERVICE_SUPERVISOR_CAPACITY,
          "over-capacity start publishes no extra process");
}

static void test_status_strings_and_accessors(void) {
    struct service_supervisor supervisor;
    enum service_supervisor_status status;
    enum service_supervisor_state state;

    reset_all();
    memset(&supervisor, 0, sizeof(supervisor));
    for (status = SERVICE_SUPERVISOR_OK;
         status <= SERVICE_SUPERVISOR_SNAPSHOT_INVALID; ++status) {
        CHECK(service_supervisor_status_string(status) != NULL,
              "supervisor status string is present for every status");
    }
    CHECK(service_supervisor_status_string(
              (enum service_supervisor_status)
                  (SERVICE_SUPERVISOR_SNAPSHOT_INVALID + 1U)) == NULL,
          "supervisor status string rejects undefined status");
    CHECK(strcmp(service_supervisor_status_string(
                     SERVICE_SUPERVISOR_DEPENDENCY_BLOCKED),
                 "SERVICE_SUPERVISOR_DEPENDENCY_BLOCKED") == 0 &&
              strcmp(service_supervisor_status_string(
                         SERVICE_SUPERVISOR_RESTART_INTERVAL),
                     "SERVICE_SUPERVISOR_RESTART_INTERVAL") == 0 &&
              strcmp(service_supervisor_status_string(
                         SERVICE_SUPERVISOR_RESTART_DISABLED),
                     "SERVICE_SUPERVISOR_RESTART_DISABLED") == 0 &&
              strcmp(service_supervisor_status_string(
                         SERVICE_SUPERVISOR_RESTART_TIME_OVERFLOW),
                     "SERVICE_SUPERVISOR_RESTART_TIME_OVERFLOW") == 0 &&
              strcmp(service_supervisor_status_string(
                         SERVICE_SUPERVISOR_READINESS_TIMEOUT),
                     "SERVICE_SUPERVISOR_READINESS_TIMEOUT") == 0 &&
              strcmp(service_supervisor_status_string(
                         SERVICE_SUPERVISOR_BAD_READINESS),
                     "SERVICE_SUPERVISOR_BAD_READINESS") == 0 &&
              strcmp(service_supervisor_status_string(
                         SERVICE_SUPERVISOR_READINESS_OVERFLOW),
                     "SERVICE_SUPERVISOR_READINESS_OVERFLOW") == 0 &&
              strcmp(service_supervisor_status_string(
                         SERVICE_SUPERVISOR_SNAPSHOT_INVALID),
                     "SERVICE_SUPERVISOR_SNAPSHOT_INVALID") == 0,
          "restart and readiness status strings are exact");
    for (state = SERVICE_SUPERVISOR_STATE_STOPPED;
         state <= SERVICE_SUPERVISOR_STATE_EXHAUSTED; ++state) {
        CHECK(service_supervisor_state_string(state) != NULL,
              "supervisor state string is present for every state");
    }
    CHECK(service_supervisor_state_string(
              (enum service_supervisor_state)4U) == NULL,
          "supervisor state string rejects undefined state");
    CHECK(service_supervisor_restart_count(NULL, 1U) == 0U &&
              service_supervisor_state(NULL, 1U) ==
                  SERVICE_SUPERVISOR_STATE_STOPPED &&
              service_supervisor_readiness_deadline_tick(NULL, 1U) == 0U,
          "supervisor accessors tolerate null supervisor");
    CHECK(service_supervisor_restart_count(&supervisor, 1U) == 0U &&
              service_supervisor_state(&supervisor, 1U) ==
                  SERVICE_SUPERVISOR_STATE_STOPPED &&
              service_supervisor_readiness_deadline_tick(&supervisor, 1U) ==
                  0U,
          "supervisor accessors reject unknown service");
}

static void test_restart_containment_null_and_bad_state(void) {
    struct service_supervisor supervisor;
    struct service_descriptor descriptor;
    uint32_t status = 0U;
    bool restarted = false;

    reset_all();
    memset(&supervisor, 0, sizeof(supervisor));
    CHECK(service_supervisor_configure_restart_policy(
              NULL, 1U, 5U, false) == SERVICE_SUPERVISOR_NULL,
          "restart policy configuration rejects null supervisor");
    CHECK(service_supervisor_configure_restart_policy(
              &supervisor, 1U, 5U, false) == SERVICE_SUPERVISOR_BAD_STATE,
          "restart policy configuration on uninitialized supervisor returns BAD_STATE");
    CHECK(service_supervisor_reap_contained(
              NULL, 1U, 10U, NULL, &status, &restarted) ==
              SERVICE_SUPERVISOR_NULL,
          "contained reap rejects null supervisor");
    CHECK(service_supervisor_reap_contained(
              &supervisor, 1U, 10U, NULL, &status, &restarted) ==
              SERVICE_SUPERVISOR_BAD_STATE,
          "contained reap on uninitialized supervisor returns BAD_STATE");
    CHECK(service_supervisor_restart_contained(
              NULL, 1U, 10U, NULL, &restarted) == SERVICE_SUPERVISOR_NULL,
          "contained restart rejects null supervisor");
    CHECK(service_supervisor_restart_contained(
              &supervisor, 1U, 10U, NULL, &restarted) ==
              SERVICE_SUPERVISOR_BAD_STATE,
          "contained restart on uninitialized supervisor returns BAD_STATE");

    CHECK(service_supervisor_init(&supervisor, TEST_HHDM) ==
              SERVICE_SUPERVISOR_OK,
          "contained null fixture initializes supervisor");
    descriptor = make_descriptor(1U, true, 1U, "contained-a");
    CHECK(service_supervisor_configure_restart_policy(
              &supervisor, 1U, 5U, false) == SERVICE_SUPERVISOR_NOT_FOUND,
          "restart policy configuration rejects unknown service");
    CHECK(service_supervisor_start(&supervisor, &descriptor,
                                   test_entry_a) ==
              SERVICE_SUPERVISOR_OK,
          "contained null fixture starts service");
    CHECK(service_supervisor_configure_restart_policy(
              &supervisor, 1U, 0U, false) == SERVICE_SUPERVISOR_BAD_STATE,
          "zero restart interval is rejected without policy publication");
    CHECK(service_supervisor_reap_contained(
              &supervisor, 1U, 10U, NULL, &status, &restarted) ==
              SERVICE_SUPERVISOR_NOT_EXITED && !restarted,
          "contained reap rejects a running service");
    CHECK(service_supervisor_exit(&supervisor, 1U, 7U) ==
              SERVICE_SUPERVISOR_OK,
          "contained null fixture exits service");
    CHECK(service_supervisor_reap_contained(
              &supervisor, 1U, 10U, NULL, NULL, &restarted) ==
              SERVICE_SUPERVISOR_NULL,
          "contained reap rejects null exit-status output");
    CHECK(service_supervisor_reap_contained(
              &supervisor, 1U, 10U, NULL, &status, NULL) ==
              SERVICE_SUPERVISOR_NULL,
          "contained reap rejects null restarted output");
    CHECK(service_supervisor_reap_contained(
              &supervisor, 1U, 10U, NULL, &status, &restarted) ==
              SERVICE_SUPERVISOR_OK && status == 7U && restarted &&
              supervisor.services[0].state ==
                  SERVICE_SUPERVISOR_STATE_RUNNING &&
              supervisor.services[0].process != NULL,
          "contained reap with no dependencies restarts exact service");
}

static void test_restart_containment_exact_interval_and_retry(void) {
    struct service_supervisor supervisor;
    struct service_descriptor descriptor_a;
    struct service_descriptor descriptor_b;
    struct kernel_process *unrelated_process;
    uint32_t status = 0U;
    bool restarted = false;

    reset_all();
    memset(&supervisor, 0, sizeof(supervisor));
    CHECK(service_supervisor_init(&supervisor, TEST_HHDM) ==
              SERVICE_SUPERVISOR_OK,
          "contained interval fixture initializes supervisor");
    descriptor_a = make_descriptor(1U, true, 2U, "contained-a");
    descriptor_b = make_descriptor(2U, true, 0U, "unrelated-b");
    CHECK(service_supervisor_start(&supervisor, &descriptor_a,
                                   test_entry_a) ==
              SERVICE_SUPERVISOR_OK &&
              service_supervisor_start(&supervisor, &descriptor_b,
                                       test_entry_b) ==
                  SERVICE_SUPERVISOR_OK,
          "contained interval fixture starts two services");
    unrelated_process = supervisor.services[1].process;
    CHECK(service_supervisor_configure_restart_policy(
              &supervisor, 1U, 5U, false) == SERVICE_SUPERVISOR_OK,
          "contained interval fixture configures restart policy");

    CHECK(service_supervisor_exit(&supervisor, 1U, 11U) ==
              SERVICE_SUPERVISOR_OK &&
              service_supervisor_reap_contained(
                  &supervisor, 1U, 10U, NULL, &status, &restarted) ==
                  SERVICE_SUPERVISOR_OK &&
              status == 11U && restarted,
          "first contained restart succeeds within budget");
    CHECK(supervisor.services[0].state ==
              SERVICE_SUPERVISOR_STATE_RUNNING &&
              supervisor.services[0].process != NULL &&
              supervisor.services[0].process->lifecycle ==
                  PROCESS_LIFECYCLE_RUNNING &&
              supervisor.services[0].restart_count == 1U &&
              supervisor.services[1].state ==
                  SERVICE_SUPERVISOR_STATE_RUNNING &&
              supervisor.services[1].process == unrelated_process &&
              supervisor.services[1].restart_count == 0U,
          "first contained restart touches only the exact service");
    CHECK(heap_allocs == 3U && heap_frees == 1U &&
              stack_allocs == 3U && stack_frees == 1U &&
              as_allocs == 3U && as_frees == 1U,
          "first contained restart keeps exactly one replacement process");

    restarted = true;
    CHECK(service_supervisor_exit(&supervisor, 1U, 12U) ==
              SERVICE_SUPERVISOR_OK &&
              service_supervisor_reap_contained(
                  &supervisor, 1U, 10U, NULL, &status, &restarted) ==
                  SERVICE_SUPERVISOR_RESTART_INTERVAL &&
              status == 12U && !restarted,
          "contained restart before interval returns RESTART_INTERVAL");
    CHECK(supervisor.services[0].state ==
              SERVICE_SUPERVISOR_STATE_EXHAUSTED &&
              supervisor.services[0].process == NULL &&
              supervisor.services[0].restart_count == 1U &&
              supervisor.services[1].state ==
                  SERVICE_SUPERVISOR_STATE_RUNNING &&
              supervisor.services[1].process == unrelated_process,
          "interval block publishes no replacement and leaves unrelated service");

    restarted = false;
    CHECK(service_supervisor_restart_contained(
              &supervisor, 1U, 15U, NULL, &restarted) ==
              SERVICE_SUPERVISOR_OK && restarted,
          "contained restart retry succeeds after interval");
    CHECK(supervisor.services[0].state ==
              SERVICE_SUPERVISOR_STATE_RUNNING &&
              supervisor.services[0].process != NULL &&
              supervisor.services[0].restart_count == 2U &&
              supervisor.services[1].state ==
                  SERVICE_SUPERVISOR_STATE_RUNNING &&
              supervisor.services[1].process == unrelated_process,
          "interval retry restarts exact service after elapsed interval");

    restarted = true;
    CHECK(service_supervisor_exit(&supervisor, 1U, 13U) ==
              SERVICE_SUPERVISOR_OK &&
              service_supervisor_reap_contained(
                  &supervisor, 1U, 20U, NULL, &status, &restarted) ==
                  SERVICE_SUPERVISOR_EXHAUSTED && !restarted,
          "final contained restart attempt returns EXHAUSTED");
    CHECK(supervisor.services[0].state ==
              SERVICE_SUPERVISOR_STATE_EXHAUSTED &&
              supervisor.services[0].process == NULL &&
              supervisor.services[0].restart_count == 2U &&
              supervisor.supervisor_pcb.child_count == 1U,
          "restart exhaustion publishes no process for exact service");
    CHECK(heap_allocs == 4U && heap_frees == 3U &&
              stack_allocs == 4U && stack_frees == 3U &&
              as_allocs == 4U && as_frees == 3U,
          "restart exhaustion leaves no leaked process resources");
}

static void test_restart_containment_dependency_policy(void) {
    struct service_supervisor supervisor;
    struct service_graph_snapshot snapshot = {0};
    struct service_supervisor_entry entries[3];
    struct service_descriptor descriptors[3];
    struct kernel_process *unrelated_process;
    uint8_t dep_one[1] = {1U};
    uint32_t status = 0U;
    bool restarted = false;

    reset_all();
    memset(&supervisor, 0, sizeof(supervisor));
    CHECK(service_supervisor_init(&supervisor, TEST_HHDM) ==
              SERVICE_SUPERVISOR_OK,
          "dependency containment fixture initializes supervisor");
    descriptors[0] = make_descriptor_with_deps(
        1U, true, 2U, "dep-b", NULL, 0U);
    descriptors[1] = make_descriptor_with_deps(
        2U, true, 2U, "dep-a", dep_one, 1U);
    descriptors[2] = make_descriptor(3U, true, 0U, "other-c");
    CHECK(service_graph_snapshot_build(&snapshot, descriptors, 3U) ==
              SERVICE_GRAPH_OK,
          "dependency containment fixture builds snapshot");
    entries[0].service_id = 1U;
    entries[0].entry = test_entry_a;
    entries[1].service_id = 2U;
    entries[1].entry = test_entry_b;
    entries[2].service_id = 3U;
    entries[2].entry = test_entry_a;
    CHECK(service_supervisor_start_ordered(&supervisor, &snapshot,
                                           entries, 3U) ==
              SERVICE_SUPERVISOR_OK,
          "dependency containment fixture starts all services");
    unrelated_process = supervisor.services[2].process;
    CHECK(service_supervisor_configure_restart_policy(
              &supervisor, 1U, 5U, false) == SERVICE_SUPERVISOR_OK &&
              service_supervisor_configure_restart_policy(
                  &supervisor, 2U, 5U, false) == SERVICE_SUPERVISOR_OK,
          "dependency containment fixture configures service policies");

    CHECK(service_supervisor_exit(&supervisor, 1U, 21U) ==
              SERVICE_SUPERVISOR_OK &&
              service_supervisor_reap_contained(
                  &supervisor, 1U, 10U, &snapshot, &status, &restarted) ==
                  SERVICE_SUPERVISOR_OK && restarted,
          "dependency service restart succeeds first");
    CHECK(service_supervisor_exit(&supervisor, 1U, 22U) ==
              SERVICE_SUPERVISOR_OK &&
              service_supervisor_reap_contained(
                  &supervisor, 1U, 10U, &snapshot, &status, &restarted) ==
                  SERVICE_SUPERVISOR_RESTART_INTERVAL && !restarted,
          "dependency service is interval-blocked");

    restarted = true;
    CHECK(service_supervisor_exit(&supervisor, 2U, 31U) ==
              SERVICE_SUPERVISOR_OK &&
              service_supervisor_reap_contained(
                  &supervisor, 2U, 10U, &snapshot, &status, &restarted) ==
                  SERVICE_SUPERVISOR_DEPENDENCY_BLOCKED && !restarted,
          "dependent restart is blocked while dependency is unavailable");
    CHECK(supervisor.services[1].state ==
              SERVICE_SUPERVISOR_STATE_EXHAUSTED &&
              supervisor.services[1].process == NULL &&
              supervisor.services[1].restart_count == 0U &&
              supervisor.services[2].state ==
                  SERVICE_SUPERVISOR_STATE_RUNNING &&
              supervisor.services[2].process == unrelated_process &&
              supervisor.services[2].restart_count == 0U,
          "dependency block publishes no dependent replacement and leaves unrelated service");
    CHECK(supervisor.supervisor_pcb.child_count == 1U,
          "dependency block leaves exactly the unrelated service process");

    restarted = false;
    CHECK(service_supervisor_restart_contained(
              &supervisor, 1U, 15U, &snapshot, &restarted) ==
              SERVICE_SUPERVISOR_OK && restarted,
          "dependency service retry succeeds after interval");
    CHECK(service_supervisor_restart_contained(
              &supervisor, 2U, 15U, &snapshot, &restarted) ==
              SERVICE_SUPERVISOR_OK && restarted,
          "blocked dependent retry succeeds after dependency is running");
    CHECK(supervisor.services[0].state ==
              SERVICE_SUPERVISOR_STATE_RUNNING &&
              supervisor.services[0].restart_count == 2U &&
              supervisor.services[1].state ==
                  SERVICE_SUPERVISOR_STATE_RUNNING &&
              supervisor.services[1].restart_count == 1U &&
              supervisor.services[2].state ==
                  SERVICE_SUPERVISOR_STATE_RUNNING &&
              supervisor.services[2].process == unrelated_process &&
              supervisor.supervisor_pcb.child_count == 3U,
          "retry restarts dependency and dependent without touching unrelated service");
}

static void test_restart_containment_disabled_after_exhaustion(void) {
    struct service_supervisor supervisor;
    struct service_descriptor descriptor;
    uint32_t status = 0U;
    bool restarted = false;

    reset_all();
    memset(&supervisor, 0, sizeof(supervisor));
    CHECK(service_supervisor_init(&supervisor, TEST_HHDM) ==
              SERVICE_SUPERVISOR_OK,
          "disabled containment fixture initializes supervisor");
    descriptor = make_descriptor(1U, true, 1U, "disabled-a");
    CHECK(service_supervisor_start(&supervisor, &descriptor,
                                   test_entry_a) ==
              SERVICE_SUPERVISOR_OK &&
              service_supervisor_configure_restart_policy(
                  &supervisor, 1U, 5U, true) == SERVICE_SUPERVISOR_OK,
          "disabled containment fixture configures disabled policy");

    CHECK(service_supervisor_exit(&supervisor, 1U, 41U) ==
              SERVICE_SUPERVISOR_OK &&
              service_supervisor_reap_contained(
                  &supervisor, 1U, 10U, NULL, &status, &restarted) ==
                  SERVICE_SUPERVISOR_OK && restarted &&
              supervisor.services[0].restart_policy.disabled_after_exhaustion &&
              supervisor.services[0].restart_policy.state ==
                  SERVICE_RESTART_POLICY_STATE_DISABLED,
          "disabled policy publishes DISABLED after final committed restart");

    restarted = true;
    CHECK(service_supervisor_exit(&supervisor, 1U, 42U) ==
              SERVICE_SUPERVISOR_OK &&
              service_supervisor_reap_contained(
                  &supervisor, 1U, 20U, NULL, &status, &restarted) ==
                  SERVICE_SUPERVISOR_RESTART_DISABLED && !restarted,
          "disabled policy blocks further contained restart");
    CHECK(service_supervisor_restart_contained(
              &supervisor, 1U, 25U, NULL, &restarted) ==
              SERVICE_SUPERVISOR_RESTART_DISABLED && !restarted,
          "disabled policy retry remains denied");
    CHECK(supervisor.services[0].state ==
              SERVICE_SUPERVISOR_STATE_EXHAUSTED &&
              supervisor.services[0].process == NULL,
          "disabled policy leaves service with no process");
}

static void test_restart_containment_process_failure_no_leak(void) {
    struct service_supervisor supervisor;
    struct service_descriptor descriptor;
    uint32_t status = 0U;
    bool restarted = false;

    reset_all();
    memset(&supervisor, 0, sizeof(supervisor));
    CHECK(service_supervisor_init(&supervisor, TEST_HHDM) ==
              SERVICE_SUPERVISOR_OK,
          "contained process failure fixture initializes supervisor");
    descriptor = make_descriptor(1U, true, 1U, "failure-a");
    CHECK(service_supervisor_start(&supervisor, &descriptor,
                                   test_entry_a) ==
              SERVICE_SUPERVISOR_OK &&
              service_supervisor_configure_restart_policy(
                  &supervisor, 1U, 5U, false) == SERVICE_SUPERVISOR_OK,
          "contained process failure fixture starts service");

    CHECK(service_supervisor_exit(&supervisor, 1U, 51U) ==
              SERVICE_SUPERVISOR_OK,
          "contained process failure fixture exits service");
    heap_fail = true;
    CHECK(service_supervisor_reap_contained(
              &supervisor, 1U, 10U, NULL, &status, &restarted) ==
              SERVICE_SUPERVISOR_PROCESS_FAILED && !restarted,
          "failed contained restart returns PROCESS_FAILED");
    CHECK(supervisor.services[0].state ==
              SERVICE_SUPERVISOR_STATE_EXHAUSTED &&
              supervisor.services[0].process == NULL &&
              supervisor.services[0].restart_count == 1U &&
              supervisor.supervisor_pcb.child_count == 0U,
          "failed contained restart publishes no process");
    CHECK(heap_allocs == 1U && heap_frees == 1U &&
              stack_allocs == 1U && stack_frees == 1U &&
              as_allocs == 1U && as_frees == 1U,
          "failed contained restart leaks no process resources");
}

static void test_resource_envelope_attach_and_limit_failure(void) {
    struct service_supervisor supervisor;
    struct service_descriptor descriptor;
    struct kernel_process *process;
    struct kernel_process *replacement;
    uint32_t status = 0U;

    reset_all();
    memset(&supervisor, 0, sizeof(supervisor));
    CHECK(service_supervisor_init(&supervisor, TEST_HHDM) ==
              SERVICE_SUPERVISOR_OK,
          "resource envelope fixture initializes supervisor");
    descriptor = make_descriptor_with_limits(
        1U, true, 1U, "limited",
        UINT64_C(3), UINT64_C(2));
    CHECK(service_supervisor_start(&supervisor, &descriptor,
                                   test_entry_a) ==
              SERVICE_SUPERVISOR_OK,
          "resource envelope fixture starts service");
    process = supervisor.services[0].process;
    CHECK(process != NULL &&
              process_limit_cpu_ticks(process) == UINT64_C(3) &&
              process_limit_user_memory_limit_pages(process) ==
                  UINT64_C(2) &&
              process_limit_user_memory_pages(process) == 0U,
          "start attaches exact CPU and user-memory limits to process");

    CHECK(process_limit_charge_user_memory(process, UINT64_C(3)) ==
              PROCESS_USER_MEMORY_LIMIT_EXCEEDED &&
              process_limit_user_memory_pages(process) == 0U &&
              process->lifecycle == PROCESS_LIFECYCLE_RUNNING &&
              supervisor.services[0].state ==
                  SERVICE_SUPERVISOR_STATE_RUNNING,
          "over-limit user-memory charge fails without kernel panic");
    CHECK(process_limit_charge_user_memory(process, UINT64_C(1)) ==
              PROCESS_OK &&
              process_limit_charge_user_memory(process, UINT64_C(2)) ==
                  PROCESS_USER_MEMORY_LIMIT_EXCEEDED &&
              process_limit_user_memory_pages(process) == 1U,
          "bounded user-memory charges publish exact accounting");

    CHECK(process_exit_kernel(process, PROCESS_EXIT_LIMIT_EXCEEDED) ==
              PROCESS_OK,
          "CPU limit breach exits service process safely");
    CHECK(service_supervisor_reap(&supervisor, 1U, &status) ==
              SERVICE_SUPERVISOR_OK &&
              status == PROCESS_EXIT_LIMIT_EXCEEDED &&
              supervisor.services[0].state ==
                  SERVICE_SUPERVISOR_STATE_RUNNING &&
              supervisor.services[0].restart_count == 1U &&
              supervisor.services[0].process != NULL,
          "supervisor treats limit breach as service failure and restarts");
    replacement = supervisor.services[0].process;
    CHECK(replacement != NULL &&
              process_limit_cpu_ticks(replacement) == UINT64_C(3) &&
              process_limit_user_memory_limit_pages(replacement) ==
                  UINT64_C(2) &&
              process_limit_user_memory_pages(replacement) == 0U,
          "restart replacement receives exact resource envelope");

    CHECK(process_exit_kernel(replacement, PROCESS_EXIT_LIMIT_EXCEEDED) ==
              PROCESS_OK &&
              service_supervisor_reap(&supervisor, 1U, &status) ==
                  SERVICE_SUPERVISOR_OK &&
              status == PROCESS_EXIT_LIMIT_EXCEEDED &&
              supervisor.services[0].state ==
                  SERVICE_SUPERVISOR_STATE_EXHAUSTED &&
              supervisor.services[0].process == NULL &&
              supervisor.supervisor_pcb.child_count == 0U,
          "final limit failure exhausts service with exact status");
    CHECK(heap_allocs == 2U && heap_frees == 2U &&
              stack_allocs == 2U && stack_frees == 2U &&
              as_allocs == 2U && as_frees == 2U,
          "limit failure lifecycle leaves no leaked process resources");
}

static void test_readiness_and_liveness_state(void) {
    struct service_supervisor supervisor;
    struct service_descriptor descriptor;
    bool published = false;
    bool failed = false;
    uint32_t allocs_before;
    uint32_t frees_before;

    reset_all();
    memset(&supervisor, 0, sizeof(supervisor));
    CHECK(service_supervisor_report_ready(NULL, 1U, 10U, &published) ==
              SERVICE_SUPERVISOR_NULL,
          "ready report rejects null supervisor");
    CHECK(service_supervisor_report_ready(&supervisor, 1U, 10U, NULL) ==
              SERVICE_SUPERVISOR_NULL,
          "ready report rejects null published output");
    CHECK(service_supervisor_report_liveness(NULL, 1U, 11U) ==
              SERVICE_SUPERVISOR_NULL,
          "liveness report rejects null supervisor");
    CHECK(service_supervisor_check_liveness(NULL, 1U, 12U, 10U, &failed) ==
              SERVICE_SUPERVISOR_NULL,
          "liveness check rejects null supervisor");
    CHECK(service_supervisor_check_liveness(&supervisor, 1U, 12U, 10U, NULL) ==
              SERVICE_SUPERVISOR_NULL,
          "liveness check rejects null failed output");
    CHECK(service_supervisor_report_ready(&supervisor, 1U, 10U, &published) ==
              SERVICE_SUPERVISOR_BAD_STATE,
          "ready report on uninitialized supervisor returns BAD_STATE");
    CHECK(service_supervisor_health_state(NULL, 1U) ==
              SERVICE_HEALTH_PENDING &&
              service_supervisor_liveness_generation(NULL, 1U) == 0U,
          "health accessors tolerate null supervisor");

    CHECK(service_supervisor_init(&supervisor, TEST_HHDM) ==
              SERVICE_SUPERVISOR_OK,
          "readiness fixture initializes supervisor");
    descriptor = make_descriptor(1U, true, 1U, "health-a");
    CHECK(service_supervisor_start(&supervisor, &descriptor,
                                   test_entry_a) ==
              SERVICE_SUPERVISOR_OK,
          "readiness fixture starts service");
    CHECK(supervisor.services[0].health_state == SERVICE_HEALTH_PENDING &&
              supervisor.services[0].liveness_generation == 0U &&
              supervisor.services[0].ready_tick == 0U &&
              supervisor.services[0].last_liveness_tick == 0U,
          "started service publishes pending health with zero generation");

    CHECK(service_supervisor_report_ready(&supervisor, 2U, 10U,
                                          &published) ==
              SERVICE_SUPERVISOR_NOT_FOUND && !published,
          "ready report rejects unknown service without publication");
    CHECK(service_supervisor_report_liveness(&supervisor, 1U, 11U) ==
              SERVICE_SUPERVISOR_NOT_READY &&
              supervisor.services[0].health_state ==
                  SERVICE_HEALTH_PENDING &&
              supervisor.services[0].liveness_generation == 0U,
          "liveness before readiness is rejected without publication");

    allocs_before = heap_allocs;
    frees_before = heap_frees;
    CHECK(service_supervisor_report_ready(&supervisor, 1U, 10U,
                                          &published) ==
              SERVICE_SUPERVISOR_OK && published,
          "valid running service publishes ready");
    CHECK(supervisor.services[0].health_state == SERVICE_HEALTH_READY &&
              supervisor.services[0].ready_tick == 10U &&
              supervisor.services[0].last_liveness_tick == 10U &&
              supervisor.services[0].liveness_generation == 1U,
          "ready publication records exact ticks and first generation");
    CHECK(heap_allocs == allocs_before && heap_frees == frees_before,
          "ready publication performs no allocation or free");

    published = true;
    CHECK(service_supervisor_report_ready(&supervisor, 1U, 11U,
                                          &published) ==
              SERVICE_SUPERVISOR_ALREADY_READY && !published,
          "second ready report returns ALREADY_READY without republish");
    CHECK(supervisor.services[0].liveness_generation == 1U &&
              supervisor.services[0].last_liveness_tick == 10U,
          "duplicate ready report does not advance health state");

    CHECK(service_supervisor_report_liveness(&supervisor, 1U, 25U) ==
              SERVICE_SUPERVISOR_OK,
          "ready service reports liveness heartbeat");
    CHECK(supervisor.services[0].liveness_generation == 2U &&
              supervisor.services[0].last_liveness_tick == 25U,
          "liveness heartbeat advances generation and liveness tick");

    failed = true;
    CHECK(service_supervisor_check_liveness(&supervisor, 1U, 30U, 10U,
                                            &failed) ==
              SERVICE_SUPERVISOR_OK && !failed &&
              supervisor.services[0].health_state ==
                  SERVICE_HEALTH_READY,
          "liveness within timeout remains ready");

    CHECK(service_supervisor_check_liveness(&supervisor, 1U, 31U, 0U,
                                            &failed) ==
              SERVICE_SUPERVISOR_BAD_LIVENESS &&
              supervisor.services[0].health_state ==
                  SERVICE_HEALTH_READY,
          "zero liveness timeout is rejected without state change");
    CHECK(service_supervisor_check_liveness(
              &supervisor, 1U, 31U,
              SERVICE_LIVENESS_TIMEOUT_MAX_TICKS + 1U,
              &failed) == SERVICE_SUPERVISOR_BAD_LIVENESS &&
              supervisor.services[0].health_state ==
                  SERVICE_HEALTH_READY,
          "over-limit liveness timeout is rejected without state change");
    CHECK(service_supervisor_check_liveness(&supervisor, 1U, 20U, 10U,
                                            &failed) ==
              SERVICE_SUPERVISOR_BAD_LIVENESS &&
              supervisor.services[0].health_state ==
                  SERVICE_HEALTH_READY,
          "non-monotonic liveness tick is rejected without state change");

    CHECK(service_supervisor_check_liveness(&supervisor, 1U, 36U, 10U,
                                            &failed) ==
              SERVICE_SUPERVISOR_LIVENESS_FAILED && failed &&
              supervisor.services[0].health_state ==
                  SERVICE_HEALTH_LIVENESS_FAILED,
          "missed liveness deadline publishes LIVENESS_FAILED");
    CHECK(service_supervisor_report_liveness(&supervisor, 1U, 37U) ==
              SERVICE_SUPERVISOR_LIVENESS_FAILED &&
              supervisor.services[0].liveness_generation == 2U &&
              supervisor.services[0].last_liveness_tick == 25U,
          "liveness heartbeat cannot revive failed service");
    CHECK(service_supervisor_check_liveness(&supervisor, 1U, 38U, 10U,
                                            &failed) ==
              SERVICE_SUPERVISOR_LIVENESS_FAILED && failed,
          "failed liveness state is permanent for current process");

    descriptor = make_descriptor(2U, true, 0U, "health-b");
    CHECK(service_supervisor_start(&supervisor, &descriptor,
                                   test_entry_b) ==
              SERVICE_SUPERVISOR_OK &&
              supervisor.services[1].health_state ==
                  SERVICE_HEALTH_PENDING,
          "second service starts pending");
    CHECK(service_supervisor_report_ready(&supervisor, 2U, 10U,
                                          &published) ==
              SERVICE_SUPERVISOR_OK && published,
          "second valid service publishes ready");
    CHECK(service_supervisor_exit(&supervisor, 2U, 0U) ==
              SERVICE_SUPERVISOR_OK &&
              supervisor.services[1].state ==
                  SERVICE_SUPERVISOR_STATE_EXITED &&
              supervisor.services[1].health_state ==
                  SERVICE_HEALTH_PENDING &&
              supervisor.services[1].liveness_generation == 0U,
          "service exit resets health state to pending");
    CHECK(service_supervisor_report_ready(&supervisor, 2U, 11U,
                                          &published) ==
              SERVICE_SUPERVISOR_INVALID_OWNED_STATE && !published,
          "exited service cannot publish ready without owned process");
    CHECK(service_supervisor_report_liveness(&supervisor, 2U, 11U) ==
              SERVICE_SUPERVISOR_INVALID_OWNED_STATE,
          "exited service cannot report liveness without owned process");

    CHECK(service_supervisor_health_state_string(
              SERVICE_HEALTH_PENDING) != NULL &&
              strcmp(service_supervisor_health_state_string(
                         SERVICE_HEALTH_PENDING),
                     "SERVICE_HEALTH_PENDING") == 0 &&
              strcmp(service_supervisor_health_state_string(
                         SERVICE_HEALTH_READY),
                     "SERVICE_HEALTH_READY") == 0 &&
              strcmp(service_supervisor_health_state_string(
                         SERVICE_HEALTH_LIVENESS_FAILED),
                     "SERVICE_HEALTH_LIVENESS_FAILED") == 0 &&
              strcmp(service_supervisor_health_state_string(
                         SERVICE_HEALTH_READINESS_TIMEOUT),
                     "SERVICE_HEALTH_READINESS_TIMEOUT") == 0,
          "health state strings are exact for defined states");
    CHECK(service_supervisor_health_state_string(
              (enum service_health_state)4U) == NULL,
          "health state string rejects undefined state");
}

static void test_readiness_deadline_state(void) {
    struct service_supervisor supervisor;
    struct service_descriptor descriptor_a;
    struct service_descriptor descriptor_b;
    bool timed_out = false;
    bool published = false;
    uint32_t status = 0U;

    reset_all();
    memset(&supervisor, 0, sizeof(supervisor));
    CHECK(service_supervisor_set_readiness_deadline(
              NULL, 1U, 10U, 5U) == SERVICE_SUPERVISOR_NULL,
          "readiness deadline arm rejects null supervisor");
    CHECK(service_supervisor_check_readiness(
              NULL, 1U, 12U, &timed_out) == SERVICE_SUPERVISOR_NULL,
          "readiness check rejects null supervisor");
    CHECK(service_supervisor_check_readiness(
              &supervisor, 1U, 12U, NULL) == SERVICE_SUPERVISOR_NULL,
          "readiness check rejects null timed-out output");
    CHECK(service_supervisor_reap_at(
              NULL, 1U, 30U, &status) == SERVICE_SUPERVISOR_NULL,
          "readiness reap rejects null supervisor");
    CHECK(service_supervisor_reap_at(
              &supervisor, 1U, 30U, NULL) == SERVICE_SUPERVISOR_NULL,
          "readiness reap rejects null status output");
    CHECK(service_supervisor_set_readiness_deadline(
              &supervisor, 1U, 10U, 5U) == SERVICE_SUPERVISOR_BAD_STATE,
          "readiness deadline arm on uninitialized supervisor returns BAD_STATE");
    CHECK(service_supervisor_check_readiness(
              &supervisor, 1U, 12U, &timed_out) ==
              SERVICE_SUPERVISOR_BAD_STATE,
          "readiness check on uninitialized supervisor returns BAD_STATE");
    CHECK(service_supervisor_reap_at(
              &supervisor, 1U, 30U, &status) ==
              SERVICE_SUPERVISOR_BAD_STATE,
          "readiness reap on uninitialized supervisor returns BAD_STATE");
    CHECK(service_supervisor_readiness_deadline_tick(NULL, 1U) == 0U,
          "readiness deadline accessor tolerates null supervisor");

    CHECK(service_supervisor_init(&supervisor, TEST_HHDM) ==
              SERVICE_SUPERVISOR_OK,
          "readiness deadline fixture initializes supervisor");
    descriptor_a = make_descriptor(1U, true, 2U, "deadline-a");
    descriptor_b = make_descriptor(2U, true, 0U, "deadline-b");
    CHECK(service_supervisor_start(&supervisor, &descriptor_a,
                                   test_entry_a) ==
              SERVICE_SUPERVISOR_OK &&
              service_supervisor_start(&supervisor, &descriptor_b,
                                       test_entry_b) ==
                  SERVICE_SUPERVISOR_OK,
          "readiness deadline fixture starts two services");

    CHECK(service_supervisor_set_readiness_deadline(
              &supervisor, 3U, 10U, 5U) == SERVICE_SUPERVISOR_NOT_FOUND,
          "readiness deadline arm rejects unknown service");
    CHECK(service_supervisor_set_readiness_deadline(
              &supervisor, 1U, 10U, 0U) ==
              SERVICE_SUPERVISOR_BAD_READINESS &&
              supervisor.services[0].readiness_timeout_ticks == 0U &&
              supervisor.services[0].readiness_deadline_tick == 0U,
          "zero readiness timeout is rejected without publication");
    CHECK(service_supervisor_set_readiness_deadline(
              &supervisor, 1U, 10U,
              SERVICE_READINESS_TIMEOUT_MAX_TICKS + 1U) ==
              SERVICE_SUPERVISOR_BAD_READINESS &&
              supervisor.services[0].readiness_timeout_ticks == 0U &&
              supervisor.services[0].readiness_deadline_tick == 0U,
          "over-limit readiness timeout is rejected without publication");
    CHECK(service_supervisor_set_readiness_deadline(
              &supervisor, 1U, UINT64_MAX, 2U) ==
              SERVICE_SUPERVISOR_READINESS_OVERFLOW &&
              supervisor.services[0].readiness_timeout_ticks == 0U &&
              supervisor.services[0].readiness_deadline_tick == 0U,
          "readiness deadline overflow is rejected without publication");

    CHECK(service_supervisor_set_readiness_deadline(
              &supervisor, 1U, 10U, 5U) == SERVICE_SUPERVISOR_OK,
          "valid running pending service arms readiness deadline");
    CHECK(supervisor.services[0].readiness_start_tick == 10U &&
              supervisor.services[0].readiness_deadline_tick == 15U &&
              supervisor.services[0].readiness_timeout_ticks == 5U &&
              service_supervisor_readiness_deadline_tick(
                  &supervisor, 1U) == 15U,
          "readiness deadline arm publishes exact start, timeout, and deadline");
    CHECK(service_supervisor_set_readiness_deadline(
              &supervisor, 1U, 20U, 5U) == SERVICE_SUPERVISOR_BAD_STATE &&
              supervisor.services[0].readiness_start_tick == 10U &&
              supervisor.services[0].readiness_deadline_tick == 15U,
          "second readiness deadline arm is rejected without mutation");

    timed_out = true;
    CHECK(service_supervisor_check_readiness(
              &supervisor, 1U, 12U, &timed_out) ==
              SERVICE_SUPERVISOR_OK && !timed_out &&
              supervisor.services[0].health_state ==
                  SERVICE_HEALTH_PENDING,
          "readiness check within deadline remains pending");
    CHECK(service_supervisor_check_readiness(
              &supervisor, 1U, 9U, &timed_out) ==
              SERVICE_SUPERVISOR_BAD_READINESS &&
              supervisor.services[0].health_state ==
                  SERVICE_HEALTH_PENDING,
          "non-monotonic readiness check is rejected without mutation");
    CHECK(service_supervisor_check_readiness(
              &supervisor, 1U, 16U, &timed_out) ==
              SERVICE_SUPERVISOR_READINESS_TIMEOUT && timed_out &&
              supervisor.services[0].health_state ==
                  SERVICE_HEALTH_READINESS_TIMEOUT,
          "readiness check after deadline publishes timeout state");
    CHECK(service_supervisor_check_readiness(
              &supervisor, 1U, 17U, &timed_out) ==
              SERVICE_SUPERVISOR_READINESS_TIMEOUT && timed_out &&
              supervisor.services[0].health_state ==
                  SERVICE_HEALTH_READINESS_TIMEOUT,
          "readiness timeout state is permanent for current generation");

    published = true;
    CHECK(service_supervisor_report_ready(
              &supervisor, 1U, 17U, &published) ==
              SERVICE_SUPERVISOR_READINESS_TIMEOUT && !published &&
              supervisor.services[0].health_state ==
                  SERVICE_HEALTH_READINESS_TIMEOUT &&
              supervisor.services[0].liveness_generation == 0U,
          "late readiness cannot resurrect timed-out generation");
    CHECK(service_supervisor_report_liveness(
              &supervisor, 1U, 17U) ==
              SERVICE_SUPERVISOR_READINESS_TIMEOUT &&
              supervisor.services[0].liveness_generation == 0U,
          "late liveness cannot advance timed-out generation");
    CHECK(service_supervisor_check_liveness(
              &supervisor, 1U, 17U, 10U, &timed_out) ==
              SERVICE_SUPERVISOR_READINESS_TIMEOUT && timed_out,
          "liveness check rejects readiness-timed-out generation");

    published = true;
    CHECK(service_supervisor_set_readiness_deadline(
              &supervisor, 2U, 20U, 5U) == SERVICE_SUPERVISOR_OK &&
              service_supervisor_report_ready(
                  &supervisor, 2U, 26U, &published) ==
                  SERVICE_SUPERVISOR_READINESS_TIMEOUT && !published &&
              supervisor.services[1].health_state ==
                  SERVICE_HEALTH_READINESS_TIMEOUT &&
              supervisor.services[1].readiness_deadline_tick == 25U,
          "late ready report itself publishes timeout without prior check");
    CHECK(service_supervisor_report_liveness(
              &supervisor, 2U, 27U) ==
              SERVICE_SUPERVISOR_READINESS_TIMEOUT,
          "readiness-timed-out service cannot report liveness");
    CHECK(service_supervisor_set_readiness_deadline(
              &supervisor, 2U, 30U, 5U) == SERVICE_SUPERVISOR_BAD_STATE,
          "readiness deadline cannot be re-armed for timed-out generation");

    CHECK(service_supervisor_exit(&supervisor, 1U, 0U) ==
              SERVICE_SUPERVISOR_OK &&
              supervisor.services[0].health_state ==
                  SERVICE_HEALTH_PENDING &&
              supervisor.services[0].readiness_start_tick == 0U &&
              supervisor.services[0].readiness_deadline_tick == 0U &&
              supervisor.services[0].readiness_timeout_ticks == 5U,
          "exit resets active deadline but preserves timeout for restart");
    CHECK(service_supervisor_reap_at(
              &supervisor, 1U, 30U, &status) ==
              SERVICE_SUPERVISOR_OK && status == 0U &&
              supervisor.services[0].state ==
                  SERVICE_SUPERVISOR_STATE_RUNNING &&
              supervisor.services[0].readiness_start_tick == 30U &&
              supervisor.services[0].readiness_deadline_tick == 35U &&
              supervisor.services[0].readiness_timeout_ticks == 5U,
          "now-aware reap re-arms readiness deadline for replacement");
    published = false;
    CHECK(service_supervisor_report_ready(
              &supervisor, 1U, 34U, &published) ==
              SERVICE_SUPERVISOR_OK && published &&
              supervisor.services[0].health_state ==
                  SERVICE_HEALTH_READY,
          "replacement generation can publish ready within fresh deadline");

    CHECK(service_supervisor_exit(&supervisor, 1U, 4U) ==
              SERVICE_SUPERVISOR_OK &&
              service_supervisor_reap(
                  &supervisor, 1U, &status) ==
                  SERVICE_SUPERVISOR_OK && status == 4U &&
              supervisor.services[0].state ==
                  SERVICE_SUPERVISOR_STATE_RUNNING &&
              supervisor.services[0].readiness_timeout_ticks == 0U &&
              supervisor.services[0].readiness_deadline_tick == 0U,
          "legacy reap clears deadline state without a new tick source");
    published = false;
    CHECK(service_supervisor_report_ready(
              &supervisor, 1U, 35U, &published) ==
              SERVICE_SUPERVISOR_OK && published,
          "legacy restarted service can publish ready without stale deadline");

    CHECK(service_supervisor_exit(&supervisor, 1U, 3U) ==
              SERVICE_SUPERVISOR_OK &&
              service_supervisor_reap_at(
                  &supervisor, 1U, 40U, &status) ==
                  SERVICE_SUPERVISOR_OK && status == 3U &&
              supervisor.services[0].state ==
                  SERVICE_SUPERVISOR_STATE_EXHAUSTED &&
              supervisor.services[0].process == NULL &&
              supervisor.services[0].readiness_start_tick == 0U &&
              supervisor.services[0].readiness_deadline_tick == 0U &&
              supervisor.services[0].readiness_timeout_ticks == 0U,
          "readiness deadline exhaustion leaves no process or stale deadline");
    CHECK(supervisor.supervisor_pcb.child_count == 1U &&
              heap_allocs == 4U && heap_frees == 3U &&
              stack_allocs == 4U && stack_frees == 3U &&
              as_allocs == 4U && as_frees == 3U,
          "readiness deadline lifecycle leaks no process resources");
}

static void test_health_reset_on_restart_and_exhaustion(void) {
    struct service_supervisor supervisor;
    struct service_descriptor descriptor;
    uint32_t status = 0U;
    bool published = false;

    reset_all();
    memset(&supervisor, 0, sizeof(supervisor));
    CHECK(service_supervisor_init(&supervisor, TEST_HHDM) ==
              SERVICE_SUPERVISOR_OK,
          "health reset fixture initializes supervisor");
    descriptor = make_descriptor(1U, true, 1U, "restart-health");
    CHECK(service_supervisor_start(&supervisor, &descriptor,
                                   test_entry_a) ==
              SERVICE_SUPERVISOR_OK &&
              service_supervisor_report_ready(&supervisor, 1U, 5U,
                                              &published) ==
                  SERVICE_SUPERVISOR_OK && published,
          "health reset fixture starts and readies service");

    CHECK(service_supervisor_exit(&supervisor, 1U, 3U) ==
              SERVICE_SUPERVISOR_OK &&
              supervisor.services[0].health_state ==
                  SERVICE_HEALTH_PENDING &&
              supervisor.services[0].liveness_generation == 0U,
          "exit before restart resets health state");
    CHECK(service_supervisor_reap(&supervisor, 1U, &status) ==
              SERVICE_SUPERVISOR_OK && status == 3U &&
              supervisor.services[0].state ==
                  SERVICE_SUPERVISOR_STATE_RUNNING &&
              supervisor.services[0].health_state ==
                  SERVICE_HEALTH_PENDING &&
              supervisor.services[0].liveness_generation == 0U,
          "automatic restart publishes a pending replacement process");

    published = false;
    CHECK(service_supervisor_report_ready(&supervisor, 1U, 6U,
                                          &published) ==
              SERVICE_SUPERVISOR_OK && published,
          "replacement process can publish ready after restart");
    CHECK(service_supervisor_exit(&supervisor, 1U, 9U) ==
              SERVICE_SUPERVISOR_OK &&
              service_supervisor_reap(&supervisor, 1U, &status) ==
                  SERVICE_SUPERVISOR_OK && status == 9U &&
              supervisor.services[0].state ==
                  SERVICE_SUPERVISOR_STATE_EXHAUSTED &&
              supervisor.services[0].process == NULL &&
              supervisor.services[0].health_state ==
                  SERVICE_HEALTH_PENDING &&
              supervisor.services[0].liveness_generation == 0U,
          "restart exhaustion leaves pending health and no process");
}

static void test_ordered_start_input_validation(void) {
    struct service_supervisor supervisor;
    struct service_graph_snapshot snapshot = {0};
    struct service_graph_snapshot unpublished;
    struct service_supervisor_entry entries[2];
    struct service_supervisor_entry wrong[1];
    struct service_descriptor descriptors[1];

    reset_all();
    descriptors[0] = make_descriptor(1U, true, 0U, "ordered-a");
    CHECK(service_graph_snapshot_build(&snapshot, descriptors, 1U) ==
              SERVICE_GRAPH_OK,
          "ordered start fixture builds enabled snapshot");
    entries[0].service_id = 1U;
    entries[0].entry = test_entry_a;
    entries[1].service_id = 1U;
    entries[1].entry = test_entry_b;

    CHECK(service_supervisor_start_ordered(NULL, &snapshot, entries, 1U) ==
              SERVICE_SUPERVISOR_NULL,
          "ordered start rejects null supervisor");
    memset(&supervisor, 0, sizeof(supervisor));
    CHECK(service_supervisor_start_ordered(&supervisor, &snapshot,
                                           entries, 1U) ==
              SERVICE_SUPERVISOR_BAD_STATE,
          "ordered start on uninitialized supervisor returns BAD_STATE");
    CHECK(service_supervisor_init(&supervisor, TEST_HHDM) ==
              SERVICE_SUPERVISOR_OK,
          "ordered start validation initializes supervisor");
    CHECK(service_supervisor_start_ordered(&supervisor, NULL,
                                           entries, 1U) ==
              SERVICE_SUPERVISOR_NULL,
          "ordered start rejects null snapshot");

    memset(&unpublished, 0, sizeof(unpublished));
    CHECK(service_supervisor_start_ordered(&supervisor, &unpublished,
                                           NULL, 0U) ==
              SERVICE_SUPERVISOR_BAD_STATE,
          "ordered start rejects unpublished snapshot");
    CHECK(service_supervisor_start_ordered(&supervisor, &snapshot,
                                           NULL, 1U) ==
              SERVICE_SUPERVISOR_NULL,
          "ordered start rejects null entries for enabled service");
    CHECK(service_supervisor_start_ordered(
              &supervisor, &snapshot, entries,
              SERVICE_SUPERVISOR_ENTRY_MAX + 1U) ==
              SERVICE_SUPERVISOR_BAD_STATE,
          "ordered start rejects over-limit entry count");

    entries[0].entry = NULL;
    CHECK(service_supervisor_start_ordered(&supervisor, &snapshot,
                                           entries, 1U) ==
              SERVICE_SUPERVISOR_NULL,
          "ordered start rejects null entry function");
    entries[0].entry = test_entry_a;
    CHECK(service_supervisor_start_ordered(&supervisor, &snapshot,
                                           entries, 2U) ==
              SERVICE_SUPERVISOR_BAD_STATE,
          "ordered start rejects duplicate entry ids");
    wrong[0].service_id = 2U;
    wrong[0].entry = test_entry_b;
    CHECK(service_supervisor_start_ordered(&supervisor, &snapshot,
                                           wrong, 1U) ==
              SERVICE_SUPERVISOR_BAD_STATE,
          "ordered start rejects missing enabled entry");

    memset(&snapshot, 0, sizeof(snapshot));
    descriptors[0] = make_descriptor(1U, false, 0U, "disabled-a");
    CHECK(service_graph_snapshot_build(&snapshot, descriptors, 1U) ==
              SERVICE_GRAPH_OK,
          "ordered start fixture builds disabled snapshot");
    CHECK(service_supervisor_start_ordered(&supervisor, &snapshot,
                                           entries, 1U) ==
              SERVICE_SUPERVISOR_BAD_STATE,
          "ordered start rejects entry for disabled service");
    CHECK(supervisor.active_count == 0U &&
              heap_allocs == 0U && stack_allocs == 0U &&
              as_allocs == 0U,
          "ordered start validation publishes no process resources");
}

static void test_ordered_start_empty_and_all_disabled(void) {
    struct service_supervisor supervisor;
    struct service_graph_snapshot snapshot = {0};
    struct service_descriptor descriptors[1];

    reset_all();
    memset(&supervisor, 0, sizeof(supervisor));
    CHECK(service_supervisor_init(&supervisor, TEST_HHDM) ==
              SERVICE_SUPERVISOR_OK,
          "empty ordered start initializes supervisor");
    CHECK(service_graph_snapshot_build(&snapshot, NULL, 0U) ==
              SERVICE_GRAPH_OK,
          "empty ordered start builds empty snapshot");
    CHECK(service_supervisor_start_ordered(&supervisor, &snapshot,
                                           NULL, 0U) ==
              SERVICE_SUPERVISOR_OK &&
              supervisor.active_count == 0U,
          "empty snapshot ordered start publishes nothing");

    memset(&snapshot, 0, sizeof(snapshot));
    descriptors[0] = make_descriptor(1U, false, 0U, "disabled-a");
    CHECK(service_graph_snapshot_build(&snapshot, descriptors, 1U) ==
              SERVICE_GRAPH_OK,
          "all-disabled ordered start builds snapshot");
    CHECK(service_supervisor_start_ordered(&supervisor, &snapshot,
                                           NULL, 0U) ==
              SERVICE_SUPERVISOR_OK &&
              supervisor.active_count == 0U,
          "all-disabled ordered start publishes nothing");
    CHECK(heap_allocs == 0U && stack_allocs == 0U && as_allocs == 0U,
          "empty and all-disabled starts allocate no process resources");
}

static void test_ordered_start_chain_and_ownership(void) {
    struct service_supervisor supervisor;
    struct service_graph_snapshot snapshot = {0};
    struct service_supervisor_entry entries[3];
    struct service_descriptor descriptors[3];
    uint8_t dep_one[1] = {1U};
    uint8_t dep_two[1] = {2U};

    reset_all();
    memset(&supervisor, 0, sizeof(supervisor));
    CHECK(service_supervisor_init(&supervisor, TEST_HHDM) ==
              SERVICE_SUPERVISOR_OK,
          "ordered chain fixture initializes supervisor");
    descriptors[0] = make_descriptor_with_deps(
        1U, true, 0U, "ordered-a", NULL, 0U);
    descriptors[1] = make_descriptor_with_deps(
        2U, true, 0U, "ordered-b", dep_one, 1U);
    descriptors[2] = make_descriptor_with_deps(
        3U, true, 0U, "ordered-c", dep_two, 1U);
    CHECK(service_graph_snapshot_build(&snapshot, descriptors, 3U) ==
              SERVICE_GRAPH_OK,
          "ordered chain fixture builds snapshot");

    entries[0].service_id = 3U;
    entries[0].entry = test_entry_b;
    entries[1].service_id = 1U;
    entries[1].entry = test_entry_a;
    entries[2].service_id = 2U;
    entries[2].entry = test_entry_b;
    CHECK(service_supervisor_start_ordered(&supervisor, &snapshot,
                                           entries, 3U) ==
              SERVICE_SUPERVISOR_OK,
          "ordered chain starts all services");
    CHECK(supervisor.active_count == 3U &&
              supervisor.services[0].descriptor.id == 1U &&
              supervisor.services[1].descriptor.id == 2U &&
              supervisor.services[2].descriptor.id == 3U,
          "ordered chain publishes snapshot start order");
    CHECK(supervisor.supervisor_pcb.child_count == 3U &&
              heap_allocs == 3U && stack_allocs == 3U &&
              as_allocs == 3U && heap_frees == 0U &&
              stack_frees == 0U && as_frees == 0U,
          "ordered chain owns one process per service");

    CHECK(service_supervisor_start_ordered(&supervisor, &snapshot,
                                           entries, 3U) ==
              SERVICE_SUPERVISOR_OK &&
              supervisor.active_count == 3U &&
              heap_allocs == 3U && stack_allocs == 3U &&
              as_allocs == 3U,
          "already running ordered chain is idempotent");
}

static void test_ordered_start_already_running_and_mismatch(void) {
    struct service_supervisor supervisor;
    struct service_graph_snapshot snapshot = {0};
    struct service_supervisor_entry entries[2];
    struct service_descriptor descriptors[2];
    struct kernel_process *service_one_process;
    uint8_t dep_one[1] = {1U};

    reset_all();
    memset(&supervisor, 0, sizeof(supervisor));
    CHECK(service_supervisor_init(&supervisor, TEST_HHDM) ==
              SERVICE_SUPERVISOR_OK,
          "already-running fixture initializes supervisor");
    descriptors[0] = make_descriptor(1U, true, 0U, "base-a");
    CHECK(service_supervisor_start(&supervisor, &descriptors[0],
                                   test_entry_a) ==
              SERVICE_SUPERVISOR_OK,
          "already-running fixture starts base service");
    service_one_process = supervisor.services[0].process;
    descriptors[1] = make_descriptor_with_deps(
        2U, true, 0U, "ordered-b", dep_one, 1U);
    CHECK(service_graph_snapshot_build(&snapshot, descriptors, 2U) ==
              SERVICE_GRAPH_OK,
          "already-running fixture builds snapshot");
    entries[0].service_id = 1U;
    entries[0].entry = test_entry_a;
    entries[1].service_id = 2U;
    entries[1].entry = test_entry_b;
    CHECK(service_supervisor_start_ordered(&supervisor, &snapshot,
                                           entries, 2U) ==
              SERVICE_SUPERVISOR_OK &&
              supervisor.active_count == 2U &&
              supervisor.services[0].process == service_one_process &&
              supervisor.services[1].descriptor.id == 2U,
          "already-running dependency is reused without duplicate process");
    CHECK(heap_allocs == 2U && stack_allocs == 2U && as_allocs == 2U,
          "already-running ordered start adds only the dependent process");

    reset_all();
    memset(&supervisor, 0, sizeof(supervisor));
    CHECK(service_supervisor_init(&supervisor, TEST_HHDM) ==
              SERVICE_SUPERVISOR_OK,
          "descriptor mismatch fixture initializes supervisor");
    descriptors[0] = make_descriptor(1U, true, 0U, "base-a");
    CHECK(service_supervisor_start(&supervisor, &descriptors[0],
                                   test_entry_a) ==
              SERVICE_SUPERVISOR_OK,
          "descriptor mismatch fixture starts base service");
    memset(&snapshot, 0, sizeof(snapshot));
    descriptors[0] = make_descriptor(1U, true, 0U, "changed-a");
    descriptors[1] = make_descriptor_with_deps(
        2U, true, 0U, "ordered-b", dep_one, 1U);
    CHECK(service_graph_snapshot_build(&snapshot, descriptors, 2U) ==
              SERVICE_GRAPH_OK,
          "descriptor mismatch fixture builds snapshot");
    CHECK(service_supervisor_start_ordered(&supervisor, &snapshot,
                                           entries, 2U) ==
              SERVICE_SUPERVISOR_BAD_STATE &&
              supervisor.active_count == 1U &&
              supervisor.services[1].state ==
                  SERVICE_SUPERVISOR_STATE_STOPPED,
          "running descriptor mismatch blocks ordered start");
    CHECK(heap_allocs == 1U && stack_allocs == 1U && as_allocs == 1U,
          "descriptor mismatch publishes no dependent process");
}

static void test_ordered_start_disabled_predecessor_blocks_dependent(void) {
    struct service_supervisor supervisor;
    struct service_graph_snapshot snapshot = {0};
    struct service_supervisor_entry entries[2];
    struct service_descriptor descriptors[3];
    uint8_t dep_one[1] = {1U};
    uint8_t dep_two[1] = {2U};

    reset_all();
    memset(&supervisor, 0, sizeof(supervisor));
    CHECK(service_supervisor_init(&supervisor, TEST_HHDM) ==
              SERVICE_SUPERVISOR_OK,
          "disabled predecessor fixture initializes supervisor");
    descriptors[0] = make_descriptor_with_deps(
        1U, false, 0U, "disabled-a", NULL, 0U);
    descriptors[1] = make_descriptor_with_deps(
        2U, true, 0U, "ordered-b", dep_one, 1U);
    descriptors[2] = make_descriptor_with_deps(
        3U, true, 0U, "ordered-c", dep_two, 1U);
    CHECK(service_graph_snapshot_build(&snapshot, descriptors, 3U) ==
              SERVICE_GRAPH_OK,
          "disabled predecessor fixture builds snapshot");
    entries[0].service_id = 2U;
    entries[0].entry = test_entry_b;
    entries[1].service_id = 3U;
    entries[1].entry = test_entry_b;
    CHECK(service_supervisor_start_ordered(&supervisor, &snapshot,
                                           entries, 2U) ==
              SERVICE_SUPERVISOR_DISABLED,
          "disabled predecessor prevents dependent start");
    CHECK(supervisor.active_count == 0U &&
              heap_allocs == 0U && stack_allocs == 0U &&
              as_allocs == 0U,
          "disabled predecessor publishes no dependent process");
}

static void test_ordered_start_process_failure_blocks_dependent(void) {
    struct service_supervisor supervisor;
    struct service_graph_snapshot snapshot = {0};
    struct service_supervisor_entry entries[3];
    struct service_descriptor descriptors[3];
    uint8_t dep_one[1] = {1U};
    uint8_t dep_two[1] = {2U};

    reset_all();
    memset(&supervisor, 0, sizeof(supervisor));
    CHECK(service_supervisor_init(&supervisor, TEST_HHDM) ==
              SERVICE_SUPERVISOR_OK,
          "process failure fixture initializes supervisor");
    descriptors[0] = make_descriptor_with_deps(
        1U, true, 0U, "ordered-a", NULL, 0U);
    descriptors[1] = make_descriptor_with_deps(
        2U, true, 0U, "ordered-b", dep_one, 1U);
    descriptors[2] = make_descriptor_with_deps(
        3U, true, 0U, "ordered-c", dep_two, 1U);
    CHECK(service_graph_snapshot_build(&snapshot, descriptors, 3U) ==
              SERVICE_GRAPH_OK,
          "process failure fixture builds snapshot");
    entries[0].service_id = 1U;
    entries[0].entry = test_entry_a;
    entries[1].service_id = 2U;
    entries[1].entry = test_entry_b;
    entries[2].service_id = 3U;
    entries[2].entry = test_entry_b;

    heap_fail = true;
    CHECK(service_supervisor_start_ordered(&supervisor, &snapshot,
                                           entries, 3U) ==
              SERVICE_SUPERVISOR_PROCESS_FAILED,
          "predecessor process failure stops ordered start");
    CHECK(supervisor.active_count == 0U &&
              heap_allocs == 0U && stack_allocs == 0U &&
              as_allocs == 0U,
          "predecessor process failure starts no dependent");
}

static void test_ordered_start_exited_and_exhausted_blocks(void) {
    struct service_supervisor supervisor;
    struct service_graph_snapshot snapshot = {0};
    struct service_supervisor_entry entries[2];
    struct service_descriptor descriptors[2];
    uint8_t dep_one[1] = {1U};
    uint32_t status = 0U;

    reset_all();
    memset(&supervisor, 0, sizeof(supervisor));
    CHECK(service_supervisor_init(&supervisor, TEST_HHDM) ==
              SERVICE_SUPERVISOR_OK,
          "exited ordered fixture initializes supervisor");
    descriptors[0] = make_descriptor(1U, true, 0U, "exited-a");
    CHECK(service_supervisor_start(&supervisor, &descriptors[0],
                                   test_entry_a) ==
              SERVICE_SUPERVISOR_OK &&
              service_supervisor_exit(&supervisor, 1U, 0U) ==
                  SERVICE_SUPERVISOR_OK,
          "exited ordered fixture starts and exits service");
    descriptors[1] = make_descriptor_with_deps(
        2U, true, 0U, "ordered-b", dep_one, 1U);
    CHECK(service_graph_snapshot_build(&snapshot, descriptors, 2U) ==
              SERVICE_GRAPH_OK,
          "exited ordered fixture builds snapshot");
    entries[0].service_id = 1U;
    entries[0].entry = test_entry_a;
    entries[1].service_id = 2U;
    entries[1].entry = test_entry_b;
    CHECK(service_supervisor_start_ordered(&supervisor, &snapshot,
                                           entries, 2U) ==
              SERVICE_SUPERVISOR_BAD_STATE &&
              supervisor.services[1].state ==
                  SERVICE_SUPERVISOR_STATE_STOPPED &&
              heap_allocs == 1U && stack_allocs == 1U &&
              as_allocs == 1U,
          "exited service blocks dependent ordered start");

    reset_all();
    memset(&supervisor, 0, sizeof(supervisor));
    CHECK(service_supervisor_init(&supervisor, TEST_HHDM) ==
              SERVICE_SUPERVISOR_OK,
          "exhausted ordered fixture initializes supervisor");
    descriptors[0] = make_descriptor(1U, true, 0U, "exhausted-a");
    CHECK(service_supervisor_start(&supervisor, &descriptors[0],
                                   test_entry_a) ==
              SERVICE_SUPERVISOR_OK &&
              service_supervisor_exit(&supervisor, 1U, 0U) ==
                  SERVICE_SUPERVISOR_OK &&
              service_supervisor_reap(&supervisor, 1U, &status) ==
                  SERVICE_SUPERVISOR_OK,
          "exhausted ordered fixture exhausts service");
    memset(&snapshot, 0, sizeof(snapshot));
    descriptors[1] = make_descriptor_with_deps(
        2U, true, 0U, "ordered-b", dep_one, 1U);
    CHECK(service_graph_snapshot_build(&snapshot, descriptors, 2U) ==
              SERVICE_GRAPH_OK,
          "exhausted ordered fixture builds snapshot");
    CHECK(service_supervisor_start_ordered(&supervisor, &snapshot,
                                           entries, 2U) ==
              SERVICE_SUPERVISOR_EXHAUSTED &&
              supervisor.services[1].state ==
                  SERVICE_SUPERVISOR_STATE_STOPPED &&
              supervisor.services[0].process == NULL,
          "exhausted service blocks dependent ordered start");
    CHECK(heap_allocs == 1U && heap_frees == 1U &&
              stack_allocs == 1U && stack_frees == 1U &&
              as_allocs == 1U && as_frees == 1U,
          "exhausted ordered start publishes no dependent process");
}

static void test_ordered_stop_input_validation(void) {
    struct service_supervisor supervisor;
    struct service_graph_snapshot snapshot = {0};
    struct service_graph_snapshot unpublished = {0};
    struct service_descriptor descriptors[1];

    reset_all();
    descriptors[0] = make_descriptor(1U, true, 0U, "stop-a");
    CHECK(service_graph_snapshot_build(&snapshot, descriptors, 1U) ==
              SERVICE_GRAPH_OK,
          "ordered stop fixture builds enabled snapshot");

    memset(&supervisor, 0, sizeof(supervisor));
    CHECK(service_supervisor_stop_ordered(NULL, &snapshot) ==
              SERVICE_SUPERVISOR_NULL,
          "ordered stop rejects null supervisor");
    CHECK(service_supervisor_stop_ordered(&supervisor, NULL) ==
              SERVICE_SUPERVISOR_NULL,
          "ordered stop rejects null snapshot");
    CHECK(service_supervisor_stop_ordered(&supervisor, &snapshot) ==
              SERVICE_SUPERVISOR_BAD_STATE,
          "ordered stop on uninitialized supervisor returns BAD_STATE");
    CHECK(service_supervisor_init(&supervisor, TEST_HHDM) ==
              SERVICE_SUPERVISOR_OK,
          "ordered stop validation initializes supervisor");
    CHECK(service_supervisor_stop_ordered(&supervisor, &unpublished) ==
              SERVICE_SUPERVISOR_BAD_STATE,
          "ordered stop rejects unpublished snapshot");
    CHECK(supervisor.active_count == 0U &&
              heap_allocs == 0U && stack_allocs == 0U &&
              as_allocs == 0U,
          "ordered stop validation performs no teardown");
}

static void test_ordered_stop_empty_and_all_disabled(void) {
    struct service_supervisor supervisor;
    struct service_graph_snapshot snapshot = {0};
    struct service_descriptor descriptors[1];

    reset_all();
    memset(&supervisor, 0, sizeof(supervisor));
    CHECK(service_supervisor_init(&supervisor, TEST_HHDM) ==
              SERVICE_SUPERVISOR_OK,
          "empty ordered stop initializes supervisor");
    CHECK(service_graph_snapshot_build(&snapshot, NULL, 0U) ==
              SERVICE_GRAPH_OK,
          "empty ordered stop builds empty snapshot");
    CHECK(service_supervisor_stop_ordered(&supervisor, &snapshot) ==
              SERVICE_SUPERVISOR_OK &&
              supervisor.active_count == 0U,
          "empty snapshot ordered stop publishes nothing");

    memset(&snapshot, 0, sizeof(snapshot));
    descriptors[0] = make_descriptor(1U, false, 0U, "disabled-stop");
    CHECK(service_graph_snapshot_build(&snapshot, descriptors, 1U) ==
              SERVICE_GRAPH_OK,
          "all-disabled ordered stop builds snapshot");
    CHECK(service_supervisor_stop_ordered(&supervisor, &snapshot) ==
              SERVICE_SUPERVISOR_OK &&
              supervisor.active_count == 0U,
          "all-disabled ordered stop publishes nothing");
    CHECK(heap_allocs == 0U && stack_allocs == 0U && as_allocs == 0U,
          "empty and all-disabled stops allocate no process resources");
}

static void test_ordered_stop_reverse_order_and_ownership(void) {
    struct service_supervisor supervisor;
    struct service_graph_snapshot snapshot = {0};
    struct service_supervisor_entry entries[3];
    struct service_descriptor descriptors[3];
    const uint32_t *stop_ids;
    uint8_t dep_one[1] = {1U};
    uint8_t dep_two[1] = {2U};

    reset_all();
    memset(&supervisor, 0, sizeof(supervisor));
    CHECK(service_supervisor_init(&supervisor, TEST_HHDM) ==
              SERVICE_SUPERVISOR_OK,
          "reverse stop fixture initializes supervisor");
    descriptors[0] = make_descriptor_with_deps(
        1U, true, 0U, "stop-a", NULL, 0U);
    descriptors[1] = make_descriptor_with_deps(
        2U, true, 0U, "stop-b", dep_one, 1U);
    descriptors[2] = make_descriptor_with_deps(
        3U, true, 0U, "stop-c", dep_two, 1U);
    CHECK(service_graph_snapshot_build(&snapshot, descriptors, 3U) ==
              SERVICE_GRAPH_OK,
          "reverse stop fixture builds snapshot");
    entries[0].service_id = 1U;
    entries[0].entry = test_entry_a;
    entries[1].service_id = 2U;
    entries[1].entry = test_entry_b;
    entries[2].service_id = 3U;
    entries[2].entry = test_entry_b;
    CHECK(service_supervisor_start_ordered(&supervisor, &snapshot,
                                           entries, 3U) ==
              SERVICE_SUPERVISOR_OK,
          "reverse stop fixture starts all services");

    service_supervisor_host_test_reset();
    CHECK(service_supervisor_stop_ordered(&supervisor, &snapshot) ==
              SERVICE_SUPERVISOR_OK,
          "ordered stop stops the running chain");
    stop_ids = service_supervisor_host_test_stop_ids();
    CHECK(service_supervisor_host_test_stop_count() == 3U &&
              stop_ids[0] == 3U && stop_ids[1] == 2U &&
              stop_ids[2] == 1U,
          "ordered stop publishes exact reverse dependency order");
    CHECK(supervisor.active_count == 0U &&
              supervisor.supervisor_pcb.child_count == 0U &&
              supervisor.services[0].state ==
                  SERVICE_SUPERVISOR_STATE_STOPPED &&
              supervisor.services[1].state ==
                  SERVICE_SUPERVISOR_STATE_STOPPED &&
              supervisor.services[2].state ==
                  SERVICE_SUPERVISOR_STATE_STOPPED &&
              supervisor.services[0].process == NULL &&
              supervisor.services[1].process == NULL &&
              supervisor.services[2].process == NULL,
          "ordered stop publishes exact stopped slots");
    CHECK(supervisor.services[0].health_state == SERVICE_HEALTH_PENDING &&
              supervisor.services[1].health_state == SERVICE_HEALTH_PENDING &&
              supervisor.services[2].health_state == SERVICE_HEALTH_PENDING,
          "ordered stop resets health state for every stopped service");
    CHECK(heap_allocs == 3U && heap_frees == 3U &&
              stack_allocs == 3U && stack_frees == 3U &&
              as_allocs == 3U && as_frees == 3U,
          "ordered stop frees each process resource exactly once");
}

static void test_ordered_stop_suppresses_automatic_restart(void) {
    struct service_supervisor supervisor;
    struct service_graph_snapshot snapshot = {0};
    struct service_supervisor_entry entry;
    struct service_descriptor descriptors[1];

    reset_all();
    memset(&supervisor, 0, sizeof(supervisor));
    CHECK(service_supervisor_init(&supervisor, TEST_HHDM) ==
              SERVICE_SUPERVISOR_OK,
          "restart suppression fixture initializes supervisor");
    descriptors[0] = make_descriptor(1U, true, 3U, "restart-stop");
    CHECK(service_graph_snapshot_build(&snapshot, descriptors, 1U) ==
              SERVICE_GRAPH_OK,
          "restart suppression fixture builds snapshot");
    entry.service_id = 1U;
    entry.entry = test_entry_a;
    CHECK(service_supervisor_start_ordered(&supervisor, &snapshot,
                                           &entry, 1U) ==
              SERVICE_SUPERVISOR_OK,
          "restart suppression fixture starts service");

    CHECK(service_supervisor_stop_ordered(&supervisor, &snapshot) ==
              SERVICE_SUPERVISOR_OK,
          "ordered stop stops restartable service");
    CHECK(supervisor.services[0].state ==
              SERVICE_SUPERVISOR_STATE_STOPPED &&
              supervisor.services[0].process == NULL &&
              supervisor.services[0].restart_count == 0U &&
              supervisor.supervisor_pcb.child_count == 0U,
          "ordered stop does not consume restart budget or restart service");
    CHECK(heap_allocs == 1U && heap_frees == 1U &&
              stack_allocs == 1U && stack_frees == 1U &&
              as_allocs == 1U && as_frees == 1U,
          "restart suppression leaves no live replacement process");
}

static void test_ordered_stop_unrelated_service_untouched(void) {
    struct service_supervisor supervisor;
    struct service_graph_snapshot snapshot = {0};
    struct service_descriptor descriptors[2];
    struct kernel_process *unrelated_process;

    reset_all();
    memset(&supervisor, 0, sizeof(supervisor));
    CHECK(service_supervisor_init(&supervisor, TEST_HHDM) ==
              SERVICE_SUPERVISOR_OK,
          "unrelated fixture initializes supervisor");
    descriptors[0] = make_descriptor(1U, true, 0U, "unrelated-a");
    descriptors[1] = make_descriptor(2U, true, 0U, "stop-b");
    CHECK(service_supervisor_start(&supervisor, &descriptors[0],
                                   test_entry_a) ==
              SERVICE_SUPERVISOR_OK &&
              service_supervisor_start(&supervisor, &descriptors[1],
                                       test_entry_b) ==
                  SERVICE_SUPERVISOR_OK,
          "unrelated fixture starts snapshot and unrelated services");
    unrelated_process = supervisor.services[0].process;
    CHECK(service_graph_snapshot_build(&snapshot, &descriptors[1], 1U) ==
              SERVICE_GRAPH_OK,
          "unrelated fixture builds stop snapshot");

    CHECK(service_supervisor_stop_ordered(&supervisor, &snapshot) ==
              SERVICE_SUPERVISOR_OK,
          "ordered stop stops only snapshot services");
    CHECK(supervisor.active_count == 1U &&
              supervisor.services[0].state ==
                  SERVICE_SUPERVISOR_STATE_RUNNING &&
              supervisor.services[0].process == unrelated_process &&
              supervisor.services[1].state ==
                  SERVICE_SUPERVISOR_STATE_STOPPED &&
              supervisor.services[1].process == NULL,
          "ordered stop leaves unrelated service owned and running");
    CHECK(heap_allocs == 2U && heap_frees == 1U &&
              stack_allocs == 2U && stack_frees == 1U &&
              as_allocs == 2U && as_frees == 1U,
          "ordered stop frees only the snapshot service resources");
}

static void test_ordered_stop_failure_blocks_dependent_stop(void) {
    struct service_supervisor supervisor;
    struct service_graph_snapshot snapshot = {0};
    struct service_supervisor_entry entries[3];
    struct service_descriptor descriptors[3];
    const uint32_t *stop_ids;
    uint8_t dep_one[1] = {1U};
    uint8_t dep_two[1] = {2U};

    reset_all();
    memset(&supervisor, 0, sizeof(supervisor));
    CHECK(service_supervisor_init(&supervisor, TEST_HHDM) ==
              SERVICE_SUPERVISOR_OK,
          "failure fixture initializes supervisor");
    descriptors[0] = make_descriptor_with_deps(
        1U, true, 0U, "stop-a", NULL, 0U);
    descriptors[1] = make_descriptor_with_deps(
        2U, true, 0U, "stop-b", dep_one, 1U);
    descriptors[2] = make_descriptor_with_deps(
        3U, true, 0U, "stop-c", dep_two, 1U);
    CHECK(service_graph_snapshot_build(&snapshot, descriptors, 3U) ==
              SERVICE_GRAPH_OK,
          "failure fixture builds snapshot");
    entries[0].service_id = 1U;
    entries[0].entry = test_entry_a;
    entries[1].service_id = 2U;
    entries[1].entry = test_entry_b;
    entries[2].service_id = 3U;
    entries[2].entry = test_entry_b;
    CHECK(service_supervisor_start_ordered(&supervisor, &snapshot,
                                           entries, 3U) ==
              SERVICE_SUPERVISOR_OK,
          "failure fixture starts all services");

    supervisor.services[1].process = NULL;
    service_supervisor_host_test_reset();
    CHECK(service_supervisor_stop_ordered(&supervisor, &snapshot) ==
              SERVICE_SUPERVISOR_INVALID_OWNED_STATE,
          "invalid owned dependent stops ordered shutdown");
    stop_ids = service_supervisor_host_test_stop_ids();
    CHECK(service_supervisor_host_test_stop_count() == 1U &&
              stop_ids[0] == 3U,
          "failure fixture stops dependent before failing");
    CHECK(supervisor.services[0].state ==
              SERVICE_SUPERVISOR_STATE_RUNNING &&
              supervisor.services[0].process != NULL &&
              supervisor.services[1].state ==
                  SERVICE_SUPERVISOR_STATE_RUNNING &&
              supervisor.services[1].process == NULL &&
              supervisor.services[2].state ==
                  SERVICE_SUPERVISOR_STATE_STOPPED &&
              supervisor.services[2].process == NULL,
          "failure leaves later dependencies running and does not restart");
    CHECK(heap_allocs == 3U && heap_frees == 1U &&
              stack_allocs == 3U && stack_frees == 1U &&
              as_allocs == 3U && as_frees == 1U,
          "failure frees exactly the dependent stopped before failure");
}

static void test_ordered_stop_slot_reuse(void) {
    struct service_supervisor supervisor;
    struct service_graph_snapshot snapshot = {0};
    struct service_supervisor_entry entry;
    struct service_descriptor descriptors[1];
    struct kernel_process *second_process;

    reset_all();
    memset(&supervisor, 0, sizeof(supervisor));
    CHECK(service_supervisor_init(&supervisor, TEST_HHDM) ==
              SERVICE_SUPERVISOR_OK,
          "slot reuse fixture initializes supervisor");
    descriptors[0] = make_descriptor(1U, true, 0U, "reuse-a");
    CHECK(service_graph_snapshot_build(&snapshot, descriptors, 1U) ==
              SERVICE_GRAPH_OK,
          "slot reuse fixture builds snapshot");
    entry.service_id = 1U;
    entry.entry = test_entry_a;
    CHECK(service_supervisor_start_ordered(&supervisor, &snapshot,
                                           &entry, 1U) ==
              SERVICE_SUPERVISOR_OK,
          "slot reuse fixture starts first generation");
    CHECK(service_supervisor_stop_ordered(&supervisor, &snapshot) ==
              SERVICE_SUPERVISOR_OK,
          "slot reuse fixture stops first generation");

    CHECK(service_supervisor_start_ordered(&supervisor, &snapshot,
                                           &entry, 1U) ==
              SERVICE_SUPERVISOR_OK &&
              supervisor.active_count == 1U &&
              supervisor.supervisor_pcb.child_count == 1U,
          "stopped service slot can start a new generation");
    second_process = supervisor.services[0].process;
    CHECK(second_process != NULL &&
              second_process->pcb.published &&
              second_process->lifecycle == PROCESS_LIFECYCLE_RUNNING &&
              supervisor.services[0].state ==
                  SERVICE_SUPERVISOR_STATE_RUNNING &&
              supervisor.services[0].restart_count == 0U,
          "new generation publishes a fresh process without stale restart");
    CHECK(heap_allocs == 2U && heap_frees == 1U &&
              stack_allocs == 2U && stack_frees == 1U &&
              as_allocs == 2U && as_frees == 1U,
          "slot reuse owns exactly one new process");
}

static void test_service_state_snapshot(void) {
    struct service_supervisor supervisor;
    struct service_supervisor uninitialized;
    struct service_descriptor descriptor_a;
    struct service_descriptor descriptor_b;
    struct service_supervisor_snapshot snapshot;
    struct service_supervisor_snapshot saved;
    struct service_supervisor_snapshot current;
    unsigned char sentinel[sizeof(snapshot)];
    bool published = false;

    reset_all();
    memset(&supervisor, 0, sizeof(supervisor));
    CHECK(service_supervisor_init(&supervisor, TEST_HHDM) ==
              SERVICE_SUPERVISOR_OK,
          "snapshot fixture initializes supervisor");
    descriptor_a = make_descriptor(7U, true, 2U, "snapshot-a");
    descriptor_b = make_descriptor(2U, true, 0U, "snapshot-b");
    CHECK(service_supervisor_start(&supervisor, &descriptor_a,
                                   test_entry_a) ==
              SERVICE_SUPERVISOR_OK &&
              service_supervisor_start(&supervisor, &descriptor_b,
                                       test_entry_b) ==
                  SERVICE_SUPERVISOR_OK,
          "snapshot fixture starts two services");
    CHECK(service_supervisor_report_ready(
              &supervisor, 7U, 5U, &published) ==
              SERVICE_SUPERVISOR_OK && published &&
              service_supervisor_set_readiness_deadline(
                  &supervisor, 2U, 10U, 20U) ==
                  SERVICE_SUPERVISOR_OK,
          "snapshot fixture publishes ready and readiness deadline state");

    memset(&snapshot, 0, sizeof(snapshot));
    CHECK(service_supervisor_snapshot(NULL, &snapshot) ==
              SERVICE_SUPERVISOR_NULL &&
              service_supervisor_snapshot(&supervisor, NULL) ==
                  SERVICE_SUPERVISOR_NULL,
          "snapshot rejects null supervisor and output");
    memset(&uninitialized, 0, sizeof(uninitialized));
    CHECK(service_supervisor_snapshot(&uninitialized, &snapshot) ==
              SERVICE_SUPERVISOR_BAD_STATE,
          "snapshot rejects uninitialized supervisor");

    memset(&snapshot, 0, sizeof(snapshot));
    CHECK(service_supervisor_snapshot(&supervisor, &snapshot) ==
              SERVICE_SUPERVISOR_OK &&
              snapshot.published &&
              snapshot.version == SERVICE_SUPERVISOR_SNAPSHOT_VERSION &&
              snapshot.generation != 0U &&
              snapshot.active_count == 2U &&
              snapshot.entry_count == 2U &&
              snapshot.entries[0].descriptor.id == 2U &&
              snapshot.entries[0].state ==
                  SERVICE_SUPERVISOR_STATE_RUNNING &&
              snapshot.entries[0].health_state ==
                  SERVICE_HEALTH_PENDING &&
              snapshot.entries[0].readiness_start_tick == 10U &&
              snapshot.entries[0].readiness_deadline_tick == 30U &&
              snapshot.entries[0].readiness_timeout_ticks == 20U &&
              snapshot.entries[1].descriptor.id == 7U &&
              snapshot.entries[1].state ==
                  SERVICE_SUPERVISOR_STATE_RUNNING &&
              snapshot.entries[1].health_state ==
                  SERVICE_HEALTH_READY &&
              snapshot.entries[1].ready_tick == 5U &&
              snapshot.entries[1].liveness_generation == 1U,
          "snapshot publishes exact sorted service state");
    CHECK(service_supervisor_snapshot_published(&snapshot) &&
              service_supervisor_snapshot_count(&snapshot) == 2U &&
              service_supervisor_snapshot_entry(&snapshot, 0U) != NULL &&
              service_supervisor_snapshot_entry(&snapshot, 2U) == NULL &&
              !service_supervisor_snapshot_published(NULL) &&
              service_supervisor_snapshot_count(NULL) == 0U &&
              service_supervisor_snapshot_entry(NULL, 0U) == NULL,
          "snapshot accessors enforce published bounds");
    saved = snapshot;

    CHECK(service_supervisor_report_liveness(
              &supervisor, 7U, 6U) == SERVICE_SUPERVISOR_OK &&
              memcmp(&snapshot, &saved, sizeof(snapshot)) == 0,
          "later liveness mutation does not change published snapshot");
    CHECK(service_supervisor_exit(&supervisor, 2U, 0U) ==
              SERVICE_SUPERVISOR_OK &&
              memcmp(&snapshot, &saved, sizeof(snapshot)) == 0,
          "later lifecycle mutation does not change published snapshot");

    memset(&current, 0, sizeof(current));
    CHECK(service_supervisor_snapshot(&supervisor, &current) ==
              SERVICE_SUPERVISOR_OK &&
              current.entry_count == 2U &&
              current.entries[0].state ==
                  SERVICE_SUPERVISOR_STATE_EXITED &&
              current.entries[1].state ==
                  SERVICE_SUPERVISOR_STATE_RUNNING &&
              current.entries[0].readiness_deadline_tick == 0U &&
              current.entries[1].ready_tick == 5U,
          "later snapshot reflects current lifecycle without stale mixed state");

    memset(sentinel, 0x5A, sizeof(sentinel));
    memcpy(&snapshot, sentinel, sizeof(snapshot));
    service_supervisor_host_test_set_snapshot_mutate(true);
    CHECK(service_supervisor_snapshot(&supervisor, &snapshot) ==
              SERVICE_SUPERVISOR_SNAPSHOT_INVALID &&
              memcmp(&snapshot, sentinel, sizeof(snapshot)) == 0,
          "mid-publication mutation is rejected without partial snapshot");
    service_supervisor_host_test_set_snapshot_mutate(false);
}

static void test_service_state_snapshot_empty_after_stop(void) {
    struct service_supervisor supervisor;
    struct service_graph_snapshot graph = {0};
    struct service_descriptor descriptor;
    struct service_supervisor_snapshot snapshot;
    struct service_supervisor_entry entry;

    reset_all();
    memset(&supervisor, 0, sizeof(supervisor));
    CHECK(service_supervisor_init(&supervisor, TEST_HHDM) ==
              SERVICE_SUPERVISOR_OK,
          "empty snapshot fixture initializes supervisor");
    descriptor = make_descriptor(1U, true, 0U, "empty-a");
    entry.service_id = 1U;
    entry.entry = test_entry_a;
    CHECK(service_graph_snapshot_build(&graph, &descriptor, 1U) ==
              SERVICE_GRAPH_OK &&
              service_supervisor_start_ordered(
                  &supervisor, &graph, &entry, 1U) ==
                  SERVICE_SUPERVISOR_OK,
          "empty snapshot fixture starts one service");
    CHECK(service_supervisor_stop_ordered(&supervisor, &graph) ==
              SERVICE_SUPERVISOR_OK,
          "empty snapshot fixture stops all services");
    memset(&snapshot, 0, sizeof(snapshot));
    CHECK(service_supervisor_snapshot(&supervisor, &snapshot) ==
              SERVICE_SUPERVISOR_OK &&
              snapshot.published &&
              snapshot.active_count == 0U &&
              snapshot.entry_count == 0U,
          "explicit stop publishes an empty exact snapshot");
}

#ifdef HFOC_SERVICE_CASCADE_DRILL_HOST_TEST
static void test_service_cascade_drill(void) {
    struct service_supervisor supervisor;
    struct service_graph_snapshot graph = {0};
    struct service_descriptor descriptors[3];
    struct service_supervisor_entry entries[3];
    const struct fault_injection_site *site = NULL;
    uint8_t dependency_middle[1] = {1U};
    uint8_t dependency_top[1] = {2U};
    uint32_t status = 0U;
    bool restarted = false;
    uint32_t index;

    reset_all();
    memset(&supervisor, 0, sizeof(supervisor));
    CHECK(service_supervisor_init(&supervisor, TEST_HHDM) ==
              SERVICE_SUPERVISOR_OK,
          "cascade drill initializes supervisor");

    descriptors[0] = make_descriptor_with_deps(
        1U, true, 2U, "cascade-base", NULL, 0U);
    descriptors[1] = make_descriptor_with_deps(
        2U, true, 1U, "cascade-middle", dependency_middle, 1U);
    descriptors[2] = make_descriptor_with_deps(
        3U, true, 0U, "cascade-top", dependency_top, 1U);
    CHECK(service_graph_snapshot_build(
              &graph, descriptors, 3U) == SERVICE_GRAPH_OK &&
              service_graph_snapshot_start_id(&graph, 0U) == 1U &&
              service_graph_snapshot_start_id(&graph, 1U) == 2U &&
              service_graph_snapshot_start_id(&graph, 2U) == 3U,
          "cascade drill builds deterministic dependency order");
    entries[0].service_id = 1U;
    entries[0].entry = test_entry_a;
    entries[1].service_id = 2U;
    entries[1].entry = test_entry_b;
    entries[2].service_id = 3U;
    entries[2].entry = test_entry_a;
    CHECK(service_supervisor_start_ordered(
              &supervisor, &graph, entries, 3U) ==
              SERVICE_SUPERVISOR_OK &&
              supervisor.active_count == 3U &&
              supervisor.supervisor_pcb.child_count == 3U,
          "cascade drill starts full service chain");

    CHECK(fault_injection_site_by_id(
              FAULT_INJECTION_SERVICE_DEPENDENCY, &site) ==
              FAULT_INJECTION_OK &&
              site->subsystem == FAULT_INJECTION_SUBSYSTEM_SERVICE &&
              site->bounded && site->test_only &&
          fault_injection_site_by_id(
              FAULT_INJECTION_SERVICE_RESTART_EXHAUSTION, &site) ==
              FAULT_INJECTION_OK &&
              site->subsystem == FAULT_INJECTION_SUBSYSTEM_SERVICE &&
              site->bounded && site->test_only &&
          fault_injection_site_by_id(
              FAULT_INJECTION_SERVICE_CONTROLLED_STOP, &site) ==
              FAULT_INJECTION_OK &&
              site->subsystem == FAULT_INJECTION_SUBSYSTEM_SERVICE &&
              site->bounded && site->test_only,
          "cascade drill consumes exact service fault points");

    CHECK(service_supervisor_exit(&supervisor, 1U, 11U) ==
              SERVICE_SUPERVISOR_OK &&
              service_supervisor_reap_contained(
                  &supervisor, 1U, 20U, &graph, &status, &restarted) ==
                  SERVICE_SUPERVISOR_OK &&
              status == 11U && restarted &&
              supervisor.services[0].restart_count == 1U &&
              supervisor.supervisor_pcb.child_count == 3U,
          "first dependency failure restarts only base service");

    restarted = false;
    CHECK(service_supervisor_exit(&supervisor, 1U, 12U) ==
              SERVICE_SUPERVISOR_OK &&
              service_supervisor_reap_contained(
                  &supervisor, 1U, 20U, &graph, &status, &restarted) ==
                  SERVICE_SUPERVISOR_RESTART_INTERVAL &&
              status == 12U && !restarted &&
              supervisor.services[0].process == NULL &&
              supervisor.supervisor_pcb.child_count == 2U,
          "restart interval leaves no replacement base process");

    restarted = false;
    CHECK(service_supervisor_exit(&supervisor, 2U, 21U) ==
              SERVICE_SUPERVISOR_OK &&
              service_supervisor_reap_contained(
                  &supervisor, 2U, 20U, &graph, &status, &restarted) ==
                  SERVICE_SUPERVISOR_DEPENDENCY_BLOCKED &&
              status == 21U && !restarted &&
              supervisor.services[1].process == NULL &&
              supervisor.supervisor_pcb.child_count == 1U,
          "dependent service is blocked while base is unavailable");

    restarted = false;
    CHECK(service_supervisor_restart_contained(
              &supervisor, 1U, 30U, &graph, &restarted) ==
              SERVICE_SUPERVISOR_OK && restarted &&
              supervisor.services[0].process != NULL &&
              supervisor.services[0].restart_count == 2U &&
              supervisor.supervisor_pcb.child_count == 2U,
          "base service recovers after interval");
    restarted = false;
    CHECK(service_supervisor_restart_contained(
              &supervisor, 2U, 30U, &graph, &restarted) ==
              SERVICE_SUPERVISOR_OK && restarted &&
              supervisor.services[1].process != NULL &&
              supervisor.services[1].restart_count == 1U &&
              supervisor.supervisor_pcb.child_count == 3U,
          "blocked dependent recovers after base is running");

    restarted = false;
    CHECK(service_supervisor_exit(&supervisor, 3U, 31U) ==
              SERVICE_SUPERVISOR_OK &&
              service_supervisor_reap_contained(
                  &supervisor, 3U, 30U, &graph, &status, &restarted) ==
                  SERVICE_SUPERVISOR_EXHAUSTED &&
              status == 31U && !restarted &&
              supervisor.services[2].process == NULL &&
              supervisor.supervisor_pcb.child_count == 2U,
          "restart exhaustion publishes no orphan top process");

    CHECK(service_supervisor_stop_ordered(
              &supervisor, &graph) == SERVICE_SUPERVISOR_OK &&
              supervisor.active_count == 1U &&
              supervisor.supervisor_pcb.child_count == 0U,
          "controlled stop leaves only exhausted slot and no child service");
    for (index = 0U; index < SERVICE_SUPERVISOR_CAPACITY; ++index) {
        CHECK(supervisor.services[index].process == NULL,
              "controlled stop leaves no service process pointer");
    }
    CHECK(heap_allocs == heap_frees && stack_allocs == stack_frees &&
              as_allocs == as_frees,
          "cascade drill leaves no process resource leak");
}
#endif

int main(void) {
    test_null_bad_state_and_not_bsp();
    test_invalid_and_disabled_descriptor();
    test_start_ownership_and_duplicate();
    test_exit_and_exact_reap();
    test_restart_budget_no_leak();
    test_restart_failure_no_leak();
    test_capacity_full();
    test_status_strings_and_accessors();
    test_restart_containment_null_and_bad_state();
    test_restart_containment_exact_interval_and_retry();
    test_restart_containment_dependency_policy();
    test_restart_containment_disabled_after_exhaustion();
    test_restart_containment_process_failure_no_leak();
    test_resource_envelope_attach_and_limit_failure();
    test_readiness_and_liveness_state();
    test_readiness_deadline_state();
    test_health_reset_on_restart_and_exhaustion();
    test_ordered_start_input_validation();
    test_ordered_start_empty_and_all_disabled();
    test_ordered_start_chain_and_ownership();
    test_ordered_start_already_running_and_mismatch();
    test_ordered_start_disabled_predecessor_blocks_dependent();
    test_ordered_start_process_failure_blocks_dependent();
    test_ordered_start_exited_and_exhausted_blocks();
    test_ordered_stop_input_validation();
    test_ordered_stop_empty_and_all_disabled();
    test_ordered_stop_reverse_order_and_ownership();
    test_ordered_stop_suppresses_automatic_restart();
    test_ordered_stop_unrelated_service_untouched();
    test_ordered_stop_failure_blocks_dependent_stop();
    test_ordered_stop_slot_reuse();
    test_service_state_snapshot();
    test_service_state_snapshot_empty_after_stop();
#ifdef HFOC_SERVICE_CASCADE_DRILL_HOST_TEST
    test_service_cascade_drill();
#endif

    if (failures != 0) {
        fprintf(stderr, "Service supervisor host tests: %d FAILURES\n",
                failures);
        return 1;
    }
    puts("Service supervisor host tests: ALL OK");
    return 0;
}

#endif
