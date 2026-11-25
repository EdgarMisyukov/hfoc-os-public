#ifndef HFOC_PROCESS_H
#define HFOC_PROCESS_H

#include "capability.h"
#include "context.h"
#include "descriptor.h"
#include "scheduler.h"
#include "vmm.h"
#include <stdbool.h>
#include <stdint.h>

#define PROCESS_MAX_PIDS 64U
#define PROCESS_KERNEL_STACK_PAGES 4U
#define PROCESS_KERNEL_STACK_SIZE \
    (PROCESS_KERNEL_STACK_PAGES * 4096U)
#define PROCESS_CPU_TICK_LIMIT_UNLIMITED 0U
#define PROCESS_USER_MEMORY_LIMIT_UNLIMITED 0U
#define PROCESS_USER_MAPPED_MAX 64U
#define PROCESS_EXIT_LIMIT_EXCEEDED UINT32_C(0xFFFFFFFF)

enum process_security_domain {
    PROCESS_SECURITY_DOMAIN_KERNEL = 0,
    PROCESS_SECURITY_DOMAIN_USER = 1,
    PROCESS_SECURITY_DOMAIN_COUNT = 2,
};

enum process_lifecycle_state {
    PROCESS_LIFECYCLE_RUNNING = 0,
    PROCESS_LIFECYCLE_EXITED = 1,
    PROCESS_LIFECYCLE_STOPPED = 2,
};

/* ITEM-633: the bounded signal set.  No user handlers in this item. */
#define PROCESS_SIGNAL_NONE 0U
#define PROCESS_SIGNAL_HUP  1U
#define PROCESS_SIGNAL_INT  2U
#define PROCESS_SIGNAL_QUIT 3U
#define PROCESS_SIGNAL_TERM 15U
#define PROCESS_SIGNAL_CHLD 17U
#define PROCESS_SIGNAL_CONT 18U
#define PROCESS_SIGNAL_TSTP 20U
#define PROCESS_SIGNAL_MAX  20U

/* ITEM-633: wait2 flags and the encoded status words. */
#define PROCESS_WAIT_NOHANG     0x1U
#define PROCESS_WAIT_UNTRACED   0x2U
#define PROCESS_WAIT_CONTINUED  0x4U
#define PROCESS_WAIT_STATUS_EXITED(code) ((((uint32_t)(code)) & 0xffU) << 8U)
#define PROCESS_WAIT_STATUS_SIGNALED(signal) ((uint32_t)(signal) & 0x7fU)
#define PROCESS_WAIT_STATUS_STOPPED(signal) \
    (((((uint32_t)(signal)) & 0xffU) << 8U) | 0x7fU)
#define PROCESS_WAIT_STATUS_CONTINUED UINT32_C(0xffff)

enum process_status {
    PROCESS_OK = 0,
    PROCESS_NULL = 1,
    PROCESS_NO_PIDS = 2,
    PROCESS_BAD_PID = 3,
    PROCESS_BAD_STATE = 4,
    PROCESS_HAS_CHILDREN = 5,
    PROCESS_LINK_FAILED = 6,
    PROCESS_HEAP_FAILED = 7,
    PROCESS_ADDRESS_SPACE_FAILED = 8,
    PROCESS_STACK_FAILED = 9,
    PROCESS_CONTEXT_FAILED = 10,
    PROCESS_NO_CHILDREN = 11,
    PROCESS_NOT_EXITED = 12,
    PROCESS_ALREADY_EXITED = 13,
    PROCESS_SCHEDULER_FAILED = 14,
    PROCESS_USER_PAGE_FAILED = 15,
    PROCESS_BAD_DOMAIN = 16,
    PROCESS_USER_MEMORY_LIMIT_EXCEEDED = 17,
    /* ITEM-633 */
    PROCESS_BAD_SIGNAL = 18,
    PROCESS_NOT_STOPPED = 19,
    PROCESS_SELF_SIGNAL = 20,
};

struct process_control_block {
    uint32_t pid;
    uint32_t pgid;
    uint32_t session_id;
    uint32_t pending_signals;
    uint32_t stop_signal;
    uint32_t wait_stopped;
    uint32_t wait_continued;
    uint32_t child_count;
    enum process_security_domain security_domain;
    uint32_t security_generation;
    /* ITEM-626: the error of the last file/exec syscall, so the native runtime
       can tell a small success value from one of the positive ABI errors. */
    uint32_t syscall_last_error;
    struct capability_table capability_table;
    bool published;
    bool orphaned;
    struct process_control_block *parent;
    struct process_control_block *children_head;
    struct process_control_block *children_tail;
    struct process_control_block *sibling_next;
    struct process_control_block *sibling_prev;
};

struct kernel_process {
    struct process_control_block pcb;
    struct descriptor_table descriptor_table;
    /*
     * ITEM-624: bounded per-process storage for the typed standard streams
     * and for VFS file descriptions.  A descriptor points into one of these
     * slots; the last close clears `published`, which makes the slot reusable.
     * The arrays are inside the process, so destroy/reap releases every one.
     */
    struct file_description stdio[3U];
    struct file_description file_descriptions[PROCESS_DESCRIPTOR_CAPACITY];
    uint32_t vfs_cwd;
    bool vfs_cwd_ready;
    struct address_space address_space;
    struct context_frame context;
    void (*entry)(void);
    uintptr_t stack_phys;
    uintptr_t stack_top;
    /* ITEM-625: native ELF entry and initial user stack top. */
    uint64_t user_entry;
    uint64_t user_stack_top;
    uint64_t user_mapped_virtual[PROCESS_USER_MAPPED_MAX];
    uint64_t user_mapped_physical[PROCESS_USER_MAPPED_MAX];
    uint32_t user_mapped_count;
    uint32_t stack_pages;
    uint32_t exit_status;
    enum process_lifecycle_state lifecycle;
    struct scheduler_task scheduler_task;
    bool scheduler_published;
    bool address_space_owned;
    bool stack_owned;
    uintptr_t user_code_phys;
    uintptr_t user_stack_phys;
    bool user_pages_owned;
    uint64_t cpu_tick_limit;
    uint64_t user_memory_limit_pages;
    uint64_t user_memory_pages;
};

enum process_status process_pid_alloc(uint32_t *pid);
enum process_status process_pid_free(uint32_t pid);
enum process_status process_pcb_init(struct process_control_block *pcb,
                                     struct process_control_block *parent,
                                     uint32_t *pid_out);
enum process_status process_pcb_init_domain(
    struct process_control_block *pcb,
    struct process_control_block *parent,
    uint32_t *pid_out,
    enum process_security_domain security_domain);
enum process_status process_pcb_destroy(struct process_control_block *pcb);
uint32_t process_pcb_child_count(const struct process_control_block *pcb);
enum process_security_domain process_pcb_security_domain(
    const struct process_control_block *pcb);
uint32_t process_pcb_security_generation(
    const struct process_control_block *pcb);
enum process_status process_bind_user_streams(
    struct kernel_process *process);
enum process_status process_create_user_image(
    struct kernel_process **out_process,
    struct process_control_block *parent, void (*entry)(void),
    uint64_t user_entry, uint64_t user_stack_top, const void *code,
    size_t code_size, uint64_t hhdm_offset);
enum process_status process_create_user(
    struct kernel_process **out_process,
    struct process_control_block *parent, void (*entry)(void),
    uint64_t user_entry, uint64_t user_stack_top, uint64_t hhdm_offset);
enum process_status process_create_kernel(
    struct kernel_process **out_process,
    struct process_control_block *parent,
    void (*entry)(void),
    uint64_t hhdm_offset);
enum process_status process_limit_set_cpu_ticks(
    struct kernel_process *process,
    uint64_t limit);
enum process_status process_limit_set_user_memory_pages(
    struct kernel_process *process,
    uint64_t limit);
enum process_status process_limit_charge_user_memory(
    struct kernel_process *process,
    uint64_t pages);
uint64_t process_limit_cpu_ticks(const struct kernel_process *process);
uint64_t process_limit_user_memory_limit_pages(
    const struct kernel_process *process);
uint64_t process_limit_user_memory_pages(
    const struct kernel_process *process);
#if defined(HFOC_USER_INIT_QEMU_TEST) || defined(HFOC_USER_INIT_HOST_TEST)
enum process_status process_create_user_init(
    struct kernel_process **out_process,
    struct process_control_block *parent,
    void (*entry)(void),
    uint64_t hhdm_offset);
#endif
enum process_status process_destroy_kernel(
    struct kernel_process *process,
    uint64_t hhdm_offset);
enum process_status process_exit_kernel(
    struct kernel_process *process,
    uint32_t exit_status);
/*
 * ITEM-625: cancel a child that is queued or blocked, never the caller.
 * The scheduler task leaves every queue before the process becomes exited, so
 * the reaper can free the address space without a stale run-queue entry.
 */
enum process_status process_cancel_kernel(
    struct kernel_process *process,
    uint32_t exit_status);
enum process_status process_wait_kernel(
    struct process_control_block *parent,
    uint32_t *pid_out,
    uint32_t *exit_status_out,
    uint64_t hhdm_offset);
/* ITEM-633: wait with WNOHANG/WUNTRACED and an encoded status. */
enum process_status process_wait2_kernel(
    struct process_control_block *parent,
    uint32_t flags,
    uint32_t *pid_out,
    uint32_t *status_out,
    uint64_t hhdm_offset);
enum process_status process_setpgid_kernel(
    struct kernel_process *process,
    uint32_t pgid);
enum process_status process_signal_kernel(
    struct kernel_process *process,
    uint32_t signal);
enum process_status process_signal_group(
    uint32_t pgid,
    uint32_t signal);
enum process_status process_signal_session(
    uint32_t session_id,
    uint32_t signal);
struct kernel_process *process_find_pid(uint32_t pid);
uint32_t process_pgid(const struct kernel_process *process);
uint32_t process_session_id(const struct kernel_process *process);
enum process_status process_reap_kernel(
    struct kernel_process *process,
    uint32_t *exit_status_out,
    uint64_t hhdm_offset);
#if !defined(HFOC_PROCESS_HOST_TEST) || \
    defined(HFOC_PROCESS_SCHEDULER_HOST_TEST)
enum process_status process_spawn_kernel(
    struct kernel_process *process,
    uint64_t hhdm_offset);
#endif
const char *process_status_string(enum process_status status);

#ifdef HFOC_PROCESS_SCHEDULER_QEMU_TEST
enum process_status process_sched_qemu_test_init(uint64_t hhdm_offset);
#endif

#ifdef HFOC_PROCESS_HOST_TEST
void process_host_test_reset(void);
void process_host_test_set_link_failure(bool fail);
#endif

#endif
