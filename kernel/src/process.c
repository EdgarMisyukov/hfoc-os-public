#include "process.h"

#include "heap.h"
#include "pmm.h"
#if !defined(HFOC_PROCESS_HOST_TEST) && !defined(HFOC_USER_INIT_HOST_TEST)
#include "console.h"
#define process_console_write console_write
#else
static void process_console_write(const char *text) { (void)text; }
#endif
#if defined(HFOC_USER_INIT_QEMU_TEST) || defined(HFOC_USER_INIT_HOST_TEST)
#include "user_init.h"
#endif
#ifdef HFOC_PROCESS_SCHEDULER_QEMU_TEST
#include "serial.h"
#define PROCESS_SCHED_QEMU_CHILD_COUNT 3U
#define PROCESS_SCHED_QEMU_LIMITED_INDEX 0U
#define PROCESS_SCHED_QEMU_LIMIT_TICKS 2U
#endif
#include <stddef.h>
#include <string.h>

extern void sched_kernel_thread_trampoline(void);

#define PROCESS_PID_WORDS ((PROCESS_MAX_PIDS + 63U) / 64U)

static uint64_t process_pid_used[PROCESS_PID_WORDS];
/* ITEM-633: pid -> live PCB registry, bounded by the same 64-PID table. */
static struct process_control_block *process_pid_table[PROCESS_MAX_PIDS];
static struct process_control_block *process_orphans_head;
static struct process_control_block *process_orphans_tail;

#ifdef HFOC_PROCESS_HOST_TEST
static bool process_host_link_failure;
#endif

static void process_child_append(struct process_control_block *parent,
                                 struct process_control_block *child)
{
    child->parent = parent;
    child->sibling_prev = parent->children_tail;
    child->sibling_next = NULL;
    if (parent->children_tail != NULL) {
        parent->children_tail->sibling_next = child;
    } else {
        parent->children_head = child;
    }
    parent->children_tail = child;
    ++parent->child_count;
}

static void process_child_remove(struct process_control_block *parent,
                                 struct process_control_block *child)
{
    if (child->sibling_prev != NULL) {
        child->sibling_prev->sibling_next = child->sibling_next;
    } else {
        parent->children_head = child->sibling_next;
    }
    if (child->sibling_next != NULL) {
        child->sibling_next->sibling_prev = child->sibling_prev;
    } else {
        parent->children_tail = child->sibling_prev;
    }
    if (parent->child_count != 0U) {
        --parent->child_count;
    }
}

static struct kernel_process *process_from_pcb(
    struct process_control_block *pcb)
{
    return (struct kernel_process *)((char *)pcb -
                                     offsetof(struct kernel_process, pcb));
}

static void process_orphan_append(struct process_control_block *pcb) {
    if (pcb->orphaned) {
        return;
    }
    pcb->orphaned = true;
    pcb->parent = NULL;
    pcb->sibling_prev = process_orphans_tail;
    pcb->sibling_next = NULL;
    if (process_orphans_tail != NULL) {
        process_orphans_tail->sibling_next = pcb;
    } else {
        process_orphans_head = pcb;
    }
    process_orphans_tail = pcb;
}

static void process_orphan_remove(struct process_control_block *pcb) {
    if (!pcb->orphaned) {
        return;
    }
    if (pcb->sibling_prev != NULL) {
        pcb->sibling_prev->sibling_next = pcb->sibling_next;
    } else {
        process_orphans_head = pcb->sibling_next;
    }
    if (pcb->sibling_next != NULL) {
        pcb->sibling_next->sibling_prev = pcb->sibling_prev;
    } else {
        process_orphans_tail = pcb->sibling_prev;
    }
    pcb->orphaned = false;
    pcb->sibling_prev = NULL;
    pcb->sibling_next = NULL;
}

static struct process_control_block *process_find_exited_child(
    struct process_control_block *list_head)
{
    struct process_control_block *pcb;

    for (pcb = list_head; pcb != NULL; pcb = pcb->sibling_next) {
        if (process_from_pcb(pcb)->lifecycle ==
            PROCESS_LIFECYCLE_EXITED) {
            return pcb;
        }
    }
    return NULL;
}

static void process_release_stack(struct kernel_process *process) {
    if (process->stack_owned && process->stack_phys != 0U &&
        process->stack_pages != 0U) {
        (void)pmm_free_contiguous_pages(process->stack_phys,
                                        process->stack_pages);
    }
    process->stack_owned = false;
    process->stack_phys = 0U;
    process->stack_pages = 0U;
    process->stack_top = 0U;
}

static void process_release_user_pages(struct kernel_process *process) {
    if (!process->user_pages_owned) {
        return;
    }
#if defined(HFOC_PROCESS_HOST_TEST) && !defined(HFOC_USER_INIT_HOST_TEST)
    return;
#else
    if (process->user_code_phys != 0U) {
        (void)pmm_free_page(process->user_code_phys);
    }
    if (process->user_stack_phys != 0U) {
        (void)pmm_free_page(process->user_stack_phys);
    }
    while (process->user_mapped_count != 0U) {
        --process->user_mapped_count;
        (void)pmm_free_page(
            process->user_mapped_physical[process->user_mapped_count]);
    }
    process->user_pages_owned = false;
    process->user_code_phys = 0U;
    process->user_stack_phys = 0U;
#endif
}

static void process_release_address_space(struct kernel_process *process,
                                          uint64_t hhdm_offset)
{
    if (process->address_space_owned &&
        process->address_space.pml4_phys != 0U) {
#if !defined(HFOC_PROCESS_HOST_TEST) && \
    !defined(HFOC_USER_INIT_HOST_TEST)
        /*
         * ITEM-625: a cancelled user process can still be the active address
         * space because the scheduler switch itself does not change CR3.  A
         * page table that is still loaded cannot be freed; return to the
         * kernel address space first and then release the image.
         */
        const struct address_space *current = vmm_current_address_space();
        const struct address_space *kernel = vmm_kernel_address_space();

        if (current != NULL && kernel != NULL &&
            current->pml4_phys == process->address_space.pml4_phys) {
            struct address_space kernel_space = *kernel;

            (void)vmm_activate_address_space(&kernel_space);
        }
#endif
        (void)vmm_destroy_address_space(&process->address_space,
                                        hhdm_offset);
    }
    process->address_space_owned = false;
    process->address_space.pml4_phys = 0U;
}

static void process_release_descriptor_table(struct kernel_process *process) {
    (void)descriptor_table_destroy(&process->descriptor_table);
}

static void process_cleanup_failed(struct kernel_process *process,
                                   uint64_t hhdm_offset)
{
    process_release_user_pages(process);
    process_release_stack(process);
    process_release_address_space(process, hhdm_offset);
    process_release_descriptor_table(process);
    memset(process, 0, sizeof(*process));
    kfree(process);
}

enum process_status process_pid_alloc(uint32_t *pid) {
    uint32_t index;

    if (pid == NULL) {
        return PROCESS_NULL;
    }
    for (index = 0U; index < PROCESS_MAX_PIDS; ++index) {
        const uint32_t word = index / 64U;
        const uint64_t bit = 1ULL << (index % 64U);
        if ((process_pid_used[word] & bit) == 0U) {
            process_pid_used[word] |= bit;
            *pid = index + 1U;
            return PROCESS_OK;
        }
    }
    return PROCESS_NO_PIDS;
}

enum process_status process_pid_free(uint32_t pid) {
    uint32_t index;
    uint32_t word;
    uint64_t bit;

    if (pid == 0U || pid > PROCESS_MAX_PIDS) {
        return PROCESS_BAD_PID;
    }
    index = pid - 1U;
    word = index / 64U;
    bit = 1ULL << (index % 64U);
    if ((process_pid_used[word] & bit) == 0U) {
        return PROCESS_BAD_PID;
    }
    process_pid_used[word] &= ~bit;
    return PROCESS_OK;
}

enum process_status process_pcb_init_domain(
    struct process_control_block *pcb,
    struct process_control_block *parent,
    uint32_t *pid_out,
    enum process_security_domain security_domain)
{
    enum process_status pid_status;

    if (pcb == NULL || pid_out == NULL) {
        return PROCESS_NULL;
    }
    if (pcb->published) {
        return PROCESS_BAD_STATE;
    }
    if (parent != NULL && (!parent->published || parent == pcb)) {
        return PROCESS_BAD_STATE;
    }
    if (security_domain >= PROCESS_SECURITY_DOMAIN_COUNT) {
        return PROCESS_BAD_DOMAIN;
    }

    memset(pcb, 0, sizeof(*pcb));
    pid_status = process_pid_alloc(pid_out);
    if (pid_status != PROCESS_OK) {
        return pid_status;
    }

#ifdef HFOC_PROCESS_HOST_TEST
    if (process_host_link_failure) {
        (void)process_pid_free(*pid_out);
        return PROCESS_LINK_FAILED;
    }
#endif

    if (parent != NULL) {
        process_child_append(parent, pcb);
    }
    const enum capability_status capability_status =
        capability_table_init(&pcb->capability_table);
    if (capability_status != CAPABILITY_OK) {
        if (parent != NULL) {
            process_child_remove(parent, pcb);
        }
        (void)process_pid_free(*pid_out);
        memset(pcb, 0, sizeof(*pcb));
        return PROCESS_BAD_STATE;
    }
    pcb->pid = *pid_out;
    pcb->pgid = parent != NULL && parent->pgid != 0U ? parent->pgid
                                                     : *pid_out;
    pcb->session_id = parent != NULL && parent->session_id != 0U
                          ? parent->session_id
                          : *pid_out;
    pcb->security_domain = security_domain;
    pcb->published = true;
    process_pid_table[*pid_out - 1U] = pcb;
    return PROCESS_OK;
}

enum process_status process_pcb_init(struct process_control_block *pcb,
                                     struct process_control_block *parent,
                                     uint32_t *pid_out)
{
    return process_pcb_init_domain(pcb, parent, pid_out,
                                   PROCESS_SECURITY_DOMAIN_KERNEL);
}

enum process_status process_pcb_destroy(struct process_control_block *pcb) {
    struct process_control_block *const parent =
        pcb != NULL ? pcb->parent : NULL;
    const uint32_t pid = pcb != NULL ? pcb->pid : 0U;

    if (pcb == NULL) {
        return PROCESS_NULL;
    }
    if (!pcb->published) {
        return PROCESS_BAD_STATE;
    }
    if (pcb->children_head != NULL) {
        return PROCESS_HAS_CHILDREN;
    }
    if (pcb->capability_table.published &&
        capability_table_destroy(&pcb->capability_table) != CAPABILITY_OK) {
        return PROCESS_BAD_STATE;
    }
    if (pcb->orphaned) {
        process_orphan_remove(pcb);
    }
    if (pcb->pid != 0U && pcb->pid <= PROCESS_MAX_PIDS &&
        process_pid_table[pcb->pid - 1U] == pcb) {
        process_pid_table[pcb->pid - 1U] = NULL;
    }
    if (parent != NULL) {
        process_child_remove(parent, pcb);
    }
    memset(pcb, 0, sizeof(*pcb));
    return process_pid_free(pid);
}

/*
 * ITEM-624: the typed standard streams.  fd 0 is an empty read stream until
 * the owner TTY arrives (ITEM-629), fd 1/2 write the kernel console.  They are
 * real descriptors, so dup2/inheritance in ITEM-627 can redirect them without
 * a second code path.
 */
static enum vfs_status process_stream_read(struct file_description *description,
                                           void *buffer, size_t size,
                                           size_t *done_out)
{
    (void)description;
    (void)buffer;
    (void)size;
    if (done_out == NULL) {
        return VFS_NULL;
    }
    *done_out = 0U;
    return VFS_OK;
}

static enum vfs_status process_stream_write(
    struct file_description *description, const void *buffer, size_t size,
    size_t *done_out)
{
    const char *bytes = (const char *)buffer;
    char chunk[129];
    size_t offset = 0U;

    (void)description;
    if (bytes == NULL || done_out == NULL) {
        return VFS_NULL;
    }
    while (offset < size) {
        const size_t take = size - offset > 128U ? 128U : size - offset;

        memcpy(chunk, bytes + offset, take);
        chunk[take] = '\0';
        process_console_write(chunk);
        offset += take;
    }
    *done_out = offset;
    return VFS_OK;
}

static void process_stream_close(struct file_description *description)
{
    (void)description;
}

static const struct descriptor_ops process_stream_ops = {
    .read = process_stream_read,
    .write = process_stream_write,
    .close = process_stream_close,
};

enum process_status process_bind_user_streams(
    struct kernel_process *process)
{
    uint32_t descriptor;

    if (process == NULL) {
        return PROCESS_NULL;
    }
    if (!descriptor_table_is_published(&process->descriptor_table)) {
        return PROCESS_BAD_STATE;
    }
    for (descriptor = 0U; descriptor < 3U; ++descriptor) {
        const bool readable = descriptor == 0U;
        const bool writable = descriptor != 0U;
        uint32_t bound = 0U;

        if (descriptor_description_init(
                &process->stdio[descriptor], &process_stream_ops, NULL,
                readable, writable) != DESCRIPTOR_OK ||
            descriptor_table_bind(&process->descriptor_table,
                                  &process->stdio[descriptor],
                                  &bound) != DESCRIPTOR_OK ||
            bound != descriptor) {
            return PROCESS_BAD_STATE;
        }
    }
    return PROCESS_OK;
}

static enum process_status process_publish_user_capabilities(
    struct kernel_process *process)
{
    static const struct capability capabilities[] = {
        { CAPABILITY_TYPE_SYSCALL_WRITE, 0U, 0U },
        { CAPABILITY_TYPE_SYSCALL_YIELD, 0U, 0U },
        { CAPABILITY_TYPE_SYSCALL_EXIT, 0U, 0U },
        { CAPABILITY_TYPE_SYSCALL_FILE, 0U, 0U },
    };
    uint32_t index;

    for (index = 0U; index < sizeof(capabilities) / sizeof(capabilities[0]);
         ++index) {
        uint64_t handle = 0U;

        if (capability_table_add(&process->pcb.capability_table,
                                 &capabilities[index], &handle) !=
            CAPABILITY_OK) {
            return PROCESS_BAD_STATE;
        }
    }
    return PROCESS_OK;
}

static enum process_status process_create_kernel_domain(
    struct kernel_process **out_process,
    struct process_control_block *parent,
    void (*entry)(void),
    uint64_t hhdm_offset,
    enum process_security_domain security_domain)
{
    struct kernel_process *process;
    uint64_t stack_phys;
    uint64_t stack_end;
    uintptr_t stack_top;
    uint32_t pid = 0U;
    enum context_status context_status;
    enum process_status pcb_status;

    if (out_process == NULL || entry == NULL) {
        return PROCESS_NULL;
    }
    *out_process = NULL;

    process = kmalloc(sizeof(*process));
    if (process == NULL) {
        return PROCESS_HEAP_FAILED;
    }
    memset(process, 0, sizeof(*process));
    process->cpu_tick_limit = PROCESS_CPU_TICK_LIMIT_UNLIMITED;
    process->user_memory_limit_pages = PROCESS_USER_MEMORY_LIMIT_UNLIMITED;
    (void)descriptor_table_init(&process->descriptor_table);

    const enum vmm_status as_status =
        vmm_create_address_space(&process->address_space, hhdm_offset);
    if (as_status != VMM_OK) {
        process_release_descriptor_table(process);
        memset(process, 0, sizeof(*process));
        kfree(process);
        return PROCESS_ADDRESS_SPACE_FAILED;
    }
    process->address_space_owned = true;

    stack_phys = pmm_alloc_contiguous_pages(PROCESS_KERNEL_STACK_PAGES);
    if (stack_phys == 0U ||
        stack_phys > UINT64_MAX - PROCESS_KERNEL_STACK_SIZE) {
        process_cleanup_failed(process, hhdm_offset);
        return PROCESS_STACK_FAILED;
    }
    process->stack_phys = stack_phys;
    process->stack_pages = PROCESS_KERNEL_STACK_PAGES;
    process->stack_owned = true;

    stack_end = stack_phys + PROCESS_KERNEL_STACK_SIZE;
    if (stack_end > UINT64_MAX - hhdm_offset) {
        process_cleanup_failed(process, hhdm_offset);
        return PROCESS_STACK_FAILED;
    }
    stack_top = (uintptr_t)(hhdm_offset + stack_end);
    process->stack_top = stack_top;

    context_status = context_frame_init(&process->context, stack_top, entry);
    if (context_status != CONTEXT_OK) {
        process_cleanup_failed(process, hhdm_offset);
        return PROCESS_CONTEXT_FAILED;
    }
    process->entry = entry;

    pcb_status = process_pcb_init_domain(&process->pcb, parent, &pid,
                                         security_domain);
    if (pcb_status != PROCESS_OK) {
        process_cleanup_failed(process, hhdm_offset);
        return pcb_status;
    }

    *out_process = process;
    return PROCESS_OK;
}

enum process_status process_create_kernel(
    struct kernel_process **out_process,
    struct process_control_block *parent,
    void (*entry)(void),
    uint64_t hhdm_offset)
{
    return process_create_kernel_domain(out_process, parent, entry,
                                        hhdm_offset,
                                        PROCESS_SECURITY_DOMAIN_KERNEL);
}

enum process_status process_create_user(
    struct kernel_process **out_process,
    struct process_control_block *parent, void (*entry)(void),
    uint64_t user_entry, uint64_t user_stack_top, uint64_t hhdm_offset)
{
    struct kernel_process *process = NULL;
    enum process_status status;

    if (out_process == NULL || entry == NULL || user_entry == 0U ||
        user_stack_top == 0U || (user_stack_top & 7U) != 0U) {
        return PROCESS_NULL;
    }
    *out_process = NULL;
    status = process_create_kernel_domain(&process, parent, entry, hhdm_offset,
                                          PROCESS_SECURITY_DOMAIN_USER);
    if (status != PROCESS_OK) {
        return status;
    }
    if (process_publish_user_capabilities(process) != PROCESS_OK ||
        process_bind_user_streams(process) != PROCESS_OK) {
        (void)process_destroy_kernel(process, hhdm_offset);
        return PROCESS_BAD_STATE;
    }
    process->user_entry = user_entry;
    process->user_stack_top = user_stack_top;
    *out_process = process;
    return PROCESS_OK;
}

enum process_status process_create_user_image(
    struct kernel_process **out_process,
    struct process_control_block *parent, void (*entry)(void),
    uint64_t user_entry, uint64_t user_stack_top, const void *code,
    size_t code_size, uint64_t hhdm_offset)
{
#if defined(HFOC_PROCESS_HOST_TEST) || defined(HFOC_USER_INIT_HOST_TEST)
    (void)out_process;
    (void)parent;
    (void)entry;
    (void)user_entry;
    (void)user_stack_top;
    (void)code;
    (void)code_size;
    (void)hhdm_offset;
    return PROCESS_BAD_STATE;
#else
    struct kernel_process *process = NULL;
    uint64_t code_phys = 0U;
    uint64_t stack_phys = 0U;
    uint8_t *code_page;
    uint8_t *stack_page;
    uint64_t user_pages[2];
    enum process_status status;

    if (out_process == NULL || entry == NULL || code == NULL ||
        code_size == 0U || code_size > 4096U) {
        return PROCESS_NULL;
    }
    status = process_create_user(out_process, parent, entry, user_entry,
                                 user_stack_top, hhdm_offset);
    if (status != PROCESS_OK) {
        return status;
    }
    process = *out_process;
    code_phys = pmm_alloc_page();
    stack_phys = pmm_alloc_page();
    if (code_phys == 0U || stack_phys == 0U ||
        code_phys > UINT64_MAX - hhdm_offset ||
        stack_phys > UINT64_MAX - hhdm_offset) {
        if (code_phys != 0U) {
            (void)pmm_free_page(code_phys);
        }
        if (stack_phys != 0U) {
            (void)pmm_free_page(stack_phys);
        }
        (void)process_destroy_kernel(process, hhdm_offset);
        return PROCESS_USER_PAGE_FAILED;
    }
    code_page = (uint8_t *)(uintptr_t)(hhdm_offset + code_phys);
    stack_page = (uint8_t *)(uintptr_t)(hhdm_offset + stack_phys);
    memset(code_page, 0, 4096U);
    memset(stack_page, 0, 4096U);
    memcpy(code_page, code, code_size);
    if (process_limit_charge_user_memory(process, 2U) != PROCESS_OK) {
        (void)pmm_free_page(code_phys);
        (void)pmm_free_page(stack_phys);
        (void)process_destroy_kernel(process, hhdm_offset);
        return PROCESS_USER_MEMORY_LIMIT_EXCEEDED;
    }
    user_pages[0] = code_phys;
    user_pages[1] = stack_phys;
    if (vmm_map_user_pages_in(
            &process->address_space, VMM_USER_IMAGE_BASE, user_pages, 1U,
            VMM_FLAG_PRESENT | VMM_FLAG_WRITABLE | VMM_FLAG_USER,
            hhdm_offset) != VMM_OK ||
        vmm_map_user_pages_in(
            &process->address_space, VMM_USER_STACK_BASE, user_pages + 1U,
            1U,
            VMM_FLAG_PRESENT | VMM_FLAG_WRITABLE | VMM_FLAG_USER |
                VMM_FLAG_NX,
            hhdm_offset) != VMM_OK) {
        (void)pmm_free_page(code_phys);
        (void)pmm_free_page(stack_phys);
        (void)process_destroy_kernel(process, hhdm_offset);
        return PROCESS_USER_PAGE_FAILED;
    }
    process->user_code_phys = code_phys;
    process->user_stack_phys = stack_phys;
    process->user_pages_owned = true;
    return PROCESS_OK;
#endif
}

#if defined(HFOC_USER_INIT_QEMU_TEST) || defined(HFOC_USER_INIT_HOST_TEST)
enum process_status process_create_user_init(
    struct kernel_process **out_process,
    struct process_control_block *parent,
    void (*entry)(void),
    uint64_t hhdm_offset)
{
    struct kernel_process *process = NULL;
    enum process_status create_status;
    uint64_t code_phys = 0U;
    uint64_t stack_phys = 0U;
    uint8_t *code_page;
    uint8_t *stack_page;
    uint64_t user_pages[2];
    enum process_status capability_status;

    if (out_process == NULL || entry == NULL) {
        return PROCESS_NULL;
    }
    *out_process = NULL;

    create_status = process_create_kernel_domain(
        &process, parent, entry, hhdm_offset,
        PROCESS_SECURITY_DOMAIN_USER);
    if (create_status != PROCESS_OK) {
        return create_status;
    }

    capability_status = process_publish_user_capabilities(process);
    if (capability_status != PROCESS_OK) {
        (void)process_destroy_kernel(process, hhdm_offset);
        return capability_status;
    }
    if (process_bind_user_streams(process) != PROCESS_OK) {
        (void)process_destroy_kernel(process, hhdm_offset);
        return PROCESS_BAD_STATE;
    }

    code_phys = pmm_alloc_page();
    stack_phys = pmm_alloc_page();
    if (code_phys == 0U || stack_phys == 0U ||
        code_phys > UINT64_MAX - hhdm_offset ||
        stack_phys > UINT64_MAX - hhdm_offset) {
        if (code_phys != 0U) {
            (void)pmm_free_page(code_phys);
        }
        if (stack_phys != 0U) {
            (void)pmm_free_page(stack_phys);
        }
        (void)process_destroy_kernel(process, hhdm_offset);
        return PROCESS_USER_PAGE_FAILED;
    }

    code_page = (uint8_t *)(uintptr_t)(hhdm_offset + code_phys);
    stack_page = (uint8_t *)(uintptr_t)(hhdm_offset + stack_phys);
    const enum process_status limit_status =
        process_limit_charge_user_memory(process, 2U);
    if (limit_status != PROCESS_OK) {
        (void)pmm_free_page(code_phys);
        (void)pmm_free_page(stack_phys);
        (void)process_destroy_kernel(process, hhdm_offset);
        return limit_status;
    }
    if (!user_init_image_copy(code_page, USER_INIT_IMAGE_MAX_SIZE)) {
        (void)pmm_free_page(code_phys);
        (void)pmm_free_page(stack_phys);
        (void)process_destroy_kernel(process, hhdm_offset);
        return PROCESS_USER_PAGE_FAILED;
    }
    memset(stack_page, 0, 4096U);

    user_pages[0] = code_phys;
    user_pages[1] = stack_phys;
    if (vmm_map_user_pages_in(
            &process->address_space,
            VMM_USER_IMAGE_BASE,
            user_pages,
            1U,
            USER_INIT_CODE_FLAGS,
            hhdm_offset) != VMM_OK ||
        vmm_map_user_pages_in(
            &process->address_space,
            VMM_USER_STACK_BASE,
            user_pages + 1U,
            1U,
            USER_INIT_STACK_FLAGS,
            hhdm_offset) != VMM_OK) {
        (void)pmm_free_page(code_phys);
        (void)pmm_free_page(stack_phys);
        (void)process_destroy_kernel(process, hhdm_offset);
        return PROCESS_USER_PAGE_FAILED;
    }

    process->user_code_phys = code_phys;
    process->user_stack_phys = stack_phys;
    process->user_pages_owned = true;
    *out_process = process;
    return PROCESS_OK;
}
#endif

enum process_status process_destroy_kernel(
    struct kernel_process *process,
    uint64_t hhdm_offset)
{
    enum process_status pcb_status;

    if (process == NULL) {
        return PROCESS_NULL;
    }
    if (!process->pcb.published) {
        return PROCESS_BAD_STATE;
    }
    if (process->lifecycle == PROCESS_LIFECYCLE_EXITED) {
        return PROCESS_ALREADY_EXITED;
    }
    if (process->scheduler_published) {
        return PROCESS_BAD_STATE;
    }
    if (process->pcb.children_head != NULL) {
        return PROCESS_HAS_CHILDREN;
    }

    process_release_user_pages(process);
    process_release_stack(process);
    process_release_address_space(process, hhdm_offset);
    pcb_status = process_pcb_destroy(&process->pcb);
    if (pcb_status != PROCESS_OK) {
        process_release_descriptor_table(process);
        memset(process, 0, sizeof(*process));
        kfree(process);
        return pcb_status;
    }

    process_release_descriptor_table(process);
    memset(process, 0, sizeof(*process));
    kfree(process);
    return PROCESS_OK;
}

enum process_status process_exit_kernel(
    struct kernel_process *process,
    uint32_t exit_status)
{
    if (process == NULL) {
        return PROCESS_NULL;
    }
    if (!process->pcb.published) {
        return PROCESS_BAD_STATE;
    }
    if (process->lifecycle == PROCESS_LIFECYCLE_EXITED) {
        return PROCESS_ALREADY_EXITED;
    }
    if (process->pcb.capability_table.published &&
        capability_table_destroy(&process->pcb.capability_table) !=
            CAPABILITY_OK) {
        return PROCESS_BAD_STATE;
    }
    /*
     * ITEM-632: a process closes its descriptors when it exits, not when its
     * parent finally reaps it.  Otherwise a reader waiting for EOF on a pipe
     * whose writer has exited but is still a zombie sleeps forever; command
     * substitution in the native shell is exactly that shape.
     */
    process_release_descriptor_table(process);

    while (process->pcb.children_head != NULL) {
        struct process_control_block *const child =
            process->pcb.children_head;

        process_child_remove(&process->pcb, child);
        process_orphan_append(child);
    }

    ++process->pcb.security_generation;
    process->exit_status = exit_status;
    process->lifecycle = PROCESS_LIFECYCLE_EXITED;
    if (process->pcb.parent == NULL && !process->pcb.orphaned) {
        process_orphan_append(&process->pcb);
    }
    return PROCESS_OK;
}

enum process_status process_cancel_kernel(
    struct kernel_process *process,
    uint32_t exit_status)
{
#if defined(HFOC_PROCESS_HOST_TEST) || defined(HFOC_USER_INIT_HOST_TEST)
    /* The focused host builds link neither the real scheduler queue owner nor
       the VMM; cancellation is proven by the ring3 QEMU lifecycle matrix. */
    (void)process;
    (void)exit_status;
    return PROCESS_BAD_STATE;
#else
    struct scheduler_task *task = NULL;

    if (process == NULL) {
        return PROCESS_NULL;
    }
    if (!process->pcb.published ||
        (process->lifecycle != PROCESS_LIFECYCLE_RUNNING &&
         process->lifecycle != PROCESS_LIFECYCLE_STOPPED)) {
        return PROCESS_BAD_STATE;
    }
    if (!process->scheduler_published) {
        return PROCESS_BAD_STATE;
    }
    if (sched_current(&task) != SCHEDULER_OK || task == NULL ||
        task == &process->scheduler_task) {
        return PROCESS_BAD_STATE;
    }
    if (sched_cancel(&process->scheduler_task) != SCHEDULER_OK) {
        return PROCESS_SCHEDULER_FAILED;
    }
    process->scheduler_published = false;
    return process_exit_kernel(process, exit_status);
#endif
}

static enum process_status process_reap_exited(
    struct process_control_block *pcb,
    uint32_t *pid_out,
    uint32_t *exit_status_out,
    uint64_t hhdm_offset)
{
    struct kernel_process *const process = process_from_pcb(pcb);
    enum process_status pcb_status;

    if (process->lifecycle != PROCESS_LIFECYCLE_EXITED) {
        return PROCESS_NOT_EXITED;
    }
    if (pcb->children_head != NULL) {
        return PROCESS_HAS_CHILDREN;
    }
    if (pcb->orphaned) {
        process_orphan_remove(pcb);
    } else if (pcb->parent != NULL) {
        process_child_remove(pcb->parent, pcb);
    }
    pcb->parent = NULL;
    *pid_out = pcb->pid;
    *exit_status_out = process->exit_status;

    process_release_user_pages(process);
    process_release_stack(process);
    process_release_address_space(process, hhdm_offset);
    pcb_status = process_pcb_destroy(pcb);
    if (pcb_status != PROCESS_OK) {
        process_release_descriptor_table(process);
        memset(process, 0, sizeof(*process));
        kfree(process);
        return pcb_status;
    }
    process_release_descriptor_table(process);
    memset(process, 0, sizeof(*process));
    kfree(process);
    return PROCESS_OK;
}

/* ITEM-633: find a live process by PID without trusting a child tree. */
struct kernel_process *process_find_pid(uint32_t pid)
{
    if (pid == 0U || pid > PROCESS_MAX_PIDS) {
        return NULL;
    }
    if (process_pid_table[pid - 1U] == NULL ||
        !process_pid_table[pid - 1U]->published) {
        return NULL;
    }
    return process_from_pcb(process_pid_table[pid - 1U]);
}

uint32_t process_pgid(const struct kernel_process *process)
{
    return process != NULL ? process->pcb.pgid : 0U;
}

uint32_t process_session_id(const struct kernel_process *process)
{
    return process != NULL ? process->pcb.session_id : 0U;
}

enum process_status process_setpgid_kernel(
    struct kernel_process *process,
    uint32_t pgid)
{
    struct kernel_process *leader;

    if (process == NULL || !process->pcb.published) {
        return PROCESS_NULL;
    }
    if (process->lifecycle != PROCESS_LIFECYCLE_RUNNING) {
        return PROCESS_BAD_STATE;
    }
    if (pgid == 0U) {
        pgid = process->pcb.pid;
    }
    if (pgid != process->pcb.pid) {
        leader = process_find_pid(pgid);
        if (leader == NULL || leader->pcb.session_id !=
                                  process->pcb.session_id) {
            return PROCESS_BAD_PID;
        }
    }
    if (process->pcb.pgid == process->pcb.pid &&
        process->pcb.pid == process->pcb.session_id &&
        pgid != process->pcb.pid) {
        return PROCESS_BAD_STATE; /* a session leader cannot leave its group */
    }
    process->pcb.pgid = pgid;
    return PROCESS_OK;
}

static bool process_is_current(const struct kernel_process *process)
{
#if defined(HFOC_PROCESS_HOST_TEST) || defined(HFOC_USER_INIT_HOST_TEST)
    (void)process;
    return false;
#else
    struct scheduler_task *current = NULL;

    return sched_current(&current) == SCHEDULER_OK && current != NULL &&
           current->process_owner == (void *)process;
#endif
}

enum process_status process_signal_kernel(
    struct kernel_process *process,
    uint32_t signal)
{
    enum process_status status;

    if (process == NULL || !process->pcb.published) {
        return PROCESS_NULL;
    }
    if (signal == PROCESS_SIGNAL_NONE || signal > PROCESS_SIGNAL_MAX) {
        return PROCESS_BAD_SIGNAL;
    }
    if (process_is_current(process)) {
        return PROCESS_SELF_SIGNAL;
    }
    if (signal == PROCESS_SIGNAL_CHLD) {
        return PROCESS_OK; /* the parent polls children; no handler yet */
    }
    if (signal == PROCESS_SIGNAL_CONT) {
        if (process->lifecycle == PROCESS_LIFECYCLE_EXITED) {
            return PROCESS_BAD_STATE;
        }
        if (process->lifecycle == PROCESS_LIFECYCLE_STOPPED) {
#if defined(HFOC_PROCESS_HOST_TEST) || defined(HFOC_USER_INIT_HOST_TEST)
            return PROCESS_SCHEDULER_FAILED;
#else
            if (!process->scheduler_published ||
                sched_resume(&process->scheduler_task) != SCHEDULER_OK) {
                return PROCESS_SCHEDULER_FAILED;
            }
#endif
            process->scheduler_published = true;
            process->lifecycle = PROCESS_LIFECYCLE_RUNNING;
            process->pcb.wait_continued = 1U;
        }
        return PROCESS_OK;
    }
    if (signal == PROCESS_SIGNAL_TSTP) {
        if (process->lifecycle == PROCESS_LIFECYCLE_EXITED) {
            return PROCESS_BAD_STATE;
        }
        if (process->lifecycle == PROCESS_LIFECYCLE_STOPPED) {
            return PROCESS_OK;
        }
#if defined(HFOC_PROCESS_HOST_TEST) || defined(HFOC_USER_INIT_HOST_TEST)
        return PROCESS_SCHEDULER_FAILED;
#else
        if (!process->scheduler_published ||
            sched_suspend(&process->scheduler_task) != SCHEDULER_OK) {
            return PROCESS_SCHEDULER_FAILED;
        }
#endif
        process->lifecycle = PROCESS_LIFECYCLE_STOPPED;
        process->pcb.stop_signal = signal;
        process->pcb.wait_stopped = 1U;
        return PROCESS_OK;
    }
    /* Fatal defaults: HUP, INT, QUIT and TERM. */
    status = process_cancel_kernel(process, 128U + signal);
    return status == PROCESS_OK ? PROCESS_OK : status;
}

enum process_status process_signal_group(uint32_t pgid, uint32_t signal)
{
    uint32_t index;
    uint32_t matched = 0U;

    if (pgid == 0U || signal == PROCESS_SIGNAL_NONE ||
        signal > PROCESS_SIGNAL_MAX) {
        return PROCESS_BAD_SIGNAL;
    }
    for (index = 0U; index < PROCESS_MAX_PIDS; ++index) {
        struct process_control_block *pcb = process_pid_table[index];

        if (pcb == NULL || !pcb->published || pcb->pgid != pgid) {
            continue;
        }
        ++matched;
        (void)process_signal_kernel(process_from_pcb(pcb), signal);
    }
    return matched != 0U ? PROCESS_OK : PROCESS_BAD_PID;
}

enum process_status process_signal_session(uint32_t session_id,
                                           uint32_t signal)
{
    uint32_t index;
    uint32_t matched = 0U;

    if (session_id == 0U || signal == PROCESS_SIGNAL_NONE ||
        signal > PROCESS_SIGNAL_MAX) {
        return PROCESS_BAD_SIGNAL;
    }
    for (index = 0U; index < PROCESS_MAX_PIDS; ++index) {
        struct process_control_block *pcb = process_pid_table[index];

        if (pcb == NULL || !pcb->published ||
            pcb->session_id != session_id) {
            continue;
        }
        ++matched;
        (void)process_signal_kernel(process_from_pcb(pcb), signal);
    }
    return matched != 0U ? PROCESS_OK : PROCESS_BAD_PID;
}

enum process_status process_wait2_kernel(
    struct process_control_block *parent,
    uint32_t flags,
    uint32_t *pid_out,
    uint32_t *status_out,
    uint64_t hhdm_offset)
{
    struct process_control_block *pcb;

    if (parent == NULL || pid_out == NULL || status_out == NULL) {
        return PROCESS_NULL;
    }
    if (!parent->published ||
        (flags & ~(PROCESS_WAIT_NOHANG | PROCESS_WAIT_UNTRACED |
                   PROCESS_WAIT_CONTINUED)) != 0U) {
        return PROCESS_BAD_STATE;
    }
    for (pcb = parent->children_head; pcb != NULL;
         pcb = pcb->sibling_next) {
        struct kernel_process *process = process_from_pcb(pcb);

        if (process->lifecycle == PROCESS_LIFECYCLE_EXITED) {
            const enum process_status status =
                process_reap_exited(pcb, pid_out, status_out, hhdm_offset);
            if (status == PROCESS_OK) {
                *status_out = PROCESS_WAIT_STATUS_EXITED(*status_out);
            }
            return status;
        }
        if ((flags & PROCESS_WAIT_UNTRACED) != 0U &&
            process->pcb.wait_stopped != 0U) {
            *pid_out = pcb->pid;
            *status_out = PROCESS_WAIT_STATUS_STOPPED(pcb->stop_signal);
            pcb->wait_stopped = 0U;
            return PROCESS_OK;
        }
        if ((flags & PROCESS_WAIT_CONTINUED) != 0U &&
            process->pcb.wait_continued != 0U) {
            *pid_out = pcb->pid;
            *status_out = PROCESS_WAIT_STATUS_CONTINUED;
            pcb->wait_continued = 0U;
            return PROCESS_OK;
        }
    }
    return parent->children_head != NULL ? PROCESS_NOT_EXITED
                                        : PROCESS_NO_CHILDREN;
}

enum process_status process_reap_kernel(
    struct kernel_process *process,
    uint32_t *exit_status_out,
    uint64_t hhdm_offset)
{
    uint32_t pid = 0U;

    if (process == NULL || exit_status_out == NULL) {
        return PROCESS_NULL;
    }
    if (!process->pcb.published) {
        return PROCESS_BAD_STATE;
    }
    if (process->lifecycle != PROCESS_LIFECYCLE_EXITED) {
        return PROCESS_NOT_EXITED;
    }
    return process_reap_exited(&process->pcb, &pid, exit_status_out,
                               hhdm_offset);
}

enum process_status process_wait_kernel(
    struct process_control_block *parent,
    uint32_t *pid_out,
    uint32_t *exit_status_out,
    uint64_t hhdm_offset)
{
    struct process_control_block *pcb;

    if (pid_out == NULL || exit_status_out == NULL) {
        return PROCESS_NULL;
    }
    if (parent != NULL) {
        if (!parent->published) {
            return PROCESS_BAD_STATE;
        }
        pcb = process_find_exited_child(parent->children_head);
        if (pcb == NULL) {
            return parent->children_head != NULL ?
                PROCESS_NOT_EXITED : PROCESS_NO_CHILDREN;
        }
    } else {
        pcb = process_find_exited_child(process_orphans_head);
        if (pcb == NULL) {
            return process_orphans_head != NULL ?
                PROCESS_NOT_EXITED : PROCESS_NO_CHILDREN;
        }
    }
    return process_reap_exited(pcb, pid_out, exit_status_out, hhdm_offset);
}

enum process_status process_limit_set_cpu_ticks(
    struct kernel_process *process,
    uint64_t limit)
{
    if (process == NULL) {
        return PROCESS_NULL;
    }
    if (!process->pcb.published ||
        process->lifecycle != PROCESS_LIFECYCLE_RUNNING ||
        process->scheduler_published) {
        return PROCESS_BAD_STATE;
    }
    process->cpu_tick_limit = limit;
    return PROCESS_OK;
}

enum process_status process_limit_set_user_memory_pages(
    struct kernel_process *process,
    uint64_t limit)
{
    if (process == NULL) {
        return PROCESS_NULL;
    }
    if (!process->pcb.published ||
        process->lifecycle != PROCESS_LIFECYCLE_RUNNING ||
        process->user_pages_owned ||
        process->user_memory_pages != 0U) {
        return PROCESS_BAD_STATE;
    }
    process->user_memory_limit_pages = limit;
    return PROCESS_OK;
}

enum process_status process_limit_charge_user_memory(
    struct kernel_process *process,
    uint64_t pages)
{
    if (process == NULL) {
        return PROCESS_NULL;
    }
    if (!process->pcb.published ||
        process->lifecycle != PROCESS_LIFECYCLE_RUNNING) {
        return PROCESS_BAD_STATE;
    }
    if (pages == 0U) {
        return PROCESS_OK;
    }
    if (process->user_memory_pages > UINT64_MAX - pages) {
        return PROCESS_USER_MEMORY_LIMIT_EXCEEDED;
    }
    if (process->user_memory_limit_pages !=
            PROCESS_USER_MEMORY_LIMIT_UNLIMITED &&
        process->user_memory_pages + pages >
            process->user_memory_limit_pages) {
        return PROCESS_USER_MEMORY_LIMIT_EXCEEDED;
    }
    process->user_memory_pages += pages;
    return PROCESS_OK;
}

uint64_t process_limit_cpu_ticks(const struct kernel_process *process) {
    return process != NULL ? process->cpu_tick_limit :
                             PROCESS_CPU_TICK_LIMIT_UNLIMITED;
}

uint64_t process_limit_user_memory_limit_pages(
    const struct kernel_process *process)
{
    return process != NULL ? process->user_memory_limit_pages :
                             PROCESS_USER_MEMORY_LIMIT_UNLIMITED;
}

uint64_t process_limit_user_memory_pages(
    const struct kernel_process *process)
{
    return process != NULL ? process->user_memory_pages : 0U;
}

#if !defined(HFOC_PROCESS_HOST_TEST) || \
    defined(HFOC_PROCESS_SCHEDULER_HOST_TEST)
static enum scheduler_status process_limit_cpu_exceeded(
    struct scheduler_task *task)
{
    struct kernel_process *process;

    if (task == NULL || task->process_owner == NULL) {
        return SCHEDULER_BAD_STATE;
    }
    process = (struct kernel_process *)task->process_owner;
    if (process_exit_kernel(process, PROCESS_EXIT_LIMIT_EXCEEDED) !=
        PROCESS_OK) {
        return SCHEDULER_BAD_STATE;
    }
    return SCHEDULER_OK;
}

static void process_kernel_entry_wrapper(void) {
    struct scheduler_task *task = NULL;
    struct kernel_process *process;

    (void)sched_current(&task);
    if (task == NULL || task->process_owner == NULL) {
        for (;;) {
            __asm__ volatile ("hlt" : : : "memory");
        }
    }
    process = (struct kernel_process *)task->process_owner;
    process->entry();
    (void)process_exit_kernel(process, 0U);
    if (sched_exit() != SCHEDULER_OK) {
        for (;;) {
            __asm__ volatile ("hlt" : : : "memory");
        }
    }
    for (;;) {
        __asm__ volatile ("hlt" : : : "memory");
    }
}

static void process_activate_address_space(void *owner)
{
#if defined(HFOC_PROCESS_HOST_TEST)
    /* The focused process host proofs do not link the VMM. */
    (void)owner;
#else
    if (owner != NULL) {
        struct kernel_process *process = (struct kernel_process *)owner;
        if (process->address_space.pml4_phys != 0U) {
            (void)vmm_activate_address_space(&process->address_space);
            return;
        }
    }
    {
        const struct address_space *kernel_space = vmm_kernel_address_space();
        if (kernel_space != NULL) {
            (void)vmm_activate_address_space(
                (struct address_space *)(uintptr_t)kernel_space);
        }
    }
#endif
}

enum process_status process_spawn_kernel(
    struct kernel_process *process,
    uint64_t hhdm_offset)
{
    enum scheduler_status scheduler_status;

    (void)hhdm_offset;
    if (process == NULL) {
        return PROCESS_NULL;
    }
    if (!process->pcb.published ||
        process->lifecycle != PROCESS_LIFECYCLE_RUNNING ||
        process->scheduler_published ||
        process->stack_top == 0U ||
        process->entry == NULL) {
        return PROCESS_BAD_STATE;
    }

    memset(&process->scheduler_task, 0, sizeof(process->scheduler_task));
    process->scheduler_task.process_owner = process;
    process->scheduler_task.state = TASK_STATE_CREATED;
    process->scheduler_task.stack_phys = 0U;
    process->scheduler_task.stack_pages = 0U;
    process->scheduler_task.kernel_stack_top = process->stack_top;
    process->scheduler_task.cpu_tick_limit = process->cpu_tick_limit;
    process->scheduler_task.context = process->context;
    process->scheduler_task.context.rip =
        (uintptr_t)sched_kernel_thread_trampoline;
    process->scheduler_task.context.rsp = process->stack_top - 8U;
    *(uintptr_t *)process->scheduler_task.context.rsp =
        (uintptr_t)process_kernel_entry_wrapper;

    (void)sched_set_limit_hook(process_limit_cpu_exceeded);
    sched_set_activate_hook(process_activate_address_space);
    scheduler_status = sched_enqueue(&process->scheduler_task);
    if (scheduler_status != SCHEDULER_OK) {
        memset(&process->scheduler_task, 0, sizeof(process->scheduler_task));
        return PROCESS_SCHEDULER_FAILED;
    }
    process->scheduler_published = true;
    return PROCESS_OK;
}
#endif

uint32_t process_pcb_child_count(const struct process_control_block *pcb) {
    return pcb != NULL ? pcb->child_count : 0U;
}

enum process_security_domain process_pcb_security_domain(
    const struct process_control_block *pcb)
{
    return pcb != NULL ? pcb->security_domain :
                         PROCESS_SECURITY_DOMAIN_COUNT;
}

uint32_t process_pcb_security_generation(
    const struct process_control_block *pcb)
{
    return pcb != NULL ? pcb->security_generation : 0U;
}

const char *process_status_string(enum process_status status) {
    switch (status) {
    case PROCESS_OK:
        return "PROCESS_OK";
    case PROCESS_NULL:
        return "PROCESS_NULL";
    case PROCESS_NO_PIDS:
        return "PROCESS_NO_PIDS";
    case PROCESS_BAD_PID:
        return "PROCESS_BAD_PID";
    case PROCESS_BAD_STATE:
        return "PROCESS_BAD_STATE";
    case PROCESS_HAS_CHILDREN:
        return "PROCESS_HAS_CHILDREN";
    case PROCESS_LINK_FAILED:
        return "PROCESS_LINK_FAILED";
    case PROCESS_HEAP_FAILED:
        return "PROCESS_HEAP_FAILED";
    case PROCESS_ADDRESS_SPACE_FAILED:
        return "PROCESS_ADDRESS_SPACE_FAILED";
    case PROCESS_STACK_FAILED:
        return "PROCESS_STACK_FAILED";
    case PROCESS_CONTEXT_FAILED:
        return "PROCESS_CONTEXT_FAILED";
    case PROCESS_NO_CHILDREN:
        return "PROCESS_NO_CHILDREN";
    case PROCESS_NOT_EXITED:
        return "PROCESS_NOT_EXITED";
    case PROCESS_ALREADY_EXITED:
        return "PROCESS_ALREADY_EXITED";
    case PROCESS_SCHEDULER_FAILED:
        return "PROCESS_SCHEDULER_FAILED";
    case PROCESS_USER_PAGE_FAILED:
        return "PROCESS_USER_PAGE_FAILED";
    case PROCESS_BAD_DOMAIN:
        return "PROCESS_BAD_DOMAIN";
    case PROCESS_USER_MEMORY_LIMIT_EXCEEDED:
        return "PROCESS_USER_MEMORY_LIMIT_EXCEEDED";
    default:
        return "PROCESS_UNKNOWN";
    }
}

#ifdef HFOC_PROCESS_SCHEDULER_QEMU_TEST
static struct kernel_process *process_sched_qemu_parent;
static struct kernel_process *process_sched_qemu_children[
    PROCESS_SCHED_QEMU_CHILD_COUNT];
static uint64_t process_sched_qemu_hhdm;

static struct kernel_process *process_sched_qemu_current(void) {
    struct scheduler_task *task = NULL;

    if (sched_current(&task) != SCHEDULER_OK || task == NULL ||
        task->process_owner == NULL) {
        return NULL;
    }
    return (struct kernel_process *)task->process_owner;
}

static void process_sched_qemu_child_entry(void) {
    struct kernel_process *const process = process_sched_qemu_current();
    uint32_t step;

    if (process == NULL) {
        serial_write("[PROC] child owner missing\n");
        for (;;) {
            __asm__ volatile ("hlt" : : : "memory");
        }
    }
    if (process_limit_cpu_ticks(process) != 0U) {
        {
            const uint64_t serial_flags = serial_sequence_begin();
            serial_write("[PROC] limited child start pid=");
            serial_write_u64(process->pcb.pid);
            serial_write(" limit=");
            serial_write_u64(process_limit_cpu_ticks(process));
            serial_write("\n");
            serial_sequence_end(serial_flags);
        }
        for (;;) {
            __asm__ volatile ("hlt" : : : "memory");
        }
    }
    for (step = 0U; step < 2U; ++step) {
        {
            const uint64_t serial_flags = serial_sequence_begin();
            serial_write("[PROC] child progress pid=");
            serial_write_u64(process->pcb.pid);
            serial_write("\n");
            serial_sequence_end(serial_flags);
        }
        (void)sched_yield();
    }
}

static void process_sched_qemu_parent_entry(void) {
    uint32_t reaped = 0U;

    serial_write("[PROC] parent wait start\n");
    while (reaped < PROCESS_SCHED_QEMU_CHILD_COUNT) {
        uint32_t pid = 0U;
        uint32_t exit_status = 0U;
        const enum process_status wait_status =
            process_wait_kernel(&process_sched_qemu_parent->pcb,
                                &pid, &exit_status,
                                process_sched_qemu_hhdm);
        if (wait_status == PROCESS_OK) {
            {
                const uint64_t serial_flags = serial_sequence_begin();
                serial_write("[PROC] reaped pid=");
                serial_write_u64(pid);
                serial_write(" status=");
                serial_write_u64(exit_status);
                serial_write("\n");
                serial_sequence_end(serial_flags);
            }
            ++reaped;
        } else if (wait_status == PROCESS_NOT_EXITED) {
            (void)sched_yield();
        } else {
            {
                const uint64_t serial_flags = serial_sequence_begin();
                serial_write("[PROC] wait failed: ");
                serial_write(process_status_string(wait_status));
                serial_write("\n");
                serial_sequence_end(serial_flags);
            }
            for (;;) {
                __asm__ volatile ("hlt" : : : "memory");
            }
        }
    }
    serial_write("[PROC] parent reaped all\n");
}

enum process_status process_sched_qemu_test_init(uint64_t hhdm_offset) {
    enum process_status process_status;
    uint32_t index;

    if (sched_init() != SCHEDULER_OK) {
        return PROCESS_SCHEDULER_FAILED;
    }
    process_sched_qemu_hhdm = hhdm_offset;
    process_status = process_create_kernel(
        &process_sched_qemu_parent, NULL,
        process_sched_qemu_parent_entry, hhdm_offset);
    if (process_status != PROCESS_OK) {
        return process_status;
    }
    for (index = 0U; index < PROCESS_SCHED_QEMU_CHILD_COUNT; ++index) {
        process_status = process_create_kernel(
            &process_sched_qemu_children[index],
            &process_sched_qemu_parent->pcb,
            process_sched_qemu_child_entry, hhdm_offset);
        if (process_status != PROCESS_OK) {
            return process_status;
        }
    }
    process_status = process_limit_set_cpu_ticks(
        process_sched_qemu_children[PROCESS_SCHED_QEMU_LIMITED_INDEX],
        PROCESS_SCHED_QEMU_LIMIT_TICKS);
    if (process_status != PROCESS_OK) {
        return process_status;
    }
    process_status = process_spawn_kernel(process_sched_qemu_parent,
                                          hhdm_offset);
    if (process_status != PROCESS_OK) {
        return process_status;
    }
    for (index = 0U; index < PROCESS_SCHED_QEMU_CHILD_COUNT; ++index) {
        process_status = process_spawn_kernel(
            process_sched_qemu_children[index], hhdm_offset);
        if (process_status != PROCESS_OK) {
            return process_status;
        }
    }
    return PROCESS_OK;
}
#endif

#ifdef HFOC_PROCESS_HOST_TEST
void process_host_test_reset(void) {
    memset(process_pid_used, 0, sizeof(process_pid_used));
    process_orphans_head = NULL;
    process_orphans_tail = NULL;
    process_host_link_failure = false;
}

void process_host_test_set_link_failure(bool fail) {
    process_host_link_failure = fail;
}
#endif
