/*
 * ITEM-625: spawn a native static ELF64 process from the boot VFS and replace
 * an existing image with exec.
 */
#include "user_process.h"

#include "elf_user.h"
#include "kernel_vfs.h"
#include "pipe.h"
#include "pmm.h"
#include "scheduler.h"
#include "tty.h"
#include "user.h"
#include "vfs.h"
#include "vfs_fd.h"
#include "vmm.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define USER_PROCESS_FILE_MAX VFS_FILE_MAX
#define USER_PROCESS_STACK_PAGES 4U
#define USER_PROCESS_ARG_MAX 8192U
#define USER_PROCESS_ARGV_MAX 64U
#define USER_PROCESS_ENVP_MAX 32U

/*
 * The kernel is cooperative and single-CPU here, so a bounded static scratch
 * area is shared by spawn and exec rather than growing the kernel heap from
 * image sizes.  No pointer to it escapes the call.
 */
static uint8_t user_process_file[USER_PROCESS_FILE_MAX];
static uint8_t user_process_stack[USER_PROCESS_STACK_PAGES * 4096U];

static void user_process_trampoline(void)
{
    struct scheduler_task *task = NULL;
    struct kernel_process *process;
    struct user_iret_frame frame;

    if (sched_current(&task) != SCHEDULER_OK || task == NULL ||
        task->process_owner == NULL) {
        return;
    }
    process = (struct kernel_process *)task->process_owner;
    if (process->user_entry == 0U || process->user_stack_top == 0U ||
        vmm_activate_address_space(&process->address_space) != VMM_OK) {
        return;
    }
    if (user_iret_frame_init(&frame, (uintptr_t)process->user_entry,
                             (uintptr_t)process->user_stack_top, 0x2U,
                             USER_CS_SELECTOR, USER_SS_SELECTOR) !=
        USER_ENTRY_OK) {
        return;
    }
    user_enter_iretq(&frame);
}

static bool user_process_push_string(uint8_t *stack, uint64_t *sp,
                                     const char *text, uint64_t base,
                                     uint64_t *address_out)
{
    const size_t length = strlen(text) + 1U;

    if (length > *sp) {
        return false;
    }
    *sp -= length;
    memcpy(stack + *sp, text, length);
    *address_out = base + *sp;
    return true;
}

static bool user_process_build_stack(uint8_t *stack, uint64_t stack_base,
                                     const char *const *argv,
                                     const char *const *envp,
                                     uint64_t *stack_top_out)
{
    uint64_t argv_addresses[USER_PROCESS_ARGV_MAX];
    uint64_t envp_addresses[USER_PROCESS_ENVP_MAX];
    uint32_t argc = 0U;
    uint32_t envc = 0U;
    uint64_t sp = USER_PROCESS_STACK_PAGES * 4096U;
    uint64_t string_bytes = 0U;
    uint64_t words;
    uint32_t index;

    memset(stack, 0, USER_PROCESS_STACK_PAGES * 4096U);
    if (argv != NULL) {
        while (argv[argc] != NULL) {
            if (argc >= USER_PROCESS_ARGV_MAX) {
                return false;
            }
            string_bytes += strlen(argv[argc]) + 1U;
            ++argc;
        }
    }
    if (envp != NULL) {
        while (envp[envc] != NULL) {
            if (envc >= USER_PROCESS_ENVP_MAX) {
                return false;
            }
            string_bytes += strlen(envp[envc]) + 1U;
            ++envc;
        }
    }
    if (string_bytes > USER_PROCESS_ARG_MAX) {
        return false;
    }
    for (index = 0U; index < argc; ++index) {
        if (!user_process_push_string(stack, &sp, argv[index], stack_base,
                                      &argv_addresses[index])) {
            return false;
        }
    }
    for (index = 0U; index < envc; ++index) {
        if (!user_process_push_string(stack, &sp, envp[index], stack_base,
                                      &envp_addresses[index])) {
            return false;
        }
    }
    sp &= ~UINT64_C(7);
    words = 1U + (uint64_t)argc + 1U + (uint64_t)envc + 1U;
    if (words * 8U > sp) {
        return false;
    }
    sp -= words * 8U;
    {
        uint64_t *word = (uint64_t *)(stack + sp);

        *word++ = argc;
        for (index = 0U; index < argc; ++index) {
            *word++ = argv_addresses[index];
        }
        *word++ = 0U;
        for (index = 0U; index < envc; ++index) {
            *word++ = envp_addresses[index];
        }
        *word++ = 0U;
    }
    *stack_top_out = stack_base + sp;
    return true;
}

/*
 * Read a whole regular file from the boot VFS into the bounded scratch area.
 * A file at the buffer limit is refused rather than silently truncated.
 */
static enum user_process_status user_process_load(const char *path,
                                                  size_t *total_out)
{
    struct vfs_context context;
    struct vfs_file *open_file = NULL;
    const struct vfs *fs = kernel_vfs_boot_instance();
    size_t total = 0U;
    enum vfs_status vfs_status;

    if (path == NULL || total_out == NULL) {
        return USER_PROCESS_NULL;
    }
    if (fs == NULL) {
        return USER_PROCESS_NO_VFS;
    }
    memset(&context, 0, sizeof(context));
    if (vfs_context_init(&context, (struct vfs *)fs) != VFS_OK) {
        return USER_PROCESS_NO_VFS;
    }
    vfs_status = vfs_open(&context, path, VFS_OPEN_READ, &open_file);
    if (vfs_status == VFS_NOT_FOUND) {
        return USER_PROCESS_NOT_FOUND;
    }
    if (vfs_status != VFS_OK) {
        return USER_PROCESS_BAD_ELF;
    }
    while (total < sizeof(user_process_file)) {
        size_t done = 0U;

        vfs_status = vfs_read(open_file, user_process_file + total,
                              sizeof(user_process_file) - total, &done);
        if (vfs_status != VFS_OK) {
            (void)vfs_close(open_file);
            return USER_PROCESS_BAD_ELF;
        }
        if (done == 0U) {
            break;
        }
        total += done;
    }
    (void)vfs_close(open_file);
    if (total == sizeof(user_process_file)) {
        return USER_PROCESS_TOO_LARGE;
    }
    *total_out = total;
    return USER_PROCESS_OK;
}

/*
 * Map the already built stack image into a caller-supplied address space.
 * On failure the pages allocated here are freed; pages already tracked by the
 * caller stay the caller's responsibility.
 */
static enum user_process_status user_process_map_stack(
    struct address_space *address_space, uint64_t hhdm_offset,
    uint64_t *virtual_pages, uint64_t *physical_pages, uint32_t capacity,
    uint32_t *count_io)
{
    uint64_t pages[USER_PROCESS_STACK_PAGES];
    uint32_t index;
    uint32_t count;
    uint32_t initial_count;

    if (address_space == NULL || virtual_pages == NULL ||
        physical_pages == NULL || count_io == NULL) {
        return USER_PROCESS_NULL;
    }
    count = *count_io;
    initial_count = count;
    if (count > capacity ||
        capacity - count < USER_PROCESS_STACK_PAGES) {
        return USER_PROCESS_TOO_LARGE;
    }
    for (index = 0U; index < USER_PROCESS_STACK_PAGES; ++index) {
        const uint64_t physical = pmm_alloc_page();

        if (physical == 0U) {
            uint32_t undo;

            for (undo = 0U; undo < index; ++undo) {
                (void)pmm_free_page(pages[undo]);
            }
            return USER_PROCESS_CREATE_FAILED;
        }
        memset((uint8_t *)(uintptr_t)(physical + hhdm_offset), 0, 4096U);
        pages[index] = physical;
    }
    for (index = 0U; index < USER_PROCESS_STACK_PAGES; ++index) {
        const uint64_t virtual_address =
            VMM_USER_STACK_BASE + (uint64_t)index * 4096U;

        if (vmm_map_page_in(address_space, virtual_address, pages[index],
                            VMM_FLAG_PRESENT | VMM_FLAG_WRITABLE |
                                VMM_FLAG_USER | VMM_FLAG_NX,
                            hhdm_offset) != VMM_OK) {
            uint32_t undo;

            for (undo = 0U; undo < USER_PROCESS_STACK_PAGES; ++undo) {
                (void)pmm_free_page(pages[undo]);
            }
            /* The caller must not free the pages we already rolled back. */
            *count_io = initial_count;
            return USER_PROCESS_CREATE_FAILED;
        }
        virtual_pages[count] = virtual_address;
        physical_pages[count] = pages[index];
        ++count;
    }
    for (index = 0U; index < USER_PROCESS_STACK_PAGES; ++index) {
        memcpy((uint8_t *)(uintptr_t)(pages[index] + hhdm_offset),
               user_process_stack + index * 4096U, 4096U);
    }
    *count_io = count;
    return USER_PROCESS_OK;
}

static void user_process_free_mapped(uint64_t *physical_pages, uint32_t count)
{
    while (count != 0U) {
        --count;
        (void)pmm_free_page(physical_pages[count]);
    }
}

static enum user_process_status user_process_apply_actions(
    struct kernel_process *process, struct process_control_block *parent,
    const struct user_process_fd_action *actions, uint32_t action_count)
{
    uint32_t index;

    if (action_count != 0U && actions == NULL) {
        return USER_PROCESS_NULL;
    }
    if (action_count > USER_PROCESS_FD_ACTION_MAX) {
        return USER_PROCESS_BAD_STATE;
    }
    if (parent != NULL) {
        const struct kernel_process *parent_process =
            (const struct kernel_process *)(const void *)parent;

        process->vfs_cwd = parent_process->vfs_cwd;
        process->vfs_cwd_ready = parent_process->vfs_cwd_ready;
    }
    for (index = 0U; index < action_count; ++index) {
        const struct user_process_fd_action *action = &actions[index];
        struct file_description *storage;

        if (action->operation == USER_PROCESS_FD_PGID) {
            if (action->source == 0U) {
                process->pcb.pgid = process->pcb.pid;
            } else {
                struct kernel_process *leader =
                    process_find_pid(action->source);

                if (leader == NULL ||
                    leader->pcb.session_id != process->pcb.session_id) {
                    return USER_PROCESS_BAD_STATE;
                }
                process->pcb.pgid = action->source;
            }
            continue;
        }
        if (action->operation == USER_PROCESS_FD_CLOSE) {
            if (action->destination >= 3U) {
                return USER_PROCESS_BAD_STATE;
            }
            (void)descriptor_table_close(&process->descriptor_table,
                                         action->destination);
            continue;
        }
        if (action->destination >= 3U) {
            return USER_PROCESS_BAD_STATE;
        }
        storage = &process->stdio[action->destination];
        if (action->operation == USER_PROCESS_FD_OPEN) {
            struct vfs_context context;
            enum vfs_status vfs_status = VFS_BAD_STATE;
            enum descriptor_status descriptor_status;
            const struct vfs *fs = kernel_vfs_boot_instance();
            uint32_t bound = 0U;

            if (fs == NULL || action->path[0] == '\0') {
                return USER_PROCESS_BAD_STATE;
            }
            (void)descriptor_table_close(&process->descriptor_table,
                                         action->destination);
            if (storage->published) {
                return USER_PROCESS_BAD_STATE;
            }
            memset(&context, 0, sizeof(context));
            if (vfs_context_init(&context, (struct vfs *)fs) != VFS_OK) {
                return USER_PROCESS_BAD_STATE;
            }
            if (process->vfs_cwd_ready) {
                context.cwd = process->vfs_cwd;
            }
            descriptor_status = vfs_file_bind_checked(
                &process->descriptor_table, &context, action->path,
                action->flags, storage, &bound, &vfs_status);
            if (descriptor_status != DESCRIPTOR_OK ||
                bound != action->destination) {
                if (descriptor_status == DESCRIPTOR_OK) {
                    (void)descriptor_table_close(&process->descriptor_table,
                                                 bound);
                }
                return USER_PROCESS_BAD_STATE;
            }
        } else if (action->operation == USER_PROCESS_FD_ADOPT) {
            struct kernel_process *parent_process;
            struct file_description *description = NULL;

            if (parent == NULL) {
                return USER_PROCESS_BAD_STATE;
            }
            parent_process =
                (struct kernel_process *)(void *)parent;
            if (descriptor_table_lookup(&parent_process->descriptor_table,
                                        action->source, &description) !=
                DESCRIPTOR_OK) {
                return USER_PROCESS_BAD_STATE;
            }
            if (pipe_description_is_pipe(description)) {
                if (pipe_adopt_fd(process, description,
                                  action->destination) != PIPE_OK) {
                    return USER_PROCESS_BAD_STATE;
                }
            } else if (tty_description_is_tty(description)) {
                if (tty_adopt_fd(process, description->context,
                                 action->destination) != TTY_OK) {
                    return USER_PROCESS_BAD_STATE;
                }
            } else if (descriptor_table_adopt_at(
                           &process->descriptor_table, description,
                           action->destination) != DESCRIPTOR_OK) {
                return USER_PROCESS_BAD_STATE;
            }
        } else if (action->operation == USER_PROCESS_FD_DUP) {
            if (descriptor_table_dup2(&process->descriptor_table,
                                      action->source,
                                      action->destination) != DESCRIPTOR_OK) {
                return USER_PROCESS_BAD_STATE;
            }
        } else {
            return USER_PROCESS_BAD_STATE;
        }
    }
    return USER_PROCESS_OK;
}

static enum user_process_status user_process_spawn_core(
    struct kernel_process **out_process, struct process_control_block *parent,
    const char *path, const char *const *argv, const char *const *envp,
    const struct user_process_fd_action *actions, uint32_t action_count,
    struct tty *tty, uint64_t hhdm_offset)
{
    struct elf_user_image image;
    struct kernel_process *process = NULL;
    uint64_t stack_top = 0U;
    uint64_t mapped_count = 0U;
    size_t total = 0U;
    enum elf_user_status elf_status;
    enum user_process_status status;

    if (out_process == NULL || path == NULL) {
        return USER_PROCESS_NULL;
    }
    *out_process = NULL;
    if (action_count != 0U && actions == NULL) {
        return USER_PROCESS_NULL;
    }
    if (action_count > USER_PROCESS_FD_ACTION_MAX) {
        return USER_PROCESS_BAD_STATE;
    }
    status = user_process_load(path, &total);
    if (status != USER_PROCESS_OK) {
        return status;
    }
    elf_status = elf_user_parse(user_process_file, total, &image);
    if (elf_status != ELF_USER_OK) {
        return USER_PROCESS_BAD_ELF;
    }
    if (image.total_pages + USER_PROCESS_STACK_PAGES >
        PROCESS_USER_MAPPED_MAX) {
        return USER_PROCESS_TOO_LARGE;
    }
    if (process_create_user(&process, parent, user_process_trampoline,
                            image.entry,
                            VMM_USER_STACK_BASE +
                                USER_PROCESS_STACK_PAGES * 4096U,
                            hhdm_offset) != PROCESS_OK) {
        return USER_PROCESS_CREATE_FAILED;
    }
    process->user_pages_owned = true;
    elf_status = elf_user_map_tracked(
        &image, user_process_file, total, &process->address_space,
        hhdm_offset, process->user_mapped_virtual,
        process->user_mapped_physical, PROCESS_USER_MAPPED_MAX,
        (uint32_t *)&mapped_count);
    process->user_mapped_count = (uint32_t)mapped_count;
    if (elf_status != ELF_USER_OK) {
        (void)process_destroy_kernel(process, hhdm_offset);
        return USER_PROCESS_CREATE_FAILED;
    }
    if (process_limit_charge_user_memory(process, image.total_pages) !=
        PROCESS_OK) {
        (void)process_destroy_kernel(process, hhdm_offset);
        return USER_PROCESS_CREATE_FAILED;
    }
    if (!user_process_build_stack(user_process_stack, VMM_USER_STACK_BASE,
                                  argv, envp, &stack_top)) {
        (void)process_destroy_kernel(process, hhdm_offset);
        return USER_PROCESS_UNSUPPORTED_LAYOUT;
    }
    status = user_process_map_stack(
        &process->address_space, hhdm_offset, process->user_mapped_virtual,
        process->user_mapped_physical, PROCESS_USER_MAPPED_MAX,
        (uint32_t *)&mapped_count);
    process->user_mapped_count = (uint32_t)mapped_count;
    if (status != USER_PROCESS_OK) {
        (void)process_destroy_kernel(process, hhdm_offset);
        return status;
    }
    if (process_limit_charge_user_memory(process,
                                         USER_PROCESS_STACK_PAGES) !=
        PROCESS_OK) {
        (void)process_destroy_kernel(process, hhdm_offset);
        return USER_PROCESS_LIMIT_EXCEEDED;
    }
    process->user_stack_top = stack_top;
    status = user_process_apply_actions(process, parent, actions, action_count);
    if (status != USER_PROCESS_OK) {
        (void)process_destroy_kernel(process, hhdm_offset);
        return status;
    }
    if (tty != NULL) {
        uint32_t descriptor;

        for (descriptor = 0U; descriptor < 3U; ++descriptor) {
            if (tty_adopt_fd(process, tty, descriptor) != TTY_OK) {
                (void)process_destroy_kernel(process, hhdm_offset);
                return USER_PROCESS_BAD_STATE;
            }
        }
    }
    if (process_spawn_kernel(process, hhdm_offset) != PROCESS_OK) {
        (void)process_destroy_kernel(process, hhdm_offset);
        return USER_PROCESS_SPAWN_FAILED;
    }
    *out_process = process;
    return USER_PROCESS_OK;
}

enum user_process_status user_process_spawn_actions(
    struct kernel_process **out_process, struct process_control_block *parent,
    const char *path, const char *const *argv, const char *const *envp,
    const struct user_process_fd_action *actions, uint32_t action_count,
    uint64_t hhdm_offset)
{
    return user_process_spawn_core(out_process, parent, path, argv, envp,
                                   actions, action_count, NULL, hhdm_offset);
}

enum user_process_status user_process_spawn_tty(
    struct kernel_process **out_process, struct process_control_block *parent,
    const char *path, const char *const *argv, const char *const *envp,
    struct tty *tty, uint64_t hhdm_offset)
{
    if (tty == NULL) {
        return USER_PROCESS_NULL;
    }
    return user_process_spawn_core(out_process, parent, path, argv, envp,
                                   NULL, 0U, tty, hhdm_offset);
}

enum user_process_status user_process_spawn(
    struct kernel_process **out_process, struct process_control_block *parent,
    const char *path, const char *const *argv, const char *const *envp,
    uint64_t hhdm_offset)
{
    return user_process_spawn_actions(out_process, parent, path, argv, envp,
                                      NULL, 0U, hhdm_offset);
}

/*
 * Free the image a process currently owns.  The caller must already have
 * activated another address space: the old page tables may still reference
 * these frames until they are destroyed here.
 */
static void user_process_release_image(struct kernel_process *process,
                                       uint64_t hhdm_offset)
{
    if (process->user_code_phys != 0U) {
        (void)pmm_free_page(process->user_code_phys);
        process->user_code_phys = 0U;
    }
    if (process->user_stack_phys != 0U) {
        (void)pmm_free_page(process->user_stack_phys);
        process->user_stack_phys = 0U;
    }
    user_process_free_mapped(process->user_mapped_physical,
                             process->user_mapped_count);
    process->user_mapped_count = 0U;
    if (process->address_space_owned &&
        process->address_space.pml4_phys != 0U) {
        (void)vmm_destroy_address_space(&process->address_space,
                                        hhdm_offset);
    }
    process->address_space_owned = false;
    process->address_space.pml4_phys = 0U;
    process->user_pages_owned = false;
}

enum user_process_status user_process_exec(
    struct kernel_process *process, const char *path,
    const char *const *argv, const char *const *envp, uint64_t hhdm_offset,
    uint64_t *entry_out, uint64_t *stack_top_out)
{
    struct address_space new_address_space;
    struct elf_user_image image;
    uint64_t new_virtual[PROCESS_USER_MAPPED_MAX];
    uint64_t new_physical[PROCESS_USER_MAPPED_MAX];
    uint32_t new_count = 0U;
    uint64_t stack_top = 0U;
    uint64_t old_pages;
    uint64_t projected_pages;
    size_t total = 0U;
    enum elf_user_status elf_status;
    enum user_process_status status;

    if (process == NULL || path == NULL || entry_out == NULL ||
        stack_top_out == NULL) {
        return USER_PROCESS_NULL;
    }
    if (!process->pcb.published ||
        process->lifecycle != PROCESS_LIFECYCLE_RUNNING ||
        !process->user_pages_owned ||
        !process->address_space_owned ||
        process->address_space.pml4_phys == 0U) {
        return USER_PROCESS_BAD_STATE;
    }
    status = user_process_load(path, &total);
    if (status != USER_PROCESS_OK) {
        return status;
    }
    elf_status = elf_user_parse(user_process_file, total, &image);
    if (elf_status != ELF_USER_OK) {
        return USER_PROCESS_BAD_ELF;
    }
    if (image.total_pages + USER_PROCESS_STACK_PAGES >
        PROCESS_USER_MAPPED_MAX) {
        return USER_PROCESS_TOO_LARGE;
    }
    if (!user_process_build_stack(user_process_stack, VMM_USER_STACK_BASE,
                                  argv, envp, &stack_top)) {
        return USER_PROCESS_BAD_STACK;
    }
    memset(&new_address_space, 0, sizeof(new_address_space));
    if (vmm_create_address_space(&new_address_space, hhdm_offset) != VMM_OK) {
        return USER_PROCESS_CREATE_FAILED;
    }
    elf_status = elf_user_map_tracked(
        &image, user_process_file, total, &new_address_space, hhdm_offset,
        new_virtual, new_physical, PROCESS_USER_MAPPED_MAX,
        &new_count);
    if (elf_status != ELF_USER_OK) {
        (void)vmm_destroy_address_space(&new_address_space, hhdm_offset);
        return USER_PROCESS_CREATE_FAILED;
    }
    status = user_process_map_stack(
        &new_address_space, hhdm_offset, new_virtual, new_physical,
        PROCESS_USER_MAPPED_MAX, &new_count);
    if (status != USER_PROCESS_OK) {
        user_process_free_mapped(new_physical, new_count);
        (void)vmm_destroy_address_space(&new_address_space, hhdm_offset);
        return status;
    }
    old_pages = process->user_mapped_count;
    if (process->user_code_phys != 0U) {
        ++old_pages;
    }
    if (process->user_stack_phys != 0U) {
        ++old_pages;
    }
    if (process->user_memory_pages < old_pages) {
        user_process_free_mapped(new_physical, new_count);
        (void)vmm_destroy_address_space(&new_address_space, hhdm_offset);
        return USER_PROCESS_BAD_STATE;
    }
    projected_pages = process->user_memory_pages - old_pages + new_count;
    if (process->user_memory_limit_pages !=
            PROCESS_USER_MEMORY_LIMIT_UNLIMITED &&
        projected_pages > process->user_memory_limit_pages) {
        user_process_free_mapped(new_physical, new_count);
        (void)vmm_destroy_address_space(&new_address_space, hhdm_offset);
        return USER_PROCESS_LIMIT_EXCEEDED;
    }
    if (vmm_activate_address_space(&new_address_space) != VMM_OK) {
        user_process_free_mapped(new_physical, new_count);
        (void)vmm_destroy_address_space(&new_address_space, hhdm_offset);
        return USER_PROCESS_CREATE_FAILED;
    }
    /*
     * The new address space is live.  Nothing below can fail: free the old
     * image and publish the new one into the same process control block, so
     * the PID, descriptors and capabilities are preserved by construction.
     */
    user_process_release_image(process, hhdm_offset);
    process->address_space = new_address_space;
    process->address_space_owned = true;
    process->user_pages_owned = true;
    process->user_mapped_count = new_count;
    memcpy(process->user_mapped_virtual, new_virtual,
           sizeof(new_virtual[0]) * new_count);
    memcpy(process->user_mapped_physical, new_physical,
           sizeof(new_physical[0]) * new_count);
    process->user_entry = image.entry;
    process->user_stack_top = stack_top;
    process->user_memory_pages = projected_pages;
    *entry_out = image.entry;
    *stack_top_out = stack_top;
    return USER_PROCESS_OK;
}

const char *user_process_status_string(enum user_process_status status)
{
    switch (status) {
    case USER_PROCESS_OK: return "USER_PROCESS_OK";
    case USER_PROCESS_NULL: return "USER_PROCESS_NULL";
    case USER_PROCESS_NO_VFS: return "USER_PROCESS_NO_VFS";
    case USER_PROCESS_NOT_FOUND: return "USER_PROCESS_NOT_FOUND";
    case USER_PROCESS_TOO_LARGE: return "USER_PROCESS_TOO_LARGE";
    case USER_PROCESS_BAD_ELF: return "USER_PROCESS_BAD_ELF";
    case USER_PROCESS_UNSUPPORTED_LAYOUT: return "USER_PROCESS_UNSUPPORTED_LAYOUT";
    case USER_PROCESS_CREATE_FAILED: return "USER_PROCESS_CREATE_FAILED";
    case USER_PROCESS_SPAWN_FAILED: return "USER_PROCESS_SPAWN_FAILED";
    case USER_PROCESS_BAD_STATE: return "USER_PROCESS_BAD_STATE";
    case USER_PROCESS_LIMIT_EXCEEDED: return "USER_PROCESS_LIMIT_EXCEEDED";
    case USER_PROCESS_BAD_STACK: return "USER_PROCESS_BAD_STACK";
    default: return "USER_PROCESS_UNKNOWN";
    }
}
